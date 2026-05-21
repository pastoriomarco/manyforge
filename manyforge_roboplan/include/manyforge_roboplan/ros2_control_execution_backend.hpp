#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/action/gripper_command.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>
#if __has_include(<control_msgs/msg/speed_scaling_factor.hpp>)
#include <control_msgs/msg/speed_scaling_factor.hpp>
#define MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG 1
#endif
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <roboplan/core/scene.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <trajectory_msgs/msg/joint_trajectory_point.hpp>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::roboplan_backend {

struct NativeKernelBundle;

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
};

struct Ros2GripperActionConfig {
  std::string action_name = "/robotiq_gripper_controller/gripper_cmd";
  // Optional RoboPlan scene projection metadata. When set, the ROS gripper
  // command/status path also updates the declared gripper group so Composer's
  // FK/link-pose scene state renders the gripper through the same robot-state
  // path as the arm.
  std::string group_id;
  std::string joint_name;
  double wait_for_action_server_timeout_s = 2.0;
  double result_timeout_s = 5.0;
};

struct Ros2GripperCommandResult {
  bool accepted = false;
  std::string error;
};

struct Ros2GripperStatus {
  std::string state = "idle";
  double progress = 0.0;
  std::optional<double> current_position;
  std::optional<double> target_position;
  std::string error;
};

/// Controller-backed execution backend for ros2_control/JTC deployments.
///
/// This class owns the timing-sensitive ROS boundary: action lifecycle,
/// controller-state observation, source freshness, tracking divergence, and
/// observer-safe snapshots. ManyForge core still sees only IExecutionBackend
/// DTOs; ROS message types do not cross the core boundary.
///
/// Lock-order contract:
/// - Do not hold state_mutex_ while taking execution_mutex_.
/// - Do not hold gripper_map_mutex_ while taking execution_mutex_ or state_mutex_.
/// - scene_mutex_ is an external RoboPlan-scene lock; monitor/callback paths
///   must use best-effort scene updates so long validation sweeps cannot block
///   reset/stop completion.
/// - Per-GripperRuntime::mutex is taken only after gripper_map_mutex_ has been
///   released, using a copied shared_ptr to the runtime.
class Ros2ControlExecutionBackend final : public core::IExecutionBackend {
 public:
  Ros2ControlExecutionBackend(std::shared_ptr<roboplan::Scene> scene,
                              std::string group_name,
                              core::IClock& clock,
                              Ros2ControlExecutionBackendConfig config,
                              std::shared_ptr<std::mutex> scene_mutex = nullptr);
  ~Ros2ControlExecutionBackend() override;

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const override;

  // Streaming-session mode (ADR-0016).
  //
  // v0.1 ros2_control semantics:
  //   - Joint-mode targets are first-class: each push builds an anchored
  //     two-point FollowJointTrajectory (anchor at observed state with
  //     negative time_from_start, target at the requested joint vector
  //     with positive time_from_start computed from joint-velocity
  //     limits). Latest-wins coalescing happens at the streaming worker.
  //     The negative-time anchor gives JTC's spline a continuous starting
  //     condition, so successive preempts produce smooth motion (no
  //     velocity discontinuity at the goal-replacement seam).
  //   - Cartesian-mode targets are NOT supported in this revision; the
  //     IK pre-resolution path is deferred. Cartesian pushes return
  //     unsupported_capability. Operators get Cartesian via the existing
  //     one-shot /api/direct/cartesian-jog path until IK pre-resolution
  //     lands; joint sliders are first-class today.
  //   - Soft stop strategies:
  //       spring_back  -> final goal: anchor at observed state with
  //                       negative time_from_start; future point at
  //                       observed *position* with zero velocity and
  //                       time_from_start = config.soft_stop_deceleration_time_s.
  //       velocity_scaling -> NOT YET implemented; falls back to spring_back
  //                       with a structured warning event. Promoting it to
  //                       the default requires the qualification matrix in
  //                       docs/open-points.md.
  core::StreamingSessionBeginResultDTO begin_streaming_session(
      const core::StreamingSessionConfigDTO& config) override;
  core::StreamingPushResultDTO push_streaming_target(
      uint64_t session_id,
      const core::StreamingTargetDTO& target) override;
  core::StreamingSessionEndResultDTO end_streaming_session(
      uint64_t session_id,
      core::SoftStopStrategy strategy) override;
  // ADR-0018: per-tick runtime velocity scale for discrete trajectory
  // monitoring and streaming dispatch.
  void set_runtime_velocity_scale(double scale) override;

  void configure_gripper_action(std::string gripper_id,
                                Ros2GripperActionConfig config);
  Ros2GripperCommandResult command_gripper(const std::string& gripper_id,
                                           double target_position,
                                           double duration_s,
                                           double max_effort);
  Ros2GripperStatus get_gripper_status(const std::string& gripper_id) const;
  void update_gripper_commands();

 private:
  using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
  using GoalHandleFJT = rclcpp_action::ClientGoalHandle<FollowJointTrajectory>;
  using GripperCommand = control_msgs::action::GripperCommand;
  using GoalHandleGripper = rclcpp_action::ClientGoalHandle<GripperCommand>;

  struct JointSample {
    std::vector<std::string> joint_names;
    std::vector<double> positions;
    std::vector<double> velocities;
    std::chrono::steady_clock::time_point received_at;
    std::string source;
    std::string topic;
  };

  struct TrackingErrorPeak {
    std::string joint_name;
    double signed_error_rad = 0.0;
    double abs_error_rad = 0.0;
    double elapsed_s = 0.0;
    double expected_position_rad = 0.0;
    double observed_position_rad = 0.0;
    double observed_velocity_rad_s = 0.0;
    bool has_expected_position = false;
    bool has_observed_position = false;
    bool has_observed_velocity = false;
  };

  enum class SceneUpdateMode {
    kBlocking,
    kBestEffort,
  };

  struct GripperRuntime {
    Ros2GripperActionConfig config;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    mutable std::mutex mutex;
    GoalHandleGripper::SharedPtr active_goal_handle;
    std::string state = "idle";
    bool executing = false;
    bool goal_accepted = false;
    bool cancel_pending = false;
    std::optional<double> start_position;
    std::optional<double> current_position;
    std::optional<double> max_effort;
    double target_position = 0.0;
    double duration_s = 0.0;
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::chrono::steady_clock::time_point> deadline_at;
    std::string error;
  };

  void validate_config() const;
  void start_ros();
  void stop_ros();

  void on_joint_state(const sensor_msgs::msg::JointState::SharedPtr msg);
  void on_controller_state(
      const control_msgs::msg::JointTrajectoryControllerState::SharedPtr msg);
  void on_action_feedback(
      GoalHandleFJT::SharedPtr,
      const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback);
  void on_action_result(const GoalHandleFJT::WrappedResult& result);

  std::optional<JointSample> select_latest_sample_locked(
      std::chrono::steady_clock::time_point now) const;
  std::optional<JointSample> wait_for_fresh_sample(double timeout_s);
  std::optional<JointSample> wait_for_fresh_sample_after(
      double timeout_s,
      std::chrono::steady_clock::time_point not_before);
  std::vector<double> positions_for_joints(const JointSample& sample,
                                           const std::vector<std::string>& joints) const;
  void sync_idle_scene_from_latest_sample();
  bool is_commanded_sim_mode() const;
  bool uses_authored_backend_state() const;
  void set_command_gate(bool enabled);
  void publish_runtime_velocity_scale(double scale);
  core::Result<void> command_sim_initial_state(std::string phase);
  std::vector<double> interpolate_joint_positions(
      const core::TrajectoryDTO& trajectory, double time_s) const;
  bool update_scene_positions(const std::vector<double>& joint_positions,
                              SceneUpdateMode mode = SceneUpdateMode::kBlocking);
  std::optional<double> scene_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const;
  bool update_scene_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position,
      SceneUpdateMode mode = SceneUpdateMode::kBlocking);
  void project_gripper_position_to_scene(
      const Ros2GripperActionConfig& config,
      double position,
      SceneUpdateMode mode = SceneUpdateMode::kBlocking);
  void publish_snapshot(core::ExecutionState state,
                        double progress,
                        const std::vector<double>& joint_positions,
                        std::optional<core::ErrorDTO> error = std::nullopt);
  core::ErrorDTO operator_intervention_error(
      core::ErrorCode code,
      std::string message,
      std::string phase,
      core::ExecutionState backend_state = core::ExecutionState::kInterventionRequired)
      const;
  core::Result<void> resync_after_intervention_if_needed(std::string phase);
  void cancel_goal_if_known(const GoalHandleFJT::SharedPtr& goal_handle) noexcept;
  std::string latest_feedback_error_summary() const;
  std::optional<TrackingErrorPeak> update_tracking_error_peak_locked(
      const JointSample& error_sample,
      double elapsed_s,
      const std::vector<double>* observed_positions = nullptr,
      const std::vector<double>* expected_positions = nullptr,
      const std::vector<double>* observed_velocities = nullptr);
  void fail_execution(std::string message,
                      core::ErrorCode code = core::ErrorCode::kExecutionAborted);
  void require_operator_intervention(std::string message,
                                     core::ErrorCode code = core::ErrorCode::kStopFailed);
  void complete_execution();
  void monitor_worker(core::TrajectoryDTO trajectory);
  bool start_state_matches(const core::TrajectoryDTO& trajectory,
                           const JointSample& sample,
                           std::string& error) const;
  bool final_state_matches(const core::TrajectoryDTO& trajectory,
                           const JointSample& sample,
                           std::string& error) const;
  double max_abs_error(const std::vector<double>& lhs,
                       const std::vector<double>& rhs) const;

  void on_gripper_feedback(
      const std::shared_ptr<GripperRuntime>& runtime,
      GoalHandleGripper::SharedPtr,
      const std::shared_ptr<const GripperCommand::Feedback> feedback);
  void on_gripper_result(
      const std::shared_ptr<GripperRuntime>& runtime,
      const GoalHandleGripper::WrappedResult& result);
  void cancel_all_gripper_commands();

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  core::IClock& clock_;
  Ros2ControlExecutionBackendConfig config_;
  std::shared_ptr<std::mutex> scene_mutex_;

  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::thread spin_thread_;
  rclcpp_action::Client<FollowJointTrajectory>::SharedPtr action_client_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_sub_;
  rclcpp::Subscription<control_msgs::msg::JointTrajectoryControllerState>::SharedPtr
      controller_state_sub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr command_gate_pub_;
#ifdef MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG
  rclcpp::Publisher<control_msgs::msg::SpeedScalingFactor>::SharedPtr
      speed_scaling_pub_;
#endif

  mutable std::mutex state_mutex_;
  std::condition_variable state_cv_;
  std::optional<JointSample> latest_joint_state_;
  std::optional<JointSample> latest_controller_state_;
  std::optional<JointSample> latest_action_feedback_;
  std::optional<JointSample> latest_action_feedback_error_;
  core::ExecutionSnapshot snapshot_;

  mutable std::mutex execution_mutex_;
  std::condition_variable execution_cv_;
  std::thread monitor_thread_;
  std::atomic<bool> monitor_active_{false};
  bool running_ = false;
  bool terminal_ = true;
  bool stop_requested_ = false;
  bool pause_requested_ = false;
  bool goal_accepted_ = false;
  bool first_feedback_seen_ = false;
  bool result_received_ = false;
  bool resync_required_ = false;
  bool resync_in_progress_ = false;
  GoalHandleFJT::SharedPtr active_goal_handle_;
  std::optional<rclcpp_action::GoalUUID> active_goal_id_;
  std::chrono::steady_clock::time_point execution_started_at_;
  std::optional<std::chrono::steady_clock::time_point> stop_requested_at_;
  std::optional<std::chrono::steady_clock::time_point> tracking_error_since_;
  std::optional<TrackingErrorPeak> peak_tracking_error_;

  mutable std::mutex gripper_map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<GripperRuntime>> grippers_;

  // Streaming-session bookkeeping (ADR-0016).
  void streaming_dispatch_worker(std::stop_token st);
  bool streaming_send_anchored_goal(
      const std::vector<std::string>& joint_names,
      const std::vector<double>& target_positions,
      double target_time_from_start_s,
      bool track_terminal_goal = false);
  bool streaming_send_spring_back_stop();
  void on_streaming_terminal_result(
      const GoalHandleFJT::WrappedResult& result);

  mutable std::mutex streaming_mutex_;
  std::condition_variable streaming_cv_;
  std::jthread streaming_thread_;
  std::atomic<bool> streaming_active_{false};
  uint64_t streaming_session_id_ = 0;
  uint64_t next_streaming_backend_seq_ = 1;
  core::StreamingSessionConfigDTO streaming_config_;
  // ADR-0018: runtime velocity scale read by monitor/dispatcher loops.
  std::atomic<double> runtime_velocity_scale_{1.0};
  std::atomic<double> applied_runtime_velocity_scale_{1.0};
  std::optional<core::StreamingTargetDTO> streaming_target_;
  // The most-recent goal handle issued by the streaming dispatcher.
  // Subsequent dispatches preempt it via async_send_goal; the existing
  // on_action_result callback handles its terminal state.
  GoalHandleFJT::SharedPtr streaming_active_goal_;
  std::optional<rclcpp_action::GoalUUID> streaming_terminal_goal_id_;
  bool streaming_terminal_result_received_ = true;
  bool streaming_terminal_result_success_ = true;
  std::optional<core::ErrorDTO> streaming_terminal_error_;
};

std::shared_ptr<NativeKernelBundle> create_ros2_control_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const Ros2ControlExecutionBackendConfig& ros_config,
    const core::ExecutionKernelConfig& kernel_config = {});

}  // namespace manyforge::roboplan_backend
