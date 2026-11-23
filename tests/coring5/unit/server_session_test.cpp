// M4.1 S1: sans-IO HTTP/1.1 server session.
//
// The session bridges raw bytes <-> one Exchange at a time. These tests drive
// it exactly like the (S2) connection type will: prepare_recv() -> on_data()
// -> drain the ExchangePool into on_exchange_events() -> next_send()/on_sent().
// The test also plays the responder: it takes the fresh exchange handle,
// attaches, publishes a final head and pushes the response body.
//
// Sans-IO: no ring, no sockets, no timers. Every test ends with the pools
// empty (live() == 0, stage pool free) so a leak fails the test.

#include "coring5/http/ExchangePool.hpp"
#include "coring5/mem/ChunkPool.hpp"
#include "coring5/proto/http1session/ServerSession.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {

constexpr uint32_t kChunk = 16;    // BodyStream chunk size
constexpr uint32_t kHigh = 64;     // BodyStream high watermark
constexpr uint32_t kLow = 16;      // BodyStream low watermark
constexpr uint32_t kBuffer = 64;   // "socket" buffer size
constexpr uint32_t kMaxHead = 512; // head limit
constexpr uint32_t kStageChunk = kMaxHead + kBuffer;

std::span<const char> chars(std::string_view s) { return {s.data(), s.size()}; }

std::string as_string(std::span<const std::byte> s) {
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

// Drives one ServerSession and plays both the transport and the responder.
struct Harness {
  // `limits.max_head_bytes` must be <= kMaxHead so the stage chunk fits.
  static Limits base_limits() {
    Limits l;
    l.max_head_bytes = kMaxHead;
    return l;
  }

  explicit Harness(uint32_t exchange_cap = 8, uint32_t stage_chunks = 8,
                   Limits limits = base_limits(), uint32_t high = kHigh,
                   uint32_t low = kLow) {
    limits.queue_high_watermark = high;
    limits.queue_low_watermark = low;
    limits_ = limits;
    high_ = high;
    low_ = low;

    auto sp = ChunkPool::create(kStageChunk, stage_chunks);
    EXPECT_TRUE(sp);
    stage_ = std::move(*sp);
    auto bp = ChunkPool::create(kChunk, 256);
    EXPECT_TRUE(bp);
    chunk_pool_ = std::move(*bp);
    pool_ = std::make_unique<ExchangePool>(exchange_cap, chunk_pool_,
                                           StreamLimits{high_, low_});
    session_ = std::make_unique<ServerSession>(*pool_, stage_, limits_, kBuffer,
                                               self_);
  }

  ~Harness() {
    session_.reset();
    pool_.reset();
    EXPECT_EQ(chunk_pool_.free_count(), chunk_pool_.capacity())
        << "body chunks leaked";
    EXPECT_EQ(stage_.free_count(), stage_.capacity()) << "stage chunks leaked";
  }

  ServerSession &s() { return *session_; }

  // ---- transport ---------------------------------------------------------
  // Feed bytes as if a recv completed. Returns bytes the session wants next.
  void feed(std::string_view in) {
    s().on_data(chars(in));
    settle();
  }

  // Deliver every ready exchange to the session (as the worker would).
  void settle() {
    for (int i = 0; i < 8 && pool_->has_ready(); ++i) {
      pool_->drain(64,
                   [this](Handle, Exchange &) { s().on_exchange_events(); });
    }
  }

  // Drain all pending output; returns the bytes "sent".
  std::string drain_out() {
    std::string out;
    for (int i = 0; i < 64; ++i) {
      const auto b = s().next_send();
      if (b.empty()) {
        break;
      }
      out += as_string(b);
      s().on_sent(static_cast<uint32_t>(b.size()));
      settle();
    }
    return out;
  }

  // ---- responder ---------------------------------------------------------
  // Take the freshly opened exchange (after a head was accepted).
  Exchange *take() {
    const Handle h = s().take_new_exchange();
    if (!h.valid()) {
      return nullptr;
    }
    return pool_->get(h);
  }

  // Publish a final response head with a known Content-Length.
  void respond(Exchange &ex, uint16_t status, std::string_view body) {
    EXPECT_TRUE(ex.attach_responder(responder_));
    ResponseMeta m;
    m.status = status;
    m.declared_known = true;
    m.declared = body.size();
    EXPECT_EQ(ex.publish_final(m), HeadStatus::Ok);
    // A no-content status (204/304/1xx/HEAD) finishes the stream itself.
    if (!content_forbidden(status, ex.request_meta().head_only)) {
      if (!body.empty()) {
        const auto c =
            ex.claim(StreamId::Res, static_cast<uint32_t>(body.size()));
        EXPECT_EQ(c.status, ClaimStatus::Granted);
        EXPECT_EQ(ex.push(StreamId::Res,
                          std::span<const std::byte>(
                              reinterpret_cast<const std::byte *>(body.data()),
                              body.size())),
                  WriteStatus::Ok);
      }
      EXPECT_EQ(ex.finish(StreamId::Res), WriteStatus::Ok);
    }
    ex.detach(Side::Responder, Errc::Ok);
    settle();
  }

  // Publish a chunked response head; the caller pushes chunks then finish().
  void respond_chunked(Exchange &ex, uint16_t status) {
    EXPECT_TRUE(ex.attach_responder(responder_));
    ResponseMeta m;
    m.status = status;
    EXPECT_EQ(ex.publish_final(m), HeadStatus::Ok);
    settle();
  }

  void push_res(Exchange &ex, std::string_view body) {
    if (body.empty()) {
      return;
    }
    const auto c = ex.claim(StreamId::Res, static_cast<uint32_t>(body.size()));
    EXPECT_EQ(c.status, ClaimStatus::Granted);
    EXPECT_EQ(ex.push(StreamId::Res,
                      std::span<const std::byte>(
                          reinterpret_cast<const std::byte *>(body.data()),
                          body.size())),
              WriteStatus::Ok);
    settle();
  }

  // Finish a chunked/close-delimited response and detach the responder.
  void finish_res(Exchange &ex) {
    EXPECT_EQ(ex.finish(StreamId::Res), WriteStatus::Ok);
    ex.detach(Side::Responder, Errc::Ok);
    settle();
  }

  SideRef self_{ConnRef{PoolId::Http1, Handle{1, 1}}, 0};
  SideRef responder_{ConnRef{PoolId::Http1, Handle{2, 1}}, 0};
  Limits limits_{};
  uint32_t high_{kHigh};
  uint32_t low_{kLow};
  ChunkPool stage_;
  ChunkPool chunk_pool_;
  std::unique_ptr<ExchangePool> pool_;
  std::unique_ptr<ServerSession> session_;
};

// A minimal valid HTTP/1.1 request head.
std::string req(std::string_view method, std::string_view target,
                std::string extra = {}) {
  std::string r = std::string(method) + " " + std::string(target) +
                  " HTTP/1.1\r\nHost: a\r\n";
  r += extra;
  r += "\r\n";
  return r;
}

} // namespace

// ---- split heads -----------------------------------------------------------

TEST(ServerSession, SplitHeadByteByByte) {
  Harness h;
  const std::string head = req("GET", "/");
  // Feed one byte at a time; the session must stage and only accept at the end.
  for (std::size_t i = 0; i + 1 < head.size(); ++i) {
    h.feed(head.substr(i, 1));
    EXPECT_EQ(h.take(), nullptr) << i; // no exchange until the head is complete
  }
  h.feed(head.substr(head.size() - 1));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "hi");
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, SplitHeadInStageChunk) {
  Harness h;
  const std::string head = req("GET", "/x");
  // Two halves, each fed as one recv.
  h.feed(head.substr(0, head.size() / 2));
  EXPECT_EQ(h.take(), nullptr);
  h.feed(head.substr(head.size() / 2));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 204, "");
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 204 No Content\r\n\r\n");
}

// ---- error statuses --------------------------------------------------------

TEST(ServerSession, BadRequestLine400) {
  Harness h;
  h.feed("G@T / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(h.take(), nullptr);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 400 Bad Request"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, UriTooLong414) {
  Harness h;
  Limits l = h.limits_;
  l.max_uri_bytes = 8;
  Harness h2(8, 8, l);
  h2.feed("GET /" + std::string(32, 'a') + " HTTP/1.1\r\nHost: a\r\n\r\n");
  const std::string out = h2.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 414"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h2.s().should_close());
}

TEST(ServerSession, HeadTooLarge431) {
  Limits l;
  l.max_head_bytes = 256;
  l.queue_high_watermark = kHigh;
  l.queue_low_watermark = kLow;
  Harness h(8, 8, l);
  h.feed("GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(400, 'a') +
         "\r\n\r\n");
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 431"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, PoolFull503) {
  Harness h(/*exchange_cap=*/1);
  // Another connection occupies the only exchange slot.
  const Handle other = h.pool_->open(
      SideRef{ConnRef{PoolId::Http1, Handle{9, 1}}, 0}, RequestMeta{});
  ASSERT_TRUE(other.valid());
  // Our request cannot be served: the pool is full -> 503 + close.
  h.feed(req("GET", "/b"));
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 503"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

// ---- request body limits ---------------------------------------------------

TEST(ServerSession, ContentLengthOverLimit413AtHead) {
  Limits l;
  l.max_head_bytes = kMaxHead;
  l.queue_high_watermark = kHigh;
  l.queue_low_watermark = kLow;
  l.max_request_body_bytes = 4;
  Harness h(8, 8, l);
  h.feed(req("POST", "/", "Content-Length: 5\r\n"));
  EXPECT_EQ(h.take(), nullptr); // rejected before an exchange is opened
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 413"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, ChunkedOverrun413PerRun) {
  Limits l;
  l.max_head_bytes = kMaxHead;
  l.queue_high_watermark = kHigh;
  l.queue_low_watermark = kLow;
  l.max_request_body_bytes = 4;
  Harness h(8, 8, l);
  h.feed(req("POST", "/", "Transfer-Encoding: chunked\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // 3 bytes then 3 more: the second run crosses the 4-byte limit.
  h.feed("3\r\nabc\r\n");
  h.feed("3\r\ndef\r\n");
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 413"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, ChunkedOverrunAfterResponseHeadCloses) {
  Limits l;
  l.max_head_bytes = kMaxHead;
  l.max_request_body_bytes = 4;
  Harness h(8, 8, l);
  h.feed(req("POST", "/", "Transfer-Encoding: chunked\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // Respond with a chunked head but keep the response open; the request body
  // is still being read while the response streams.
  h.respond_chunked(*ex, 200);
  h.push_res(*ex, "ok");
  h.drain_out();
  // The request body now overruns the limit: the head is already out, so the
  // session must not emit a second response; it closes once the response ends.
  h.feed("3\r\nabc\r\n");
  h.feed("3\r\ndef\r\n");
  const std::string out = h.drain_out();
  EXPECT_EQ(out.find("413"), std::string::npos);
  // Finish the in-flight response; the session then closes.
  h.finish_res(*ex);
  h.drain_out();
  EXPECT_TRUE(h.s().should_close());
}

// ---- Blocked / BodyBlocked / stage park ------------------------------------

TEST(ServerSession, BlockedParksTailAndResumesOnReqWritable) {
  // Small BodyStream high watermark so the producer blocks quickly.
  Limits l;
  l.max_head_bytes = kMaxHead;
  Harness h(8, 8, l, /*high=*/16, /*low=*/4);
  h.feed(req("POST", "/", "Content-Length: 40\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // Feed more than the stream can hold; the session must park the tail.
  h.feed(std::string(40, 'x'));
  EXPECT_EQ(h.s().rx(), RxState::BodyBlocked);
  EXPECT_GT(h.s().staged(), 0U);
  EXPECT_EQ(h.s().prepare_recv(), 0U); // no recv while blocked

  // The responder drains the request body; the session resumes.
  std::string got;
  while (!ex->stream(StreamId::Req).end_seen()) {
    const auto f = ex->front(StreamId::Req);
    if (f.empty()) {
      break;
    }
    got += as_string(f);
    ex->consume(StreamId::Req, static_cast<uint32_t>(f.size()));
    h.settle();
  }
  EXPECT_EQ(got, std::string(40, 'x'));
  EXPECT_EQ(h.s().rx(), RxState::Held);
  EXPECT_EQ(h.s().staged(), 0U);
}

TEST(ServerSession, ShortReadReleasesCredit) {
  Harness h;
  h.feed(req("POST", "/", "Content-Length: 10\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // Feed fewer bytes than the claim; the unused credit is released.
  h.feed("abc");
  EXPECT_EQ(h.s().held_credit(), 0U);
  EXPECT_EQ(ex->stream(StreamId::Req).claimed(), 0U);
  EXPECT_EQ(ex->stream(StreamId::Req).queued(), 3U);
}

TEST(ServerSession, PrepareRecvZeroInHeldBodyBlockedStopped) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().rx(), RxState::Held);
  EXPECT_EQ(h.s().prepare_recv(), 0U); // no recv while Held

  // BodyBlocked: a body larger than the stream can hold.
  Limits l;
  l.max_head_bytes = kMaxHead;
  Harness hb(8, 8, l, /*high=*/16, /*low=*/4);
  hb.feed(req("POST", "/", "Content-Length: 40\r\n"));
  Exchange *exb = hb.take();
  ASSERT_NE(exb, nullptr);
  hb.feed(std::string(40, 'x'));
  EXPECT_EQ(hb.s().rx(), RxState::BodyBlocked);
  EXPECT_EQ(hb.s().prepare_recv(), 0U);

  // Stopped: after a hard framing error.
  Harness hs;
  hs.feed("G@T / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(hs.s().rx(), RxState::Stopped);
  EXPECT_EQ(hs.s().prepare_recv(), 0U);
}

TEST(ServerSession, ChunkedDecoderDrivesBodyStream) {
  Harness h;
  h.feed(req("POST", "/", "Transfer-Encoding: chunked\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.feed("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  std::string got;
  while (!ex->stream(StreamId::Req).end_seen()) {
    const auto f = ex->front(StreamId::Req);
    if (f.empty()) {
      break;
    }
    got += as_string(f);
    ex->consume(StreamId::Req, static_cast<uint32_t>(f.size()));
    h.settle();
  }
  EXPECT_EQ(got, "hello world");
  EXPECT_EQ(h.s().rx(), RxState::Held);
}

// ---- pipelining ------------------------------------------------------------

TEST(ServerSession, PipelinedTailParsedAfterFinishCycle) {
  Harness h;
  // Two requests in one recv: the second must NOT be parsed until the first
  // response cycle completes.
  h.feed(req("GET", "/a") + req("GET", "/b"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().served(), 1U);
  EXPECT_EQ(h.take(), nullptr); // second request not parsed yet
  h.respond(*ex, 200, "a");
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");
  // Now the pipelined request is parsed.
  Exchange *ex2 = h.take();
  ASSERT_NE(ex2, nullptr);
  EXPECT_EQ(h.s().served(), 2U);
  h.respond(*ex2, 200, "b");
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb");
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, StageChunkFreedWhenDrained) {
  Harness h;
  h.feed(req("GET", "/a") + req("GET", "/b"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_GT(h.s().staged(), 0U); // the pipelined tail is parked
  h.respond(*ex, 200, "a");
  h.drain_out();
  Exchange *ex2 = h.take();
  ASSERT_NE(ex2, nullptr);
  h.respond(*ex2, 200, "b");
  h.drain_out();
  EXPECT_EQ(h.s().staged(), 0U); // stage chunk returned once drained
  EXPECT_EQ(h.stage_.free_count(), h.stage_.capacity());
}

// ---- Expect: 100-continue --------------------------------------------------

TEST(ServerSession, ExpectDuplicate417) {
  Harness h;
  h.feed(req("POST", "/",
             "Expect: 100-continue\r\nExpect: 100-continue\r\n"
             "Content-Length: 3\r\n"));
  EXPECT_EQ(h.take(), nullptr);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 417"), std::string::npos);
  EXPECT_NE(out.find("Connection: close"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, ExpectUnknown417) {
  Harness h;
  h.feed(req("POST", "/", "Expect: something-else\r\nContent-Length: 3\r\n"));
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 417"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, ExpectIgnoredForHttp10) {
  Harness h;
  h.feed(
      "POST / HTTP/1.0\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // HTTP/1.0: the Expect header is ignored, so the body is read immediately.
  EXPECT_EQ(h.s().rx(), RxState::Body);
}

TEST(ServerSession, ExpectIgnoredForNoBody) {
  Harness h;
  h.feed(req("GET", "/", "Expect: 100-continue\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().rx(), RxState::Held); // no body: no ExpectWait
}

TEST(ServerSession, ExpectIgnoredWhenTailNonEmpty) {
  Harness h;
  h.feed(req("POST", "/", "Expect: 100-continue\r\nContent-Length: 3\r\n") +
         "abc");
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // Body bytes already arrived: no 100, straight to Body.
  EXPECT_EQ(h.s().rx(), RxState::Held);
}

TEST(ServerSession, Continue100OnlyOnPublishInterim) {
  Harness h;
  h.feed(req("POST", "/", "Expect: 100-continue\r\nContent-Length: 3\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().rx(), RxState::ExpectWait);
  // No interim published yet: nothing to send.
  EXPECT_TRUE(h.drain_out().empty());
  // The responder attaches (routing) and publishes 100; the session forwards
  // exactly that.
  EXPECT_TRUE(ex->attach_responder(h.responder_));
  EXPECT_EQ(ex->publish_interim(100), HeadStatus::Ok);
  h.settle();
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 100 Continue\r\n\r\n");
  EXPECT_EQ(h.s().rx(), RxState::ExpectWait);
}

TEST(ServerSession, FinalHeadBefore100Closes) {
  Harness h;
  h.feed(req("POST", "/", "Expect: 100-continue\r\nContent-Length: 3\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().rx(), RxState::ExpectWait);
  // A final head before any 100: no 100 is emitted and the connection closes
  // (the request body was never read).
  ASSERT_TRUE(ex->attach_responder(h.responder_));
  ASSERT_EQ(ex->publish_interim(100), HeadStatus::Ok);
  ASSERT_EQ(ex->respond_synthetic(200), HeadStatus::Ok);
  ex->detach(Side::Responder, Errc::Ok);
  h.settle();
  const std::string out = h.drain_out();
  EXPECT_EQ(out.find("100 Continue"), std::string::npos);
  EXPECT_NE(out.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

// ---- response framing ------------------------------------------------------

TEST(ServerSession, ResponseContentLength) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "hello");
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
}

TEST(ServerSession, ResponseFieldsAndExplicitNoContent) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  ASSERT_TRUE(ex->attach_responder(h.responder_));
  const ResponseField fields[]{{"X-Route", "selected"}, {"ETag", "\"v1\""}};
  const ResponseMeta meta{.status = 200, .no_content = true};
  ASSERT_EQ(ex->publish_final(meta, fields), HeadStatus::Ok);
  ex->detach(Side::Responder, Errc::Ok);
  h.settle();
  EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nX-Route: selected\r\n"
                           "ETag: \"v1\"\r\nContent-Length: 0\r\n\r\n");
}

TEST(ServerSession, ResponseChunked) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond_chunked(*ex, 200);
  h.push_res(*ex, "hello");
  h.finish_res(*ex);
  EXPECT_EQ(h.drain_out(),
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5\r\nhello\r\n0\r\n\r\n");
}

TEST(ServerSession, ResponseHttp10CloseDelimited) {
  Harness h;
  h.feed("GET / HTTP/1.0\r\n\r\n");
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // HTTP/1.0 with no Content-Length: close-delimited, Connection: close.
  h.respond_chunked(*ex, 200);
  h.push_res(*ex, "body");
  h.finish_res(*ex);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"),
            std::string::npos);
  EXPECT_NE(out.find("body"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, ResponseNoBodyStatuses) {
  for (const uint16_t st : {204u, 304u}) {
    Harness h;
    h.feed(req("GET", "/"));
    Exchange *ex = h.take();
    ASSERT_NE(ex, nullptr);
    h.respond(*ex, st, "");
    const std::string out = h.drain_out();
    EXPECT_EQ(out.find("Content-Length"), std::string::npos) << st;
    EXPECT_EQ(out.find("Transfer-Encoding"), std::string::npos) << st;
  }
}

TEST(ServerSession, ResponseHeadOnlyNoBody) {
  Harness h;
  h.feed(req("HEAD", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "ignored");
  const std::string out = h.drain_out();
  // HEAD: the head is sent with the declared length, but no body bytes.
  EXPECT_NE(out.find("Content-Length: 7"), std::string::npos);
  EXPECT_EQ(out.find("ignored"), std::string::npos);
}

TEST(ServerSession, SmallBodyCoalescedIntoOutChunk) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  // A small, already-complete body is coalesced into the head chunk, so the
  // whole response is one contiguous send.
  h.respond(*ex, 200, "abc");
  const auto b = h.s().next_send();
  EXPECT_EQ(as_string(b), "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
  h.s().on_sent(static_cast<uint32_t>(b.size()));
}

// ---- response abort --------------------------------------------------------

TEST(ServerSession, ResAbortedBeforeHead503) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_TRUE(ex->attach_responder(h.responder_));
  ex->abort(StreamId::Res, Errc::UpstreamUnavailable);
  h.settle();
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 503"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, ResponderShutdownDuringContinueSends503) {
  Harness h;
  h.feed(req("POST", "/", "Expect: 100-continue\r\nContent-Length: 3\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  ASSERT_TRUE(ex->attach_responder(h.responder_));
  ASSERT_EQ(ex->publish_interim(100), HeadStatus::Ok);
  h.settle();
  constexpr std::string_view interim = "HTTP/1.1 100 Continue\r\n\r\n";
  constexpr uint32_t sent = 8;
  ASSERT_EQ(as_string(h.s().next_send()), interim);
  h.s().on_sent(sent);
  ex->detach(Side::Responder, Errc::Shutdown);
  h.settle();
  EXPECT_EQ(h.drain_out(),
            std::string(interim.substr(sent)) +
                "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(h.s().should_close());
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, ResAbortedBeforeHeadTimeout504) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_TRUE(ex->attach_responder(h.responder_));
  ex->abort(StreamId::Res, Errc::Timeout);
  h.settle();
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 504"), std::string::npos);
}

TEST(ServerSession, ResAbortedAfterHeadClosesWithoutFinalChunk) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond_chunked(*ex, 200);
  h.push_res(*ex, "partial");
  h.drain_out();
  // Abort the response mid-body: the session must close without a final chunk.
  ex->abort(StreamId::Res, Errc::Reset);
  h.settle();
  const std::string out = h.drain_out();
  EXPECT_EQ(out.find("0\r\n\r\n"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

// ---- keep-alive ------------------------------------------------------------

TEST(ServerSession, KeepAliveServesMultipleRequests) {
  Harness h;
  for (int i = 0; i < 3; ++i) {
    h.feed(req("GET", "/"));
    Exchange *ex = h.take();
    ASSERT_NE(ex, nullptr);
    h.respond(*ex, 200, "x");
    EXPECT_EQ(h.drain_out(), "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
    EXPECT_FALSE(h.s().should_close());
  }
  EXPECT_EQ(h.s().served(), 3U);
  EXPECT_EQ(h.pool_->live(), 0U);
}

TEST(ServerSession, MaxRequestsClosesAfterLimit) {
  Limits l;
  l.max_head_bytes = kMaxHead;
  l.queue_high_watermark = kHigh;
  l.queue_low_watermark = kLow;
  l.max_requests = 2;
  Harness h(8, 8, l);
  for (int i = 0; i < 2; ++i) {
    h.feed(req("GET", "/"));
    Exchange *ex = h.take();
    ASSERT_NE(ex, nullptr);
    h.respond(*ex, 200, "x");
    h.drain_out();
  }
  EXPECT_EQ(h.s().served(), 2U);
  EXPECT_TRUE(h.s().should_close()); // limit reached -> close
}

TEST(ServerSession, OnDrainWhenIdleCloses) {
  Harness h;
  h.s().on_drain();
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, EofInHeadClosesGracefully) {
  Harness h;
  h.feed("GET / HTTP/1.1\r\nHost: a\r\n"); // incomplete head
  h.s().on_eof(Errc::Eof);
  EXPECT_TRUE(h.s().should_close());
  EXPECT_FALSE(h.s().hard_failed());
}

TEST(ServerSession, EofInBodyIsHardError) {
  Harness h;
  h.feed(req("POST", "/", "Content-Length: 10\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.feed("abc");
  h.s().on_eof(Errc::Eof);
  EXPECT_TRUE(h.s().hard_failed());
  EXPECT_TRUE(h.s().should_close());
}

// ---- deadlines -------------------------------------------------------------

TEST(ServerSession, DeadlineHeaderReadBeforeHead) {
  Harness h;
  EXPECT_EQ(h.s().deadline().kind, SessionDeadline::HeaderRead);
  h.s().on_deadline(SessionDeadline::HeaderRead);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 408"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, DeadlineKeepAliveWhenIdle) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "x");
  h.drain_out();
  EXPECT_EQ(h.s().deadline().kind, SessionDeadline::KeepAlive);
  h.s().on_deadline(SessionDeadline::KeepAlive);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, DeadlineBodyIdle408BeforeHead) {
  Harness h;
  h.feed(req("POST", "/", "Content-Length: 10\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().deadline().kind, SessionDeadline::BodyIdle);
  h.s().on_deadline(SessionDeadline::BodyIdle);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 408"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, DeadlineUpstreamFirstByte504BeforeHead) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(h.s().deadline().kind, SessionDeadline::UpstreamFirstByte);
  h.s().on_deadline(SessionDeadline::UpstreamFirstByte);
  const std::string out = h.drain_out();
  EXPECT_NE(out.find("HTTP/1.1 504"), std::string::npos);
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, DeadlineWriteStallHardCloses) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "x");
  // Output is pending: the deadline is WriteStall.
  EXPECT_EQ(h.s().deadline().kind, SessionDeadline::WriteStall);
  h.s().on_deadline(SessionDeadline::WriteStall);
  EXPECT_TRUE(h.s().hard_failed());
  EXPECT_TRUE(h.s().should_close());
}

TEST(ServerSession, DeadlineAfterHeadIsHardClose) {
  Harness h;
  h.feed(req("POST", "/", "Content-Length: 10\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.respond(*ex, 200, "ok");
  h.drain_out();
  // Head already published: a body-idle timeout is a hard close, not a 408.
  h.s().on_deadline(SessionDeadline::BodyIdle);
  EXPECT_TRUE(h.s().hard_failed());
}

// ---- release ---------------------------------------------------------------

TEST(ServerSession, ReleaseReturnsEverything) {
  Harness h;
  h.feed(req("POST", "/", "Content-Length: 10\r\n"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.feed("abc");
  h.s().release();
  h.settle(); // the worker reaps the detached exchange
  EXPECT_EQ(h.pool_->live(), 0U);
  EXPECT_EQ(h.stage_.free_count(), h.stage_.capacity());
}

TEST(ServerSession, ReleaseIsIdempotent) {
  Harness h;
  h.feed(req("GET", "/"));
  Exchange *ex = h.take();
  ASSERT_NE(ex, nullptr);
  h.s().release();
  h.s().release();
  h.settle();
  EXPECT_EQ(h.pool_->live(), 0U);
}

// ---- keep_alive_after / connection_token predicates ------------------------

TEST(ServerSession, KeepAlivePredicate) {
  KeepAliveInput k;
  EXPECT_TRUE(keep_alive_after(k));
  k.request_keep_alive = false;
  EXPECT_FALSE(keep_alive_after(k));
  k = KeepAliveInput{};
  k.close_delimited = true;
  EXPECT_FALSE(keep_alive_after(k));
  k = KeepAliveInput{};
  k.request_incomplete = true;
  EXPECT_FALSE(keep_alive_after(k));
  k = KeepAliveInput{};
  k.draining = true;
  EXPECT_FALSE(keep_alive_after(k));
  k = KeepAliveInput{};
  k.limit_reached = true;
  EXPECT_FALSE(keep_alive_after(k));
  k = KeepAliveInput{};
  k.errored = true;
  EXPECT_FALSE(keep_alive_after(k));
}

TEST(ServerSession, ConnectionToken) {
  EXPECT_EQ(connection_token(false, 1), "close");
  EXPECT_EQ(connection_token(false, 0), "close");
  EXPECT_EQ(connection_token(true, 1), "");
  EXPECT_EQ(connection_token(true, 0), "keep-alive");
}
