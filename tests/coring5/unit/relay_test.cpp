#include "coring5/mem/ChunkPool.hpp"
#include "coring5/mem/ChunkQueue.hpp"
#include "coring5/upstream/UpstreamPool.hpp"
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

using namespace Coring5;

namespace {

std::vector<std::byte> bytes_of(const std::string &s) {
  std::vector<std::byte> v(s.size());
  std::memcpy(v.data(), s.data(), s.size());
  return v;
}

// Drains `q` completely through front()/consume() in `step`-sized bites.
std::string drain(ChunkQueue &q, ChunkPool &p, uint32_t step) {
  std::string out;
  while (!q.empty()) {
    const auto f = q.front(p);
    EXPECT_FALSE(f.empty());
    const auto n = std::min<std::size_t>(f.size(), step);
    out.append(reinterpret_cast<const char *>(f.data()), n);
    q.consume(p, static_cast<uint32_t>(n));
  }
  return out;
}

} // namespace

// ------------------------------------------------------------------ ChunkPool

TEST(ChunkPool, AllocatesEveryChunkOnceAndExhausts) {
  auto r = ChunkPool::create(64, 8);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  std::vector<uint32_t> got;
  for (int i = 0; i < 8; ++i) {
    const uint32_t c = p.alloc();
    ASSERT_NE(c, ChunkPool::kNil);
    got.push_back(c);
  }
  EXPECT_EQ(p.alloc(), ChunkPool::kNil);
  std::sort(got.begin(), got.end());
  for (uint32_t i = 0; i < 8; ++i) {
    EXPECT_EQ(got[i], i);
  }
  for (auto c : got) {
    p.free(c);
  }
  EXPECT_EQ(p.free_count(), 8U);
}

TEST(ChunkPool, RejectsDegenerateSizes) {
  EXPECT_FALSE(ChunkPool::create(0, 4));
  EXPECT_FALSE(ChunkPool::create(64, 0));
}

TEST(ChunkPool, EmptyPoolNeverAllocates) {
  ChunkPool p;
  EXPECT_EQ(p.alloc(), ChunkPool::kNil);
  EXPECT_EQ(p.capacity(), 0U);
}

// ----------------------------------------------------------------- ChunkQueue

TEST(ChunkQueue, PushOnZeroCapacityPoolIsRefusedWithoutCrashing) {
  ChunkPool p; // chunk_size 0: would divide by zero without the guard
  ChunkQueue q;
  const auto d = bytes_of("x");
  EXPECT_FALSE(q.push(p, d));
  EXPECT_TRUE(q.empty());
}

TEST(ChunkQueue, PreservesOrderAcrossChunkBoundaries) {
  auto r = ChunkPool::create(16, 64);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  ChunkQueue q;
  std::string want;
  for (int i = 0; i < 40; ++i) {
    std::string piece(static_cast<std::size_t>(i % 7 + 1),
                      static_cast<char>('a' + i % 26));
    want += piece;
    ASSERT_TRUE(q.push(p, bytes_of(piece)));
  }
  EXPECT_EQ(q.bytes(), want.size());
  EXPECT_EQ(drain(q, p, 5), want);
  EXPECT_EQ(p.free_count(), p.capacity()) << "chunks leaked";
}

TEST(ChunkQueue, PushIsAllOrNothingWhenThePoolIsShort) {
  auto r = ChunkPool::create(16, 2);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  ChunkQueue q;
  ASSERT_TRUE(q.push(p, bytes_of(std::string(20, 'a')))); // 2 chunks
  const auto before = q.bytes();
  EXPECT_FALSE(q.push(p, bytes_of(std::string(13, 'b')))); // needs a 3rd
  EXPECT_EQ(q.bytes(), before);
  EXPECT_TRUE(q.push(p, bytes_of(std::string(12, 'c')))); // fits the tail room
  EXPECT_EQ(drain(q, p, 100), std::string(20, 'a') + std::string(12, 'c'));
  EXPECT_EQ(p.free_count(), 2U);
}

TEST(ChunkQueue, FrontStaysValidWhilePushingBehindIt) {
  // An in-flight send reads straight from front(): appending must not move it.
  auto r = ChunkPool::create(16, 8);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  ChunkQueue q;
  ASSERT_TRUE(q.push(p, bytes_of("0123456789")));
  const auto f = q.front(p);
  const std::byte *addr = f.data();
  ASSERT_TRUE(q.push(p, bytes_of(std::string(40, 'z'))));
  EXPECT_EQ(q.front(p).data(), addr);
  EXPECT_EQ(std::memcmp(addr, "0123456789", 10), 0);
  q.clear(p); // a ChunkQueue must be empty when destroyed
  EXPECT_EQ(p.free_count(), p.capacity());
}

TEST(ChunkQueue, ClearReturnsEverything) {
  auto r = ChunkPool::create(16, 8);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  ChunkQueue q;
  ASSERT_TRUE(q.push(p, bytes_of(std::string(100, 'q'))));
  q.consume(p, 30);
  q.clear(p);
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(p.free_count(), 8U);
}

TEST(ChunkQueue, RandomizedAgainstReferenceModel) {
  auto r = ChunkPool::create(32, 128);
  ASSERT_TRUE(r);
  ChunkPool &p = *r;
  ChunkQueue q;
  std::string model;
  std::mt19937 rng(12345);
  for (int step = 0; step < 20000; ++step) {
    if (rng() % 2 == 0) {
      std::string piece(rng() % 100 + 1, static_cast<char>('A' + rng() % 26));
      if (q.push(p, bytes_of(piece))) {
        model += piece;
      }
    } else if (!q.empty()) {
      const auto f = q.front(p);
      const uint32_t n = static_cast<uint32_t>(rng() % f.size() + 1);
      ASSERT_EQ(std::memcmp(f.data(), model.data(), n), 0);
      model.erase(0, n);
      q.consume(p, n);
    }
    ASSERT_EQ(q.bytes(), model.size());
    ASSERT_EQ(q.empty(), model.empty());
  }
  EXPECT_EQ(drain(q, p, 7), model);
  EXPECT_EQ(p.free_count(), p.capacity());
}

// ---------------------------------------------------------------- UpstreamPool

namespace {

Endpoint ep(const char *text) {
  auto e = Endpoint::parse(text);
  EXPECT_TRUE(e);
  return *e;
}

ConfigSnapshot snapshot_with(UpstreamSpec spec) {
  ConfigSnapshot s;
  s.upstreams.push_back(std::move(spec));
  return s;
}

UpstreamSpec spec3(LbPolicy policy = LbPolicy::RoundRobin) {
  UpstreamSpec u;
  u.id = UpstreamId{1};
  u.name = "u";
  u.backends = {ep("10.0.0.1:80"), ep("10.0.0.2:80"), ep("10.0.0.3:80")};
  u.policy = policy;
  u.max_fails = 2;
  u.eject_time = Millis{1000};
  return u;
}

const TimePoint t0{};

} // namespace

TEST(UpstreamPool, RoundRobinCyclesAndSkipsExcluded) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  std::vector<uint32_t> seq;
  for (int i = 0; i < 6; ++i) {
    auto r = pool.pick(UpstreamId{1}, t0);
    ASSERT_TRUE(r);
    seq.push_back(r->backend);
  }
  EXPECT_EQ(seq, (std::vector<uint32_t>{0, 1, 2, 0, 1, 2}));

  const uint32_t ex[] = {0, 1};
  auto r = pool.pick(UpstreamId{1}, t0, ex);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->backend, 2U);
  const uint32_t all[] = {0, 1, 2};
  EXPECT_FALSE(pool.pick(UpstreamId{1}, t0, all));
}

TEST(UpstreamPool, UnknownUpstreamFails) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto r = pool.pick(UpstreamId{9}, t0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, Errc::UpstreamUnavailable);
}

TEST(UpstreamPool, LeastConnectionsPrefersTheIdleBackend) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3(LbPolicy::LeastConnections)));
  auto a = pool.pick(UpstreamId{1}, t0);
  pool.acquire(*a);
  auto b = pool.pick(UpstreamId{1}, t0);
  pool.acquire(*b);
  auto c = pool.pick(UpstreamId{1}, t0);
  pool.acquire(*c);
  EXPECT_NE(a->backend, b->backend);
  EXPECT_NE(b->backend, c->backend);
  EXPECT_NE(a->backend, c->backend);
  pool.release(*b);
  auto d = pool.pick(UpstreamId{1}, t0);
  EXPECT_EQ(d->backend, b->backend);
  EXPECT_EQ(pool.active(*a), 1U);
  pool.release(*a);
  pool.release(*a); // unbalanced release never underflows
  EXPECT_EQ(pool.active(*a), 0U);
}

TEST(UpstreamPool, EjectsAfterMaxFailsAndReadmitsAfterTheEjectTime) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto first = pool.pick(UpstreamId{1}, t0);
  ASSERT_TRUE(first);
  EXPECT_FALSE(pool.report(*first, false, t0));
  EXPECT_TRUE(pool.report(*first, false, t0)) << "2nd failure ejects";
  EXPECT_TRUE(pool.ejected(*first, t0));

  for (int i = 0; i < 10; ++i) {
    auto r = pool.pick(UpstreamId{1}, t0);
    ASSERT_TRUE(r);
    EXPECT_NE(r->backend, first->backend);
    EXPECT_FALSE(r->panic);
  }
  const TimePoint later = t0 + Millis{1001};
  EXPECT_FALSE(pool.ejected(*first, later));
  // Half-open: the failure counter was kept, so ONE failure re-ejects at once.
  EXPECT_TRUE(pool.report(*first, false, later));
  // A success clears everything.
  EXPECT_FALSE(pool.report(*first, true, later + Millis{5000}));
  EXPECT_FALSE(pool.report(*first, false, later + Millis{5000}));
}

TEST(UpstreamPool, PanicModeStillPicksWhenEverythingIsEjected) {
  UpstreamPool pool;
  auto s = spec3();
  s.max_fails = 1;
  pool.apply_config(snapshot_with(s));
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t ex[] = {};
    (void)ex;
    BackendRef ref{UpstreamId{1}, i, pool.pick(UpstreamId{1}, t0)->epoch,
                   false};
    pool.report(ref, false, t0);
  }
  auto r = pool.pick(UpstreamId{1}, t0);
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->panic);
  // Panic mode still honours the connection's own exclude list.
  const uint32_t all[] = {0, 1, 2};
  EXPECT_FALSE(pool.pick(UpstreamId{1}, t0, all));
}

TEST(UpstreamPool, MaxFailsZeroDisablesEjection) {
  UpstreamPool pool;
  auto s = spec3();
  s.max_fails = 0;
  pool.apply_config(snapshot_with(s));
  auto r = pool.pick(UpstreamId{1}, t0);
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(pool.report(*r, false, t0));
  }
  EXPECT_FALSE(pool.ejected(*r, t0));
}

TEST(UpstreamPool, StaleRefsAfterAChangedReloadAreHarmless) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto old_ref = pool.pick(UpstreamId{1}, t0);
  pool.acquire(*old_ref);

  auto changed = spec3();
  changed.backends = {ep("10.9.9.9:80")}; // different spec => fresh epoch
  pool.apply_config(snapshot_with(changed));

  EXPECT_EQ(pool.endpoint(*old_ref), nullptr);
  pool.release(*old_ref); // must not touch the new entry
  EXPECT_FALSE(pool.report(*old_ref, false, t0));
  auto fresh = pool.pick(UpstreamId{1}, t0);
  ASSERT_TRUE(fresh);
  EXPECT_NE(fresh->epoch, old_ref->epoch);
  EXPECT_EQ(pool.active(*fresh), 0U);
  EXPECT_NE(pool.endpoint(*fresh), nullptr);
}

TEST(UpstreamPool, UnchangedReloadKeepsStateAndRefs) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto ref = pool.pick(UpstreamId{1}, t0);
  pool.acquire(*ref);
  pool.apply_config(snapshot_with(spec3())); // identical spec
  EXPECT_EQ(pool.active(*ref), 1U);
  EXPECT_NE(pool.endpoint(*ref), nullptr);
  pool.release(*ref);
  EXPECT_EQ(pool.active(*ref), 0U);
}

TEST(UpstreamPool, RemovedUpstreamDisappears) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto ref = pool.pick(UpstreamId{1}, t0);
  pool.apply_config(ConfigSnapshot{});
  EXPECT_FALSE(pool.pick(UpstreamId{1}, t0));
  EXPECT_EQ(pool.endpoint(*ref), nullptr);
  EXPECT_EQ(pool.backend_count(UpstreamId{1}), 0U);
}

TEST(UpstreamPool, ConnectTimeoutFallsBackToTheGlobalLimit) {
  UpstreamPool pool;
  auto s = spec3();
  pool.apply_config(snapshot_with(s));
  EXPECT_EQ(pool.connect_timeout(UpstreamId{1}, Millis{3000}), Millis{3000});
  s.connect_timeout = Millis{250};
  pool.apply_config(snapshot_with(s));
  EXPECT_EQ(pool.connect_timeout(UpstreamId{1}, Millis{3000}), Millis{250});
}

// ---------------------------------------------------------------- BackendLease

TEST(BackendLease, AcquiresOnConstructionAndReleasesExactlyOnce) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3(LbPolicy::LeastConnections)));
  auto ref = pool.pick(UpstreamId{1}, t0);
  ASSERT_TRUE(ref);
  EXPECT_EQ(pool.active(*ref), 0U);
  {
    BackendLease a(pool, *ref);
    EXPECT_TRUE(a.valid());
    EXPECT_EQ(pool.active(*ref), 1U);
    BackendLease b(std::move(a)); // moving transfers, never double-counts
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(pool.active(*ref), 1U);
    BackendLease c;
    c = std::move(b);
    EXPECT_EQ(pool.active(*ref), 1U);
    c.reset();
    EXPECT_EQ(pool.active(*ref), 0U);
    c.reset(); // idempotent
    EXPECT_EQ(pool.active(*ref), 0U);
  }
  EXPECT_EQ(pool.active(*ref), 0U);
}

TEST(BackendLease, MoveAssignReleasesTheOldTarget) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3(LbPolicy::LeastConnections)));
  auto r1 = pool.pick(UpstreamId{1}, t0);
  auto r2 = pool.pick(UpstreamId{1}, t0);
  ASSERT_NE(r1->backend, r2->backend);
  BackendLease a(pool, *r1);
  BackendLease b(pool, *r2);
  a = std::move(b);
  EXPECT_EQ(pool.active(*r1), 0U) << "overwritten lease must release";
  EXPECT_EQ(pool.active(*r2), 1U);
}

TEST(BackendLease, OutlivingAReloadIsHarmless) {
  UpstreamPool pool;
  pool.apply_config(snapshot_with(spec3()));
  auto ref = pool.pick(UpstreamId{1}, t0);
  BackendLease lease(pool, *ref);
  auto changed = spec3();
  changed.backends = {ep("10.9.9.9:80")};
  pool.apply_config(snapshot_with(changed));
  auto fresh = pool.pick(UpstreamId{1}, t0);
  lease.reset(); // stale epoch: must not decrement the new entry
  EXPECT_EQ(pool.active(*fresh), 0U);
}
