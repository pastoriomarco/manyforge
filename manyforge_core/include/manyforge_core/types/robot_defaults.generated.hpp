// Generated from manyforge_core/resources/robot_defaults.tsv by manyforge_core/tools/generate_robot_defaults.py; do not edit.
#pragma once

#include <cstdint>
#include <string_view>

namespace manyforge::core {

/// A Deployment component's runtime defaults: the value of each field the
/// Deployment leaves out (the GUI shows the same table as "Default").
/// `execution_tick_hz`
inline constexpr std::uint32_t kDefaultExecutionTickHz = 100U;
/// `collision_monitor_period_ms`
inline constexpr double kDefaultCollisionMonitorPeriodMs = 50.0;
/// `planner_rng_seed`
inline constexpr std::uint32_t kDefaultPlannerRngSeed = 12345U;
/// `collision_clearance_m`
inline constexpr double kDefaultCollisionClearanceM = 0.001;
/// `collision_check_step_rad`
inline constexpr double kDefaultCollisionCheckStepRad = 0.05;
/// `collision_check_fresh_plans`
inline constexpr bool kDefaultCollisionCheckFreshPlans = true;
/// `planner_search`
inline constexpr std::string_view kDefaultPlannerSearchName{"rrt_connect"};
/// `planner_optimize`
inline constexpr std::string_view kDefaultPlannerOptimizeName{"first"};
/// `planner_n`
inline constexpr std::uint32_t kDefaultPlannerBestOfN = 4U;
/// `planner_max_nodes`
inline constexpr std::uint32_t kDefaultPlannerMaxNodes = 5000U;
/// `cartesian_timing`
inline constexpr std::string_view kDefaultCartesianTimingName{"constant_speed"};
/// `motion_timing`
inline constexpr std::string_view kDefaultMotionTimingName{"smooth"};
/// `stop_lead_in_anchor`
inline constexpr std::string_view kDefaultStopLeadInAnchorName{"commanded"};
/// `controller_tcp_offset_sync`
inline constexpr bool kDefaultControllerTcpOffsetSync = true;
/// `lifecycle_command_timeout_s`
inline constexpr double kDefaultLifecycleCommandTimeoutS = 10.0;

}  // namespace manyforge::core
