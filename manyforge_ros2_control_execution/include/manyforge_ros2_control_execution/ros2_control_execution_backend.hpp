#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/list_controllers.hpp>
#include <controller_manager_msgs/srv/list_hardware_components.hpp>
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
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/trajectory_progress_projection.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>

namespace manyforge::ros2_control_execution {

namespace detail {
class SceneProjectionWorker;
struct SceneProjectionCompletion;
struct SceneProjectionWorkerDeleter {
  void operator()(SceneProjectionWorker* worker) const noexcept;
};
}  // namespace detail

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

struct Ros2ControlSceneProjectionMetrics {
  std::uint64_t misses_total = 0;
  std::uint64_t successful_updates_total = 0;
  std::uint64_t lag_max_ns = 0;
  std::int64_t last_success_timestamp_ns = 0;
  std::uint64_t last_success_age_ns = 0;
  std::uint64_t terminal_retries_scheduled_total = 0;
  std::uint64_t terminal_retry_successes_total = 0;
  std::uint64_t terminal_retry_exhausted_total = 0;
  std::uint64_t active_requests_total = 0;
  std::uint64_t joint_requests_total = 0;
  std::uint64_t requests_coalesced_total = 0;
  std::uint64_t requests_discarded_total = 0;
  std::uint64_t requests_rejected_while_suspended_total = 0;
  std::uint64_t completion_callback_exceptions_total = 0;
  std::uint64_t apply_duration_max_ns = 0;
  std::uint64_t queue_lag_max_ns = 0;
};

struct Ros2ControlControllerProgressMetrics {
  std::uint64_t executor_thread_count = 0;
  std::uint64_t state_callbacks_total = 0;
  std::uint64_t action_feedback_callbacks_total = 0;
  std::uint64_t action_result_callbacks_total = 0;
  std::uint64_t command_progress_samples_total = 0;
  std::uint64_t cancel_confirmations_total = 0;
  std::uint64_t stop_confirmations_total = 0;
  std::uint64_t latest_state_sample_age_ns = 0;
  std::uint64_t last_command_progress_latency_ns = 0;
  std::uint64_t max_command_progress_latency_ns = 0;
  std::uint64_t last_action_result_latency_ns = 0;
  std::uint64_t max_action_result_latency_ns = 0;
  std::uint64_t last_cancel_latency_ns = 0;
  std::uint64_t max_cancel_latency_ns = 0;
  std::uint64_t last_stop_confirmation_latency_ns = 0;
  std::uint64_t max_stop_confirmation_latency_ns = 0;
  std::uint64_t observed_state_reader_retry_exhausted_total = 0;
  std::uint64_t observed_state_published_total = 0;
  std::uint64_t observed_state_rejected_total = 0;
  std::uint64_t observed_state_writer_busy_total = 0;
  std::uint64_t observed_state_slot_exhausted_total = 0;
  std::uint64_t authored_active_publications_total = 0;
  std::uint64_t authored_active_interarrival_max_ns = 0;
  std::uint64_t jtc_cancel_reference_publications_total = 0;
  std::uint64_t jtc_cancel_reference_rejections_total = 0;
  std::uint64_t controller_binding_activity_events_total = 0;
  std::uint64_t controller_binding_refresh_attempts_total = 0;
  std::uint64_t controller_binding_refresh_successes_total = 0;
  std::uint64_t controller_binding_refresh_failures_total = 0;
  std::uint64_t controller_binding_dispatch_rejections_total = 0;
  std::uint64_t execution_admission_dispatch_rejections_total = 0;
  std::uint64_t dispatch_authority_losses_total = 0;
  std::vector<core::ObservedRobotStateSourceMetrics> observed_state_sources;
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
/// - dispatch_admission_mutex_ may be taken before execution_mutex_ only at
///   the bounded pre-dispatch linearization point. No path may take
///   dispatch_admission_mutex_ while holding execution_mutex_.
/// - The scoped JTC cancel-reference writer may take execution_mutex_ then the
///   authored-state publication leaf lock; the leaf lock never takes
///   execution_mutex_.
/// - Do not hold gripper_map_mutex_ while taking execution_mutex_ or state_mutex_.
/// - monitor/callback paths must use best-effort execution-state sink updates
///   so long validation sweeps cannot block reset/stop completion.
/// - Per-GripperRuntime::mutex is taken only after gripper_map_mutex_ has been
///   released, using a copied shared_ptr to the runtime.
/// - A gripper runtime mutex may call only the sink's kBestEffort projection;
///   that call uses try_lock and no scene/sink path may call back into a
///   GripperRuntime. This linearizes generation invalidation with projection
///   without introducing a blocking scene-lock dependency.
class Ros2ControlExecutionBackend final
    : public core::IExecutionBackend,
      public core::IGripperCommandBackend {
 public:
  struct DeferredRosExecutorStartTag {};

  Ros2ControlExecutionBackend(std::shared_ptr<core::IExecutionStateSink>
                                  execution_state_sink,
                              std::string group_name,
                              core::IClock& clock,
                              core::IScheduler& scheduler,
                              Ros2ControlExecutionBackendConfig config);
  Ros2ControlExecutionBackend(
      DeferredRosExecutorStartTag,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string group_name,
      core::IClock& clock,
      core::IScheduler& scheduler,
      Ros2ControlExecutionBackendConfig config);
  ~Ros2ControlExecutionBackend() override;

  // Deployment-owned ROS entities, including gripper transports, must be
  // installed before this one-way transition. Mutating the ROS graph while the
  // executor spins is rejected because rclcpp/rcutils graph construction is not
  // a runtime-safe reconfiguration boundary.
  void complete_ros_configuration();

  core::ExecutionHandle execute_trajectory(const core::TrajectoryDTO& trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO& trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const override;
  const core::ObservedRobotStatePublication*
  observed_robot_state_publication() const noexcept override {
    return observed_state_publication_.get();
  }
  const core::ExecutionProgressPublication*
  execution_progress_publication() const noexcept override {
    return execution_progress_publication_.get();
  }
  void retire_execution_progress_generation(
      std::uint64_t execution_generation) noexcept override {
    if (execution_progress_publication_) {
      (void)execution_progress_publication_->invalidate_generation(
          execution_generation);
    }
  }
  core::ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t execution_generation) noexcept override {
    return execution_progress_publication_
               ? execution_progress_publication_->claim_hazard_authority(
                     execution_generation)
               : core::ExecutionProgressHazardClaimResult::
                     kGenerationMismatch;
  }
  const std::atomic<std::uint64_t>* authored_state_authority_epoch()
      const noexcept {
    return uses_authored_backend_state() ? &authored_state_authority_epoch_
                                         : nullptr;
  }

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
  core::GripperCommandResultDTO command_gripper(
      const core::GripperCommandRequestDTO& request) override;
  core::GripperCommandResultDTO mark_gripper_grasp_lost(
      const std::string& gripper_id,
      std::uint64_t expected_command_id,
      std::uint64_t expected_generation,
      const std::string& evidence_label,
      const std::string& message,
      const std::string& reason_code = "grasp_confirmation_lost") override;
  core::GripperCommandStatusDTO get_gripper_status(
      const std::string& gripper_id) const override;
  void invalidate_gripper_commands(
      core::GripperInvalidationReason reason) noexcept override;
  void authorize_gripper_commands() noexcept override;

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
  Ros2ControlSceneProjectionMetrics scene_projection_metrics() const;
  Ros2ControlControllerProgressMetrics controller_progress_metrics() const;

  /// Publish the latest robot-family-neutral lifecycle admission. The
  /// lifecycle owner calls this from NRT work; dispatch and runtime monitoring
  /// only acquire an immutable snapshot.
  bool publish_execution_admission_snapshot(
      ExecutionAdmissionSnapshot snapshot);
  std::optional<ExecutionAdmissionSnapshot>
  execution_admission_snapshot() const;
  std::optional<ControllerBindingSnapshot>
  controller_binding_snapshot() const;

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
      core::IScheduler& scheduler,
      Ros2ControlExecutionBackendConfig config)
      : execution_state_sink_(std::move(execution_state_sink)),
        group_name_(std::move(group_name)),
        clock_(clock),
        scheduler_(scheduler),
        config_(std::move(config)) {
    if (!execution_state_sink_) {
      throw std::invalid_argument("execution state sink must not be null");
    }
    validate_config();
    initialize_observed_state_publication();
    initialize_execution_progress_publication();
    initialize_scene_projection_worker();
    initialize_dispatch_admission();
    snapshot_.timestamp_ns = clock_.now();
    initialize_authored_state();
  }

  void testing_begin_managed_fjt_terminal_session(
      std::uint64_t generation,
      std::optional<rclcpp_action::GoalUUID> goal_id) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_generation_ = generation;
    managed_fjt_terminal_goal_id_ = std::move(goal_id);
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_result_received_at_.reset();
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
      std::chrono::steady_clock::time_point received_at,
      std::vector<double> velocities = {},
      std::optional<std::int64_t> received_timestamp_ns = std::nullopt) {
    testing_store_latest_joint_sample_impl(
        std::move(joint_names), std::move(positions), received_at,
        std::move(velocities), received_timestamp_ns);
  }

  void testing_store_latest_controller_sample(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::chrono::steady_clock::time_point received_at,
      std::vector<double> velocities = {},
      std::optional<std::int64_t> received_timestamp_ns = std::nullopt) {
    testing_store_latest_controller_sample_impl(
        std::move(joint_names), std::move(positions), received_at,
        std::move(velocities), received_timestamp_ns);
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
      const std::vector<std::string>& joint_names,
      const std::vector<double>& positions,
      const std::vector<double>& velocities,
      std::chrono::steady_clock::time_point received_at) {
    const auto received_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 received_at.time_since_epoch())
                                 .count();
    publish_observed_robot_state(
        core::ObservedRobotStateSourceKind::kActionFeedback, joint_names,
        positions, velocities, 0, received_ns);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!latest_action_feedback_.has_value()) {
      latest_action_feedback_.emplace();
    }
    assign_joint_sample(*latest_action_feedback_,
                        joint_names,
                        positions,
                        velocities,
                        received_at,
                        clock_.now(),
                        "action_feedback",
                        config_.action_name);
    const auto authority_generation =
        active_action_feedback_authority_generation_.load(
            std::memory_order_acquire);
    if (authority_generation != 0) {
      latest_action_feedback_->execution_generation =
          authority_generation;
      latest_action_feedback_->goal_id = rclcpp_action::GoalUUID{};
    }
  }

  void testing_set_action_feedback_authority(bool active) {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (active && !running_) {
      set_running_locked(true);
      terminal_ = false;
    }
    active_action_feedback_authority_generation_.store(
        active ? running_generation_.load(std::memory_order_acquire) : 0,
        std::memory_order_release);
  }

  std::optional<std::string> testing_selected_sample_source(
      std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto* sample = select_latest_sample_ptr_locked(now);
    return sample != nullptr
               ? std::optional<std::string>(sample->source)
               : std::nullopt;
  }

  std::size_t testing_latest_action_feedback_position_capacity() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return latest_action_feedback_.has_value()
               ? latest_action_feedback_->positions.capacity()
               : 0;
  }

  void testing_complete_execution() {
    testing_complete_execution_impl();
  }

  void testing_schedule_active_scene_projection(
      std::vector<double> positions,
      std::int64_t source_timestamp_ns = 0) {
    (void)schedule_active_scene_projection(std::move(positions),
                                           source_timestamp_ns);
  }

  bool testing_drain_scene_projection_for(std::chrono::nanoseconds timeout) {
    return drain_scene_projection_for(timeout);
  }

  void testing_record_command_progress_latency_ns(std::int64_t latency_ns) {
    record_command_progress_latency_ns(latency_ns);
  }

  void testing_begin_command_progress_metrics_publication(
      std::int64_t latency_ns) {
    begin_command_progress_metrics_publication(latency_ns);
  }

  void testing_finish_command_progress_metrics_publication() {
    finish_command_progress_metrics_publication();
  }

  bool testing_publish_snapshot(core::ExecutionState state,
                                const std::vector<double>& positions) {
    return publish_snapshot(state, 0.0, positions);
  }

  bool testing_publish_snapshot(
      core::ExecutionState state,
      double progress,
      const std::vector<double>& positions,
      std::optional<core::ErrorDTO> error) {
    return publish_snapshot(state, progress, positions, std::move(error));
  }

  std::uint64_t testing_running_generation() const {
    return running_generation_.load(std::memory_order_acquire);
  }

  bool testing_completion_gate_pending() const {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    return completion_gate_request_.has_value();
  }

  bool testing_publish_passive_observed_snapshot(
      const std::vector<double>& positions,
      const std::vector<double>& velocities,
      std::int64_t source_timestamp_ns,
      std::uint64_t expected_running_generation) {
    return publish_passive_observed_snapshot(
        positions, velocities, source_timestamp_ns,
        expected_running_generation);
  }

  bool testing_publish_snapshot_for_generation(
      core::ExecutionState state,
      const std::vector<double>& positions,
      std::uint64_t expected_running_generation) {
    return publish_snapshot(state, 0.0, positions, std::nullopt, "",
                            expected_running_generation);
  }

  bool testing_action_progress_has_dedicated_callback_group() const {
    return action_callback_group_ && state_callback_group_ &&
           action_callback_group_.get() != state_callback_group_.get();
  }

  bool testing_wait_for_managed_terminal_or_stop(
      std::stop_token stop_token,
      std::chrono::nanoseconds timeout) {
    std::unique_lock<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_received_at_.reset();
    return managed_fjt_cv_.wait_for(
        lock, stop_token, timeout,
        [this]() { return managed_fjt_terminal_result_received_; });
  }

  std::optional<std::string>
  testing_wait_for_fresh_passive_sample_after(
      std::chrono::steady_clock::time_point not_before,
      std::chrono::nanoseconds timeout,
      std::stop_token stop_token = {}) {
    const auto sample = wait_for_fresh_passive_sample_after(
        not_before, std::chrono::steady_clock::now() + timeout, stop_token);
    return sample.has_value()
               ? std::optional<std::string>(sample->source)
               : std::nullopt;
  }

  std::uint64_t testing_begin_jtc_cancel_reference_authority() {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    return begin_jtc_cancel_reference_authority_locked();
  }

  void testing_end_jtc_cancel_reference_authority(std::uint64_t token) {
    end_jtc_cancel_reference_authority(token);
  }

  bool testing_publish_jtc_cancel_reference(
      const std::vector<std::string>& joint_names,
      const std::vector<double>& positions,
      const std::vector<double>& velocities,
      std::int64_t received_timestamp_ns,
      std::uint64_t expected_token) {
    return publish_jtc_cancel_reference(
        joint_names, positions, velocities, received_timestamp_ns,
        expected_token);
  }

  void testing_publish_controller_binding_snapshot(
      ControllerBindingSnapshot snapshot) {
    publish_controller_binding_snapshot(std::move(snapshot));
  }

  std::optional<DispatchAuthorityPin> testing_dispatch_authority_pin() const {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    return active_dispatch_authority_;
  }

  std::optional<DispatchAuthorityPin> testing_planning_environment_pin() const {
    std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
    return planning_environment_authority_;
  }

  std::optional<DispatchAuthorityPin> testing_resumable_environment_pin() const {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    return resumable_dispatch_authority_;
  }

  const std::string& execution_backend_component_epoch() const noexcept {
    return execution_backend_component_epoch_;
  }
#endif

 private:
  enum class SnapshotPublicationAuthority {
    kBackendAuthored,
    kMeasuredDiagnostic,
  };

  using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
  using GoalHandleFJT = rclcpp_action::ClientGoalHandle<FollowJointTrajectory>;
  using GripperCommand = control_msgs::action::GripperCommand;
  using GoalHandleGripper = rclcpp_action::ClientGoalHandle<GripperCommand>;

  struct JointSample {
    std::vector<std::string> joint_names;
    std::vector<double> positions;
    std::vector<double> velocities;
    std::chrono::steady_clock::time_point received_at;
    std::int64_t received_timestamp_ns = 0;
    std::string source;
    std::string topic;
    // Only action feedback is goal-scoped. Passive controller/joint samples
    // deliberately leave these empty. A feedback sample may be selected only
    // while both values still identify the active execution.
    std::optional<std::uint64_t> execution_generation;
    std::optional<rclcpp_action::GoalUUID> goal_id;
  };

  struct CompletionGateRequest {
    std::uint64_t execution_generation = 0;
    rclcpp_action::GoalUUID goal_id{};
    std::chrono::steady_clock::time_point result_received_at;
    std::shared_ptr<const core::TrajectoryDTO> trajectory;
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
    std::string topic;
    std::int64_t received_timestamp_ns = 0;
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
    std::uint64_t command_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    double progress = 0.0;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    std::uint64_t lifecycle_schedule_generation = 0;
    std::optional<double> start_position;
    std::optional<double> current_position;
    std::optional<double> observed_position;
    std::optional<std::chrono::steady_clock::time_point> observed_at;
    std::optional<double> max_effort;
    double target_position = 0.0;
    double duration_s = 0.0;
    core::TimePoint started_at_ns = 0;
    std::optional<core::TimePoint> deadline_at_ns;
    std::string commanded_state;
    std::string actuation_kind;
    std::string state_authority;
    std::string completion_policy;
    std::string grasp_confirmation_policy;
    std::string transport_kind;
    core::GripperProjectionState projection_state =
        core::GripperProjectionState::kNotRequired;
    std::string projection_reason;
    int projection_retry_count = 0;
    std::optional<std::size_t> scene_projection_slot;
    std::uint64_t scene_projection_request_id = 0;
    std::vector<std::string> evidence;
    std::string evidence_state;
    bool externally_failed = false;
    std::string error;
    std::string reason_code;
  };

  void validate_config() const;
  void initialize_observed_state_publication();
  void initialize_execution_progress_publication();
  void initialize_scene_projection_worker();
  void initialize_dispatch_admission();
  void initialize_authored_state();
  void publish_observed_robot_state(
      core::ObservedRobotStateSourceKind source,
      const std::vector<std::string>& joint_names,
      const std::vector<double>& positions,
      const std::vector<double>& velocities,
      std::int64_t source_timestamp_ns,
      std::int64_t received_timestamp_ns) noexcept;
  bool publish_authored_robot_state(
      std::span<const double> positions,
      std::span<const double> velocities,
      std::int64_t timestamp_ns,
      std::optional<std::uint64_t> expected_running_generation,
      bool invalidate_before_publish) noexcept;
  Ros2ControlExecutionBackend(
      bool defer_ros_executor_start,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string group_name,
      core::IClock& clock,
      core::IScheduler& scheduler,
      Ros2ControlExecutionBackendConfig config);
  void initialize_ros();
  void stop_ros();
  void start_controller_binding_monitor();
  void stop_controller_binding_monitor() noexcept;
  void controller_binding_monitor_worker(std::stop_token stop_token);
  void request_controller_binding_refresh();
  void on_controller_manager_activity(
      const controller_manager_msgs::msg::ControllerManagerActivity::SharedPtr
          msg);
  void publish_controller_binding_snapshot(
      ControllerBindingSnapshot snapshot);
  void invalidate_controller_binding(std::string reason_code);
  DispatchAdmissionDecision current_dispatch_admission(
      const std::vector<std::string>& trajectory_joint_names) const;
  DispatchAdmissionDecision current_active_dispatch_authority(
      const DispatchAuthorityPin& pin) const;
  core::Result<void> capture_planning_environment_authority();
  bool install_dispatch_authority_pin(
      const DispatchAuthorityPin& pin,
      std::uint64_t dispatch_generation);
  void fail_active_dispatch_authority(
      DispatchAdmissionDecision decision);

  void on_joint_state(const sensor_msgs::msg::JointState::SharedPtr msg);
  void on_controller_state(
      const control_msgs::msg::JointTrajectoryControllerState::SharedPtr msg);
  std::uint64_t begin_jtc_cancel_reference_authority_locked() noexcept;
  void end_jtc_cancel_reference_authority(std::uint64_t token) noexcept;
  bool publish_jtc_cancel_reference(
      const std::vector<std::string>& joint_names,
      const std::vector<double>& positions,
      const std::vector<double>& velocities,
      std::int64_t received_timestamp_ns,
      std::uint64_t expected_token) noexcept;
  void on_action_feedback(
      GoalHandleFJT::SharedPtr,
      const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback);
  void on_action_result(const GoalHandleFJT::WrappedResult& result,
                        std::uint64_t execution_generation);

  std::optional<JointSample> select_latest_sample_locked(
      std::chrono::steady_clock::time_point now) const;
  const JointSample* select_latest_sample_ptr_locked(
      std::chrono::steady_clock::time_point now) const;
  const JointSample* select_latest_passive_sample_ptr_locked(
      std::chrono::steady_clock::time_point now) const;
  std::optional<JointSample> select_latest_passive_sample_after_locked(
      std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::time_point not_before) const;
  bool joint_sample_is_complete_and_finite(
      const JointSample& sample) const noexcept;
  bool joint_sample_is_fresh_and_valid(
      const JointSample& sample,
      std::chrono::steady_clock::time_point now,
      std::optional<std::chrono::steady_clock::time_point> not_before =
          std::nullopt) const noexcept;
  bool action_sample_matches_active_execution_locked(
      const JointSample& sample) const noexcept;
  std::optional<JointSample> wait_for_fresh_passive_sample(double timeout_s);
  std::optional<JointSample> wait_for_fresh_passive_sample_after(
      std::chrono::steady_clock::time_point not_before,
      std::chrono::steady_clock::time_point deadline,
      std::stop_token stop_token = {});
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
                                  std::int64_t received_timestamp_ns,
                                  const std::string& source,
                                  const std::string& topic);
  void sync_idle_scene_from_latest_sample();
  enum class ProgressRetirement {
    kRetireImmediately,
    kPreserveTerminalHandoff,
  };
  enum class ResumeAuthorityRetention {
    kDiscard,
    kPreserve,
  };
  void set_running_locked(
      bool running,
      ProgressRetirement progress_retirement =
          ProgressRetirement::kRetireImmediately,
      ResumeAuthorityRetention resume_authority_retention =
          ResumeAuthorityRetention::kDiscard) noexcept;
  void testing_store_latest_joint_sample_impl(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::chrono::steady_clock::time_point received_at,
      std::vector<double> velocities,
      std::optional<std::int64_t> received_timestamp_ns);
  void testing_store_latest_controller_sample_impl(
      std::vector<std::string> joint_names,
      std::vector<double> positions,
      std::chrono::steady_clock::time_point received_at,
      std::vector<double> velocities,
      std::optional<std::int64_t> received_timestamp_ns);
  void testing_complete_execution_impl();
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
      std::chrono::steady_clock::time_point now,
      std::optional<std::uint64_t> expected_running_generation = std::nullopt);
  bool publish_observed_cancel_stop_snapshot(
      const std::vector<std::string>& joint_names,
      double progress,
      std::chrono::steady_clock::time_point now,
      std::optional<std::uint64_t> expected_running_generation = std::nullopt);
  std::optional<PostStopObservation> wait_for_post_stop_observation(
      const std::vector<std::string>& joint_names,
      double settle_velocity_threshold_rad_s,
      std::chrono::steady_clock::time_point not_before,
      std::chrono::steady_clock::time_point deadline,
      std::stop_token stop_token = {});
  std::uint64_t complete_managed_controlled_stop_execution();
  core::Result<void> command_sim_initial_state(std::string phase);
  bool update_scene_positions(
      const std::vector<double>& joint_positions,
      SceneUpdateMode mode = SceneUpdateMode::kBlocking,
      std::int64_t source_timestamp_ns = 0);
  void project_terminal_scene_positions(
      const std::vector<double>& joint_positions,
      std::int64_t source_timestamp_ns);
  bool schedule_active_scene_projection(
      std::vector<double> joint_positions,
      std::int64_t source_timestamp_ns);
  void stop_scene_projection_worker() noexcept;
  bool drain_scene_projection_for(std::chrono::nanoseconds timeout);
  void record_scene_projection_success(std::int64_t source_timestamp_ns);
  void record_scene_projection_miss();
  void record_command_progress_latency_ns(std::int64_t latency_ns);
  void begin_command_progress_metrics_publication(std::int64_t latency_ns);
  void finish_command_progress_metrics_publication();
  void record_action_result_latency_ns(std::int64_t latency_ns);
  void record_cancel_latency_ns(std::int64_t latency_ns);
  void record_stop_confirmation_latency_ns(std::int64_t latency_ns);
  std::optional<double> scene_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const;
  bool schedule_gripper_scene_projection_locked(
      const std::shared_ptr<GripperRuntime>& runtime,
      double position,
      bool terminal);
  void on_gripper_scene_projection_complete(
      const std::weak_ptr<GripperRuntime>& runtime,
      const detail::SceneProjectionCompletion& completion) noexcept;
  bool publish_snapshot(
      core::ExecutionState state,
      double progress,
      const std::vector<double>& joint_positions,
      std::optional<core::ErrorDTO> error = std::nullopt,
      std::string state_source = "",
      std::optional<std::uint64_t> expected_running_generation = std::nullopt,
      SnapshotPublicationAuthority authority =
          SnapshotPublicationAuthority::kBackendAuthored,
      const std::vector<double>* authored_velocities = nullptr);
  bool publish_passive_observed_snapshot(
      const std::vector<double>& joint_positions,
      const std::vector<double>& joint_velocities,
      std::int64_t source_timestamp_ns,
      std::uint64_t expected_running_generation);
  bool publish_observed_snapshot(
      core::ExecutionState state,
      double progress,
      const std::vector<double>& joint_positions,
      const std::vector<double>& joint_velocities,
      std::int64_t source_timestamp_ns,
      std::optional<core::ErrorDTO> error,
      std::uint64_t expected_running_generation);
  bool publish_observed_metadata(
      core::ExecutionState state,
      double progress,
      std::optional<core::ErrorDTO> error,
      std::uint64_t expected_running_generation);
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
  void commit_action_result_terminal(
      const rclcpp_action::GoalUUID& goal_id,
      std::uint64_t execution_generation,
      bool canceled,
      std::optional<core::ErrorDTO> failure,
      std::chrono::steady_clock::time_point result_received_at);
  DispatchAdmissionDecision current_active_dispatch_authority_locked(
      const DispatchAuthorityPin& pin) const;
  void require_operator_intervention(std::string message,
                                     core::ErrorCode code = core::ErrorCode::kStopFailed);
  void require_operator_intervention(std::string message,
                                     core::ErrorCode code,
                                     std::string reason_code,
                                     std::map<std::string, std::string> details);
  void complete_execution(const JointSample* verified_observed_sample);
  bool completion_gate_matches_locked(
      const CompletionGateRequest& request) const;
  void process_completion_gate(const CompletionGateRequest& request);
  void commit_completion_gate_terminal(
      const CompletionGateRequest& request,
      std::optional<JointSample> verified_observed_sample,
      std::optional<core::ErrorDTO> failure);
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
      std::uint64_t command_id,
      std::uint64_t generation,
      GoalHandleGripper::SharedPtr,
      const std::shared_ptr<const GripperCommand::Feedback> feedback);
  void on_gripper_result(
      const std::shared_ptr<GripperRuntime>& runtime,
      std::uint64_t command_id,
      std::uint64_t generation,
      const GoalHandleGripper::WrappedResult& result);
  void cancel_all_gripper_commands();
  bool schedule_gripper_lifecycle(
      const std::shared_ptr<GripperRuntime>& runtime,
      std::uint64_t command_id,
      std::uint64_t generation,
      core::TimePoint deadline_ns);
  void advance_gripper_command(
      const std::shared_ptr<GripperRuntime>& runtime,
      std::uint64_t command_id,
      std::uint64_t generation,
      std::uint64_t schedule_generation);

  std::shared_ptr<core::IExecutionStateSink> execution_state_sink_;
  std::string group_name_;
  core::IClock& clock_;
  core::IScheduler& scheduler_;
  Ros2ControlExecutionBackendConfig config_;
  const std::string execution_backend_component_epoch_;
  std::unique_ptr<core::ObservedRobotStatePublication>
      observed_state_publication_;
  std::unique_ptr<core::ExecutionProgressPublication>
      execution_progress_publication_;
  // Command/predicted authority has one backend-owned writer domain. This leaf
  // lock is never used by ROS measured-state ingress or by publication readers;
  // it only orders the monitor and lifecycle terminal writers so an old monitor
  // tick cannot overwrite a newer terminal command reference.
  mutable std::mutex authored_state_publication_mutex_;
  std::vector<double> authored_at_rest_velocities_;
  // Odd epochs are invalid. Lifecycle/terminal replacement moves through an
  // odd epoch and only successful publication commits the next even epoch.
  std::atomic<std::uint64_t> authored_state_authority_epoch_{1};

  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  mutable std::mutex ros_configuration_mutex_;
  bool ros_configuration_complete_ = false;
  std::thread spin_thread_;
  rclcpp_action::Client<FollowJointTrajectory>::SharedPtr action_client_;
  rclcpp::CallbackGroup::SharedPtr action_callback_group_;
  rclcpp::CallbackGroup::SharedPtr state_callback_group_;
  rclcpp::CallbackGroup::SharedPtr binding_callback_group_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_sub_;
  rclcpp::Subscription<control_msgs::msg::JointTrajectoryControllerState>::SharedPtr
      controller_state_sub_;
  rclcpp::Subscription<
      controller_manager_msgs::msg::ControllerManagerActivity>::SharedPtr
      controller_manager_activity_sub_;
  rclcpp::Client<controller_manager_msgs::srv::ListControllers>::SharedPtr
      list_controllers_client_;
  rclcpp::Client<
      controller_manager_msgs::srv::ListHardwareComponents>::SharedPtr
      list_hardware_components_client_;
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
  std::condition_variable_any state_cv_;
  std::optional<JointSample> latest_joint_state_;
  std::optional<JointSample> latest_controller_state_;
  std::optional<JointSample> latest_action_feedback_;
  std::optional<JointSample> latest_action_feedback_error_;
  core::ExecutionSnapshot snapshot_;

  mutable std::mutex execution_mutex_;
  std::condition_variable_any execution_cv_;
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
  std::optional<CompletionGateRequest> completion_gate_request_;
  bool controlled_stop_preempt_in_progress_ = false;
  // While JTC owns a qualified `decelerate_on_cancel`, commanded-simulation
  // authority follows JointTrajectoryControllerState.reference. The scoped
  // token is generation-like: zero is inactive and every delegation gets a
  // distinct value, so a callback prepared for an older stop cannot publish
  // into a later stop. Access that crosses publication is serialized by
  // execution_mutex_ (execution -> authored-state leaf lock).
  std::atomic<std::uint64_t> jtc_cancel_reference_authority_token_{0};
  std::uint64_t next_jtc_cancel_reference_authority_token_ = 1;
  bool resync_required_ = false;
  bool resync_in_progress_ = false;
  GoalHandleFJT::SharedPtr active_goal_handle_;
  std::optional<rclcpp_action::GoalUUID> active_goal_id_;
  // Exact active-goal feedback authority. Zero is retired. The value is the
  // owning execution generation, so state-only selectors can fence a retired
  // goal without reading active_goal_id_ outside execution_mutex_.
  std::atomic<std::uint64_t>
      active_action_feedback_authority_generation_{0};
  // The raw rclcpp_action result code from the last
  // discrete-trajectory goal terminal callback. Persisted so the
  // hard_deceleration controlled-stop path can distinguish a true
  // `CANCELED` (controller honored decelerate_on_cancel) from a
  // `SUCCEEDED` race where the trajectory completed normally just
  // as the stop was requested. Reset to `std::nullopt` at each new
  // dispatch.
  std::optional<rclcpp_action::ResultCode> last_action_result_code_;
  std::chrono::steady_clock::time_point execution_started_at_;
  std::chrono::steady_clock::time_point active_goal_accepted_at_;
  std::optional<std::chrono::steady_clock::time_point> stop_requested_at_;
  std::optional<std::chrono::steady_clock::time_point> tracking_error_since_;
  std::optional<TrackingErrorPeak> peak_tracking_error_;
  std::optional<DispatchAuthorityPin> active_dispatch_authority_;
  // The original dispatch environment survives only a successful Pause. A
  // resumed trajectory is freshly planned, but it may not cross a component
  // replacement boundary and silently inherit the prior execution.
  std::optional<DispatchAuthorityPin> resumable_dispatch_authority_;

  // Phase 2.7A binding/admission publications. Publishers serialize with the
  // bounded dispatch enqueue through this NRT mutex; runtime readers acquire
  // immutable shared snapshots without controller-manager calls.
  mutable std::mutex dispatch_admission_mutex_;
  std::atomic<std::shared_ptr<const ControllerBindingSnapshot>>
      controller_binding_snapshot_{nullptr};
  std::atomic<std::shared_ptr<const ExecutionAdmissionSnapshot>>
      execution_admission_snapshot_{nullptr};
  // Captured only after the measured pre-planning state barrier succeeds.
  // Final dispatch must observe the same semantic component epochs.
  std::optional<DispatchAuthorityPin> planning_environment_authority_;
  std::atomic<std::uint64_t> next_controller_binding_snapshot_generation_{1};
  std::atomic<std::uint64_t> controller_binding_activity_generation_{0};
  mutable std::mutex controller_binding_monitor_mutex_;
  std::condition_variable_any controller_binding_monitor_cv_;
  bool controller_binding_refresh_requested_ = false;
  std::jthread controller_binding_monitor_thread_;

  // Authority serializes admission/dispatch against reset, stop, and runtime
  // replacement without holding lifecycle or scene locks across ROS waits.
  // Lock order is authority -> map -> per-runtime; callbacks only take the
  // per-runtime leaf lock.
  mutable std::mutex gripper_authority_mutex_;
  std::uint64_t gripper_authority_generation_ = 1;
  bool gripper_admission_blocked_ = false;
  mutable std::mutex gripper_map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<GripperRuntime>> grippers_;
  std::atomic<std::uint64_t> next_gripper_command_id_{1};
  std::atomic<std::uint64_t> next_gripper_generation_{1};

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
  std::condition_variable_any managed_fjt_cv_;
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
  std::chrono::steady_clock::time_point managed_fjt_goal_accepted_at_{};
  bool managed_fjt_terminal_result_received_ = true;
  bool managed_fjt_terminal_result_success_ = true;
  std::optional<std::chrono::steady_clock::time_point>
      managed_fjt_terminal_result_received_at_;
  std::optional<core::ErrorDTO> managed_fjt_terminal_error_;
  std::shared_ptr<const core::TrajectoryDTO>
      controlled_stop_path_deviation_reference_;
  std::atomic<bool> controlled_stop_path_deviation_tracking_active_{false};
  std::optional<double> controlled_stop_path_deviation_max_rad_;
  std::size_t controlled_stop_path_deviation_feedback_samples_ = 0;
  std::optional<core::ErrorDTO> controlled_stop_path_deviation_error_;
  std::atomic<std::uint64_t> scene_projection_misses_total_{0};
  std::atomic<std::uint64_t> scene_projection_successes_total_{0};
  std::atomic<std::uint64_t> scene_projection_lag_max_ns_{0};
  std::atomic<std::int64_t> scene_projection_last_success_timestamp_ns_{0};
  std::atomic<std::uint64_t> state_callbacks_total_{0};
  std::atomic<std::uint64_t> action_feedback_callbacks_total_{0};
  std::atomic<std::uint64_t> action_result_callbacks_total_{0};
  // Even values identify complete command-progress metric snapshots; odd
  // values fence the brief multi-atomic publication in progress. This lets
  // qualification pair the monotonic sample count with exactly its last/max
  // latency values instead of observing a torn change point.
  std::atomic<std::uint64_t> command_progress_metrics_sequence_{0};
  std::atomic<std::uint64_t> command_progress_samples_total_{0};
  std::atomic<std::uint64_t> cancel_confirmations_total_{0};
  std::atomic<std::uint64_t> stop_confirmations_total_{0};
  std::atomic<std::uint64_t> last_command_progress_latency_ns_{0};
  std::atomic<std::uint64_t> max_command_progress_latency_ns_{0};
  std::atomic<std::uint64_t> last_action_result_latency_ns_{0};
  std::atomic<std::uint64_t> max_action_result_latency_ns_{0};
  std::atomic<std::uint64_t> last_cancel_latency_ns_{0};
  std::atomic<std::uint64_t> max_cancel_latency_ns_{0};
  std::atomic<std::uint64_t> last_stop_confirmation_latency_ns_{0};
  std::atomic<std::uint64_t> max_stop_confirmation_latency_ns_{0};
  std::atomic<std::uint64_t> jtc_cancel_reference_publications_total_{0};
  std::atomic<std::uint64_t> jtc_cancel_reference_rejections_total_{0};
  std::atomic<std::uint64_t> controller_binding_activity_events_total_{0};
  std::atomic<std::uint64_t> controller_binding_refresh_attempts_total_{0};
  std::atomic<std::uint64_t> controller_binding_refresh_successes_total_{0};
  std::atomic<std::uint64_t> controller_binding_refresh_failures_total_{0};
  std::atomic<std::uint64_t> controller_binding_dispatch_rejections_total_{0};
  std::atomic<std::uint64_t>
      execution_admission_dispatch_rejections_total_{0};
  std::atomic<std::uint64_t> dispatch_authority_losses_total_{0};
  std::atomic<std::uint64_t> authored_active_publications_total_{0};
  std::atomic<std::uint64_t> authored_active_interarrival_max_ns_{0};
  std::atomic<std::int64_t> authored_active_last_publication_ns_{0};
  mutable std::mutex scene_projection_barrier_mutex_;
  std::unique_ptr<detail::SceneProjectionWorker,
                  detail::SceneProjectionWorkerDeleter>
      scene_projection_worker_;
};

}  // namespace manyforge::ros2_control_execution
