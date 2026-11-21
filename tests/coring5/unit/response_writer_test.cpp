// M4.1: HTTP/1.1 response serializer (sans-IO, allocation-free, strict).
//
// The writer fills a caller-provided bounded buffer. These tests cover the
// status line, header emission, Content-Length/chunked framing, empty and
// no-content bodies, the chunked encoder helpers, and buffer bounds/overflow
// (which must poison the writer with Errc::TooLarge, never truncate silently).

#include "coring5/proto/http1/ResponseWriter.hpp"
#include <array>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {

// A generous fixed buffer; the writer never allocates.
struct Buf {
  std::array<char, 512> data{};
  std::string str() const { return std::string(data.data(), data.size()); }
};

std::string_view sv(const Buf &b, std::size_t n) { return {b.data.data(), n}; }

} // namespace

// ---- reason_phrase ---------------------------------------------------------

TEST(ReasonPhrase, KnownCodes) {
  EXPECT_EQ(reason_phrase(200), "OK");
  EXPECT_EQ(reason_phrase(204), "No Content");
  EXPECT_EQ(reason_phrase(304), "Not Modified");
  EXPECT_EQ(reason_phrase(404), "Not Found");
  EXPECT_EQ(reason_phrase(500), "Internal Server Error");
  EXPECT_EQ(reason_phrase(503), "Service Unavailable");
}

TEST(ReasonPhrase, UnknownCodeIsEmpty) {
  EXPECT_EQ(reason_phrase(299), "");
  EXPECT_EQ(reason_phrase(599), "");
}

// ---- field validation ------------------------------------------------------

TEST(FieldValidation, Names) {
  EXPECT_TRUE(is_valid_field_name("Content-Type"));
  EXPECT_TRUE(is_valid_field_name("X-Custom_1"));
  EXPECT_TRUE(is_valid_field_name("a!#$%&'*+-.^_`|~"));
  EXPECT_FALSE(is_valid_field_name(""));
  EXPECT_FALSE(is_valid_field_name("Bad Name"));    // SP
  EXPECT_FALSE(is_valid_field_name("Bad:Name"));    // ':'
  EXPECT_FALSE(is_valid_field_name("Bad\r\nName")); // CRLF
  EXPECT_FALSE(is_valid_field_name("Bad\tName"));   // HTAB not a tchar
}

TEST(FieldValidation, Values) {
  EXPECT_TRUE(is_valid_field_value(""));
  EXPECT_TRUE(is_valid_field_value("text/plain; charset=utf-8"));
  EXPECT_TRUE(is_valid_field_value("a\tb")); // HTAB allowed
  EXPECT_FALSE(is_valid_field_value("a\rb"));
  EXPECT_FALSE(is_valid_field_value("a\nb"));
  EXPECT_FALSE(is_valid_field_value(std::string_view("a\0b", 3)));
  EXPECT_FALSE(is_valid_field_value("a\x7f"
                                    "b")); // DEL
}

// ---- status line -----------------------------------------------------------

TEST(StatusLine, DefaultReason) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(200), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\n");
}

TEST(StatusLine, Http10) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(200, 0), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.0 200 OK\r\n");
}

TEST(StatusLine, CustomReason) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(404, 1, "Gone Fishing"), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 404 Gone Fishing\r\n");
}

TEST(StatusLine, UnknownCodeHasEmptyReason) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(299), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 299 \r\n");
}

TEST(StatusLine, RejectsBadStatusAndVersion) {
  {
    Buf b;
    ResponseWriter w(b.data);
    EXPECT_EQ(w.status_line(99), Errc::InvalidArgument);
    EXPECT_EQ(w.error(), Errc::InvalidArgument);
  }
  {
    Buf b;
    ResponseWriter w(b.data);
    EXPECT_EQ(w.status_line(600), Errc::InvalidArgument);
  }
  {
    Buf b;
    ResponseWriter w(b.data);
    EXPECT_EQ(w.status_line(200, 2), Errc::InvalidArgument);
  }
}

TEST(StatusLine, RejectsInjectionInReason) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(200, 1, "OK\r\nX-Evil: 1"), Errc::InvalidArgument);
  EXPECT_EQ(w.size(), 0U); // nothing written
}

TEST(StatusLine, OnlyOnce) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.status_line(200), Errc::Ok);
  // A second status line would corrupt the message: rejected.
  EXPECT_EQ(w.status_line(200), Errc::InvalidArgument);
}

// ---- headers ---------------------------------------------------------------

TEST(Header, Basic) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  EXPECT_EQ(w.header("Content-Type", "text/plain"), Errc::Ok);
  EXPECT_EQ(w.header("X-Trace", "abc"), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\n"
                             "Content-Type: text/plain\r\n"
                             "X-Trace: abc\r\n");
}

TEST(Header, EmptyValueAllowed) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  EXPECT_EQ(w.header("X-Empty", ""), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\nX-Empty: \r\n");
}

TEST(Header, BeforeStatusLineRejected) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.header("X", "y"), Errc::InvalidArgument);
}

TEST(Header, RejectsInjection) {
  {
    Buf b;
    ResponseWriter w(b.data);
    ASSERT_EQ(w.status_line(200), Errc::Ok);
    EXPECT_EQ(w.header("X-Evil\r\nY", "1"), Errc::InvalidArgument);
  }
  {
    Buf b;
    ResponseWriter w(b.data);
    ASSERT_EQ(w.status_line(200), Errc::Ok);
    EXPECT_EQ(w.header("X", "1\r\nY: 2"), Errc::InvalidArgument);
  }
  {
    Buf b;
    ResponseWriter w(b.data);
    ASSERT_EQ(w.status_line(200), Errc::Ok);
    EXPECT_EQ(w.header("X", std::string_view("a\0b", 3)),
              Errc::InvalidArgument);
  }
}

// ---- Content-Length / chunked framing --------------------------------------

TEST(Framing, ContentLength) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  EXPECT_EQ(w.content_length(0), Errc::Ok);
  EXPECT_EQ(w.content_length(1234567890), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\n"
                             "Content-Length: 0\r\n"
                             "Content-Length: 1234567890\r\n");
}

TEST(Framing, Chunked) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  EXPECT_EQ(w.chunked(), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\n"
                             "Transfer-Encoding: chunked\r\n");
}

TEST(Framing, EndHead) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(204), Errc::Ok);
  EXPECT_EQ(w.end_head(), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 204 No Content\r\n\r\n");
}

TEST(Framing, EndHeadWithoutStatusRejected) {
  Buf b;
  ResponseWriter w(b.data);
  EXPECT_EQ(w.end_head(), Errc::InvalidArgument);
}

TEST(Framing, EmptyBodyIsJustHead) {
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  ASSERT_EQ(w.content_length(0), Errc::Ok);
  ASSERT_EQ(w.end_head(), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 200 OK\r\n"
                             "Content-Length: 0\r\n"
                             "\r\n");
}

TEST(Framing, NoContentBodyHasNoFramingHeaders) {
  // 204/304 carry no body and must not advertise Content-Length or chunked.
  Buf b;
  ResponseWriter w(b.data);
  ASSERT_EQ(w.status_line(304), Errc::Ok);
  ASSERT_EQ(w.end_head(), Errc::Ok);
  EXPECT_EQ(sv(b, w.size()), "HTTP/1.1 304 Not Modified\r\n\r\n");
}

// ---- chunked encoder helpers -----------------------------------------------

TEST(ChunkHeader, EncodesLowercaseHex) {
  std::array<char, kChunkHeaderMax> out{};
  EXPECT_EQ(encode_chunk_header(out, 1), 3U);
  EXPECT_EQ(std::string_view(out.data(), 3), "1\r\n");

  EXPECT_EQ(encode_chunk_header(out, 0x1a), 4U);
  EXPECT_EQ(std::string_view(out.data(), 4), "1a\r\n");

  EXPECT_EQ(encode_chunk_header(out, 0xdeadbeef), 10U);
  EXPECT_EQ(std::string_view(out.data(), 10), "deadbeef\r\n");
}

TEST(ChunkHeader, MaxValueFits) {
  std::array<char, kChunkHeaderMax> out{};
  const auto n = encode_chunk_header(out, 0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(n, 18U); // 16 hex digits + CRLF
  EXPECT_EQ(std::string_view(out.data(), n), "ffffffffffffffff\r\n");
}

TEST(ChunkHeader, ZeroReturnsZero) {
  std::array<char, kChunkHeaderMax> out{};
  EXPECT_EQ(encode_chunk_header(out, 0), 0U);
}

TEST(ChunkHeader, TooSmallBufferReturnsZero) {
  std::array<char, 2> out{};
  EXPECT_EQ(encode_chunk_header(out, 0x1a), 0U); // needs 4
}

TEST(LastChunk, NoTrailers) {
  std::array<char, 16> out{};
  const auto r = write_last_chunk(out);
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, 5U);
  EXPECT_EQ(std::string_view(out.data(), *r), "0\r\n\r\n");
}

TEST(LastChunk, WithTrailers) {
  std::array<char, 64> out{};
  const FieldView trailers[] = {{"X-Checksum", "abc"}, {"X-Count", "2"}};
  const auto r = write_last_chunk(out, trailers);
  ASSERT_TRUE(r);
  EXPECT_EQ(std::string_view(out.data(), *r),
            "0\r\nX-Checksum: abc\r\nX-Count: 2\r\n\r\n");
}

TEST(LastChunk, RejectsBadTrailerName) {
  std::array<char, 64> out{};
  const FieldView trailers[] = {{"Bad Name", "x"}};
  const auto r = write_last_chunk(out, trailers);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Errc::InvalidArgument);
}

TEST(LastChunk, RejectsBadTrailerValue) {
  std::array<char, 64> out{};
  const FieldView trailers[] = {{"X", "a\r\nb"}};
  const auto r = write_last_chunk(out, trailers);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Errc::InvalidArgument);
}

TEST(LastChunk, OverflowIsTooLarge) {
  std::array<char, 4> out{};
  const auto r = write_last_chunk(out);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Errc::TooLarge);
}

// ---- buffer bounds / overflow ----------------------------------------------

TEST(Bounds, ExactFitSucceeds) {
  const std::string_view want = "HTTP/1.1 200 OK\r\n\r\n";
  std::vector<char> buf(want.size());
  ResponseWriter w(buf);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  ASSERT_EQ(w.end_head(), Errc::Ok);
  EXPECT_EQ(w.size(), want.size());
  EXPECT_EQ(w.view(), want);
}

TEST(Bounds, OverflowPoisonsWriter) {
  std::array<char, 8> buf{};
  ResponseWriter w(buf);
  EXPECT_EQ(w.status_line(200), Errc::TooLarge);
  EXPECT_EQ(w.error(), Errc::TooLarge);
  // Once poisoned, every further call is a no-op returning the sticky error.
  EXPECT_EQ(w.header("X", "y"), Errc::TooLarge);
  EXPECT_EQ(w.end_head(), Errc::TooLarge);
  EXPECT_EQ(w.size(), 0U); // nothing was partially written
}

TEST(Bounds, OverflowMidHeaderLeavesNoPartialBytes) {
  // Room for the status line but not the header: the header is all-or-nothing.
  const std::string_view status = "HTTP/1.1 200 OK\r\n";
  std::vector<char> buf(status.size() + 4);
  ResponseWriter w(buf);
  ASSERT_EQ(w.status_line(200), Errc::Ok);
  const auto before = w.size();
  EXPECT_EQ(w.header("X-Long", "0123456789"), Errc::TooLarge);
  EXPECT_EQ(w.size(), before); // header not partially appended
}

TEST(Bounds, ZeroSizedBufferFails) {
  std::span<char> empty;
  ResponseWriter w(empty);
  EXPECT_EQ(w.status_line(200), Errc::TooLarge);
}
