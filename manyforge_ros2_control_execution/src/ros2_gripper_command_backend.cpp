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

#include "scene_projection_worker.hpp"
#include "transport_helpers.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::ros2_control_execution {
using detail::clamp_progress;
using detail::wait_for_action_server_until;
namespace {
core::Duration gripper_duration_ns(double seconds) noexcept {
  constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
  const double nanoseconds = seconds * kNanosecondsPerSecond;
  if (!std::isfinite(nanoseconds) ||
      nanoseconds >=
          static_cast<double>(std::numeric_limits<core::Duration>::max())) {
    return std::numeric_limits<core::Duration>::max();
  }
  if (nanoseconds <= 0.0) {
    return 0;
  }
  return static_cast<core::Duration>(nanoseconds);
}

core::TimePoint gripper_deadline(core::TimePoint started_at,
                                 core::Duration duration) noexcept {
  const auto maximum = std::numeric_limits<core::TimePoint>::max();
  if (duration > 0 && started_at > maximum - duration) {
    return maximum;
  }
  return started_at + duration;
}

core::Duration gripper_duration_sum(core::Duration lhs,
                                    core::Duration rhs) noexcept {
  const auto maximum = std::numeric_limits<core::Duration>::max();
  return lhs > maximum - rhs ? maximum : lhs + rhs;
}

void add_unique_evidence(std::vector<std::string> &evidence,
                         const std::string &label) {
  if (label.empty()) {
    return;
  }
  if (std::find(evidence.begin(), evidence.end(), label) == evidence.end()) {
    evidence.push_back(label);
  }
}

core::GripperCommandResultDTO
rejected_gripper_command(std::string reason_code, std::string message,
                         std::uint64_t command_id = 0,
                         std::uint64_t generation = 0) {
  return core::GripperCommandResultDTO{
      false, command_id, generation,
      core::ErrorDTO{std::move(reason_code), std::move(message), {}}};
}

bool gripper_request_matches_transport(
    const core::GripperCommandRequestDTO &request,
    const Ros2GripperActionConfig &config) noexcept {
  const auto selects = [](const std::string &requested,
                          const std::string &physical,
                          const std::string_view resolved) {
    return requested.empty() || requested == physical || requested == resolved;
  };
  return (request.group_id.empty() || request.group_id == config.group_id) &&
         (request.joint_name.empty() ||
          request.joint_name == config.joint_name) &&
         selects(request.transport_kind, config.transport_kind,
                 "resolved_binding") &&
         selects(request.completion_policy, config.completion_policy,
                 "goal_fenced_target_reached");
}

} // namespace

Ros2GripperCommandBackend::Ros2GripperCommandBackend(
    Ros2TransportResources &resources,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    core::IClock &clock, core::IScheduler &scheduler, bool commanded_sim)
    : resources_(resources),
      execution_state_sink_(std::move(execution_state_sink)), clock_(clock),
      scheduler_(scheduler), node_(resources.node()),
      scene_projection_worker_(&resources.projection_worker()),
      commanded_sim_(commanded_sim) {
  if (!execution_state_sink_)
    throw std::invalid_argument("execution state sink must not be null");
  if (resources.started())
    throw std::logic_error(
        "gripper must be installed before ROS transport starts");
}

Ros2GripperCommandBackend::~Ros2GripperCommandBackend() { shutdown(); }

void Ros2GripperCommandBackend::complete_ros_configuration() {
  std::lock_guard<std::mutex> lock(ros_configuration_mutex_);
  if (ros_configuration_complete_)
    throw std::logic_error("gripper configuration was already completed");
  ros_configuration_complete_ = true;
}

void Ros2GripperCommandBackend::shutdown() noexcept {
  if (shutdown_started_.exchange(true, std::memory_order_acq_rel))
    return;
  invalidate_gripper_commands(core::GripperInvalidationReason::kShutdown);
  if (!scheduler_.drain_for(core::SchedulerPriority::ControllerCommand,
                            std::chrono::seconds(1))) {
    RCLCPP_FATAL(node_->get_logger(),
                 "Timed out draining gripper controller-command callbacks");
    std::terminate();
  }
}

std::optional<double> Ros2GripperCommandBackend::scene_group_joint_position(
    const std::string &group_id, const std::string &joint_name) const {
  return execution_state_sink_->group_joint_position(group_id, joint_name);
}

bool Ros2GripperCommandBackend::schedule_gripper_scene_projection_locked(
    const std::shared_ptr<GripperRuntime> &runtime, double position,
    bool terminal) {
  if (!runtime || !std::isfinite(position)) {
    return false;
  }
  if (runtime->config.group_id.empty() || runtime->config.joint_name.empty()) {
    runtime->projection_state = core::GripperProjectionState::kNotRequired;
    runtime->projection_reason.clear();
    return true;
  }
  if (scene_projection_worker_ == nullptr ||
      !runtime->scene_projection_slot.has_value()) {
    runtime->projection_state = core::GripperProjectionState::kPending;
    runtime->projection_reason = "scene_projection_worker_unavailable";
    return false;
  }

  const detail::SceneProjectionToken token{
      runtime->command_id,
      runtime->generation,
      ++runtime->scene_projection_request_id,
  };
  const bool queued =
      terminal
          ? scene_projection_worker_->submit_terminal_joint(
                *runtime->scene_projection_slot, position, clock_.now(), token)
          : scene_projection_worker_->submit_active_joint(
                *runtime->scene_projection_slot, position, clock_.now(), token);
  runtime->projection_state = core::GripperProjectionState::kPending;
  runtime->projection_reason =
      queued ? "scene_projection_queued" : "scene_projection_suspended";
  return queued;
}

void Ros2GripperCommandBackend::on_gripper_scene_projection_complete(
    const std::weak_ptr<GripperRuntime> &weak_runtime,
    const detail::SceneProjectionCompletion &completion) noexcept {
  const auto runtime = weak_runtime.lock();
  if (!runtime) {
    return;
  }

  bool retry_superseded_terminal = false;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != completion.token.command_id ||
        runtime->generation != completion.token.generation ||
        runtime->scene_projection_request_id != completion.token.request_id) {
      return;
    }
    command_id = runtime->command_id;
    generation = runtime->generation;
    switch (completion.outcome) {
    case detail::SceneProjectionOutcome::kApplied:
      runtime->projection_state = core::GripperProjectionState::kApplied;
      runtime->projection_reason.clear();
      runtime->projection_retry_count = 0;
      break;
    case detail::SceneProjectionOutcome::kMissed:
      runtime->projection_state = core::GripperProjectionState::kPending;
      runtime->projection_reason = "scene_sink_busy";
      break;
    case detail::SceneProjectionOutcome::kRetryExhausted:
      runtime->projection_state = core::GripperProjectionState::kFailed;
      runtime->projection_reason = "scene_sink_retry_exhausted";
      runtime->state = "failed";
      runtime->executing = false;
      runtime->error =
          "Timed out publishing the terminal gripper scene position";
      runtime->reason_code = "gripper_projection_failed";
      break;
    case detail::SceneProjectionOutcome::kSuperseded:
      runtime->projection_state = core::GripperProjectionState::kPending;
      runtime->projection_reason = "scene_projection_superseded";
      retry_superseded_terminal =
          !runtime->executing && runtime->state == "completed";
      break;
    }
    ++runtime->revision;
  }
  if (retry_superseded_terminal) {
    try {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    } catch (...) {
      // The generation remains conservatively pending. Reset/replacement can
      // still invalidate it; a scheduler exception must not escape this NRT
      // completion callback or terminate the projection worker.
    }
  }
}

std::optional<double>
position_for_joint(const sensor_msgs::msg::JointState &msg,
                   const std::string &joint_name);

void Ros2GripperCommandBackend::configure_gripper_action(
    std::string gripper_id, Ros2GripperActionConfig config) {
  if (gripper_id.empty()) {
    throw std::invalid_argument("gripper_id must not be empty");
  }
  if (config.transport_kind.empty()) {
    config.transport_kind = "ros_gripper_command";
  }
  if (config.transport_kind != "ros_gripper_command" &&
      config.transport_kind != "isaac_joint_state_effort") {
    throw std::invalid_argument(
        "gripper transport_kind must be ros_gripper_command or "
        "isaac_joint_state_effort");
  }
  if (config.transport_kind == "ros_gripper_command" &&
      config.action_name.empty()) {
    throw std::invalid_argument(
        "gripper action_name must not be empty for ros_gripper_command");
  }
  if (config.transport_kind == "isaac_joint_state_effort") {
    if (config.command_topic.empty()) {
      throw std::invalid_argument("gripper command_topic must not be empty for "
                                  "isaac_joint_state_effort");
    }
    if (config.command_joint_names.empty() && config.joint_name.empty()) {
      throw std::invalid_argument(
          "gripper joint_name or command_joint_names is required for "
          "isaac_joint_state_effort");
    }
    if (!std::isfinite(config.open_position) ||
        !std::isfinite(config.closed_position) ||
        config.open_position == config.closed_position ||
        !std::isfinite(config.open_effort) ||
        !std::isfinite(config.close_effort)) {
      throw std::invalid_argument(
          "isaac_joint_state_effort gripper positions and efforts are invalid");
    }
    if (!config.state_topic.empty() && config.state_joint_name.empty()) {
      if (config.command_joint_names.size() != 1U) {
        throw std::invalid_argument(
            "gripper state_joint_name is required when the raw state topic "
            "cannot be resolved from one command joint");
      }
      config.state_joint_name = config.command_joint_names.front();
    }
  }
  if (!std::isfinite(config.state_freshness_timeout_s) ||
      !std::isfinite(config.wait_for_action_server_timeout_s) ||
      !std::isfinite(config.result_timeout_s) ||
      config.state_freshness_timeout_s < 0.0 ||
      config.wait_for_action_server_timeout_s < 0.0 ||
      config.result_timeout_s < 0.0) {
    throw std::invalid_argument("gripper action timing config is invalid");
  }
  if (config.completion_policy != "position_reached" &&
      config.completion_policy != "driver_result" &&
      config.completion_policy != "dwell_elapsed") {
    throw std::invalid_argument(
        "gripper completion_policy must be position_reached, driver_result, "
        "or dwell_elapsed");
  }
  if (config.group_id.empty() != config.joint_name.empty()) {
    throw std::invalid_argument(
        "gripper group_id and joint_name must be configured together");
  }
  std::lock_guard<std::mutex> ros_configuration_lock(ros_configuration_mutex_);
  if (ros_configuration_complete_ || resources_.started()) {
    throw std::logic_error(
        "gripper transports must be configured before the ROS executor starts");
  }
  auto runtime = std::make_shared<GripperRuntime>();
  runtime->config = std::move(config);
  if (runtime->config.transport_kind == "ros_gripper_command") {
    runtime->action_client = rclcpp_action::create_client<GripperCommand>(
        node_, runtime->config.action_name);
  } else {
    runtime->joint_state_pub =
        node_->create_publisher<sensor_msgs::msg::JointState>(
            runtime->config.command_topic, rclcpp::SystemDefaultsQoS());
    if (!runtime->config.state_topic.empty()) {
      const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
      runtime->joint_state_sub =
          node_->create_subscription<sensor_msgs::msg::JointState>(
              runtime->config.state_topic, rclcpp::SystemDefaultsQoS(),
              [weak_runtime](
                  const sensor_msgs::msg::JointState::SharedPtr msg) {
                const auto runtime = weak_runtime.lock();
                if (!runtime || !msg) {
                  return;
                }
                const auto &observed_joint = runtime->config.state_joint_name;
                if (observed_joint.empty()) {
                  return;
                }
                const auto position = position_for_joint(*msg, observed_joint);
                std::lock_guard<std::mutex> lock(runtime->mutex);
                if (runtime->feedback_messages_received !=
                    std::numeric_limits<std::uint64_t>::max()) {
                  ++runtime->feedback_messages_received;
                }
                if (!position.has_value()) {
                  return;
                }
                if (runtime->feedback_samples_matched !=
                    std::numeric_limits<std::uint64_t>::max()) {
                  ++runtime->feedback_samples_matched;
                }
                runtime->observed_position = *position;
                runtime->observed_at = std::chrono::steady_clock::now();
                ++runtime->revision;
              });
    }
  }
  runtime->generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  if (!runtime->config.group_id.empty()) {
    const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
    runtime->scene_projection_slot =
        scene_projection_worker_->register_joint_slot(
            runtime->config.group_id, runtime->config.joint_name,
            [this, weak_runtime](
                const detail::SceneProjectionCompletion &completion) {
              on_gripper_scene_projection_complete(weak_runtime, completion);
            });
  }
  std::shared_ptr<GripperRuntime> replaced_runtime;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto existing = grippers_.find(gripper_id);
    if (existing != grippers_.end()) {
      replaced_runtime = existing->second;
    }
    grippers_[std::move(gripper_id)] = std::move(runtime);
  }
  if (replaced_runtime) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(replaced_runtime->mutex);
      goal_handle = replaced_runtime->active_goal_handle;
      action_client = replaced_runtime->action_client;
      lifecycle_handle = replaced_runtime->lifecycle_handle;
      replaced_runtime->lifecycle_handle.reset();
      ++replaced_runtime->lifecycle_schedule_generation;
      replaced_runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      replaced_runtime->active_goal_handle.reset();
      replaced_runtime->executing = false;
      replaced_runtime->goal_accepted = false;
      replaced_runtime->cancel_pending = true;
      replaced_runtime->state = "idle";
      ++replaced_runtime->revision;
    }
    if (lifecycle_handle.has_value()) {
      try {
        scheduler_.cancel(*lifecycle_handle);
      } catch (...) {
        // Generation fencing has already made the callback inert. Lifecycle
        // invalidation is noexcept and remains authoritative even when eager
        // scheduler cleanup fails.
      }
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception &exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel replaced gripper action goal: %s",
                    exc.what());
      }
    }
  }
}

void publish_isaac_joint_state_gripper_command(
    const rclcpp::Node::SharedPtr &node, const Ros2GripperActionConfig &config,
    const rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr &publisher,
    double target_position) {
  if (!publisher) {
    throw std::runtime_error(
        "Isaac gripper command publisher is not configured");
  }
  std::vector<std::string> joint_names = config.command_joint_names;
  if (joint_names.empty()) {
    joint_names.push_back(config.joint_name);
  }
  const bool closing = std::abs(target_position - config.closed_position) <=
                       std::abs(target_position - config.open_position);
  const double effort = closing ? config.close_effort : config.open_effort;

  sensor_msgs::msg::JointState msg;
  msg.header.stamp = node->now();
  msg.name = joint_names;
  if (config.publish_nominal_position) {
    msg.position.assign(joint_names.size(), target_position);
  }
  msg.effort.assign(joint_names.size(), effort);
  publisher->publish(msg);
}

std::optional<double>
position_for_joint(const sensor_msgs::msg::JointState &msg,
                   const std::string &joint_name) {
  for (std::size_t i = 0; i < msg.name.size(); ++i) {
    if (msg.name[i] == joint_name && i < msg.position.size()) {
      return std::isfinite(msg.position[i])
                 ? std::optional<double>{msg.position[i]}
                 : std::nullopt;
    }
  }
  return std::nullopt;
}

void Ros2GripperCommandBackend::on_gripper_feedback(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, GoalHandleGripper::SharedPtr goal_handle,
    const std::shared_ptr<const GripperCommand::Feedback> feedback) {
  if (!runtime || !feedback) {
    return;
  }
  std::optional<double> feedback_position;
  bool reached_goal = false;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation || runtime->externally_failed) {
      return;
    }
    if (!runtime->executing) {
      return;
    }
    if (!std::isfinite(feedback->position)) {
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      action_client = runtime->action_client;
      if (!goal_handle) {
        goal_handle = runtime->active_goal_handle;
      }
      runtime->active_goal_handle.reset();
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = true;
      runtime->externally_failed = true;
      runtime->error = "Gripper action feedback position is not finite";
      runtime->reason_code = ros2_control_fault_reason::kGripperStateNotFinite;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
    } else {
      runtime->current_position = feedback->position;
      runtime->progress =
          clamp_progress(static_cast<double>(std::max<core::Duration>(
                             0, clock_.now() - runtime->started_at_ns)) /
                         static_cast<double>(std::max<core::Duration>(
                             1, gripper_duration_ns(runtime->duration_s))));
      feedback_position = feedback->position;
      reached_goal = feedback->reached_goal;
      if (reached_goal) {
        runtime->state = "completed";
        runtime->executing = false;
        runtime->goal_accepted = false;
        runtime->cancel_pending = false;
        runtime->active_goal_handle.reset();
        lifecycle_handle = runtime->lifecycle_handle;
        runtime->lifecycle_handle.reset();
        ++runtime->lifecycle_schedule_generation;
        runtime->progress = 1.0;
        runtime->error.clear();
        runtime->reason_code.clear();
      }
    }
    ++runtime->revision;
  }
  if (lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*lifecycle_handle);
    } catch (...) {
      // The schedule generation already fences queued work; preserve the
      // result.
    }
  }
  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel gripper goal after non-finite feedback: %s",
                  exc.what());
    }
  }
  if (feedback_position.has_value() &&
      (!is_commanded_sim_mode() || reached_goal)) {
    bool retry_projection = false;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id == command_id &&
          runtime->generation == generation) {
        const bool queued = schedule_gripper_scene_projection_locked(
            runtime, *feedback_position, reached_goal);
        retry_projection = reached_goal && !queued;
        ++runtime->revision;
      }
    }
    if (retry_projection) {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    }
  }
}

void Ros2GripperCommandBackend::on_gripper_result(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, const GoalHandleGripper::WrappedResult &result) {
  if (!runtime) {
    return;
  }
  Ros2GripperActionConfig runtime_config;
  std::optional<double> result_position;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      return;
    }
    if (runtime->externally_failed) {
      return;
    }
    runtime_config = runtime->config;
    runtime->active_goal_handle.reset();
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->executing = false;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    const bool result_position_not_finite =
        result.result && !std::isfinite(result.result->position);
    if (result.result && !result_position_not_finite) {
      runtime->current_position = result.result->position;
      result_position = result.result->position;
    }
    if (result_position_not_finite) {
      runtime->state = "failed";
      runtime->externally_failed = true;
      runtime->error = "Gripper action result position is not finite";
      runtime->reason_code = ros2_control_fault_reason::kGripperStateNotFinite;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
    } else {
      switch (result.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        if (runtime_config.completion_policy == "driver_result" ||
            runtime_config.completion_policy == "dwell_elapsed" ||
            (result.result && result.result->reached_goal)) {
          runtime->state = "completed";
          runtime->progress = 1.0;
          runtime->error.clear();
          runtime->reason_code.clear();
          if (!result_position.has_value()) {
            result_position = runtime->target_position;
          }
        } else {
          runtime->state = "failed";
          runtime->error = "Gripper action succeeded without reaching the goal";
          runtime->reason_code =
              ros2_control_fault_reason::kGripperResultFailed;
        }
        break;
      case rclcpp_action::ResultCode::ABORTED:
        runtime->state = "failed";
        runtime->error = "Gripper action aborted";
        runtime->reason_code = ros2_control_fault_reason::kGripperGoalAborted;
        break;
      case rclcpp_action::ResultCode::CANCELED:
        runtime->state = "idle";
        runtime->error.clear();
        runtime->reason_code.clear();
        break;
      default:
        runtime->state = "failed";
        runtime->error = "Gripper action returned an unknown result code";
        runtime->reason_code = ros2_control_fault_reason::kGripperResultFailed;
        break;
      }
    }
    ++runtime->revision;
  }
  if (lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*lifecycle_handle);
    } catch (...) {
      // The schedule generation already fences queued work; preserve the
      // result.
    }
  }
  if (result_position.has_value()) {
    bool retry_projection = false;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id == command_id &&
          runtime->generation == generation) {
        const bool terminal = runtime->state == "completed";
        const bool queued = schedule_gripper_scene_projection_locked(
            runtime, *result_position, terminal);
        retry_projection = terminal && !queued;
        ++runtime->revision;
      }
    }
    if (retry_projection) {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    }
  }
}

void Ros2GripperCommandBackend::invalidate_gripper_commands(
    core::GripperInvalidationReason /*reason*/) noexcept {
  std::vector<std::shared_ptr<GripperRuntime>> runtimes;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    gripper_admission_blocked_ = true;
    ++gripper_authority_generation_;
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    for (const auto &[_, runtime] : grippers_) {
      runtimes.push_back(runtime);
    }
  }
  for (auto &runtime : runtimes) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      goal_handle = runtime->active_goal_handle;
      action_client = runtime->action_client;
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = true;
      runtime->active_goal_handle.reset();
      runtime->state = "idle";
      runtime->progress = 0.0;
      runtime->evidence.clear();
      runtime->evidence_state.clear();
      runtime->externally_failed = false;
      runtime->error.clear();
      runtime->reason_code.clear();
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
      ++runtime->revision;
    }
    if (lifecycle_handle.has_value()) {
      try {
        scheduler_.cancel(*lifecycle_handle);
      } catch (...) {
        // The generation fence already prevents a dequeued callback from
        // progressing.
      }
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception &exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel gripper action goal: %s", exc.what());
      }
    }
  }
}

void Ros2GripperCommandBackend::authorize_gripper_commands() noexcept {
  std::lock_guard<std::mutex> lock(gripper_authority_mutex_);
  if (!gripper_admission_blocked_) {
    return;
  }
  ++gripper_authority_generation_;
  gripper_admission_blocked_ = false;
}

Ros2GripperTransportReadinessSnapshot
Ros2GripperCommandBackend::gripper_transport_readiness(
    const std::string &gripper_id) const {
  Ros2GripperTransportReadinessSnapshot result;
  if (gripper_id.empty()) {
    result.reason_codes.push_back("gripper_identity_missing");
    return result;
  }

  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end()) {
      result.reason_codes.push_back("gripper_transport_not_configured");
      return result;
    }
    runtime = found->second;
  }

  Ros2GripperActionConfig runtime_config;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr command_publisher;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
      state_subscriber;
  std::optional<std::chrono::steady_clock::time_point> observed_at;
  {
    std::lock_guard<std::mutex> runtime_lock(runtime->mutex);
    runtime_config = runtime->config;
    action_client = runtime->action_client;
    command_publisher = runtime->joint_state_pub;
    state_subscriber = runtime->joint_state_sub;
    observed_at = runtime->observed_at;
    result.feedback_messages_received = runtime->feedback_messages_received;
    result.feedback_samples_matched = runtime->feedback_samples_matched;
  }
  result.transport_kind = runtime_config.transport_kind;
  result.state_topic = runtime_config.state_topic;
  result.state_joint_name = runtime_config.state_joint_name;
  result.configured = true;

  try {
    if (runtime_config.transport_kind == "ros_gripper_command") {
      result.command_path_ready =
          action_client && action_client->action_server_is_ready();
      result.feedback_path_ready = result.command_path_ready;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_action_server_unavailable");
      }
    } else if (runtime_config.transport_kind == "isaac_joint_state_effort") {
      result.command_path_ready =
          command_publisher && command_publisher->get_subscription_count() > 0U;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_command_subscriber_unavailable");
      }
      if (runtime_config.state_topic.empty()) {
        result.feedback_path_ready = true;
      } else {
        result.feedback_publisher_count =
            state_subscriber ? state_subscriber->get_publisher_count() : 0U;
        if (observed_at.has_value()) {
          const auto age = std::chrono::steady_clock::now() - *observed_at;
          const auto freshness = std::chrono::duration<double>(
              runtime_config.state_freshness_timeout_s);
          result.feedback_path_ready =
              age >= decltype(age)::zero() && age <= freshness;
        }
      }
      if (!result.feedback_path_ready) {
        result.reason_codes.push_back("gripper_state_feedback_unavailable");
        if (result.feedback_publisher_count == 0U) {
          result.reason_codes.push_back("gripper_state_publisher_unavailable");
        } else if (result.feedback_messages_received == 0U) {
          result.reason_codes.push_back("gripper_state_message_unreceived");
        } else if (result.feedback_samples_matched == 0U) {
          result.reason_codes.push_back("gripper_state_joint_unobserved");
        } else {
          result.reason_codes.push_back("gripper_state_feedback_stale");
        }
      }
    } else {
      result.reason_codes.push_back("gripper_transport_kind_invalid");
    }
  } catch (...) {
    result.command_path_ready = false;
    result.feedback_path_ready = false;
    result.reason_codes.push_back("gripper_transport_readiness_exception");
  }

  {
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto current = grippers_.find(gripper_id);
    if (current == grippers_.end() || current->second != runtime) {
      result.configured = false;
      result.command_path_ready = false;
      result.feedback_path_ready = false;
      result.reason_codes.push_back("gripper_transport_replaced");
    }
  }
  std::sort(result.reason_codes.begin(), result.reason_codes.end());
  result.reason_codes.erase(
      std::unique(result.reason_codes.begin(), result.reason_codes.end()),
      result.reason_codes.end());
  // Lifecycle readiness describes the physical transport, not command
  // admission. The simulator gate remains independently observable and every
  // command path still rejects while it is closed. Including the gate here
  // creates a startup cycle: CCLC requires external lifecycle readiness before
  // it can select the route whose reset flow opens the gate.
  result.ready = result.configured && result.command_path_ready &&
                 result.feedback_path_ready && result.reason_codes.empty();
  return result;
}

core::Result<void> Ros2GripperCommandBackend::validate_gripper_command(
    const core::GripperCommandRequestDTO &request) const {
  const auto rejected = [](std::string reason, std::string message) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        std::move(message),
        {{"reason", std::move(reason)}}});
  };
  const double max_effort = request.max_effort.value_or(0.0);
  if (request.gripper_id.empty() || !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || !std::isfinite(max_effort) ||
      request.duration_s < 0.0) {
    return rejected(ros2_control_fault_reason::kGripperInvalidCommand,
                    "Invalid gripper command target, duration, or max effort");
  }

  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_) {
      return rejected(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    }
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto configured = grippers_.find(request.gripper_id);
    if (configured == grippers_.end()) {
      return rejected(ros2_control_fault_reason::kGripperNotConfigured,
                      "No gripper transport configured for " +
                          request.gripper_id);
    }
    runtime = configured->second;
  }

  Ros2GripperActionConfig runtime_config;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime_config = runtime->config;
    if (runtime->executing) {
      return rejected(ros2_control_fault_reason::kGripperBusy,
                      "Gripper is already executing");
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected(runtime->reason_code.empty() ? "gripper_request_failed"
                                                   : runtime->reason_code,
                      runtime->error.empty() ? "Gripper state is unavailable"
                                             : runtime->error);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected(ros2_control_fault_reason::kGripperBusy,
                      "Gripper terminal scene projection is still pending");
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying");
    }
  }
  if (!gripper_request_matches_transport(request, runtime_config)) {
    return rejected(
        ros2_control_fault_reason::kInvalidGripperSemantics,
        "Gripper request semantics do not match the configured transport");
  }

  if (!runtime_config.group_id.empty() || !runtime_config.joint_name.empty()) {
    std::optional<double> start_position;
    try {
      start_position = scene_group_joint_position(runtime_config.group_id,
                                                  runtime_config.joint_name);
    } catch (const std::exception &exception) {
      return rejected("gripper_state_unavailable",
                      std::string("Cannot read gripper execution state: ") +
                          exception.what());
    } catch (...) {
      return rejected("gripper_state_unavailable",
                      "Cannot read gripper execution state");
    }
    if (!start_position.has_value()) {
      return rejected(ros2_control_fault_reason::kGripperNotConfigured,
                      "Cannot project gripper '" + request.gripper_id +
                          "' into execution-state sink group '" +
                          runtime_config.group_id + "' joint '" +
                          runtime_config.joint_name + "'");
    }
    if (!std::isfinite(*start_position)) {
      return rejected(
          ros2_control_fault_reason::kGripperStateNotFinite,
          "Configured gripper execution-state position is not finite");
    }
  }
  return core::Result<void>::Ok();
}

core::GripperCommandResultDTO Ros2GripperCommandBackend::command_gripper(
    const core::GripperCommandRequestDTO &request) {
  const double max_effort = request.max_effort.value_or(0.0);
  if (request.gripper_id.empty() || !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || !std::isfinite(max_effort) ||
      request.duration_s < 0.0) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kGripperInvalidCommand,
        "Invalid gripper command target, duration, or max effort");
  }
  std::shared_ptr<GripperRuntime> runtime;
  std::uint64_t authority_generation = 0;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    }
    authority_generation = gripper_authority_generation_;
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(request.gripper_id);
    if (it == grippers_.end()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "No gripper transport configured for " + request.gripper_id);
    }
    runtime = it->second;
  }
  const Ros2GripperActionConfig runtime_config = runtime->config;
  const auto runtime_is_current = [this, &request, &runtime]() {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto current = grippers_.find(request.gripper_id);
    return current != grippers_.end() && current->second == runtime;
  };
  const auto reject_existing_state =
      [&runtime]() -> std::optional<core::GripperCommandResultDTO> {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing) {
      return rejected_gripper_command(ros2_control_fault_reason::kGripperBusy,
                                      "Gripper is already executing",
                                      runtime->command_id, runtime->generation);
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected_gripper_command(
          runtime->reason_code.empty() ? "gripper_request_failed"
                                       : runtime->reason_code,
          runtime->error.empty() ? "Gripper state is unavailable"
                                 : runtime->error,
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperBusy,
          "Gripper terminal scene projection is still pending",
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected_gripper_command(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying",
          runtime->command_id, runtime->generation);
    }
    return std::nullopt;
  };
  if (auto rejected = reject_existing_state()) {
    return *rejected;
  }
  if (!gripper_request_matches_transport(request, runtime_config)) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kInvalidGripperSemantics,
        "Gripper request semantics do not match the configured transport");
  }
  std::optional<double> start_position;
  if (!runtime_config.group_id.empty() || !runtime_config.joint_name.empty()) {
    try {
      start_position = scene_group_joint_position(runtime_config.group_id,
                                                  runtime_config.joint_name);
    } catch (const std::exception &exception) {
      return rejected_gripper_command(
          "gripper_state_unavailable",
          std::string("Cannot read gripper execution state: ") +
              exception.what());
    } catch (...) {
      return rejected_gripper_command("gripper_state_unavailable",
                                      "Cannot read gripper execution state");
    }
    if (!start_position.has_value()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "Cannot project gripper '" + request.gripper_id +
              "' into execution-state sink group '" + runtime_config.group_id +
              "' joint '" + runtime_config.joint_name + "'");
    }
    if (!std::isfinite(*start_position)) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperStateNotFinite,
          "Configured gripper execution-state position is not finite");
    }
  }

  if (runtime_config.transport_kind == "ros_gripper_command") {
    if (!wait_for_action_server_until(
            runtime->action_client,
            runtime->config.wait_for_action_server_timeout_s)) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperActionServerUnavailable,
          "Timed out waiting for gripper action server " +
              runtime->config.action_name);
    }
  }

  const auto command_id =
      next_gripper_command_id_.fetch_add(1, std::memory_order_relaxed);
  const auto generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  std::optional<core::ScheduleHandle> superseded_lifecycle_handle;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_ ||
        gripper_authority_generation_ != authority_generation ||
        !runtime_is_current()) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper lifecycle authority changed during command admission");
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing) {
      return rejected_gripper_command(ros2_control_fault_reason::kGripperBusy,
                                      "Gripper " + request.gripper_id +
                                          " is already executing",
                                      runtime->command_id, runtime->generation);
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected_gripper_command(
          runtime->reason_code.empty() ? "gripper_request_failed"
                                       : runtime->reason_code,
          runtime->error.empty() ? "Gripper state is unavailable"
                                 : runtime->error,
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperBusy,
          "Gripper terminal scene projection is still pending",
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected_gripper_command(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying",
          runtime->command_id, runtime->generation);
    }
    superseded_lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->state = "executing";
    runtime->executing = true;
    runtime->command_id = command_id;
    runtime->addressed_identity = request.addressed_identity;
    runtime->generation = generation;
    ++runtime->revision;
    runtime->progress = 0.0;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->start_position = start_position;
    runtime->current_position = start_position;
    runtime->target_position = request.target_position;
    runtime->duration_s = request.duration_s;
    runtime->max_effort = request.max_effort;
    runtime->started_at_ns = clock_.now();
    runtime->deadline_at_ns.reset();
    if (runtime_config.transport_kind == "ros_gripper_command" &&
        runtime->config.result_timeout_s > 0.0) {
      runtime->deadline_at_ns = gripper_deadline(
          runtime->started_at_ns,
          gripper_duration_sum(
              gripper_duration_ns(request.duration_s),
              gripper_duration_ns(runtime->config.result_timeout_s)));
    }
    runtime->active_goal_handle.reset();
    runtime->commanded_state = request.commanded_state;
    runtime->actuation_kind = request.actuation_kind;
    runtime->state_authority = request.state_authority;
    runtime->completion_policy = request.completion_policy.empty()
                                     ? runtime_config.completion_policy
                                     : request.completion_policy;
    runtime->grasp_confirmation_policy = request.grasp_confirmation_policy;
    runtime->transport_kind = runtime_config.transport_kind;
    runtime->evidence = request.evidence;
    runtime->evidence_state = request.evidence_state;
    runtime->projection_state = runtime_config.group_id.empty()
                                    ? core::GripperProjectionState::kNotRequired
                                    : core::GripperProjectionState::kPending;
    runtime->projection_reason.clear();
    runtime->projection_retry_count = 0;
    runtime->externally_failed = false;
    runtime->error.clear();
    runtime->reason_code.clear();
  }
  if (superseded_lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*superseded_lifecycle_handle);
    } catch (...) {
      // Admission already changed the command generation; old queued work is
      // inert.
    }
  }

  if (runtime_config.transport_kind == "isaac_joint_state_effort") {
    {
      std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
      if (gripper_admission_blocked_ ||
          gripper_authority_generation_ != authority_generation ||
          !runtime_is_current()) {
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper lifecycle authority changed before command dispatch",
            command_id, generation);
      }
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (!runtime->executing || runtime->command_id != command_id ||
          runtime->generation != generation) {
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper command was invalidated before dispatch", command_id,
            generation);
      }
      try {
        publish_isaac_joint_state_gripper_command(node_, runtime_config,
                                                  runtime->joint_state_pub,
                                                  request.target_position);
      } catch (const std::exception &exc) {
        runtime->state = "failed";
        runtime->executing = false;
        runtime->goal_accepted = false;
        runtime->cancel_pending = false;
        runtime->error =
            std::string("Failed to publish Isaac gripper command: ") +
            exc.what();
        runtime->reason_code =
            ros2_control_fault_reason::kGripperGoalDispatchFailed;
        ++runtime->revision;
        return rejected_gripper_command(
            ros2_control_fault_reason::kGripperGoalDispatchFailed,
            runtime->error, command_id, generation);
      }
    }
    if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                    clock_.now())) {
      // The physical command was already published. Report truthful admission;
      // the owner-published status is now terminal failed (or was superseded by
      // a concurrent lifecycle transition) and must not be disguised as a
      // side-effect-free rejection.
      return {true, command_id, generation, std::nullopt};
    }
    return {true, command_id, generation, std::nullopt};
  }

  GripperCommand::Goal goal;
  goal.command.position = request.target_position;
  goal.command.max_effort = max_effort;

  rclcpp_action::Client<GripperCommand>::SendGoalOptions options;
  // Runtime state is generation-owned. Backend access is limited to cancelling
  // the owner-scheduled deadline; the retained host quiesces the ROS executor
  // before adapter destruction.
  options.goal_response_callback = [this, runtime, command_id,
                                    generation](const GoalHandleGripper::SharedPtr& goal_handle) {
    GoalHandleGripper::SharedPtr handle_to_cancel;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id != command_id || runtime->generation != generation) {
        handle_to_cancel = goal_handle;
        action_client = runtime->action_client;
      } else if (!goal_handle) {
        if (runtime->executing) {
          lifecycle_handle = runtime->lifecycle_handle;
          runtime->lifecycle_handle.reset();
          ++runtime->lifecycle_schedule_generation;
          runtime->state = "failed";
          runtime->executing = false;
          runtime->goal_accepted = false;
          runtime->cancel_pending = false;
          runtime->error = "Gripper action goal was rejected";
          runtime->reason_code = ros2_control_fault_reason::kGripperGoalRejected;
          runtime->projection_state = core::GripperProjectionState::kNotRequired;
          runtime->projection_reason.clear();
          ++runtime->revision;
        }
      } else if (runtime->cancel_pending || !runtime->executing) {
        lifecycle_handle = runtime->lifecycle_handle;
        runtime->lifecycle_handle.reset();
        ++runtime->lifecycle_schedule_generation;
        handle_to_cancel = goal_handle;
        action_client = runtime->action_client;
        runtime->active_goal_handle.reset();
        runtime->goal_accepted = false;
        runtime->executing = false;
        runtime->cancel_pending = false;
        runtime->state = "idle";
        runtime->error.clear();
        runtime->reason_code.clear();
        runtime->projection_state = core::GripperProjectionState::kNotRequired;
        runtime->projection_reason.clear();
        ++runtime->revision;
      } else {
        runtime->goal_accepted = true;
        runtime->active_goal_handle = goal_handle;
        ++runtime->revision;
      }
    }
    if (lifecycle_handle.has_value()) {
      try {
        scheduler_.cancel(*lifecycle_handle);
      } catch (...) {
        // The response already retired this lifecycle generation.
      }
    }
    if (action_client && handle_to_cancel) {
      try {
        (void)action_client->async_cancel_goal(handle_to_cancel);
      } catch (const std::exception&) {
        // Reset/stop remains best-effort if the remote goal disappears
        // during negotiation; the next status/result callback will
        // reconcile state.
      }
    }
  };
  options.feedback_callback =
      [this, runtime, command_id, generation](
          GoalHandleGripper::SharedPtr goal_handle,
          const std::shared_ptr<const GripperCommand::Feedback> feedback) {
        on_gripper_feedback(runtime, command_id, generation, goal_handle,
                            feedback);
      };
  options.result_callback =
      [this, runtime, command_id,
       generation](const GoalHandleGripper::WrappedResult &result) {
        on_gripper_result(runtime, command_id, generation, result);
      };

  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_ ||
        gripper_authority_generation_ != authority_generation ||
        !runtime_is_current()) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper lifecycle authority changed before command dispatch",
          command_id, generation);
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!runtime->executing || runtime->command_id != command_id ||
        runtime->generation != generation) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command was invalidated before dispatch", command_id,
          generation);
    }
    try {
      // rclcpp_action queues these callbacks on the executor; it does not
      // invoke them inline. Keeping the authority and runtime leaf locks
      // through this bounded enqueue makes dispatch linearizable with
      // reset/replacement.
      (void)runtime->action_client->async_send_goal(goal, options);
    } catch (const std::exception &exc) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->error =
          std::string("Failed to dispatch gripper action goal: ") + exc.what();
      runtime->reason_code =
          ros2_control_fault_reason::kGripperGoalDispatchFailed;
      ++runtime->revision;
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperGoalDispatchFailed,
          "Failed to dispatch gripper action goal: " + std::string(exc.what()),
          command_id, generation);
    }
  }
  if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                  clock_.now())) {
    // async_send_goal already crossed the transport boundary. Preserve the
    // admitted result and expose scheduler failure through terminal status;
    // the goal-response callback cancels a late handle for the failed owner.
    return {true, command_id, generation, std::nullopt};
  }
  return {true, command_id, generation, std::nullopt};
}

core::GripperCommandResultDTO
Ros2GripperCommandBackend::mark_gripper_grasp_lost(
    const std::string &gripper_id, std::uint64_t expected_command_id,
    std::uint64_t expected_generation, const std::string &evidence_label,
    const std::string &message, const std::string &reason_code) {
  if (gripper_id.empty() || evidence_label.empty()) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kGripperInvalidCommand,
        "gripper_id and evidence_label are required");
  }
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "No gripper transport configured for " + gripper_id);
    }
    runtime = it->second;
  }

  GoalHandleGripper::SharedPtr goal_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    const bool has_command_or_status =
        runtime->state != "idle" || runtime->current_position.has_value() ||
        runtime->observed_position.has_value() || !runtime->evidence.empty() ||
        !runtime->error.empty() || !runtime->reason_code.empty();
    if (!has_command_or_status || runtime->command_id != expected_command_id ||
        runtime->generation != expected_generation) {
      return rejected_gripper_command("stale_gripper_state",
                                      "Gripper '" + gripper_id +
                                          "' command identity changed before "
                                          "grasp-loss evidence was applied");
    }
    command_id = runtime->command_id;
    generation =
        next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
    goal_handle = runtime->active_goal_handle;
    action_client = runtime->action_client;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->generation = generation;
    runtime->active_goal_handle.reset();
    runtime->state = "failed";
    runtime->executing = false;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->externally_failed = true;
    runtime->projection_state = core::GripperProjectionState::kNotRequired;
    runtime->projection_reason.clear();
    add_unique_evidence(runtime->evidence, evidence_label);
    runtime->evidence_state = "pressure_or_part_lost";
    runtime->error =
        message.empty() ? "gripper grasp confirmation was lost" : message;
    runtime->reason_code =
        reason_code.empty() ? "grasp_confirmation_lost" : reason_code;
    ++runtime->revision;
  }

  if (lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*lifecycle_handle);
    } catch (...) {
      // The generation changed before this eager cleanup. A queued callback
      // cannot mutate the externally failed status.
    }
  }

  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel lost-grasp gripper action goal: %s",
                  exc.what());
    }
  }
  return {true, command_id, generation, std::nullopt};
}

bool Ros2GripperCommandBackend::schedule_gripper_lifecycle(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, core::TimePoint deadline_ns) {
  std::optional<core::ScheduleHandle> old_handle;
  std::uint64_t schedule_generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      return false;
    }
    old_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    schedule_generation = ++runtime->lifecycle_schedule_generation;
  }
  if (old_handle.has_value()) {
    try {
      scheduler_.cancel(*old_handle);
    } catch (...) {
      // The schedule generation already invalidated the old callback.
    }
  }

  core::ScheduleHandle handle;
  try {
    handle = scheduler_.schedule(
        deadline_ns,
        [this, runtime, command_id, generation, schedule_generation]() {
          {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            const bool lifecycle_active =
                runtime->executing ||
                (runtime->state == "completed" &&
                 runtime->projection_state ==
                     core::GripperProjectionState::kPending);
            if (runtime->command_id != command_id ||
                runtime->generation != generation ||
                runtime->lifecycle_schedule_generation != schedule_generation ||
                !lifecycle_active) {
              return;
            }
          }
          // A callback passing this fence is already executing on the scheduler
          // lane. Host shutdown invalidates generations, then drains that lane
          // before destroying the adapter. Later queued callbacks never enter
          // it.
          advance_gripper_command(runtime, command_id, generation,
                                  schedule_generation);
        },
        core::SchedulerPriority::ControllerCommand);
  } catch (...) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
      runtime->error = "Gripper lifecycle could not be scheduled";
      runtime->reason_code = ros2_control_fault_reason::kGripperRequestFailed;
      ++runtime->revision;
    }
    return false;
  }
  bool retained = false;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    const bool lifecycle_active =
        runtime->executing ||
        (runtime->state == "completed" &&
         runtime->projection_state == core::GripperProjectionState::kPending);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation &&
        lifecycle_active) {
      runtime->lifecycle_handle = handle;
      retained = true;
    }
  }
  if (!retained) {
    try {
      scheduler_.cancel(handle);
    } catch (...) {
      // A stale queued callback is generation-fenced at entry.
    }
  }
  return retained;
}

void Ros2GripperCommandBackend::advance_gripper_command(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, std::uint64_t schedule_generation) {
  GoalHandleGripper::SharedPtr goal_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  Ros2GripperActionConfig runtime_config;
  std::optional<double> scene_position;
  bool schedule_next = false;
  core::TimePoint next_deadline_ns = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation ||
        runtime->lifecycle_schedule_generation != schedule_generation) {
      return;
    }
    runtime->lifecycle_handle.reset();
    runtime_config = runtime->config;
    const auto now_ns = clock_.now();
    const auto duration_ns =
        std::max<core::Duration>(1, gripper_duration_ns(runtime->duration_s));
    const auto elapsed_ns =
        std::max<core::Duration>(0, now_ns - runtime->started_at_ns);
    runtime->progress = runtime->duration_s <= 0.0
                            ? 1.0
                            : clamp_progress(static_cast<double>(elapsed_ns) /
                                             static_cast<double>(duration_ns));

    if (runtime->executing && runtime->deadline_at_ns.has_value() &&
        now_ns >= *runtime->deadline_at_ns) {
      goal_handle = runtime->active_goal_handle;
      action_client = runtime->action_client;
      runtime->active_goal_handle.reset();
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->error = "Timed out waiting for gripper action result";
      runtime->reason_code = ros2_control_fault_reason::kGripperResultTimeout;
    } else if (runtime->executing &&
               runtime_config.transport_kind == "isaac_joint_state_effort" &&
               (runtime->duration_s <= 0.0 || elapsed_ns >= duration_ns)) {
      runtime->state = "completed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->progress = 1.0;
      runtime->error.clear();
      runtime->reason_code.clear();
      runtime->current_position = runtime->target_position;
      scene_position = runtime->target_position;
    } else if (!runtime_config.group_id.empty() &&
               !runtime_config.joint_name.empty()) {
      if (runtime->executing && is_commanded_sim_mode() &&
          runtime->start_position.has_value()) {
        scene_position = *runtime->start_position +
                         (runtime->target_position - *runtime->start_position) *
                             runtime->progress;
        runtime->current_position = scene_position;
      } else if (!runtime->executing &&
                 runtime->projection_state ==
                     core::GripperProjectionState::kPending) {
        // ROS action feedback/result callbacks own live observed projection.
        // The lifecycle timer only interpolates commanded-sim state or retries
        // a terminal projection; replaying an unchanged feedback sample at
        // 100 Hz would create avoidable scene commits and contention.
        if (runtime->current_position.has_value()) {
          scene_position = *runtime->current_position;
        } else if (runtime->start_position.has_value()) {
          scene_position = *runtime->start_position;
        }
      }
    }
    ++runtime->revision;
    schedule_next = runtime->executing;
    next_deadline_ns = now_ns + 10'000'000;
    if (runtime->executing &&
        runtime_config.transport_kind == "isaac_joint_state_effort") {
      next_deadline_ns =
          std::min(next_deadline_ns,
                   gripper_deadline(runtime->started_at_ns, duration_ns));
    } else if (runtime->deadline_at_ns.has_value()) {
      next_deadline_ns = std::min(next_deadline_ns, *runtime->deadline_at_ns);
    }
  }

  if (scene_position.has_value()) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation) {
      const bool terminal = !runtime->executing;
      const bool queued = schedule_gripper_scene_projection_locked(
          runtime, *scene_position, terminal);
      if (!queued && terminal) {
        ++runtime->projection_retry_count;
        if (runtime->projection_retry_count >= 10) {
          runtime->projection_state = core::GripperProjectionState::kFailed;
          runtime->projection_reason = "scene_sink_retry_exhausted";
          runtime->state = "failed";
          runtime->error =
              "Timed out queueing the terminal gripper scene position";
          runtime->reason_code = "gripper_projection_failed";
        } else {
          schedule_next = true;
          next_deadline_ns = clock_.now() + 20'000'000;
        }
      }
      ++runtime->revision;
    }
  }
  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel timed-out gripper action goal: %s",
                  exc.what());
    }
  }
  if (schedule_next) {
    (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                     next_deadline_ns);
  }
}

core::GripperCommandStatusDTO Ros2GripperCommandBackend::get_gripper_status(
    const std::string &gripper_id) const {
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      core::GripperCommandStatusDTO status;
      status.gripper_id = gripper_id;
      return status;
    }
    runtime = it->second;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  core::GripperCommandStatusDTO status;
  status.state =
      runtime->projection_state == core::GripperProjectionState::kFailed
          ? core::GripperCommandState::kFailed
      : runtime->state == "completed" &&
              runtime->projection_state ==
                  core::GripperProjectionState::kPending
          ? core::GripperCommandState::kExecuting
      : runtime->state == "executing" ? core::GripperCommandState::kExecuting
      : runtime->state == "completed" ? core::GripperCommandState::kCompleted
      : runtime->state == "failed"    ? core::GripperCommandState::kFailed
                                      : core::GripperCommandState::kIdle;
  status.gripper_id = gripper_id;
  status.command_id = runtime->command_id;
  status.generation = runtime->generation;
  status.revision = runtime->revision;
  status.progress = runtime->progress;
  status.current_position = runtime->current_position;
  // Return the last owner-published observation verbatim. Freshness is an
  // admission/evidence concern at the owning callback boundary; deriving it
  // from wall time here would make this supposedly immutable status change
  // without a publication or revision increment.
  status.observed_position = runtime->observed_position;
  status.target_position = runtime->target_position;
  if (runtime->command_id != 0) {
    status.commanded_position = runtime->target_position;
  }
  status.commanded_state = runtime->commanded_state;
  status.actuation_kind = runtime->actuation_kind;
  status.state_authority = runtime->state_authority;
  status.completion_policy = runtime->completion_policy;
  status.grasp_confirmation_policy = runtime->grasp_confirmation_policy;
  status.transport_kind = runtime->transport_kind;
  status.evidence = runtime->evidence;
  status.evidence_state = runtime->evidence_state;
  status.projection_state = runtime->projection_state;
  status.projection_reason = runtime->projection_reason;
  if (!runtime->error.empty() || !runtime->reason_code.empty()) {
    status.error =
        core::ErrorDTO{runtime->reason_code.empty() ? "gripper_request_failed"
                                                    : runtime->reason_code,
                       runtime->error,
                       {}};
  }
  return status;
}

} // namespace manyforge::ros2_control_execution
