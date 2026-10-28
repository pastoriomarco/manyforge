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

#include "scene_projection_worker.hpp"
#include "transport_helpers.hpp"
#include <action_msgs/srv/cancel_goal.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/time/stuck_drain_wait.hpp>
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

bool requires_pneumatic_commanded_projection(
    const Ros2GripperActionConfig &config) noexcept {
  return config.actuation_kind == core::GripperActuationKind::kPneumatic &&
         config.output_mode == core::GripperOutputMode::kNone &&
         config.state_topic.empty() && !config.group_id.empty();
}

bool gripper_request_matches_transport(
    const core::GripperCommandRequestDTO &request,
    const Ros2GripperActionConfig &config) noexcept {
  const auto selects = [](const std::string &requested,
                          const std::string &physical,
                          const std::string_view resolved) {
    return requested.empty() || requested == physical || requested == resolved;
  };
  const bool pneumatic =
      config.actuation_kind == core::GripperActuationKind::kPneumatic;
  const bool no_output = config.output_mode == core::GripperOutputMode::kNone;
  if (pneumatic) {
    const std::string_view policy = no_output ? "no_output" : "output_delivery";
    if (!request.completion_policy.empty() &&
        request.completion_policy != policy)
      return false;
  }
  if (no_output && (!request.grasp_confirmation_policy.empty() &&
                    request.grasp_confirmation_policy != "not_required"))
    return false;
  return (request.group_id.empty() || request.group_id == config.group_id) &&
         (request.joint_name.empty() ||
          request.joint_name == config.joint_name) &&
         selects(request.transport_kind, config.transport_kind,
                 "resolved_binding") &&
         (pneumatic ||
          selects(request.completion_policy, config.completion_policy,
                  "goal_fenced_target_reached"));
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
  core::WaitForDrain("gripper_backend.scheduler_controller_command_lane",
                     std::chrono::seconds(1),
                     [this](std::chrono::nanoseconds bound) {
                       return scheduler_.drain_for(
                           core::SchedulerPriority::ControllerCommand, bound);
                     });
}

void Ros2GripperCommandBackend::advance_pneumatic_locked(
    GripperRuntime &runtime) const {
  if (!runtime.executing)
    return;
  const auto now = clock_.now();
  if (runtime.config.output_mode == core::GripperOutputMode::kNone ||
      runtime.delivered_at_ns) {
    runtime.state = "completed";
    runtime.executing = false;
    runtime.progress = 1.0;
    runtime.error.clear();
    runtime.reason_code.clear();
    ++runtime.revision;
  } else if (runtime.deadline_at_ns && now >= *runtime.deadline_at_ns) {
    runtime.state = "failed";
    runtime.executing = false;
    // Exact late callbacks cannot flip Failure.
    runtime.externally_failed = true;
    runtime.error = "Timed out waiting for gripper action result";
    runtime.reason_code = ros2_control_fault_reason::kGripperResultTimeout;
    add_unique_evidence(runtime.evidence,
                        "pneumatic_output_delivery_unconfirmed");
    ++runtime.revision;
  }
}

void Ros2GripperCommandBackend::confirm_pneumatic_delivery_locked(
    GripperRuntime &runtime) {
  if (!runtime.executing || runtime.delivered_at_ns)
    return;
  runtime.delivered_at_ns = clock_.now();
  // Delivery is the complete pneumatic backend transaction.
  runtime.deadline_at_ns.reset();
  add_unique_evidence(runtime.evidence, "pneumatic_output_delivery_confirmed");
  ++runtime.revision;
  advance_pneumatic_locked(runtime);
}

void Ros2GripperCommandBackend::publish_pneumatic_display(
    const std::shared_ptr<GripperRuntime> &runtime,
    double fallback_position) noexcept {
  try {
    std::optional<double> observed;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->observed_at &&
          std::chrono::steady_clock::now() - *runtime->observed_at <=
              std::chrono::duration<double>(
                  runtime->config.state_freshness_timeout_s))
        observed = runtime->observed_position;
    }
    if (runtime->config.publish_display_position)
      (void)runtime->config.publish_display_position(
          observed.value_or(fallback_position));
  } catch (...) {
    // Display/cache availability is separate from delivery.
  }
}

core::GripperActuationKind Ros2GripperCommandBackend::gripper_actuation_kind(
    const std::string &gripper_id) const {
  std::lock_guard<std::mutex> lock(gripper_map_mutex_);
  const auto found = grippers_.find(gripper_id);
  return found == grippers_.end()
             ? core::GripperActuationKind::kPositionCommanded
             : found->second->config.actuation_kind;
}

core::GripperOutputMode Ros2GripperCommandBackend::gripper_output_mode(
    const std::string &gripper_id) const {
  std::lock_guard<std::mutex> lock(gripper_map_mutex_);
  const auto found = grippers_.find(gripper_id);
  return found == grippers_.end() ? core::GripperOutputMode::kCommanded
                                  : found->second->config.output_mode;
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
  if (config.output_mode != core::GripperOutputMode::kCommanded &&
      config.output_mode != core::GripperOutputMode::kNone)
    throw std::invalid_argument("gripper output_mode is invalid");
  const bool no_output = config.output_mode == core::GripperOutputMode::kNone;
  if (no_output) {
    if (config.actuation_kind != core::GripperActuationKind::kPneumatic)
      throw std::invalid_argument("no output requires pneumatic actuation");
    if (!config.transport_kind.empty() || !config.action_name.empty() ||
        !config.command_topic.empty() || !config.command_joint_names.empty())
      throw std::invalid_argument(
          "no-output gripper must not configure a ROS command transport");
    if (config.completion_policy != "no_output")
      throw std::invalid_argument(
          "no-output gripper completion must be no_output");
    if (!config.state_topic.empty() && config.state_joint_name.empty())
      throw std::invalid_argument(
          "no-output display topic requires an exact raw state joint");
  } else {
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
        throw std::invalid_argument(
            "gripper command_topic must not be empty for "
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
        throw std::invalid_argument("isaac_joint_state_effort gripper "
                                    "positions and efforts are invalid");
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
  }
  const bool pneumatic =
      config.actuation_kind == core::GripperActuationKind::kPneumatic;
  if (!config.result_timeout_s)
    config.result_timeout_s = pneumatic ? 1.0 : 5.0;
  if (!std::isfinite(config.state_freshness_timeout_s) ||
      !std::isfinite(config.wait_for_action_server_timeout_s) ||
      !std::isfinite(*config.result_timeout_s) ||
      (config.state_freshness_timeout_s <= 0.0 &&
       !(no_output && config.state_topic.empty() &&
         config.state_freshness_timeout_s == 0.0)) ||
      config.wait_for_action_server_timeout_s < 0.0 ||
      *config.result_timeout_s < 0.0 ||
      (pneumatic && *config.result_timeout_s == 0.0)) {
    throw std::invalid_argument("gripper action timing config is invalid");
  }
  if (pneumatic) {
    if (config.completion_policy !=
        (no_output ? "no_output" : "output_delivery"))
      throw std::invalid_argument(
          "gripper completion_policy does not match actuation");
  } else if (config.completion_policy != "position_reached" &&
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
  if (runtime->config.output_mode == core::GripperOutputMode::kNone) {
    // The genuine local owner has no ROS command entities.
  } else if (runtime->config.transport_kind == "ros_gripper_command") {
    runtime->action_client = rclcpp_action::create_client<GripperCommand>(
        node_, runtime->config.action_name);
  } else {
    runtime->joint_state_pub =
        node_->create_publisher<sensor_msgs::msg::JointState>(
            runtime->config.command_topic, rclcpp::SystemDefaultsQoS());
  }
  if (!runtime->config.state_topic.empty() &&
      (runtime->config.transport_kind == "isaac_joint_state_effort" ||
       runtime->config.actuation_kind ==
           core::GripperActuationKind::kPneumatic)) {
    const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
    runtime->joint_state_sub =
        node_->create_subscription<sensor_msgs::msg::JointState>(
            runtime->config.state_topic, rclcpp::SystemDefaultsQoS(),
            [weak_runtime](const sensor_msgs::msg::JointState::SharedPtr msg) {
              const auto runtime = weak_runtime.lock();
              if (!runtime || !msg) {
                return;
              }
              const auto &observed_joint = runtime->config.state_joint_name;
              if (observed_joint.empty()) {
                return;
              }
              const auto position = position_for_joint(*msg, observed_joint);
              // CP9 liveness: lock-free, no allocation.
              if (position.has_value() && runtime->config.feedback_stream)
                runtime->config.feedback_stream->RecordArrival();
              {
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
                if (runtime->config.actuation_kind !=
                    core::GripperActuationKind::kPneumatic)
                  ++runtime->revision;
              }
              if (runtime->config.actuation_kind ==
                      core::GripperActuationKind::kPneumatic &&
                  runtime->config.publish_display_position) {
                try {
                  (void)runtime->config.publish_display_position(*position);
                } catch (...) { /* Display never changes an I/O result. */
                }
              }
            });
  }
  runtime->generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  if (runtime->config.actuation_kind !=
          core::GripperActuationKind::kPneumatic &&
      !runtime->config.group_id.empty()) {
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
        const auto command_id = runtime->lifecycle_disable_command_id;
        const auto generation = runtime->lifecycle_disable_generation;
        const auto goal_id = goal_handle->get_goal_id();
        (void)action_client->async_cancel_goal(
            goal_handle,
            [weak_runtime = std::weak_ptr<GripperRuntime>(runtime), command_id,
             generation, goal_id](const auto response) {
              const auto runtime = weak_runtime.lock();
              if (!runtime)
                return;
              std::lock_guard lock(runtime->mutex);
              if (!runtime->lifecycle_disable_pending ||
                  runtime->lifecycle_disable_command_id != command_id ||
                  runtime->lifecycle_disable_generation != generation)
                return;
              const bool accepted = response &&
                  response->return_code ==
                      action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
                  std::any_of(response->goals_canceling.begin(),
                              response->goals_canceling.end(),
                              [&](const auto &row) {
                                return row.goal_id.uuid == goal_id;
                              });
              if (!accepted && !runtime->lifecycle_disable_terminal)
                runtime->lifecycle_disable_failed = true;
              runtime->lifecycle_condition.notify_all();
            });
      } catch (const std::exception &exc) {
        {
          std::lock_guard lock(runtime->mutex);
          if (runtime->lifecycle_disable_pending)
            runtime->lifecycle_disable_failed = true;
          runtime->lifecycle_condition.notify_all();
        }
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
  if (runtime->config.actuation_kind ==
      core::GripperActuationKind::kPneumatic) {
    // Finger position/reached_goal never establishes pneumatic delivery or
    // state.
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
  if (runtime->config.actuation_kind ==
      core::GripperActuationKind::kPneumatic) {
    std::optional<core::ScheduleHandle> previous;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id != command_id ||
          runtime->generation != generation)
        return;
      advance_pneumatic_locked(*runtime);
      if (runtime->externally_failed) {
        if (result.code == rclcpp_action::ResultCode::SUCCEEDED &&
            result.result) {
          const auto count = runtime->evidence.size();
          add_unique_evidence(runtime->evidence,
                              "ros_gripper_command_late_result_succeeded");
          if (runtime->evidence.size() != count)
            ++runtime->revision;
        }
        // Preserve the first reported Failure/error.
        return;
      }
      if (!runtime->executing || runtime->delivered_at_ns)
        return;
      previous = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      runtime->active_goal_handle.reset();
      runtime->goal_accepted = false;
      if (result.code == rclcpp_action::ResultCode::SUCCEEDED &&
          result.result) {
        add_unique_evidence(runtime->evidence,
                            "ros_gripper_command_result_succeeded");
        confirm_pneumatic_delivery_locked(*runtime);
      } else {
        runtime->state = "failed";
        runtime->executing = false;
        runtime->externally_failed = true;
        runtime->error = result.code == rclcpp_action::ResultCode::ABORTED
                             ? "Gripper action aborted"
                             : "Gripper output delivery was not confirmed";
        runtime->reason_code =
            result.code == rclcpp_action::ResultCode::ABORTED
                ? ros2_control_fault_reason::kGripperGoalAborted
                : ros2_control_fault_reason::kGripperResultFailed;
        add_unique_evidence(runtime->evidence,
                            "pneumatic_output_delivery_unconfirmed");
        ++runtime->revision;
      }
    }
    if (previous) {
      try {
        scheduler_.cancel(*previous);
      } catch (...) {
      }
    }
    return;
  }
  Ros2GripperActionConfig runtime_config;
  std::optional<double> result_position;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      if (runtime->lifecycle_disable_pending &&
          runtime->lifecycle_disable_command_id == command_id &&
          runtime->lifecycle_disable_generation == generation) {
        runtime->lifecycle_disable_terminal = true;
        runtime->lifecycle_disable_failed =
            result.code == rclcpp_action::ResultCode::ABORTED ||
            result.code == rclcpp_action::ResultCode::UNKNOWN;
        runtime->cancel_pending = false;
        runtime->lifecycle_condition.notify_all();
      }
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
    if (runtime->lifecycle_disable_pending &&
        runtime->lifecycle_disable_command_id == command_id &&
        runtime->lifecycle_disable_generation == generation) {
      runtime->lifecycle_disable_terminal = true;
      runtime->lifecycle_disable_failed =
          result.code == rclcpp_action::ResultCode::ABORTED ||
          result.code == rclcpp_action::ResultCode::UNKNOWN;
    }
    ++runtime->revision;
  }
  runtime->lifecycle_condition.notify_all();
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
    core::GripperInvalidationReason reason) noexcept {
  std::vector<std::shared_ptr<GripperRuntime>> runtimes;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    gripper_motion_admission_blocked_ = true;
    ++gripper_authority_generation_;
    if (reason != core::GripperInvalidationReason::kStop) {
      gripper_admission_blocked_ = true;
      ++gripper_lifecycle_generation_;
    }
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
      if (reason == core::GripperInvalidationReason::kStop &&
          runtime->config.actuation_kind ==
              core::GripperActuationKind::kPneumatic) {
        // A motion Stop has no authority over pneumatic transactions or idle
        // pneumatic admission. Destructive lifecycle fences remain separate.
        continue;
      }
      goal_handle = runtime->active_goal_handle;
      // A local no-output command has no physical action goal to cancel.
      const bool command_unsettled =
          (runtime->executing &&
           runtime->config.output_mode != core::GripperOutputMode::kNone) ||
          goal_handle;
      // Only a ROS action goal has a remote terminal to settle. A scheduled
      // (Isaac publish) command is settled by this generation fence itself.
      if (command_unsettled &&
          runtime->config.transport_kind == "ros_gripper_command" &&
          !runtime->lifecycle_disable_pending) {
        runtime->lifecycle_disable_pending = true;
        runtime->lifecycle_disable_terminal = false;
        runtime->lifecycle_disable_failed = false;
        runtime->lifecycle_cancel_dispatched = false;
        runtime->lifecycle_disable_command_id = runtime->command_id;
        runtime->lifecycle_disable_generation = runtime->generation;
      }
      if (goal_handle && runtime->lifecycle_disable_pending) {
        if (runtime->lifecycle_cancel_dispatched) {
          goal_handle.reset();
        } else {
          runtime->lifecycle_cancel_dispatched = true;
        }
      }
      action_client = runtime->action_client;
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = command_unsettled;
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
    runtime->lifecycle_condition.notify_all();
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
  (void)try_authorize_gripper_commands();
}

core::Result<void>
Ros2GripperCommandBackend::try_authorize_gripper_commands() noexcept {
  std::lock_guard<std::mutex> lock(gripper_authority_mutex_);
  {
    std::lock_guard map_lock(gripper_map_mutex_);
    for (const auto &row : grippers_) {
      std::lock_guard runtime_lock(row.second->mutex);
      if (!row.second->lifecycle_disable_pending) continue;
      if (row.second->lifecycle_disable_terminal &&
          !row.second->lifecycle_disable_failed) {
        row.second->lifecycle_disable_pending = false;
        continue;
      }
      return core::Result<void>::Err(core::ErrorDTO{
          "ros2_control_gripper_bridge_disable_unsettled",
          "The prior gripper cancellation remains unsettled", {}});
    }
  }
  if (!gripper_admission_blocked_ && !gripper_motion_admission_blocked_) {
    return core::Result<void>::Ok();
  }
  ++gripper_authority_generation_;
  gripper_admission_blocked_ = false;
  gripper_motion_admission_blocked_ = false;
  return core::Result<void>::Ok();
}

Ros2GripperLifecycleResult
Ros2GripperCommandBackend::authorize_lifecycle_gripper(
    const std::string &gripper_id) {
  std::lock_guard authority_lock(gripper_authority_mutex_);
  std::lock_guard map_lock(gripper_map_mutex_);
  const auto found = grippers_.find(gripper_id);
  if (found == grippers_.end())
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_configured",
        "The exact gripper command bridge is not configured", {}}};
  std::lock_guard runtime_lock(found->second->mutex);
  if (found->second->lifecycle_disable_pending)
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The prior gripper cancellation remains unsettled", {}}};
  ++gripper_authority_generation_;
  gripper_admission_blocked_ = false;
  gripper_motion_admission_blocked_ = false;
  return {true, true, std::nullopt};
}

bool Ros2GripperCommandBackend::lifecycle_disable_pending(
    const std::string &gripper_id) const {
  std::lock_guard map_lock(gripper_map_mutex_);
  const auto found = grippers_.find(gripper_id);
  if (found == grippers_.end()) return true;
  std::lock_guard runtime_lock(found->second->mutex);
  return found->second->lifecycle_disable_pending;
}

Ros2GripperLifecycleResult Ros2GripperCommandBackend::prepare_lifecycle_gripper(
    const std::string &gripper_id) {
  const auto readiness = gripper_transport_readiness(gripper_id);
  if (!readiness.configured || !readiness.ready ||
      readiness.transport_kind != "ros_gripper_command")
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The exact ROS action gripper command bridge is not ready", {}}};
  std::lock_guard authority_lock(gripper_authority_mutex_);
  std::lock_guard map_lock(gripper_map_mutex_);
  const auto found = grippers_.find(gripper_id);
  if (found == grippers_.end())
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_configured",
        "The exact gripper command bridge is not configured", {}}};
  std::lock_guard runtime_lock(found->second->mutex);
  if (found->second->executing || found->second->goal_accepted ||
      found->second->active_goal_handle ||
      found->second->cancel_pending ||
      found->second->lifecycle_disable_pending)
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_busy",
        "The gripper command bridge is not idle for lifecycle preparation",
        {}}};
  gripper_admission_blocked_ = true;
  gripper_motion_admission_blocked_ = true;
  ++gripper_authority_generation_;
  ++gripper_lifecycle_generation_;
  return {true, true, std::nullopt};
}

Ros2GripperLifecycleResult Ros2GripperCommandBackend::disable_lifecycle_gripper(
    const std::string &gripper_id, const std::stop_token cancellation) {
  std::shared_ptr<GripperRuntime> runtime;
  GoalHandleGripper::SharedPtr goal;
  rclcpp_action::Client<GripperCommand>::SharedPtr client;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end())
      return {false, false, core::ErrorDTO{
          "ros2_control_gripper_bridge_not_configured",
          "The exact gripper command bridge is not configured", {}}};
    runtime = found->second;
    std::lock_guard runtime_lock(runtime->mutex);
    if (runtime->config.transport_kind != "ros_gripper_command")
      return {false, false, core::ErrorDTO{
          "ros2_control_gripper_bridge_transport_unsupported",
          "Lifecycle disable requires the exact ROS action gripper bridge",
          {}}};
    if (runtime->lifecycle_disable_pending) {
      if (runtime->lifecycle_disable_terminal) {
        const bool success = !runtime->lifecycle_disable_failed;
        runtime->lifecycle_disable_pending = false;
        return {success, true,
                success ? std::optional<core::ErrorDTO>{}
                        : std::optional<core::ErrorDTO>{core::ErrorDTO{
                              "ros2_control_gripper_bridge_disable_unsettled",
                              "The retained gripper cancellation did not settle successfully",
                              {}}}};
      }
      command_id = runtime->lifecycle_disable_command_id;
      generation = runtime->lifecycle_disable_generation;
      goal = runtime->active_goal_handle;
      client = runtime->action_client;
    } else {
    gripper_admission_blocked_ = true;
    gripper_motion_admission_blocked_ = true;
    ++gripper_authority_generation_;
    ++gripper_lifecycle_generation_;
    if (!runtime->executing && !runtime->goal_accepted &&
        !runtime->active_goal_handle)
      return {true, true, std::nullopt};
    runtime->lifecycle_disable_pending = true;
    runtime->lifecycle_disable_terminal = false;
    runtime->lifecycle_disable_failed = false;
    runtime->lifecycle_cancel_dispatched = false;
    runtime->cancel_pending = true;
    command_id = runtime->command_id;
    generation = runtime->generation;
    runtime->lifecycle_disable_command_id = command_id;
    runtime->lifecycle_disable_generation = generation;
    goal = runtime->active_goal_handle;
    client = runtime->action_client;
    }
  }

  std::unique_lock runtime_lock(runtime->mutex);
  if (!goal) {
    runtime->lifecycle_condition.wait(runtime_lock, cancellation, [&] {
      return shutdown_started_.load() ||
             runtime->lifecycle_disable_terminal ||
             runtime->lifecycle_disable_failed ||
             runtime->command_id != command_id ||
             runtime->generation != generation || runtime->active_goal_handle;
    });
    goal = runtime->active_goal_handle;
    client = runtime->action_client;
  }
  if (runtime->lifecycle_disable_terminal &&
      !runtime->lifecycle_disable_failed) {
    runtime->lifecycle_disable_pending = false;
    return {true, true, std::nullopt};
  }
  if (shutdown_started_.load() ||
      runtime->command_id != command_id || runtime->generation != generation ||
      runtime->lifecycle_disable_failed || !goal || !client) {
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The exact gripper command did not establish a terminal cancellation outcome",
        {}}};
  }
  if (runtime->lifecycle_cancel_dispatched) {
    runtime->lifecycle_condition.wait(runtime_lock, cancellation, [&] {
      return runtime->lifecycle_disable_terminal ||
             runtime->lifecycle_disable_failed;
    });
    if (runtime->lifecycle_disable_terminal &&
        !runtime->lifecycle_disable_failed) {
      runtime->lifecycle_disable_pending = false;
      return {true, true, std::nullopt};
    }
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The exact gripper cancellation remains unsettled", {}}};
  }
  runtime->lifecycle_cancel_dispatched = true;
  runtime_lock.unlock();
  try {
    const auto goal_id = goal->get_goal_id();
    (void)client->async_cancel_goal(
        goal, [weak_runtime = std::weak_ptr<GripperRuntime>(runtime), command_id,
               generation, goal_id](const auto response) {
          const auto runtime = weak_runtime.lock();
          if (!runtime)
            return;
          std::lock_guard lock(runtime->mutex);
          if (!runtime->lifecycle_disable_pending ||
              runtime->command_id != command_id ||
              runtime->generation != generation)
            return;
          const bool accepted = response &&
              response->return_code ==
                  action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
              std::any_of(response->goals_canceling.begin(),
                          response->goals_canceling.end(), [&](const auto &row) {
                            return row.goal_id.uuid == goal_id;
                          });
          if (!accepted) runtime->lifecycle_disable_failed = true;
          runtime->lifecycle_condition.notify_all();
        });
  } catch (...) {
    runtime_lock.lock();
    runtime->lifecycle_disable_failed = true;
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_cancel_failed",
        "The gripper cancellation request could not be dispatched", {}}};
  }
  runtime_lock.lock();
  runtime->lifecycle_condition.wait(runtime_lock, cancellation, [&] {
    return shutdown_started_.load() ||
           runtime->lifecycle_disable_terminal ||
           runtime->lifecycle_disable_failed ||
           runtime->command_id != command_id ||
           runtime->generation != generation;
  });
  const bool settled = runtime->lifecycle_disable_terminal &&
                       !runtime->lifecycle_disable_failed &&
                       runtime->command_id == command_id &&
                       runtime->generation == generation;
  if (settled) {
    runtime->lifecycle_disable_pending = false;
    return {true, true, std::nullopt};
  }
  return {false, true, core::ErrorDTO{
      "ros2_control_gripper_bridge_disable_unsettled",
      "The exact gripper command did not establish a terminal cancellation outcome",
      {}}};
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
  result.output_mode = runtime_config.output_mode;
  result.transport_kind = runtime_config.transport_kind;
  result.state_topic = runtime_config.state_topic;
  result.state_joint_name = runtime_config.state_joint_name;
  result.configured = true;

  try {
    if (runtime_config.output_mode == core::GripperOutputMode::kNone) {
      result.logical_owner_ready =
          runtime_config.actuation_kind ==
              core::GripperActuationKind::kPneumatic &&
          !shutdown_started_.load(std::memory_order_acquire);
      // No action server, command endpoint, feedback gate or physical-ready
      // claim.
    } else if (runtime_config.transport_kind == "ros_gripper_command") {
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
        if (observed_at.has_value() && runtime_config.feedback_stream) {
          // CP9: the pushed `gripper_feedback` stream owns staleness.
          result.feedback_path_ready = true;
        } else if (observed_at.has_value()) {
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
      result.logical_owner_ready = false;
      result.command_path_ready = false;
      result.feedback_path_ready = false;
      result.reason_codes.push_back("gripper_transport_replaced");
    }
  }
  std::sort(result.reason_codes.begin(), result.reason_codes.end());
  result.reason_codes.erase(
      std::unique(result.reason_codes.begin(), result.reason_codes.end()),
      result.reason_codes.end());
  // Readiness describes actual physical transport for commanded output, or
  // the explicitly configured local owner for none. It cannot reopen admission.
  // Including admission here would create a startup cycle: CCLC needs readiness
  // before it selects the route whose existing reset flow opens the gate.
  result.ready = result.output_mode == core::GripperOutputMode::kNone
                     ? result.configured && result.logical_owner_ready &&
                           result.reason_codes.empty()
                     : result.configured && result.command_path_ready &&
                           result.feedback_path_ready &&
                           result.reason_codes.empty();
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
    if (gripper_motion_admission_blocked_ &&
        runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic)
      return rejected(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
  }

  Ros2GripperActionConfig runtime_config;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime_config = runtime->config;
    if (runtime->executing) {
      return rejected(ros2_control_fault_reason::kGripperBusy,
                      "Gripper is already executing");
    }
    if (runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic &&
        runtime->state == "failed" &&
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

  if (runtime_config.actuation_kind != core::GripperActuationKind::kPneumatic &&
      (!runtime_config.group_id.empty() ||
       !runtime_config.joint_name.empty())) {
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
  const bool owner_proof_supplied =
      request.committed_owner_is_current ||
      request.owner_lifecycle_generation.has_value();
  // This private proof is queried only during this synchronous call; it is
  // never retained in runtime state or substituted by an addressed identity.
  const auto committed_owner_current = [&]() noexcept {
    if (!runtime ||
        runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic ||
        !request.addressed_identity || !request.committed_owner_is_current ||
        !request.owner_lifecycle_generation ||
        *request.owner_lifecycle_generation != gripper_lifecycle_generation_)
      return false;
    try {
      return request.committed_owner_is_current(*request.addressed_identity);
    } catch (...) {
      return false;
    }
  };
  std::uint64_t authority_generation = 0;
  std::uint64_t lifecycle_generation = 0;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    }
    authority_generation = gripper_authority_generation_;
    lifecycle_generation = gripper_lifecycle_generation_;
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(request.gripper_id);
    if (it == grippers_.end()) {
      if (gripper_admission_blocked_)
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper command admission is closed by the execution lifecycle");
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "No gripper transport configured for " + request.gripper_id);
    }
    runtime = it->second;
    if (gripper_motion_admission_blocked_ &&
        runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic)
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    if (owner_proof_supplied && !committed_owner_current())
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper committed owner is no longer current");
  }
  const auto admission_authorized = [&]() noexcept {
    if (gripper_admission_blocked_)
      return false;
    if (runtime->config.actuation_kind ==
        core::GripperActuationKind::kPneumatic)
      return owner_proof_supplied
                 ? committed_owner_current()
                 : gripper_lifecycle_generation_ == lifecycle_generation;
    return !gripper_motion_admission_blocked_ &&
           gripper_authority_generation_ == authority_generation;
  };
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
    if (runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic &&
        runtime->state == "failed" &&
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
  if (runtime_config.actuation_kind != core::GripperActuationKind::kPneumatic &&
      (!runtime_config.group_id.empty() ||
       !runtime_config.joint_name.empty())) {
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
    const bool ready =
        runtime_config.actuation_kind == core::GripperActuationKind::kPneumatic
            ? runtime->action_client->action_server_is_ready()
            : wait_for_action_server_until(
                  runtime->action_client,
                  runtime_config.wait_for_action_server_timeout_s);
    if (!ready) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperActionServerUnavailable,
          runtime_config.actuation_kind ==
                  core::GripperActuationKind::kPneumatic
              ? "Gripper action server is unavailable: " +
                    runtime_config.action_name
              : "Timed out waiting for gripper action server " +
                    runtime_config.action_name);
    }
  }

  const auto command_id =
      next_gripper_command_id_.fetch_add(1, std::memory_order_relaxed);
  const auto generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  std::optional<core::ScheduleHandle> superseded_lifecycle_handle;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (!admission_authorized() || !runtime_is_current()) {
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
    if (runtime->config.actuation_kind !=
            core::GripperActuationKind::kPneumatic &&
        runtime->state == "failed" &&
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
    runtime->delivered_at_ns.reset();
    if (runtime_config.transport_kind == "ros_gripper_command" &&
        *runtime->config.result_timeout_s > 0.0) {
      runtime->deadline_at_ns = gripper_deadline(
          runtime->started_at_ns,
          gripper_duration_sum(
              runtime_config.actuation_kind ==
                      core::GripperActuationKind::kPneumatic
                  ? 0
                  : gripper_duration_ns(request.duration_s),
              gripper_duration_ns(*runtime->config.result_timeout_s)));
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
    runtime->projection_state = (runtime_config.group_id.empty() ||
                                 (runtime_config.actuation_kind ==
                                      core::GripperActuationKind::kPneumatic &&
                                  !requires_pneumatic_commanded_projection(
                                      runtime_config)))
                                    ? core::GripperProjectionState::kNotRequired
                                    : core::GripperProjectionState::kPending;
    runtime->projection_reason.clear();
    runtime->projection_retry_count = 0;
    runtime->externally_failed = false;
    runtime->error.clear();
    runtime->reason_code.clear();
    if (runtime_config.output_mode == core::GripperOutputMode::kNone) {
      add_unique_evidence(runtime->evidence, "pneumatic_no_output");
      if (!requires_pneumatic_commanded_projection(runtime_config))
        advance_pneumatic_locked(*runtime);
    }
  }
  if (superseded_lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*superseded_lifecycle_handle);
    } catch (...) {
      // Admission already changed the command generation; old queued work is
      // inert.
    }
  }

  if (runtime_config.output_mode == core::GripperOutputMode::kNone) {
    if (requires_pneumatic_commanded_projection(runtime_config)) {
      {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        if (runtime->command_id != command_id ||
            runtime->generation != generation || !runtime->executing)
          return {true, command_id, generation, std::nullopt};
      }
      // Do not hold gripper locks while the existing state owner waits for the
      // model. Reset can retire this generation while publication is in flight.
      core::ExecutionStateSinkResult projected{
          core::ExecutionStateSinkStatus::kException};
      try {
        projected = execution_state_sink_->apply_group_joint_position(
            runtime_config.group_id, runtime_config.joint_name,
            request.target_position, core::ExecutionStateUpdateMode::kBlocking);
      } catch (...) {
        // Report the existing typed projection failure below.
      }
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id != command_id ||
          runtime->generation != generation || !runtime->executing)
        return {true, command_id, generation, std::nullopt};
      if (projected.applied()) {
        runtime->projection_state = core::GripperProjectionState::kApplied;
        advance_pneumatic_locked(*runtime);
      } else {
        runtime->projection_state = core::GripperProjectionState::kFailed;
        runtime->projection_reason = "scene_projection_unavailable";
        runtime->state = "failed";
        runtime->executing = false;
        runtime->error = "Could not publish the commanded gripper scene position";
        runtime->reason_code = "gripper_projection_failed";
        ++runtime->revision;
      }
    } else {
      publish_pneumatic_display(runtime, request.target_position);
    }
    return {true, command_id, generation, std::nullopt};
  }

  if (runtime_config.transport_kind == "isaac_joint_state_effort") {
    {
      std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
      if (!admission_authorized() || !runtime_is_current()) {
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
        if (runtime_config.actuation_kind ==
            core::GripperActuationKind::kPneumatic) {
          add_unique_evidence(runtime->evidence,
                              "isaac_gripper_command_published");
          confirm_pneumatic_delivery_locked(*runtime);
        }
      } catch (const std::exception &exc) {
        runtime->state = "failed";
        runtime->executing = false;
        if (runtime_config.actuation_kind ==
            core::GripperActuationKind::kPneumatic) {
          runtime->externally_failed = true;
          add_unique_evidence(runtime->evidence,
                              "pneumatic_output_delivery_unconfirmed");
        }
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
      } catch (...) {
        if (runtime_config.actuation_kind !=
            core::GripperActuationKind::kPneumatic)
          throw;
        runtime->state = "failed";
        runtime->executing = false;
        runtime->externally_failed = true;
        runtime->error =
            "Failed to publish Isaac gripper command: unknown exception";
        runtime->reason_code =
            ros2_control_fault_reason::kGripperGoalDispatchFailed;
        add_unique_evidence(runtime->evidence,
                            "pneumatic_output_delivery_unconfirmed");
        ++runtime->revision;
        return rejected_gripper_command(runtime->reason_code, runtime->error,
                                        command_id, generation);
      }
    }
    if (runtime_config.actuation_kind ==
        core::GripperActuationKind::kPneumatic) {
      publish_pneumatic_display(runtime, request.target_position);
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (!runtime->executing)
        return {true, command_id, generation, std::nullopt};
    }
    if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                    clock_.now())) {
      // Publication already happened; scheduling failure is not a
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
  // before adapter destruction. The runtime owns the action client, which owns
  // these callbacks (pending requests and goal handles), so they hold the
  // runtime weakly: a strong capture forms a cycle that leaks the client, node
  // and DDS participant whenever a goal never reaches a final result.
  const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
  options.goal_response_callback = [this, weak_runtime, command_id,
                                    generation](const GoalHandleGripper::SharedPtr& goal_handle) {
    const auto runtime = weak_runtime.lock();
    if (!runtime)
      return;
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
          if (runtime->config.actuation_kind ==
              core::GripperActuationKind::kPneumatic) {
            runtime->externally_failed = true;
            add_unique_evidence(runtime->evidence,
                                "ros_gripper_command_goal_rejected");
            add_unique_evidence(runtime->evidence,
                                "pneumatic_output_delivery_unconfirmed");
          }
          runtime->error = "Gripper action goal was rejected";
          runtime->reason_code = ros2_control_fault_reason::kGripperGoalRejected;
          runtime->projection_state = core::GripperProjectionState::kNotRequired;
          runtime->projection_reason.clear();
          if (runtime->lifecycle_disable_pending) {
            runtime->lifecycle_disable_failed = true;
            runtime->lifecycle_disable_terminal = true;
          }
          ++runtime->revision;
        }
        runtime->lifecycle_condition.notify_all();
      } else if (runtime->config.actuation_kind ==
                     core::GripperActuationKind::kPneumatic &&
                 !runtime->executing) {
        // A late acceptance is diagnostic only after a final pneumatic result.
        return;
      } else if (runtime->lifecycle_disable_pending) {
        runtime->goal_accepted = true;
        runtime->active_goal_handle = goal_handle;
        if (!runtime->lifecycle_cancel_dispatched) {
          runtime->lifecycle_cancel_dispatched = true;
          handle_to_cancel = goal_handle;
          action_client = runtime->action_client;
        }
        runtime->lifecycle_condition.notify_all();
        ++runtime->revision;
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
        const auto goal_id = handle_to_cancel->get_goal_id();
        (void)action_client->async_cancel_goal(
            handle_to_cancel,
            [weak_runtime = std::weak_ptr<GripperRuntime>(runtime), command_id,
             generation, goal_id](const auto response) {
              const auto runtime = weak_runtime.lock();
              if (!runtime)
                return;
              std::lock_guard lock(runtime->mutex);
              if (!runtime->lifecycle_disable_pending ||
                  runtime->lifecycle_disable_command_id != command_id ||
                  runtime->lifecycle_disable_generation != generation)
                return;
              const bool accepted = response &&
                  response->return_code ==
                      action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
                  std::any_of(response->goals_canceling.begin(),
                              response->goals_canceling.end(),
                              [&](const auto &row) {
                                return row.goal_id.uuid == goal_id;
                              });
              if (!accepted && !runtime->lifecycle_disable_terminal)
                runtime->lifecycle_disable_failed = true;
              runtime->lifecycle_condition.notify_all();
            });
      } catch (const std::exception&) {
        std::lock_guard lock(runtime->mutex);
        if (runtime->lifecycle_disable_pending) {
          runtime->lifecycle_disable_failed = true;
          runtime->lifecycle_condition.notify_all();
        }
      }
    }
  };
  options.feedback_callback =
      [this, weak_runtime, command_id, generation](
          GoalHandleGripper::SharedPtr goal_handle,
          const std::shared_ptr<const GripperCommand::Feedback> feedback) {
        if (const auto runtime = weak_runtime.lock())
          on_gripper_feedback(runtime, command_id, generation, goal_handle,
                              feedback);
      };
  options.result_callback =
      [this, weak_runtime, command_id,
       generation](const GoalHandleGripper::WrappedResult &result) {
        if (const auto runtime = weak_runtime.lock())
          on_gripper_result(runtime, command_id, generation, result);
      };

  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (!admission_authorized() || !runtime_is_current()) {
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
    if (runtime_config.actuation_kind ==
            core::GripperActuationKind::kPneumatic &&
        !runtime->action_client->action_server_is_ready()) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->externally_failed = true;
      runtime->reason_code =
          ros2_control_fault_reason::kGripperActionServerUnavailable;
      runtime->error = "Gripper action server disappeared before dispatch";
      add_unique_evidence(runtime->evidence,
                          "pneumatic_output_delivery_unconfirmed");
      ++runtime->revision;
      return rejected_gripper_command(runtime->reason_code, runtime->error,
                                      command_id, generation);
    }
    try {
      // rclcpp_action queues these callbacks on the executor; it does not
      // invoke them inline. Keeping the authority and runtime leaf locks
      // through this bounded enqueue makes dispatch linearizable with
      // reset/replacement.
      (void)runtime->action_client->async_send_goal(goal, options);
      if (runtime_config.actuation_kind ==
          core::GripperActuationKind::kPneumatic)
        add_unique_evidence(runtime->evidence, "ros_gripper_command_enqueued");
    } catch (const std::exception &exc) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      if (runtime_config.actuation_kind ==
          core::GripperActuationKind::kPneumatic) {
        runtime->externally_failed = true;
        add_unique_evidence(runtime->evidence,
                            "pneumatic_output_delivery_unconfirmed");
      }
      runtime->error =
          std::string("Failed to dispatch gripper action goal: ") + exc.what();
      runtime->reason_code =
          ros2_control_fault_reason::kGripperGoalDispatchFailed;
      ++runtime->revision;
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperGoalDispatchFailed,
          "Failed to dispatch gripper action goal: " + std::string(exc.what()),
          command_id, generation);
    } catch (...) {
      if (runtime_config.actuation_kind !=
          core::GripperActuationKind::kPneumatic)
        throw;
      runtime->state = "failed";
      runtime->executing = false;
      runtime->externally_failed = true;
      runtime->error =
          "Failed to dispatch gripper action goal: unknown exception";
      runtime->reason_code =
          ros2_control_fault_reason::kGripperGoalDispatchFailed;
      add_unique_evidence(runtime->evidence,
                          "pneumatic_output_delivery_unconfirmed");
      ++runtime->revision;
      return rejected_gripper_command(runtime->reason_code, runtime->error,
                                      command_id, generation);
    }
  }
  if (runtime_config.actuation_kind == core::GripperActuationKind::kPneumatic)
    publish_pneumatic_display(runtime, request.target_position);
  if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                  clock_.now())) {
    // Enqueue crossed the transport boundary. Position retains its existing
    // failure/cancel path; pneumatic keeps the same delivery observation and
    // timeout obligation. Neither is a side-effect-free rejection.
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
      if (runtime->config.actuation_kind ==
          core::GripperActuationKind::kPneumatic) {
        add_unique_evidence(runtime->evidence,
                            "gripper_lifecycle_schedule_failed");
        ++runtime->revision;
        // Existing owner status reads continue the exact clock-based
        // obligation.
        return false;
      }
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
  if (runtime->config.actuation_kind ==
      core::GripperActuationKind::kPneumatic) {
    bool active = false;
    core::TimePoint next = 0;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id != command_id ||
          runtime->generation != generation ||
          runtime->lifecycle_schedule_generation != schedule_generation)
        return;
      runtime->lifecycle_handle.reset();
      advance_pneumatic_locked(*runtime);
      active = runtime->executing;
      next = clock_.now() + 10'000'000;
      if (runtime->deadline_at_ns)
        next = std::min(next, *runtime->deadline_at_ns);
    }
    if (active)
      (void)schedule_gripper_lifecycle(runtime, command_id, generation, next);
    return;
  }
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
  std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
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
  if (runtime->config.actuation_kind ==
          core::GripperActuationKind::kPneumatic &&
      !runtime->lifecycle_handle &&
      !(requires_pneumatic_commanded_projection(runtime->config) &&
        runtime->projection_state == core::GripperProjectionState::kPending))
    advance_pneumatic_locked(*runtime);
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
  status.admission_generation = gripper_authority_generation_;
  status.lifecycle_generation = gripper_lifecycle_generation_;
  status.admission_closed = gripper_admission_blocked_ ||
                            (gripper_motion_admission_blocked_ &&
                             runtime->config.actuation_kind !=
                                 core::GripperActuationKind::kPneumatic);
  status.command_id = runtime->command_id;
  status.generation = runtime->generation;
  status.revision = runtime->revision;
  status.progress = runtime->progress;
  status.current_position = runtime->current_position;
  // Return the last owner-published observation verbatim. Freshness is an
  // admission/evidence concern at the owning callback boundary; deriving it
  // from wall time here would make this supposedly immutable status change
  // without a publication or revision increment.
  if (runtime->config.actuation_kind != core::GripperActuationKind::kPneumatic)
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
