#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <manyforge_core/types/motion_interrupt.hpp>

namespace manyforge::roboplan_backend {

struct Ros2ControlExecutionBackendConfig {
  std::string node_name = "manyforge_ros2_control_execution_backend";
  std::string action_name;
  std::string joint_state_topic = "/joint_states";
  std::string controller_state_topic;
  std::vector<std::string> required_joints;

  double joint_state_max_age_s = 0.25;
  double wait_for_state_timeout_s = 1.0;
  double wait_for_action_server_timeout_s = 2.0;
  double start_tolerance_rad = 0.02;
  double final_tolerance_rad = 0.02;
  double tracking_error_tolerance_rad = 0.05;
  double tracking_error_timeout_s = 0.1;
  double monitor_period_s = 0.01;
  double stop_completion_timeout_s = 2.0;
  double sim_reset_motion_duration_s = 2.0;
  bool require_fresh_state_before_planning = true;
  bool disable_controller_path_tolerances = false;
  std::string execution_state_source_mode = "observed_robot";
  std::vector<double> initial_joint_positions_rad;
  std::string command_gate_topic;
  // JTC Jazzy+ on-controller speed-scaling input. If empty, derived from
  // action_name by replacing `/follow_joint_trajectory` with
  // `/speed_scaling_input`.
  std::string speed_scaling_topic;
  // Leave true for current deployments that rely on JTC's speed-scaling
  // topic for the Composer velocity slider. Production `soft_on_path`
  // path-ramp capability requires this to be false, otherwise JTC can retime
  // the replacement stop trajectory and violate wall-time deceleration_time_s.
  bool enable_jtc_speed_scaling_input = true;

  // kHardDeceleration capability evidence (spec/260 §5.6 +
  // ROS2_CONTROL_EXECUTION_ADAPTER_PLAN.md). The backend only
  // advertises `kHardDeceleration` to the kernel when ALL THREE of
  // the following preconditions hold; deployments whose controller
  // configuration does not meet them MUST leave these fields at
  // their defaults so the capability gate fails closed. Spec/260
  // §5.6 explicitly forbids hard_deceleration substitution on a
  // controller that lacks the underlying deceleration mechanism.
  //
  // 1) `decelerate_on_cancel` mirrors the JTC `constraints.decelerate_on_cancel`
  //    parameter. The controller must be configured to honor cancel
  //    requests with a deceleration profile (not an immediate stop).
  // 2) `velocity_state_interface_joints` MUST contain every
  //    `required_joints` entry. The controller cannot compute a
  //    deceleration profile without velocity feedback.
  // 3) `max_deceleration_on_cancel_rad_s2` MUST contain a strictly
  //    positive value for every `required_joints` entry. Zero or
  //    negative values would degenerate to an immediate stop.
  bool decelerate_on_cancel = false;
  std::set<std::string> velocity_state_interface_joints;
  std::map<std::string, double> max_deceleration_on_cancel_rad_s2;

  // Production path-ramp gate. This may advertise `kSoftOnPath` only
  // when JTC speed-scaling input is disabled above; otherwise the
  // controller can stretch the stop ramp. The experimental scaffold
  // remains available through `enable_soft_on_path_experimental` below.
  bool enable_soft_on_path_path_ramp = false;
  double soft_on_path_stop_traj_delta_s = 0.05;
  double soft_on_path_sample_period_s = 0.01;
  std::optional<double> soft_on_path_max_initial_path_deviation_rad;
  std::optional<double> soft_on_path_max_initial_velocity_deviation_rad_s;
  std::map<std::string, double> soft_on_path_max_jerk_rad_s3;
  bool allow_soft_on_path_model_velocity_fallback = false;

  // Production-disabled scaffold. When true and the production
  // path-ramp gate above is not active, the backend advertises `kSoftOnPath`
  // with an `_experimental` capability id and physically executes
  // spring_back_hold through explicit fallback gating.
  bool enable_soft_on_path_experimental = false;
};

/// Pure capability evaluation for ros2_control deployments. This helper
/// intentionally has no ROS message/runtime dependency so tests and deployment
/// audit tools can inspect a config in containers that do not install ROS2.
core::MotionInterruptStopCapabilityDTO
compute_ros2_control_motion_interrupt_stop_capability(
    const Ros2ControlExecutionBackendConfig& config);

}  // namespace manyforge::roboplan_backend
