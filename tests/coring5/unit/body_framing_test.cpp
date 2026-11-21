// M4.1b: Content-Length body framing (sans-IO) and begin_body().
//
// The framer only counts bytes; the caller claims credit on a BodyStream,
// receives bytes, asks take(avail) how many belong to the body, pushes exactly
// that prefix, and finishes once done(). These tests cover the framer in
// isolation and its interaction with BodyStream::set_declared().

#include "coring5/http/BodyStream.hpp"
#include "coring5/proto/http1/BodyFraming.hpp"
#include "coring5/proto/http1/Parser.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {

constexpr uint32_t kChunk = 16;

std::vector<std::byte> bytes(std::size_t n, char fill = 'x') {
  return std::vector<std::byte>(n, static_cast<std::byte>(fill));
}

std::string as_string(std::span<const std::byte> s) {
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

// Owns a pool big enough for the test and checks nothing leaked when it dies.
struct Fixture {
  explicit Fixture(uint32_t chunks = 256) {
    auto p = ChunkPool::create(kChunk, chunks);
    EXPECT_TRUE(p);
    pool = std::move(*p);
  }
  ~Fixture() {
    EXPECT_EQ(pool.free_count(), pool.capacity()) << "chunks leaked";
  }

  std::unique_ptr<BodyStream> stream(uint32_t high, uint32_t low) {
    return std::make_unique<BodyStream>(pool, StreamLimits{high, low});
  }

  ChunkPool pool;
};

// Parses a request head so begin_body() can be exercised on real input.
RequestHead head_of(const std::string &req) {
  RequestHead h;
  const auto r = parse_head({req.data(), req.size()}, Limits{}, h);
  EXPECT_EQ(r.status, ParseStatus::Done) << req;
  return h;
}

} // namespace

// ---- ContentLengthFramer ---------------------------------------------------

TEST(ContentLengthFramer, NonZeroBodyCountsDown) {
  ContentLengthFramer f;
  f.start(10);
  EXPECT_EQ(f.declared(), 10U);
  EXPECT_EQ(f.remaining(), 10U);
  EXPECT_FALSE(f.done());

  EXPECT_EQ(f.take(4), 4U);
  EXPECT_EQ(f.remaining(), 6U);
  EXPECT_FALSE(f.done());

  EXPECT_EQ(f.take(6), 6U);
  EXPECT_EQ(f.remaining(), 0U);
  EXPECT_TRUE(f.done());
  EXPECT_EQ(f.on_eof(), Errc::Ok);
}

TEST(ContentLengthFramer, ZeroLengthIsCompleteImmediately) {
  ContentLengthFramer f;
  f.start(0);
  EXPECT_EQ(f.declared(), 0U);
  EXPECT_EQ(f.remaining(), 0U);
  EXPECT_TRUE(f.done());
  // Nothing may be taken from a zero-length body.
  EXPECT_EQ(f.take(8), 0U);
  EXPECT_EQ(f.remaining(), 0U);
  EXPECT_EQ(f.on_eof(), Errc::Ok);
}

TEST(ContentLengthFramer, TakeCapsAtRemaining) {
  ContentLengthFramer f;
  f.start(5);
  // avail larger than the body: only the body bytes are taken.
  EXPECT_EQ(f.take(100), 5U);
  EXPECT_EQ(f.remaining(), 0U);
  EXPECT_TRUE(f.done());
  // A further take (pipelined bytes) yields nothing.
  EXPECT_EQ(f.take(100), 0U);
}

TEST(ContentLengthFramer, TakeNeverConsumesPastBoundary) {
  ContentLengthFramer f;
  f.start(3);
  EXPECT_EQ(f.take(1), 1U); // 2 left
  EXPECT_EQ(f.take(1), 1U); // 1 left
  EXPECT_EQ(f.take(1), 1U); // 0 left
  EXPECT_TRUE(f.done());
  // Bytes of the next pipelined request must not be attributed to this body.
  EXPECT_EQ(f.take(64), 0U);
  EXPECT_EQ(f.remaining(), 0U);
}

TEST(ContentLengthFramer, OnEofIncompleteIsViolation) {
  ContentLengthFramer f;
  f.start(10);
  EXPECT_EQ(f.take(4), 4U);
  EXPECT_FALSE(f.done());
  EXPECT_EQ(f.on_eof(), Errc::ProtocolViolation);

  // Completing the body makes EOF acceptable again.
  EXPECT_EQ(f.take(6), 6U);
  EXPECT_TRUE(f.done());
  EXPECT_EQ(f.on_eof(), Errc::Ok);
}

TEST(ContentLengthFramer, OnEofBeforeAnyByteIsViolation) {
  ContentLengthFramer f;
  f.start(1);
  EXPECT_EQ(f.on_eof(), Errc::ProtocolViolation);
}

// ---- begin_body ------------------------------------------------------------

TEST(BeginBody, NoBodyWhenNoFramingHeaders) {
  const auto h = head_of("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
  const auto p = begin_body(h);
  EXPECT_EQ(p.kind, BodyKind::None);
  EXPECT_EQ(p.length, 0U);
  EXPECT_EQ(p.error, Errc::Ok);
}

TEST(BeginBody, ZeroContentLengthIsNoBody) {
  const auto h =
      head_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n");
  EXPECT_TRUE(h.has_content_length);
  EXPECT_EQ(h.content_length, 0U);
  const auto p = begin_body(h);
  EXPECT_EQ(p.kind, BodyKind::None);
  EXPECT_EQ(p.length, 0U);
  EXPECT_EQ(p.error, Errc::Ok);
}

TEST(BeginBody, ContentLength) {
  const auto h =
      head_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 42\r\n\r\n");
  const auto p = begin_body(h);
  EXPECT_EQ(p.kind, BodyKind::ContentLength);
  EXPECT_EQ(p.length, 42U);
  EXPECT_EQ(p.error, Errc::Ok);
}

TEST(BeginBody, Chunked) {
  const auto h = head_of(
      "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n");
  EXPECT_TRUE(h.chunked);
  const auto p = begin_body(h);
  EXPECT_EQ(p.kind, BodyKind::Chunked);
  EXPECT_EQ(p.length, 0U);
  EXPECT_EQ(p.error, Errc::Ok);
}

TEST(BeginBody, RejectsContentLengthWithChunked) {
  // The head parser already rejects CL+TE, so build the RequestHead directly to
  // exercise begin_body's defensive re-check.
  RequestHead h;
  h.chunked = true;
  h.has_content_length = true;
  h.content_length = 4;
  const auto p = begin_body(h);
  EXPECT_EQ(p.error, Errc::ProtocolViolation);
  EXPECT_EQ(p.kind, BodyKind::None);
}

// ---- interaction with BodyStream::set_declared -----------------------------

TEST(BodyFraming, FramerDrivesBodyStream) {
  Fixture f;
  auto s = f.stream(64, 16);

  ContentLengthFramer framer;
  framer.start(10);
  ASSERT_TRUE(s->set_declared(framer.declared()));

  // Feed a buffer that also carries the next pipelined request.
  const std::string wire = "0123456789GET /next HTTP/1.1\r\n";
  std::size_t off = 0;
  while (!framer.done()) {
    const auto avail = wire.size() - off;
    const auto n = framer.take(avail);
    ASSERT_GT(n, 0U);
    const auto c = s->claim(static_cast<uint32_t>(n));
    ASSERT_EQ(c.status, ClaimStatus::Granted);
    ASSERT_EQ(s->push(std::span<const std::byte>(
                  reinterpret_cast<const std::byte *>(wire.data() + off), n)),
              WriteStatus::Ok);
    off += n;
  }
  // Exactly the body was consumed; the pipelined bytes were left behind.
  EXPECT_EQ(off, 10U);
  EXPECT_EQ(s->pushed(), 10U);
  ASSERT_EQ(s->finish(), WriteStatus::Ok);

  std::string got;
  while (!s->front().empty()) {
    const auto fr = s->front();
    got += as_string(fr);
    s->consume(static_cast<uint32_t>(fr.size()));
  }
  EXPECT_EQ(got, "0123456789");
  EXPECT_TRUE(s->end_seen());
}

TEST(BodyFraming, SetDeclaredRejectsShorterThanPushed) {
  Fixture f;
  auto s = f.stream(64, 16);
  // Push 4 bytes first, then declare a shorter body: refused.
  const auto c = s->claim(4);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  ASSERT_EQ(s->push(bytes(4)), WriteStatus::Ok);
  EXPECT_FALSE(s->set_declared(3));
  // Declaring the exact (or larger) length is fine.
  EXPECT_TRUE(s->set_declared(4));
  EXPECT_FALSE(s->set_declared(4)); // only once
}

TEST(BodyFraming, SetDeclaredOnlyOnce) {
  Fixture f;
  auto s = f.stream(64, 16);
  EXPECT_TRUE(s->set_declared(8));
  EXPECT_FALSE(s->set_declared(8));
  EXPECT_FALSE(s->set_declared(16));
}

TEST(BodyFraming, PushingPastDeclaredAborts) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(2));
  const auto c = s->claim(4);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  EXPECT_EQ(s->push(bytes(4)), WriteStatus::Violation);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::ProtocolViolation);
}

TEST(BodyFraming, FinishingShortOfDeclaredAborts) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(4));
  const auto c = s->claim(2);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  ASSERT_EQ(s->push(bytes(2)), WriteStatus::Ok);
  EXPECT_EQ(s->finish(), WriteStatus::Violation);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::ProtocolViolation);
}

TEST(BodyFraming, ZeroLengthBodyFinishesWithoutData) {
  Fixture f;
  auto s = f.stream(64, 16);
  ContentLengthFramer framer;
  framer.start(0);
  ASSERT_TRUE(framer.done());
  ASSERT_TRUE(s->set_declared(framer.declared()));
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_TRUE(s->end_seen());
  EXPECT_EQ(s->pushed(), 0U);
}
