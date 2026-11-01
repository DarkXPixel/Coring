#include "coring5/mem/HandlePool.hpp"
#include <gtest/gtest.h>
#include <set>

using namespace Coring5;

namespace {
struct Base {
  virtual ~Base() = default;
  virtual int id() const { return 0; }
};
struct Derived final : Base {
  explicit Derived(int v) : v_(v) {}
  int id() const override { return v_; }
  int v_;
  char pad[24]{};
};
int g_alive = 0;
struct Counted final : Base {
  Counted() { ++g_alive; }
  ~Counted() override { --g_alive; }
};
} // namespace

// Regression for legacy SessionStorage: resize()+push_back() left `capacity`
// zeros at the bottom of the free list, so index 0 was handed out twice.
TEST(HandlePool, EveryIndexIsHandedOutExactlyOnce) {
  constexpr uint32_t N = 64;
  HandlePool<Base, 64> pool(N);
  std::set<uint32_t> seen;
  for (uint32_t i = 0; i < N; ++i) {
    auto [h, p] = pool.create<Base>();
    ASSERT_NE(p, nullptr);
    ASSERT_TRUE(h.valid());
    EXPECT_TRUE(seen.insert(h.index).second)
        << "index " << h.index << " reused while live";
  }
  EXPECT_EQ(pool.live(), N);
  EXPECT_EQ(seen.size(), N);
}

TEST(HandlePool, FullPoolReturnsInvalidAndNeverAllocates) {
  HandlePool<Base, 64> pool(2);
  (void)pool.create<Base>();
  (void)pool.create<Base>();
  auto [h, p] = pool.create<Base>();
  EXPECT_FALSE(h.valid());
  EXPECT_EQ(p, nullptr);
}

TEST(HandlePool, StaleHandleIsRejectedAfterReuse) {
  HandlePool<Base, 64> pool(1);
  auto [h1, p1] = pool.create<Derived>(7);
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(pool.get(h1)->id(), 7);
  EXPECT_TRUE(pool.destroy(h1));
  EXPECT_EQ(pool.get(h1), nullptr); // stale right after destroy
  EXPECT_FALSE(pool.destroy(h1));   // double destroy is a no-op

  auto [h2, p2] = pool.create<Derived>(9);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(h2.index, h1.index);    // same slot...
  EXPECT_NE(h2.gen, h1.gen);        // ...different generation
  EXPECT_EQ(pool.get(h1), nullptr); // old handle must not alias the new object
  EXPECT_EQ(pool.get(h2)->id(), 9);
}

TEST(HandlePool, InvalidAndOutOfRangeHandles) {
  HandlePool<Base, 64> pool(4);
  EXPECT_EQ(pool.get(Handle{}), nullptr);
  EXPECT_EQ(pool.get(Handle{1000, 1}), nullptr);
  EXPECT_FALSE(pool.destroy(Handle{1000, 1}));
}

TEST(HandlePool, GenerationNeverBecomesZeroOnWrap) {
  HandlePool<Base, 64> pool(1);
  pool.debug_set_generation(0, UINT32_MAX);
  auto [h, p] = pool.create<Base>();
  ASSERT_EQ(h.gen, UINT32_MAX);
  EXPECT_TRUE(pool.destroy(h));
  EXPECT_EQ(pool.get(h), nullptr);
  auto [h2, p2] = pool.create<Base>();
  ASSERT_NE(p2, nullptr);
  EXPECT_NE(h2.gen, 0U) << "0 is the reserved 'invalid' generation";
  EXPECT_TRUE(h2.valid());
  EXPECT_EQ(pool.get(h), nullptr);
}

TEST(HandlePool, DestructorsRunExactlyOnce) {
  g_alive = 0;
  {
    HandlePool<Base, 64> pool(8);
    Handle hs[5];
    for (auto &h : hs) {
      h = pool.create<Counted>().first;
    }
    EXPECT_EQ(g_alive, 5);
    pool.destroy(hs[1]);
    pool.destroy(hs[3]);
    EXPECT_EQ(g_alive, 3);
  } // remaining objects destroyed by the pool destructor
  EXPECT_EQ(g_alive, 0);
}

TEST(HandlePool, ForEachMayDestroyVisitedElement) {
  HandlePool<Base, 64> pool(8);
  for (int i = 0; i < 8; ++i) {
    (void)pool.create<Derived>(i);
  }
  int visited = 0;
  pool.for_each([&](Handle h, Base &) {
    ++visited;
    pool.destroy(h);
  });
  EXPECT_EQ(visited, 8);
  EXPECT_EQ(pool.live(), 0U);
}

TEST(HandlePool, ChurnKeepsAccounting) {
  HandlePool<Base, 64> pool(16);
  std::vector<Handle> live;
  for (int round = 0; round < 2000; ++round) {
    if (live.size() < 16 && (round % 3 != 0)) {
      auto [h, p] = pool.create<Base>();
      ASSERT_NE(p, nullptr);
      live.push_back(h);
    } else if (!live.empty()) {
      EXPECT_TRUE(pool.destroy(live.back()));
      live.pop_back();
    }
    ASSERT_EQ(pool.live(), live.size());
  }
}
