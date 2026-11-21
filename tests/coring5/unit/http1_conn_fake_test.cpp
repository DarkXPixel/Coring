// M4.1 S2: seeded FakeRing lifecycle test for Http1Conn.
//
// Drives a real Http1Conn through io/RingSeam.hpp (CORING5_FAKE_RING) with a
// seeded PRNG: random request bytes (valid and malformed), short sends, timer
// fires, exchange drains and a close trigger chosen from every state. After the
// close it completes the outstanding ops and cancels, reaps the slot and
// asserts the INVARIANTS.md end state: live()==0, inflight==0, stale_events==0,
// the stage and body chunk pools are full again and no fd leaked.
//
// A second group exercises the ConnPool dispatch itself: a stale handle (wrong
// generation) must be reported, never delivered (I4).

#include "coring5/io/ConnPool.hpp"
#include "http1_conn_harness.hpp"

#include <random>

using namespace Coring5;
using namespace Coring5::Test;

namespace {

// Complete every outstanding send with `n` bytes (short sends allowed).
void complete_sends(ConnHarness &h, std::mt19937 &rng, bool short_ok) {
  // Snapshot: deliver_op() erases from outstanding().
  std::vector<ConnHarness::Outstanding> ops = h.outstanding();
  for (const auto &p : ops) {
    if (p.kind != OpKind::Send || h.closed()) {
      continue;
    }
    uint32_t n = p.len;
    if (short_ok && p.len > 1 && (rng() & 1U)) {
      n = 1 + (rng() % p.len);
    }
    h.deliver_op(p, static_cast<int32_t>(n));
  }
}

// One random step that does not close the connection.
void random_step(ConnHarness &h, std::mt19937 &rng) {
  switch (rng() % 8) {
  case 0:
    h.deliver_recv(req("GET", "/"));
    break;
  case 1:
    h.deliver_recv(req("POST", "/", "Content-Length: 3\r\n"));
    h.deliver_recv("abc");
    break;
  case 2:
    h.deliver_recv("garbage not http\r\n\r\n");
    break;
  case 3:
    h.deliver_recv("G"); // partial head: drives sync without closing
    break;
  case 4:
    complete_sends(h, rng, /*short_ok=*/true);
    break;
  case 5:
    h.fire_timer();
    break;
  case 6:
    // Pipelined pair: two requests in one recv.
    h.deliver_recv(req("GET", "/a") + req("GET", "/b"));
    break;
  case 7:
    // A chunked request head + a chunk, exercising the body path.
    h.deliver_recv(req("POST", "/", "Transfer-Encoding: chunked\r\n"));
    h.deliver_recv("3\r\nabc\r\n0\r\n\r\n");
    break;
  }
  h.drain_exchanges();
}

} // namespace

TEST(Http1ConnFake, SeededLifecycleClosesCleanlyAtEveryState) {
  for (uint32_t seed = 1; seed <= 1000; ++seed) {
    std::mt19937 rng(seed);
    ConnHarness h;
    const int steps = static_cast<int>(rng() % 12);
    for (int i = 0; i < steps && !h.closing(); ++i) {
      random_step(h, rng);
    }

    if (!h.closing()) {
      // Choose a close trigger from every state.
      switch (rng() % 6) {
      case 0:
        h.on_drain();
        break;
      case 1:
        h.deliver_recv_eof();
        break;
      case 2:
        h.deliver_recv_error(-ECONNRESET);
        break;
      case 3:
        h.c().core.close(h.ctx(), Errc::Shutdown);
        break;
      case 4:
        h.fire_timer(); // may or may not close depending on state
        break;
      case 5:
        h.deliver_recv_error(-ENOBUFS); // shed path
        break;
      }
    }
    if (!h.closing()) {
      h.c().core.close(h.ctx(), Errc::Shutdown);
    }

    // Flush whatever the close produced, then complete the cancels.
    complete_sends(h, rng, /*short_ok=*/true);
    h.flush_pending();
    h.reap_closed();
    EXPECT_TRUE(h.closed()) << "seed " << seed;
    h.expect_clean_end();
  }
}

TEST(Http1ConnFake, LateCqeAfterCloseIsNotDeliveredToTheProtocol) {
  ConnHarness h;
  // Start a request so a send is in flight, then close.
  h.deliver_recv(req("GET", "/"));
  h.drain_exchanges();
  ASSERT_FALSE(h.closing());
  h.c().core.close(h.ctx(), Errc::Shutdown);
  ASSERT_TRUE(h.closing());
  // A send CQE that arrives after close must be accounted (I2) but must NOT run
  // on_cqe (I3): begin_cqe returns false while Closing.
  const Cqe late{h.c().core.tag(OpKind::Send), 5, 0};
  EXPECT_FALSE(h.c().core.begin_cqe(late));
  h.c().core.end_cqe(h.ctx());
  h.flush_pending();
  h.reap_closed();
  EXPECT_TRUE(h.closed());
  h.expect_clean_end();
}

TEST(Http1ConnFake, StaleHandleIsRejectedByThePool) {
  // Exercise the ConnPool dispatch (I4): a CQE whose generation does not match
  // the live slot must be reported as stale, never delivered.
  FakeRing ring;
  FakeBufferRing buffers(16, kBuffer);
  WorkerMetrics metrics{};
  Limits limits = ConnHarness::base_limits();
  TimerWheel timers(Clock::now(), Nanos{1'000'000}, 64);
  ChunkPool stage;
  ChunkPool chunks;
  {
    auto sp = ChunkPool::create(kStageChunk, 4);
    ASSERT_TRUE(sp);
    stage = std::move(*sp);
    auto cp = ChunkPool::create(kChunk, 64);
    ASSERT_TRUE(cp);
    chunks = std::move(*cp);
  }
  ExchangePool exchanges(4, chunks, StreamLimits{kHigh, kLow});
  IoContext ctx{ring, buffers, timers, metrics, &limits, Clock::now(), {}};
  ctx.exchanges = &exchanges;
  ctx.stage = &stage;

  ConnPool<Http1Conn> pool(4);
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  FdOwner fd{fds[0]};
  Http1Conn *c = pool.spawn(ctx, fd);
  ASSERT_NE(c, nullptr);
  const Handle live = c->core.handle();

  // A CQE for a different generation of the same slot: stale.
  const OpTag stale = OpTag::make(OpKind::Recv, live.index, live.gen + 1,
                                  kAuxConnection, PoolId::Http1);
  IoBufferRing::Lease lease;
  EXPECT_FALSE(pool.on_cqe(ctx, Cqe{stale, 0, 0}, lease));

  // The real handle still works (an EOF recv closes the connection).
  EXPECT_TRUE(pool.on_cqe(ctx, Cqe{c->core.tag(OpKind::Recv), 0, 0}, lease));
  EXPECT_TRUE(c->core.closing());

  // Finish the close and reap.
  for (const auto &p : ring.pending) {
    if (p.kind == OpKind::Cancel) {
      (void)pool.on_cqe(ctx, Cqe{p.tag, -ECANCELED, 0}, lease);
    }
  }
  for (const ConnRef r : ctx.closed) {
    pool.destroy(r.handle);
  }
  ctx.closed.clear();
  EXPECT_EQ(pool.live(), 0U);
  EXPECT_EQ(get(metrics.stale_events), 0U);
  EXPECT_EQ(chunks.free_count(), chunks.capacity());
  EXPECT_EQ(stage.free_count(), stage.capacity());
  ::close(fds[1]);
}
