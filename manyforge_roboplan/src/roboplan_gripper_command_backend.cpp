#include <manyforge_roboplan/roboplan_gripper_command_backend.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

constexpr int kTerminalProjectionRetryLimit = 10;

core::ErrorDTO gripper_error(std::string reason, std::string message) {
  return core::ErrorDTO{
      std::move(reason), std::move(message), {}};
}

std::optional<double> observed_position_for(
    const std::string& authority,
    std::optional<double> current_position) {
  if (authority == "native_backend" || authority == "sim_observed" ||
      authority == "real_observed") {
    return current_position;
  }
  return std::nullopt;
}

double clamp_progress(double progress) {
  return std::clamp(progress, 0.0, 1.0);
}

bool supersedable_grasp_loss(
    const std::optional<core::ErrorDTO>& error) noexcept {
  return error.has_value() && error->code == "grasp_confirmation_lost";
}

core::Duration duration_ns(double duration_s) {
  constexpr double kNanosPerSecond = 1'000'000'000.0;
  const double nanos = duration_s * kNanosPerSecond;
  if (!std::isfinite(nanos) ||
      nanos >= static_cast<double>(std::numeric_limits<core::Duration>::max())) {
    return std::numeric_limits<core::Duration>::max();
  }
  return static_cast<core::Duration>(nanos);
}

core::TimePoint saturating_deadline(core::TimePoint started_at,
                                    core::Duration duration) {
  const auto maximum = std::numeric_limits<core::TimePoint>::max();
  if (duration > 0 && started_at > maximum - duration) {
    return maximum;
  }
  return started_at + duration;
}

}  // namespace

struct RoboPlanGripperCommandBackend::SharedState {
  struct Runtime {
    core::GripperCommandStatusDTO status;
    std::string group_id;
    std::string joint_name;
    double start_position = 0.0;
    core::TimePoint started_at_ns = 0;
    core::TimePoint deadline_ns = 0;
    std::optional<core::ScheduleHandle> scheduled_handle;
    std::uint64_t schedule_generation = 0;
    int terminal_projection_attempts = 0;
  };

  std::shared_ptr<core::IExecutionStateSink> execution_state_sink;
  core::IClock* clock = nullptr;
  core::IScheduler* scheduler = nullptr;
  core::Duration update_period_ns = 10'000'000;

  mutable std::mutex mutex;
  std::map<std::string, Runtime> runtimes;
  std::uint64_t next_command_id = 1;
  std::uint64_t next_generation = 1;
  std::uint64_t authority_generation = 1;
  bool admission_blocked = false;
  bool closing = false;
};

namespace {

using SharedState = RoboPlanGripperCommandBackend::SharedState;

struct ScheduleAttempt {
  // `schedule()` is allowed to run a due callback before returning its handle.
  // This bit records that the callback passed the command/generation gate, so
  // its admission cannot later be misreported as rejected during handoff.
  std::atomic<bool> callback_admitted{false};
};

bool schedule_native_gripper_tick(
    const std::shared_ptr<SharedState>& state,
    const std::string& gripper_id,
    std::uint64_t command_id,
    std::uint64_t generation,
    core::TimePoint deadline_ns);

void update_projection_result_locked(
    SharedState::Runtime& runtime,
    core::ExecutionStateSinkResult result,
    bool terminal) {
  if (result.applied()) {
    runtime.status.projection_state = core::GripperProjectionState::kApplied;
    runtime.status.projection_reason.clear();
    runtime.terminal_projection_attempts = 0;
    if (terminal) {
      runtime.status.state = core::GripperCommandState::kCompleted;
      runtime.status.progress = 1.0;
    }
  } else if (result.status == core::ExecutionStateSinkStatus::kBusy) {
    runtime.status.projection_state = core::GripperProjectionState::kPending;
    runtime.status.projection_reason =
        std::string(core::execution_state_sink_status_reason(result.status));
    if (terminal) {
      ++runtime.terminal_projection_attempts;
      if (runtime.terminal_projection_attempts >=
          kTerminalProjectionRetryLimit) {
        runtime.status.state = core::GripperCommandState::kFailed;
        runtime.status.projection_state = core::GripperProjectionState::kFailed;
        runtime.status.error = gripper_error(
            "gripper_projection_failed",
            "Timed out publishing the terminal native gripper position");
      }
    }
  } else {
    runtime.status.state = core::GripperCommandState::kFailed;
    runtime.status.projection_state = core::GripperProjectionState::kFailed;
    runtime.status.projection_reason =
        std::string(core::execution_state_sink_status_reason(result.status));
    runtime.status.error = gripper_error(
        "gripper_projection_failed",
        "Native gripper position could not be published to the execution-state sink");
  }
  ++runtime.status.revision;
}

void run_native_gripper_tick(
    const std::shared_ptr<SharedState>& state,
    const std::string& gripper_id,
    std::uint64_t command_id,
    std::uint64_t generation,
    std::uint64_t schedule_generation,
    const std::shared_ptr<ScheduleAttempt>& attempt) {
  std::string group_id;
  std::string joint_name;
  double position = 0.0;
  bool terminal = false;
  core::TimePoint next_deadline = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->runtimes.find(gripper_id);
    if (state->closing || it == state->runtimes.end()) {
      return;
    }
    auto& runtime = it->second;
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
    const double progress = runtime.deadline_ns <= runtime.started_at_ns
                                ? 1.0
                                : clamp_progress(
                                      static_cast<double>(elapsed_ns) /
                                      static_cast<double>(total_ns));
    position = runtime.start_position +
               (*runtime.status.target_position - runtime.start_position) *
                   progress;
    runtime.status.progress = progress;
    runtime.status.current_position = position;
    runtime.status.observed_position = observed_position_for(
        runtime.status.state_authority, runtime.status.current_position);
    terminal = progress >= 1.0;
    ++runtime.status.revision;
    group_id = runtime.group_id;
    joint_name = runtime.joint_name;
    next_deadline = terminal
                        ? now_ns + state->update_period_ns
                        : std::min(runtime.deadline_ns,
                                   now_ns + state->update_period_ns);
  }

  bool needs_another_tick = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->runtimes.find(gripper_id);
    if (state->closing || it == state->runtimes.end() ||
        it->second.status.command_id != command_id ||
        it->second.status.generation != generation ||
        it->second.schedule_generation != schedule_generation) {
      return;
    }
    // The sink call is best-effort and never waits for its scene mutex. Holding
    // the gripper authority lock linearizes reset/replacement against the
    // external projection effect, closing the check-to-write generation race.
    core::ExecutionStateSinkResult projection;
    try {
      projection = state->execution_state_sink->apply_group_joint_position(
          group_id, joint_name, position,
          core::ExecutionStateUpdateMode::kBestEffort);
    } catch (...) {
      projection.status = core::ExecutionStateSinkStatus::kException;
    }
    update_projection_result_locked(it->second, projection, terminal);
    needs_another_tick =
        it->second.status.state == core::GripperCommandState::kExecuting ||
        (it->second.status.state == core::GripperCommandState::kCompleted &&
         it->second.status.projection_state ==
             core::GripperProjectionState::kPending);
  }
  if (needs_another_tick) {
    (void)schedule_native_gripper_tick(
        state, gripper_id, command_id, generation, next_deadline);
  }
}

bool schedule_native_gripper_tick(
    const std::shared_ptr<SharedState>& state,
    const std::string& gripper_id,
    std::uint64_t command_id,
    std::uint64_t generation,
    core::TimePoint deadline_ns) {
  std::optional<core::ScheduleHandle> old_handle;
  std::uint64_t schedule_generation = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->runtimes.find(gripper_id);
    if (state->closing || it == state->runtimes.end() ||
        it->second.status.command_id != command_id ||
        it->second.status.generation != generation) {
      return false;
    }
    old_handle = it->second.scheduled_handle;
    it->second.scheduled_handle.reset();
    schedule_generation = ++it->second.schedule_generation;
  }
  if (old_handle.has_value()) {
    try {
      state->scheduler->cancel(*old_handle);
    } catch (...) {
      // The generation increment above already made the old callback inert.
    }
  }
  const std::weak_ptr<SharedState> weak_state = state;
  const auto attempt = std::make_shared<ScheduleAttempt>();
  core::ScheduleHandle handle;
  try {
    handle = state->scheduler->schedule(
        deadline_ns,
        [weak_state, attempt, gripper_id, command_id, generation,
         schedule_generation]() {
          if (const auto locked = weak_state.lock()) {
            run_native_gripper_tick(
                locked, gripper_id, command_id, generation,
                schedule_generation, attempt);
          }
        },
        core::SchedulerPriority::ControllerCommand);
  } catch (...) {
    if (attempt->callback_admitted.load(std::memory_order_acquire)) {
      // A scheduler may execute an immediately-due callback before surfacing a
      // later registration error. The command already crossed its generation
      // gate, so reporting a side-effect-free rejection would be untruthful.
      return true;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->runtimes.find(gripper_id);
    if (!state->closing && it != state->runtimes.end() &&
        it->second.status.command_id == command_id &&
        it->second.status.generation == generation &&
        it->second.schedule_generation == schedule_generation) {
      it->second.status.state = core::GripperCommandState::kFailed;
      it->second.status.projection_state =
          core::GripperProjectionState::kNotRequired;
      it->second.status.projection_reason.clear();
      it->second.status.error = gripper_error(
          "gripper_request_failed",
          "Native gripper lifecycle could not be scheduled");
      ++it->second.status.revision;
    }
    return false;
  }

  bool admitted = attempt->callback_admitted.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->runtimes.find(gripper_id);
    if (!state->closing && it != state->runtimes.end() &&
        it->second.status.command_id == command_id &&
        it->second.status.generation == generation) {
      if (it->second.schedule_generation == schedule_generation &&
          !attempt->callback_admitted.load(std::memory_order_acquire)) {
        it->second.scheduled_handle = handle;
      }
      // Either the handle is now published, or the callback already crossed
      // the exact generation gate and owns this lifecycle attempt (possibly
      // scheduling its successor before this schedule() call returned).
      admitted = true;
    }
  }
  if (!admitted) {
    try {
      state->scheduler->cancel(handle);
    } catch (...) {
      // A stale queued callback is generation-fenced at entry.
    }
  }
  return admitted;
}

}  // namespace

RoboPlanGripperCommandBackend::RoboPlanGripperCommandBackend(
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    core::IClock& clock,
    core::IScheduler& scheduler,
    core::Duration update_period_ns) {
  if (!execution_state_sink) {
    throw std::invalid_argument("execution state sink must not be null");
  }
  if (update_period_ns <= 0) {
    throw std::invalid_argument("gripper update period must be positive");
  }
  state_ = std::make_shared<SharedState>();
  state_->execution_state_sink = std::move(execution_state_sink);
  state_->clock = &clock;
  state_->scheduler = &scheduler;
  state_->update_period_ns = update_period_ns;
}

RoboPlanGripperCommandBackend::~RoboPlanGripperCommandBackend() {
  invalidate_gripper_commands(core::GripperInvalidationReason::kShutdown);
  state_.reset();
}

core::GripperCommandResultDTO
RoboPlanGripperCommandBackend::command_gripper(
    const core::GripperCommandRequestDTO& request) {
  if (request.gripper_id.empty() || request.group_id.empty() ||
      request.joint_name.empty() || !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || request.duration_s < 0.0 ||
      (request.max_effort.has_value() &&
       !std::isfinite(*request.max_effort))) {
    return {false, 0, 0,
            gripper_error("gripper_invalid_command",
                          "Invalid native gripper command")};
  }

  std::uint64_t authority_generation = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || state_->admission_blocked) {
      return {false, 0, 0,
              gripper_error("gripper_command_not_authorized",
                            "Gripper command admission is closed by the execution lifecycle")};
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

  std::optional<double> start_position;
  try {
    start_position = state_->execution_state_sink->group_joint_position(
        request.group_id, request.joint_name);
  } catch (const std::exception& exception) {
    return {false, 0, 0,
            gripper_error(
                "gripper_state_unavailable",
                std::string("Native gripper start position read failed: ") +
                    exception.what())};
  } catch (...) {
    return {false, 0, 0,
            gripper_error("gripper_state_unavailable",
                          "Native gripper start position read failed")};
  }
  if (!start_position.has_value()) {
    return {false, 0, 0,
            gripper_error(
                "gripper_state_unavailable",
                "Native gripper start position is unavailable for the configured group/joint")};
  }
  if (!std::isfinite(*start_position)) {
    return {false, 0, 0,
            gripper_error(
                "gripper_state_not_finite",
                "Native gripper start position is not finite")};
  }

  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  core::TimePoint first_deadline = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || state_->admission_blocked ||
        state_->authority_generation != authority_generation) {
      return {false, 0, 0,
              gripper_error(
                  state_->closing ? "gripper_backend_closed"
                                  : "gripper_command_not_authorized",
                  state_->closing
                      ? "Native gripper backend is closing"
                      : "Gripper lifecycle authority changed during command admission")};
    }
    const auto current = state_->runtimes.find(request.gripper_id);
    if (current != state_->runtimes.end() &&
        current->second.status.state ==
            core::GripperCommandState::kExecuting) {
      return {false, current->second.status.command_id,
              current->second.status.generation,
              gripper_error("gripper_busy",
                            "Gripper is already executing a command")};
    }
    if (current != state_->runtimes.end() &&
        current->second.status.state == core::GripperCommandState::kFailed &&
        !supersedable_grasp_loss(current->second.status.error)) {
      return {false, current->second.status.command_id,
              current->second.status.generation,
              current->second.status.error};
    }

    command_id = state_->next_command_id++;
    generation = state_->next_generation++;
    auto& runtime = state_->runtimes[request.gripper_id];
    runtime = SharedState::Runtime{};
    runtime.group_id = request.group_id;
    runtime.joint_name = request.joint_name;
    runtime.start_position = *start_position;
    runtime.started_at_ns = state_->clock->now();
    runtime.deadline_ns = saturating_deadline(
        runtime.started_at_ns, duration_ns(request.duration_s));
    first_deadline = request.duration_s <= 0.0
                         ? runtime.started_at_ns
                         : std::min(runtime.deadline_ns,
                                    runtime.started_at_ns +
                                        state_->update_period_ns);

    auto& status = runtime.status;
    status.state = core::GripperCommandState::kExecuting;
    status.gripper_id = request.gripper_id;
    status.command_id = command_id;
    status.generation = generation;
    status.revision = 1;
    status.current_position = *start_position;
    status.target_position = request.target_position;
    status.commanded_position = request.target_position;
    status.observed_position = observed_position_for(
        request.state_authority, status.current_position);
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

  if (!schedule_native_gripper_tick(
          state_, request.gripper_id, command_id, generation, first_deadline)) {
    const auto status = get_gripper_status(request.gripper_id);
    return {false, command_id, generation,
            status.command_id == command_id &&
                    status.generation == generation && status.error.has_value()
                ? status.error
                : std::optional<core::ErrorDTO>(gripper_error(
                      "gripper_command_not_authorized",
                      "Gripper lifecycle authority changed before scheduling"))};
  }
  return {true, command_id, generation, std::nullopt};
}

core::GripperCommandStatusDTO
RoboPlanGripperCommandBackend::get_gripper_status(
    const std::string& gripper_id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto it = state_->runtimes.find(gripper_id);
  if (it == state_->runtimes.end()) {
    core::GripperCommandStatusDTO status;
    status.gripper_id = gripper_id;
    return status;
  }
  return it->second.status;
}

core::GripperCommandResultDTO
RoboPlanGripperCommandBackend::mark_gripper_grasp_lost(
    const std::string& gripper_id,
    std::uint64_t expected_command_id,
    std::uint64_t expected_generation,
    const std::string& evidence_label,
    const std::string& message,
    const std::string& reason_code) {
  std::optional<core::ScheduleHandle> handle;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->runtimes.find(gripper_id);
    if (it == state_->runtimes.end() ||
        it->second.status.command_id == 0 ||
        it->second.status.command_id != expected_command_id ||
        it->second.status.generation != expected_generation) {
      return {false, 0, 0,
              gripper_error("stale_gripper_state",
                            "Gripper command identity changed before grasp-loss evidence was applied")};
    }
    auto& runtime = it->second;
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
        std::find(runtime.status.evidence.begin(), runtime.status.evidence.end(),
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
      // Generation fencing already made the callback inert. Cancellation is
      // only an eager queue cleanup and must not turn accepted loss evidence
      // into an exception after the terminal state was committed.
    }
  }
  return {true, command_id, generation, std::nullopt};
}

void RoboPlanGripperCommandBackend::invalidate_gripper_commands(
    core::GripperInvalidationReason reason) noexcept {
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
    for (auto& [_, runtime] : state_->runtimes) {
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
      // `noexcept` lifecycle invalidation is authoritative. A scheduler that
      // cannot eagerly erase a task still cannot revive it because every task
      // rechecks the invalidated command and schedule generations.
    }
  }
}

void RoboPlanGripperCommandBackend::authorize_gripper_commands() noexcept {
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
