#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/types/timeouts.hpp>

namespace manyforge::core {
namespace {

TEST(StopTimingTest, AuthoredDecimalDurationsUseOneNanosecondRoundingPolicy) {
  const auto duration = authored_stop_duration_ns(1.1);
  ASSERT_TRUE(duration.ok());
  EXPECT_EQ(duration.value(), 1'100'000'000);
  EXPECT_EQ(authored_stop_duration_ns(1.100000001).value(), 1'100'000'001);
  EXPECT_EQ(authored_stop_duration_ns(0.0000000015).value(), 2);
  for (const auto invalid : {0.0, -1.0, 0.0000000001, 2.1, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(authored_stop_duration_ns(invalid).ok());
}

TEST(StopTimingTest, ControlledStopDeadlineIsMainsDecelerationPlusCompletionTolerance) {
  // Q28 (as `main`): one deadline per Stop, computed at Stop time from its
  // profile: deceleration time (or its default) plus 500 ms.
  MotionInterruptStopProfileDTO profile;
  profile.deceleration_time_s = 1.5;
  EXPECT_EQ(kCompleteMoveOnSignalTimeoutNs, 500'000'000);
  EXPECT_EQ(controlled_stop_deadline_ns(profile), 2'000'000'000);
  EXPECT_EQ(controlled_stop_deadline_ns(profile, 250'000'000), 1'750'000'000);
  EXPECT_EQ(controlled_stop_deadline_ns(profile, -1), 1'500'000'000);
  profile.deceleration_time_s.reset();
  EXPECT_EQ(controlled_stop_deadline_ns(profile),
            static_cast<Duration>(kDefaultDecelerationTimeS * 1e9) + 500'000'000);
}

TEST(StopTimingTest, SentStopTrajectoryUsesTheNormalMotionNoReportBound) {
  // Maintainer M6: a normal motion's watchdog, max(10 s, minimum,
  // 1.5 x duration + 2 s), is also a sent Stop trajectory's no-report bound.
  EXPECT_DOUBLE_EQ(trajectory_execution_budget_s(1.0), 10.0);
  EXPECT_DOUBLE_EQ(trajectory_execution_budget_s(1.0, 12.0), 12.0);
  EXPECT_DOUBLE_EQ(trajectory_execution_budget_s(8.0), 14.0);
  EXPECT_EQ(stop_trajectory_no_report_ns(1.1), 10'000'000'000);
  EXPECT_EQ(stop_trajectory_no_report_ns(8.0), 14'000'000'000);
  EXPECT_EQ(stop_trajectory_no_report_ns(-1.0), 10'000'000'000);
  // The Kernel's stage watchdog: the deadline (latest send) + that bound for a
  // trajectory as long as the deadline, never before the provider's bound.
  EXPECT_EQ(controlled_stage_watchdog_ns(1'500'000'000), 11'500'000'000);
  EXPECT_EQ(controlled_stage_watchdog_ns(8'000'000'000), 22'000'000'000);
}

TEST(StopTimingTest, ExplicitMaxStopTimeIsTheRobotsOwnControlledDeadline) {
  // Q51 (as `main`'s per-profile override): `max_stop_time_s`, when set, is
  // the deadline itself, measured from the Stop request; no tolerance is
  // added and nothing outside the profile shortens or extends it.
  MotionInterruptStopProfileDTO profile;
  profile.deceleration_time_s = 1.5;
  profile.max_stop_time_s = 3.0;
  EXPECT_EQ(controlled_stop_deadline_ns(profile), 3'000'000'000);
  EXPECT_EQ(controlled_stop_deadline_ns(profile, 250'000'000), 3'000'000'000);
  profile.max_stop_time_s = 0.25;  // shorter than the deceleration: still the deadline
  EXPECT_EQ(controlled_stop_deadline_ns(profile), 250'000'000);

  EXPECT_TRUE(max_stop_time_valid(std::nullopt));
  EXPECT_TRUE(max_stop_time_valid(0.001));
  for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), 1e10}) {
    EXPECT_FALSE(max_stop_time_valid(invalid)) << invalid;
  }
}

}  // namespace
}  // namespace manyforge::core
