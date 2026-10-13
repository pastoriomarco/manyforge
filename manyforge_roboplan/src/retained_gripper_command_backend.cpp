#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <manyforge_core/execution_stop_support.hpp>
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

[[nodiscard]] bool
supersedable_grasp_loss(const std::optional<core::ErrorDTO> &error) noexcept {
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

[[nodiscard]] core::TimePoint
saturating_deadline(const core::TimePoint started_at,
                    const core::Duration duration) {
  const auto maximum = std::numeric_limits<core::TimePoint>::max();
  if (duration > 0 && started_at > maximum - duration) {
    return maximum;
  }
  return started_at + duration;
}

[[nodiscard]] double
position_value(const core::CoordinatePositionDTO &position) noexcept {
  if (const auto *angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

[[nodiscard]] std::string
projection_reason(const RetainedStateProjectionResult &result) {
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

} // namespace

struct RetainedGripperCommandBackend::SharedState {
  struct Runtime final {
    core::GripperCommandStatusDTO status;
    double start_position{};
    core::TimePoint started_at_ns{};
    core::TimePoint deadline_ns{};
    std::optional<core::ScheduleHandle> scheduled_handle;
    std::uint64_t schedule_generation{};
    int terminal_projection_attempts{};
    std::optional<core::AddressedGripperCommandIdentityDTO> addressed_identity;
  };

  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  std::optional<core::RuntimeParticipantBinding> binding;
  core::IClock *clock{};
  core::IScheduler *scheduler{};
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

[[nodiscard]] bool schedule_tick(const std::shared_ptr<SharedState> &state,
                                 const std::string &gripper_id,
                                 std::uint64_t command_id,
                                 std::uint64_t generation,
                                 core::TimePoint deadline_ns);

void update_projection_result_locked(
    SharedState::Runtime &runtime, const RetainedStateProjectionResult &result,
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

void run_tick(const std::shared_ptr<SharedState> &state,
              const std::string &gripper_id, const std::uint64_t command_id,
              const std::uint64_t generation,
              const std::uint64_t schedule_generation,
              const core::TimePoint scheduled_deadline_ns,
              const std::shared_ptr<ScheduleAttempt> &attempt) {
  double position = 0.0;
  bool terminal = false;
  bool idle_heartbeat = false;
  core::TimePoint next_deadline = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->runtimes.find(gripper_id);
    if (state->closing || found == state->runtimes.end()) {
      return;
    }
    auto &runtime = found->second;
    if (runtime.status.command_id != command_id ||
        runtime.status.generation != generation ||
        runtime.schedule_generation != schedule_generation) {
      return;
    }
    attempt->callback_admitted.store(true, std::memory_order_release);
    runtime.scheduled_handle.reset();
    if (runtime.status.state == core::GripperCommandState::kFailed) {
      return;
    }

    const auto now_ns = state->clock->now();
    if (runtime.status.state == core::GripperCommandState::kIdle) {
      // A real state source continues reporting while no command is active.
      // The retained native backend models that same authority so post-Stop
      // surveillance and recovery never depend on a one-shot Reset sample.
      // An IScheduler callback is contractually not early; fail closed rather
      // than recurse if a test adapter violates that contract.
      if (now_ns < scheduled_deadline_ns ||
          !runtime.status.current_position.has_value() ||
          !std::isfinite(*runtime.status.current_position)) {
        return;
      }
      idle_heartbeat = true;
      position = *runtime.status.current_position;
      next_deadline = saturating_deadline(now_ns, state->update_period_ns);
    } else {
      if (runtime.status.state != core::GripperCommandState::kExecuting ||
          !runtime.status.target_position.has_value()) {
        return;
      }
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
      next_deadline =
          terminal
              ? saturating_deadline(now_ns, state->update_period_ns)
              : std::min(runtime.deadline_ns,
                         saturating_deadline(now_ns, state->update_period_ns));
    }
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
      projection = idle_heartbeat
                       ? state->state_channel->publish_state(
                             positions, velocities, now_ns, now_ns)
                       : state->state_channel->publish_and_project(
                             positions, velocities, now_ns, now_ns,
                             RetainedStateProjectionMode::kBestEffort);
    } catch (const std::exception &error) {
      projection = {
          RetainedStateProjectionStatus::kFailed,
          gripper_error("gripper_projection_exception", error.what())};
    } catch (...) {
      projection = {RetainedStateProjectionStatus::kFailed,
                    gripper_error("gripper_projection_exception",
                                  "Native gripper state projection threw")};
    }
    if (idle_heartbeat) {
      auto &runtime = found->second;
      if (projection.applied()) {
        runtime.terminal_projection_attempts = 0;
        schedule_again =
            runtime.status.state == core::GripperCommandState::kIdle;
      } else if (projection.status == RetainedStateProjectionStatus::kBusy &&
                 ++runtime.terminal_projection_attempts <
                     kTerminalProjectionRetryLimit) {
        schedule_again =
            runtime.status.state == core::GripperCommandState::kIdle;
      } else {
        state->admission_blocked = true;
        runtime.status.state = core::GripperCommandState::kFailed;
        runtime.status.projection_state = core::GripperProjectionState::kFailed;
        runtime.status.projection_reason = projection_reason(projection);
        runtime.status.error = gripper_error(
            "gripper_state_heartbeat_failed",
            "Native gripper idle state publication could not be maintained");
        ++runtime.status.revision;
      }
    } else {
      update_projection_result_locked(found->second, projection, terminal);
      schedule_again =
          found->second.status.state == core::GripperCommandState::kExecuting ||
          (found->second.status.state ==
               core::GripperCommandState::kCompleted &&
           found->second.status.projection_state ==
               core::GripperProjectionState::kPending);
    }
  }
  if (schedule_again) {
    (void)schedule_tick(state, gripper_id, command_id, generation,
                        next_deadline);
  }
}

bool schedule_tick(const std::shared_ptr<SharedState> &state,
                   const std::string &gripper_id,
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
         schedule_generation, deadline_ns]() {
          if (const auto locked = weak_state.lock()) {
            run_tick(locked, gripper_id, command_id, generation,
                     schedule_generation, deadline_ns, attempt);
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

[[nodiscard]] bool exact_request(const core::GripperCommandRequestDTO &request,
                                 const SharedState &state) noexcept {
  return request.gripper_id == state.gripper_id &&
         request.group_id == state.group_id &&
         request.joint_name == state.joint_name &&
         request.actuation_kind == state.actuation_kind &&
         request.state_authority == state.state_authority &&
         request.commanded_state == "position" &&
         request.transport_kind == "resolved_binding";
}

[[nodiscard]] std::optional<core::ErrorDTO>
validate_request(const core::GripperCommandRequestDTO &request,
                 const SharedState &state) {
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

} // namespace

RetainedGripperCommandBackend::RetainedGripperCommandBackend(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel,
    std::optional<RetainedGripperPositionLimits> position_limits,
    core::IClock &clock, core::IScheduler &scheduler,
    const core::Duration update_period_ns) {
  if (!state_channel || update_period_ns <= 0 ||
      (position_limits.has_value() &&
       (!std::isfinite(position_limits->lower_position) ||
        !std::isfinite(position_limits->upper_position) ||
        position_limits->lower_position > position_limits->upper_position))) {
    throw std::invalid_argument("retained gripper backend requires state, "
                                "limits, and a positive period");
  }
  const auto &value = binding.value();
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
  if (position_limits.has_value() && value.resources.front().coordinate_kind !=
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
  state_->binding = std::move(binding);
}

RetainedGripperCommandBackend::~RetainedGripperCommandBackend() {
  invalidate_gripper_commands(core::GripperInvalidationReason::kShutdown);
  state_.reset();
}

core::Result<void> RetainedGripperCommandBackend::RebindRoute(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel) {
  if (!state_channel) {
    return core::Result<void>::Err(
        gripper_error("gripper_route_rebind_state_missing",
                      "Native gripper route publication has no state channel"));
  }
  auto expected = core::make_observed_state_query_context(
      binding, state_channel->query_context().freshness_budget_ns);
  if (!expected.ok() || expected.value() != state_channel->query_context()) {
    return core::Result<void>::Err(gripper_error(
        "gripper_route_rebind_mismatch",
        "Native gripper route publication crosses its retained authority"));
  }
  try {
    std::lock_guard lock(state_->mutex);
    if (state_->closing ||
        std::any_of(state_->runtimes.begin(), state_->runtimes.end(),
                    [](const auto &row) {
                      return row.second.status.state ==
                             core::GripperCommandState::kExecuting;
                    })) {
      return core::Result<void>::Err(gripper_error(
          "gripper_route_rebind_busy",
          "Native gripper route publication cannot replace active state"));
    }
    if (state_->binding->value() != binding.value()) {
      for (auto& [unused, runtime] : state_->runtimes) {
        static_cast<void>(unused);
        runtime.status.grasp_evidence.reset();
        ++runtime.status.revision;
      }
    }
    state_->binding = std::move(binding);
    state_->state_channel = std::move(state_channel);
    return core::Result<void>::Ok();
  } catch (...) {
    return core::Result<void>::Err(gripper_error(
        "gripper_route_rebind_capacity_unavailable",
        "Native gripper route publication could not retain its binding"));
  }
}

core::Result<void> RetainedGripperCommandBackend::validate_gripper_command(
    const core::GripperCommandRequestDTO &request) const {
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
      current->second.status.state == core::GripperCommandState::kExecuting) {
    return core::Result<void>::Err(gripper_error(
        "gripper_busy", "Gripper is already executing a command"));
  }
  if (current != state_->runtimes.end() &&
      current->second.status.state == core::GripperCommandState::kFailed &&
      !supersedable_grasp_loss(current->second.status.error)) {
    return core::Result<void>::Err(current->second.status.error.value_or(
        gripper_error("gripper_command_failed",
                      "Gripper requires recovery before another command")));
  }
  return core::Result<void>::Ok();
}

core::GripperCommandResultDTO RetainedGripperCommandBackend::command_gripper(
    const core::GripperCommandRequestDTO &request) {
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
  const double start_position =
      establish_component_local_state
          ? request.target_position
          : position_value(controlled.value().positions.front());
  if (!std::isfinite(start_position)) {
    return {false, 0U, 0U,
            gripper_error("gripper_state_not_finite",
                          "Native gripper start position is not finite")};
  }

  std::uint64_t command_id = 0U;
  std::uint64_t generation = 0U;
  core::TimePoint first_deadline = 0;
  std::optional<core::ScheduleHandle> superseded_handle;
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
    auto &runtime = state_->runtimes[request.gripper_id];
    superseded_handle = runtime.scheduled_handle;
    runtime = SharedState::Runtime{};
    runtime.start_position = start_position;
    runtime.started_at_ns = state_->clock->now();
    runtime.deadline_ns = saturating_deadline(runtime.started_at_ns,
                                              duration_ns(request.duration_s));
    runtime.addressed_identity = request.addressed_identity;
    first_deadline =
        request.duration_s <= 0.0
            ? runtime.started_at_ns
            : std::min(runtime.deadline_ns,
                       runtime.started_at_ns + state_->update_period_ns);

    auto &status = runtime.status;
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

  if (superseded_handle.has_value()) {
    try {
      state_->scheduler->cancel(*superseded_handle);
    } catch (...) {
      // The command and generation replacement already fence the callback.
    }
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
    const std::string &gripper_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->runtimes.find(gripper_id);
  if (found == state_->runtimes.end()) {
    core::GripperCommandStatusDTO status;
    status.gripper_id = gripper_id;
    return status;
  }
  return found->second.status;
}

void RetainedGripperCommandBackend::invalidate_grasp_evidence() noexcept {
  std::lock_guard lock(state_->mutex);
  for (auto& [unused, runtime] : state_->runtimes) {
    static_cast<void>(unused);
    runtime.status.grasp_evidence.reset();
    // Also retire an attestation prepared before the source was lost but
    // not yet recorded. Reconciliation cannot make that old CAS current.
    ++runtime.status.revision;
  }
}

core::GripperCommandResultDTO
RetainedGripperCommandBackend::mark_gripper_grasp_lost(
    const std::string &gripper_id, const std::uint64_t expected_command_id,
    const std::uint64_t expected_generation, const std::string &evidence_label,
    const std::string &message, const std::string &reason_code) {
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
    auto &runtime = found->second;
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
    runtime.status.grasp_evidence.reset();
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
  struct HeartbeatTarget final {
    std::string gripper_id;
    std::uint64_t command_id{};
    std::uint64_t generation{};
    core::TimePoint first_deadline_ns{};
  };
  std::vector<HeartbeatTarget> heartbeat_targets;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->admission_blocked = true;
    ++state_->authority_generation;
    if (reason == core::GripperInvalidationReason::kShutdown ||
        reason == core::GripperInvalidationReason::kDeploymentSwap) {
      state_->closing = true;
    }
    const auto keep_observing = !state_->closing;
    const auto first_deadline_ns =
        saturating_deadline(state_->clock->now(), state_->update_period_ns);
    heartbeat_targets.reserve(keep_observing ? state_->runtimes.size() : 0U);
    for (auto &[gripper_id, runtime] : state_->runtimes) {
      if (runtime.scheduled_handle.has_value()) {
        handles.push_back(*runtime.scheduled_handle);
        runtime.scheduled_handle.reset();
      }
      ++runtime.schedule_generation;
      runtime.status.generation = state_->next_generation++;
      runtime.status.state = core::GripperCommandState::kIdle;
      runtime.status.progress = 0.0;
      runtime.status.error.reset();
      runtime.status.grasp_evidence.reset();
      runtime.status.projection_state =
          core::GripperProjectionState::kNotRequired;
      runtime.status.projection_reason.clear();
      ++runtime.status.revision;
      if (keep_observing && runtime.status.current_position.has_value()) {
        heartbeat_targets.push_back(
            HeartbeatTarget{gripper_id, runtime.status.command_id,
                            runtime.status.generation, first_deadline_ns});
      }
    }
  }
  for (const auto handle : handles) {
    try {
      state_->scheduler->cancel(handle);
    } catch (...) {
      // Lifecycle invalidation is authoritative; callbacks are fenced.
    }
  }
  // Stop/reset closes command admission, not observed-state publication.
  // Keeping this source fresh lets recovery verify rest without authorizing a
  // command or depending on a one-shot Stop sample.
  for (const auto &target : heartbeat_targets) {
    (void)schedule_tick(state_, target.gripper_id, target.command_id,
                        target.generation, target.first_deadline_ns);
  }
}

void RetainedGripperCommandBackend::authorize_gripper_commands() noexcept {
  if (!state_) {
    return;
  }
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  std::vector<double> positions;
  std::uint64_t authority_generation = 0U;
  struct HeartbeatTarget final {
    std::string gripper_id;
    std::uint64_t command_id{};
    std::uint64_t generation{};
    core::TimePoint first_deadline_ns{};
  };
  std::vector<HeartbeatTarget> heartbeat_targets;
  bool state_available = true;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || !state_->admission_blocked) {
      return;
    }
    state_channel = state_->state_channel;
    authority_generation = state_->authority_generation;
    positions.reserve(state_->runtimes.size());
    for (const auto &[unused, runtime] : state_->runtimes) {
      static_cast<void>(unused);
      if (!runtime.status.current_position.has_value()) {
        state_available = false;
        break;
      }
      positions.push_back(*runtime.status.current_position);
    }
  }

  bool state_refreshed = state_available && positions.empty();
  if (state_available && !state_refreshed && state_channel != nullptr) {
    try {
      std::vector<double> velocities(positions.size(), 0.0);
      const auto observed_at_ns = state_->clock->now();
      state_refreshed = state_channel
                            ->publish_state(positions, velocities,
                                            observed_at_ns, observed_at_ns)
                            .applied();
    } catch (...) {
      state_refreshed = false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || !state_->admission_blocked ||
        state_->authority_generation != authority_generation ||
        !state_refreshed) {
      return;
    }
    heartbeat_targets.reserve(state_->runtimes.size());
    const auto first_deadline_ns =
        saturating_deadline(state_->clock->now(), state_->update_period_ns);
    for (const auto &[gripper_id, runtime] : state_->runtimes) {
      if (runtime.status.state != core::GripperCommandState::kIdle ||
          !runtime.status.current_position.has_value()) {
        return;
      }
      heartbeat_targets.push_back(
          HeartbeatTarget{gripper_id, runtime.status.command_id,
                          runtime.status.generation, first_deadline_ns});
    }
  }

  for (const auto &target : heartbeat_targets) {
    if (!schedule_tick(state_, target.gripper_id, target.command_id,
                       target.generation, target.first_deadline_ns)) {
      return;
    }
  }

  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->closing || !state_->admission_blocked ||
      state_->authority_generation != authority_generation) {
    return;
  }
  for (const auto &target : heartbeat_targets) {
    const auto found = state_->runtimes.find(target.gripper_id);
    if (found == state_->runtimes.end() ||
        found->second.status.command_id != target.command_id ||
        found->second.status.generation != target.generation ||
        !found->second.scheduled_handle.has_value()) {
      return;
    }
  }
  ++state_->authority_generation;
  state_->admission_blocked = false;
}

core::ParticipantStopStageResultDTO
RetainedGripperCommandBackend::TranslateStopStage(
    const core::ResolvedParticipantStopStageDTO &stage,
    const core::RemainingStopBudgetDTO &budget,
    const std::stop_token cancellation) {
  const auto started_at = state_->clock->now();
  const auto failure =
      [&](std::string reason, std::string message,
          const core::StopStageAttemptDispositionDTO disposition =
              core::StopStageAttemptDispositionDTO::kNotDispatched) {
        const core::ErrorDTO error{core::kErrorMotionInterruptBackendStopFailed,
                                   std::move(message),
                                   {{"reason", std::move(reason)}}};
        core::ParticipantStopStageResultDTO result{
            stage,
            disposition,
            core::ParticipantStopStageOutcomeDTO::kFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::max<std::int64_t>(0, state_->clock->now() - started_at),
            {},
            error};
        if (stage.stage_index <
            stage.participant_stop_plan.stage_commands.size()) {
          const auto &requirements =
              stage.participant_stop_plan.stage_commands[stage.stage_index]
                  .resolved_stage.evidence_requirements;
          result.evidence_items.reserve(requirements.size());
          for (const auto &requirement : requirements) {
            result.evidence_items.push_back(
                {requirement.requirement.evidence_item_id,
                 requirement.requirement.evidence_kind,
                 core::StopEvidenceItemStateDTO::kMissing,
                 requirement.resolved_source, std::nullopt, std::nullopt,
                 std::nullopt, std::nullopt, error});
          }
        }
        return result;
      };
  const auto superseded = [&] {
    auto result = failure("stop_stage_superseded_before_dispatch",
                          "Stop stage was cancelled before dispatch");
    result.outcome = core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect;
    result.error = core::ErrorDTO{"stop_stage_superseded_before_dispatch",
                                  "Stop stage was cancelled before dispatch",
                                  {}};
    for (auto &item : result.evidence_items) {
      item.state = core::StopEvidenceItemStateDTO::kInapplicable;
      item.error = result.error;
    }
    return result;
  };
  if (cancellation.stop_requested()) {
    return superseded();
  }
  if (!state_->binding.has_value() ||
      stage.stage_index >= stage.participant_stop_plan.stage_commands.size() ||
      budget.stop_id != stage.stop_id ||
      budget.stop_generation != stage.stop_generation ||
      budget.elapsed_since_stop_ns < 0 ||
      budget.remaining_controlled_deadline_ns.has_value()) {
    return failure("resolved_stop_stage_budget_invalid",
                   "native gripper Stop has no valid per-robot Stop timing");
  }

  const auto &binding = state_->binding->value();
  const auto &path = *binding.control_path;
  const auto &command =
      stage.participant_stop_plan.stage_commands[stage.stage_index]
          .resolved_stage;
  std::vector<core::ResourceId> exact_resources;
  exact_resources.reserve(binding.resources.size());
  for (const auto &resource : binding.resources) {
    exact_resources.push_back(resource.resource_id);
  }
  std::sort(exact_resources.begin(), exact_resources.end());
  if (stage.resources != exact_resources) {
    return failure("resolved_stop_stage_resource_mismatch",
                   "native gripper Stop crosses its exact resource scope");
  }
  if (stage.control_domain_id != path.control_domain_id ||
      stage.control_domain_revision != path.control_domain_revision ||
      stage.control_binding_id != path.control_binding_id ||
      stage.control_binding_revision != path.control_binding_revision ||
      stage.control_route_id != path.control_route_id ||
      stage.active_control_configuration_id !=
          path.active_control_configuration_id ||
      stage.active_control_configuration_generation !=
          path.active_control_configuration_generation) {
    return failure("resolved_stop_stage_control_path_mismatch",
                   "native gripper Stop crosses its retained control path");
  }
  if (command.command_target.provider_instance_id !=
          path.provider_instance_id ||
      command.command_target.provider_manifest_id !=
          path.provider_manifest_id ||
      command.command_target.provider_manifest_revision !=
          path.provider_manifest_revision ||
      command.command_target.runtime_unit_id != path.provider_runtime_unit_id ||
      command.command_target.provider_runtime_epoch !=
          path.provider_runtime_epoch) {
    return failure("resolved_stop_stage_provider_occurrence_mismatch",
                   "native gripper Stop names another provider occurrence");
  }
  // Q58 (as `main`): the gripper's Stop is a fire-and-forget cancel with no
  // Stop time limit of its own; the bound below only keeps this local state
  // read/publication from blocking (`main`'s bounded native drain).
  const auto stage_deadline_ns = saturating_deadline(
      started_at,
      static_cast<std::int64_t>(core::kExecutionDrainTimeoutS * 1e9));
  const bool controller_goal_cancel =
      command.containment_command.command_kind ==
      core::StopContainmentCommandKind::kControllerGoalCancel;
  const bool software_hard =
      command.containment_command.command_kind ==
          core::StopContainmentCommandKind::kRegistered &&
      command.containment_command.registered_command_kind_id.has_value() &&
      command.containment_command.registered_command_kind_id->value() ==
          "manyforge.stop.software_hard";
  if (!controller_goal_cancel && !software_hard) {
    return failure("resolved_stop_stage_command_mismatch",
                   "native gripper Stop does not name goal cancel or its "
                   "registered software-hard command");
  }
  std::optional<core::AddressedGripperCommandIdentityDTO> addressed_identity;
  std::optional<core::GripperCommandStatusDTO> addressed_status;
  if (controller_goal_cancel) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      const auto current =
          state_->runtimes.find(stage.command_target_id.value());
      if (current != state_->runtimes.end()) {
        addressed_identity = current->second.addressed_identity;
        addressed_status = current->second.status;
      }
    }
    const bool exact_goal =
        addressed_identity.has_value() && addressed_status.has_value() &&
        addressed_identity->operation_id == stage.operation_id &&
        addressed_identity->operation_revision == stage.operation_revision &&
        addressed_identity->participant_id == stage.participant_id &&
        addressed_identity->controller_goal_id == stage.controller_goal_id &&
        addressed_identity->controller_goal_generation ==
            stage.controller_goal_generation &&
        addressed_status->state != core::GripperCommandState::kFailed &&
        addressed_status->command_id != 0U &&
        addressed_status->generation != 0U && addressed_status->revision != 0U;
    if (!exact_goal) {
      return failure(
          "controller_goal_dispatch_membership_unavailable",
          "native gripper Stop cannot prove exact prepared-goal membership");
    }
  }

  core::ParticipantStopStageResultDTO result{
      stage,
      core::StopStageAttemptDispositionDTO::kDispatched,
      core::ParticipantStopStageOutcomeDTO::kFailed,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::max<std::int64_t>(0, state_->clock->now() - started_at),
      {},
      std::nullopt};
  const auto fail_after_dispatch = [&](std::string reason,
                                       std::string message) {
    result.error = core::ErrorDTO{core::kErrorMotionInterruptBackendStopFailed,
                                  std::move(message),
                                  {{"reason", std::move(reason)}}};
    result.evidence_items.clear();
    result.evidence_items.reserve(command.evidence_requirements.size());
    for (const auto &requirement : command.evidence_requirements) {
      result.evidence_items.push_back({requirement.requirement.evidence_item_id,
                                       requirement.requirement.evidence_kind,
                                       core::StopEvidenceItemStateDTO::kMissing,
                                       requirement.resolved_source,
                                       std::nullopt, std::nullopt, std::nullopt,
                                       std::nullopt, result.error});
    }
    return result;
  };

  if (controller_goal_cancel) {
    invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
    std::optional<core::AddressedGripperCommandIdentityDTO> terminal_identity;
    std::optional<core::GripperCommandStatusDTO> terminal_status;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      const auto current =
          state_->runtimes.find(stage.command_target_id.value());
      if (current != state_->runtimes.end()) {
        terminal_identity = current->second.addressed_identity;
        terminal_status = current->second.status;
      }
    }
    const bool exact_terminal =
        terminal_identity == addressed_identity &&
        terminal_status.has_value() &&
        terminal_status->command_id == addressed_status->command_id &&
        terminal_status->state == core::GripperCommandState::kIdle &&
        terminal_status->generation > addressed_status->generation &&
        terminal_status->revision > addressed_status->revision;
    if (!exact_terminal) {
      return fail_after_dispatch(
          "stop_goal_terminal_state_mismatch",
          "native gripper goal did not reach its exact cancelled terminal");
    }
    result.evidence_items.reserve(command.evidence_requirements.size());
    for (const auto &requirement : command.evidence_requirements) {
      if (requirement.requirement.evidence_kind !=
              core::StopEvidenceRequirementKind::kControllerGoalTerminal ||
          requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kControllerGoalStatus ||
          requirement.resolved_source.runtime_target.provider_runtime_epoch !=
              path.provider_runtime_epoch) {
        return fail_after_dispatch("stop_evidence_source_mismatch",
                                   "native gripper goal-terminal evidence "
                                   "differs from its retained source");
      }
      result.evidence_items.push_back(
          {requirement.requirement.evidence_item_id,
           requirement.requirement.evidence_kind,
           core::StopEvidenceItemStateDTO::kSatisfied,
           requirement.resolved_source, path.provider_runtime_epoch,
           terminal_status->generation, 0, core::JsonValueDTO{true},
           std::nullopt});
    }
    result.controller_acknowledgement_sequence = terminal_status->generation;
    result.source_epoch = path.provider_runtime_epoch;
    result.source_sequence = terminal_status->generation;
    result.outcome = core::ParticipantStopStageOutcomeDTO::kSucceeded;
    result.receiver_elapsed_duration_ns =
        std::max<std::int64_t>(0, state_->clock->now() - started_at);
    return result;
  }

  const auto before_now_ns = state_->clock->now();
  if (before_now_ns >= stage_deadline_ns) {
    return failure("stop_evidence_source_deadline_expired",
                   "native gripper Stop ran out of its bounded state-read "
                   "budget");
  }
  const auto before = state_->state_channel->current_state_bounded(
      before_now_ns, stage_deadline_ns, cancellation);
  if (!before.ok()) {
    if (cancellation.stop_requested()) {
      return superseded();
    }
    return failure("stop_evidence_source_unavailable", before.error().message);
  }
  if (cancellation.stop_requested()) {
    return superseded();
  }
  invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  std::vector<double> positions;
  positions.reserve(before.value().positions.size());
  for (const auto &position : before.value().positions) {
    positions.push_back(std::visit(
        [](const auto &typed) {
          if constexpr (requires { typed.value_rad; }) {
            return typed.value_rad;
          } else {
            return typed.value_m;
          }
        },
        position));
  }
  std::vector<double> velocities(positions.size(), 0.0);
  const auto projection_time = state_->clock->now();
  const auto projected = state_->state_channel->publish_state_bounded(
      positions, velocities, projection_time, projection_time,
      stage_deadline_ns, cancellation);

  if (!projected.applied()) {
    return fail_after_dispatch(
        projection_reason(projected),
        "native gripper rest state could not be published within its "
        "bounded Stop-stage budget");
  }
  if (cancellation.stop_requested()) {
    return fail_after_dispatch(
        "stop_state_projection_cancelled",
        "native gripper Stop publication completed after cancellation");
  }
  const auto observed_now_ns = state_->clock->now();
  if (observed_now_ns >= stage_deadline_ns) {
    return fail_after_dispatch(
        "stop_evidence_source_deadline_expired",
        "native gripper rest evidence missed its bounded Stop-stage budget");
  }
  const auto observed = state_->state_channel->current_state_bounded(
      observed_now_ns, stage_deadline_ns, cancellation);
  if (!observed.ok()) {
    return fail_after_dispatch(cancellation.stop_requested()
                                   ? "stop_evidence_observation_cancelled"
                                   : "stop_evidence_source_unavailable",
                               observed.error().message);
  }
  if (!observed.value().velocities.has_value() ||
      observed.value().resources != exact_resources ||
      observed.value().source_epoch != path.provider_runtime_epoch ||
      observed.value().sequence == 0U ||
      std::any_of(observed.value().velocities->begin(),
                  observed.value().velocities->end(), [](const auto &value) {
                    return std::visit(
                        [](const auto &velocity) {
                          if constexpr (requires { velocity.value_rad_s; }) {
                            return velocity.value_rad_s != 0.0;
                          } else {
                            return velocity.value_m_s != 0.0;
                          }
                        },
                        value);
                  })) {
    return fail_after_dispatch(
        "stop_evidence_not_at_rest",
        "native gripper state does not prove exact rest");
  }
  const auto &snapshot = observed.value();
  result.evidence_items.reserve(command.evidence_requirements.size());
  for (const auto &requirement : command.evidence_requirements) {
    const auto age = std::max<std::int64_t>(
        0, state_->clock->now() - snapshot.received_timestamp_ns);
    if (requirement.requirement.evidence_kind !=
        core::StopEvidenceRequirementKind::kMotionRest) {
      return fail_after_dispatch("stop_evidence_kind_mismatch",
                                 "native gripper stage requires unsupported "
                                 "evidence");
    }
    if (requirement.resolved_source.source_kind !=
        core::StopEvidenceSourceKind::kBindingStateAuthority) {
      return fail_after_dispatch(
          "stop_evidence_source_kind_mismatch",
          "native gripper stage requires another evidence source kind");
    }
    if (requirement.resolved_source.runtime_target.provider_runtime_epoch !=
        snapshot.source_epoch) {
      return fail_after_dispatch(
          "stop_evidence_source_epoch_mismatch",
          "native gripper stage crosses its provider runtime epoch");
    }
    if (requirement.resolved_source.state_authority_id !=
            std::optional(snapshot.state_authority_id) ||
        requirement.resolved_source.state_authority_class !=
            std::optional(snapshot.state_authority_class)) {
      return fail_after_dispatch(
          "stop_evidence_state_authority_mismatch",
          "native gripper stage requires another state authority");
    }
    if (age > requirement.requirement.source_requirement.freshness_budget_ns) {
      return fail_after_dispatch("stop_evidence_source_stale",
                                 "native gripper rest evidence is stale");
    }
    result.evidence_items.push_back(
        {requirement.requirement.evidence_item_id,
         requirement.requirement.evidence_kind,
         core::StopEvidenceItemStateDTO::kSatisfied,
         requirement.resolved_source, snapshot.source_epoch, snapshot.sequence,
         age, core::JsonValueDTO{true}, std::nullopt});
  }
  result.outcome = core::ParticipantStopStageOutcomeDTO::kSucceeded;
  result.source_epoch = snapshot.source_epoch;
  result.source_sequence = snapshot.sequence;
  result.receiver_elapsed_duration_ns =
      std::max<std::int64_t>(0, state_->clock->now() - started_at);
  return result;
}

} // namespace manyforge::roboplan_backend
