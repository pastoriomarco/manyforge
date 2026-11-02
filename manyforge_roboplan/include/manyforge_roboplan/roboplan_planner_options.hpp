#pragma once

// Pure policy extraction from this path at parent
// c4b075fa6dfb69e463c61f3efa8cfce8de681e8a. The future I6 planning adapter
// must consume these qualified defaults; this restores no planner owner.

#include <string>
#include <optional>
#include <cstdint>
#include <limits>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/planner_settings.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

namespace manyforge::roboplan_backend {

/// The configured step (Deployment `collision_check_step_rad`) on the first
/// attempt, min(step, 0.01) on retries (core::collision_check_step_for_attempt).
inline constexpr double collision_check_step_size_for_attempt(
    const std::uint32_t planning_attempt_index,
    const double configured_step_rad = core::kDefaultCollisionCheckStepRad) noexcept {
  return core::collision_check_step_for_attempt(configured_step_rad, planning_attempt_index);
}

inline constexpr bool shortcut_candidate_for_attempt(
    const std::uint32_t planning_attempt_index) noexcept {
  // Shortcutting is an optional quality/performance pass, not the collision
  // authority. Validation checks the executed curve with the collision
  // clearance, so a shortcut that skims an obstacle at a checked sample is
  // rejected there rather than dispatched. The first retry (0.01 step) is
  // therefore shortcut too: without it the M55 F-1 detour took 17.9 s instead
  // of about 8.5 s. If validation rejects that shortcut path as well, later
  // retries keep the unshortcut collision-avoiding detour.
  return planning_attempt_index <= 1U;
}

/// RRT options from the effective planner block (Deployment default with the
/// motion's override applied). best_of_n plans with first-path RRT, repeated
/// by the candidate computer.
inline roboplan::RRTOptions production_rrt_options(
    const std::string& group_name, double max_planning_time_s,
    const std::uint32_t planning_attempt_index = 0U,
    const core::PlannerSettingsDTO& planner = {}) {
  roboplan::RRTOptions options;
  options.group_name = group_name;
  options.max_planning_time = max_planning_time_s;
  options.max_nodes = planner.max_nodes;
  options.collision_check_step_size = collision_check_step_size_for_attempt(
      planning_attempt_index, planner.collision_check_step_rad);
  options.rrt_connect = planner.search == core::PlannerSearchDTO::kRrtConnect;
  // rrt_star rewires and spends the node/time budget on the cheapest path.
  options.rrt_star = planner.optimize == core::PlannerOptimizeDTO::kRrtStar;
  options.fast_return = !options.rrt_star;
  return options;
}

inline roboplan::PathShortcuttingOptions production_shortcutting_options(
    const std::string& group_name,
    std::optional<std::uint32_t> deterministic_seed = std::nullopt,
    const std::uint32_t planning_attempt_index = 0U,
    const double configured_step_rad = core::kDefaultCollisionCheckStepRad) {
  roboplan::PathShortcuttingOptions options;
  options.group_name = group_name;
  options.max_step_size = collision_check_step_size_for_attempt(
      planning_attempt_index, configured_step_rad);
  options.max_iters = 100;
  // RoboPlan 0.4 always spent the configured 100-iteration budget. Keep that
  // compatibility behavior; convergence-shortcutting remains a separate
  // opt-in row until its owning milestone qualifies a production policy change.
  options.max_convergence_iters = 0;
  options.seed = deterministic_seed.has_value()
                     ? static_cast<int>(
                           *deterministic_seed &
                           static_cast<std::uint32_t>(
                               std::numeric_limits<int>::max()))
                     : -1;
  return options;
}

inline roboplan::SimpleIkOptions production_planning_ik_options(
    const std::string& group_name) {
  roboplan::SimpleIkOptions options;
  options.group_name = group_name;
  options.step_size = 0.25;
  options.max_restarts = 50;
  options.max_time = 1.0;
  options.fast_return = true;
  return options;
}

inline roboplan::SimpleIkOptions production_single_shot_ik_options(
    const std::string& group_name, double max_time_s) {
  auto options = production_planning_ik_options(group_name);
  options.max_time = max_time_s;
  return options;
}

}  // namespace manyforge::roboplan_backend
