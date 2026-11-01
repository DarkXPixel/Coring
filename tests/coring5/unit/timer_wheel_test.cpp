#include "coring5/io/TimerWheel.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace Coring5;
using namespace std::chrono_literals;

namespace {
struct Fixture : ::testing::Test {
  TimePoint t0{Clock::time_point{} + 1000s};
  TimerWheel wheel{t0, 10ms, 64};
  std::vector<int> fired;

  void advance(Nanos to) {
    wheel.advance(t0 + to, [&](TimerNode &n) {
      fired.push_back(static_cast<int>(n.owner.index));
    });
  }
};
} // namespace

TEST_F(Fixture, FiresInDeadlineOrderAndNotEarly) {
  TimerNode a, b, c;
  a.owner = {1, 1};
  b.owner = {2, 1};
  c.owner = {3, 1};
  wheel.schedule(b, t0 + 50ms);
  wheel.schedule(a, t0 + 20ms);
  wheel.schedule(c, t0 + 80ms);

  advance(10ms);
  EXPECT_TRUE(fired.empty());
  advance(20ms);
  EXPECT_EQ(fired, (std::vector<int>{1}));
  advance(60ms);
  EXPECT_EQ(fired, (std::vector<int>{1, 2}));
  advance(100ms);
  EXPECT_EQ(fired, (std::vector<int>{1, 2, 3}));
}

TEST_F(Fixture, CancelPreventsFiring) {
  TimerNode a;
  a.owner = {1, 1};
  wheel.schedule(a, t0 + 30ms);
  EXPECT_TRUE(a.scheduled());
  a.cancel();
  EXPECT_FALSE(a.scheduled());
  advance(1s);
  EXPECT_TRUE(fired.empty());
}

TEST_F(Fixture, DestructorUnlinksNode) {
  {
    TimerNode a;
    a.owner = {1, 1};
    wheel.schedule(a, t0 + 30ms);
  } // owner destroyed while still scheduled: must not leave a dangling entry
  advance(1s);
  EXPECT_TRUE(fired.empty());
}

TEST_F(Fixture, RescheduleMovesTheDeadline) {
  TimerNode a;
  a.owner = {1, 1};
  wheel.schedule(a, t0 + 30ms);
  wheel.schedule(a, t0 + 200ms);
  advance(100ms);
  EXPECT_TRUE(fired.empty());
  advance(250ms);
  EXPECT_EQ(fired, (std::vector<int>{1}));
}

TEST_F(Fixture, DeadlineBeyondOneLapWaitsForItsLap) {
  TimerNode a;
  a.owner = {1, 1};
  wheel.schedule(a, t0 + 10ms * 64 * 2 + 30ms); // > 2 laps of the 64-slot wheel
  advance(10ms * 64);
  EXPECT_TRUE(fired.empty());
  advance(10ms * 64 + 100ms);
  EXPECT_TRUE(fired.empty());
  advance(10ms * 64 * 2 + 100ms);
  EXPECT_EQ(fired, (std::vector<int>{1}));
}

TEST_F(Fixture, LongStallVisitsEverySlotOnce) {
  TimerNode a, b;
  a.owner = {1, 1};
  b.owner = {2, 1};
  wheel.schedule(a, t0 + 30ms);
  wheel.schedule(b, t0 + 400ms);
  advance(std::chrono::hours(5)); // loop stalled for hours
  EXPECT_EQ(fired.size(), 2U);
}

TEST_F(Fixture, CallbackMayRescheduleAndCancelOthers) {
  TimerNode a, b;
  a.owner = {1, 1};
  b.owner = {2, 1};
  wheel.schedule(a, t0 + 20ms);
  wheel.schedule(b, t0 + 20ms);
  wheel.advance(t0 + 40ms, [&](TimerNode &n) {
    fired.push_back(static_cast<int>(n.owner.index));
    if (n.owner.index == 1) {
      b.cancel();                    // cancel a peer due in the same slot
      wheel.schedule(a, t0 + 500ms); // re-arm itself
    }
  });
  EXPECT_EQ(fired, (std::vector<int>{1}));
  EXPECT_TRUE(a.scheduled());
  EXPECT_FALSE(b.scheduled());
}

TEST_F(Fixture, PastDeadlineFiresOnNextTickNotImmediatelyInsideSchedule) {
  TimerNode a;
  a.owner = {1, 1};
  wheel.schedule(a, t0 - 5s);
  EXPECT_TRUE(fired.empty());
  advance(20ms);
  EXPECT_EQ(fired, (std::vector<int>{1}));
}
