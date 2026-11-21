// M4.1d: request-smuggling corpus. Every entry must be rejected strictly
// (never normalised) by the head parser / body framing / chunked decoder.

#include "coring5/proto/http1/BodyFraming.hpp"
#include "coring5/proto/http1/ChunkedDecoder.hpp"
#include "coring5/proto/http1/Parser.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {

ParseError head_error(const std::string &req) {
  RequestHead h;
  const auto r = parse_head({req.data(), req.size()}, Limits{}, h);
  EXPECT_EQ(r.status, ParseStatus::Error) << req;
  return r.error;
}

std::string req11(const std::string &headers) {
  return "POST / HTTP/1.1\r\nHost: a\r\n" + headers + "\r\n";
}

ChunkedStep decode(const std::string &body) {
  ChunkedDecoder d;
  ChunkedStep s;
  std::span<const char> in{body.data(), body.size()};
  for (;;) {
    s = d.feed(in, 1U << 20);
    if (s.event == ChunkedEvent::Data) {
      in = in.subspan(s.consumed);
      continue;
    }
    return s;
  }
}

} // namespace

TEST(Smuggling, ClAndTeRejected) {
  EXPECT_EQ(
      head_error(req11("Content-Length: 4\r\nTransfer-Encoding: chunked\r\n")),
      ParseError::ConflictingLength);
  EXPECT_EQ(
      head_error(req11("Transfer-Encoding: chunked\r\nContent-Length: 4\r\n")),
      ParseError::ConflictingLength);
}

TEST(Smuggling, DuplicateConflictingClRejected) {
  EXPECT_EQ(head_error(req11("Content-Length: 4\r\nContent-Length: 5\r\n")),
            ParseError::ConflictingLength);
}

TEST(Smuggling, BadClSyntaxRejected) {
  for (const char *v :
       {"+4", "-1", "4 4", "0x4", "", "4,4", "99999999999999999999"}) {
    EXPECT_EQ(head_error(req11(std::string("Content-Length: ") + v + "\r\n")),
              ParseError::BadContentLength)
        << v;
  }
}

TEST(Smuggling, BadTransferEncodingRejected) {
  for (const char *v : {"gzip", "chunked, gzip", "gzip, chunked", "identity",
                        "Xchunked", "chunked;q=1", ""}) {
    EXPECT_EQ(
        head_error(req11(std::string("Transfer-Encoding: ") + v + "\r\n")),
        ParseError::BadTransferEncoding)
        << v;
  }
  EXPECT_EQ(head_error(req11("Transfer-Encoding: chunked\r\n"
                             "Transfer-Encoding: chunked\r\n")),
            ParseError::BadTransferEncoding);
}

TEST(Smuggling, Http10ChunkedRejected) {
  EXPECT_EQ(head_error("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"),
            ParseError::BadTransferEncoding);
}

TEST(Smuggling, HeaderSyntaxRejected) {
  // Whitespace before ':' is not a valid field name (never normalised).
  EXPECT_EQ(head_error(req11("Transfer-Encoding : chunked\r\n")),
            ParseError::BadHeaderName);
  EXPECT_EQ(head_error(req11("Content-Length : 4\r\n")),
            ParseError::BadHeaderName);
  EXPECT_EQ(head_error(req11("X: a\r\n b\r\n")), ParseError::ObsFold);
  EXPECT_EQ(head_error(req11("X: a\r\n\tb\r\n")), ParseError::ObsFold);
  EXPECT_EQ(head_error("POST / HTTP/1.1\r\nHost: a\nX: b\r\n\r\n"),
            ParseError::BareLf);
  // NUL inside a header value is a control character: rejected outright.
  EXPECT_EQ(head_error(req11(std::string("X: a\0b\r\n", 8))),
            ParseError::BadHeader);
  EXPECT_EQ(head_error("POST / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"),
            ParseError::DuplicateHost);
}

TEST(Smuggling, BeginBodyRejectsCombination) {
  RequestHead h;
  h.chunked = true;
  h.has_content_length = true;
  h.content_length = 3;
  EXPECT_EQ(begin_body(h).error, Errc::ProtocolViolation);
}

TEST(Smuggling, BadChunkFramingRejected) {
  struct Case {
    const char *body;
    ChunkedReason reason;
  };
  const Case cases[] = {
      {"\r\n", ChunkedReason::BadChunkSize},
      {"+5\r\nhello\r\n0\r\n\r\n", ChunkedReason::BadChunkSize},
      {"0x5\r\nhello\r\n0\r\n\r\n", ChunkedReason::BadChunkSize},
      {"5 \r\nhello\r\n0\r\n\r\n", ChunkedReason::BadChunkSize},
      {"5\nhello\r\n0\r\n\r\n", ChunkedReason::BadChunkSize},
      {"5\r\nhelloX\r\n0\r\n\r\n", ChunkedReason::BadChunkCrlf},
      {"5\r\nhello\n0\r\n\r\n", ChunkedReason::BadChunkCrlf},
      {"FFFFFFFFFFFFFFFFF\r\n", ChunkedReason::BodyTooLarge},
      {"0\r\nContent-Length: 5\r\n\r\n", ChunkedReason::ForbiddenTrailer},
      {"0\r\nTransfer-Encoding: chunked\r\n\r\n",
       ChunkedReason::ForbiddenTrailer},
      {"0\r\nX : y\r\n\r\n", ChunkedReason::BadTrailer},
      {"0\r\nX: a\r\n b\r\n\r\n", ChunkedReason::BadTrailer},
  };
  for (const auto &c : cases) {
    const auto s = decode(c.body);
    EXPECT_EQ(s.event, ChunkedEvent::Error) << c.body;
    EXPECT_EQ(s.reason, c.reason) << c.body;
  }
}
