// Copyright 2026 Flexin Group SRL
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
#include <control_msgs/action/gripper_command.hpp>
#include <cstdint>
#include <functional>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
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
namespace detail {
struct SceneProjectionCompletion;
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
  // Optional execution-state sink projection metadata. When set, the ROS
  // position-commanded gripper path also updates the declared gripper group so
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
  /// Feedback staleness threshold, from the gripper target configuration's
  /// `state_freshness_timeout_s` (no code default; must be positive); also the
  /// default budget of the CP9 `gripper_feedback` stream (overridable per
  /// component in the Deployment).
  double state_freshness_timeout_s{};
  /// CP9 `gripper_feedback` stream of the owning component, declared by the
  /// host. When set it records every matched feedback sample and owns the
  /// staleness judgement (the readiness snapshot no longer ages feedback).
  std::shared_ptr<core::StreamRecorder> feedback_stream{};
  double wait_for_action_server_timeout_s = 2.0;
  // Omission uses the typed default: pneumatic 1s, position 5s.
  std::optional<double> result_timeout_s;
  core::GripperActuationKind actuation_kind{
      core::GripperActuationKind::kPositionCommanded};
  /// Pneumatic display only; never an authoritative execution-state
  /// publication.
  std::function<core::Result<void>(double)> publish_display_position;
  core::GripperOutputMode output_mode{core::GripperOutputMode::kCommanded};
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
      Ros2TransportResources &resources,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      core::IClock &clock, core::IScheduler &scheduler, bool commanded_sim);
  ~Ros2GripperCommandBackend() override;
  void complete_ros_configuration();
  void shutdown() noexcept;

  void configure_gripper_action(std::string gripper_id,
                                Ros2GripperActionConfig config);
  [[nodiscard]] Ros2GripperTransportReadinessSnapshot
  gripper_transport_readiness(const std::string &gripper_id) const;
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
  void authorize_gripper_commands() noexcept override;
  core::Result<void> try_authorize_gripper_commands() noexcept override;
  [[nodiscard]] Ros2GripperLifecycleResult authorize_lifecycle_gripper(
      const std::string &gripper_id);
  [[nodiscard]] bool lifecycle_disable_pending(
      const std::string &gripper_id) const;
  [[nodiscard]] Ros2GripperLifecycleResult prepare_lifecycle_gripper(
      const std::string &gripper_id);
  [[nodiscard]] Ros2GripperLifecycleResult disable_lifecycle_gripper(
      const std::string &gripper_id, std::stop_token cancellation);

private:
  using GripperCommand = control_msgs::action::GripperCommand;
  using GoalHandleGripper = rclcpp_action::ClientGoalHandle<GripperCommand>;
  struct GripperRuntime {
    Ros2GripperActionConfig config;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr joint_state_pub;
    rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
        joint_state_sub;
    mutable std::mutex mutex;
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
    std::vector<std::string> evidence;
    std::string evidence_state;
    bool externally_failed = false;
    std::string error;
    std::string reason_code;
  };

  void advance_pneumatic_locked(GripperRuntime &runtime) const;
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
  bool
  schedule_gripper_lifecycle(const std::shared_ptr<GripperRuntime> &runtime,
                             std::uint64_t command_id, std::uint64_t generation,
                             core::TimePoint deadline_ns);
  void advance_gripper_command(const std::shared_ptr<GripperRuntime> &runtime,
                               std::uint64_t command_id,
                               std::uint64_t generation,
                               std::uint64_t schedule_generation);

  Ros2TransportResources &resources_;
  std::shared_ptr<core::IExecutionStateSink> execution_state_sink_;
  core::IClock &clock_;
  core::IScheduler &scheduler_;
  rclcpp::Node::SharedPtr node_;
  detail::SceneProjectionWorker *scene_projection_worker_;
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
  std::uint64_t gripper_authority_generation_ = 1;
  std::uint64_t gripper_lifecycle_generation_ = 1;
  bool gripper_admission_blocked_ = false; // Destructive lifecycle fence.
  bool gripper_motion_admission_blocked_ = false; // Stop affects motion only.
  mutable std::mutex gripper_map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<GripperRuntime>> grippers_;
  std::atomic<std::uint64_t> next_gripper_command_id_{1};
  std::atomic<std::uint64_t> next_gripper_generation_{1};
};

} // namespace manyforge::ros2_control_execution
