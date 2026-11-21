// Unit tests for the sans-IO static handlers (http/Handlers.{hpp,cpp}):
// cold-path validation in Respond::make / Redirect::make (status ranges, size
// limits, token names, header-value CTLs, forbidden framing/hop-by-hop headers,
// Location rules) and the hot-path apply() state machine (happy path, Blocked
// resume with the same ApplyCursor, HEAD/204/304 no-content, wrong phase and
// aborted stream -> Errc::Canceled, clean pool teardown). Sans-IO: no ring.

#include "coring5/http/Exchange.hpp"
#include "coring5/http/ExchangePool.hpp"
#include "coring5/http/Handlers.hpp"
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Coring5;

namespace {

constexpr uint32_t kChunk = 16;
constexpr uint32_t kHigh = 64;
constexpr uint32_t kLow = 16;

std::string as_string(std::span<const std::byte> s) {
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

SideRef side(uint32_t index) {
  return SideRef{ConnRef{PoolId::Http1, Handle{index, 1}}, /*stream=*/index};
}

// Owns a ChunkPool and an ExchangePool; checks nothing leaked when it dies.
struct Fixture {
  explicit Fixture(uint32_t capacity = 8, uint32_t chunks = 256)
      : pool_(std::make_unique<ExchangePool>(capacity, chunk_pool_,
                                             StreamLimits{kHigh, kLow})) {
    auto p = ChunkPool::create(kChunk, chunks);
    EXPECT_TRUE(p);
    chunk_pool_ = std::move(*p);
  }
  ~Fixture() {
    pool_.reset();
    EXPECT_EQ(chunk_pool_.free_count(), chunk_pool_.capacity())
        << "chunks leaked";
  }

  Handle open(RequestMeta meta, uint32_t ingress = 0) {
    return pool_->open(side(ingress), meta);
  }
  Exchange &at(Handle h) {
    Exchange *e = pool_->get(h);
    EXPECT_NE(e, nullptr);
    return *e;
  }
  template <class F> uint32_t drain(F &&f) {
    return pool_->drain(64, std::forward<F>(f));
  }

  ChunkPool chunk_pool_;
  std::unique_ptr<ExchangePool> pool_;
};

RequestMeta with_body(uint64_t declared = 0) {
  RequestMeta m;
  m.has_body = true;
  m.declared_known = declared != 0;
  m.declared = declared;
  return m;
}

std::string body_of(std::size_t n, char fill = 'b') {
  return std::string(n, fill);
}

} // namespace

// ---------------------------------------------------------- Respond::make

TEST(RespondMake, AcceptsStatusRange) {
  EXPECT_TRUE(Respond::make(200, {}, {}).has_value());
  EXPECT_TRUE(Respond::make(204, {}, {}).has_value());
  EXPECT_TRUE(Respond::make(304, {}, {}).has_value());
  EXPECT_TRUE(Respond::make(404, {}, {}).has_value());
  EXPECT_TRUE(Respond::make(599, {}, {}).has_value());
}

TEST(RespondMake, RejectsInvalidStatus) {
  for (uint16_t s : {0, 1, 99, 100, 101, 199, 600, 65535}) {
    const auto r = Respond::make(s, {}, {});
    ASSERT_FALSE(r.has_value()) << "status " << s;
    EXPECT_EQ(r.error().code, Errc::InvalidArgument) << "status " << s;
  }
}

TEST(RespondMake, RejectsBodyOn204And304) {
  for (uint16_t s : {204, 304}) {
    const auto r = Respond::make(s, {}, body_of(1));
    ASSERT_FALSE(r.has_value()) << "status " << s;
    EXPECT_EQ(r.error().code, Errc::InvalidArgument) << "status " << s;
  }
  // Empty body is fine.
  EXPECT_TRUE(Respond::make(204, {}, {}).has_value());
  EXPECT_TRUE(Respond::make(304, {}, {}).has_value());
}

TEST(RespondMake, RejectsTooManyHeaders) {
  std::vector<StaticHeader> hs;
  for (std::size_t i = 0; i <= kMaxStaticHeaders; ++i) {
    hs.push_back(StaticHeader{"X-H" + std::to_string(i), "v"});
  }
  const auto r = Respond::make(200, std::move(hs), {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, Errc::TooLarge);
  // Exactly the limit is accepted.
  std::vector<StaticHeader> ok;
  for (std::size_t i = 0; i < kMaxStaticHeaders; ++i) {
    ok.push_back(StaticHeader{"X-H" + std::to_string(i), "v"});
  }
  EXPECT_TRUE(Respond::make(200, std::move(ok), {}).has_value());
}

TEST(RespondMake, RejectsOversizedHeaderValue) {
  std::vector<StaticHeader> hs{
      StaticHeader{"X-Big", std::string(kMaxStaticHeaderBytes + 1, 'a')}};
  const auto r = Respond::make(200, std::move(hs), {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, Errc::InvalidArgument);
  // Exactly the limit is accepted.
  std::vector<StaticHeader> ok{
      StaticHeader{"X-Big", std::string(kMaxStaticHeaderBytes, 'a')}};
  EXPECT_TRUE(Respond::make(200, std::move(ok), {}).has_value());
}

TEST(RespondMake, RejectsOversizedBody) {
  const auto r = Respond::make(200, {}, body_of(kMaxStaticBodyBytes + 1));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, Errc::TooLarge);
  EXPECT_TRUE(Respond::make(200, {}, body_of(kMaxStaticBodyBytes)).has_value());
}

TEST(RespondMake, RejectsNonTokenHeaderName) {
  for (std::string_view n : {"", "Bad Name", "Bad:Name", "Bad\nName",
                             "Bad\tName", "Bad(Name)", "Bad@Name"}) {
    const auto r = Respond::make(200, {StaticHeader{std::string(n), "v"}}, {});
    ASSERT_FALSE(r.has_value()) << "name [" << n << "]";
    EXPECT_EQ(r.error().code, Errc::InvalidArgument) << "name [" << n << "]";
  }
  // A valid token with the full tchar set is accepted.
  EXPECT_TRUE(Respond::make(200, {StaticHeader{"X-A!#$%&'*+-.^_`|~", "v"}}, {})
                  .has_value());
}

TEST(RespondMake, RejectsControlCharsInHeaderValue) {
  for (char c : {'\r', '\n', '\0', '\x7f', '\x01', '\x1f'}) {
    std::string v = "a";
    v.push_back(c);
    v.push_back('b');
    const auto r = Respond::make(200, {StaticHeader{"X-H", v}}, {});
    ASSERT_FALSE(r.has_value()) << "char " << static_cast<int>(c);
    EXPECT_EQ(r.error().code, Errc::InvalidArgument)
        << "char " << static_cast<int>(c);
  }
  // HTAB is explicitly allowed.
  EXPECT_TRUE(
      Respond::make(200, {StaticHeader{"X-H", "a\tb"}}, {}).has_value());
}

TEST(RespondMake, RejectsForbiddenFramingAndHopByHopHeaders) {
  for (std::string_view n :
       {"Content-Length", "Transfer-Encoding", "Connection", "Keep-Alive", "TE",
        "Trailer", "Upgrade", "Expect", "Proxy-Connection",
        "Proxy-Authenticate", "Proxy-Authorization"}) {
    const auto r = Respond::make(200, {StaticHeader{std::string(n), "v"}}, {});
    ASSERT_FALSE(r.has_value()) << "header " << n;
    EXPECT_EQ(r.error().code, Errc::InvalidArgument) << "header " << n;
  }
}

TEST(RespondMake, ForbiddenHeaderCheckIsCaseInsensitive) {
  for (std::string_view n :
       {"content-length", "CONTENT-LENGTH", "cOnTeNt-LeNgTh",
        "transfer-encoding", "CONNECTION", "keep-alive", "te", "TRAILER",
        "upgrade", "expect", "proxy-connection"}) {
    EXPECT_TRUE(forbidden_static_header(n)) << "header " << n;
  }
  EXPECT_FALSE(forbidden_static_header("X-Content-Length"));
  EXPECT_FALSE(forbidden_static_header("Content-Type"));
}

TEST(RespondMake, ExposesStatusHeadersBody) {
  const auto r =
      Respond::make(201, {StaticHeader{"X-A", "1"}}, body_of(3, 'z'));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status(), 201);
  ASSERT_EQ(r->headers().size(), 1U);
  EXPECT_EQ(r->headers()[0].name, "X-A");
  EXPECT_EQ(r->headers()[0].value, "1");
  EXPECT_EQ(r->body(), body_of(3, 'z'));
}

// --------------------------------------------------------- Redirect::make

TEST(RedirectMake, AcceptsOnlyRedirectStatuses) {
  for (uint16_t s : {301, 302, 303, 307, 308}) {
    EXPECT_TRUE(Redirect::valid_status(s)) << "status " << s;
    EXPECT_TRUE(Redirect::make(s, "/next").has_value()) << "status " << s;
  }
  for (uint16_t s : {200, 204, 300, 304, 305, 306, 309, 400, 500}) {
    EXPECT_FALSE(Redirect::valid_status(s)) << "status " << s;
    const auto r = Redirect::make(s, "/next");
    ASSERT_FALSE(r.has_value()) << "status " << s;
    EXPECT_EQ(r.error().code, Errc::InvalidArgument) << "status " << s;
  }
}

TEST(RedirectMake, RejectsBadLocation) {
  // Empty.
  EXPECT_FALSE(Redirect::valid_location(""));
  // Space, CTL, DEL, and non-ASCII (>= 0x80) are rejected.
  const std::vector<std::string> bad{"a b",
                                     "a\tb",
                                     "a\rb",
                                     "a\nb",
                                     std::string("a") + '\x7f' + "b",
                                     std::string("a") + '\x01' + "b",
                                     std::string("a") +
                                         static_cast<char>(0xff) + "b",
                                     "\xc3\xa9"};
  for (const std::string &loc : bad) {
    EXPECT_FALSE(Redirect::valid_location(loc)) << "loc size " << loc.size();
    const auto r = Redirect::make(302, loc);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Errc::InvalidArgument);
  }
  // Over the limit.
  const std::string big(kMaxLocationBytes + 1, 'a');
  EXPECT_FALSE(Redirect::valid_location(big));
  const auto r = Redirect::make(302, big);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, Errc::InvalidArgument);
  // Exactly the limit is accepted.
  EXPECT_TRUE(
      Redirect::make(302, std::string(kMaxLocationBytes, 'a')).has_value());
}

TEST(RedirectMake, ExposesLocationHeader) {
  const auto r = Redirect::make(308, "/moved/here");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->status(), 308);
  EXPECT_EQ(r->location(), "/moved/here");
  ASSERT_EQ(r->headers().size(), 1U);
  EXPECT_EQ(r->headers()[0].name, "Location");
  EXPECT_EQ(r->headers()[0].value, "/moved/here");
  EXPECT_TRUE(r->response().body().empty());
}

// ------------------------------------------------------------- apply()

TEST(RespondApply, HappyPathPublishesHeadPushesBodyFinishes) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  const auto r = Respond::make(200, {StaticHeader{"X-A", "1"}}, body_of(8));
  ASSERT_TRUE(r.has_value());

  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, ApplyStatus::Done);
  EXPECT_TRUE(cur.head_done);
  EXPECT_EQ(cur.sent, 8U);
  EXPECT_TRUE(e.head_published());
  EXPECT_EQ(e.phase(), Phase::Responding);
  EXPECT_EQ(e.response_meta().status, 200);
  EXPECT_EQ(e.response_meta().declared, 8U);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_EQ(as_string(e.front(StreamId::Res)), body_of(8));
}

TEST(RespondApply, EmptyBodyStillPublishesAndFinishes) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  const auto r = Respond::make(200, {}, {});
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, ApplyStatus::Done);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_EQ(e.stream(StreamId::Res).queued(), 0U);
}

TEST(RespondApply, BlockedResumesWithSameCursor) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  // Body larger than the high watermark: the first apply must block on credit.
  const std::string body = body_of(kHigh * 3);
  const auto r = Respond::make(200, {}, body);
  ASSERT_TRUE(r.has_value());

  ApplyCursor cur;
  const auto first = r->apply(e, cur);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, ApplyStatus::Blocked);
  EXPECT_TRUE(cur.head_done);
  EXPECT_LT(cur.sent, body.size());
  // A blocked producer holds no claim, so nothing is writable until the
  // ingress consumes; the wake is raised by consume(), not by claim().
  EXPECT_EQ(e.pending_events(Side::Responder) & ExEvent::kResWritable, 0);

  // The ingress drains the response; each consume wakes the blocked producer.
  bool done = false;
  while (!done) {
    const uint32_t q = e.stream(StreamId::Res).queued();
    if (q > 0) {
      e.consume(StreamId::Res, q);
      EXPECT_NE(e.pending_events(Side::Responder) & ExEvent::kResWritable, 0);
    }
    const auto st = r->apply(e, cur);
    ASSERT_TRUE(st.has_value());
    done = *st == ApplyStatus::Done;
  }
  EXPECT_EQ(cur.sent, body.size());
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  // Drain the tail the ingress has not read yet.
  e.consume(StreamId::Res, e.stream(StreamId::Res).queued());
  EXPECT_EQ(e.stream(StreamId::Res).queued(), 0U);
}

TEST(RespondApply, HeadOnlyRequestPushesNoBody) {
  Fixture f;
  RequestMeta rm = with_body(10);
  rm.head_only = true;
  const Handle h = f.open(rm);
  Exchange &e = f.at(h);
  const auto r = Respond::make(200, {}, body_of(32));
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, ApplyStatus::Done);
  EXPECT_TRUE(e.response_meta().no_content);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_EQ(e.stream(StreamId::Res).queued(), 0U);
  EXPECT_EQ(cur.sent, 0U);
}

TEST(RespondApply, NoContentStatusPushesNoBody) {
  for (uint16_t s : {204, 304}) {
    Fixture f;
    const Handle h = f.open(with_body(10));
    Exchange &e = f.at(h);
    const auto r = Respond::make(s, {}, {});
    ASSERT_TRUE(r.has_value());
    ApplyCursor cur;
    const auto st = r->apply(e, cur);
    ASSERT_TRUE(st.has_value()) << "status " << s;
    EXPECT_EQ(*st, ApplyStatus::Done) << "status " << s;
    EXPECT_TRUE(e.response_meta().no_content) << "status " << s;
    EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished)
        << "status " << s;
    EXPECT_EQ(e.stream(StreamId::Res).queued(), 0U) << "status " << s;
  }
}

TEST(RespondApply, WrongPhaseIsCanceled) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  // Publish a head first: the phase is now Responding, so a second final head
  // is a wrong-phase publish and apply() must fail with Canceled.
  ASSERT_TRUE(e.attach_responder(side(1)));
  ResponseMeta m;
  m.status = 200;
  ASSERT_EQ(e.publish_final(m), HeadStatus::Ok);
  const auto r = Respond::make(200, {}, body_of(4));
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code, Errc::Canceled);
}

TEST(RespondApply, AbortedStreamIsCanceled) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  e.detach(Side::Ingress, Errc::Reset); // Closing: publish_final -> WrongPhase
  const auto r = Respond::make(200, {}, body_of(4));
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code, Errc::Canceled);
}

TEST(RespondApply, AbortedResStreamAfterHeadIsCanceled) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  const auto r = Respond::make(200, {}, body_of(kHigh * 3));
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto first = r->apply(e, cur);
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(*first, ApplyStatus::Blocked);
  // The response stream dies under the producer: the resume must fail.
  e.abort(StreamId::Res, Errc::Reset);
  const auto st = r->apply(e, cur);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code, Errc::Canceled);
}

TEST(RedirectApply, PublishesLocationAndFinishes) {
  Fixture f;
  const Handle h = f.open(with_body(10));
  Exchange &e = f.at(h);
  const auto r = Redirect::make(302, "/elsewhere");
  ASSERT_TRUE(r.has_value());
  ApplyCursor cur;
  const auto st = r->apply(e, cur);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, ApplyStatus::Done);
  EXPECT_EQ(e.response_meta().status, 302);
  EXPECT_EQ(e.stream(StreamId::Res).state(), StreamState::Finished);
  EXPECT_EQ(e.stream(StreamId::Res).queued(), 0U);
}

// ------------------------------------------------------------- teardown

TEST(HandlersApply, CleanPoolTeardownAfterApply) {
  Fixture f(/*capacity=*/4);
  std::vector<Handle> hs;
  for (uint32_t i = 0; i < 4; ++i) {
    hs.push_back(f.open(with_body(10), i));
  }
  for (Handle h : hs) {
    Exchange &e = f.at(h);
    const auto r = Respond::make(200, {}, body_of(8));
    ASSERT_TRUE(r.has_value());
    ApplyCursor cur;
    ASSERT_TRUE(r->apply(e, cur).has_value());
    // apply() attached a responder; detach both sides so the Exchange is
    // reapable (E11 requires the responder not to be attached).
    e.detach(Side::Responder, Errc::Canceled);
    e.detach(Side::Ingress, Errc::Reset);
  }
  (void)f.drain([](Handle, Exchange &) {});
  EXPECT_EQ(f.pool_->live(), 0U);
  EXPECT_EQ(f.pool_->stats().reaped, 4U);
  EXPECT_FALSE(f.pool_->has_ready());
}
