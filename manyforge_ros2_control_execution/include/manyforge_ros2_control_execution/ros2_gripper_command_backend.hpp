// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <control_msgs/action/parallel_gripper_command.hpp>
#include <cstdint>
#include <functional>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <string>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace manyforge::ros2_control_execution {

class GraphEventObserver;
namespace detail {
struct SceneProjectionCompletion;
class SceneProjectionWorker;
}

struct Ros2GripperActionConfig {
  std::string action_name = "/robotiq_gripper_controller/gripper_cmd";
  std::string transport_kind = "ros_gripper_command";
  std::string command_topic;
  std::string state_topic;
  std::vector<std::string> command_joint_names;
  // Exact joint identity carried by the raw transport state topic. This is
  // intentionally separate from joint_name, which is the compiled coordinate
  // used by the execution-state projection sink.
  std::string state_joint_name;
  // Optional execution-state sink projection metadata. The position-commanded
  // path and the no-feedback pneumatic no-output path update the declared
  // gripper group through the same robot-state path as the arm.
  std::string group_id;
  std::string joint_name;
  std::string completion_policy = "position_reached";
  double open_position = 0.0;
  double closed_position = 0.0;
  double open_effort = 0.0;
  double close_effort = 0.0;
  bool publish_nominal_position = false;
  /// Feedback staleness threshold (no code default; must be positive): the
  /// budget of the component's `gripper_feedback` stream row.
  double state_freshness_timeout_s{};
  /// That row's severity/recovery, used when the host declares the stream.
  core::StreamSeverityDTO feedback_stream_severity{};
  core::StreamRecoveryDTO feedback_stream_recovery{};
  /// The `gripper_feedback` stream of the owning component, declared by the
  /// host. When set it records every matched feedback sample and owns the
  /// staleness judgement (the readiness snapshot no longer ages feedback).
  std::shared_ptr<core::StreamRecorder> feedback_stream{};
  double wait_for_action_server_timeout_s = 2.0;
  // Omission uses the typed default: pneumatic 1s, position 5s.
  std::optional<double> result_timeout_s;
  core::GripperActuationKind actuation_kind{
      core::GripperActuationKind::kPositionCommanded};
  /// Display-only callback for pneumatic paths with feedback or no model group.
  std::function<core::Result<void>(double)> publish_display_position;
  core::GripperOutputMode output_mode{core::GripperOutputMode::kCommanded};
  /// Opt-in stall-window grasp evidence for position grippers: a completed
  /// command whose terminal position lies in [min, max] is confirmed, outside
  /// it is not. Absence publishes no grasp evidence (the existing default).
  struct GraspStallWindow {
    double min_position{};
    double max_position{};
  };
  std::optional<GraspStallWindow> grasp_stall_window;
  /// Stop finish tolerance (the gripper's position units, >= 0): a Stop that
  /// finds the fingers this close to the commanded target lets the action
  /// goal finish instead of cancelling it (the stock parallel gripper action
  /// controller never answers a cancel that races its own success). 0 always cancels.
  double stop_finish_tolerance{};
};

struct Ros2GripperLifecycleResult {
  bool succeeded{};
  bool effect_started{};
  std::optional<core::ErrorDTO> error;
};

/// One bounded, non-blocking readiness read for a configured gripper
/// transport. This is diagnostic/lifecycle evidence only; it grants no command
/// authority and performs no discovery wait.
struct Ros2GripperTransportReadinessSnapshot {
  std::string transport_kind;
  std::string state_topic;
  std::string state_joint_name;
  bool configured{};
  bool command_path_ready{};
  bool feedback_path_ready{};
  // Physical transport readiness for commanded output, or explicit local
  // owner readiness for none; command admission belongs to the owner.
  bool ready{};
  std::uint64_t feedback_publisher_count{};
  std::uint64_t feedback_messages_received{};
  std::uint64_t feedback_samples_matched{};
  std::vector<std::string> reason_codes;

  core::GripperOutputMode output_mode{core::GripperOutputMode::kCommanded};
  // A configured local owner, independent of any physical transport path.
  bool logical_owner_ready{};

  friend bool
  operator==(const Ros2GripperTransportReadinessSnapshot &,
             const Ros2GripperTransportReadinessSnapshot &) = default;
};

/// ROS gripper transport and autonomous command progression.
/// The retained host owns ROS resources and quiesces callbacks before
/// destruction. Stop policy and realization are outside this command adapter.
class Ros2GripperCommandBackend final : public core::IGripperCommandBackend {
public:
  Ros2GripperCommandBackend(
      std::shared_ptr<Ros2TransportResources> resources,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      core::IClock &clock, core::IScheduler &scheduler, bool commanded_sim);
  ~Ros2GripperCommandBackend() override;
  void complete_ros_configuration();
  void set_retirement_wake(std::function<void()> wake);
  void shutdown() noexcept;
  // Only after motion and gripper projection producers have joined.
  // A false result requires the host to retain this backend and its owner.
  [[nodiscard]] bool StopSceneProjectionUntil(
      std::chrono::steady_clock::time_point deadline) noexcept;

  void configure_gripper_action(std::string gripper_id,
                                Ros2GripperActionConfig config,
                                core::Duration lifecycle_timeout_ns);
  struct StagedRosRebind;
  /// Builds gated, unpublished entities for the existing runtime records.
  [[nodiscard]] std::shared_ptr<StagedRosRebind> stage_ros_rebind(
      std::shared_ptr<Ros2TransportResources> replacement);
  /// Publish the exact staged replacement after the previous owner joined.
  /// The caller retains the stage until after releasing its own locks.
  [[nodiscard]] core::Result<void> commit_ros_rebind(
      const std::shared_ptr<StagedRosRebind> &stage);
  /// Validate under the gripper's existing authority/map/runtime locks, then
  /// run the enclosing no-throw commit before the final owner publication.
  /// A false callback must leave every enclosing slot unchanged.
  [[nodiscard]] bool commit_ros_rebind_with(
      const std::shared_ptr<StagedRosRebind> &stage,
      bool (*commit)(void *) noexcept, void *context,
      const std::shared_ptr<Ros2TransportResources> &failed_dependency = {});
  /// Enable staged feedback only after the enclosing owner transaction has
  /// published all of its consumers.
  [[nodiscard]] core::Result<void> activate_ros_rebind(
      const std::shared_ptr<StagedRosRebind> &stage);
  [[nodiscard]] Ros2GripperTransportReadinessSnapshot
  gripper_transport_readiness(const std::string &gripper_id) const;
  /// Pure local owner-readiness snapshot; absent or stale evidence is false.
  [[nodiscard]] bool
  cached_gripper_local_owner_ready(const std::string &gripper_id) const;
  [[nodiscard]] core::Result<void> validate_gripper_command(
      const core::GripperCommandRequestDTO &request) const override;
  core::GripperActuationKind
  gripper_actuation_kind(const std::string &gripper_id) const override;
  [[nodiscard]] core::GripperOutputMode
  gripper_output_mode(const std::string &gripper_id) const override;
  core::GripperCommandResultDTO
  command_gripper(const core::GripperCommandRequestDTO &request) override;
  core::GripperCommandResultDTO mark_gripper_grasp_lost(
      const std::string &gripper_id, std::uint64_t expected_command_id,
      std::uint64_t expected_generation, const std::string &evidence_label,
      const std::string &message,
      const std::string &reason_code = "grasp_confirmation_lost") override;
  core::GripperCommandStatusDTO
  get_gripper_status(const std::string &gripper_id) const override;
  void invalidate_gripper_commands(
      core::GripperInvalidationReason reason) noexcept override;
  [[nodiscard]] bool rebuilt_unknown_pending(const std::string &gripper_id,
      std::uint64_t command_id, std::uint64_t generation) const override;
  [[nodiscard]] std::shared_ptr<const void> rebuilt_unknown_owner(const std::string &gripper_id,
      std::uint64_t command_id, std::uint64_t generation) const override;
  [[nodiscard]] core::Result<void> retire_rebuilt_unknown(const std::string &gripper_id,
      std::uint64_t command_id, std::uint64_t generation,
      const std::shared_ptr<const void> &expected_owner) override;
  void authorize_gripper_commands() noexcept override;
  core::Result<void> try_authorize_gripper_commands() noexcept override;
  [[nodiscard]] Ros2GripperLifecycleResult authorize_lifecycle_gripper(
      const std::string &gripper_id);
  [[nodiscard]] bool lifecycle_disable_pending(
      const std::string &gripper_id) const;
  [[nodiscard]] Ros2GripperLifecycleResult prepare_lifecycle_gripper(
      const std::string &gripper_id);
  [[nodiscard]] Ros2GripperLifecycleResult disable_lifecycle_gripper(
      const std::string &gripper_id, std::stop_token cancellation,
      std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::time_point::max());

  using LateCancelJournalHook = std::function<core::Result<void>(
      std::uint64_t, std::uint64_t, std::uint64_t, const core::CommandTargetId &, const std::string &)>;
  [[nodiscard]] core::Result<void> InstallGripperLateCancelJournalHook(
      const std::string &gripper_id, const void *engine_identity,
      LateCancelJournalHook hook);
  enum class LateCancelShutdownStatus { kSettled, kPendingOrFailed, kDurableUnknown, kStuck };
  [[nodiscard]] LateCancelShutdownStatus gripper_late_cancel_shutdown_status() const noexcept;
  [[nodiscard]] bool gripper_late_cancel_failed() const noexcept;
  [[nodiscard]] std::vector<std::shared_ptr<core::GripperPreparedSessionPin>>
  tracked_gripper_prepared_sessions() const override;
  [[nodiscard]] LateCancelShutdownStatus CloseGripperLateCancels(
      std::chrono::steady_clock::time_point deadline);

private:
  struct GripperRuntime;
  struct RosEntities;
  struct LateCancelTracking {
    struct Entry {
      std::string gripper_id;
      std::shared_ptr<core::GripperLateCancelSlot> slot;
      std::shared_ptr<GripperRuntime> runtime;
      std::shared_ptr<RosEntities> bundle;
      std::shared_ptr<core::GripperPreparedSessionPin> session;
    };
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<Entry> entries;
  };
  std::shared_ptr<LateCancelTracking> late_cancel_tracking_ =
      std::make_shared<LateCancelTracking>();
  std::shared_ptr<std::atomic<std::uint64_t>> late_cancel_failure_epoch_ =
      std::make_shared<std::atomic<std::uint64_t>>(0);
  std::atomic<std::uint64_t> late_cancel_cleared_epoch_{0};
  friend class Ros2GripperCommandBackendTestPeer;
  void invalidate_gripper_commands_impl(
      core::GripperInvalidationReason reason) noexcept;
  core::Result<void> try_authorize_gripper_commands_impl(
      const std::shared_ptr<Ros2TransportResources> &owner) noexcept;
  Ros2GripperLifecycleResult authorize_lifecycle_gripper_impl(
      const std::string &gripper_id, const std::shared_ptr<Ros2TransportResources> &owner);
  Ros2GripperLifecycleResult prepare_lifecycle_gripper_impl(
      const std::string &gripper_id, const std::shared_ptr<Ros2TransportResources> &owner);
  core::GripperCommandResultDTO mark_gripper_grasp_lost_impl(
      const std::string &gripper_id, std::uint64_t expected_command_id,
      std::uint64_t expected_generation, const std::string &evidence_label,
      const std::string &message, const std::string &reason_code);
  using GripperCommand = control_msgs::action::ParallelGripperCommand;
  using GoalHandleGripper = rclcpp_action::ClientGoalHandle<GripperCommand>;
  struct RosEntities;
  struct GripperRuntime {
    Ros2GripperActionConfig config;
    core::Duration lifecycle_timeout_ns{};
    std::shared_ptr<RosEntities> ros;
    /// The exact transport that sent the current action goal.
    std::shared_ptr<RosEntities> goal_ros;
    mutable std::mutex mutex;
    const void *late_cancel_engine_identity{};
    LateCancelJournalHook late_cancel_journal_hook;
    std::shared_ptr<core::GripperLateCancelSlot> goal_late_cancel_slot;
    std::atomic<std::uint64_t> send_generation{0};
    std::condition_variable_any lifecycle_condition;
    GoalHandleGripper::SharedPtr active_goal_handle;
    std::string state = "idle";
    bool executing = false;
    bool goal_accepted = false;
    bool cancel_pending = false;
    bool lifecycle_disable_pending = false;
    bool lifecycle_disable_terminal = false;
    bool lifecycle_disable_failed = false;
    bool lifecycle_cancel_dispatched = false;
    // A Stop found the command already done (fingers within
    // stop_finish_tolerance, or its command_duration_s elapsed): the goal of
    // this command finishes and is never cancelled (also when its goal
    // response arrives after the Stop).
    bool stop_lets_goal_finish = false;
    std::uint64_t lifecycle_disable_command_id = 0;
    std::uint64_t lifecycle_disable_generation = 0;
    std::uint64_t command_id = 0;
    std::optional<core::AddressedGripperCommandIdentityDTO> addressed_identity;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    double progress = 0.0;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    std::uint64_t lifecycle_schedule_generation = 0;
    std::optional<double> start_position;
    std::optional<double> current_position;
    std::optional<double> observed_position;
    std::optional<std::chrono::steady_clock::time_point> observed_at;
    std::uint64_t feedback_messages_received = 0;
    std::uint64_t feedback_samples_matched = 0;
    std::optional<double> max_effort;
    double target_position = 0.0;
    double duration_s = 0.0;
    bool wait_for_duration = false;
    // The controller reported done; the command waits for duration_s.
    bool controller_done = false;
    core::TimePoint started_at_ns = 0;
    std::optional<core::TimePoint> delivered_at_ns;
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
    /// Whether the current pending projection has actually entered the worker.
    bool scene_projection_submitted = false;
    std::uint64_t scene_projection_submitted_request_id = 0;
    bool scene_projection_missed_settled = false;
    std::vector<std::string> evidence;
    std::string evidence_state;
    bool externally_failed = false;
    std::string error;
    std::string reason_code;
  };

  struct RosEntities {
    struct ActionLeaf { std::timed_mutex mutex; };
    struct PublisherLeaf {
      enum class State : std::uint8_t { kOpen, kRebindFenced, kRetired };
      std::timed_mutex mutex;
      std::atomic<State> state{State::kOpen};
    };
    std::shared_ptr<Ros2TransportResources> owner;
    std::shared_ptr<std::atomic<bool>> callbacks_enabled;
    // OD-66: the action client's own time-critical group (null on an owner
    // without a time-critical thread: the default group).
    rclcpp::CallbackGroup::SharedPtr action_callback_group;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::shared_ptr<ActionLeaf> action_leaf;
    std::shared_ptr<PublisherLeaf> publisher_leaf;
    std::shared_ptr<std::atomic<int>> action_ready;
    std::shared_ptr<GraphEventObserver> action_graph_observer;
    // -1 until the exact publisher receives a matched event.
    std::shared_ptr<std::atomic<std::int64_t>> command_matches;
    rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr joint_state_pub;
    rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_sub;
  };
  [[nodiscard]] static bool old_effect_owner_retired(const GripperRuntime &runtime) noexcept;
  [[nodiscard]] static RosEntities create_ros_entities(
      const std::shared_ptr<Ros2TransportResources> &owner,
      const std::shared_ptr<GripperRuntime> &runtime,
      std::shared_ptr<std::atomic<bool>> callbacks_enabled = {});

  void advance_pneumatic_locked(GripperRuntime &runtime) const;
  // A controller completion of a wait_for_duration command before
  // duration_s has elapsed holds it executing (controller_done); the
  // lifecycle tick then completes it. False when the command completes now.
  bool hold_until_duration_locked(GripperRuntime &runtime) const;
  void confirm_pneumatic_delivery_locked(GripperRuntime &runtime);
  void publish_pneumatic_display(const std::shared_ptr<GripperRuntime> &runtime,
                                 double fallback_position) noexcept;

  std::optional<double>
  scene_group_joint_position(const std::string &group_id,
                             const std::string &joint_name) const;
  bool schedule_gripper_scene_projection_locked(
      const std::shared_ptr<GripperRuntime> &runtime, double position,
      bool terminal);
  void on_gripper_scene_projection_complete(
      const std::weak_ptr<GripperRuntime> &runtime,
      const detail::SceneProjectionCompletion &completion) noexcept;
  void on_gripper_feedback(
      const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
      std::uint64_t generation, GoalHandleGripper::SharedPtr,
      const std::shared_ptr<const GripperCommand::Feedback> feedback);
  void on_gripper_result(const std::shared_ptr<GripperRuntime> &runtime,
                         std::uint64_t command_id, std::uint64_t generation,
                         const GoalHandleGripper::WrappedResult &result);
  // Sends the cancel of a pending disable's goal (command_id, generation).
  // For a command already fenced by a Stop/invalidation, the server's
  // ACCEPT settles it like the goal's own result, whichever comes first.
  // send_on_calling_thread: the Stop invalidation sends directly under the
  // action leaf (its caller holds no lock). ROS callbacks keep the
  // StopControl hop: they run under rclcpp_action's internal locks, so
  // entering the leaf there would order those locks before the leaf, the
  // reverse of async_send_goal under the leaf.
  void dispatch_disable_cancel(
      const std::shared_ptr<GripperRuntime> &runtime,
      const std::shared_ptr<RosEntities> &goal_ros,
      const GoalHandleGripper::SharedPtr &goal, std::uint64_t command_id,
      std::uint64_t generation,
      const std::shared_ptr<core::GripperLateCancelSlot> &slot,
      bool settle_lifecycle_on_accept = true,
      bool send_on_calling_thread = false);
  bool
  schedule_gripper_lifecycle(const std::shared_ptr<GripperRuntime> &runtime,
                             std::uint64_t command_id, std::uint64_t generation,
                             core::TimePoint deadline_ns);
  void advance_gripper_command(const std::shared_ptr<GripperRuntime> &runtime,
                               std::uint64_t command_id,
                               std::uint64_t generation,
                               std::uint64_t schedule_generation);

  std::atomic<std::shared_ptr<Ros2TransportResources>> resources_;
  std::shared_ptr<core::IExecutionStateSink> execution_state_sink_;
  core::IClock &clock_;
  core::IScheduler &scheduler_;
  std::unique_ptr<detail::SceneProjectionWorker> scene_projection_worker_;
  bool commanded_sim_;
  bool is_commanded_sim_mode() const noexcept { return commanded_sim_; }
  mutable std::mutex ros_configuration_mutex_;
  bool ros_configuration_complete_ = false;
  std::atomic<bool> shutdown_started_{false};
  // Authority serializes admission/dispatch against reset, stop, and runtime
  // replacement without holding lifecycle or scene locks across ROS waits.
  // Lock order is authority -> map -> per-runtime; callbacks only take the
  // per-runtime leaf lock.
  mutable std::mutex gripper_authority_mutex_;
  std::function<void()> retirement_wake_;
  // Admission closes under the authority mutex, but the SDK send leaf cannot
  // take that mutex. These generations publish the same invalidations to it.
  std::atomic<std::uint64_t> gripper_authority_generation_{1};
  std::atomic<std::uint64_t> gripper_lifecycle_generation_{1};
  bool gripper_admission_blocked_ = false; // Destructive lifecycle fence.
  bool gripper_terminally_closed_ = false; // Protected by gripper_authority_mutex_.
  bool gripper_motion_admission_blocked_ = false; // Stop affects motion only.
  mutable std::mutex gripper_map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<GripperRuntime>> grippers_;
  std::atomic<std::uint64_t> next_gripper_command_id_{1};
  std::atomic<std::uint64_t> next_gripper_generation_{1};
};

} // namespace manyforge::ros2_control_execution
