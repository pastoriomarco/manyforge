#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <trajectory_msgs/msg/joint_trajectory_point.hpp>

#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>

namespace manyforge::ros2_control_execution {

struct Ros2GripperActionConfig {
  std::string action_name = "/robotiq_gripper_controller/gripper_cmd";
  std::string transport_kind = "ros_gripper_command";
  std::string command_topic;
  std::string state_topic;
  std::vector<std::string> command_joint_names;
  // Optional execution-state sink projection metadata. When set, the ROS
  // gripper command/status path also updates the declared gripper group so
  // Composer's FK/link-pose scene state renders the gripper through the same
  // robot-state path as the arm.
  std::string group_id;
  std::string joint_name;
  std::string completion_policy = "position_reached";
  double open_position = 0.0;
  double closed_position = 0.0;
  double open_effort = 0.0;
  double close_effort = 0.0;
  bool publish_nominal_position = false;
  double state_freshness_timeout_s = 1.0;
  double wait_for_action_server_timeout_s = 2.0;
  double result_timeout_s = 5.0;
};

struct Ros2GripperCommandResult {
  bool accepted = false;
  std::string error;
  // Stable fault token from ros2_control_fault_reason; empty when accepted.
  std::string reason_code;
};

struct Ros2GripperStatus {
  std::string state = "idle";
  double progress = 0.0;
  std::optional<double> current_position;
  std::optional<double> observed_position;
  std::optional<double> target_position;
  std::vector<std::string> evidence;
  std::string evidence_state;
  std::string error;
  // Stable fault token from ros2_control_fault_reason; empty unless
  // state == "failed".
  std::string reason_code;
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
/// - monitor/callback paths must use best-effort execution-state sink updates
///   so long validation sweeps cannot block reset/stop completion.
/// - Per-GripperRuntime::mutex is taken only after gripper_map_mutex_ has been
///   released, using a copied shared_ptr to the runtime.
class Ros2ControlExecutionBackend final : public core::IExecutionBackend {
 public:
  Ros2ControlExecutionBackend(std::shared_ptr<core::IExecutionStateSink>
                                  execution_state_sink,
                              std::string group_name,
                              core::IClock& clock,
                              Ros2ControlExecutionBackendConfig config);
  ~Ros2ControlExecutionBackend() override;

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const override;

  // Controlled-stop capability + result surface for motion-scoped
  // interrupt outcomes (spec/260 §5.6). The ros2_control adapter
  // supports these stop modes:
  // - `kSpringBackHold`: JTC preempt + backend-computed on-path replacement.
  //   The replacement decelerates forward for half the configured time, then
  //   returns along the accepted path to the trip cursor for the remaining half.
  //   It is not implemented through cancel-first hold behavior.
  // - `kHardDeceleration`: native JTC `decelerate_on_cancel`.
  //   Advertised only when controller-config evidence is complete
  //   (see `Ros2ControlExecutionBackendConfig` capability fields);
  //   cancel the active goal and let the controller decelerate
  //   in-place using its configured per-joint limits, surface as
  //   `kJtcCancelDecelerateOnCancel` with `stop_distance_rad`
  //   telemetry.
  // - `kSoftOnPath`: path-ramp replacement trajectory, advertised
  //   only when explicitly enabled and qualified by the active deployment.
  core::MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO& profile) override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO& profile,
      std::stop_token stop_token) override;

  // Pure capability evaluation, exposed as a static so tests and
  // deployment audit tools can verify which stop modes a
  // deployment's `Ros2ControlExecutionBackendConfig` will advertise
  // without instantiating ROS. The runtime
  // `get_motion_interrupt_stop_capability()` is just a thin wrapper
  // around this helper.
  static core::MotionInterruptStopCapabilityDTO
  compute_motion_interrupt_stop_capability(
      const Ros2ControlExecutionBackendConfig& config);

  // ADR-0018: per-tick runtime velocity scale for discrete trajectory
  // execution/monitoring.
  void set_runtime_velocity_scale(double scale) override;

  void configure_gripper_action(std::string gripper_id,
                                Ros2GripperActionConfig config);
  Ros2GripperCommandResult command_gripper(const std::string& gripper_id,
                                           double target_position,
                                           double duration_s,
                                           double max_effort);
  Ros2GripperCommandResult mark_gripper_grasp_lost(
      const std::string& gripper_id,
      const std::string& evidence_label,
      const std::string& message,
      const std::string& reason_code = "grasp_confirmation_lost");
  Ros2GripperStatus get_gripper_status(const std::string& gripper_id) const;
  void update_gripper_commands();

  // Command-enable gate: this backend is the SINGLE publishing authority
  // for the gate topic (reliable + transient_local, so the latched value is
  // the truth for late-joining bridge subscribers). External flows that need
  // command suppression — e.g. the simulator-reset flow — must request it
  // here instead of publishing the topic themselves; a second publisher
  // makes the latched gate value reader-dependent (split-brain).
  bool command_gate_configured() const {
    return !config_.command_gate_topic.empty();
  }
  bool command_gate_enabled() const {
    return command_gate_enabled_.load(std::memory_order_acquire);
  }
  // Public request form of the gate write. Returns false when no gate is
  // configured for this deployment (callers must treat that as a contract
  // error when they required gating).
  bool request_command_gate(bool enabled);

#ifdef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
  using TestingFollowJointTrajectory =
      control_msgs::action::FollowJointTrajectory;
  using TestingGoalHandleFJT =
      rclcpp_action::ClientGoalHandle<TestingFollowJointTrajectory>;
  struct TestingNoRosStartupTag {};

  Ros2ControlExecutionBackend(
      TestingNoRosStartupTag,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string group_name,
      core::IClock& clock,
      Ros2ControlExecutionBackendConfig config)
      : execution_state_sink_(std::move(execution_state_sink)),
        group_name_(std::move(group_name)),
        clock_(clock),
        config_(std::move(config)) {
    if (!execution_state_sink_) {
      throw std::invalid_argument("execution state sink must not be null");
    }
    validate_config();
    snapshot_.timestamp_ns = clock_.now();
  }

  void testing_begin_managed_fjt_terminal_session(
      std::uint64_t generation,
      std::optional<rclcpp_action::GoalUUID> goal_id) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_generation_ = generation;
    managed_fjt_terminal_goal_id_ = std::move(goal_id);
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_.reset();
  }

  void testing_on_managed_fjt_terminal_result(
      const TestingGoalHandleFJT::WrappedResult& result,
      std::uint64_t generation) {
    on_managed_fjt_terminal_result(result, generation);
  }

  bool testing_managed_fjt_terminal_received() const {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    return managed_fjt_terminal_result_received_;
  }

  bool testing_managed_fjt_terminal_success() const {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    return managed_fjt_terminal_result_success_;
  }

  bool testing_install_controlled_stop_command_reference(
      const core::TrajectoryDTO& trajectory,
      std::chrono::steady_clock::time_point started_at) {
    return install_controlled_stop_command_reference(trajectory, started_at);
  }

  bool testing_publish_controlled_stop_command_reference_snapshot(
      std::chrono::steady_clock::time_point now) {
    return publish_controlled_stop_command_reference_snapshot(now);
  }

  void testing_store_latest_joint_sample(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::chrono::steady_clock::time_point received_at) {
    testing_store_latest_joint_sample_impl(
        std::move(joint_names), std::move(positions), received_at);
  }

  void testing_sync_idle_scene_from_latest_sample() {
    sync_idle_scene_from_latest_sample();
  }

  void testing_set_running_state(bool running) {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    set_running_locked(running);
    terminal_ = !running;
  }

  void testing_store_action_feedback_sample(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::vector<double> velocities,
      std::chrono::steady_clock::time_point received_at) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!latest_action_feedback_.has_value()) {
      latest_action_feedback_.emplace();
    }
    assign_joint_sample(*latest_action_feedback_,
                        joint_names,
                        positions,
                        velocities,
                        received_at,
                        "action_feedback",
                        config_.action_name);
  }

  std::size_t testing_latest_action_feedback_position_capacity() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return latest_action_feedback_.has_value()
               ? latest_action_feedback_->positions.capacity()
               : 0;
  }
#endif

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

  struct ControlledStopCommandReference {
    core::TrajectoryDTO trajectory;
    std::chrono::steady_clock::time_point started_at;
    core::TrajectorySampleResult sample;
  };

  struct PostStopObservation {
    std::vector<double> positions;
    std::vector<double> velocities;
    std::string source;
    double max_abs_velocity_rad_s = 0.0;
    bool velocity_available = false;
    bool at_rest = false;
    std::chrono::steady_clock::time_point observed_at;
  };

  enum class SceneUpdateMode {
    kBlocking,
    kBestEffort,
  };

  struct GripperRuntime {
    Ros2GripperActionConfig config;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr joint_state_pub;
    rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_sub;
    mutable std::mutex mutex;
    GoalHandleGripper::SharedPtr active_goal_handle;
    std::string state = "idle";
    bool executing = false;
    bool goal_accepted = false;
    bool cancel_pending = false;
    std::optional<double> start_position;
    std::optional<double> current_position;
    std::optional<double> observed_position;
    std::optional<std::chrono::steady_clock::time_point> observed_at;
    std::optional<double> max_effort;
    double target_position = 0.0;
    double duration_s = 0.0;
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::chrono::steady_clock::time_point> deadline_at;
    std::vector<std::string> evidence;
    std::string evidence_state;
    bool externally_failed = false;
    std::string error;
    std::string reason_code;
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
  std::optional<JointSample> select_latest_sample_after_locked(
      std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::time_point not_before) const;
  std::optional<JointSample> wait_for_fresh_sample(double timeout_s);
  std::optional<JointSample> wait_for_fresh_sample_after(
      double timeout_s,
      std::chrono::steady_clock::time_point not_before);
  std::vector<double> velocities_for_joints(
      const JointSample& sample,
      const std::vector<std::string>& joints) const;
  std::vector<double> positions_for_joints(const JointSample& sample,
                                           const std::vector<std::string>& joints) const;
  static void assign_joint_sample(JointSample& sample,
                                  const std::vector<std::string>& joint_names,
                                  const std::vector<double>& positions,
                                  const std::vector<double>& velocities,
                                  std::chrono::steady_clock::time_point received_at,
                                  const std::string& source,
                                  const std::string& topic);
  void sync_idle_scene_from_latest_sample();
  void set_running_locked(bool running) noexcept;
  void testing_store_latest_joint_sample_impl(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::chrono::steady_clock::time_point received_at);
  bool is_commanded_sim_mode() const;
  bool uses_authored_backend_state() const;
  void set_command_gate(bool enabled);
  void publish_runtime_velocity_scale(double scale);
  void begin_controlled_stop_runtime_scale_freeze(bool assert_unit_scale);
  void assert_controlled_stop_unit_runtime_scale();
  bool end_controlled_stop_runtime_scale_freeze();
  bool install_controlled_stop_command_reference(
      const core::TrajectoryDTO& trajectory,
      std::chrono::steady_clock::time_point started_at);
  void clear_managed_controlled_stop_state();
  bool publish_controlled_stop_command_reference_snapshot(
      std::chrono::steady_clock::time_point now);
  bool publish_observed_cancel_stop_snapshot(
      const std::vector<std::string>& joint_names,
      double progress,
      std::chrono::steady_clock::time_point now);
  std::optional<PostStopObservation> wait_for_post_stop_observation(
      const std::vector<std::string>& joint_names,
      double settle_velocity_threshold_rad_s,
      std::chrono::steady_clock::time_point deadline);
  void complete_managed_controlled_stop_execution();
  core::Result<void> command_sim_initial_state(std::string phase);
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
                        std::optional<core::ErrorDTO> error = std::nullopt,
                        std::string state_source = "");
  bool publish_passive_idle_snapshot(const std::vector<double>& joint_positions);
  core::ErrorDTO operator_intervention_error(
      core::ErrorCode code,
      std::string message,
      std::string phase,
      core::ExecutionState backend_state = core::ExecutionState::kInterventionRequired,
      std::map<std::string, std::string> details = {})
      const;
  core::Result<void> resync_after_intervention_if_needed(std::string phase);
  void cancel_all_fjt_goals() noexcept;
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
  void fail_execution(std::string message,
                      core::ErrorCode code,
                      std::string reason_code,
                      std::map<std::string, std::string> details);
  void require_operator_intervention(std::string message,
                                     core::ErrorCode code = core::ErrorCode::kStopFailed);
  void require_operator_intervention(std::string message,
                                     core::ErrorCode code,
                                     std::string reason_code,
                                     std::map<std::string, std::string> details);
  void complete_execution();
  void monitor_worker(core::TrajectoryDTO trajectory);
  bool start_state_matches(const core::TrajectoryDTO& trajectory,
                           const JointSample& sample,
                           std::string& error,
                           std::map<std::string, std::string>* details = nullptr) const;
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

  std::shared_ptr<core::IExecutionStateSink> execution_state_sink_;
  std::string group_name_;
  core::IClock& clock_;
  Ros2ControlExecutionBackendConfig config_;

  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  std::thread spin_thread_;
  rclcpp_action::Client<FollowJointTrajectory>::SharedPtr action_client_;
  rclcpp::CallbackGroup::SharedPtr state_callback_group_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_sub_;
  rclcpp::Subscription<control_msgs::msg::JointTrajectoryControllerState>::SharedPtr
      controller_state_sub_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr command_gate_pub_;
  // Last value published on the command-gate topic. Read by goal-rejection
  // diagnostics so a closed gate is named as the rejection cause instead of
  // surfacing as an unexplained controller refusal.
  std::atomic<bool> command_gate_enabled_{false};
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
  std::atomic<bool> running_flag_{false};
  std::atomic<std::uint64_t> running_generation_{0};
  bool terminal_ = true;
  bool stop_requested_ = false;
  bool natural_completion_stop_requested_ = false;
  bool pause_requested_ = false;
  bool goal_accepted_ = false;
  bool first_feedback_seen_ = false;
  bool result_received_ = false;
  bool controlled_stop_preempt_in_progress_ = false;
  bool resync_required_ = false;
  bool resync_in_progress_ = false;
  GoalHandleFJT::SharedPtr active_goal_handle_;
  std::optional<rclcpp_action::GoalUUID> active_goal_id_;
  // The raw rclcpp_action result code from the last
  // discrete-trajectory goal terminal callback. Persisted so the
  // hard_deceleration controlled-stop path can distinguish a true
  // `CANCELED` (controller honored decelerate_on_cancel) from a
  // `SUCCEEDED` race where the trajectory completed normally just
  // as the stop was requested. Reset to `std::nullopt` at each new
  // dispatch.
  std::optional<rclcpp_action::ResultCode> last_action_result_code_;
  std::chrono::steady_clock::time_point execution_started_at_;
  std::optional<std::chrono::steady_clock::time_point> stop_requested_at_;
  std::optional<std::chrono::steady_clock::time_point> tracking_error_since_;
  std::optional<TrackingErrorPeak> peak_tracking_error_;

  mutable std::mutex gripper_map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<GripperRuntime>> grippers_;

  // Managed FollowJointTrajectory preempt/controlled-stop substrate. Used by
  // the motion-interrupt controlled-stop paths (spring_back_hold /
  // soft_on_path) to dispatch preempting replacement goals and track their
  // terminal result.
  bool send_fjt_replacement_trajectory(
      const core::TrajectoryDTO& trajectory,
      bool track_terminal_goal = false);
  void begin_controlled_stop_path_deviation_tracking(
      std::shared_ptr<const core::TrajectoryDTO> accepted_trajectory,
      double initial_path_deviation_rad);
  void record_controlled_stop_path_deviation_feedback(
      const std::vector<std::string>& joint_names,
      const std::vector<double>& positions);
  void clear_controlled_stop_path_deviation_tracking();
  void on_managed_fjt_terminal_result(
      const GoalHandleFJT::WrappedResult& result,
      std::uint64_t generation);

  mutable std::mutex managed_fjt_mutex_;
  std::condition_variable managed_fjt_cv_;
  // ADR-0018: runtime velocity scale read by the monitor loop.
  std::atomic<double> runtime_velocity_scale_{1.0};
  std::atomic<double> applied_runtime_velocity_scale_{1.0};
  mutable std::mutex runtime_velocity_scale_mutex_;
  int controlled_stop_runtime_scale_freeze_depth_ = 0;
  bool controlled_stop_unit_scale_asserted_ = false;
  std::optional<double> controlled_stop_runtime_scale_restore_;
  std::optional<double> deferred_runtime_velocity_scale_;
  std::shared_ptr<const core::TrajectoryDTO> active_trajectory_;
  std::shared_ptr<ControlledStopCommandReference>
      controlled_stop_command_reference_;
  std::uint64_t idle_scene_sync_generation_ = 0;
  std::optional<std::chrono::steady_clock::time_point>
      last_idle_scene_sample_received_at_;
  std::chrono::steady_clock::time_point last_idle_scene_projection_at_{};
  double active_trajectory_time_s_ = 0.0;
  double active_runtime_velocity_scale_ = 1.0;
  // The most-recent managed FJT goal handle issued by a controlled-stop
  // preempt. Subsequent dispatches preempt it via async_send_goal; the
  // on_managed_fjt_terminal_result callback handles its terminal state.
  GoalHandleFJT::SharedPtr managed_fjt_goal_;
  std::uint64_t managed_fjt_generation_ = 0;
  std::optional<rclcpp_action::GoalUUID> managed_fjt_terminal_goal_id_;
  bool managed_fjt_terminal_result_received_ = true;
  bool managed_fjt_terminal_result_success_ = true;
  std::optional<core::ErrorDTO> managed_fjt_terminal_error_;
  std::shared_ptr<const core::TrajectoryDTO>
      controlled_stop_path_deviation_reference_;
  std::atomic<bool> controlled_stop_path_deviation_tracking_active_{false};
  std::optional<double> controlled_stop_path_deviation_max_rad_;
  std::size_t controlled_stop_path_deviation_feedback_samples_ = 0;
  std::optional<core::ErrorDTO> controlled_stop_path_deviation_error_;
};

}  // namespace manyforge::ros2_control_execution
