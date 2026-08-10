#include <gtest/gtest.h>

#include <manyforge_roboplan/roboplan_planner_options.hpp>

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

TEST(RoboPlanCompatibilityOptionsTest,
     ProductionDefaultsPreserveQualifiedRoboPlan04Policy) {
  const auto rrt = production_rrt_options("arm", 2.5);
  EXPECT_EQ(rrt.group_name, "arm");
  EXPECT_DOUBLE_EQ(rrt.max_planning_time, 2.5);
  EXPECT_EQ(rrt.max_nodes, 5000);
  EXPECT_DOUBLE_EQ(rrt.collision_check_step_size, 0.05);
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
    EXPECT_FALSE(shortcut_candidate_for_attempt(attempt));
  }
}

TEST(RoboPlanCompatibilityOptionsTest,
     PortablePlanningProfilesMapToExplicitRoboPlanPolicies) {
  const auto base_rrt = production_rrt_options("arm", 2.5);
  const auto first = planning_profile_rrt_options(
      base_rrt, PlanningProfileDTO::kFirstFeasible);
  EXPECT_FALSE(first.rrt_star);
  EXPECT_TRUE(first.fast_return);

  const auto balanced = planning_profile_rrt_options(
      base_rrt, PlanningProfileDTO::kBalanced);
  EXPECT_TRUE(balanced.rrt_star);
  EXPECT_TRUE(balanced.fast_return);

  const auto quality = planning_profile_rrt_options(
      base_rrt, PlanningProfileDTO::kQualityBounded);
  EXPECT_TRUE(quality.rrt_star);
  EXPECT_FALSE(quality.fast_return);

  const auto base_ik = production_planning_ik_options("arm");
  EXPECT_TRUE(planning_profile_ik_options(
                  base_ik, PlanningProfileDTO::kFirstFeasible)
                  .fast_return);
  EXPECT_TRUE(planning_profile_ik_options(
                  base_ik, PlanningProfileDTO::kBalanced)
                  .fast_return);
  EXPECT_FALSE(planning_profile_ik_options(
                   base_ik, PlanningProfileDTO::kQualityBounded)
                   .fast_return);
}
