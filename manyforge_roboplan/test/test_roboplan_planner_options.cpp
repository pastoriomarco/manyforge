#include <gtest/gtest.h>

#include <manyforge_roboplan/roboplan_planner_options.hpp>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

namespace {
[[nodiscard]] PlannerSettingsDTO step_settings(double step) {
  PlannerSettingsDTO settings;
  settings.collision_check_step_rad = step;
  return settings;
}
}  // namespace

TEST(RoboPlanCompatibilityOptionsTest,
     ProductionDefaultsAreRrtConnectFirstPath) {
  const auto rrt = production_rrt_options("arm", 2.5);
  EXPECT_EQ(rrt.group_name, "arm");
  EXPECT_DOUBLE_EQ(rrt.max_planning_time, 2.5);
  EXPECT_EQ(rrt.max_nodes, 5000);
  EXPECT_DOUBLE_EQ(rrt.collision_check_step_size, 0.05);
  EXPECT_TRUE(rrt.rrt_connect);
  EXPECT_FALSE(rrt.rrt_star);
  EXPECT_TRUE(rrt.fast_return);

  const auto shortcut = production_shortcutting_options("arm", 42U);
  EXPECT_EQ(shortcut.group_name, "arm");
  EXPECT_DOUBLE_EQ(shortcut.max_step_size, 0.05);
  EXPECT_EQ(shortcut.max_iters, 100);
  EXPECT_EQ(shortcut.max_convergence_iters, 0);
  EXPECT_EQ(shortcut.seed, 42);

  const auto ik = production_planning_ik_options("arm");
  EXPECT_EQ(ik.group_name, "arm");
  EXPECT_DOUBLE_EQ(ik.step_size, 0.25);
  EXPECT_EQ(ik.max_restarts, 50);
  EXPECT_DOUBLE_EQ(ik.max_time, 1.0);
  EXPECT_TRUE(ik.fast_return);
}

TEST(RoboPlanCompatibilityOptionsTest,
     AuthoritativeCollisionRetriesKeepTheTighterRrtDetour) {
  const auto initial_rrt = production_rrt_options("arm", 2.5, 0U);
  const auto initial_shortcut =
      production_shortcutting_options("arm", 42U, 0U);
  EXPECT_DOUBLE_EQ(initial_rrt.collision_check_step_size, 0.05);
  EXPECT_DOUBLE_EQ(initial_shortcut.max_step_size, 0.05);
  EXPECT_TRUE(shortcut_candidate_for_attempt(0U));

  for (const std::uint32_t attempt : {1U, 2U, 17U}) {
    const auto retry_rrt = production_rrt_options("arm", 2.5, attempt);
    const auto retry_shortcut =
        production_shortcutting_options("arm", 42U, attempt);
    EXPECT_DOUBLE_EQ(retry_rrt.collision_check_step_size, 0.01);
    EXPECT_DOUBLE_EQ(retry_shortcut.max_step_size, 0.01);
    EXPECT_EQ(retry_shortcut.seed, 42);
  }
}

TEST(RoboPlanCompatibilityOptionsTest,
     ConfiguredCollisionCheckStepDrivesTheFirstAttemptAndBoundsRetries) {
  // M6 decision 18: the step is settable (Deployment collision_check_step_rad)
  // and validation uses the same value. Retries keep the finer of it and 0.01.
  EXPECT_DOUBLE_EQ(kDefaultCollisionCheckStepRad, 0.05);
  const auto coarse = production_rrt_options("arm", 2.5, 0U, step_settings(0.08));
  EXPECT_DOUBLE_EQ(coarse.collision_check_step_size, 0.08);
  EXPECT_DOUBLE_EQ(production_shortcutting_options("arm", 42U, 0U, 0.08).max_step_size,
                   0.08);
  EXPECT_DOUBLE_EQ(production_rrt_options("arm", 2.5, 1U, step_settings(0.08)).collision_check_step_size,
                   0.01);
  const auto fine = production_rrt_options("arm", 2.5, 0U, step_settings(0.004));
  EXPECT_DOUBLE_EQ(fine.collision_check_step_size, 0.004);
  EXPECT_DOUBLE_EQ(production_rrt_options("arm", 2.5, 2U, step_settings(0.004)).collision_check_step_size,
                   0.004);
  EXPECT_DOUBLE_EQ(production_shortcutting_options("arm", 42U, 1U, 0.004).max_step_size,
                   0.004);
  // One rule, shared with validation of the attempt's trajectory.
  for (const std::uint32_t attempt : {0U, 1U, 5U}) {
    for (const double step : {0.08, 0.004}) {
      EXPECT_DOUBLE_EQ(production_rrt_options("arm", 2.5, attempt, step_settings(step)).collision_check_step_size,
                       collision_check_step_for_attempt(step, attempt));
    }
  }
}

TEST(RoboPlanCompatibilityOptionsTest,
     FirstValidationRetryShortcutsAndLaterRetriesKeepTheDetour) {
  // M6 clearance: validation with the clearance margin rejects a shortcut
  // that skims an obstacle at a checked sample, so the first retry (attempt
  // 1) shortcuts its 0.01-step detour; only a retry after that keeps the raw
  // detour.
  EXPECT_TRUE(shortcut_candidate_for_attempt(0U));
  EXPECT_TRUE(shortcut_candidate_for_attempt(1U));
  for (const std::uint32_t attempt : {2U, 3U, 17U}) {
    EXPECT_FALSE(shortcut_candidate_for_attempt(attempt));
  }
}

TEST(RoboPlanCompatibilityOptionsTest, PlannerBlockMapsToRoboPlanRrtOptions) {
  PlannerSettingsDTO planner;
  planner.search = PlannerSearchDTO::kRrt;
  planner.max_nodes = 1234U;
  const auto rrt = production_rrt_options("arm", 2.5, 0U, planner);
  EXPECT_FALSE(rrt.rrt_connect);
  EXPECT_EQ(rrt.max_nodes, 1234U);
  EXPECT_FALSE(rrt.rrt_star);
  EXPECT_TRUE(rrt.fast_return);

  planner.search = PlannerSearchDTO::kRrtConnect;
  planner.optimize = PlannerOptimizeDTO::kRrtStar;
  const auto star = production_rrt_options("arm", 2.5, 0U, planner);
  EXPECT_TRUE(star.rrt_connect);
  EXPECT_TRUE(star.rrt_star);
  EXPECT_FALSE(star.fast_return);

  // best_of_n repeats first-path plans; the candidate computer keeps the
  // shortest.
  planner.optimize = PlannerOptimizeDTO::kBestOfN;
  const auto best = production_rrt_options("arm", 2.5, 0U, planner);
  EXPECT_FALSE(best.rrt_star);
  EXPECT_TRUE(best.fast_return);
}

TEST(RoboPlanCompatibilityOptionsTest, MotionOverrideReplacesOnlyItsMembers) {
  PlannerSettingsDTO defaults;
  defaults.n = 6U;
  PlannerOverrideDTO override_value;
  override_value.optimize = PlannerOptimizeDTO::kBestOfN;
  override_value.collision_check_step_rad = 0.02;
  const auto resolved = resolve_planner_settings(defaults, override_value);
  EXPECT_EQ(resolved.search, PlannerSearchDTO::kRrtConnect);
  EXPECT_EQ(resolved.optimize, PlannerOptimizeDTO::kBestOfN);
  EXPECT_EQ(resolved.n, 6U);
  EXPECT_DOUBLE_EQ(resolved.collision_check_step_rad, 0.02);
  EXPECT_EQ(resolved.max_nodes, kDefaultPlannerMaxNodes);
  EXPECT_EQ(resolve_planner_settings(defaults, {}), defaults);

  EXPECT_TRUE(valid_planner_override(override_value));
  override_value.n = 1U;
  EXPECT_FALSE(valid_planner_override(override_value));
}
