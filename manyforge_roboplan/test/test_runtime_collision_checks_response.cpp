#include "execution_host_responses.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <cstddef>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {

// Runs one monitor through scripted check durations on the scripted clock of
// the monitor's lateness tests, and returns its diagnostics row (OD-217).
CanonicalValue RowAfterChecks(const std::vector<core::Duration> &durations) {
  constexpr core::Duration kPeriodNs = 10'000'000;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::size_t tick = 0;
  core::RuntimeCollisionMonitor monitor{
      clock, scheduler, core::RuntimeCollisionMonitor::Config{kPeriodNs},
      [&]() {
        clock.advance(durations.at(tick++));
        return core::Result<void>::Ok();
      },
      [&]() { return tick < durations.size(); }};
  monitor.start();
  for (int step = 0; step < 200 && tick < durations.size(); ++step) {
    clock.advance(1'000'000);
    scheduler.advance_to(clock.now());
  }
  EXPECT_EQ(tick, durations.size());
  return RuntimeCollisionChecksRowValue("lite6_robot", monitor.timing_metrics(),
                                        kPeriodNs);
}

double Number(const CanonicalValue &row, const char *name) {
  const auto *member = row.find_object_member(name);
  EXPECT_NE(member, nullptr) << name;
  return member == nullptr ? -1.0 : member->as_number();
}

TEST(RuntimeCollisionChecksResponseTest, RowCountsLateChecksAndSkippedPeriods) {
  // Period 10 ms. Check 1 ends 25 ms after its deadline (two periods
  // skipped), check 2 is on time, check 3 ends 12 ms late (one skipped).
  const auto row = RowAfterChecks({25'000'000, 1'000'000, 12'000'000});
  ASSERT_TRUE(row.is_object());
  EXPECT_EQ(row.as_object().size(), 5U);
  EXPECT_EQ(row.find_object_member("controllable_component_id")->as_string(),
            "lite6_robot");
  EXPECT_EQ(Number(row, "check_period_ns"), 10'000'000.0);
  EXPECT_EQ(Number(row, "checks_run"), 3.0);
  EXPECT_EQ(Number(row, "checks_late"), 2.0);
  EXPECT_EQ(Number(row, "periods_skipped"), 3.0);
}

TEST(RuntimeCollisionChecksResponseTest, RowIsZeroForARobotThatNeverCheckedLate) {
  const auto row = RowAfterChecks({1'000'000, 1'000'000, 1'000'000});
  EXPECT_EQ(Number(row, "checks_run"), 3.0);
  EXPECT_EQ(Number(row, "checks_late"), 0.0);
  EXPECT_EQ(Number(row, "periods_skipped"), 0.0);

  const auto never_checked = RuntimeCollisionChecksRowValue(
      "ur5e_robot", core::RuntimeCollisionMonitorTimingMetrics{}, 50'000'000);
  EXPECT_EQ(Number(never_checked, "check_period_ns"), 50'000'000.0);
  EXPECT_EQ(Number(never_checked, "checks_run"), 0.0);
  EXPECT_EQ(Number(never_checked, "checks_late"), 0.0);
  EXPECT_EQ(Number(never_checked, "periods_skipped"), 0.0);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
