// M4.1c: HTTP/1.1 chunked body decoder (sans-IO).
//
// The decoder consumes bytes and reports one event per feed(): NeedMore,
// NeedCredit, Data (a zero-copy sub-span of the input), Done or Error. It is
// strict: it rejects, never normalises. These tests cover the happy path, split
// invariance (byte-by-byte and seeded random splits), pipelined tails, credit
// and limit handling, the strict rejects, trailer validation and the
// integration with BodyStream (including a pool leak check).

#include "coring5/http/BodyStream.hpp"
#include "coring5/proto/http1/ChunkedDecoder.hpp"
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace Coring5;
using namespace Coring5::Http1;

namespace {

constexpr uint32_t kChunk = 16;

std::span<const char> span_of(std::string_view s) {
  return {s.data(), s.size()};
}

std::string as_string(std::span<const char> s) {
  return std::string(s.data(), s.size());
}

std::string as_string(std::span<const std::byte> s) {
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

// The full observable outcome of decoding one buffer, used to compare a
// single-buffer run against any split of the same bytes.
struct Outcome {
  std::string data;
  std::size_t consumed{0};
  uint32_t trailers{0};
  uint64_t body_bytes{0};
  bool done{false};
  bool failed{false};
  ChunkedEvent last_event{ChunkedEvent::NeedMore};
  Errc error{Errc::Ok};
  ChunkedReason reason{ChunkedReason::None};

  bool operator==(const Outcome &) const = default;
};

// Feeds `in` in fixed-size pieces (piece == in.size() means one shot) with
// unlimited credit, carrying the decoder across calls. Only the meaningful
// events (Data/Done/Error) are recorded, so the outcome is independent of how
// the input was chopped up.
Outcome run(std::string_view in, std::size_t piece,
            const Limits &limits = Limits{},
            uint32_t max_ext = ChunkedDecoder::kDefaultMaxExtBytes) {
  ChunkedDecoder d(limits, max_ext);
  Outcome o;
  std::size_t off = 0;
  while (off < in.size()) {
    const std::size_t n = std::min(piece, in.size() - off);
    const auto s = d.feed(span_of(in).subspan(off, n), 1u << 20);
    off += s.consumed;
    if (s.event == ChunkedEvent::Data) {
      o.last_event = s.event;
      o.data += as_string(s.data);
    } else if (s.event == ChunkedEvent::Done) {
      o.last_event = s.event;
      o.done = true;
      break;
    } else if (s.event == ChunkedEvent::Error) {
      o.last_event = s.event;
      o.error = s.error;
      o.reason = s.reason;
      o.failed = true;
      break;
    }
  }
  o.consumed = off;
  o.trailers = d.trailers_seen();
  o.body_bytes = d.body_bytes();
  return o;
}

// Feeds `in` in one shot with unlimited credit.
Outcome run_single(std::string_view in, const Limits &limits = Limits{},
                   uint32_t max_ext = ChunkedDecoder::kDefaultMaxExtBytes) {
  return run(in, in.size(), limits, max_ext);
}

// Feeds `in` in fixed-size pieces, carrying the decoder across calls.
Outcome run_split(std::string_view in, std::size_t piece,
                  const Limits &limits = Limits{},
                  uint32_t max_ext = ChunkedDecoder::kDefaultMaxExtBytes) {
  return run(in, piece, limits, max_ext);
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

} // namespace

// ---- happy path ------------------------------------------------------------

TEST(ChunkedDecoder, SingleChunk) {
  const auto o = run_single("5\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_FALSE(o.failed);
  EXPECT_EQ(o.data, "hello");
  EXPECT_EQ(o.consumed, 15U);
  EXPECT_EQ(o.body_bytes, 5U);
  EXPECT_EQ(o.trailers, 0U);
}

TEST(ChunkedDecoder, MultipleChunks) {
  const auto o = run_single("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello world");
  EXPECT_EQ(o.body_bytes, 11U);
}

TEST(ChunkedDecoder, UppercaseHex) {
  const auto o = run_single("A\r\n0123456789\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "0123456789");
  EXPECT_EQ(o.body_bytes, 10U);
}

TEST(ChunkedDecoder, LowercaseHex) {
  const auto o = run_single("a\r\n0123456789\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "0123456789");
}

TEST(ChunkedDecoder, MixedCaseHex) {
  // 0x1A == 26 bytes of payload.
  const auto o = run_single("1A\r\n01234567890123456789012345\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.body_bytes, 26U);
}

TEST(ChunkedDecoder, EmptyBody) {
  const auto o = run_single("0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "");
  EXPECT_EQ(o.consumed, 5U);
  EXPECT_EQ(o.body_bytes, 0U);
}

TEST(ChunkedDecoder, ChunkExtensions) {
  const auto o = run_single("5;foo=bar\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello");
}

TEST(ChunkedDecoder, ChunkExtensionsQuotedAndMultiple) {
  const auto o = run_single("5;a=\"b;c\";d=e\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello");
}

TEST(ChunkedDecoder, ChunkExtensionQuotedEscape) {
  const auto o = run_single("5;a=\"b\\\"c\"\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello");
}

TEST(ChunkedDecoder, TrailersAcceptedAndCounted) {
  const auto o =
      run_single("5\r\nhello\r\n0\r\nX-Foo: bar\r\nX-Baz: qux\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello");
  EXPECT_EQ(o.trailers, 2U);
}

TEST(ChunkedDecoder, TrailerWithEmptyValue) {
  const auto o = run_single("0\r\nX-Empty:\r\n\r\n");
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.trailers, 1U);
}

// ---- split invariance ------------------------------------------------------

namespace {

const std::vector<std::string> kCorpus{
    "5\r\nhello\r\n0\r\n\r\n",
    "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
    "A\r\n0123456789\r\n0\r\n\r\n",
    "5;foo=bar;baz=\"q\\\"z\"\r\nhello\r\n0\r\n\r\n",
    "5\r\nhello\r\n0\r\nX-Foo: bar\r\nX-Baz: qux\r\n\r\n",
    "0\r\n\r\n",
    "0\r\nX-T: 1\r\n\r\n",
    // error cases: the reason must survive any split too
    "Z\r\nhello\r\n0\r\n\r\n",
    "5\r\nhello\r\n0\r\ncontent-length: 1\r\n\r\n",
    "5\r\nhello\r\n0\r\nbad name: x\r\n\r\n",
    "5\r\nhello\r\n0\r\nX: a\r\n", // truncated trailer
    "5\r\nhello",                  // truncated body
};

} // namespace

TEST(ChunkedDecoder, SplitInvarianceByteByByte) {
  for (const auto &wire : kCorpus) {
    const auto single = run_single(wire);
    const auto split = run_split(wire, 1);
    EXPECT_EQ(single, split) << "wire=" << wire;
  }
}

TEST(ChunkedDecoder, SplitInvarianceSeededRandom) {
  std::mt19937 rng(0xC0FFEE);
  for (const auto &wire : kCorpus) {
    const auto single = run_single(wire);
    for (int trial = 0; trial < 64; ++trial) {
      std::uniform_int_distribution<std::size_t> pick(1, 4);
      const auto piece = pick(rng);
      const auto split = run_split(wire, piece);
      EXPECT_EQ(single, split) << "wire=" << wire << " piece=" << piece;
    }
  }
}

TEST(ChunkedDecoder, SplitInvarianceEveryBoundary) {
  const std::string wire = "5;e=1\r\nhello\r\n0\r\nX-Foo: bar\r\n\r\n";
  const auto single = run_single(wire);
  for (std::size_t piece = 1; piece <= wire.size(); ++piece) {
    EXPECT_EQ(single, run_split(wire, piece)) << "piece=" << piece;
  }
}

// ---- pipelined tail --------------------------------------------------------

TEST(ChunkedDecoder, PipelinedTailLeftUnconsumed) {
  const std::string wire = "5\r\nhello\r\n0\r\n\r\nGET /next HTTP/1.1\r\n";
  ChunkedDecoder d;
  std::size_t off = 0;
  ChunkedStep s;
  do {
    s = d.feed(span_of(wire).subspan(off), 1u << 20);
    off += s.consumed;
  } while (s.event == ChunkedEvent::Data);
  ASSERT_EQ(s.event, ChunkedEvent::Done);
  EXPECT_EQ(off, 15U); // stops right after the terminating CRLF
  EXPECT_EQ(wire.substr(off), "GET /next HTTP/1.1\r\n");
}

TEST(ChunkedDecoder, DoneIsIdempotent) {
  ChunkedDecoder d;
  const auto first = d.feed(span_of("0\r\n\r\n"), 1u << 20);
  ASSERT_EQ(first.event, ChunkedEvent::Done);
  EXPECT_TRUE(d.done());

  // Further feeds report Done again and consume nothing.
  const auto again = d.feed(span_of("garbage"), 1u << 20);
  EXPECT_EQ(again.event, ChunkedEvent::Done);
  EXPECT_EQ(again.consumed, 0U);
  EXPECT_EQ(d.on_eof(), Errc::Ok);
}

// ---- credit / max_data -----------------------------------------------------

TEST(ChunkedDecoder, NeedCreditWhenNoCredit) {
  ChunkedDecoder d;
  const auto s = d.feed(span_of("5\r\nhello\r\n0\r\n\r\n"), 0);
  ASSERT_EQ(s.event, ChunkedEvent::NeedCredit);
  EXPECT_EQ(s.consumed, 3U); // "5\r\n" framing consumed, no data taken
  EXPECT_EQ(s.data.size(), 0U);
}

TEST(ChunkedDecoder, MaxDataChunksTheRun) {
  ChunkedDecoder d;
  const std::string wire = "5\r\nhello\r\n0\r\n\r\n";
  std::string got;
  std::size_t off = 0;
  for (;;) {
    const auto s = d.feed(span_of(wire).subspan(off), 2);
    off += s.consumed;
    if (s.event == ChunkedEvent::Data) {
      EXPECT_LE(s.data.size(), 2U);
      got += as_string(s.data);
    } else if (s.event == ChunkedEvent::Done) {
      break;
    } else {
      FAIL() << "unexpected event " << static_cast<int>(s.event);
    }
  }
  EXPECT_EQ(got, "hello");
}

TEST(ChunkedDecoder, NeedCreditResumesAtSameByte) {
  ChunkedDecoder d;
  const std::string wire = "5\r\nhello\r\n0\r\n\r\n";
  // First feed with zero credit: framing only.
  auto s = d.feed(span_of(wire), 0);
  ASSERT_EQ(s.event, ChunkedEvent::NeedCredit);
  const std::size_t off = s.consumed;
  // Resume with credit from exactly where we stopped.
  s = d.feed(span_of(wire).subspan(off), 5);
  ASSERT_EQ(s.event, ChunkedEvent::Data);
  EXPECT_EQ(as_string(s.data), "hello");
}

// ---- size / limit errors ---------------------------------------------------

TEST(ChunkedDecoder, HexOverflowTooManyDigits) {
  // 17 hex digits: more than 16 -> TooLarge.
  const auto o = run_single("10000000000000000\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.error, Errc::TooLarge);
  EXPECT_EQ(o.reason, ChunkedReason::BodyTooLarge);
}

TEST(ChunkedDecoder, SixteenDigitsAccepted) {
  // 16 hex digits is the maximum accepted size; the size line parses and the
  // decoder then waits for the (never delivered) payload.
  ChunkedDecoder d;
  const auto s = d.feed(span_of("FFFFFFFFFFFFFFFF\r\n"), 1u << 20);
  EXPECT_EQ(s.event, ChunkedEvent::NeedMore);
  EXPECT_FALSE(d.failed());
}

TEST(ChunkedDecoder, BodyTooLargeOnSumOverflow) {
  // A small first chunk is delivered, then a second size line whose value
  // overflows the running total trips the guard at St::SizeLf.
  const std::string wire = "1\r\nx\r\nFFFFFFFFFFFFFFFF\r\n";
  ChunkedDecoder d;
  std::size_t off = 0;
  auto s = d.feed(span_of(wire).subspan(off), 1u << 20);
  ASSERT_EQ(s.event, ChunkedEvent::Data);
  EXPECT_EQ(as_string(s.data), "x");
  off += s.consumed;
  EXPECT_EQ(d.body_bytes(), 1U);

  // Second chunk size = UINT64_MAX; 1 + UINT64_MAX overflows.
  s = d.feed(span_of(wire).subspan(off), 1u << 20);
  EXPECT_EQ(s.event, ChunkedEvent::Error);
  EXPECT_EQ(s.error, Errc::TooLarge);
  EXPECT_EQ(s.reason, ChunkedReason::BodyTooLarge);
  EXPECT_TRUE(d.failed());
}

// ---- strict rejects --------------------------------------------------------

TEST(ChunkedDecoder, RejectsBareLfAfterSize) {
  // A bare LF where the size line's CRLF is expected is a bad size line.
  const auto o = run_single("5\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.error, Errc::ProtocolViolation);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkSize);
}

TEST(ChunkedDecoder, RejectsBareCrAfterSize) {
  const auto o = run_single("5\rXhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkCrlf);
}

TEST(ChunkedDecoder, RejectsBareLfAfterData) {
  const auto o = run_single("5\r\nhello\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkCrlf);
}

TEST(ChunkedDecoder, RejectsBareCrAfterData) {
  const auto o = run_single("5\r\nhello\rX0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkCrlf);
}

TEST(ChunkedDecoder, RejectsEmptySize) {
  const auto o = run_single("\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkSize);
}

TEST(ChunkedDecoder, RejectsNonHexSize) {
  const auto o = run_single("g\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkSize);
}

TEST(ChunkedDecoder, RejectsSignInSize) {
  const auto o = run_single("+5\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkSize);
}

TEST(ChunkedDecoder, RejectsSpaceInSize) {
  const auto o = run_single("5 \r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkSize);
}

TEST(ChunkedDecoder, RejectsBadExtensionName) {
  const auto o = run_single("5;@bad\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkExt);
}

TEST(ChunkedDecoder, RejectsSpaceBeforeExtensionValue) {
  const auto o = run_single("5;a= b\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkExt);
}

TEST(ChunkedDecoder, RejectsJunkAfterQuotedExtension) {
  const auto o = run_single("5;a=\"b\"x\r\nhello\r\n0\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkExt);
}

TEST(ChunkedDecoder, RejectsCtlInQuotedExtension) {
  const auto o =
      run_single(std::string("5;a=\"b\x01c\"\r\nhello\r\n0\r\n\r\n"));
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadChunkExt);
}

TEST(ChunkedDecoder, ExtTooLarge) {
  // max_ext_bytes counts from the ';' inclusive.
  const auto o = run_single("5;abcdefghij\r\nhello\r\n0\r\n\r\n", Limits{}, 4);
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.error, Errc::TooLarge);
  EXPECT_EQ(o.reason, ChunkedReason::ExtTooLarge);
}

TEST(ChunkedDecoder, ExtExactlyAtLimitAccepted) {
  // max_ext_bytes counts from the ';' inclusive and includes the terminating
  // CR: ";abc\r" is 5 bytes.
  const auto o = run_single("5;abc\r\nhello\r\n0\r\n\r\n", Limits{}, 5);
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.data, "hello");
}

// ---- trailers --------------------------------------------------------------

TEST(ChunkedDecoder, RejectsForbiddenTrailerNames) {
  const std::vector<std::string> forbidden{"content-length",
                                           "transfer-encoding",
                                           "trailer",
                                           "connection",
                                           "keep-alive",
                                           "proxy-connection",
                                           "te",
                                           "upgrade",
                                           "host",
                                           "expect"};
  for (const auto &name : forbidden) {
    const std::string wire = "0\r\n" + name + ": x\r\n\r\n";
    const auto o = run_single(wire);
    EXPECT_TRUE(o.failed) << "name=" << name;
    EXPECT_EQ(o.reason, ChunkedReason::ForbiddenTrailer) << "name=" << name;
  }
}

TEST(ChunkedDecoder, ForbiddenTrailerNameIsCaseInsensitive) {
  const auto o = run_single("0\r\nContent-Length: 1\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::ForbiddenTrailer);
}

TEST(ChunkedDecoder, RejectsSpaceBeforeTrailerColon) {
  const auto o = run_single("0\r\nX-Foo : bar\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadTrailer);
}

TEST(ChunkedDecoder, RejectsObsFoldInTrailer) {
  const auto o = run_single("0\r\nX-Foo: a\r\n b\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadTrailer);
}

TEST(ChunkedDecoder, RejectsCtlInTrailerValue) {
  const auto o = run_single(std::string("0\r\nX-Foo: a\x01b\r\n\r\n"));
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadTrailer);
}

TEST(ChunkedDecoder, RejectsNonTokenTrailerName) {
  const auto o = run_single("0\r\nX@Foo: bar\r\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadTrailer);
}

TEST(ChunkedDecoder, RejectsBareLfInTrailer) {
  const auto o = run_single("0\r\nX-Foo: bar\n\r\n");
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.reason, ChunkedReason::BadTrailer);
}

TEST(ChunkedDecoder, TrailersTooMany) {
  Limits l;
  l.max_headers = 2;
  const auto o = run_single("0\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", l);
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.error, Errc::TooLarge);
  EXPECT_EQ(o.reason, ChunkedReason::TrailersTooLarge);
}

TEST(ChunkedDecoder, TrailersExactlyAtHeaderLimit) {
  Limits l;
  l.max_headers = 2;
  const auto o = run_single("0\r\nA: 1\r\nB: 2\r\n\r\n", l);
  EXPECT_TRUE(o.done);
  EXPECT_EQ(o.trailers, 2U);
}

TEST(ChunkedDecoder, TrailersTooLarge) {
  Limits l;
  l.max_head_bytes = 8;
  const auto o = run_single("0\r\nX-Foo: bar\r\n\r\n", l);
  EXPECT_TRUE(o.failed);
  EXPECT_EQ(o.error, Errc::TooLarge);
  EXPECT_EQ(o.reason, ChunkedReason::TrailersTooLarge);
}

// ---- EOF / sticky error ----------------------------------------------------

TEST(ChunkedDecoder, OnEofAfterDoneIsOk) {
  ChunkedDecoder d;
  ASSERT_EQ(d.feed(span_of("0\r\n\r\n"), 1u << 20).event, ChunkedEvent::Done);
  EXPECT_EQ(d.on_eof(), Errc::Ok);
}

TEST(ChunkedDecoder, OnEofMidSizeIsViolation) {
  ChunkedDecoder d;
  ASSERT_EQ(d.feed(span_of("5"), 1u << 20).event, ChunkedEvent::NeedMore);
  EXPECT_EQ(d.on_eof(), Errc::ProtocolViolation);
  EXPECT_TRUE(d.failed());
  EXPECT_EQ(d.reason(), ChunkedReason::PrematureEof);
}

TEST(ChunkedDecoder, OnEofMidDataIsViolation) {
  ChunkedDecoder d;
  ASSERT_EQ(d.feed(span_of("5\r\nhel"), 1u << 20).event, ChunkedEvent::Data);
  EXPECT_EQ(d.on_eof(), Errc::ProtocolViolation);
  EXPECT_EQ(d.reason(), ChunkedReason::PrematureEof);
}

TEST(ChunkedDecoder, OnEofMidTrailersIsViolation) {
  ChunkedDecoder d;
  ASSERT_EQ(d.feed(span_of("0\r\nX-Foo: bar\r\n"), 1u << 20).event,
            ChunkedEvent::NeedMore);
  EXPECT_EQ(d.on_eof(), Errc::ProtocolViolation);
  EXPECT_EQ(d.reason(), ChunkedReason::PrematureEof);
}

TEST(ChunkedDecoder, ErrorIsSticky) {
  ChunkedDecoder d;
  const auto first = d.feed(span_of("Z\r\n"), 1u << 20);
  ASSERT_EQ(first.event, ChunkedEvent::Error);
  EXPECT_TRUE(d.failed());
  EXPECT_EQ(d.reason(), ChunkedReason::BadChunkSize);

  // A later feed reports the same error and consumes nothing.
  const auto again = d.feed(span_of("0\r\n\r\n"), 1u << 20);
  EXPECT_EQ(again.event, ChunkedEvent::Error);
  EXPECT_EQ(again.error, Errc::ProtocolViolation);
  EXPECT_EQ(again.reason, ChunkedReason::BadChunkSize);
  EXPECT_EQ(again.consumed, 0U);
}

TEST(ChunkedDecoder, TooLargeErrorIsSticky) {
  ChunkedDecoder d;
  ASSERT_EQ(d.feed(span_of("10000000000000000\r\n"), 1u << 20).event,
            ChunkedEvent::Error);
  const auto again = d.feed(span_of("0\r\n\r\n"), 1u << 20);
  EXPECT_EQ(again.event, ChunkedEvent::Error);
  EXPECT_EQ(again.error, Errc::TooLarge);
}

// ---- BodyStream integration ------------------------------------------------

TEST(ChunkedDecoder, DrivesBodyStream) {
  Fixture f;
  auto s = f.stream(64, 16);

  const std::string wire = "5\r\nhello\r\n6\r\n world\r\n0\r\nX-Foo: "
                           "bar\r\n\r\nGET /next HTTP/1.1\r\n";
  ChunkedDecoder d;
  std::size_t off = 0;
  bool finished = false;
  while (!finished) {
    const auto step = d.feed(span_of(wire).subspan(off), 1u << 20);
    off += step.consumed;
    switch (step.event) {
    case ChunkedEvent::Data: {
      const auto c = s->claim(static_cast<uint32_t>(step.data.size()));
      ASSERT_EQ(c.status, ClaimStatus::Granted);
      ASSERT_EQ(s->push(std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(step.data.data()),
                    step.data.size())),
                WriteStatus::Ok);
      break;
    }
    case ChunkedEvent::Done:
      ASSERT_EQ(s->finish(d.trailers_seen() > 0), WriteStatus::Ok);
      finished = true;
      break;
    case ChunkedEvent::Error:
      FAIL() << "unexpected error";
      break;
    default:
      break;
    }
  }

  // The pipelined tail was left unconsumed.
  EXPECT_EQ(wire.substr(off), "GET /next HTTP/1.1\r\n");

  std::string got;
  while (!s->front().empty()) {
    const auto fr = s->front();
    got += as_string(fr);
    s->consume(static_cast<uint32_t>(fr.size()));
  }
  EXPECT_EQ(got, "hello world");
  EXPECT_TRUE(s->end_seen());
  EXPECT_TRUE(s->trailers_available());
}

TEST(ChunkedDecoder, BodyStreamPoolLeakCheck) {
  Fixture f;
  {
    auto s = f.stream(64, 16);
    const std::string wire = "5\r\nhello\r\n0\r\n\r\n";
    ChunkedDecoder d;
    std::size_t off = 0;
    for (;;) {
      const auto step = d.feed(span_of(wire).subspan(off), 1u << 20);
      off += step.consumed;
      if (step.event == ChunkedEvent::Data) {
        const auto c = s->claim(static_cast<uint32_t>(step.data.size()));
        ASSERT_EQ(c.status, ClaimStatus::Granted);
        ASSERT_EQ(s->push(std::span<const std::byte>(
                      reinterpret_cast<const std::byte *>(step.data.data()),
                      step.data.size())),
                  WriteStatus::Ok);
      } else if (step.event == ChunkedEvent::Done) {
        ASSERT_EQ(s->finish(), WriteStatus::Ok);
        break;
      }
    }
    // Drain so the chunks are returned before the stream dies.
    while (!s->front().empty()) {
      s->consume(static_cast<uint32_t>(s->front().size()));
    }
  }
  // Fixture's destructor asserts free_count() == capacity().
}
