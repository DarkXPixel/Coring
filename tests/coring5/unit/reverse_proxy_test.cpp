#include "coring5/http/ExchangePool.hpp"
#include "coring5/http/Handlers.hpp"

#include <array>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>

using namespace Coring5;
using namespace std::literals;

namespace {

using BodyStatus = ProxyBodyStep::Status;

std::span<const std::byte> bytes(std::string_view text) {
  return std::as_bytes(std::span{text});
}

std::string_view view(std::span<const std::byte> data) {
  return {reinterpret_cast<const char *>(data.data()), data.size()};
}

class ReverseProxyTest : public testing::Test {
protected:
  void SetUp() override {
    auto chunks = ChunkPool::create(8, 16);
    ASSERT_TRUE(chunks);
    chunks_ = std::move(*chunks);
    pool_.emplace(1, chunks_, StreamLimits{8, 2});
  }

  void TearDown() override {
    ASSERT_TRUE(pool_);
    EXPECT_EQ(pool_->live(), 0u);
    EXPECT_EQ(chunks_.free_count(), chunks_.capacity());
  }

  Exchange &open(RequestMeta meta = {}) {
    handle_ = pool_->open({}, meta);
    return *pool_->get(handle_);
  }

  void reap(Exchange &ex) {
    ex.detach(Side::Ingress, Errc::Canceled);
    EXPECT_EQ(pool_->drain(1, [](Handle, Exchange &) {}), 1u);
    EXPECT_EQ(pool_->get(handle_), nullptr);
  }

  ChunkPool chunks_;
  std::optional<ExchangePool> pool_;
  Handle handle_;
};

TEST(ReverseProxyHeaders, StripsHopByHopAndRegeneratesForwardingFields) {
  constexpr std::array input{
      ProxyHeader{"X-Hop-Before", "drop"},
      ProxyHeader{"Connection", " close, X-Hop-Before ,,\t"},
      ProxyHeader{"cOnNeCtIoN", "x-hop-after, X-Forwarded-For"},
      ProxyHeader{"X-Hop-After", "drop"},
      ProxyHeader{"Keep-Alive", "timeout=10"},
      ProxyHeader{"Proxy-Connection", "keep-alive"},
      ProxyHeader{"Proxy-Authenticate", "Basic"},
      ProxyHeader{"Proxy-Authorization", "Basic credentials"},
      ProxyHeader{"Proxy-Authentication-Info", "token"},
      ProxyHeader{"TE", "trailers"},
      ProxyHeader{"Trailer", "x-checksum"},
      ProxyHeader{"Upgrade", "websocket"},
      ProxyHeader{"Transfer-Encoding", "chunked"},
      ProxyHeader{"Host", "public.example:8443"},
      ProxyHeader{"Expect", "100-continue"},
      ProxyHeader{"Cookie", "a=b"},
      ProxyHeader{"cookie", "c=d"},
      ProxyHeader{"x-FoRwArDeD-FoR", "10.0.0.1"},
      ProxyHeader{"X-Forwarded-Host", "spoof.example"},
      ProxyHeader{"X-Forwarded-Proto", "http"},
      ProxyHeader{"X-Forwarded-Port", "80"},
      ProxyHeader{"Forwarded", "for=spoof.example"},
  };
  std::array<ProxyHeader, 7> output;
  const auto filtered = ReverseProxy::request_headers(
      input, {"2001:db8::12", "public.example:8443", true}, output);
  ASSERT_TRUE(filtered);
  ASSERT_EQ(filtered->size(), output.size());
  constexpr std::array expected{
      ProxyHeader{"Host", "public.example:8443"},
      ProxyHeader{"Expect", "100-continue"},
      ProxyHeader{"Cookie", "a=b"},
      ProxyHeader{"cookie", "c=d"},
      ProxyHeader{"X-Forwarded-For", "2001:db8::12"},
      ProxyHeader{"X-Forwarded-Host", "public.example:8443"},
      ProxyHeader{"X-Forwarded-Proto", "https"},
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*filtered)[i].name, expected[i].name);
    EXPECT_EQ((*filtered)[i].value, expected[i].value);
  }
}

TEST(ReverseProxyHeaders, BoundsOutputAndRejectsInvalidFields) {
  constexpr ProxyForwarding forwarding{"192.0.2.9", {}};
  constexpr std::array malformed{ProxyHeader{"Connection", "bad option"}};
  constexpr std::array injected{ProxyHeader{"X-Custom", "value\r\nFoo: bar"}};
  std::array<ProxyHeader, 2> output;
  auto result = ReverseProxy::request_headers(malformed, forwarding, output);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Errc::ProtocolViolation);
  result = ReverseProxy::request_headers(injected, forwarding, output);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Errc::ProtocolViolation);
  result =
      ReverseProxy::request_headers({}, forwarding, std::span(output).first(1));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Errc::TooLarge);
  result =
      ReverseProxy::request_headers({}, {"192.0.2.9, 192.0.2.10", {}}, output);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, Errc::InvalidArgument);
  result = ReverseProxy::request_headers({}, forwarding, output);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ((*result)[0].name, "X-Forwarded-For");
  EXPECT_EQ((*result)[0].value, "192.0.2.9");
  EXPECT_EQ((*result)[1].name, "X-Forwarded-Proto");
  EXPECT_EQ((*result)[1].value, "http");
}

TEST_F(ReverseProxyTest, RetriesOnlyIdempotentMethodsAndWithinBudget) {
  for (const auto method :
       {"GET"sv, "HEAD"sv, "PUT"sv, "DELETE"sv, "OPTIONS"sv, "TRACE"sv,
        "POST"sv, "PATCH"sv, "get"sv, "CUSTOM"sv}) {
    SCOPED_TRACE(method);
    auto &ex = open();
    ReverseProxy proxy(UpstreamId{7});
    EXPECT_EQ(proxy.upstream(), UpstreamId{7});
    ASSERT_TRUE(proxy.start(ex, {}, method));
    const bool retryable = method == "GET" || method == "HEAD" ||
                           method == "PUT" || method == "DELETE" ||
                           method == "OPTIONS" || method == "TRACE";
    EXPECT_EQ(
        proxy.on_failure(ex, Errc::UpstreamUnavailable, ProxyRetry::IfUnsent),
        retryable);
    if (retryable) {
      EXPECT_EQ(ex.phase(), Phase::Active);
      EXPECT_FALSE(ex.head_published());
      EXPECT_FALSE(proxy.on_failure(ex, Errc::UpstreamUnavailable,
                                    ProxyRetry::IfUnsent));
    }
    EXPECT_EQ(ex.response_meta().status, 503);
    EXPECT_EQ(ex.responder_state(), Exchange::ResponderState::Detached);
    reap(ex);
  }
}

TEST_F(ReverseProxyTest, RejectsRetryAfterWritingOrOnNonTransientError) {
  auto &wrote_head = open();
  ReverseProxy get(UpstreamId{1}, 3);
  ASSERT_TRUE(get.start(wrote_head, {}, "GET"));
  EXPECT_FALSE(get.on_failure(wrote_head, Errc::Timeout, ProxyRetry::Disallow));
  EXPECT_EQ(wrote_head.response_meta().status, 504);
  reap(wrote_head);

  auto &malformed = open();
  ReverseProxy head(UpstreamId{1});
  ASSERT_TRUE(head.start(malformed, {}, "HEAD"));
  EXPECT_FALSE(head.on_failure(malformed, Errc::ProtocolViolation,
                               ProxyRetry::IfUnsent));
  EXPECT_EQ(malformed.response_meta().status, 502);
  reap(malformed);
}

TEST_F(ReverseProxyTest, InterimResponsePrecludesRetry) {
  auto &ex = open();
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "GET"));
  EXPECT_EQ(proxy.on_interim(ex, 103), HeadStatus::Ok);
  EXPECT_EQ(ex.take_interim(), 103);
  EXPECT_FALSE(proxy.on_failure(ex, Errc::Reset, ProxyRetry::IfUnsent));
  EXPECT_EQ(ex.response_meta().status, 502);
  reap(ex);
}

TEST_F(ReverseProxyTest, RetainsUnsentBodyForRetryButOnceConsumedNeverReplays) {
  auto &ex = open(RequestMeta{.has_body = true});
  ReverseProxy proxy(UpstreamId{1}, 2);
  ASSERT_TRUE(proxy.start(ex, {}, "PUT"));
  ASSERT_EQ(ex.claim(StreamId::Req, 3).status, ClaimStatus::Granted);
  ASSERT_EQ(ex.push(StreamId::Req, bytes("abc")), WriteStatus::Ok);
  std::array<std::byte, 4> buffer;
  ASSERT_TRUE(proxy.prepare_request_body(ex, buffer));
  EXPECT_TRUE(proxy.on_failure(ex, Errc::Reset, ProxyRetry::IfUnsent));
  const auto replay = proxy.prepare_request_body(ex, buffer);
  ASSERT_TRUE(replay);
  ASSERT_EQ(replay->status, BodyStatus::Ready);
  EXPECT_EQ(view(std::span(buffer).first(replay->size)), "abc");
  ASSERT_TRUE(proxy.on_request_sent(ex, 1));
  EXPECT_FALSE(proxy.on_failure(ex, Errc::Reset, ProxyRetry::IfUnsent));
  EXPECT_EQ(ex.response_meta().status, 502);
  EXPECT_EQ(ex.stream(StreamId::Req).queued(), 0u);
  reap(ex);
}

TEST_F(ReverseProxyTest, StreamsBothDirectionsAndResumesOnCredit) {
  auto &ex = open(
      RequestMeta{.has_body = true, .declared_known = true, .declared = 8});
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "PUT"));
  std::array<std::byte, 8> send_buffer;
  auto step = proxy.prepare_request_body(ex, send_buffer);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Blocked);
  ASSERT_EQ(ex.claim(StreamId::Req, 8).status, ClaimStatus::Granted);
  ASSERT_EQ(ex.push(StreamId::Req, bytes("abcdefgh")), WriteStatus::Ok);
  EXPECT_NE(ex.take_events(Side::Responder) & ExEvent::kReqReadable, 0);
  ASSERT_EQ(ex.finish(StreamId::Req), WriteStatus::Ok);
  step = proxy.prepare_request_body(ex, send_buffer);
  ASSERT_TRUE(step);
  ASSERT_EQ(step->size, 8u);
  EXPECT_EQ(view(send_buffer), "abcdefgh");
  ASSERT_TRUE(proxy.on_request_sent(ex, 3));
  EXPECT_EQ(ex.consumed(StreamId::Req), 3u);
  step = proxy.prepare_request_body(ex, send_buffer);
  ASSERT_TRUE(step);
  ASSERT_EQ(step->size, 5u);
  EXPECT_EQ(view(std::span(send_buffer).first(step->size)), "defgh");
  ASSERT_TRUE(proxy.on_request_sent(ex, 5));
  step = proxy.prepare_request_body(ex, send_buffer);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Done);

  constexpr std::array upstream{
      ProxyHeader{"Set-Cookie", "a=b"},
      ProxyHeader{"Connection", "x-hop"},
      ProxyHeader{"x-hop", "drop"},
      ProxyHeader{"Transfer-Encoding", "chunked"},
      ProxyHeader{"Set-Cookie", "c=d"},
  };
  ApplyCursor cur;
  std::array<ProxyHeader, 2> output;
  const auto head = proxy.on_response_head(ex, cur, ResponseMeta{.status = 200},
                                           upstream, output);
  ASSERT_TRUE(head);
  ASSERT_EQ(head->size(), 2u);
  EXPECT_EQ((*head)[0].value, "a=b");
  EXPECT_EQ((*head)[1].value, "c=d");
  EXPECT_NE(ex.take_events(Side::Ingress) & ExEvent::kResponseHeadReady, 0);
  EXPECT_TRUE(ex.wait_readable(StreamId::Res));

  step = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Ready);
  EXPECT_EQ(step->size, 8u);
  const auto pending = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->status, BodyStatus::Blocked);
  ASSERT_TRUE(proxy.on_read(ex, cur, bytes("12345678"), false));
  EXPECT_NE(ex.take_events(Side::Ingress) & ExEvent::kResReadable, 0);
  step = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Blocked);
  EXPECT_EQ(view(ex.front(StreamId::Res)), "12345678");
  ex.consume(StreamId::Res, 6);
  EXPECT_EQ(ex.take_events(Side::Responder) & ExEvent::kResWritable, 0);
  ex.consume(StreamId::Res, 2);
  EXPECT_NE(ex.take_events(Side::Responder) & ExEvent::kResWritable, 0);
  step = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Ready);
  ASSERT_TRUE(proxy.on_read(ex, cur, bytes(""), false)); // framing only
  EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
  step = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Ready);
  ASSERT_TRUE(proxy.on_read(ex, cur, bytes("90"), true));
  EXPECT_EQ(cur.sent, 10u);
  EXPECT_EQ(ex.responder_state(), Exchange::ResponderState::Detached);
  step = proxy.apply(ex, cur, 16);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Done);
  EXPECT_EQ(view(ex.front(StreamId::Res)), "90");
  ex.consume(StreamId::Res, 2);
  EXPECT_TRUE(ex.stream(StreamId::Res).end_seen());
  reap(ex);
}

TEST_F(ReverseProxyTest, EarlyFinalCancelsUploadAndIgnoresLateSend) {
  auto &ex = open(RequestMeta{.has_body = true});
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "POST"));
  EXPECT_EQ(proxy.on_interim(ex, 100), HeadStatus::Ok);
  EXPECT_EQ(ex.take_interim(), 100);
  ASSERT_EQ(ex.claim(StreamId::Req, 8).status, ClaimStatus::Granted);
  ASSERT_EQ(ex.push(StreamId::Req, bytes("staged")), WriteStatus::Ok);
  std::array<std::byte, 8> buffer;
  const auto staged = proxy.prepare_request_body(ex, buffer);
  ASSERT_TRUE(staged);
  ApplyCursor cur;
  ASSERT_TRUE(
      proxy.on_response_head(ex, cur, ResponseMeta{.status = 204}, {}, {}));
  EXPECT_EQ(ex.stream(StreamId::Req).aborted_with(), Errc::Canceled);
  EXPECT_EQ(view(std::span(buffer).first(staged->size)), "staged");
  const auto sent = proxy.on_request_sent(ex, staged->size);
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error().code, Errc::Canceled);
  const auto step = proxy.apply(ex, cur, 8);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Done);
  reap(ex);
}

TEST_F(ReverseProxyTest, CancelWithReadInFlightReturnsCreditAndReaps) {
  auto &ex = open();
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "GET"));
  ApplyCursor cur;
  ASSERT_TRUE(
      proxy.on_response_head(ex, cur, ResponseMeta{.status = 200}, {}, {}));
  ASSERT_TRUE(proxy.apply(ex, cur, 8));
  ASSERT_EQ(ex.stream(StreamId::Res).claimed(), 8u);
  ex.detach(Side::Ingress, Errc::Timeout);
  const auto late = proxy.on_read(ex, cur, bytes("late"), false);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error().code, Errc::Canceled);
  EXPECT_EQ(ex.stream(StreamId::Res).claimed(), 0u);
  EXPECT_EQ(ex.responder_state(), Exchange::ResponderState::Detached);
  reap(ex);
}

TEST_F(ReverseProxyTest, FinishesZeroLengthResponseWithoutRead) {
  auto &ex = open();
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "GET"));
  ApplyCursor cur;
  constexpr std::array upstream{ProxyHeader{"Content-Length", "0"}};
  const auto head = proxy.on_response_head(
      ex, cur, ResponseMeta{.status = 200, .declared_known = true}, upstream,
      {});
  ASSERT_TRUE(head);
  EXPECT_TRUE(head->empty());
  EXPECT_TRUE(ex.response_meta().declared_known);
  EXPECT_EQ(ex.response_meta().declared, 0u);
  const auto step = proxy.apply(ex, cur, 0);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->status, BodyStatus::Done);
  reap(ex);
}

TEST_F(ReverseProxyTest, RejectsShortDeclaredResponseWithoutRetry) {
  auto &ex = open();
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "GET"));
  ApplyCursor cur;
  ASSERT_TRUE(proxy.on_response_head(
      ex, cur,
      ResponseMeta{.status = 200, .declared_known = true, .declared = 4}, {},
      {}));
  ASSERT_TRUE(proxy.apply(ex, cur, 8));
  const auto read = proxy.on_read(ex, cur, bytes("ab"), true);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().code, Errc::ProtocolViolation);
  EXPECT_FALSE(proxy.on_failure(ex, Errc::Reset, ProxyRetry::IfUnsent));
  EXPECT_EQ(ex.response_meta().status, 200);
  EXPECT_EQ(ex.stream(StreamId::Res).state(), StreamState::Aborted);
  reap(ex);
}

TEST_F(ReverseProxyTest, InvalidResponseHeadBecomesBadGateway) {
  auto &ex = open();
  ReverseProxy proxy(UpstreamId{1});
  ASSERT_TRUE(proxy.start(ex, {}, "GET"));
  ApplyCursor cur;
  constexpr std::array invalid{ProxyHeader{"Connection", "bad option"}};
  const auto head =
      proxy.on_response_head(ex, cur, ResponseMeta{.status = 200}, invalid, {});
  ASSERT_FALSE(head);
  EXPECT_EQ(head.error().code, Errc::ProtocolViolation);
  EXPECT_FALSE(cur.head_done);
  EXPECT_EQ(ex.response_meta().status, 502);
  EXPECT_TRUE(ex.response_meta().synthesized);
  reap(ex);
}

} // namespace
