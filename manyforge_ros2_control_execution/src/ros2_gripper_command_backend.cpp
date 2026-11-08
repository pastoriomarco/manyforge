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
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#include <action_msgs/srv/cancel_goal.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>
#include <manyforge_ros2_control_execution/graph_event_observer.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::ros2_control_execution {
using detail::clamp_progress;
namespace {
void settle_gripper_cancel(const std::shared_ptr<core::GripperLateCancelSlot> &slot) noexcept {
  if (!slot) return;
  slot->cancel_allowed.store(false, std::memory_order_release);
  {
    std::lock_guard lock(slot->mutex);
    if (slot->state == core::GripperLateCancelState::kPending)
      slot->state = core::GripperLateCancelState::kSettled;
  }
  if (slot->state_changed) slot->state_changed();
}
void mark_gripper_cancel_unissued(const std::shared_ptr<core::GripperLateCancelSlot> &slot) noexcept {
  if (!slot) return;
  {
    std::lock_guard lock(slot->mutex);
    if (slot->state == core::GripperLateCancelState::kPending) {
      slot->state = core::GripperLateCancelState::kFailed;
      if (slot->failure_epoch) {
        auto epoch = slot->failure_epoch->load(std::memory_order_acquire);
        while (epoch != std::numeric_limits<std::uint64_t>::max() &&
               !slot->failure_epoch->compare_exchange_weak(epoch, epoch + 1, std::memory_order_acq_rel)) {}
      }
    }
  }
  if (slot->state_changed) slot->state_changed();
}
void handoff_gripper_cancel_failure(const std::shared_ptr<core::GripperLateCancelSlot> &slot) noexcept {
  if (!slot) return;
  if (slot->handoff_running.exchange(true, std::memory_order_acq_rel)) return;
  struct ReleaseClaim {
    std::atomic<bool> &claim;
    bool active{true};
    ~ReleaseClaim() { if (active) claim.store(false, std::memory_order_release); }
  } release{slot->handoff_running};
  decltype(slot->handoff) handoff;
  std::optional<core::CommandTargetId> target;
  std::string gripper_id;
  std::uint64_t preparation = 0, command = 0, generation = 0;
  {
    std::lock_guard lock(slot->mutex);
    if (slot->state != core::GripperLateCancelState::kFailed || !slot->receipt_installed) {
      // Drop the claim before the slot lock: receipt installation can then
      // either claim the handoff or observe an already-running writer.
      slot->handoff_running.store(false, std::memory_order_release);
      release.active = false;
      return;
    }
    handoff = slot->handoff;
    target = slot->target;
    gripper_id = slot->gripper_id;
    preparation = slot->preparation_id;
    command = slot->command_id;
    generation = slot->generation;
  }
  if (!handoff || !target || gripper_id.empty()) return;
  try {
    if (!handoff(preparation, command, generation, *target, gripper_id).ok()) return;
    {
      std::lock_guard lock(slot->mutex);
      if (slot->state == core::GripperLateCancelState::kFailed &&
          slot->preparation_id == preparation && slot->command_id == command &&
          slot->generation == generation && slot->target == target && slot->gripper_id == gripper_id)
        slot->state = core::GripperLateCancelState::kHandedOff;
    }
    if (slot->state_changed) slot->state_changed();
  } catch (...) { /* Keep the failure for teardown. */ }
}
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

// The single finger joint's position in a ParallelGripperCommand state. The
// stock controller's cancel result carries an empty state.
std::optional<double>
gripper_state_position(const sensor_msgs::msg::JointState &state) {
  if (state.position.empty()) {
    return std::nullopt;
  }
  return state.position.front();
}

} // namespace

Ros2GripperCommandBackend::Ros2GripperCommandBackend(
    std::shared_ptr<Ros2TransportResources> resources,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    core::IClock &clock, core::IScheduler &scheduler, bool commanded_sim)
    : resources_(std::move(resources)),
      execution_state_sink_(std::move(execution_state_sink)), clock_(clock),
      scheduler_(scheduler),
      commanded_sim_(commanded_sim) {
  if (!resources_.load())
    throw std::invalid_argument("gripper ROS transport must not be null");
  if (!execution_state_sink_)
    throw std::invalid_argument("execution state sink must not be null");
  if (resources_.load()->started())
    throw std::logic_error(
        "gripper must be installed before ROS transport starts");
  scene_projection_worker_ = resources_.load()->take_projection_worker();
}

Ros2GripperCommandBackend::~Ros2GripperCommandBackend() { shutdown(); }

void Ros2GripperCommandBackend::complete_ros_configuration() {
  std::lock_guard<std::mutex> lock(ros_configuration_mutex_);
  if (ros_configuration_complete_)
    throw std::logic_error("gripper configuration was already completed");
  ros_configuration_complete_ = true;
}

void Ros2GripperCommandBackend::set_retirement_wake(std::function<void()> wake) {
  std::lock_guard lock(gripper_authority_mutex_);
  retirement_wake_ = std::move(wake);
}

void Ros2GripperCommandBackend::shutdown() noexcept {
  if (shutdown_started_.exchange(true, std::memory_order_acq_rel))
    return;
  // State fence plus a scheduled cancel: no owner dispatch (it already runs
  // off-owner from invalidate_gripper_commands and CloseGripperLateCancels).
  invalidate_gripper_commands_impl(core::GripperInvalidationReason::kShutdown);
  core::WaitForDrain("gripper_backend.scheduler_controller_command_lane",
                     std::chrono::seconds(1),
                     [this](std::chrono::nanoseconds bound) {
                       return scheduler_.drain_for(
                           core::SchedulerPriority::ControllerCommand, bound);
                     });
}

bool Ros2GripperCommandBackend::StopSceneProjectionUntil(
    std::chrono::steady_clock::time_point deadline) noexcept {
  if (!scene_projection_worker_) return true;
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = deadline > now ? deadline - now : std::chrono::steady_clock::duration::zero();
  return scene_projection_worker_->stop_and_join_for(
      std::chrono::duration_cast<std::chrono::nanoseconds>(remaining));
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
    if (runtime.config.transport_kind == "ros_gripper_command" &&
        old_effect_owner_retired(runtime))
      return;
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

bool Ros2GripperCommandBackend::hold_until_duration_locked(
    GripperRuntime &runtime) const {
  if (!runtime.wait_for_duration ||
      clock_.now() - runtime.started_at_ns >=
          gripper_duration_ns(runtime.duration_s)) {
    return false;
  }
  runtime.controller_done = true;
  runtime.state = "executing";
  runtime.executing = true;
  return true;
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
  if (queued) {
    runtime->scene_projection_submitted = true;
    runtime->scene_projection_submitted_request_id = token.request_id;
  }
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
    if (runtime->command_id == completion.token.command_id &&
        runtime->generation == completion.token.generation &&
        runtime->scene_projection_submitted_request_id == completion.token.request_id &&
        completion.outcome != detail::SceneProjectionOutcome::kSuperseded) {
      if (completion.outcome == detail::SceneProjectionOutcome::kMissed)
        runtime->scene_projection_missed_settled = true;
      runtime->scene_projection_submitted = false;
      runtime->scene_projection_submitted_request_id = 0;
    }
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
      // The best-effort request completed; no worker obligation remains.
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

bool Ros2GripperCommandBackend::old_effect_owner_retired(
    const GripperRuntime &runtime) noexcept {
  return runtime.goal_ros &&
      (runtime.goal_ros->owner->executor_failure_started() ||
       runtime.goal_ros->owner->joined());
}

struct Ros2GripperCommandBackend::StagedRosRebind final {
  struct Entry {
    std::string id;
    std::shared_ptr<GripperRuntime> runtime;
    std::shared_ptr<RosEntities> previous;
    std::shared_ptr<RosEntities> replacement;
  };
  std::shared_ptr<Ros2TransportResources> owner;
  std::shared_ptr<Ros2TransportResources> previous_owner;
  const Ros2GripperCommandBackend *backend{};
  std::vector<Entry> entries;
};

std::shared_ptr<Ros2GripperCommandBackend::StagedRosRebind>
Ros2GripperCommandBackend::stage_ros_rebind(
    std::shared_ptr<Ros2TransportResources> replacement) {
  if (!replacement)
    throw std::invalid_argument("gripper rebind requires a different ROS owner");
  auto stage = std::make_shared<StagedRosRebind>();
  stage->backend = this;
  stage->owner = std::move(replacement);
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    stage->previous_owner = resources_.load();
    if (stage->owner == stage->previous_owner)
      throw std::invalid_argument("gripper rebind requires a different ROS owner");
    stage->entries.reserve(grippers_.size());
    for (const auto &[id, runtime] : grippers_) {
      std::lock_guard runtime_lock(runtime->mutex);
      if (!runtime->ros || runtime->ros->owner != stage->previous_owner)
        throw std::logic_error("gripper runtime does not belong to the current ROS owner");
      stage->entries.push_back({id, runtime, runtime->ros, {}});
    }
  }
  stage->owner->invoke_creation([&] {
    for (auto &entry : stage->entries) {
      auto gate = std::make_shared<std::atomic<bool>>(false);
      auto entities = create_ros_entities(stage->owner, entry.runtime, std::move(gate));
      entry.replacement = std::make_shared<RosEntities>(std::move(entities));
    }
  });
  return stage;
}

core::Result<void> Ros2GripperCommandBackend::commit_ros_rebind(
    const std::shared_ptr<StagedRosRebind> &stage) {
  if (commit_ros_rebind_with(stage, [](void *) noexcept { return true; }, nullptr))
    return core::Result<void>::Ok();
  return core::Result<void>::Err(core::ErrorDTO{
      "ros2_control_gripper_bridge_not_ready",
      "The staged gripper transport is no longer current or ready", {}});
}

bool Ros2GripperCommandBackend::commit_ros_rebind_with(
    const std::shared_ptr<StagedRosRebind> &stage,
    bool (*commit)(void *) noexcept, void *context,
    const std::shared_ptr<Ros2TransportResources> &failed_dependency) {
  if (!stage || stage->backend != this || !stage->owner ||
      !commit ||
      !stage->previous_owner || !stage->previous_owner->joined() ||
      !(stage->previous_owner->executor_failure_started() ||
        (failed_dependency && failed_dependency->joined() &&
         failed_dependency->executor_failure_started())) ||
      !stage->owner->started() || stage->owner->joined() ||
      stage->owner->executor_failure_started() ||
      stage->owner->executor_fault())
    return false;
  struct PublisherFence {
    std::vector<std::shared_ptr<RosEntities>> bundles;
    bool committed{false};
    ~PublisherFence() {
      if (!committed)
        for (const auto &bundle : bundles) {
          auto fenced = RosEntities::PublisherLeaf::State::kRebindFenced;
          MANYFORGE_GRIPPER_REBIND_ROLLBACK_TEST_POINT();
          const bool reopened = bundle->publisher_leaf->state.compare_exchange_strong(
              fenced, RosEntities::PublisherLeaf::State::kOpen, std::memory_order_acq_rel);
          MANYFORGE_GRIPPER_REBIND_ROLLBACK_RESULT(reopened);
          (void)reopened;
        }
    }
  } fence;
  fence.bundles.reserve(stage->entries.size());
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    if (resources_.load() != stage->previous_owner || grippers_.size() != stage->entries.size())
      return false;
    for (const auto &entry : stage->entries) {
      const auto found = grippers_.find(entry.id);
      if (found == grippers_.end() || found->second != entry.runtime || !entry.previous)
        return false;
      std::lock_guard runtime_lock(entry.runtime->mutex);
      if (entry.runtime->ros != entry.previous) return false;
      if (entry.previous->publisher_leaf) {
        auto open = RosEntities::PublisherLeaf::State::kOpen;
        if (!entry.previous->publisher_leaf->state.compare_exchange_strong(
                open, RosEntities::PublisherLeaf::State::kRebindFenced,
                std::memory_order_acq_rel)) return false;
        fence.bundles.push_back(entry.previous);
      }
    }
  }
  const auto started = std::chrono::steady_clock::now();
  for (const auto &entry : stage->entries) {
    if (!entry.previous || !entry.previous->publisher_leaf) continue;
    const auto budget = std::chrono::nanoseconds(entry.runtime->lifecycle_timeout_ns);
    const auto remaining = std::chrono::steady_clock::time_point::max() - started;
    const auto deadline = budget >= remaining ? std::chrono::steady_clock::time_point::max()
                                               : started + budget;
    std::unique_lock leaf(entry.previous->publisher_leaf->mutex, std::defer_lock);
    if (!leaf.try_lock_until(deadline)) return false;
  }
  std::vector<rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr> retired_publishers;
  retired_publishers.reserve(fence.bundles.size());
  std::vector<std::unique_lock<std::mutex>> runtime_locks;
  runtime_locks.reserve(stage->entries.size());
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    if (resources_.load() != stage->previous_owner ||
        grippers_.size() != stage->entries.size())
      return false;
    for (const auto &entry : stage->entries) {
      const auto found = grippers_.find(entry.id);
      if (found == grippers_.end() || found->second != entry.runtime ||
          !entry.replacement || entry.replacement->owner != stage->owner ||
          !entry.replacement->callbacks_enabled ||
          entry.replacement->callbacks_enabled->load(std::memory_order_acquire))
        return false;
      runtime_locks.emplace_back(entry.runtime->mutex);
      if (entry.runtime->ros != entry.previous)
        return false;
      if (entry.previous->publisher_leaf &&
          entry.previous->publisher_leaf->state.load(std::memory_order_acquire) !=
              RosEntities::PublisherLeaf::State::kRebindFenced)
        return false;
    }
    if (!commit(context))
      return false;
    for (const auto &entry : stage->entries) {
      entry.runtime->send_generation.store(0, std::memory_order_release);
      // A known completed goal needs no old SDK handle after its owner joined.
      // Unknown or pending outcomes retain their exact old bundle.
      if (entry.runtime->goal_ros == entry.previous &&
          entry.runtime->state == "completed" && !entry.runtime->executing &&
          !entry.runtime->goal_accepted && !entry.runtime->active_goal_handle &&
          !entry.runtime->cancel_pending && !entry.runtime->lifecycle_disable_pending)
        entry.runtime->goal_ros.reset();
      entry.runtime->ros = entry.replacement;
    }
    resources_.store(stage->owner);
  }
  runtime_locks.clear();
  fence.committed = true;
  for (const auto &bundle : fence.bundles) {
    std::lock_guard leaf(bundle->publisher_leaf->mutex);
    bundle->publisher_leaf->state.store(RosEntities::PublisherLeaf::State::kRetired,
                                       std::memory_order_release);
    if (bundle->command_matches) bundle->command_matches->store(-1, std::memory_order_release);
    if (bundle->joint_state_pub) retired_publishers.push_back(std::move(bundle->joint_state_pub));
  }
  // The owner finalizer takes the same leaf; release aliases after unlocking.
  retired_publishers.clear();
  return true;
}

core::Result<void> Ros2GripperCommandBackend::activate_ros_rebind(
    const std::shared_ptr<StagedRosRebind> &stage) {
  const auto refused = [] {
    return core::Result<void>::Err(core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The staged gripper transport is no longer current or ready", {}});
  };
  if (!stage || stage->backend != this || !stage->owner ||
      stage->owner->joined() || stage->owner->executor_failure_started() ||
      stage->owner->executor_fault())
    return refused();
  std::vector<std::unique_lock<std::mutex>> runtime_locks;
  runtime_locks.reserve(stage->entries.size());
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    if (resources_.load() != stage->owner || grippers_.size() != stage->entries.size())
      return refused();
    for (const auto &entry : stage->entries) {
      const auto found = grippers_.find(entry.id);
      if (found == grippers_.end() || found->second != entry.runtime ||
          !entry.replacement || !entry.replacement->callbacks_enabled)
        return refused();
      runtime_locks.emplace_back(entry.runtime->mutex);
      if (entry.runtime->ros != entry.replacement)
        return refused();
    }
    for (const auto &entry : stage->entries)
      entry.replacement->callbacks_enabled->store(true, std::memory_order_release);
  }
  return core::Result<void>::Ok();
}

Ros2GripperCommandBackend::RosEntities
Ros2GripperCommandBackend::create_ros_entities(
    const std::shared_ptr<Ros2TransportResources> &owner,
    const std::shared_ptr<GripperRuntime> &runtime,
    std::shared_ptr<std::atomic<bool>> callbacks_enabled) {
  RosEntities entities;
  entities.owner = owner;
  entities.callbacks_enabled = std::move(callbacks_enabled);
  const auto node = owner->node();
  if (runtime->config.output_mode == core::GripperOutputMode::kNone) {
    // The genuine local owner has no ROS command entities.
  } else if (runtime->config.transport_kind == "ros_gripper_command") {
    entities.action_leaf = std::make_shared<RosEntities::ActionLeaf>();
    // OD-66: goal response, feedback, result and cancel responses run on the
    // owner's time-critical thread, in a group of this client's own.
    entities.action_callback_group = owner->create_time_critical_callback_group();
    entities.action_client = rclcpp_action::create_client<GripperCommand>(
        node, runtime->config.action_name, entities.action_callback_group);
    entities.action_ready = std::make_shared<std::atomic<int>>(-1);
    const auto refresh = [client = std::weak_ptr(entities.action_client),
                          ready = entities.action_ready] {
      try {
        const auto current = client.lock();
        ready->store(current ? static_cast<int>(current->action_server_is_ready()) : -1,
                     std::memory_order_release);
      } catch (...) {
        ready->store(-1, std::memory_order_release);
      }
    };
    entities.action_graph_observer = owner->retain_entity(std::make_shared<GraphEventObserver>(
        node, node->get_node_base_interface()->get_default_callback_group(), refresh));
    refresh();
  } else {
    // The cache belongs to this exact ROS entity bundle. Staged bundles may
    // record their own matches before publication, but cannot affect the
    // installed runtime until the bundle is committed and its gate opens.
    entities.publisher_leaf = std::make_shared<RosEntities::PublisherLeaf>();
    entities.command_matches = std::make_shared<std::atomic<std::int64_t>>(-1);
    rclcpp::PublisherOptions options;
    options.event_callbacks.matched_callback =
        [matches = entities.command_matches](rclcpp::MatchedInfo &info) {
          matches->store(info.current_count, std::memory_order_release);
        };
    entities.joint_state_pub =
        node->create_publisher<sensor_msgs::msg::JointState>(
            runtime->config.command_topic, rclcpp::SystemDefaultsQoS(), options);
    const auto initial_count = static_cast<std::int64_t>(entities.joint_state_pub->get_subscription_count());
    std::int64_t unobserved = -1;
    (void)entities.command_matches->compare_exchange_strong(
        unobserved, initial_count, std::memory_order_acq_rel);
  }
  if (!runtime->config.state_topic.empty() &&
      (runtime->config.transport_kind == "isaac_joint_state_effort" ||
       runtime->config.actuation_kind ==
           core::GripperActuationKind::kPneumatic)) {
    const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
    const auto gate = entities.callbacks_enabled;
    entities.joint_state_sub =
        node->create_subscription<sensor_msgs::msg::JointState>(
            runtime->config.state_topic, rclcpp::SystemDefaultsQoS(),
            [weak_runtime, gate](const sensor_msgs::msg::JointState::SharedPtr msg) {
              if (gate && !gate->load(std::memory_order_acquire)) return;
              const auto runtime = weak_runtime.lock();
              if (!runtime || !msg) {
                return;
              }
              const auto &observed_joint = runtime->config.state_joint_name;
              if (observed_joint.empty()) {
                return;
              }
              const auto position = position_for_joint(*msg, observed_joint);
              // Liveness: lock-free, no allocation.
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
  entities.action_client = owner->retain_entity(std::move(entities.action_client),
                                                entities.action_callback_group);
  if (entities.joint_state_pub) {
    auto publisher = std::move(entities.joint_state_pub);
    auto *const pointer = publisher.get();
    auto leaf = entities.publisher_leaf;
    entities.joint_state_pub = decltype(entities.joint_state_pub)(
        pointer, [publisher = std::move(publisher), leaf = std::move(leaf)](auto *) mutable {
          std::lock_guard lock(leaf->mutex);
          publisher.reset();
        });
  }
  entities.joint_state_pub = owner->retain_entity(std::move(entities.joint_state_pub));
  entities.joint_state_sub = owner->retain_entity(std::move(entities.joint_state_sub));
  return entities;
}

void Ros2GripperCommandBackend::configure_gripper_action(
    std::string gripper_id, Ros2GripperActionConfig config,
    core::Duration lifecycle_timeout_ns) {
  if (lifecycle_timeout_ns <= 0)
    throw std::invalid_argument("gripper lifecycle timeout must be positive");
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
  if (!std::isfinite(config.stop_finish_tolerance) ||
      config.stop_finish_tolerance < 0.0) {
    throw std::invalid_argument("gripper stop_finish_tolerance is invalid");
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
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr retired_publisher;
  std::lock_guard<std::mutex> ros_configuration_lock(ros_configuration_mutex_);
  if (ros_configuration_complete_ || resources_.load()->started()) {
    throw std::logic_error(
        "gripper transports must be configured before the ROS executor starts");
  }
  auto runtime = std::make_shared<GripperRuntime>();
  runtime->config = std::move(config);
  runtime->lifecycle_timeout_ns = lifecycle_timeout_ns;
  runtime->ros = std::make_shared<RosEntities>(create_ros_entities(resources_.load(), runtime));
  runtime->generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  runtime->send_generation.store(runtime->generation, std::memory_order_release);
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
      // Preserve the exact same-component journal binding across an internal
      // ROS reconfiguration; prior goal slots retain their captured hook.
      std::lock_guard runtime_lock(replaced_runtime->mutex);
      runtime->late_cancel_engine_identity = replaced_runtime->late_cancel_engine_identity;
      runtime->late_cancel_journal_hook = replaced_runtime->late_cancel_journal_hook;
    }
    grippers_[std::move(gripper_id)] = std::move(runtime);
  }
  if (replaced_runtime) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::shared_ptr<RosEntities> replaced_ros;
    std::shared_ptr<core::GripperLateCancelSlot> cancel_slot;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    std::uint64_t command_id = 0;
    std::uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(replaced_runtime->mutex);
      goal_handle = replaced_runtime->active_goal_handle;
      cancel_slot = replaced_runtime->goal_late_cancel_slot;
      replaced_ros = goal_handle ? replaced_runtime->goal_ros : replaced_runtime->ros;
      if (replaced_ros) action_client = replaced_ros->action_client;
      // Production config precedes Bind/publication and owner start; no
      // command can reach this pre-start replacement publisher (DX4132).
      if (replaced_runtime->ros && replaced_runtime->ros->publisher_leaf) {
        replaced_runtime->ros->publisher_leaf->state.store(
            RosEntities::PublisherLeaf::State::kRetired, std::memory_order_release);
        if (replaced_runtime->ros->command_matches)
          replaced_runtime->ros->command_matches->store(-1, std::memory_order_release);
        retired_publisher = std::move(replaced_runtime->ros->joint_state_pub);
      }
      lifecycle_handle = replaced_runtime->lifecycle_handle;
      command_id = replaced_runtime->lifecycle_disable_command_id;
      generation = replaced_runtime->lifecycle_disable_generation;
      replaced_runtime->lifecycle_handle.reset();
      ++replaced_runtime->lifecycle_schedule_generation;
      replaced_runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      replaced_runtime->send_generation.store(replaced_runtime->generation, std::memory_order_release);
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
    if (action_client && goal_handle)
      dispatch_disable_cancel(replaced_runtime, replaced_ros, goal_handle,
                              command_id, generation, cancel_slot, false);
  }
}

sensor_msgs::msg::JointState make_isaac_joint_state_gripper_command(
    const rclcpp::Node::SharedPtr &node, const Ros2GripperActionConfig &config,
    double target_position) {
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
  return msg;
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
  const std::optional<double> reported_position =
      gripper_state_position(feedback->state);
  std::optional<double> feedback_position;
  GoalHandleGripper::SharedPtr retired_goal;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  std::shared_ptr<RosEntities> goal_ros;
  std::shared_ptr<core::GripperLateCancelSlot> cancel_slot;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation || runtime->externally_failed) {
      return;
    }
    if (!runtime->executing) {
      return;
    }
    if (!reported_position.has_value() || !std::isfinite(*reported_position)) {
      cancel_slot = runtime->goal_late_cancel_slot;
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      goal_ros = runtime->goal_ros;
      if (goal_ros) action_client = goal_ros->action_client;
      if (!goal_handle) {
        goal_handle = runtime->active_goal_handle;
      }
      retired_goal = std::move(runtime->active_goal_handle);
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
      runtime->current_position = *reported_position;
      runtime->progress =
          clamp_progress(static_cast<double>(std::max<core::Duration>(
                             0, clock_.now() - runtime->started_at_ns)) /
                         static_cast<double>(std::max<core::Duration>(
                             1, gripper_duration_ns(runtime->duration_s))));
      // ParallelGripperCommand feedback carries only the state: completion
      // (reached_goal) comes with the result.
      feedback_position = *reported_position;
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
    dispatch_disable_cancel(runtime, goal_ros, goal_handle, command_id,
                            generation, cancel_slot);
  }
  if (feedback_position.has_value() && !is_commanded_sim_mode()) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id == command_id && runtime->generation == generation) {
      (void)schedule_gripper_scene_projection_locked(runtime, *feedback_position, false);
      ++runtime->revision;
    }
  }
}

void Ros2GripperCommandBackend::on_gripper_result(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, const GoalHandleGripper::WrappedResult &result) {
  GoalHandleGripper::SharedPtr retired_goal;
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
      retired_goal = std::move(runtime->active_goal_handle);
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
  bool held_for_duration = false;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      // A result after the cancel's ACCEPT settled the stopped
      // command is ignored.
      if (runtime->lifecycle_disable_pending &&
          runtime->lifecycle_disable_command_id == command_id &&
          runtime->lifecycle_disable_generation == generation &&
          !runtime->lifecycle_disable_terminal) {
        runtime->lifecycle_disable_terminal = true;
        runtime->lifecycle_disable_failed =
            result.code == rclcpp_action::ResultCode::ABORTED ||
            result.code == rclcpp_action::ResultCode::UNKNOWN;
        runtime->cancel_pending = false;
        // The goal's own success settled the stopped command first (no
        // cancel sent, or before the cancel's ACCEPT): it finished.
        if (result.code == rclcpp_action::ResultCode::SUCCEEDED)
          add_unique_evidence(runtime->evidence,
                              "completed_by_natural_completion");
        ++runtime->revision;
        runtime->lifecycle_condition.notify_all();
      }
      return;
    }
    if (runtime->externally_failed) {
      return;
    }
    runtime_config = runtime->config;
    retired_goal = std::move(runtime->active_goal_handle);
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->executing = false;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    const std::optional<double> reported_position =
        result.result ? gripper_state_position(result.result->state)
                      : std::nullopt;
    const bool result_position_not_finite =
        reported_position.has_value() && !std::isfinite(*reported_position);
    if (reported_position.has_value() && !result_position_not_finite) {
      runtime->current_position = *reported_position;
      result_position = *reported_position;
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
      held_for_duration =
          runtime->state == "completed" && hold_until_duration_locked(*runtime);
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
  if (held_for_duration) {
    (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                     clock_.now());
  }
}

void Ros2GripperCommandBackend::invalidate_gripper_commands(
    core::GripperInvalidationReason reason) noexcept {
  invalidate_gripper_commands_impl(reason);
}

bool Ros2GripperCommandBackend::rebuilt_unknown_pending(
    const std::string &id, std::uint64_t command_id, std::uint64_t generation) const {
  return static_cast<bool>(rebuilt_unknown_owner(id, command_id, generation));
}

std::shared_ptr<const void> Ros2GripperCommandBackend::rebuilt_unknown_owner(
    const std::string &id, std::uint64_t command_id, std::uint64_t generation) const {
  if (!command_id) return {};
  std::shared_ptr<Ros2TransportResources> current_owner;
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(id);
    if (found == grippers_.end()) return {};
    const auto &runtime = *found->second;
    std::lock_guard runtime_lock(runtime.mutex);
    if (!runtime.ros) return {};
    current_owner = runtime.ros->owner;
    const bool unknown = runtime.executing ||
        (runtime.state == "failed" && runtime.error == "Gripper action returned an unknown result code");
    if (runtime.command_id == command_id && runtime.generation == generation && unknown &&
        runtime.goal_ros && runtime.goal_ros->owner != current_owner &&
        runtime.goal_ros->owner->joined() && current_owner == resources_.load() &&
        current_owner->started() && !current_owner->executor_failure_started()) return current_owner;
  }
  if (!current_owner || current_owner != resources_.load() || !current_owner->started() ||
      current_owner->executor_failure_started()) return {};
  std::vector<LateCancelTracking::Entry> entries;
  {
    std::lock_guard lock(late_cancel_tracking_->mutex);
    entries = late_cancel_tracking_->entries;
  }
  for (const auto &entry : entries) {
    if (entry.gripper_id != id || !entry.bundle || !entry.bundle->owner ||
        entry.bundle->owner == current_owner || !entry.bundle->owner->joined()) continue;
    std::lock_guard lock(entry.slot->mutex);
    if (entry.slot->command_id == command_id && entry.slot->generation == generation &&
        (entry.slot->state == core::GripperLateCancelState::kFailed ||
         entry.slot->state == core::GripperLateCancelState::kHandedOff)) return current_owner;
  }
  return {};
}

core::Result<void> Ros2GripperCommandBackend::retire_rebuilt_unknown(
    const std::string &id, std::uint64_t command_id, std::uint64_t generation,
    const std::shared_ptr<const void> &expected_owner) {
  std::shared_ptr<core::GripperPreparedSessionPin> retired_session;
  std::shared_ptr<GripperRuntime> retired_runtime;
  std::shared_ptr<RosEntities> retired_bundle;
  const auto retire_slot = [&](const std::shared_ptr<Ros2TransportResources> &current_owner) {
    std::lock_guard tracking_lock(late_cancel_tracking_->mutex);
    for (auto &entry : late_cancel_tracking_->entries) {
      if (entry.gripper_id != id || !entry.bundle || !entry.bundle->owner ||
          entry.bundle->owner == current_owner || !entry.bundle->owner->joined()) continue;
      std::lock_guard slot_lock(entry.slot->mutex);
      if (entry.slot->command_id != command_id || entry.slot->generation != generation ||
          (entry.slot->state != core::GripperLateCancelState::kFailed &&
           entry.slot->state != core::GripperLateCancelState::kHandedOff)) continue;
      entry.slot->state = core::GripperLateCancelState::kSettled;
      entry.slot->cancel_allowed.store(false, std::memory_order_release);
      retired_session = std::move(entry.session);
      retired_runtime = std::move(entry.runtime);
      retired_bundle = std::move(entry.bundle);
      const auto epoch = late_cancel_failure_epoch_->load(std::memory_order_acquire);
      bool others_failed = false;
      for (const auto &other : late_cancel_tracking_->entries) {
        if (other.slot == entry.slot) continue;
        std::lock_guard other_lock(other.slot->mutex);
        if (other.slot->state == core::GripperLateCancelState::kFailed ||
            other.slot->state == core::GripperLateCancelState::kHandedOff) {
          others_failed = true;
          break;
        }
      }
      if (!others_failed && epoch != std::numeric_limits<std::uint64_t>::max())
        late_cancel_cleared_epoch_.store(epoch, std::memory_order_release);
      return true;
    }
    return false;
  };
  // A preempted slot may be retired without rewriting the newer command.
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    const auto current = grippers_.find(id);
    if (current != grippers_.end() && expected_owner && command_id) {
      std::lock_guard runtime_lock(current->second->mutex);
      const auto current_owner = current->second->ros ? current->second->ros->owner : nullptr;
      const bool exact_current = current->second->command_id == command_id &&
                                 current->second->generation == generation;
      if (!exact_current && current_owner && current_owner.get() == expected_owner.get() &&
          current_owner == resources_.load() && current_owner->started() &&
          !current_owner->executor_failure_started() && retire_slot(current_owner))
        return core::Result<void>::Ok();
    }
  }
  std::shared_ptr<GripperRuntime> runtime;
  std::shared_ptr<RosEntities> old_bundle;
  GoalHandleGripper::SharedPtr old_goal;
  std::optional<core::ScheduleHandle> old_lifecycle;
  std::function<void()> wake;
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    std::lock_guard map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(id);
    if (found == grippers_.end())
      return core::Result<void>::Err(core::ErrorDTO{"stale_gripper_state", "The exact gripper is unavailable", {}});
    runtime = found->second;
    std::lock_guard runtime_lock(runtime->mutex);
    const bool unknown = runtime->executing ||
        (runtime->state == "failed" && runtime->error == "Gripper action returned an unknown result code");
    if (!expected_owner || !command_id || runtime->command_id != command_id || runtime->generation != generation ||
        !unknown || !runtime->goal_ros || !runtime->ros ||
        runtime->goal_ros->owner == runtime->ros->owner || !runtime->goal_ros->owner->joined() ||
        runtime->ros->owner != resources_.load() || runtime->ros->owner.get() != expected_owner.get() ||
        !runtime->ros->owner->started() ||
        runtime->ros->owner->executor_failure_started())
      return core::Result<void>::Err(core::ErrorDTO{"stale_gripper_state", "The rebuilt gripper command is no longer current", {}});
    if (runtime->projection_state == core::GripperProjectionState::kPending &&
        runtime->scene_projection_submitted)
      return core::Result<void>::Err(core::ErrorDTO{
          ros2_control_fault_reason::kGripperBusy, "The existing gripper projection is still pending", {}});
    wake = retirement_wake_;
    add_unique_evidence(runtime->evidence, "owner_rebuild_retired_unknown");
    const bool projection_missed = runtime->scene_projection_missed_settled;
    if (projection_missed) add_unique_evidence(runtime->evidence, "scene_projection_missed");
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      if (!projection_missed) runtime->projection_reason.clear();
    }
    old_bundle = std::move(runtime->goal_ros);
    old_goal = std::move(runtime->active_goal_handle);
    old_lifecycle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->state = "idle";
    runtime->executing = false;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->controller_done = false;
    runtime->error.clear();
    runtime->reason_code.clear();
    if (runtime->lifecycle_disable_pending &&
        runtime->lifecycle_disable_command_id == command_id &&
        runtime->lifecycle_disable_generation == generation) {
      runtime->lifecycle_disable_pending = false;
      runtime->lifecycle_disable_terminal = true;
      runtime->lifecycle_disable_failed = true;
    }
    ++runtime->revision;
    (void)retire_slot(runtime->ros->owner);
  }
  runtime->lifecycle_condition.notify_all();
  if (old_lifecycle) {
    try { scheduler_.cancel(*old_lifecycle); } catch (...) {}
  }
  if (wake) {
    try { wake(); } catch (...) {}
  }
  return core::Result<void>::Ok();
}

void Ros2GripperCommandBackend::invalidate_gripper_commands_impl(
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
    GoalHandleGripper::SharedPtr retired_goal;
    std::shared_ptr<core::GripperLateCancelSlot> cancel_slot;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::shared_ptr<RosEntities> goal_ros;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    std::uint64_t disable_command_id = 0;
    std::uint64_t disable_generation = 0;
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
      cancel_slot = runtime->goal_late_cancel_slot;
      // A command waiting for its duration is already done at the
      // controller; a Stop ends it like a goal finished in its last moment.
      const bool waiting_for_duration =
          runtime->executing && runtime->controller_done;
      // A local no-output command has no physical action goal to cancel.
      const bool command_unsettled =
          (runtime->executing && !waiting_for_duration &&
           runtime->config.output_mode != core::GripperOutputMode::kNone) ||
          goal_handle;
      // A failed owner cannot establish the outcome of an admitted action.
      // Keep its exact command and evidence until an existing settlement
      // path proves the result; the closed send gate prevents a late cancel.
      const bool terminal_unknown = runtime->state == "failed" &&
          runtime->error == "Gripper action returned an unknown result code";
      if ((command_unsettled || terminal_unknown) && old_effect_owner_retired(*runtime))
        continue;
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
      // Stock GripperActionController quirk: a cancel that arrives after the
      // controller finished the goal but before realtime_tools sent its
      // success result is accepted and never answered (no result either);
      // the ACCEPT itself settles the stopped command
      // (dispatch_disable_cancel). A Stop also lets a command that is already
      // done finish (like a robot move stopped in its last moment) and
      // settles on the controller's own result instead of cancelling:
      // the fingers are within the finish tolerance of the target, or the node's
      // command_duration_s has elapsed since the start. A reached_goal
      // feedback already completed the command (on_gripper_feedback), so
      // nothing is left to cancel.
      const auto position = runtime->observed_position.has_value()
                                ? runtime->observed_position
                                : runtime->current_position;
      const bool within_finish_tolerance =
          runtime->config.stop_finish_tolerance > 0.0 &&
          position.has_value() &&
          std::abs(*position - runtime->target_position) <=
              runtime->config.stop_finish_tolerance;
      const bool declared_duration_elapsed =
          runtime->duration_s > 0.0 &&
          clock_.now() - runtime->started_at_ns >=
              gripper_duration_ns(runtime->duration_s);
      const bool finish_instead_of_cancel =
          reason == core::GripperInvalidationReason::kStop &&
          runtime->lifecycle_disable_pending &&
          (within_finish_tolerance || declared_duration_elapsed);
      if (finish_instead_of_cancel) {
        runtime->stop_lets_goal_finish = true;
        goal_handle.reset();
      } else if (goal_handle && runtime->lifecycle_disable_pending) {
        if (runtime->lifecycle_cancel_dispatched) {
          goal_handle.reset();
        } else {
          runtime->lifecycle_cancel_dispatched = true;
        }
      }
      disable_command_id = runtime->lifecycle_disable_command_id;
      disable_generation = runtime->lifecycle_disable_generation;
      goal_ros = goal_handle ? runtime->goal_ros : runtime->ros;
      if (goal_ros) action_client = goal_ros->action_client;
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      runtime->send_generation.store(runtime->generation, std::memory_order_release);
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = command_unsettled;
      retired_goal = std::move(runtime->active_goal_handle);
      runtime->state = "idle";
      runtime->progress = 0.0;
      runtime->controller_done = false;
      runtime->evidence.clear();
      if (finish_instead_of_cancel ||
          (reason == core::GripperInvalidationReason::kStop &&
           waiting_for_duration))
        runtime->evidence.push_back("completed_by_natural_completion");
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
    // A Stop sends its cancel directly (no scheduler hop on the Stop path);
    // its callers hold no state lock here. Reset and Shutdown keep the
    // scheduled send: Stop settlement reaches the Reset fence under the
    // engine's session mutex, and neither is time-critical.
    if (action_client && goal_handle)
      dispatch_disable_cancel(runtime, goal_ros, goal_handle,
                              disable_command_id, disable_generation, cancel_slot,
                              true, reason == core::GripperInvalidationReason::kStop);
  }
}

void Ros2GripperCommandBackend::dispatch_disable_cancel(
    const std::shared_ptr<GripperRuntime> &runtime,
    const std::shared_ptr<RosEntities> &goal_ros,
    const GoalHandleGripper::SharedPtr &goal, std::uint64_t command_id,
    std::uint64_t generation,
    const std::shared_ptr<core::GripperLateCancelSlot> &slot,
    bool settle_lifecycle_on_accept, bool send_on_calling_thread) {
  if (slot && slot->cancel_queued.exchange(true, std::memory_order_acq_rel))
    return;
  struct CancelItem {
    rclcpp_action::Client<GripperCommand>::SharedPtr client;
    GoalHandleGripper::SharedPtr goal;
    std::shared_ptr<RosEntities::ActionLeaf> leaf;
    std::weak_ptr<GripperRuntime> runtime;
    std::shared_ptr<core::GripperLateCancelSlot> slot;
    std::uint64_t command_id;
    std::uint64_t generation;
    bool settle_lifecycle_on_accept;
  };
  const auto owner = goal_ros->owner;
  auto *const scheduler = &scheduler_;
  try {
    const auto scheduled_at = clock_.now();
    auto item = owner->retain_entity(std::make_shared<CancelItem>(CancelItem{
        goal_ros->action_client, goal, goal_ros->action_leaf, runtime, slot,
        command_id, generation, settle_lifecycle_on_accept}));
    // One non-waiting SDK call under the gripper action leaf; nothing else is
    // acquired while it is held. The caller must hold no state lock.
    const auto send = [owner, scheduler, scheduled_at](
                          const std::shared_ptr<CancelItem> &cancel,
                          bool on_scheduler) noexcept {
      try {
        MANYFORGE_LEAF_LOCK(leaf, cancel->leaf->mutex, detail::LeafEntity::kGripperCancel);
        if (owner->executor_failure_started() || owner->joined())
          throw std::runtime_error("gripper cancel owner failed");
        if (cancel->slot) {
          if (!cancel->slot->cancel_allowed.load(std::memory_order_acquire)) return;
          const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          if (now_ns >= cancel->slot->lifecycle_deadline_ns.load(std::memory_order_acquire))
            throw std::runtime_error("gripper cancel lifecycle deadline expired");
        }
        const auto goal_id = cancel->goal->get_goal_id();
        (void)cancel->client->async_cancel_goal(cancel->goal,
            [weak_runtime = cancel->runtime, slot = cancel->slot,
             command_id = cancel->command_id, generation = cancel->generation,
             settle_lifecycle_on_accept = cancel->settle_lifecycle_on_accept,
             scheduler, scheduled_at,
             goal_id](const auto response) {
          const bool accepted = response &&
              response->return_code == action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
              std::any_of(response->goals_canceling.begin(), response->goals_canceling.end(),
                          [&](const auto &row) { return row.goal_id.uuid == goal_id; });
          if (slot) {
            if (!accepted) {
              mark_gripper_cancel_unissued(slot);
              try {
                (void)scheduler->schedule(scheduled_at, [slot] {
                  handoff_gripper_cancel_failure(slot);
                }, core::SchedulerPriority::StopControl);
              } catch (...) { /* The failed slot remains visible to teardown. */ }
            }
            else {
              settle_gripper_cancel(slot);
            }
          }
          const auto runtime = weak_runtime.lock();
          if (!runtime) return;
          std::lock_guard lock(runtime->mutex);
          if (!runtime->lifecycle_disable_pending ||
              runtime->lifecycle_disable_command_id != command_id ||
              runtime->lifecycle_disable_generation != generation ||
              runtime->lifecycle_disable_terminal) return;
          if (!accepted) runtime->lifecycle_disable_failed = true;
          else if (settle_lifecycle_on_accept && runtime->generation != generation) {
            runtime->lifecycle_disable_terminal = true;
            runtime->lifecycle_disable_failed = false;
            runtime->cancel_pending = false;
            ++runtime->revision;
          }
          runtime->lifecycle_condition.notify_all();
        });
      } catch (...) {
        mark_gripper_cancel_unissued(cancel->slot);
        if (on_scheduler) {
          handoff_gripper_cancel_failure(cancel->slot);
        } else {
          // The failure handoff (a journal write) stays on StopControl, as
          // for a refused cancel response, not on the Stop caller.
          try {
            (void)scheduler->schedule(scheduled_at, [slot = cancel->slot] {
              handoff_gripper_cancel_failure(slot);
            }, core::SchedulerPriority::StopControl);
          } catch (...) { /* The failed slot remains visible to teardown. */ }
        }
      }
    };
    if (send_on_calling_thread) {
      send(item, false);
      return;
    }
    (void)scheduler_.schedule(scheduled_at, [send, item] { send(item, true); },
                              core::SchedulerPriority::StopControl);
  } catch (...) {
    mark_gripper_cancel_unissued(slot);
    std::lock_guard lock(runtime->mutex);
    if (runtime->lifecycle_disable_pending && !runtime->lifecycle_disable_terminal) {
      runtime->lifecycle_disable_failed = true;
      runtime->lifecycle_condition.notify_all();
    }
  }
}

void Ros2GripperCommandBackend::authorize_gripper_commands() noexcept {
  (void)try_authorize_gripper_commands();
}

core::Result<void>
Ros2GripperCommandBackend::try_authorize_gripper_commands() noexcept {
  // Pure authority/runtime state under their own mutexes, which no gripper
  // callback or send holds across a ROS call: run on the calling thread.
  return try_authorize_gripper_commands_impl(resources_.load());
}

core::Result<void>
Ros2GripperCommandBackend::try_authorize_gripper_commands_impl(
    const std::shared_ptr<Ros2TransportResources> &owner) noexcept {
  std::lock_guard<std::mutex> lock(gripper_authority_mutex_);
  if (gripper_terminally_closed_)
    return core::Result<void>::Err(core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The gripper transport has been closed", {}});
  if (resources_.load() != owner)
    return core::Result<void>::Err(core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The gripper transport changed during authorization", {}});
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
  // State only (see try_authorize_gripper_commands): calling thread.
  return authorize_lifecycle_gripper_impl(gripper_id, resources_.load());
}

Ros2GripperLifecycleResult
Ros2GripperCommandBackend::authorize_lifecycle_gripper_impl(
    const std::string &gripper_id, const std::shared_ptr<Ros2TransportResources> &owner) {
  std::lock_guard authority_lock(gripper_authority_mutex_);
  if (gripper_terminally_closed_)
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The gripper transport has been closed", {}}};
  if (resources_.load() != owner)
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The gripper transport changed during authorization", {}}};
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
  const auto owner = resources_.load();
  const auto readiness = gripper_transport_readiness(gripper_id);
  if (!readiness.configured || !readiness.ready ||
      readiness.transport_kind != "ros_gripper_command")
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The exact ROS action gripper command bridge is not ready", {}}};
  // State only (see try_authorize_gripper_commands): calling thread.
  return prepare_lifecycle_gripper_impl(gripper_id, owner);
}

Ros2GripperLifecycleResult
Ros2GripperCommandBackend::prepare_lifecycle_gripper_impl(
    const std::string &gripper_id, const std::shared_ptr<Ros2TransportResources> &owner) {
  std::lock_guard authority_lock(gripper_authority_mutex_);
  if (resources_.load() != owner)
    return {false, false, core::ErrorDTO{
        "ros2_control_gripper_bridge_not_ready",
        "The gripper transport changed during preparation", {}}};
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
    const std::string &gripper_id, const std::stop_token cancellation,
    const std::chrono::steady_clock::time_point deadline) {
  std::shared_ptr<GripperRuntime> runtime;
  GoalHandleGripper::SharedPtr goal;
  rclcpp_action::Client<GripperCommand>::SharedPtr client;
  std::shared_ptr<RosEntities> goal_ros;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::optional<Ros2GripperLifecycleResult> immediate;
  const auto owner = resources_.load();
  [&] {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    if (resources_.load() != owner) {
      immediate = Ros2GripperLifecycleResult{false, false, core::ErrorDTO{
          "ros2_control_gripper_bridge_disable_unsettled",
          "The gripper transport changed during disable", {}}};
      return;
    }
    std::lock_guard map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end()) {
      immediate = Ros2GripperLifecycleResult{false, false, core::ErrorDTO{
          "ros2_control_gripper_bridge_not_configured",
          "The exact gripper command bridge is not configured", {}}};
      return;
    }
    runtime = found->second;
    std::lock_guard runtime_lock(runtime->mutex);
    if (runtime->config.transport_kind != "ros_gripper_command") {
      immediate = Ros2GripperLifecycleResult{false, false, core::ErrorDTO{
          "ros2_control_gripper_bridge_transport_unsupported",
          "Lifecycle disable requires the exact ROS action gripper bridge",
          {}}};
      return;
    }
    if (runtime->lifecycle_disable_pending) {
      if (runtime->lifecycle_disable_terminal) {
        const bool success = !runtime->lifecycle_disable_failed;
        runtime->lifecycle_disable_pending = false;
        immediate = Ros2GripperLifecycleResult{success, true,
                success ? std::optional<core::ErrorDTO>{}
                        : std::optional<core::ErrorDTO>{core::ErrorDTO{
                              "ros2_control_gripper_bridge_disable_unsettled",
                              "The retained gripper cancellation did not settle successfully",
                              {}}}};
        return;
      }
      command_id = runtime->lifecycle_disable_command_id;
      generation = runtime->lifecycle_disable_generation;
      goal = runtime->active_goal_handle;
      goal_ros = goal ? runtime->goal_ros
                      : (runtime->goal_ros ? runtime->goal_ros : runtime->ros);
      if (goal_ros) client = goal_ros->action_client;
    } else {
    gripper_admission_blocked_ = true;
    gripper_motion_admission_blocked_ = true;
    ++gripper_authority_generation_;
    ++gripper_lifecycle_generation_;
    if (!runtime->executing && !runtime->goal_accepted &&
        !runtime->active_goal_handle) {
      immediate = Ros2GripperLifecycleResult{true, true, std::nullopt};
      return;
    }
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
    goal_ros = goal ? runtime->goal_ros
                    : (runtime->goal_ros ? runtime->goal_ros : runtime->ros);
    if (goal_ros) client = goal_ros->action_client;
    }
    if (runtime->goal_late_cancel_slot && deadline != std::chrono::steady_clock::time_point::max()) {
      const auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
      auto &cut = runtime->goal_late_cancel_slot->lifecycle_deadline_ns;
      auto previous = cut.load(std::memory_order_acquire);
      while (deadline_ns < previous &&
             !cut.compare_exchange_weak(previous, deadline_ns, std::memory_order_acq_rel)) {}
    }
  }();
  if (immediate)
    return *immediate;
  if (!goal_ros)
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The exact gripper cancellation remains unsettled", {}}};

  std::stop_source wait_stop;
  std::stop_callback caller_stopped(cancellation, [&wait_stop] { wait_stop.request_stop(); });
  std::stop_callback owner_failed(goal_ros->owner->executor_failure_token(),
                                  [&wait_stop] { wait_stop.request_stop(); });
  std::unique_lock runtime_lock(runtime->mutex);
  if (!goal) {
    runtime->lifecycle_condition.wait_until(runtime_lock, wait_stop.get_token(), deadline, [&] {
      return shutdown_started_.load() ||
             runtime->lifecycle_disable_terminal ||
             runtime->lifecycle_disable_failed ||
             runtime->command_id != command_id ||
             runtime->generation != generation || runtime->active_goal_handle;
    });
    goal = runtime->active_goal_handle;
    goal_ros = goal ? runtime->goal_ros
                    : (runtime->goal_ros ? runtime->goal_ros : runtime->ros);
    client = goal_ros ? goal_ros->action_client : nullptr;
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
  if (goal_ros->owner->executor_failure_started())
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The exact gripper cancellation remains unsettled", {}}};
  const auto cancel_slot = runtime->goal_late_cancel_slot;
  if (runtime->lifecycle_cancel_dispatched ||
      (cancel_slot && cancel_slot->cancel_queued.exchange(true, std::memory_order_acq_rel))) {
    runtime->lifecycle_condition.wait_until(runtime_lock, wait_stop.get_token(), deadline, [&] {
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
  auto *const scheduler = &scheduler_;
  core::TimePoint scheduled_at{};
  bool has_scheduled_at = false;
  bool cancel_sent = false;
  try {
    scheduled_at = clock_.now();
    has_scheduled_at = true;
    {
      MANYFORGE_LEAF_DEFERRED_LOCK(leaf, goal_ros->action_leaf->mutex,
                                  detail::LeafEntity::kGripperCancel);
      const bool locked = deadline == std::chrono::steady_clock::time_point::max()
          ? (leaf.lock(), true) : leaf.try_lock_until(deadline);
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (locked && std::chrono::steady_clock::now() < deadline &&
          !goal_ros->owner->executor_failure_started() && !goal_ros->owner->joined() &&
          (!cancel_slot || (cancel_slot->cancel_allowed.load(std::memory_order_acquire) &&
           now_ns < cancel_slot->lifecycle_deadline_ns.load(std::memory_order_acquire)))) {
      const auto goal_id = goal->get_goal_id();
      (void)client->async_cancel_goal(
        goal, [weak_runtime = std::weak_ptr<GripperRuntime>(runtime), command_id,
               generation, goal_id, cancel_slot, scheduler, scheduled_at](const auto response) {
          const bool accepted = response &&
              response->return_code == action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
              std::any_of(response->goals_canceling.begin(), response->goals_canceling.end(),
                          [&](const auto &row) { return row.goal_id.uuid == goal_id; });
          if (accepted && cancel_slot) {
            settle_gripper_cancel(cancel_slot);
          } else if (!accepted) {
            mark_gripper_cancel_unissued(cancel_slot);
            try {
              (void)scheduler->schedule(scheduled_at, [cancel_slot] {
                handoff_gripper_cancel_failure(cancel_slot);
              }, core::SchedulerPriority::StopControl);
            } catch (...) { /* Teardown retains the failed slot. */ }
          }
          const auto runtime = weak_runtime.lock();
          if (!runtime)
            return;
          std::lock_guard lock(runtime->mutex);
          if (!runtime->lifecycle_disable_pending ||
              runtime->command_id != command_id ||
              runtime->generation != generation)
            return;
          if (!accepted) runtime->lifecycle_disable_failed = true;
          runtime->lifecycle_condition.notify_all();
        });
      cancel_sent = true;
      }
    }
  } catch (...) {
    mark_gripper_cancel_unissued(cancel_slot);
    try { if (has_scheduled_at) (void)scheduler->schedule(scheduled_at, [cancel_slot] {
      handoff_gripper_cancel_failure(cancel_slot);
    }, core::SchedulerPriority::StopControl); } catch (...) {}
    runtime_lock.lock();
    runtime->lifecycle_disable_failed = true;
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_cancel_failed",
        "The gripper cancellation request could not be dispatched", {}}};
  }
  if (!cancel_sent) {
    mark_gripper_cancel_unissued(cancel_slot);
    try { (void)scheduler->schedule(scheduled_at, [cancel_slot] {
      handoff_gripper_cancel_failure(cancel_slot);
    }, core::SchedulerPriority::StopControl); } catch (...) {}
    return {false, true, core::ErrorDTO{
        "ros2_control_gripper_bridge_disable_unsettled",
        "The exact gripper cancellation remains unsettled", {}}};
  }
  runtime_lock.lock();
  runtime->lifecycle_condition.wait_until(runtime_lock, wait_stop.get_token(), deadline, [&] {
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

bool
Ros2GripperCommandBackend::cached_gripper_local_owner_ready(
    const std::string &gripper_id) const {
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end()) return false;
    runtime = found->second;
  }
  std::lock_guard lock(runtime->mutex);
  if (runtime->config.output_mode == core::GripperOutputMode::kNone)
    return runtime->config.actuation_kind == core::GripperActuationKind::kPneumatic &&
           !shutdown_started_.load(std::memory_order_acquire);
  const auto &ros = runtime->ros;
  if (!ros || !ros->owner || ros->owner != resources_.load() ||
      !ros->owner->started() || ros->owner->joined() ||
      ros->owner->executor_failure_started() ||
      shutdown_started_.load(std::memory_order_acquire) ||
      (ros->callbacks_enabled && !ros->callbacks_enabled->load(std::memory_order_acquire)))
    return false;
  if (runtime->config.transport_kind == "isaac_joint_state_effort")
    return ros->command_matches && ros->command_matches->load(std::memory_order_acquire) > 0;
  if (runtime->config.transport_kind == "ros_gripper_command")
    return ros->action_ready && ros->action_ready->load(std::memory_order_acquire) > 0;
  return false;
}

core::Result<void> Ros2GripperCommandBackend::InstallGripperLateCancelJournalHook(
    const std::string &gripper_id, const void *engine_identity,
    LateCancelJournalHook hook) {
  if (!engine_identity || !hook)
    return core::Result<void>::Err({"late_cancel_owner_unavailable",
                                    "Exact gripper journal owner is unavailable", {}});
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end())
      return core::Result<void>::Err({"gripper_not_configured", "Exact gripper is unavailable", {}});
    runtime = found->second;
  }
  std::lock_guard lock(runtime->mutex);
  if (runtime->late_cancel_journal_hook &&
      runtime->late_cancel_engine_identity != engine_identity)
    return core::Result<void>::Err({"late_cancel_owner_changed",
                                    "Gripper journal owner changed", {}});
  runtime->late_cancel_engine_identity = engine_identity;
  runtime->late_cancel_journal_hook = std::move(hook);
  return core::Result<void>::Ok();
}

Ros2GripperCommandBackend::LateCancelShutdownStatus
Ros2GripperCommandBackend::gripper_late_cancel_shutdown_status() const noexcept {
  std::lock_guard lock(late_cancel_tracking_->mutex);
  bool unknown = false;
  for (const auto &entry : late_cancel_tracking_->entries) {
    const auto &slot = entry.slot;
    std::lock_guard slot_lock(slot->mutex);
    if (slot->state != core::GripperLateCancelState::kSettled &&
        slot->state != core::GripperLateCancelState::kHandedOff)
      return LateCancelShutdownStatus::kPendingOrFailed;
    unknown |= slot->state == core::GripperLateCancelState::kHandedOff;
  }
  return unknown ? LateCancelShutdownStatus::kDurableUnknown
                 : LateCancelShutdownStatus::kSettled;
}

bool Ros2GripperCommandBackend::gripper_late_cancel_failed() const noexcept {
  const auto epoch = late_cancel_failure_epoch_->load(std::memory_order_acquire);
  return epoch == std::numeric_limits<std::uint64_t>::max() ||
         epoch != late_cancel_cleared_epoch_.load(std::memory_order_acquire);
}

std::vector<std::shared_ptr<core::GripperPreparedSessionPin>>
Ros2GripperCommandBackend::tracked_gripper_prepared_sessions() const {
  std::vector<std::shared_ptr<core::GripperPreparedSessionPin>> sessions;
  std::lock_guard lock(late_cancel_tracking_->mutex);
  for (const auto &entry : late_cancel_tracking_->entries)
    if (entry.session) sessions.push_back(entry.session);
  return sessions;
}

Ros2GripperCommandBackend::LateCancelShutdownStatus
Ros2GripperCommandBackend::CloseGripperLateCancels(std::chrono::steady_clock::time_point deadline) {
  const auto cutoff = deadline == std::chrono::steady_clock::time_point::max()
      ? std::numeric_limits<std::int64_t>::max()
      : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
  std::vector<std::shared_ptr<core::GripperLateCancelSlot>> slots;
  std::vector<LateCancelTracking::Entry> entries;
  std::vector<std::shared_ptr<RosEntities>> bundles;
  std::vector<rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr> retired_publishers;
  {
    // Registering an action slot takes the same authority before tracking.
    // Fence new sends and publish every cutoff before queuing any cancels.
    std::lock_guard authority_lock(gripper_authority_mutex_);
    gripper_terminally_closed_ = true;
    gripper_admission_blocked_ = true;
    gripper_motion_admission_blocked_ = true;
    ++gripper_authority_generation_;
    ++gripper_lifecycle_generation_;
    {
      std::lock_guard map_lock(gripper_map_mutex_);
      for (const auto &[id, runtime] : grippers_) {
        (void)id;
        std::lock_guard runtime_lock(runtime->mutex);
        if (runtime->ros) {
          if (runtime->ros->publisher_leaf)
            runtime->ros->publisher_leaf->state.store(
                RosEntities::PublisherLeaf::State::kRetired, std::memory_order_release);
          bundles.push_back(runtime->ros);
        }
        if (runtime->goal_ros && runtime->goal_ros != runtime->ros) {
          if (runtime->goal_ros->publisher_leaf)
            runtime->goal_ros->publisher_leaf->state.store(
                RosEntities::PublisherLeaf::State::kRetired, std::memory_order_release);
          bundles.push_back(runtime->goal_ros);
        }
      }
    }
    std::lock_guard lock(late_cancel_tracking_->mutex);
    entries = late_cancel_tracking_->entries;
    for (const auto &entry : entries) slots.push_back(entry.slot);
    for (const auto &slot : slots) {
      auto previous = slot->lifecycle_deadline_ns.load(std::memory_order_acquire);
      while (cutoff < previous && !slot->lifecycle_deadline_ns.compare_exchange_weak(
                 previous, cutoff, std::memory_order_acq_rel)) {}
    }
  }
  invalidate_gripper_commands_impl(core::GripperInvalidationReason::kShutdown);
  const auto queue_failed_handoffs = [&] {
    for (const auto &slot : slots) {
      bool ready = false;
      {
        std::lock_guard lock(slot->mutex);
        ready = slot->state == core::GripperLateCancelState::kFailed &&
                slot->receipt_installed && static_cast<bool>(slot->handoff) &&
                slot->target && !slot->gripper_id.empty();
      }
      if (!ready || slot->shutdown_handoff_queued.exchange(true, std::memory_order_acq_rel))
        continue;
      try {
        (void)scheduler_.schedule(clock_.now(), [slot] {
          handoff_gripper_cancel_failure(slot);
        }, core::SchedulerPriority::StopControl);
      } catch (...) {
        // The exact Failed slot stays visible to the host and journal recovery.
      }
    }
  };
  queue_failed_handoffs();
  {
    std::unique_lock lock(late_cancel_tracking_->mutex);
    late_cancel_tracking_->condition.wait_until(lock, deadline, [&] {
      for (const auto &slot : slots) {
        std::lock_guard slot_lock(slot->mutex);
        if (slot->state == core::GripperLateCancelState::kPending) return false;
      }
      return true;
    });
  }
  // The cutoff prevents subsequent admissions. Acquiring each exact leaf
  // proves any SDK operation admitted before that cutoff has returned.
  for (const auto &entry : entries) {
    if (!entry.bundle || !entry.bundle->action_leaf) continue;
    std::unique_lock leaf(entry.bundle->action_leaf->mutex, std::defer_lock);
    const bool acquired = std::chrono::steady_clock::now() >= deadline
        ? leaf.try_lock() : leaf.try_lock_until(deadline);
    if (!acquired) {
      for (const auto &slot : slots) mark_gripper_cancel_unissued(slot);
      return LateCancelShutdownStatus::kStuck;
    }
  }
  for (const auto &bundle : bundles) {
    if (!bundle || !bundle->publisher_leaf) continue;
    std::unique_lock leaf(bundle->publisher_leaf->mutex, std::defer_lock);
    const bool acquired = std::chrono::steady_clock::now() >= deadline
        ? leaf.try_lock() : leaf.try_lock_until(deadline);
    if (!acquired) {
      for (const auto &slot : slots) mark_gripper_cancel_unissued(slot);
      return LateCancelShutdownStatus::kStuck;
    }
    bundle->publisher_leaf->state.store(RosEntities::PublisherLeaf::State::kRetired,
                                        std::memory_order_release);
    if (bundle->command_matches)
      bundle->command_matches->store(-1, std::memory_order_release);
    if (bundle->joint_state_pub)
      retired_publishers.push_back(std::move(bundle->joint_state_pub));
  }
  // The last bundle alias enters the owner's retirement path only after the
  // leaf is unlocked; the owner finalizer itself takes the exact leaf.
  retired_publishers.clear();
  for (const auto &slot : slots) mark_gripper_cancel_unissued(slot);
  queue_failed_handoffs();
  {
    std::unique_lock lock(late_cancel_tracking_->mutex);
    late_cancel_tracking_->condition.wait_until(lock, deadline, [&] {
      for (const auto &slot : slots) {
        std::lock_guard slot_lock(slot->mutex);
        if (slot->state != core::GripperLateCancelState::kSettled &&
            slot->state != core::GripperLateCancelState::kHandedOff) return false;
      }
      return true;
    });
  }
  return gripper_late_cancel_shutdown_status();
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
  std::shared_ptr<RosEntities> ros;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
      state_subscriber;
  std::optional<std::chrono::steady_clock::time_point> observed_at;
  {
    std::lock_guard<std::mutex> runtime_lock(runtime->mutex);
    runtime_config = runtime->config;
    ros = runtime->ros;
    action_client = ros->action_client;
    state_subscriber = ros->joint_state_sub;
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
      // A thread-safe graph read: made directly, never through the owner.
      if (action_client) {
        result.command_path_ready = action_client->action_server_is_ready();
        // The cached state reading follows this direct read: the server's
        // endpoints can match after the last graph event, which then never
        // refreshes the cache.
        if (ros->action_ready)
          ros->action_ready->store(result.command_path_ready ? 1 : 0, std::memory_order_release);
      }
      result.feedback_path_ready = result.command_path_ready;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_action_server_unavailable");
      }
    } else if (runtime_config.transport_kind == "isaac_joint_state_effort") {
      result.command_path_ready = ros->publisher_leaf &&
          ros->publisher_leaf->state.load(std::memory_order_acquire) == RosEntities::PublisherLeaf::State::kOpen && ros->command_matches &&
          ros->command_matches->load(std::memory_order_acquire) > 0;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_command_subscriber_unavailable");
      }
      if (runtime_config.state_topic.empty()) {
        result.feedback_path_ready = true;
      } else {
        // A thread-safe graph read: made directly, never through the owner.
        if (state_subscriber)
          result.feedback_publisher_count =
              state_subscriber->get_publisher_count();
        if (observed_at.has_value() && runtime_config.feedback_stream) {
          // The pushed `gripper_feedback` stream owns staleness.
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
    bool same = current != grippers_.end() && current->second == runtime;
    if (same) {
      std::lock_guard runtime_lock(runtime->mutex);
      same = runtime->ros == ros;
    }
    if (!same) {
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
  // Including admission here would create a startup cycle: the execution host
  // needs readiness before it opens the route whose existing reset flow opens
  // the gate.
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
  GoalHandleGripper::SharedPtr retired_goal;
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
  std::shared_ptr<RosEntities> ros;
  {
    std::lock_guard lock(runtime->mutex);
    ros = runtime->ros;
  }
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
    // Position commands wait for graph discovery (no lock is held here);
    // pneumatic commands fail fast on the current server state.
    const bool ready =
        runtime_config.actuation_kind == core::GripperActuationKind::kPneumatic
            ? ros->action_client->action_server_is_ready()
            : detail::wait_for_action_server_until(
                  ros->action_client,
                  runtime_config.wait_for_action_server_timeout_s);
    if (!ready) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperActionServerUnavailable,
          runtime_config.actuation_kind ==
                  core::GripperActuationKind::kPneumatic
              ? "Gripper action server is unavailable: " +
                    runtime_config.action_name
              : "Gripper action server is unavailable: " +
                    runtime_config.action_name);
    }
  }

  const auto command_id =
      next_gripper_command_id_.fetch_add(1, std::memory_order_relaxed);
  const auto generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<RosEntities> prior_goal_ros;
  // Called under runtime->mutex only before an SDK send has happened.
  const auto owner_closed_before_send = [&]() {
    if (!runtime->executing || runtime->command_id != command_id ||
        runtime->generation != generation)
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command was invalidated before dispatch", command_id,
          generation);
    runtime->state = "failed";
    runtime->executing = false;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->goal_ros = prior_goal_ros;
    runtime->projection_state = core::GripperProjectionState::kNotRequired;
    runtime->projection_reason.clear();
    runtime->reason_code = ros2_control_fault_reason::kGripperGoalDispatchFailed;
    runtime->error = "The gripper ROS owner closed before command dispatch";
    if (runtime->lifecycle_disable_pending &&
        runtime->lifecycle_disable_command_id == command_id &&
        runtime->lifecycle_disable_generation == generation) {
      runtime->lifecycle_disable_terminal = true;
      runtime->lifecycle_disable_failed = false;
      runtime->lifecycle_condition.notify_all();
    }
    ++runtime->revision;
    return rejected_gripper_command(runtime->reason_code, runtime->error,
                                    command_id, generation);
  };
  // The authority lock is held and this dispatch has not entered an SDK
  // send. A concurrent Disable may already be waiting for a goal response
  // which cannot arrive for this exact never-sent command.
  const auto authority_closed_before_send = [&]() {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing && runtime->command_id == command_id &&
        runtime->generation == generation) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->goal_ros = prior_goal_ros;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
      runtime->reason_code = "gripper_command_not_authorized";
      runtime->error = "Gripper lifecycle authority changed before command dispatch";
      if (runtime->lifecycle_disable_pending &&
          runtime->lifecycle_disable_command_id == command_id &&
          runtime->lifecycle_disable_generation == generation) {
        runtime->lifecycle_disable_terminal = true;
        runtime->lifecycle_disable_failed = false;
        runtime->lifecycle_condition.notify_all();
      }
      ++runtime->revision;
    }
    return rejected_gripper_command(
        "gripper_command_not_authorized",
        "Gripper lifecycle authority changed before command dispatch",
        command_id, generation);
  };
  std::optional<core::ScheduleHandle> superseded_lifecycle_handle;
  std::optional<core::GripperCommandResultDTO> admission_result;
  {
    const auto admit = [&]() -> std::optional<core::GripperCommandResultDTO> {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (!admission_authorized() || !runtime_is_current()) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper lifecycle authority changed during command admission");
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->ros != ros)
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper transport changed during command admission");
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
    prior_goal_ros = runtime->goal_ros;
    runtime->scene_projection_submitted = false;
    runtime->scene_projection_submitted_request_id = 0;
    runtime->scene_projection_missed_settled = false;
    if (runtime_config.output_mode != core::GripperOutputMode::kNone)
      runtime->goal_ros = ros;
    superseded_lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->state = "executing";
    runtime->executing = true;
    runtime->command_id = command_id;
    runtime->addressed_identity = request.addressed_identity;
    runtime->generation = generation;
    runtime->send_generation.store(generation, std::memory_order_release);
    ++runtime->revision;
    runtime->progress = 0.0;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->stop_lets_goal_finish = false;
    runtime->start_position = start_position;
    runtime->current_position = start_position;
    runtime->target_position = request.target_position;
    runtime->duration_s = request.duration_s;
    runtime->wait_for_duration = request.wait_for_duration;
    runtime->controller_done = false;
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
    retired_goal = std::move(runtime->active_goal_handle);
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
    return std::nullopt;
    };
    admission_result = admit();
  }
  if (admission_result)
    return *admission_result;
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
    std::optional<core::GripperCommandResultDTO> dispatch_result;
    {
      const auto dispatch = [&]() -> std::optional<core::GripperCommandResultDTO> {
      std::unique_lock<std::mutex> authority_lock(gripper_authority_mutex_);
      if (!admission_authorized() || !runtime_is_current()) {
        auto refused = authority_closed_before_send();
        if (ros->publisher_leaf && ros->publisher_leaf->state.load(std::memory_order_acquire) != RosEntities::PublisherLeaf::State::kOpen)
          refused.error = core::ErrorDTO{"ros2_control_gripper_bridge_not_ready",
              "The exact gripper command publisher is unavailable", {}};
        return refused;
      }
      std::unique_lock<std::mutex> lock(runtime->mutex);
      if (runtime->ros != ros && runtime->executing && runtime->command_id == command_id &&
          runtime->generation == generation)
        return owner_closed_before_send();
      if (!runtime->executing || runtime->command_id != command_id ||
          runtime->generation != generation) {
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper command was invalidated before dispatch", command_id,
            generation);
      }
      bool entered_publish = false;
      try {
        lock.unlock();
        authority_lock.unlock();
        bool sent = false;
        bool publisher_missing = false;
        try {
        const auto msg = make_isaac_joint_state_gripper_command(
            ros->owner->node(), runtime_config, request.target_position);
        {
          MANYFORGE_LEAF_LOCK(leaf, ros->publisher_leaf->mutex,
                              detail::LeafEntity::kGripperPublish);
          const bool pneumatic = runtime_config.actuation_kind == core::GripperActuationKind::kPneumatic;
          const bool proof_current = !pneumatic || !owner_proof_supplied ||
              (request.admission_token && request.admission_token->load(std::memory_order_acquire) ==
                  request.admission_token_expected);
          publisher_missing = ros->publisher_leaf->state.load(std::memory_order_acquire) != RosEntities::PublisherLeaf::State::kOpen || !ros->joint_state_pub;
          if (!publisher_missing && !ros->owner->executor_failure_started() && !ros->owner->joined() && proof_current &&
              runtime->send_generation.load(std::memory_order_acquire) == generation &&
              gripper_lifecycle_generation_.load(std::memory_order_acquire) == lifecycle_generation &&
              (pneumatic || gripper_authority_generation_.load(std::memory_order_acquire) == authority_generation)) {
            entered_publish = true;
            MANYFORGE_GRIPPER_PUBLISH_ENTERED();
            ros->joint_state_pub->publish(msg);
            sent = true;
          }
        }
        } catch (...) {
          authority_lock.lock();
          lock.lock();
          throw;
        }
        authority_lock.lock();
        lock.lock();
        if (!sent) {
          auto refused = owner_closed_before_send();
          if (publisher_missing)
            refused.error = core::ErrorDTO{"ros2_control_gripper_bridge_not_ready",
                "The exact gripper command publisher is unavailable", {}};
          return refused;
        }
        if (runtime->command_id != command_id || runtime->generation != generation)
          return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
        if (runtime_config.actuation_kind ==
            core::GripperActuationKind::kPneumatic) {
          add_unique_evidence(runtime->evidence,
                              "isaac_gripper_command_published");
          confirm_pneumatic_delivery_locked(*runtime);
        }
      } catch (const std::exception &exc) {
        if (runtime->command_id != command_id || runtime->generation != generation)
          return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
        if (entered_publish && ros->owner->executor_failure_started())
          return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
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
        if (runtime->command_id != command_id || runtime->generation != generation)
          return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
        if (entered_publish && ros->owner->executor_failure_started())
          return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
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
      return std::nullopt;
      };
      dispatch_result = dispatch();
    }
    if (dispatch_result)
      return *dispatch_result;
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

  // One finger joint; an empty effort leaves the controller's configured
  // max_effort (used only when it claims a max_effort_interface).
  GripperCommand::Goal goal;
  goal.command.position = {request.target_position};
  if (request.max_effort.has_value()) {
    goal.command.effort = {max_effort};
  }

  rclcpp_action::Client<GripperCommand>::SendGoalOptions options;
  // The exact goal obligation exists before SDK admission, so a late response
  // remains visible even if a newer prepared session becomes resident.
  const auto late_cancel_slot = request.late_cancel_slot
      ? request.late_cancel_slot : std::make_shared<core::GripperLateCancelSlot>();
  std::vector<LateCancelTracking::Entry> retired_entries;
  {
    std::lock_guard authority_lock(gripper_authority_mutex_);
    if (!admission_authorized() || !runtime_is_current())
      return authority_closed_before_send();
    {
    std::lock_guard lock(runtime->mutex);
    std::lock_guard slot_lock(late_cancel_slot->mutex);
    late_cancel_slot->command_id = command_id;
    late_cancel_slot->generation = generation;
    late_cancel_slot->gripper_id = request.gripper_id;
    late_cancel_slot->handoff = runtime->late_cancel_journal_hook;
    late_cancel_slot->failure_epoch = late_cancel_failure_epoch_;
    late_cancel_slot->state_changed = [weak = std::weak_ptr<LateCancelTracking>(late_cancel_tracking_)] {
      if (const auto tracking = weak.lock()) {
        std::lock_guard lock(tracking->mutex);
        tracking->condition.notify_all();
      }
    };
    late_cancel_slot->state = core::GripperLateCancelState::kPending;
    if (runtime->command_id == command_id && runtime->generation == generation)
      runtime->goal_late_cancel_slot = late_cancel_slot;
    }
    std::lock_guard lock(late_cancel_tracking_->mutex);
    auto &entries = late_cancel_tracking_->entries;
    for (auto it = entries.begin(); it != entries.end();) {
      bool settled = false;
      {
        std::lock_guard slot_lock(it->slot->mutex);
        settled = it->slot->state == core::GripperLateCancelState::kSettled;
      }
      if (settled) {
        retired_entries.push_back(std::move(*it));
        it = entries.erase(it);
      } else ++it;
    }
    entries.push_back({request.gripper_id, late_cancel_slot, runtime, ros,
                       request.prepared_session_pin});
  }
  // Runtime state is generation-owned. Backend access is limited to cancelling
  // the owner-scheduled deadline; the retained host quiesces the ROS executor
  // before adapter destruction. The runtime owns the action client, which owns
  // these callbacks (pending requests and goal handles), so they hold the
  // runtime weakly: a strong capture forms a cycle that leaks the client, node
  // and DDS participant whenever a goal never reaches a final result.
  const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
  const std::weak_ptr<RosEntities> weak_ros = ros;
  options.goal_response_callback = [this, weak_runtime, weak_ros, late_cancel_slot, command_id,
                                    generation](const GoalHandleGripper::SharedPtr& goal_handle) {
    if (!goal_handle) {
      settle_gripper_cancel(late_cancel_slot);
    }
    const auto runtime = weak_runtime.lock();
    const auto ros = weak_ros.lock();
    if (!runtime || !ros)
      return;
    const auto retained_goal = goal_handle ? ros->owner->retain_entity(goal_handle)
                                           : GoalHandleGripper::SharedPtr{};
    GoalHandleGripper::SharedPtr retired_callback_goal;
    if (ros->action_graph_observer) (void)ros->action_graph_observer->request_refresh();
    GoalHandleGripper::SharedPtr handle_to_cancel;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id != command_id || runtime->generation != generation) {
        // A Stop that let this goal finish never cancels it (see
        // invalidate_gripper_commands); every other stale goal is cancelled.
        if (!(runtime->stop_lets_goal_finish &&
              runtime->lifecycle_disable_command_id == command_id &&
              runtime->lifecycle_disable_generation == generation)) {
          handle_to_cancel = retained_goal;
          action_client = ros->action_client;
        }
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
        retired_callback_goal = std::move(runtime->active_goal_handle);
        runtime->active_goal_handle = retained_goal;
        if (!runtime->lifecycle_cancel_dispatched) {
          runtime->lifecycle_cancel_dispatched = true;
          handle_to_cancel = retained_goal;
          action_client = ros->action_client;
        }
        runtime->lifecycle_condition.notify_all();
        ++runtime->revision;
      } else if (runtime->cancel_pending || !runtime->executing) {
        lifecycle_handle = runtime->lifecycle_handle;
        runtime->lifecycle_handle.reset();
        ++runtime->lifecycle_schedule_generation;
        handle_to_cancel = retained_goal;
        action_client = ros->action_client;
        retired_callback_goal = std::move(runtime->active_goal_handle);
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
        retired_callback_goal = std::move(runtime->active_goal_handle);
        runtime->active_goal_handle = retained_goal;
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
    if (action_client && handle_to_cancel)
      dispatch_disable_cancel(runtime, ros, handle_to_cancel,
                              command_id, generation, late_cancel_slot);
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
      [this, weak_runtime, weak_ros, late_cancel_slot, command_id,
       generation](const GoalHandleGripper::WrappedResult &result) {
        if (result.code != rclcpp_action::ResultCode::UNKNOWN) {
          settle_gripper_cancel(late_cancel_slot);
        }
        if (const auto ros = weak_ros.lock(); ros && ros->action_graph_observer)
          (void)ros->action_graph_observer->request_refresh();
        if (const auto runtime = weak_runtime.lock())
          on_gripper_result(runtime, command_id, generation, result);
      };

  std::optional<core::GripperCommandResultDTO> dispatch_result;
  bool sdk_entered = false;
  {
    const auto dispatch = [&]() -> std::optional<core::GripperCommandResultDTO> {
    std::unique_lock<std::mutex> authority_lock(gripper_authority_mutex_);
    if (!admission_authorized() || !runtime_is_current()) {
      return authority_closed_before_send();
    }
    std::unique_lock<std::mutex> lock(runtime->mutex);
    if (runtime->ros != ros && runtime->executing && runtime->command_id == command_id &&
        runtime->generation == generation)
      return owner_closed_before_send();
    if (!runtime->executing || runtime->command_id != command_id ||
        runtime->generation != generation) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command was invalidated before dispatch", command_id,
          generation);
    }
    const auto without_owner_locks = [&](auto &&operation) {
      lock.unlock();
      authority_lock.unlock();
      try {
        operation();
      } catch (...) {
        authority_lock.lock();
        lock.lock();
        throw;
      }
      authority_lock.lock();
      lock.lock();
    };
    bool server_ready = true;
    if (runtime_config.actuation_kind == core::GripperActuationKind::kPneumatic)
      without_owner_locks([&] {
        server_ready = ros->action_client->action_server_is_ready();
      });
    if (!runtime->executing || runtime->command_id != command_id ||
        runtime->generation != generation)
      return rejected_gripper_command("gripper_command_not_authorized",
                                     "Gripper command was invalidated before dispatch",
                                     command_id, generation);
    if (!server_ready) {
      runtime->goal_ros = prior_goal_ros;
      runtime->state = "failed";
      runtime->executing = false;
      runtime->externally_failed = true;
      runtime->reason_code =
          ros2_control_fault_reason::kGripperActionServerUnavailable;
      runtime->error = "Gripper action server disappeared before dispatch";
      if (runtime->lifecycle_disable_pending &&
          runtime->lifecycle_disable_command_id == command_id &&
          runtime->lifecycle_disable_generation == generation) {
        runtime->lifecycle_disable_terminal = true;
        runtime->lifecycle_disable_failed = false;
        runtime->lifecycle_condition.notify_all();
      }
      add_unique_evidence(runtime->evidence,
                          "pneumatic_output_delivery_unconfirmed");
      ++runtime->revision;
      return rejected_gripper_command(runtime->reason_code, runtime->error,
                                      command_id, generation);
    }
    try {
      bool sent = false;
      without_owner_locks([&] {
        MANYFORGE_LEAF_LOCK(leaf, ros->action_leaf->mutex,
                            detail::LeafEntity::kGripperGoal);
        const bool pneumatic = runtime_config.actuation_kind ==
                               core::GripperActuationKind::kPneumatic;
        const bool proof_current = !pneumatic || !owner_proof_supplied ||
            (request.admission_token &&
             request.admission_token->load(std::memory_order_acquire) ==
                 request.admission_token_expected);
        if (ros->owner->executor_failure_started() || ros->owner->joined() || !proof_current ||
            runtime->send_generation.load(std::memory_order_acquire) != generation ||
            gripper_lifecycle_generation_.load(std::memory_order_acquire) != lifecycle_generation ||
            (!pneumatic && gripper_authority_generation_.load(std::memory_order_acquire) != authority_generation))
          return;
        sdk_entered = true;
        (void)ros->action_client->async_send_goal(goal, options);
        sent = true;
      });
      if (!sent) return owner_closed_before_send();
      if (runtime->command_id != command_id || runtime->generation != generation)
        return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
      if (runtime_config.actuation_kind ==
          core::GripperActuationKind::kPneumatic)
        add_unique_evidence(runtime->evidence, "ros_gripper_command_enqueued");
    } catch (const std::exception &exc) {
      if (runtime->command_id != command_id || runtime->generation != generation)
        return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
      if (ros->owner->executor_failure_started()) {
        // The SDK was entered, but this owner failed before its send outcome
        // could be established. Keep the exact command resident and unknown.
        return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
      }
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
      if (runtime->command_id != command_id || runtime->generation != generation)
        return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
      if (ros->owner->executor_failure_started())
        return core::GripperCommandResultDTO{true, command_id, generation, std::nullopt};
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
    return std::nullopt;
    };
    dispatch_result = dispatch();
  }
  if (dispatch_result) {
    if (!sdk_entered || !dispatch_result->accepted) {
      if (sdk_entered) mark_gripper_cancel_unissued(late_cancel_slot);
      else settle_gripper_cancel(late_cancel_slot);
    }
    return *dispatch_result;
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
  return mark_gripper_grasp_lost_impl(
      gripper_id, expected_command_id, expected_generation, evidence_label,
      message, reason_code);
}

core::GripperCommandResultDTO
Ros2GripperCommandBackend::mark_gripper_grasp_lost_impl(
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
  std::shared_ptr<RosEntities> goal_ros;
  std::shared_ptr<core::GripperLateCancelSlot> cancel_slot;
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
    cancel_slot = runtime->goal_late_cancel_slot;
    goal_ros = goal_handle ? runtime->goal_ros : runtime->ros;
    if (goal_ros) action_client = goal_ros->action_client;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->generation = generation;
    runtime->send_generation.store(generation, std::memory_order_release);
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

  if (action_client && goal_handle)
    dispatch_disable_cancel(runtime, goal_ros, goal_handle, command_id,
                            expected_generation, cancel_slot, false);
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
      if (runtime->command_id != command_id || runtime->generation != generation ||
          runtime->lifecycle_schedule_generation != schedule_generation)
        return;
      runtime->lifecycle_handle.reset();
      advance_pneumatic_locked(*runtime);
      active = runtime->executing &&
               !(runtime->config.transport_kind == "ros_gripper_command" &&
                 old_effect_owner_retired(*runtime));
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
  std::shared_ptr<RosEntities> ros;
  std::shared_ptr<core::GripperLateCancelSlot> cancel_slot;
  Ros2GripperActionConfig runtime_config;
  std::optional<double> scene_position;
  bool schedule_next = false;
  core::TimePoint next_deadline_ns = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id || runtime->generation != generation ||
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

    if (runtime->executing && runtime->controller_done) {
      // The controller is done; complete once duration_s elapsed.
      if (elapsed_ns >= duration_ns) {
        runtime->state = "completed";
        runtime->executing = false;
        runtime->progress = 1.0;
        scene_position =
            runtime->current_position.value_or(runtime->target_position);
      }
    } else if (runtime->executing && runtime->deadline_at_ns.has_value() &&
        now_ns >= *runtime->deadline_at_ns) {
      // Order timeout against failure while the exact command is locked.
      if (runtime_config.transport_kind == "ros_gripper_command" &&
          old_effect_owner_retired(*runtime))
        return;
      goal_handle = runtime->active_goal_handle;
      cancel_slot = runtime->goal_late_cancel_slot;
      ros = goal_handle ? runtime->goal_ros : runtime->ros;
      if (ros) action_client = ros->action_client;
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
      if (old_effect_owner_retired(*runtime))
        return;
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
        (runtime->controller_done ||
         runtime_config.transport_kind == "isaac_joint_state_effort")) {
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
  if (ros && ros->action_graph_observer)
    (void)ros->action_graph_observer->request_refresh();
  if (action_client && goal_handle)
    dispatch_disable_cancel(runtime, ros, goal_handle, command_id,
                            generation, cancel_slot, false);
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
  status.cancellation_pending = runtime->lifecycle_disable_pending &&
                                !runtime->lifecycle_disable_terminal &&
                                !runtime->lifecycle_disable_failed;
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
  // Opt-in stall-window grasp evidence of the current completed command,
  // derived from this same published state: the measured finger position
  // (else the terminal one) inside the configured window. It belongs to this
  // command only and is retired by the next one.
  const auto terminal_position = status.observed_position.has_value()
                                     ? status.observed_position
                                     : runtime->current_position;
  if (runtime->config.grasp_stall_window &&
      status.state == core::GripperCommandState::kCompleted &&
      terminal_position.has_value()) {
    const auto &window = *runtime->config.grasp_stall_window;
    core::GripperGraspObservationDTO observation;
    observation.gripper_id = gripper_id;
    observation.command_id = runtime->command_id;
    observation.generation = runtime->generation;
    observation.expected_status_revision = runtime->revision;
    observation.confirmed = *terminal_position >= window.min_position &&
                            *terminal_position <= window.max_position;
    // The command's own start time keeps this status free of wall time.
    observation.observed_at_ns = runtime->started_at_ns;
    observation.attribution = "stall_position_window";
    // The source is this gripper's own command state, never a foreign binding.
    status.grasp_evidence = core::GripperGraspEvidenceDTO{
        core::GraspConfirmationSourceKind::kForceCurrent,
        core::ControlBindingId(gripper_id),
        core::ControlBindingRevision("stall_position_window"),
        core::RuntimeEpoch(std::to_string(runtime->generation)),
        std::move(observation), std::numeric_limits<core::TimePoint>::max()};
  }
  return status;
}

} // namespace manyforge::ros2_control_execution
