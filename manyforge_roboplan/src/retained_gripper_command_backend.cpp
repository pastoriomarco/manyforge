#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <manyforge_roboplan/retained_gripper_command_backend.hpp>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

constexpr int kTerminalProjectionRetryLimit = 10;

[[nodiscard]] core::ErrorDTO gripper_error(std::string reason,
                                           std::string message) {
  return core::ErrorDTO{std::move(reason), std::move(message), {}};
}

[[nodiscard]] double clamp_progress(const double progress) {
  return std::clamp(progress, 0.0, 1.0);
}

[[nodiscard]] bool supersedable_grasp_loss(
    const std::optional<core::ErrorDTO>& error) noexcept {
  return error.has_value() && error->code == "grasp_confirmation_lost";
}

[[nodiscard]] core::Duration duration_ns(const double duration_s) {
  constexpr double kNanosPerSecond = 1'000'000'000.0;
  const double nanos = duration_s * kNanosPerSecond;
  if (!std::isfinite(nanos) ||
      nanos >=
          static_cast<double>(std::numeric_limits<core::Duration>::max())) {
    return std::numeric_limits<core::Duration>::max();
  }
  return static_cast<core::Duration>(nanos);
}

[[nodiscard]] core::TimePoint saturating_deadline(
    const core::TimePoint started_at, const core::Duration duration) {
  const auto maximum = std::numeric_limits<core::TimePoint>::max();
  if (duration > 0 && started_at > maximum - duration) {
    return maximum;
  }
  return started_at + duration;
}

[[nodiscard]] double position_value(
    const core::CoordinatePositionDTO& position) noexcept {
  if (const auto* angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

[[nodiscard]] std::string projection_reason(
    const RetainedStateProjectionResult& result) {
  if (!result.error.has_value()) {
    switch (result.status) {
      case RetainedStateProjectionStatus::kApplied:
        return {};
      case RetainedStateProjectionStatus::kBusy:
        return "retained_state_projection_busy";
      case RetainedStateProjectionStatus::kRejected:
        return "retained_state_projection_rejected";
      case RetainedStateProjectionStatus::kFailed:
        return "retained_state_projection_failed";
    }
  }
  const auto found = result.error->details.find("reason");
  return found == result.error->details.end() ? result.error->code
                                              : found->second;
}

}  // namespace

struct RetainedGripperCommandBackend::SharedState {
  struct Runtime final {
    core::GripperCommandStatusDTO status;
    double start_position{};
    core::TimePoint started_at_ns{};
    core::TimePoint deadline_ns{};
    std::optional<core::ScheduleHandle> scheduled_handle;
    std::uint64_t schedule_generation{};
    int terminal_projection_attempts{};
  };

  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  core::IClock* clock{};
  core::IScheduler* scheduler{};
  core::Duration update_period_ns{10'000'000};
  std::string gripper_id;
  std::string group_id;
  std::string joint_name;
  std::string actuation_kind;
  std::string state_authority;
  std::optional<RetainedGripperPositionLimits> position_limits;

  mutable std::mutex mutex;
  std::map<std::string, Runtime> runtimes;
  std::uint64_t next_command_id{1U};
  std::uint64_t next_generation{1U};
  std::uint64_t authority_generation{1U};
  bool admission_blocked{};
  bool closing{};
};

namespace {

using SharedState = RetainedGripperCommandBackend::SharedState;

struct ScheduleAttempt final {
  std::atomic<bool> callback_admitted{false};
};

[[nodiscard]] bool schedule_tick(const std::shared_ptr<SharedState>& state,
                                 const std::string& gripper_id,
                                 std::uint64_t command_id,
                                 std::uint64_t generation,
                                 core::TimePoint deadline_ns);

void update_projection_result_locked(
    SharedState::Runtime& runtime, const RetainedStateProjectionResult& result,
    const bool terminal) {
  if (result.applied()) {
    runtime.status.projection_state = core::GripperProjectionState::kApplied;
    runtime.status.projection_reason.clear();
    runtime.terminal_projection_attempts = 0;
    if (terminal) {
      runtime.status.state = core::GripperCommandState::kCompleted;
      runtime.status.progress = 1.0;
    }
  } else if (result.status == RetainedStateProjectionStatus::kBusy) {
    runtime.status.projection_state = core::GripperProjectionState::kPending;
    runtime.status.projection_reason = projection_reason(result);
    if (terminal && ++runtime.terminal_projection_attempts >=
                        kTerminalProjectionRetryLimit) {
      runtime.status.state = core::GripperCommandState::kFailed;
      runtime.status.projection_state = core::GripperProjectionState::kFailed;
      runtime.status.error = gripper_error(
          "gripper_projection_failed",
          "Timed out publishing the terminal native gripper position");
    }
  } else {
    runtime.status.state = core::GripperCommandState::kFailed;
    runtime.status.projection_state = core::GripperProjectionState::kFailed;
    runtime.status.projection_reason = projection_reason(result);
    runtime.status.error = gripper_error(
        "gripper_projection_failed",
        "Native gripper state could not be committed to its retained target");
  }
  ++runtime.status.revision;
}

void run_tick(const std::shared_ptr<SharedState>& state,
              const std::string& gripper_id, const std::uint64_t command_id,
              const std::uint64_t generation,
              const std::uint64_t schedule_generation,
              const std::shared_ptr<ScheduleAttempt>& attempt) {
  double position = 0.0;
  bool terminal = false;
  core::TimePoint next_deadline = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (state->closing || found == state->runtimes.end()) {
      return;
    }
    auto& runtime = found->second;
    if (runtime.status.command_id != command_id ||
        runtime.status.generation != generation ||
        runtime.schedule_generation != schedule_generation) {
      return;
    }
    attempt->callback_admitted.store(true, std::memory_order_release);
    runtime.scheduled_handle.reset();
    if (runtime.status.state == core::GripperCommandState::kFailed ||
        runtime.status.state == core::GripperCommandState::kIdle) {
      return;
    }

    const auto now_ns = state->clock->now();
    const auto total_ns = std::max<core::Duration>(
        1, runtime.deadline_ns - runtime.started_at_ns);
    const auto elapsed_ns =
        std::max<core::Duration>(0, now_ns - runtime.started_at_ns);
    const double progress =
        runtime.deadline_ns <= runtime.started_at_ns
            ? 1.0
            : clamp_progress(static_cast<double>(elapsed_ns) /
                             static_cast<double>(total_ns));
    position =
        runtime.start_position +
        (*runtime.status.target_position - runtime.start_position) * progress;
    runtime.status.progress = progress;
    runtime.status.current_position = position;
    runtime.status.observed_position = position;
    terminal = progress >= 1.0;
    ++runtime.status.revision;
    next_deadline = terminal ? now_ns + state->update_period_ns
                             : std::min(runtime.deadline_ns,
                                        now_ns + state->update_period_ns);
  }

  bool schedule_again = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (state->closing || found == state->runtimes.end() ||
        found->second.status.command_id != command_id ||
        found->second.status.generation != generation ||
        found->second.schedule_generation != schedule_generation) {
      return;
    }

    // Holding this authority lock linearizes invalidation/replacement against
    // the external state commit. Best-effort projection never waits on the
    // shared scene lock.
    RetainedStateProjectionResult projection;
    try {
      const std::array<double, 1U> positions{position};
      const std::array<double, 1U> velocities{0.0};
      const auto now_ns = state->clock->now();
      projection = state->state_channel->publish_and_project(
          positions, velocities, now_ns, now_ns,
          RetainedStateProjectionMode::kBestEffort);
    } catch (const std::exception& error) {
      projection = {
          RetainedStateProjectionStatus::kFailed,
          gripper_error("gripper_projection_exception", error.what())};
    } catch (...) {
      projection = {RetainedStateProjectionStatus::kFailed,
                    gripper_error("gripper_projection_exception",
                                  "Native gripper state projection threw")};
    }
    update_projection_result_locked(found->second, projection, terminal);
    schedule_again =
        found->second.status.state == core::GripperCommandState::kExecuting ||
        (found->second.status.state == core::GripperCommandState::kCompleted &&
         found->second.status.projection_state ==
             core::GripperProjectionState::kPending);
  }
  if (schedule_again) {
    (void)schedule_tick(state, gripper_id, command_id, generation,
                        next_deadline);
  }
}

bool schedule_tick(const std::shared_ptr<SharedState>& state,
                   const std::string& gripper_id,
                   const std::uint64_t command_id,
                   const std::uint64_t generation,
                   const core::TimePoint deadline_ns) {
  std::optional<core::ScheduleHandle> old_handle;
  std::uint64_t schedule_generation = 0U;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (state->closing || found == state->runtimes.end() ||
        found->second.status.command_id != command_id ||
        found->second.status.generation != generation) {
      return false;
    }
    old_handle = found->second.scheduled_handle;
    found->second.scheduled_handle.reset();
    schedule_generation = ++found->second.schedule_generation;
  }
  if (old_handle.has_value()) {
    try {
      state->scheduler->cancel(*old_handle);
    } catch (...) {
      // The incremented schedule generation already makes it inert.
    }
  }

  const std::weak_ptr<SharedState> weak_state = state;
  const auto attempt = std::make_shared<ScheduleAttempt>();
  core::ScheduleHandle handle{};
  try {
    handle = state->scheduler->schedule(
        deadline_ns,
        [weak_state, attempt, gripper_id, command_id, generation,
         schedule_generation]() {
          if (const auto locked = weak_state.lock()) {
            run_tick(locked, gripper_id, command_id, generation,
                     schedule_generation, attempt);
          }
        },
        core::SchedulerPriority::ControllerCommand);
  } catch (...) {
    if (attempt->callback_admitted.load(std::memory_order_acquire)) {
      return true;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (!state->closing && found != state->runtimes.end() &&
        found->second.status.command_id == command_id &&
        found->second.status.generation == generation &&
        found->second.schedule_generation == schedule_generation) {
      found->second.status.state = core::GripperCommandState::kFailed;
      found->second.status.projection_state =
          core::GripperProjectionState::kNotRequired;
      found->second.status.projection_reason.clear();
      found->second.status.error =
          gripper_error("gripper_request_failed",
                        "Native gripper lifecycle could not be scheduled");
      ++found->second.status.revision;
    }
    return false;
  }

  bool admitted = attempt->callback_admitted.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (!state->closing && found != state->runtimes.end() &&
        found->second.status.command_id == command_id &&
        found->second.status.generation == generation) {
      if (found->second.schedule_generation == schedule_generation &&
          !attempt->callback_admitted.load(std::memory_order_acquire)) {
        found->second.scheduled_handle = handle;
      }
      admitted = true;
    }
  }
  if (!admitted) {
    try {
      state->scheduler->cancel(handle);
    } catch (...) {
      // A stale callback is generation-fenced at entry.
    }
  }
  return admitted;
}

[[nodiscard]] bool exact_request(const core::GripperCommandRequestDTO& request,
                                 const SharedState& state) noexcept {
  return request.gripper_id == state.gripper_id &&
         request.group_id == state.group_id &&
         request.joint_name == state.joint_name &&
         request.actuation_kind == state.actuation_kind &&
         request.state_authority == state.state_authority &&
         request.commanded_state == "position" &&
         request.transport_kind == "resolved_binding";
}

[[nodiscard]] std::optional<core::ErrorDTO> validate_request(
    const core::GripperCommandRequestDTO& request, const SharedState& state) {
  if (!exact_request(request, state) ||
      !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || request.duration_s < 0.0 ||
      !std::isfinite(request.completion_timeout_s) ||
      request.completion_timeout_s <= 0.0 ||
      request.duration_s > request.completion_timeout_s ||
      (request.max_effort.has_value() &&
       (!std::isfinite(*request.max_effort) || *request.max_effort < 0.0))) {
    return gripper_error(
        "gripper_invalid_command",
        "Native gripper command differs from its exact retained target");
  }
  if (state.position_limits.has_value() &&
      (request.target_position < state.position_limits->lower_position ||
       request.target_position > state.position_limits->upper_position)) {
    return gripper_error(
        "gripper_position_limit_violation",
        "Native gripper position is outside its retained URDF limits");
  }
  return std::nullopt;
}

}  // namespace

RetainedGripperCommandBackend::RetainedGripperCommandBackend(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel,
    std::optional<RetainedGripperPositionLimits> position_limits,
    core::IClock& clock,
    core::IScheduler& scheduler, const core::Duration update_period_ns) {
  if (!state_channel || update_period_ns <= 0 ||
      (position_limits.has_value() &&
       (!std::isfinite(position_limits->lower_position) ||
        !std::isfinite(position_limits->upper_position) ||
        position_limits->lower_position > position_limits->upper_position))) {
    throw std::invalid_argument(
        "retained gripper backend requires state, "
        "limits, and a positive period");
  }
  const auto& value = binding.value();
  const bool model_backed =
      value.resources.size() == 1U &&
      value.resources.front().source_joint_name.has_value();
  const bool exact_model_scope =
      model_backed && value.model_groups.size() == 1U &&
      std::count(
          value.model_groups.front().expanded_compiled_joint_names.begin(),
          value.model_groups.front().expanded_compiled_joint_names.end(),
          value.resources.front().compiled_resource_name) == 1 &&
      position_limits.has_value();
  const bool exact_component_local_scope =
      value.resources.size() == 1U && !model_backed &&
      value.model_groups.empty() && !position_limits.has_value();
  if (!value.control_path.has_value() ||
      (!exact_model_scope && !exact_component_local_scope)) {
    throw std::invalid_argument(
        "retained gripper backend requires one exact command coordinate");
  }
  if (position_limits.has_value() &&
      value.resources.front().coordinate_kind !=
          position_limits->coordinate_kind) {
    throw std::invalid_argument(
        "retained gripper limits differ from the bound coordinate kind");
  }
  auto expected_context = core::make_observed_state_query_context(
      binding, state_channel->query_context().freshness_budget_ns);
  if (!expected_context.ok() ||
      expected_context.value() != state_channel->query_context()) {
    throw std::invalid_argument(
        "retained gripper backend state channel crosses its binding");
  }

  state_ = std::make_shared<SharedState>();
  state_->state_channel = std::move(state_channel);
  state_->clock = &clock;
  state_->scheduler = &scheduler;
  state_->update_period_ns = update_period_ns;
  state_->gripper_id = value.command_target_id.value();
  state_->group_id = model_backed
                         ? value.model_groups.front().compiled_group_name
                         : std::string{};
  state_->joint_name = value.resources.front().compiled_resource_name;
  state_->actuation_kind =
      value.resources.front().coordinate_kind == core::CoordinateKind::kAngular
          ? "angular"
          : "linear";
  state_->state_authority = value.control_path->state_authority_id.value();
  state_->position_limits = std::move(position_limits);
}

RetainedGripperCommandBackend::~RetainedGripperCommandBackend() {
  invalidate_gripper_commands(core::GripperInvalidationReason::kShutdown);
  state_.reset();
}

core::Result<void> RetainedGripperCommandBackend::validate_gripper_command(
    const core::GripperCommandRequestDTO& request) const {
  const auto invalid = validate_request(request, *state_);
  if (invalid.has_value()) {
    auto public_error = *invalid;
    public_error.details["reason"] = public_error.code;
    public_error.code =
        std::string(core::to_string(core::ErrorCode::kInvalidRequest));
    return core::Result<void>::Err(std::move(public_error));
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->closing || state_->admission_blocked) {
    return core::Result<void>::Err(gripper_error(
        "gripper_command_not_authorized",
        "Gripper command admission is closed by the execution lifecycle"));
  }
  const auto current = state_->runtimes.find(request.gripper_id);
  if (current != state_->runtimes.end() &&
      current->second.status.state ==
          core::GripperCommandState::kExecuting) {
    return core::Result<void>::Err(gripper_error(
        "gripper_busy", "Gripper is already executing a command"));
  }
  if (current != state_->runtimes.end() &&
      current->second.status.state == core::GripperCommandState::kFailed &&
      !supersedable_grasp_loss(current->second.status.error)) {
    return core::Result<void>::Err(
        current->second.status.error.value_or(gripper_error(
            "gripper_command_failed",
            "Gripper requires recovery before another command")));
  }
  return core::Result<void>::Ok();
}

core::GripperCommandResultDTO RetainedGripperCommandBackend::command_gripper(
    const core::GripperCommandRequestDTO& request) {
  if (const auto invalid = validate_request(request, *state_);
      invalid.has_value()) {
    return {false, 0U, 0U, *invalid};
  }

  std::uint64_t authority_generation = 0U;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || state_->admission_blocked) {
      return {
          false, 0U, 0U,
          gripper_error("gripper_command_not_authorized",
                        "Gripper command admission is closed by the execution "
                        "lifecycle")};
    }
    authority_generation = state_->authority_generation;
    const auto current = state_->runtimes.find(request.gripper_id);
    if (current != state_->runtimes.end()) {
      if (current->second.status.state ==
          core::GripperCommandState::kExecuting) {
        return {false, current->second.status.command_id,
                current->second.status.generation,
                gripper_error("gripper_busy",
                              "Gripper is already executing a command")};
      }
      if (current->second.status.state == core::GripperCommandState::kFailed &&
          !supersedable_grasp_loss(current->second.status.error)) {
        return {false, current->second.status.command_id,
                current->second.status.generation,
                current->second.status.error};
      }
    }
  }

  auto controlled = state_->state_channel->current_controlled_state();
  const bool establish_component_local_state =
      !controlled.ok() && !state_->position_limits.has_value() &&
      request.duration_s == 0.0;
  if ((!controlled.ok() && !establish_component_local_state) ||
      (controlled.ok() && controlled.value().positions.size() != 1U)) {
    return {false, 0U, 0U,
            controlled.ok()
                ? std::optional<core::ErrorDTO>(gripper_error(
                      "gripper_state_unavailable",
                      "Native gripper start position has invalid shape"))
                : std::optional<core::ErrorDTO>(controlled.error())};
  }
  const double start_position = establish_component_local_state
                                    ? request.target_position
                                    : position_value(
                                          controlled.value().positions.front());
  if (!std::isfinite(start_position)) {
    return {false, 0U, 0U,
            gripper_error("gripper_state_not_finite",
                          "Native gripper start position is not finite")};
  }

  std::uint64_t command_id = 0U;
  std::uint64_t generation = 0U;
  core::TimePoint first_deadline = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || state_->admission_blocked ||
        state_->authority_generation != authority_generation) {
      return {false, 0U, 0U,
              gripper_error(
                  state_->closing ? "gripper_backend_closed"
                                  : "gripper_command_not_authorized",
                  "Gripper lifecycle authority changed during admission")};
    }
    const auto current = state_->runtimes.find(request.gripper_id);
    if (current != state_->runtimes.end() &&
        current->second.status.state == core::GripperCommandState::kExecuting) {
      return {false, current->second.status.command_id,
              current->second.status.generation,
              gripper_error("gripper_busy",
                            "Gripper is already executing a command")};
    }
    if (current != state_->runtimes.end() &&
        current->second.status.state == core::GripperCommandState::kFailed &&
        !supersedable_grasp_loss(current->second.status.error)) {
      return {false, current->second.status.command_id,
              current->second.status.generation, current->second.status.error};
    }

    command_id = state_->next_command_id++;
    generation = state_->next_generation++;
    auto& runtime = state_->runtimes[request.gripper_id];
    runtime = SharedState::Runtime{};
    runtime.start_position = start_position;
    runtime.started_at_ns = state_->clock->now();
    runtime.deadline_ns = saturating_deadline(runtime.started_at_ns,
                                              duration_ns(request.duration_s));
    first_deadline =
        request.duration_s <= 0.0
            ? runtime.started_at_ns
            : std::min(runtime.deadline_ns,
                       runtime.started_at_ns + state_->update_period_ns);

    auto& status = runtime.status;
    status.state = core::GripperCommandState::kExecuting;
    status.gripper_id = request.gripper_id;
    status.command_id = command_id;
    status.generation = generation;
    status.revision = 1U;
    status.current_position = start_position;
    status.target_position = request.target_position;
    status.commanded_position = request.target_position;
    status.observed_position = start_position;
    status.commanded_state = request.commanded_state;
    status.actuation_kind = request.actuation_kind;
    status.state_authority = request.state_authority;
    status.completion_policy = request.completion_policy;
    status.grasp_confirmation_policy = request.grasp_confirmation_policy;
    status.transport_kind = request.transport_kind;
    status.evidence = request.evidence;
    status.evidence_state = request.evidence_state;
    status.projection_state = core::GripperProjectionState::kPending;
  }

  if (!schedule_tick(state_, request.gripper_id, command_id, generation,
                     first_deadline)) {
    const auto status = get_gripper_status(request.gripper_id);
    return {false, command_id, generation,
            status.command_id == command_id &&
                    status.generation == generation && status.error.has_value()
                ? status.error
                : std::optional<core::ErrorDTO>(gripper_error(
                      "gripper_command_not_authorized",
                      "Gripper authority changed before scheduling"))};
  }
  return {true, command_id, generation, std::nullopt};
}

core::GripperCommandStatusDTO RetainedGripperCommandBackend::get_gripper_status(
    const std::string& gripper_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->runtimes.find(gripper_id);
  if (found == state_->runtimes.end()) {
    core::GripperCommandStatusDTO status;
    status.gripper_id = gripper_id;
    return status;
  }
  return found->second.status;
}

core::GripperCommandResultDTO
RetainedGripperCommandBackend::mark_gripper_grasp_lost(
    const std::string& gripper_id, const std::uint64_t expected_command_id,
    const std::uint64_t expected_generation, const std::string& evidence_label,
    const std::string& message, const std::string& reason_code) {
  std::optional<core::ScheduleHandle> handle;
  std::uint64_t command_id = 0U;
  std::uint64_t generation = 0U;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->runtimes.find(gripper_id);
    if (gripper_id != state_->gripper_id || found == state_->runtimes.end() ||
        found->second.status.command_id == 0U ||
        found->second.status.command_id != expected_command_id ||
        found->second.status.generation != expected_generation) {
      return {false, 0U, 0U,
              gripper_error(
                  "stale_gripper_state",
                  "Gripper identity changed before grasp-loss evidence was "
                  "applied")};
    }
    auto& runtime = found->second;
    handle = runtime.scheduled_handle;
    runtime.scheduled_handle.reset();
    ++runtime.schedule_generation;
    runtime.status.generation = state_->next_generation++;
    generation = runtime.status.generation;
    command_id = runtime.status.command_id;
    runtime.status.state = core::GripperCommandState::kFailed;
    runtime.status.projection_state =
        core::GripperProjectionState::kNotRequired;
    runtime.status.projection_reason.clear();
    runtime.status.evidence_state = "pressure_or_part_lost";
    if (!evidence_label.empty() &&
        std::find(runtime.status.evidence.begin(),
                  runtime.status.evidence.end(),
                  evidence_label) == runtime.status.evidence.end()) {
      runtime.status.evidence.push_back(evidence_label);
    }
    runtime.status.error = gripper_error(
        reason_code.empty() ? "grasp_confirmation_lost" : reason_code,
        message.empty() ? "Gripper grasp confirmation was lost" : message);
    ++runtime.status.revision;
  }
  if (handle.has_value()) {
    try {
      state_->scheduler->cancel(*handle);
    } catch (...) {
      // Generation fencing already made the callback inert.
    }
  }
  return {true, command_id, generation, std::nullopt};
}

void RetainedGripperCommandBackend::invalidate_gripper_commands(
    const core::GripperInvalidationReason reason) noexcept {
  if (!state_) {
    return;
  }
  std::vector<core::ScheduleHandle> handles;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->admission_blocked = true;
    ++state_->authority_generation;
    if (reason == core::GripperInvalidationReason::kShutdown ||
        reason == core::GripperInvalidationReason::kDeploymentSwap) {
      state_->closing = true;
    }
    for (auto& [unused, runtime] : state_->runtimes) {
      static_cast<void>(unused);
      if (runtime.scheduled_handle.has_value()) {
        handles.push_back(*runtime.scheduled_handle);
        runtime.scheduled_handle.reset();
      }
      ++runtime.schedule_generation;
      runtime.status.generation = state_->next_generation++;
      runtime.status.state = core::GripperCommandState::kIdle;
      runtime.status.progress = 0.0;
      runtime.status.error.reset();
      runtime.status.projection_state =
          core::GripperProjectionState::kNotRequired;
      runtime.status.projection_reason.clear();
      ++runtime.status.revision;
    }
  }
  for (const auto handle : handles) {
    try {
      state_->scheduler->cancel(handle);
    } catch (...) {
      // Lifecycle invalidation is authoritative; callbacks are fenced.
    }
  }
}

void RetainedGripperCommandBackend::authorize_gripper_commands() noexcept {
  if (!state_) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->closing || !state_->admission_blocked) {
    return;
  }
  ++state_->authority_generation;
  state_->admission_blocked = false;
}

}  // namespace manyforge::roboplan_backend
