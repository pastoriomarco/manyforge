#pragma once

// Pure policy extraction from this path at parent
// c4b075fa6dfb69e463c61f3efa8cfce8de681e8a. The future I6 planning adapter
// must consume these qualified defaults; this restores no planner owner.

#include <string>
#include <optional>
#include <cstdint>
#include <limits>

#include <manyforge_core/types/motion_spec.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

namespace manyforge::roboplan_backend {

inline roboplan::RRTOptions production_rrt_options(
    const std::string& group_name, double max_planning_time_s) {
  roboplan::RRTOptions options;
  options.group_name = group_name;
  options.max_planning_time = max_planning_time_s;
  options.max_nodes = 5000;
  // Preserve ManyForge's qualified first-feasible policy inherited from the
  // 0.4 baseline. Later RoboPlan releases add budget-spending and optimal
  // variants, but they remain opt-in capabilities.
  options.rrt_star = false;
  options.fast_return = true;
  return options;
}

inline roboplan::RRTOptions planning_profile_rrt_options(
    roboplan::RRTOptions options,
    core::PlanningProfileDTO profile) {
  switch (profile) {
    case core::PlanningProfileDTO::kFirstFeasible:
      options.rrt_star = false;
      options.fast_return = true;
      break;
    case core::PlanningProfileDTO::kBalanced:
      // Rewire while searching, but return the first complete candidate. This
      // improves the in-search tree without deliberately spending the
      // remaining request budget after feasibility has been established.
      options.rrt_star = true;
      options.fast_return = true;
      break;
    case core::PlanningProfileDTO::kQualityBounded:
      options.rrt_star = true;
      options.fast_return = false;
      break;
  }
  return options;
}

inline roboplan::PathShortcuttingOptions production_shortcutting_options(
    const std::string& group_name,
    std::optional<std::uint32_t> deterministic_seed = std::nullopt) {
  roboplan::PathShortcuttingOptions options;
  options.group_name = group_name;
  options.max_step_size = 0.05;
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

inline roboplan::SimpleIkOptions planning_profile_ik_options(
    roboplan::SimpleIkOptions options,
    core::PlanningProfileDTO profile) {
  // Quality-bounded planning searches the bounded restart set for the
  // seed-nearest feasible solution. The other profiles return the first
  // feasible IK candidate; Balanced spends quality effort in path search.
  options.fast_return =
      profile != core::PlanningProfileDTO::kQualityBounded;
  return options;
}

inline roboplan::SimpleIkOptions production_single_shot_ik_options(
    const std::string& group_name, double max_time_s) {
  auto options = production_planning_ik_options(group_name);
  options.max_time = max_time_s;
  return options;
}

}  // namespace manyforge::roboplan_backend
