#include "coring5/proto/http1/Parser.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {
HeadResult parse(const std::string &s, RequestHead &h, const Limits &l = {}) {
  return parse_head({s.data(), s.size()}, l, h);
}
} // namespace

TEST(Http1Head, SimpleGet) {
  RequestHead h;
  const std::string req = "GET /index.html?a=1 HTTP/1.1\r\nHost: "
                          "example.com\r\nUser-Agent: t\r\n\r\n";
  const auto r = parse(req, h);
  ASSERT_EQ(r.status, ParseStatus::Done);
  EXPECT_EQ(r.consumed, req.size());
  EXPECT_EQ(h.method, "GET");
  EXPECT_EQ(h.target, "/index.html?a=1");
  EXPECT_EQ(h.host, "example.com");
  EXPECT_EQ(h.header_count, 2U);
  EXPECT_TRUE(h.keep_alive);
  EXPECT_EQ(h.find("user-agent"), "t");
}

TEST(Http1Head, NeedMoreUntilTerminator) {
  RequestHead h;
  const std::string req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  for (std::size_t n = 0; n < req.size(); ++n) {
    EXPECT_EQ(parse(req.substr(0, n), h).status, ParseStatus::NeedMore) << n;
  }
  EXPECT_EQ(parse(req, h).status, ParseStatus::Done);
}

TEST(Http1Head, ConsumedExcludesPipelinedBytes) {
  RequestHead h;
  const std::string one = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
  const auto r = parse(one + "GET /b HTTP/1.1\r\n", h);
  ASSERT_EQ(r.status, ParseStatus::Done);
  EXPECT_EQ(r.consumed, one.size());
}

TEST(Http1Head, KeepAliveRules) {
  RequestHead h;
  ASSERT_EQ(parse("GET / HTTP/1.0\r\n\r\n", h).status, ParseStatus::Done);
  EXPECT_FALSE(h.keep_alive);
  ASSERT_EQ(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", h).status,
            ParseStatus::Done);
  EXPECT_TRUE(h.keep_alive);
  ASSERT_EQ(parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", h).status,
            ParseStatus::Done);
  EXPECT_FALSE(h.keep_alive);
}

TEST(Http1Head, BodyFramingHeaders) {
  RequestHead h;
  ASSERT_EQ(parse("POST / HTTP/1.1\r\nContent-Length: 42\r\n\r\n", h).status,
            ParseStatus::Done);
  EXPECT_TRUE(h.has_content_length);
  EXPECT_EQ(h.content_length, 42U);
  ASSERT_EQ(
      parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", h).status,
      ParseStatus::Done);
  EXPECT_TRUE(h.chunked);
  // identical duplicate CL is tolerated, conflicting is not (below)
  ASSERT_EQ(
      parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
            h)
          .status,
      ParseStatus::Done);
}

// ---- request smuggling / malformed input: every case must be REJECTED ------
namespace {
struct BadCase {
  const char *name;
  std::string input;
  ParseError expect;
};
} // namespace

TEST(Http1Head, RejectsSmugglingAndMalformedInput) {
  const std::vector<BadCase> cases = {
      {"CL+TE",
       "POST / HTTP/1.1\r\nContent-Length: 4\r\nTransfer-Encoding: "
       "chunked\r\n\r\n",
       ParseError::ConflictingLength},
      {"TE+CL",
       "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: "
       "4\r\n\r\n",
       ParseError::ConflictingLength},
      {"conflicting CL",
       "POST / HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n",
       ParseError::ConflictingLength},
      {"TE gzip", "POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
       ParseError::BadTransferEncoding},
      {"TE obfuscated",
       "POST / HTTP/1.1\r\nTransfer-Encoding: xchunked\r\n\r\n",
       ParseError::BadTransferEncoding},
      {"TE twice",
       "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
       "chunked\r\n\r\n",
       ParseError::BadTransferEncoding},
      {"CL negative", "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
       ParseError::BadContentLength},
      {"CL plus", "POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\n",
       ParseError::BadContentLength},
      {"CL junk", "POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n",
       ParseError::BadContentLength},
      {"CL empty", "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n",
       ParseError::BadContentLength},
      {"CL huge",
       "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
       ParseError::BadContentLength},
      {"space before colon", "GET / HTTP/1.1\r\nHost : a\r\n\r\n",
       ParseError::BadHeaderName},
      {"obs-fold", "GET / HTTP/1.1\r\nX: a\r\n b\r\n\r\n", ParseError::ObsFold},
      {"bare LF in header", "GET / HTTP/1.1\nHost: a\r\n\r\n",
       ParseError::BareLf},
      {"bare LF terminator", "GET / HTTP/1.1\r\nHost: a\r\n\n",
       ParseError::BareLf},
      {"NUL in value",
       std::string("GET / HTTP/1.1\r\nX: a\0b\r\n\r\n",
                   sizeof("GET / HTTP/1.1\r\nX: a\0b\r\n\r\n") - 1),
       ParseError::BadHeader},
      {"no colon", "GET / HTTP/1.1\r\nHost a\r\n\r\n", ParseError::BadHeader},
      {"empty name", "GET / HTTP/1.1\r\n: a\r\n\r\n", ParseError::BadHeader},
      {"bad method", "G@T / HTTP/1.1\r\n\r\n", ParseError::BadMethod},
      {"empty method", " / HTTP/1.1\r\n\r\n", ParseError::BadMethod},
      {"bad version", "GET / HTTP/2.0\r\n\r\n", ParseError::BadVersion},
      {"version lowercase", "GET / http/1.1\r\n\r\n", ParseError::BadVersion},
      {"extra spaces", "GET  / HTTP/1.1\r\n\r\n", ParseError::BadRequestLine},
      {"missing version", "GET /\r\n\r\n", ParseError::BadRequestLine},
      {"too many parts", "GET / HTTP/1.1 x\r\n\r\n",
       ParseError::BadRequestLine},
  };
  for (const auto &c : cases) {
    RequestHead h;
    const auto r = parse(c.input, h);
    EXPECT_EQ(r.status, ParseStatus::Error) << c.name;
    EXPECT_EQ(r.error, c.expect) << c.name;
  }
}

TEST(Http1Head, EnforcesLimits) {
  Limits l;
  l.max_head_bytes = 256;
  l.max_headers = 3;
  l.max_uri_bytes = 16;
  RequestHead h;

  EXPECT_EQ(
      parse("GET /" + std::string(32, 'a') + " HTTP/1.1\r\n\r\n", h, l).error,
      ParseError::UriTooLong);
  EXPECT_EQ(
      parse("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n", h, l)
          .error,
      ParseError::TooManyHeaders);
  // oversized head: rejected as soon as the limit is crossed, without a
  // terminator
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nX: " + std::string(300, 'a'), h, l).error,
            ParseError::HeadTooLarge);
  // oversized head WITH terminator
  EXPECT_EQ(
      parse("GET / HTTP/1.1\r\nX: " + std::string(300, 'a') + "\r\n\r\n", h, l)
          .error,
      ParseError::HeadTooLarge);
}

TEST(Http1Head, SegmentationDoesNotChangeTheVerdict) {
  const std::string req =
      "POST /upload HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc";
  RequestHead whole;
  const auto w = parse(req, whole);
  ASSERT_EQ(w.status, ParseStatus::Done);
  for (std::size_t step = 1; step < req.size(); ++step) {
    RequestHead h;
    HeadResult r;
    std::string prefix; // must outlive `h`: its string_views point into it
    for (std::size_t n = step;; n += step) {
      prefix = req.substr(0, std::min(n, req.size()));
      r = parse(prefix, h);
      if (r.status != ParseStatus::NeedMore || n >= req.size()) {
        break;
      }
    }
    EXPECT_EQ(r.status, w.status) << step;
    EXPECT_EQ(r.consumed, w.consumed) << step;
    EXPECT_EQ(h.target, whole.target) << step;
  }
}
