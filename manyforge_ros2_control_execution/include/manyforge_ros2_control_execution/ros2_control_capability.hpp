#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

namespace manyforge::ros2_control_execution {

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
  double stop_completion_timeout_s = 0.5;
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
  // Anchor the soft-stop seam velocity on the trajectory model (path tangent x
  // runtime scale) rather than measured feedback. Unset = auto: prefer the model
  // on the commanded-sim lane, where the reported feedback velocity is decoupled
  // from the commanded motion (it can be opposite-signed) and anchoring on it
  // sign-flips the seam; prefer measured feedback otherwise. Set explicitly to
  // force a lane's choice (e.g. feedback-first on a real robot with faithful
  // velocity feedback).
  std::optional<bool> prefer_model_anchor_velocity;

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

/// Resolve which state source feeds the `soft_on_path` seam anchor's velocity,
/// per the per-lane anchor-state authority. Returns `kMeasuredFeedback` only
/// when measured velocity is present AND the lane is not configured to prefer
/// the model; otherwise `kModelDerived` (the planner then derives the velocity
/// from the trajectory tangent x runtime scale). `prefer_model_anchor_velocity`
/// defaults to true on the commanded-sim lane, where reported feedback velocity
/// is decoupled from the commanded motion. Pure (no ROS) so it is unit-testable.
core::SoftOnPathStateSource resolve_soft_on_path_anchor_velocity_source(
    const Ros2ControlExecutionBackendConfig& config,
    bool has_measured_velocity);

namespace ros2_control_fault_reason {
inline constexpr const char* kActionServerUnavailable =
    "action_server_unavailable";
inline constexpr const char* kControllerCancelTimeout =
    "controller_cancel_timeout";
inline constexpr const char* kControllerFeedbackTimeout =
    "controller_feedback_timeout";
inline constexpr const char* kControllerGoalDispatchFailed =
    "controller_goal_dispatch_failed";
inline constexpr const char* kControllerGoalRejected =
    "controller_goal_rejected";
inline constexpr const char* kControllerGoalResponseTimeout =
    "controller_goal_response_timeout";
inline constexpr const char* kControllerResultFailed =
    "controller_result_failed";
inline constexpr const char* kEmptyTrajectory = "empty_trajectory";
inline constexpr const char* kMonitorThreadStillShuttingDown =
    "monitor_thread_still_shutting_down";
inline constexpr const char* kStaleControllerState =
    "stale_controller_state";
inline constexpr const char* kStartStateMismatch = "start_state_mismatch";
inline constexpr const char* kTrackingErrorExceeded =
    "tracking_error_exceeded";
// Gripper fault classes. Same taxonomy contract as the
// FollowJointTrajectory classes above: the stable token travels in
// `reason_code` next to the prose error so digest promotion and the
// external diagnostics surfaces classify gripper faults instead of seeing
// bare `tree_failed` prose. All gripper classes are ERROR severity: a
// gripper fault aborts the tree fail-closed but does not leave arm motion
// unconfirmed (the CRITICAL rule is reserved for unconfirmed-stop /
// stuck-lifecycle classes).
inline constexpr const char* kGripperActionServerUnavailable =
    "gripper_action_server_unavailable";
inline constexpr const char* kGripperBusy = "gripper_busy";
inline constexpr const char* kGripperGoalAborted = "gripper_goal_aborted";
inline constexpr const char* kGripperGoalDispatchFailed =
    "gripper_goal_dispatch_failed";
inline constexpr const char* kGripperGoalRejected = "gripper_goal_rejected";
inline constexpr const char* kGripperInvalidCommand =
    "gripper_invalid_command";
inline constexpr const char* kGripperNotConfigured = "gripper_not_configured";
inline constexpr const char* kGripperResultFailed = "gripper_result_failed";
inline constexpr const char* kGripperResultTimeout =
    "gripper_result_timeout";
}  // namespace ros2_control_fault_reason

/// Build a backend ErrorDTO with the stable reason token carried in
/// `details["reason_code"]`. The top-level ErrorDTO code remains one of the
/// core error-code constants so existing kernel state-machine handling stays
/// compatible; UI/observability surfaces use the reason token for the exact
/// operator-facing cause.
core::ErrorDTO make_ros2_control_fault_error(
    core::ErrorCode code,
    std::string message,
    std::string reason_code,
    std::map<std::string, std::string> details = {});

}  // namespace manyforge::ros2_control_execution
