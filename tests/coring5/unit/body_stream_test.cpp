#include "coring5/http/BodyStream.hpp"
#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace Coring5;

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

// Claim, push `n` bytes, assert success.
void put(BodyStream &s, std::size_t n, char fill = 'x') {
  const auto c = s.claim(static_cast<uint32_t>(n));
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  ASSERT_EQ(s.push(bytes(n, fill)), WriteStatus::Ok);
}

// Drain everything currently readable; returns the bytes.
std::string drain(BodyStream &s) {
  std::string out;
  while (!s.front().empty()) {
    const auto f = s.front();
    out += as_string(f);
    s.consume(static_cast<uint32_t>(f.size()));
  }
  return out;
}

} // namespace

// --------------------------------------------------------------------- limits

TEST(BodyStreamLimits, ValidityAndFromConfig) {
  EXPECT_TRUE((StreamLimits{10, 0}.valid()));
  EXPECT_TRUE((StreamLimits{10, 9}.valid()));
  EXPECT_FALSE((StreamLimits{0, 0}.valid()));
  EXPECT_FALSE((StreamLimits{10, 10}.valid()));
  EXPECT_FALSE((StreamLimits{10, 11}.valid()));
  Limits l;
  const auto s = StreamLimits::from(l);
  EXPECT_EQ(s.high, l.queue_high_watermark);
  EXPECT_EQ(s.low, l.queue_low_watermark);
  EXPECT_TRUE(s.valid());
}

// -------------------------------------------------------------------- credit

TEST(BodyStreamCredit, GrantsUpToHighAndConservesTheInvariant) {
  Fixture f;
  auto s = f.stream(/*H=*/64, /*L=*/16);
  EXPECT_EQ(s->credit(), 64U);

  const auto c = s->claim(40);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  EXPECT_EQ(c.granted, 40U);
  EXPECT_EQ(s->claimed(), 40U);
  EXPECT_EQ(s->credit(), 24U);
  EXPECT_EQ(s->queued() + s->claimed(), 40U);

  ASSERT_EQ(s->push(bytes(40)), WriteStatus::Ok);
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_EQ(s->queued(), 40U);
  EXPECT_EQ(s->pushed(), 40U);
}

TEST(BodyStreamCredit, ClaimIsClampedToHighWatermark) {
  // Without the clamp a claim larger than H could never be granted and the
  // producer would be blocked forever (model: "Q > H" configuration).
  Fixture f;
  auto s = f.stream(32, 8);
  const auto c = s->claim(1000);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  EXPECT_EQ(c.granted, 32U);
  EXPECT_EQ(s->queued() + s->claimed(), 32U);
}

TEST(BodyStreamCredit, ClaimWhenCreditIsShortBlocksAndRegistersWant) {
  Fixture f;
  auto s = f.stream(32, 8);
  put(*s, 30);
  const auto c = s->claim(8); // credit is 2
  EXPECT_EQ(c.status, ClaimStatus::Blocked);
  EXPECT_EQ(s->want(), 8U);
  EXPECT_EQ(s->claimed(), 0U) << "a blocked producer holds no claim";
  EXPECT_EQ(s->pending_events(), 0);
}

TEST(BodyStreamCredit, ShortPushReturnsTheUnusedPartWithoutWakingAnyone) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_EQ(s->claim(48).status, ClaimStatus::Granted);
  ASSERT_EQ(s->push(bytes(5)), WriteStatus::Ok); // short read: 5 of 48
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_EQ(s->queued(), 5U);
  EXPECT_EQ(s->credit(), 59U);
  // Release of an unused claim wakes nobody (spec 3.4 rule 6).
  EXPECT_EQ(s->pending_events() & StreamEvent::kWritable, 0);
}

TEST(BodyStreamCredit, ZeroLengthPushSettlesTheClaimButWakesNoReader) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->wait_readable());
  ASSERT_EQ(s->claim(10).status, ClaimStatus::Granted);
  ASSERT_EQ(s->push({}), WriteStatus::Ok);
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_EQ(s->pending_events(), 0) << "no data arrived: nothing to read";
  // The reader is still registered and is woken by real data.
  put(*s, 3);
  EXPECT_EQ(s->take_events(), StreamEvent::kReadable);
}

TEST(BodyStreamCredit, SecondClaimBeforeSettlingIsRejected) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Granted);
#ifdef NDEBUG
  // Release builds fail safe: the stream is aborted instead of drifting.
  EXPECT_EQ(s->claim(8).status, ClaimStatus::Closed);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::Internal);
#else
  EXPECT_DEATH((void)s->claim(8), "");
  // Settle the claim so the fixture's leak check stays meaningful.
  ASSERT_EQ(s->push(bytes(8)), WriteStatus::Ok);
#endif
}

TEST(BodyStreamCredit, PushLargerThanTheClaimIsAContractViolation) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Granted);
#ifdef NDEBUG
  EXPECT_EQ(s->push(bytes(9)), WriteStatus::Closed);
  EXPECT_EQ(s->aborted_with(), Errc::Internal);
  EXPECT_EQ(s->queued() + s->claimed(), 0U);
#else
  EXPECT_DEATH((void)s->push(bytes(9)), "");
  ASSERT_EQ(s->push(bytes(8)), WriteStatus::Ok);
#endif
}

// ---------------------------------------------------------------- wake rule

TEST(BodyStreamWake,
     ConsumeWakesBlockedProducerOnlyAtLowWatermarkWithEnoughCredit) {
  Fixture f;
  auto s = f.stream(/*H=*/32, /*L=*/8);
  put(*s, 32);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);

  s->consume(10); // queued 22: above L, credit 10 >= 8, but not yet <= L
  EXPECT_EQ(s->take_events(), 0) << "queued > L: must not wake";
  s->consume(13); // queued 9: still above L
  EXPECT_EQ(s->take_events(), 0);
  s->consume(1); // queued 8 == L, credit 24 >= 8
  EXPECT_EQ(s->take_events(), StreamEvent::kWritable);
  EXPECT_EQ(s->want(), 0U) << "want is cleared once the wake is raised";
}

TEST(BodyStreamWake, LargeWantResumesLaterThanLowWatermark) {
  // H - L < want: the effective resume level is min(L, H - want). It must wake
  // when the credit suffices (queued <= H - want), not hang waiting for L.
  Fixture f;
  auto s = f.stream(/*H=*/10, /*L=*/9);
  put(*s, 10);
  ASSERT_EQ(s->claim(6).status, ClaimStatus::Blocked);
  s->consume(1); // queued 9 <= L(9), credit 1 < 6
  EXPECT_EQ(s->take_events(), 0);
  s->consume(3); // queued 6, credit 4 < 6
  EXPECT_EQ(s->take_events(), 0);
  s->consume(1); // queued 5, credit 5 < 6
  EXPECT_EQ(s->take_events(), 0);
  s->consume(1); // queued 4, credit 6 >= 6
  EXPECT_EQ(s->take_events(), StreamEvent::kWritable);
}

TEST(BodyStreamWake, WritableIsRaisedExactlyOncePerBlock) {
  Fixture f;
  auto s = f.stream(16, 4);
  put(*s, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
  s->consume(16);
  EXPECT_EQ(s->take_events(), StreamEvent::kWritable);
  // More consumption without a new block must not re-raise it.
  put(*s, 4);
  s->consume(4);
  EXPECT_EQ(s->take_events(), 0);
}

TEST(BodyStreamWake, ProducerWithoutBlockingIsNeverWoken) {
  Fixture f;
  auto s = f.stream(32, 8);
  put(*s, 20);
  s->consume(20);
  EXPECT_EQ(s->take_events(), 0) << "only a Blocked producer is woken";
}

TEST(BodyStreamWake, RetryAfterWakeSucceeds) {
  Fixture f;
  auto s = f.stream(16, 4);
  put(*s, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
  s->consume(16);
  ASSERT_EQ(s->take_events(), StreamEvent::kWritable);
  const auto c = s->claim(8);
  ASSERT_EQ(c.status, ClaimStatus::Granted);
  EXPECT_EQ(c.granted, 8U);
}

// ------------------------------------------------------------- reader events

TEST(BodyStreamReader, WaitReadableRegistersOnlyWhenEmptyAndOpen) {
  Fixture f;
  auto s = f.stream(32, 8);
  EXPECT_TRUE(s->wait_readable());
  put(*s, 4);
  EXPECT_EQ(s->take_events(), StreamEvent::kReadable);
  EXPECT_FALSE(s->wait_readable()) << "data is available now";
  s->consume(4);
  EXPECT_TRUE(s->wait_readable());
}

TEST(BodyStreamReader, UnregisteredReaderGetsNoEvent) {
  Fixture f;
  auto s = f.stream(32, 8);
  put(*s, 4);
  EXPECT_EQ(s->take_events(), 0);
}

TEST(BodyStreamReader, FinishWakesAWaitingReader) {
  Fixture f;
  auto s = f.stream(32, 8);
  ASSERT_TRUE(s->wait_readable());
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_EQ(s->take_events(), StreamEvent::kReadable);
  EXPECT_TRUE(s->end_seen());
}

// ----------------------------------------------------------------------- EOF

TEST(BodyStreamEof, FinishedStreamIsDrainedBeforeTheEndIsSeen) {
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 10, 'a');
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_EQ(s->state(), StreamState::Finished);
  EXPECT_FALSE(s->end_seen()) << "data is still queued";
  EXPECT_EQ(drain(*s), std::string(10, 'a'));
  EXPECT_TRUE(s->end_seen());
  EXPECT_EQ(s->aborted_with(), Errc::Ok);
  EXPECT_FALSE(s->wait_readable()) << "end is available: do not wait";
}

TEST(BodyStreamEof, FinishReturnsAnOutstandingClaim) {
  // Mutation "finish keeps credit": the read that was granted credit returns
  // EOF, so the credit must come back.
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_EQ(s->claim(48).status, ClaimStatus::Granted);
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_EQ(s->credit(), 64U);
}

TEST(BodyStreamEof, FinishWhileBlockedIsAContractViolation) {
  Fixture f;
  auto s = f.stream(16, 4);
  put(*s, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
#ifdef NDEBUG
  EXPECT_EQ(s->finish(), WriteStatus::Closed);
  EXPECT_EQ(s->aborted_with(), Errc::Internal);
#else
  EXPECT_DEATH((void)s->finish(), "");
#endif
}

TEST(BodyStreamEof, OperationsAfterFinishAreRefusedWithoutDamage) {
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 6, 'q');
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
#ifndef NDEBUG
  EXPECT_DEATH((void)s->claim(1), "");
  EXPECT_DEATH((void)s->finish(), "");
#endif
  EXPECT_EQ(drain(*s), std::string(6, 'q')) << "data survives a refused call";
}

// --------------------------------------------------------------- trailers

TEST(BodyStreamTrailers, VisibleOnlyAfterTheLastByteWasConsumed) {
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 10);
  ASSERT_EQ(s->finish(/*has_trailers=*/true), WriteStatus::Ok);
  EXPECT_FALSE(s->trailers_available()) << "data still queued";
  s->consume(9);
  EXPECT_FALSE(s->trailers_available());
  s->consume(1);
  EXPECT_TRUE(s->trailers_available());
}

TEST(BodyStreamTrailers, AbsentWhenNotAnnouncedAndDroppedOnAbort) {
  Fixture f;
  {
    auto s = f.stream(64, 16);
    ASSERT_EQ(s->finish(false), WriteStatus::Ok);
    EXPECT_FALSE(s->trailers_available());
  }
  {
    auto s = f.stream(64, 16);
    put(*s, 4);
    ASSERT_EQ(s->finish(true), WriteStatus::Ok);
    s->abort(Errc::Canceled); // consumer cancels before reading the body
    EXPECT_FALSE(s->trailers_available());
  }
}

TEST(BodyStreamTrailers, EmptyBodyWithTrailersIsImmediatelyAvailable) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_EQ(s->finish(true), WriteStatus::Ok);
  EXPECT_TRUE(s->trailers_available());
}

// ---------------------------------------------------------------------- abort

TEST(BodyStreamAbort, DropsQueueAndCreditAndIsTerminal) {
  // Mutation "abort keeps credit".
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 20);
  ASSERT_EQ(s->claim(30).status, ClaimStatus::Granted);
  const uint32_t before = f.pool.free_count();
  s->abort(Errc::Reset);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::Reset);
  EXPECT_EQ(s->queued(), 0U);
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_GT(f.pool.free_count(), before) << "chunks returned at abort";
  EXPECT_TRUE(s->end_seen());
  EXPECT_TRUE(s->front().empty());
}

TEST(BodyStreamAbort, WakesABlockedProducer) {
  // Mutation "abort does not wake" (producer side).
  Fixture f;
  auto s = f.stream(16, 4);
  put(*s, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
  s->abort(Errc::Timeout);
  EXPECT_EQ(s->take_events(), StreamEvent::kWritable);
  EXPECT_EQ(s->want(), 0U);
  EXPECT_EQ(s->claim(8).status, ClaimStatus::Closed) << "retry sees the abort";
}

TEST(BodyStreamAbort, WakesAWaitingConsumer) {
  // Mutation "abort does not wake" (consumer side).
  Fixture f;
  auto s = f.stream(16, 4);
  ASSERT_TRUE(s->wait_readable());
  s->abort(Errc::Reset);
  EXPECT_EQ(s->take_events(), StreamEvent::kReadable);
  EXPECT_TRUE(s->end_seen());
}

TEST(BodyStreamAbort, WakesBothAtOnceWhenBothAreParked) {
  Fixture f;
  auto s = f.stream(16, 4);
  put(*s, 16);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
  // A consumer cannot be "waiting" while data is queued, so drain, re-block.
  s->consume(16);
  (void)s->take_events();
  ASSERT_TRUE(s->wait_readable());
  put(*s, 16); // wakes reader; fill again to block a new claim
  (void)s->take_events();
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Blocked);
  ASSERT_FALSE(s->wait_readable()); // data present: not waiting
  s->abort(Errc::Reset);
  EXPECT_EQ(s->take_events(), StreamEvent::kWritable);
}

TEST(BodyStreamAbort, FirstReasonWinsAndRepeatIsHarmless) {
  Fixture f;
  auto s = f.stream(16, 4);
  s->abort(Errc::Timeout);
  s->abort(Errc::Reset);
  EXPECT_EQ(s->aborted_with(), Errc::Timeout);
  EXPECT_EQ(s->take_events(), 0) << "nobody was registered";
}

TEST(BodyStreamAbort, ProducerOperationsOnAnAbortedStreamReportClosed) {
  Fixture f;
  auto s = f.stream(16, 4);
  s->abort(Errc::Canceled);
  EXPECT_EQ(s->claim(4).status, ClaimStatus::Closed);
  EXPECT_EQ(s->push(bytes(1)), WriteStatus::Closed);
  EXPECT_EQ(s->finish(), WriteStatus::Closed);
  EXPECT_FALSE(s->set_declared(10));
  s->consume(0); // harmless
}

TEST(BodyStreamAbort, AbortAfterTheConsumerSawTheEndIsANoOp) {
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 4);
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  s->consume(4);
  ASSERT_TRUE(s->end_seen());
  s->abort(Errc::Canceled); // e.g. ingress detaching after a complete body
  EXPECT_EQ(s->state(), StreamState::Finished);
  EXPECT_EQ(s->aborted_with(), Errc::Ok);
}

TEST(BodyStreamAbort, ConsumerCancelOfAFinishedStreamWithUnreadDataAborts) {
  // Spec R3: a consumer that detaches before seeing the end cancels the stream.
  Fixture f;
  auto s = f.stream(64, 16);
  put(*s, 8);
  ASSERT_EQ(s->finish(), WriteStatus::Ok);
  s->abort(Errc::Canceled);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->queued(), 0U);
}

// -------------------------------------------------------------- declared
// length

TEST(BodyStreamDeclared, ExactLengthFinishesCleanly) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(12));
  put(*s, 7);
  put(*s, 5);
  EXPECT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_EQ(s->aborted_with(), Errc::Ok);
}

TEST(BodyStreamDeclared, PushingMoreThanDeclaredAborts) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(10));
  put(*s, 6);
  ASSERT_EQ(s->claim(8).status, ClaimStatus::Granted);
  EXPECT_EQ(s->push(bytes(8)), WriteStatus::Violation);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::ProtocolViolation);
  EXPECT_EQ(s->claimed(), 0U);
}

TEST(BodyStreamDeclared, FinishingShortAborts) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(10));
  put(*s, 6);
  EXPECT_EQ(s->finish(), WriteStatus::Violation);
  EXPECT_EQ(s->aborted_with(), Errc::ProtocolViolation);
}

TEST(BodyStreamDeclared, ZeroLengthBodyAndRejectedSetters) {
  Fixture f;
  auto s = f.stream(64, 16);
  ASSERT_TRUE(s->set_declared(0));
  EXPECT_FALSE(s->set_declared(5)) << "only once";
  EXPECT_EQ(s->finish(), WriteStatus::Ok);
  EXPECT_FALSE(s->set_declared(0)) << "not after finish";

  auto t = f.stream(64, 16);
  put(*t, 8);
  EXPECT_FALSE(t->set_declared(4)) << "less than already pushed";
  EXPECT_TRUE(t->set_declared(8));
  t->abort(Errc::Canceled);
}

// ------------------------------------------------------------ pool exhaustion

TEST(BodyStreamPool, ExhaustionAbortsWithOverloadedAndKeepsAccountingClean) {
  Fixture f(/*chunks=*/2); // 32 bytes of chunk memory only
  auto s = f.stream(/*H=*/1024, /*L=*/16);
  put(*s, 32);
  ASSERT_EQ(s->claim(64).status, ClaimStatus::Granted);
  EXPECT_EQ(s->push(bytes(64)), WriteStatus::Exhausted);
  EXPECT_EQ(s->state(), StreamState::Aborted);
  EXPECT_EQ(s->aborted_with(), Errc::Overloaded);
  EXPECT_EQ(s->claimed(), 0U);
  EXPECT_EQ(s->queued(), 0U);
  EXPECT_EQ(f.pool.free_count(), 2U) << "all chunks returned";
}

TEST(BodyStreamPool, ZeroCapacityPoolShedsInsteadOfCrashing) {
  ChunkPool empty; // chunk_size 0, capacity 0
  BodyStream s(empty, StreamLimits{64, 16});
  ASSERT_EQ(s.claim(8).status, ClaimStatus::Granted);
  EXPECT_EQ(s.push(bytes(8)), WriteStatus::Exhausted);
  EXPECT_EQ(s.aborted_with(), Errc::Overloaded);
}

TEST(BodyStreamPool, DestructorReturnsChunksOfAnUnfinishedStream) {
  Fixture f;
  {
    auto s = f.stream(256, 64);
    put(*s, 100);
    EXPECT_LT(f.pool.free_count(), f.pool.capacity());
  }
  EXPECT_EQ(f.pool.free_count(), f.pool.capacity());
}

// ----------------------------------------------------------- borrow semantics

TEST(BodyStreamBorrow, FrontStaysValidWhilePushingBehindIt) {
  // A byte-stream session may send() straight from front() and consume() on
  // the send CQE, while the producer keeps appending.
  Fixture f;
  auto s = f.stream(256, 64);
  ASSERT_EQ(s->claim(10).status, ClaimStatus::Granted);
  ASSERT_EQ(s->push(bytes(10, 'a')), WriteStatus::Ok);
  const auto head = s->front();
  const std::byte *addr = head.data();
  put(*s, 100, 'b');
  EXPECT_EQ(s->front().data(), addr);
  EXPECT_EQ(std::memcmp(addr, std::string(10, 'a').data(), 10), 0);
  EXPECT_EQ(drain(*s), std::string(10, 'a') + std::string(100, 'b'));
}

TEST(BodyStreamBorrow, PartialConsumeKeepsTheRestInOrder) {
  Fixture f;
  auto s = f.stream(256, 64);
  ASSERT_EQ(s->claim(40).status, ClaimStatus::Granted);
  std::string payload;
  for (int i = 0; i < 40; ++i) {
    payload += static_cast<char>('A' + i % 26);
  }
  std::vector<std::byte> b(payload.size());
  std::memcpy(b.data(), payload.data(), payload.size());
  ASSERT_EQ(s->push(b), WriteStatus::Ok);
  s->consume(7);
  EXPECT_EQ(drain(*s), payload.substr(7));
}

// ------------------------------------------------- randomized reference model

// Drives a real BodyStream and a tiny reference model with random
// producer/consumer/abort actions and checks, after EVERY step:
//   queued + claimed <= H, the data read equals the data written (order),
//   a Writable event is raised iff the wake rule says so, and no blocked
//   producer is ever left without a wakeup once the rule's condition holds.
TEST(BodyStreamRandom, AgainstReferenceModel) {
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    std::mt19937 rng(seed);
    Fixture f(512);
    const uint32_t H = 8 + rng() % 56;
    const uint32_t L = rng() % H; // 0 <= L < H
    auto s = f.stream(H, L);

    std::string written;  // everything pushed, in order
    std::string consumed; // everything the consumer took
    uint32_t want = 0;    // model: producer blocked on this much (0 = not)
    bool claimed = false;
    uint32_t claim_size = 0;
    char next = 'a';
    bool done = false;

    auto check_invariants = [&] {
      ASSERT_LE(s->queued() + s->claimed(), H) << "seed " << seed;
      ASSERT_EQ(written.size() - consumed.size(),
                s->state() == StreamState::Aborted
                    ? written.size() - consumed.size()
                    : s->queued())
          << "seed " << seed;
    };

    for (int step = 0; step < 400 && !done; ++step) {
      switch (rng() % 5) {
      case 0: { // producer claims
        if (claimed || want != 0 || s->state() != StreamState::Open) {
          break;
        }
        const uint32_t n = 1 + rng() % (H + 8); // sometimes larger than H
        const auto c = s->claim(n);
        const uint32_t clamped = std::min(n, H);
        if (c.status == ClaimStatus::Granted) {
          ASSERT_EQ(c.granted, clamped);
          ASSERT_GE(H - s->queued() + s->claimed(), clamped) << "seed " << seed;
          claimed = true;
          claim_size = c.granted;
        } else {
          ASSERT_EQ(c.status, ClaimStatus::Blocked);
          ASSERT_LT(s->credit(), clamped) << "blocked although credit sufficed";
          want = clamped;
        }
        break;
      }
      case 1: { // producer pushes (maybe short) or finishes on EOF
        if (!claimed) {
          break;
        }
        if (rng() % 8 == 0) {
          ASSERT_EQ(s->finish(), WriteStatus::Ok);
          claimed = false;
          done = true;
          break;
        }
        const uint32_t k = rng() % (claim_size + 1);
        std::vector<std::byte> b(k);
        for (auto &x : b) {
          x = static_cast<std::byte>(next);
          written += next;
          next = next == 'z' ? 'a' : static_cast<char>(next + 1);
        }
        ASSERT_EQ(s->push(b), WriteStatus::Ok) << "seed " << seed;
        claimed = false;
        break;
      }
      case 2:
      case 3: { // consumer takes some bytes
        const auto front = s->front();
        if (front.empty()) {
          break;
        }
        const auto k = 1 + rng() % front.size();
        consumed += as_string(front.first(k));
        const uint32_t queued_before = s->queued();
        s->consume(static_cast<uint32_t>(k));
        // Model of the wake rule, evaluated on the post-consume state.
        const bool should_wake = want != 0 && s->queued() <= L &&
                                 H - s->queued() - s->claimed() >= want;
        const uint8_t ev = s->take_events();
        if (should_wake) {
          ASSERT_NE(ev & StreamEvent::kWritable, 0)
              << "seed " << seed << " lost wakeup (queued " << queued_before
              << "->" << s->queued() << " want " << want << ")";
          want = 0;
        } else {
          ASSERT_EQ(ev & StreamEvent::kWritable, 0)
              << "seed " << seed << " spurious wake";
        }
        break;
      }
      case 4: { // rare abort
        if (rng() % 25 != 0) {
          break;
        }
        const bool was_blocked = want != 0;
        s->abort(Errc::Reset);
        const uint8_t ev = s->take_events();
        if (was_blocked) {
          ASSERT_NE(ev & StreamEvent::kWritable, 0)
              << "seed " << seed << " abort left a blocked producer parked";
        }
        ASSERT_EQ(s->queued() + s->claimed(), 0U);
        done = true;
        break;
      }
      }
      check_invariants();
      if (testing::Test::HasFatalFailure()) {
        return;
      }
    }

    // Drain what is left and compare the byte stream.
    if (s->state() == StreamState::Open && claimed) {
      ASSERT_EQ(s->finish(), WriteStatus::Ok);
    }
    if (s->state() != StreamState::Aborted) {
      consumed += drain(*s);
      EXPECT_EQ(consumed, written) << "seed " << seed << " data corrupted";
    }
  }
}
