#include <gtest/gtest.h>

#include <chrono>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <string>

namespace manyforge::core {

TEST(ManualSchedulerTest, ScheduleAndFire) {
  ManualScheduler sched;
  int fired = 0;
  sched.schedule(100, [&] { ++fired; });
  EXPECT_EQ(fired, 0);
  sched.advance_to(100);
  EXPECT_EQ(fired, 1);
}

TEST(ManualSchedulerTest, DoesNotFireBeforeDeadline) {
  ManualScheduler sched;
  int fired = 0;
  sched.schedule(200, [&] { ++fired; });
  sched.advance_to(199);
  EXPECT_EQ(fired, 0);
  sched.advance_to(200);
  EXPECT_EQ(fired, 1);
}

TEST(ManualSchedulerTest, FiresMultipleInDeadlineOrder) {
  ManualScheduler sched;
  std::vector<int> order;
  sched.schedule(300, [&] { order.push_back(3); });
  sched.schedule(100, [&] { order.push_back(1); });
  sched.schedule(200, [&] { order.push_back(2); });
  sched.advance_to(300);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(ManualSchedulerTest, MultipleSameDeadlineBothFire) {
  ManualScheduler sched;
  int count = 0;
  sched.schedule(100, [&] { ++count; });
  sched.schedule(100, [&] { ++count; });
  sched.advance_to(100);
  EXPECT_EQ(count, 2);
}

TEST(ManualSchedulerTest, CancelPreventsFireing) {
  ManualScheduler sched;
  int fired = 0;
  auto h = sched.schedule(100, [&] { ++fired; });
  sched.cancel(h);
  sched.advance_to(100);
  EXPECT_EQ(fired, 0);
}

TEST(ManualSchedulerTest, CancelInvalidHandleIsNoOp) {
  ManualScheduler sched;
  sched.cancel(999);  // no crash
}

TEST(ManualSchedulerTest, DoesNotRefireFiredCallbacks) {
  ManualScheduler sched;
  int fired = 0;
  sched.schedule(100, [&] { ++fired; });
  sched.advance_to(100);
  EXPECT_EQ(fired, 1);
  sched.advance_to(200);
  EXPECT_EQ(fired, 1);  // not re-fired
}

TEST(ManualSchedulerTest, PartialAdvanceFiresOnlyEligible) {
  ManualScheduler sched;
  int a = 0, b = 0;
  sched.schedule(100, [&] { ++a; });
  sched.schedule(300, [&] { ++b; });
  sched.advance_to(200);
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 0);
  sched.advance_to(300);
  EXPECT_EQ(b, 1);
}

TEST(ManualSchedulerTest, ReturnedHandlesAreUnique) {
  ManualScheduler sched;
  auto h1 = sched.schedule(100, [] {});
  auto h2 = sched.schedule(100, [] {});
  auto h3 = sched.schedule(200, [] {});
  EXPECT_NE(h1, h2);
  EXPECT_NE(h1, h3);
  EXPECT_NE(h2, h3);
}

TEST(ManualSchedulerTest, CancelOneOfTwoSameDeadline) {
  ManualScheduler sched;
  int count = 0;
  sched.schedule(100, [&] { ++count; });
  auto h2 = sched.schedule(100, [&] { ++count; });
  sched.cancel(h2);
  sched.advance_to(100);
  EXPECT_EQ(count, 1);
}

TEST(ManualSchedulerTest, PriorityAdvanceLeavesOtherDueLanesPending) {
  ManualScheduler sched;
  std::vector<SchedulerPriority> fired;
  sched.schedule(
      100, [&] { fired.push_back(SchedulerPriority::Monitoring); }, SchedulerPriority::Monitoring);
  sched.schedule(
      100, [&] { fired.push_back(SchedulerPriority::Background); }, SchedulerPriority::Background);

  sched.advance_to(100, SchedulerPriority::Background);
  ASSERT_EQ(fired.size(), 1U);
  EXPECT_EQ(fired.front(), SchedulerPriority::Background);

  sched.advance_to(100, SchedulerPriority::Monitoring);
  ASSERT_EQ(fired.size(), 2U);
  EXPECT_EQ(fired.back(), SchedulerPriority::Monitoring);
}

TEST(StuckDrainWaitTest, DrainWithinBoundWritesNothing) {
  int calls = 0;
  int stuck_hooks = 0;
  const char* before = FirstStuckDrainName();
  testing::internal::CaptureStderr();
  WaitForDrain(
      "test.drain", std::chrono::milliseconds(5),
      [&](std::chrono::nanoseconds bound) {
        EXPECT_EQ(bound, std::chrono::milliseconds(5));
        ++calls;
        return true;
      },
      [&] { ++stuck_hooks; });
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(stuck_hooks, 0);
  EXPECT_EQ(FirstStuckDrainName(), before);
}

TEST(StuckDrainWaitTest, StuckDrainIsReportedOnceAndWaitedForInsteadOfTerminating) {
  int calls = 0;
  int stuck_hooks = 0;
  const char* before = FirstStuckDrainName();
  testing::internal::CaptureStderr();
  WaitForDrain(
      "test.drain", std::chrono::milliseconds(5),
      [&](std::chrono::nanoseconds) { return ++calls == 4; },
      [&] {
        ++stuck_hooks;
        ASSERT_NE(FirstStuckDrainName(), nullptr);
        throw 1;  // A failing evidence hook never stops the wait.
      });
  const auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(stuck_hooks, 1);
  const char* first = FirstStuckDrainName();
  ASSERT_NE(first, nullptr);
  if (before == nullptr) {
    EXPECT_STREQ(first, "test.drain");
  } else {
    EXPECT_EQ(first, before);
  }
  EXPECT_EQ(output,
            "manyforge_fault code=drain_stuck drain=test.drain bound_ms=5 "
            "state=waiting_for_operator\n"
            "manyforge_fault code=drain_stuck drain=test.drain state=drained_late\n");

  int later_calls = 0;
  testing::internal::CaptureStderr();
  WaitForDrain("test.later", std::chrono::milliseconds(5),
               [&](std::chrono::nanoseconds) { return ++later_calls == 2; });
  static_cast<void>(testing::internal::GetCapturedStderr());
  EXPECT_EQ(later_calls, 2);
  EXPECT_EQ(FirstStuckDrainName(), first);
}

}  // namespace manyforge::core
