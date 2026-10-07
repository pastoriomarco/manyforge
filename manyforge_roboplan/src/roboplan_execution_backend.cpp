#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "native_stop_qualification_probe.hpp"

namespace manyforge::roboplan_backend {

namespace detail {

struct MotionIdleHeartbeatState final {
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  core::IClock *clock{};
  core::IScheduler *scheduler{};
  core::Duration period_ns{};

  std::mutex mutex;
  std::shared_ptr<const std::vector<double>> positions;
  std::shared_ptr<const std::vector<double>> velocities;
  std::optional<core::ScheduleHandle> scheduled_handle;
  std::uint64_t schedule_generation{};
  int consecutive_busy{};
  bool enabled{};
  bool closing{};
};

} // namespace detail

namespace {

constexpr int kTerminalSceneProjectionRetryAttempts = 10;
constexpr auto kTerminalSceneProjectionRetryDelay =
    std::chrono::milliseconds(20);
constexpr int kIdleHeartbeatBusyRetryLimit = 10;
constexpr std::size_t kMaximumStopStageErrorDetails = 8U;

// A replacement may consume its already elapsed command-frame lead-in, but
// must never restart that prefix after planning or dispatch was delayed.
[[nodiscard]] core::Result<double>
stop_replay_elapsed(std::chrono::steady_clock::time_point origin,
                    double lead_in_budget_s) {
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - origin)
          .count();
  if (!std::isfinite(elapsed) || elapsed < 0.0 || elapsed > lead_in_budget_s) {
    return core::Result<double>::Err(core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "Native Stop missed its command-frame handoff budget",
        {{"reason", "native_stop_handoff_budget_exceeded"},
         {"elapsed_s", std::to_string(elapsed)},
         {"lead_in_budget_s", std::to_string(lead_in_budget_s)},
         {"recommended_disposition", "hard_stop"}}});
  }
  return core::Result<double>::Ok(elapsed);
}

struct HeartbeatScheduleAttempt final {
  std::atomic<bool> callback_admitted{false};
};

[[nodiscard]] core::TimePoint
saturating_deadline(const core::TimePoint now,
                    const core::Duration duration) noexcept {
  if (duration > 0 &&
      now > std::numeric_limits<core::TimePoint>::max() - duration) {
    return std::numeric_limits<core::TimePoint>::max();
  }
  return now + duration;
}

[[nodiscard]] bool schedule_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state,
    core::TimePoint deadline_ns);

void run_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state,
    const std::uint64_t schedule_generation,
    const core::TimePoint scheduled_deadline_ns,
    const std::shared_ptr<HeartbeatScheduleAttempt> &attempt) noexcept {
  std::shared_ptr<const std::vector<double>> positions;
  std::shared_ptr<const std::vector<double>> velocities;
  core::TimePoint now_ns{};
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || !state->enabled ||
        state->schedule_generation != schedule_generation) {
      return;
    }
    attempt->callback_admitted.store(true, std::memory_order_release);
    state->scheduled_handle.reset();
    now_ns = state->clock->now();
    if (now_ns < scheduled_deadline_ns || !state->positions ||
        !state->velocities) {
      state->enabled = false;
      return;
    }
    positions = state->positions;
    velocities = state->velocities;
  }

  RetainedStateProjectionResult published;
  try {
    published = state->state_channel->publish_state(*positions, *velocities,
                                                    now_ns, now_ns);
  } catch (const std::exception &error) {
    published = {
        RetainedStateProjectionStatus::kFailed,
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInternalError)),
            "Native motion idle-state heartbeat threw",
            {{"reason", "native_motion_state_heartbeat_exception"},
             {"exception", error.what()}}}};
  } catch (...) {
    published = {
        RetainedStateProjectionStatus::kFailed,
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInternalError)),
            "Native motion idle-state heartbeat threw",
            {{"reason", "native_motion_state_heartbeat_exception"}}}};
  }

  bool schedule_again = false;
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || !state->enabled ||
        state->schedule_generation != schedule_generation) {
      return;
    }
    if (published.applied()) {
      state->consecutive_busy = 0;
      schedule_again = true;
    } else if (published.status == RetainedStateProjectionStatus::kBusy &&
               ++state->consecutive_busy < kIdleHeartbeatBusyRetryLimit) {
      schedule_again = true;
    } else {
      // Loss of the owner-issued state source is observable as unavailable
      // evidence. Do not refresh timestamps or fabricate recovery truth.
      state->enabled = false;
    }
  }
  if (schedule_again) {
    (void)schedule_idle_heartbeat(
        state, saturating_deadline(now_ns, state->period_ns));
  }
}

[[nodiscard]] bool schedule_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state,
    const core::TimePoint deadline_ns) {
  std::optional<core::ScheduleHandle> old_handle;
  std::uint64_t schedule_generation{};
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || !state->enabled || !state->positions ||
        !state->velocities) {
      return false;
    }
    old_handle = state->scheduled_handle;
    state->scheduled_handle.reset();
    schedule_generation = ++state->schedule_generation;
  }
  if (old_handle.has_value()) {
    try {
      state->scheduler->cancel(*old_handle);
    } catch (...) {
      // The incremented generation makes a late callback inert.
    }
  }

  const std::weak_ptr<detail::MotionIdleHeartbeatState> weak_state = state;
  const auto attempt = std::make_shared<HeartbeatScheduleAttempt>();
  core::ScheduleHandle handle{};
  try {
    handle = state->scheduler->schedule(
        deadline_ns,
        [weak_state, attempt, schedule_generation, deadline_ns]() noexcept {
          if (const auto retained = weak_state.lock()) {
            run_idle_heartbeat(retained, schedule_generation, deadline_ns,
                               attempt);
          }
        },
        core::SchedulerPriority::RuntimeSafety);
  } catch (...) {
    if (attempt->callback_admitted.load(std::memory_order_acquire)) {
      return true;
    }
    std::lock_guard lock(state->mutex);
    if (!state->closing && state->enabled &&
        state->schedule_generation == schedule_generation) {
      state->enabled = false;
    }
    return false;
  }

  bool retained = attempt->callback_admitted.load(std::memory_order_acquire);
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing && state->enabled) {
      if (state->schedule_generation == schedule_generation &&
          !attempt->callback_admitted.load(std::memory_order_acquire)) {
        state->scheduled_handle = handle;
      }
      retained = true;
    }
  }
  if (!retained) {
    try {
      state->scheduler->cancel(handle);
    } catch (...) {
      // A stale callback is generation-fenced at entry.
    }
  }
  return retained;
}

void arm_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state,
    const std::vector<double> &positions) noexcept {
  if (!state || positions.empty()) {
    return;
  }
  try {
    auto retained_positions =
        std::make_shared<const std::vector<double>>(positions);
    auto retained_velocities =
        std::make_shared<const std::vector<double>>(positions.size(), 0.0);
    {
      std::lock_guard lock(state->mutex);
      if (state->closing) {
        return;
      }
      state->positions = std::move(retained_positions);
      state->velocities = std::move(retained_velocities);
      state->consecutive_busy = 0;
      state->enabled = true;
    }
    (void)schedule_idle_heartbeat(
        state, saturating_deadline(state->clock->now(), state->period_ns));
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->enabled = false;
  }
}

void disarm_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state) noexcept {
  if (!state) {
    return;
  }
  std::optional<core::ScheduleHandle> handle;
  {
    std::lock_guard lock(state->mutex);
    state->enabled = false;
    ++state->schedule_generation;
    handle = state->scheduled_handle;
    state->scheduled_handle.reset();
  }
  if (handle.has_value()) {
    try {
      state->scheduler->cancel(*handle);
    } catch (...) {
    }
  }
}

void close_idle_heartbeat(
    const std::shared_ptr<detail::MotionIdleHeartbeatState> &state) noexcept {
  if (!state) {
    return;
  }
  std::optional<core::ScheduleHandle> handle;
  {
    std::lock_guard lock(state->mutex);
    state->closing = true;
    state->enabled = false;
    ++state->schedule_generation;
    handle = state->scheduled_handle;
    state->scheduled_handle.reset();
  }
  if (handle.has_value()) {
    try {
      state->scheduler->cancel(*handle);
    } catch (...) {
    }
  }
  try {
    (void)state->scheduler->drain_for(core::SchedulerPriority::RuntimeSafety,
                                      std::chrono::seconds(5));
  } catch (...) {
  }
}

double controlled_stop_wait_timeout_s(
    const core::MotionInterruptStopProfileDTO &profile,
    double stop_completion_tolerance_s) {
  return core::motion_interrupt_deceleration_time_or_default(profile) +
         std::max(0.0, stop_completion_tolerance_s);
}

std::map<std::string, std::string>
stop_stage_error_details(const std::map<std::string, std::string> &details,
                         std::string reason) {
  std::map<std::string, std::string> source = details;
  source.insert_or_assign("reason", std::move(reason));
  std::map<std::string, std::string> projected;
  const auto add = [&](std::string_view key) {
    const auto found = source.find(std::string(key));
    if (found == source.end() || projected.size() >= 7U || key.empty() ||
        key.size() > 64U || key.find('\0') != std::string_view::npos) {
      return;
    }
    auto value = found->second;
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());
    if (value.size() > 256U) {
      value.resize(256U);
    }
    projected.emplace(found->first, std::move(value));
  };
  constexpr std::array preferred{
      std::string_view("reason"),
      std::string_view("recommended_disposition"),
      std::string_view("trigger_reason"),
      std::string_view("validation_failure_reason"),
      std::string_view("retimer_validation_failure_reason"),
      std::string_view("ali_validation_failure_reason"),
      std::string_view("handoff_path_time_s"),
      std::string_view("joint")};
  for (const auto key : preferred) {
    add(key);
  }
  for (const auto &[key, value] : source) {
    (void)value;
    add(key);
  }
  if (projected.size() < source.size()) {
    projected.insert_or_assign("details_truncated", "true");
  }
  if (projected.empty()) {
    projected.emplace("reason", "native_stop_stage_failed");
  }
  if (projected.size() > kMaximumStopStageErrorDetails) {
    projected.erase(std::prev(projected.end()));
  }
  return projected;
}

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

void atomic_max(std::atomic<std::uint64_t> &target, std::uint64_t value) {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(
                                current, value, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
  }
}

template <typename Rep, typename Period>
std::string seconds_string(std::chrono::duration<Rep, Period> duration) {
  return std::to_string(std::chrono::duration<double>(duration).count());
}

core::ParticipantStopStageResultDTO
terminal_stop_stage_failure(const core::ResolvedParticipantStopStageDTO &stage,
                            std::string reason, std::string message,
                            std::int64_t elapsed_ns = 0) {
  const core::ErrorDTO error{core::kErrorMotionInterruptBackendStopFailed,
                             std::move(message),
                             {{"reason", std::move(reason)}}};
  core::ParticipantStopStageResultDTO result{
      stage,
      core::StopStageAttemptDispositionDTO::kNotDispatched,
      core::ParticipantStopStageOutcomeDTO::kFailed,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::max<std::int64_t>(0, elapsed_ns),
      {},
      error};
  if (stage.stage_index < stage.participant_stop_plan.stage_commands.size()) {
    const auto &requirements =
        stage.participant_stop_plan.stage_commands[stage.stage_index]
            .resolved_stage.evidence_requirements;
    result.evidence_items.reserve(requirements.size());
    for (const auto &requirement : requirements) {
      result.evidence_items.push_back(core::StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id,
          requirement.requirement.evidence_kind,
          core::StopEvidenceItemStateDTO::kMissing, requirement.resolved_source,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt, error});
    }
  }
  return result;
}

core::ParticipantStopStageResultDTO
superseded_stop_stage(const core::ResolvedParticipantStopStageDTO &stage) {
  const core::ErrorDTO error{"stop_stage_superseded_before_dispatch",
                             "Stop stage was cancelled before dispatch",
                             {}};
  auto result = terminal_stop_stage_failure(
      stage, "stop_stage_superseded_before_dispatch",
      "Stop stage was cancelled before dispatch");
  result.outcome = core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect;
  result.error = error;
  for (auto &item : result.evidence_items) {
    item.state = core::StopEvidenceItemStateDTO::kInapplicable;
    item.error = error;
  }
  return result;
}

// The exact prepared goal already reached its accepted endpoint after Stop
// acceptance and before this controlled stage. No Stop command is issued; the
// result is the profile-authorized natural completion, and the caller still
// requires fresh post-observation rest and exact goal-terminal evidence.
core::ControlledStopResultDTO
natural_completion_before_stage(const core::MotionInterruptStopProfileDTO &profile,
                                std::int64_t observed_after_stage_start_ns) {
  core::ControlledStopResultDTO result;
  result.accepted = true;
  result.backend_stop_confirmed = true;
  result.completed_by_natural_completion = true;
  result.executed_stop_mode = profile.stop_mode;
  result.stop_command_source =
      core::MotionInterruptStopCommandSource::kDiagnosticOther;
  result.confirmation_latency_ns =
      std::max<std::int64_t>(0, observed_after_stage_start_ns);
  result.diagnostics["command_source"] =
      core::interrupt_structured_string("native_simulated_natural_completion");
  result.diagnostics["natural_completion_basis"] =
      core::interrupt_structured_string("goal_completed_before_stop_stage");
  result.diagnostics["requested_stop_mode"] = core::interrupt_structured_string(
      profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath
          ? "soft_on_path"
          : "spring_back_hold");
  return result;
}

std::optional<std::int64_t>
remaining_phase(const core::RemainingStopBudgetDTO &budget,
                const core::StopBudgetPhaseId &phase_id) {
  const auto found = std::find_if(
      budget.phases.begin(), budget.phases.end(),
      [&](const auto &phase) { return phase.phase_id == phase_id; });
  return found == budget.phases.end()
             ? std::nullopt
             : std::optional(found->remaining_deadline_offset_ns);
}

} // namespace

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel,
    core::IClock &clock, RoboPlanExecutionBackendConfig config)
    : RoboPlanExecutionBackend(std::move(binding), std::move(state_channel),
                               clock, std::move(config), nullptr, nullptr) {}

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel,
    core::IClock &clock, core::IScheduler &scheduler,
    RoboPlanExecutionBackendConfig config)
    : RoboPlanExecutionBackend(std::move(binding), std::move(state_channel),
                               clock, std::move(config), &scheduler, nullptr) {}

RoboPlanExecutionBackend::RoboPlanExecutionBackend(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> state_channel,
    core::IClock &clock, RoboPlanExecutionBackendConfig config,
    core::IScheduler *heartbeat_scheduler,
    detail::INativeStopQualificationProbe *qualification_probe)
    : binding_(std::make_shared<const core::RuntimeParticipantBinding>(
          std::move(binding))),
      program_aliases_(std::make_shared<
                       const std::vector<core::RuntimeParticipantBinding>>()),
      state_channel_(std::move(state_channel)), clock_(clock),
      config_(std::move(config)), qualification_probe_(qualification_probe) {
  if (config_.reset_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.reset_completion_timeout_s)) {
    throw std::invalid_argument(
        "reset_completion_timeout_s must be finite and > 0");
  }
  if (config_.execution_tick_period_ns <= 0 ||
      config_.execution_tick_period_ns > 1'000'000'000) {
    throw std::invalid_argument(
        "execution_tick_period_ns must be in (0, 1 second]");
  }
  if (config_.soft_on_path_stop_traj_delta_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_stop_traj_delta_s)) {
    throw std::invalid_argument(
        "soft_on_path_stop_traj_delta_s must be finite and >= 0");
  }
  if (config_.soft_on_path_sample_period_s <= 0.0 ||
      !std::isfinite(config_.soft_on_path_sample_period_s)) {
    throw std::invalid_argument(
        "soft_on_path_sample_period_s must be finite and > 0");
  }
  if (config_.soft_on_path_max_initial_velocity_deviation_rad_s.has_value() &&
      (!std::isfinite(
           *config_.soft_on_path_max_initial_velocity_deviation_rad_s) ||
       *config_.soft_on_path_max_initial_velocity_deviation_rad_s < 0.0)) {
    throw std::invalid_argument("soft_on_path_max_initial_velocity_deviation_"
                                "rad_s must be finite and >= 0");
  }
  if (config_.soft_on_path_max_initial_path_deviation_rad.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_path_deviation_rad) ||
       *config_.soft_on_path_max_initial_path_deviation_rad < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_path_deviation_rad must be finite and >= 0");
  }
  const auto retained_binding = binding_.load(std::memory_order_acquire);
  const auto &value = retained_binding->value();
  if (!state_channel_ || !value.control_path.has_value() ||
      value.model_groups.size() != 1U || value.resources.empty()) {
    throw std::invalid_argument(
        "native execution requires one exact retained motion target");
  }
  for (const auto &resource : value.resources) {
    if (resource.coordinate_kind != core::CoordinateKind::kAngular ||
        std::count(
            value.model_groups.front().expanded_compiled_joint_names.begin(),
            value.model_groups.front().expanded_compiled_joint_names.end(),
            resource.compiled_resource_name) != 1) {
      throw std::invalid_argument(
          "native trajectory execution requires an all-angular target");
    }
  }
  auto expected_context = core::make_observed_state_query_context(
      *retained_binding, state_channel_->query_context().freshness_budget_ns);
  if (!expected_context.ok() ||
      expected_context.value() != state_channel_->query_context()) {
    throw std::invalid_argument(
        "native execution state channel crosses its retained binding");
  }
  observed_at_rest_velocities_.assign(value.resources.size(), 0.0);
  execution_progress_publication_ =
      std::make_unique<core::ExecutionProgressPublication>(
          core::ExecutionProgressSourceKind::kBackendCommanded,
          core::ExecutionProgressAuthorityProfile::kPredictedNative,
          "native_predicted_progress_v1",
          /*freshness_budget_ns=*/0);
  state_.snapshot.timestamp_ns = clock_.now();
  if (heartbeat_scheduler != nullptr) {
    const auto freshness_budget_ns =
        state_channel_->query_context().freshness_budget_ns;
    const auto heartbeat_period_ns =
        freshness_budget_ns > 1 ? std::min(config_.execution_tick_period_ns,
                                           freshness_budget_ns / 2)
                                : config_.execution_tick_period_ns;
    idle_heartbeat_ = std::make_shared<detail::MotionIdleHeartbeatState>();
    idle_heartbeat_->state_channel = state_channel_;
    idle_heartbeat_->clock = &clock_;
    idle_heartbeat_->scheduler = heartbeat_scheduler;
    idle_heartbeat_->period_ns =
        std::max<core::Duration>(1, heartbeat_period_ns);
  }

  auto controlled = state_channel_->current_controlled_state();
  if (controlled.ok() &&
      controlled.value().positions.size() == value.resources.size()) {
    std::vector<double> positions;
    positions.reserve(controlled.value().positions.size());
    bool all_angular = true;
    for (const auto &position : controlled.value().positions) {
      const auto *angular = std::get_if<core::AngularPositionDTO>(&position);
      if (angular == nullptr || !std::isfinite(angular->value_rad)) {
        all_angular = false;
        break;
      }
      positions.push_back(angular->value_rad);
    }
    if (all_angular) {
      (void)publish_snapshot(core::ExecutionState::kIdle, 0.0, positions);
    }
  }
}

RoboPlanExecutionBackend::~RoboPlanExecutionBackend() {
  stop_terminal_scene_projection_retry_worker();
  close_idle_heartbeat(idle_heartbeat_);
  (void)reset();
  // Join while the execution state and condition variable are still alive.
  // Member-order auto-join would happen only after those worker dependencies
  // begin destruction.
  if (worker_thread_.joinable()) {
    request_execution_worker_stop();
    worker_thread_.join();
  }
}

core::PrepareParticipantResultDTO RoboPlanExecutionBackend::Prepare(
    const core::PreparedOperationParticipantDTO &participant,
    const std::uint64_t execution_id) {
  const auto reject = [&](std::string reason) {
    auto result = core::MakePrepareParticipantResult(
        participant, core::PrepareParticipantResultStateDTO::kRejected,
        core::ErrorDTO{core::kErrorExecutionRejected,
                       "Native backend rejected prepared participant",
                       {{"reason", std::move(reason)}}});
    if (!result.ok()) {
      throw std::invalid_argument(result.error().message);
    }
    return std::move(result.value());
  };
  const auto retained_binding = binding_.load(std::memory_order_acquire);
  const auto binding_error =
      [&](const core::RuntimeParticipantBinding &binding) {
        return core::prepared_participant_backend_binding_field_error(
            participant, binding,
            core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      };
  auto field_error = binding_error(*retained_binding);
  if (field_error.has_value()) {
    const auto aliases = program_aliases_.load(std::memory_order_acquire);
    const auto matched =
        std::find_if(aliases->begin(), aliases->end(), [&](const auto &alias) {
          return !binding_error(alias).has_value();
        });
    if (matched == aliases->end()) {
      field_error =
          core::prepared_participant_backend_physical_binding_field_error(
              participant, *retained_binding,
              core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      if (field_error.has_value()) {
        return reject(std::string(*field_error));
      }
    }
  }
  const auto &motion =
      std::get<core::PreparedMotionExecutionDTO>(participant.payload);
  if (motion.trajectory.resources != participant.resources) {
    return reject("prepared_trajectory_resource_scope_mismatch");
  }
  if (!motion.trajectory.runtime_scale_bounds) {
    return reject("native_runtime_scale_preparation_missing");
  }

  std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
  auto result = core::MakePrepareParticipantResult(
      participant, core::PrepareParticipantResultStateDTO::kPrepared);
  if (!result.ok()) {
    throw std::invalid_argument(result.error().message);
  }
  const bool same_preparation = prepared_membership_.matches(participant);
  if (const auto invalid =
          prepared_membership_.prepare(execution_id, participant))
    return reject(std::string(*invalid));
  if (!same_preparation)
    prepared_execution_clock_.reset();
  return std::move(result.value());
}

core::Result<void> RoboPlanExecutionBackend::BindPreparedExecutionClock(
    const core::PreparedOperationParticipantDTO &participant,
    std::shared_ptr<core::detail::CoordinatedExecutionClock> clock) {
  std::lock_guard lock(prepared_execution_mutex_);
  const auto *motion =
      std::get_if<core::PreparedMotionExecutionDTO>(&participant.payload);
  if (!clock || !prepared_membership_.matches(participant) || !motion ||
      !motion->trajectory.runtime_scale_bounds ||
      !clock->accepts(
          participant.participant_id,
          {motion->trajectory.runtime_scale_bounds->max_rate_per_s,
           motion->trajectory.runtime_scale_bounds->max_rate_change_per_s2})) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Prepared coordinated clock does not match the participant",
        {{"reason", "prepared_execution_clock_mismatch"}}});
  }
  prepared_execution_clock_ = std::move(clock);
  return core::Result<void>::Ok();
}

core::Result<void> RoboPlanExecutionBackend::RetirePreparedExecution(
    const std::uint64_t execution_id) {
  std::lock_guard lock(prepared_execution_mutex_);
  const auto active = prepared_membership_.active_execution_id();
  if (active == execution_id &&
      (state_.running.load(std::memory_order_acquire) ||
       controlled_stop_activity_count_.load(std::memory_order_acquire) != 0U)) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Prepared execution still has active motion or Stop work",
        {{"reason", "prepared_execution_not_quiescent"}}});
  }
  if (!prepared_membership_.retire(execution_id)) {
    return core::Result<void>::Err(
        core::ErrorDTO{core::kErrorInvalidRequest,
                       "Prepared retirement names an unknown execution",
                       {{"reason", "prepared_execution_stale"}}});
  }
  if (active == execution_id) {
    prepared_goal_.reset();
    prepared_execution_clock_.reset();
  }
  return core::Result<void>::Ok();
}

core::ExecutionHandle RoboPlanExecutionBackend::dispatch_prepared(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation,
    bool resume_dispatch) {
  const auto rejected = [&](std::string reason) {
    core::ExecutionSnapshot snapshot;
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = core::ExecutionState::kFailed;
    snapshot.error = core::ErrorDTO{core::kErrorExecutionRejected,
                                    "Native backend rejected prepared dispatch",
                                    {{"reason", std::move(reason)}}};
    core::ExecutionHandle handle{
        [](std::int64_t) { return true; },
        [snapshot = std::move(snapshot)]() { return snapshot; }, 0U};
    handle.controller_goal_id = controller_goal_id;
    handle.controller_goal_generation = controller_goal_generation;
    handle.prepared_contract_digest = participant.prepared_contract_digest;
    return handle;
  };

  std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
  if (controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
    return rejected("controller_goal_identity_invalid");
  }
  const auto retained_binding = binding_.load(std::memory_order_acquire);
  const auto binding_error =
      [&](const core::RuntimeParticipantBinding &binding) {
        return core::prepared_participant_backend_binding_field_error(
            participant, binding,
            core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      };
  auto field_error = binding_error(*retained_binding);
  if (field_error.has_value()) {
    const auto aliases = program_aliases_.load(std::memory_order_acquire);
    const auto matched =
        std::find_if(aliases->begin(), aliases->end(), [&](const auto &alias) {
          return !binding_error(alias).has_value();
        });
    if (matched == aliases->end()) {
      field_error =
          core::prepared_participant_backend_physical_binding_field_error(
              participant, *retained_binding,
              core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      if (field_error.has_value()) {
        return rejected(std::string(*field_error));
      }
    }
  }
  if (!prepared_membership_.matches(participant)) {
    return rejected("prepared_participant_not_admitted");
  }
  if (prepared_goal_ && prepared_goal_->goal_id == controller_goal_id) {
    if (prepared_goal_->generation == controller_goal_generation &&
        prepared_goal_->prepared_contract_digest ==
            participant.prepared_contract_digest) {
      return prepared_goal_->handle;
    }
    return rejected("controller_goal_identity_conflict");
  }

  const auto &trajectory =
      std::get<core::PreparedMotionExecutionDTO>(participant.payload)
          .trajectory;
  if (resume_dispatch && state_.running.load(std::memory_order_acquire))
    request_active_controlled_stop();
  auto handle = execute_with_clock(trajectory, prepared_execution_clock_,
                                   participant.participant_id);
  handle.controller_goal_id = controller_goal_id;
  handle.controller_goal_generation = controller_goal_generation;
  handle.prepared_contract_digest = participant.prepared_contract_digest;
  if (core::prepared_execution_handle_field_error(
          handle, participant, controller_goal_id, controller_goal_generation))
    return handle;
  std::vector<core::PreparedParticipantStopPlanDTO> stop_plans;
  stop_plans.push_back(participant.ordinary_stop_plan);
  if (participant.fault_stop_plan.has_value()) {
    stop_plans.push_back(*participant.fault_stop_plan);
  }
  for (const auto &scope :
       std::get<core::PreparedMotionExecutionDTO>(participant.payload)
           .interrupt_scopes) {
    stop_plans.push_back(scope.stop_plan);
  }
  prepared_goal_ =
      PreparedGoalRecord{controller_goal_id,
                         controller_goal_generation,
                         participant.prepared_contract_digest,
                         participant.operation_id,
                         participant.operation_revision,
                         participant.participant_id,
                         participant.controllable_component_id,
                         participant.command_target_id,
                         participant.capability_id,
                         participant.control_domain_id,
                         participant.control_domain_revision,
                         participant.control_binding_id,
                         participant.control_binding_revision,
                         participant.controller_topology_id,
                         participant.topology_revision,
                         participant.control_route_id,
                         participant.active_control_configuration_id,
                         participant.active_control_configuration_generation,
                         participant.control_provider.runtime_epoch,
                         participant.resources,
                         participant.stop_evidence_policy_id,
                         participant.stop_evidence_policy_revision,
                         std::move(stop_plans),
                         handle};
  return handle;
}

core::ExecutionHandle RoboPlanExecutionBackend::Dispatch(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation) {
  return dispatch_prepared(participant, controller_goal_id,
                           controller_goal_generation, false);
}

core::ExecutionHandle RoboPlanExecutionBackend::DispatchResume(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation) {
  return dispatch_prepared(participant, controller_goal_id,
                           controller_goal_generation, true);
}

void RoboPlanExecutionBackend::observe_stop_qualification(
    detail::NativeStopQualificationEvent event, std::string_view detail) {
  if (qualification_probe_ != nullptr) {
    qualification_probe_->observe(event, detail);
  }
}

std::int64_t RoboPlanExecutionBackend::publish_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double> &joint_positions,
    std::optional<core::ErrorDTO> error, TimeLawSample *time_law) {
  const auto timestamp_ns = time_law ? time_law->timestamp_ns : clock_.now();
  {
    // Freeze and target consumption are ordered with the same existing mutex.
    // No source/Scene publication occurs while it is held. An accepted target
    // not yet consumed when Stop freezes remains pending, not a new lead-in.
    std::unique_lock scale_lock(runtime_velocity_scale_mutex_, std::defer_lock);
    if (time_law) {
      scale_lock.lock();
      if (controlled_stop_runtime_scale_freeze_depth_ == 0) {
        time_law->valid =
            time_law->governor.retarget(time_law->available_target_scale);
      }
      if (!time_law->valid) {
        state = core::ExecutionState::kFailed;
        error = core::ErrorDTO{
            core::kErrorExecutionRejected,
            "Native runtime time law became invalid",
            {{"reason", "native_runtime_scale_retarget_invalid"}}};
      }
    }
    std::lock_guard lock(state_mutex_);
    state_.snapshot.timestamp_ns = timestamp_ns;
    state_.snapshot.sequence_number++;
    state_.snapshot.state = state;
    state_.snapshot.progress = clamp_progress(progress);
    state_.snapshot.joint_positions_rad = joint_positions;
    state_.snapshot.error = std::move(error);
    if (time_law) {
      state_.active_trajectory_time_s = time_law->trajectory_time_s;
      state_.active_runtime_velocity_scale = time_law->governor.scale();
      state_.active_runtime_scale_rate_per_s = time_law->governor.rate_per_s();
      state_.active_runtime_scale_target = time_law->governor.target_scale();
    } else if (state != core::ExecutionState::kExecuting &&
               state != core::ExecutionState::kStopping) {
      state_.active_runtime_scale_rate_per_s = 0.0;
    }
    if (controlled_stop_activity_count_.load(std::memory_order_acquire) != 0U)
      stop_snapshot_cv_.notify_all();
  }
  const bool at_rest_state =
      state == core::ExecutionState::kIdle ||
      state == core::ExecutionState::kPaused ||
      state == core::ExecutionState::kStopped ||
      state == core::ExecutionState::kCompleted ||
      state == core::ExecutionState::kFailed ||
      state == core::ExecutionState::kInterventionRequired;
  disarm_idle_heartbeat(idle_heartbeat_);
  // RT-8C publication is deliberately outside state_mutex_: the RT0 writer
  // has its own bounded preallocated lane and cannot queue behind lifecycle
  // snapshot readers.
  bool state_published = false;
  if (joint_positions.size() == observed_at_rest_velocities_.size()) {
    const std::span<const double> velocities =
        at_rest_state ? std::span<const double>(observed_at_rest_velocities_)
                      : std::span<const double>{};
    const int attempts = at_rest_state ? 8 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      const auto published = publish_observed_state_authority(
          joint_positions, velocities, timestamp_ns);
      if (published.applied()) {
        state_published = true;
        break;
      }
      if (published.status != RetainedStateProjectionStatus::kBusy) {
        break;
      }
      if (attempt + 1 < attempts) {
        std::this_thread::yield();
      }
    }
  }
  if (at_rest_state && state_published) {
    arm_idle_heartbeat(idle_heartbeat_, joint_positions);
  }
  return timestamp_ns;
}

RetainedStateProjectionResult
RoboPlanExecutionBackend::publish_observed_state_authority(
    std::span<const double> positions, std::span<const double> velocities,
    std::int64_t timestamp_ns) noexcept {
  try {
    return state_channel_->publish_state(positions, velocities, timestamp_ns,
                                         timestamp_ns);
  } catch (const std::exception &error) {
    return {RetainedStateProjectionStatus::kFailed,
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "Native state publication threw",
                {{"reason", "native_state_publication_exception"},
                 {"exception", error.what()}}}};
  } catch (...) {
    return {RetainedStateProjectionStatus::kFailed,
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "Native state publication threw",
                {{"reason", "native_state_publication_exception"}}}};
  }
}

bool RoboPlanExecutionBackend::update_scene_positions(
    const std::vector<double> &joint_positions, SceneUpdateMode mode,
    std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return true;
  }

  const auto now_ns = clock_.now();
  bool latest_matches = false;
  if (auto latest = state_channel_->current_state(now_ns);
      latest.ok() &&
      latest.value().positions.size() == joint_positions.size()) {
    latest_matches = true;
    for (std::size_t index = 0U; index < joint_positions.size(); ++index) {
      const auto *angular = std::get_if<core::AngularPositionDTO>(
          &latest.value().positions[index]);
      if (angular == nullptr || angular->value_rad != joint_positions[index]) {
        latest_matches = false;
        break;
      }
    }
  }
  if (!latest_matches) {
    const auto published = publish_observed_state_authority(
        joint_positions, std::span<const double>{},
        source_timestamp_ns > 0 ? source_timestamp_ns : now_ns);
    if (!published.applied()) {
      record_scene_projection_miss();
      return false;
    }
  }
  const auto projected = state_channel_->project_latest(
      now_ns, mode == SceneUpdateMode::kBlocking
                  ? RetainedStateProjectionMode::kBlocking
                  : RetainedStateProjectionMode::kBestEffort);
  if (!projected.applied()) {
    record_scene_projection_miss();
    return false;
  }
  record_scene_projection_success(source_timestamp_ns);
  return true;
}

void RoboPlanExecutionBackend::record_scene_projection_success(
    std::int64_t source_timestamp_ns) {
  const auto now_ns = clock_.now();
  scene_projection_successes_total_.fetch_add(1, std::memory_order_relaxed);
  scene_projection_last_success_timestamp_ns_.store(now_ns,
                                                    std::memory_order_relaxed);
  if (source_timestamp_ns > 0 && now_ns >= source_timestamp_ns) {
    atomic_max(scene_projection_lag_max_ns_,
               static_cast<std::uint64_t>(now_ns - source_timestamp_ns));
  }
}

void RoboPlanExecutionBackend::record_scene_projection_miss() {
  scene_projection_misses_total_.fetch_add(1, std::memory_order_relaxed);
}

RoboPlanSceneProjectionMetrics
RoboPlanExecutionBackend::scene_projection_metrics() const {
  RoboPlanSceneProjectionMetrics metrics;
  metrics.misses_total =
      scene_projection_misses_total_.load(std::memory_order_relaxed);
  metrics.successful_updates_total =
      scene_projection_successes_total_.load(std::memory_order_relaxed);
  metrics.lag_max_ns =
      scene_projection_lag_max_ns_.load(std::memory_order_relaxed);
  metrics.last_success_timestamp_ns =
      scene_projection_last_success_timestamp_ns_.load(
          std::memory_order_relaxed);
  metrics.terminal_retries_scheduled_total =
      terminal_scene_projection_retries_scheduled_total_.load(
          std::memory_order_relaxed);
  metrics.terminal_retry_successes_total =
      terminal_scene_projection_retry_successes_total_.load(
          std::memory_order_relaxed);
  metrics.terminal_retry_exhausted_total =
      terminal_scene_projection_retry_exhausted_total_.load(
          std::memory_order_relaxed);
  const auto now_ns = clock_.now();
  if (metrics.last_success_timestamp_ns > 0 &&
      now_ns >= metrics.last_success_timestamp_ns) {
    metrics.last_success_age_ns =
        static_cast<std::uint64_t>(now_ns - metrics.last_success_timestamp_ns);
  }
  return metrics;
}

core::Result<void> RoboPlanExecutionBackend::RebindRoute(
    core::RuntimeParticipantBinding binding,
    std::vector<core::RuntimeParticipantBinding> program_aliases) {
  std::lock_guard prepared_lock(prepared_execution_mutex_);
  if (prepared_membership_.active_execution_id() ||
      state_.running.load(std::memory_order_acquire) ||
      controlled_stop_activity_count_.load(std::memory_order_acquire) != 0U) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "native route publication cannot rebind an active motion backend",
        {{"reason", "native_motion_route_rebind_busy"}}});
  }
  auto expected = core::make_observed_state_query_context(
      binding, state_channel_->query_context().freshness_budget_ns);
  if (!expected.ok() || expected.value() != state_channel_->query_context()) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        "native route publication crosses the motion state authority",
        {{"reason", "native_motion_route_rebind_mismatch"}}});
  }
  if (program_aliases.size() > 64U ||
      std::any_of(
          program_aliases.begin(), program_aliases.end(),
          [&](const auto &alias) {
            return !core::runtime_participant_binding_physically_contains(
                binding, alias);
          })) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        "native execution Program aliases cross the physical backend",
        {{"reason", "native_motion_program_alias_mismatch"}}});
  }
  try {
    auto retained = std::make_shared<const core::RuntimeParticipantBinding>(
        std::move(binding));
    auto retained_aliases =
        std::make_shared<const std::vector<core::RuntimeParticipantBinding>>(
            std::move(program_aliases));
    binding_.store(std::move(retained), std::memory_order_release);
    program_aliases_.store(std::move(retained_aliases),
                           std::memory_order_release);
    return core::Result<void>::Ok();
  } catch (...) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInternalError)),
        "native route publication could not retain the motion binding",
        {{"reason", "native_motion_route_rebind_capacity_unavailable"}}});
  }
}

void RoboPlanExecutionBackend::schedule_terminal_scene_projection_retry(
    std::vector<double> joint_positions, std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(terminal_scene_projection_retry_mutex_);
    pending_terminal_scene_projection_ = PendingTerminalSceneProjection{
        std::move(joint_positions), source_timestamp_ns};
    terminal_scene_projection_retries_scheduled_total_.fetch_add(
        1, std::memory_order_relaxed);
    if (!terminal_scene_projection_retry_thread_.joinable()) {
      terminal_scene_projection_retry_thread_ =
          std::jthread([this](std::stop_token stop_token) {
            terminal_scene_projection_retry_worker(stop_token);
          });
    }
  }
  terminal_scene_projection_retry_cv_.notify_one();
}

void RoboPlanExecutionBackend::terminal_scene_projection_retry_worker(
    std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    PendingTerminalSceneProjection pending;
    {
      std::unique_lock<std::mutex> lock(terminal_scene_projection_retry_mutex_);
      // Token-aware wait: a plain condition_variable whose predicate reads
      // stop_requested() (state that flips outside this mutex) can lose the
      // stop wakeup between predicate evaluation and enqueue, deadlocking the
      // destructor join. The (lock, stoken, pred) overload re-checks the stop
      // state under the lock and cannot lose it.
      terminal_scene_projection_retry_cv_.wait(lock, stop_token, [this]() {
        return pending_terminal_scene_projection_.has_value();
      });
      if (stop_token.stop_requested()) {
        return;
      }
      pending = std::move(*pending_terminal_scene_projection_);
      pending_terminal_scene_projection_.reset();
    }

    bool projected = false;
    for (int attempt = 0; attempt < kTerminalSceneProjectionRetryAttempts;
         ++attempt) {
      if (stop_token.stop_requested()) {
        return;
      }
      if (update_scene_positions(pending.joint_positions,
                                 SceneUpdateMode::kBestEffort,
                                 pending.source_timestamp_ns)) {
        projected = true;
        terminal_scene_projection_retry_successes_total_.fetch_add(
            1, std::memory_order_relaxed);
        break;
      }
      if (attempt + 1 < kTerminalSceneProjectionRetryAttempts) {
        std::this_thread::sleep_for(kTerminalSceneProjectionRetryDelay);
      }
    }
    if (!projected && !stop_token.stop_requested()) {
      terminal_scene_projection_retry_exhausted_total_.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
}

void RoboPlanExecutionBackend::
    stop_terminal_scene_projection_retry_worker() noexcept {
  if (terminal_scene_projection_retry_thread_.joinable()) {
    // request_stop() wakes the token-aware wait through the jthread's
    // internal stop_callback; no manual notify is needed (and a manual
    // notify without the mutex would be the lost-wakeup pattern this
    // codebase has already fixed twice).
    terminal_scene_projection_retry_thread_.request_stop();
    terminal_scene_projection_retry_thread_.join();
  }
}

void RoboPlanExecutionBackend::execution_worker(
    std::stop_token st, core::TrajectoryDTO trajectory,
    std::uint64_t execution_generation,
    std::shared_ptr<core::detail::CoordinatedExecutionClock> common_clock,
    core::OperationParticipantId participant_id) {
  const double total_duration_s =
      trajectory.waypoints.empty()
          ? 0.0
          : trajectory.waypoints.back().time_from_start_s;
  std::vector<double> latest_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;
  bool completed_normally = false;

  if (st.stop_requested()) {
    observe_stop_qualification(detail::NativeStopQualificationEvent::
                                   kWorkerStopObservedBeforeTerminal);
    const auto stopped_snapshot_ns =
        publish_snapshot(core::ExecutionState::kStopped,
                         get_execution_snapshot().progress, latest_positions);
    if (!update_scene_positions(latest_positions, SceneUpdateMode::kBestEffort,
                                stopped_snapshot_ns)) {
      schedule_terminal_scene_projection_retry(latest_positions,
                                               stopped_snapshot_ns);
    }
    (void)execution_progress_publication_->invalidate_generation(
        execution_generation);
    publish_execution_inactive();
    return;
  }

  const auto initial_snapshot_ns =
      publish_snapshot(core::ExecutionState::kExecuting, 0.0, latest_positions);
  update_scene_positions(latest_positions, SceneUpdateMode::kBestEffort,
                         initial_snapshot_ns);

  int64_t last_tick_ns = clock_.now();
  double trajectory_time_s = 0.0;
  double applied_runtime_scale;
  {
    std::lock_guard lock(state_mutex_);
    applied_runtime_scale = state_.active_runtime_velocity_scale;
  }
  const auto &bounds = *trajectory.runtime_scale_bounds;
  core::detail::RuntimeScaleGovernor governor(
      applied_runtime_scale,
      {bounds.max_rate_per_s, bounds.max_rate_change_per_s2});
  core::TrajectorySampleResult sample_buffer;
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed);

  if (common_clock) {
    const auto origin = common_clock->start_and_wait(participant_id, clock_,
                                                     applied_runtime_scale, st);
    if (!origin && !st.stop_requested()) {
      const bool stopped_before_start = common_clock->stop_prevented_start();
      publish_snapshot(
          stopped_before_start ? core::ExecutionState::kStopped
                               : core::ExecutionState::kFailed,
          0.0, latest_positions,
          stopped_before_start
              ? std::nullopt
              : std::optional<core::ErrorDTO>{core::ErrorDTO{
                    core::kErrorExecutionRejected,
                    "Coordinated start was cancelled or inconsistent",
                    {{"reason", "coordinated_execution_start_failed"}}}});
      (void)execution_progress_publication_->invalidate_generation(
          execution_generation);
      publish_execution_inactive();
      return;
    }
    if (origin) {
      last_tick_ns = origin->timestamp_ns;
      governor = origin->governor;
    }
  }

  while (true) {
    const bool pause_requested = state_.pause_requested.load();

    if (st.stop_requested()) {
      observe_stop_qualification(detail::NativeStopQualificationEvent::
                                     kWorkerStopObservedBeforeTerminal);
      const auto stopped_snapshot_ns =
          publish_snapshot(core::ExecutionState::kStopped,
                           get_execution_snapshot().progress, latest_positions);
      if (!update_scene_positions(latest_positions,
                                  SceneUpdateMode::kBestEffort,
                                  stopped_snapshot_ns)) {
        schedule_terminal_scene_projection_retry(latest_positions,
                                                 stopped_snapshot_ns);
      }
      break;
    }

    if (pause_requested) {
      // A paused worker consumes scale intent without advancing trajectory
      // time. Its dormant register is the starting scale of a later Resume.
      {
        std::lock_guard scale_lock(runtime_velocity_scale_mutex_);
        if (controlled_stop_runtime_scale_freeze_depth_ == 0) {
          applied_runtime_scale =
              runtime_velocity_scale_.load(std::memory_order_acquire);
        }
        governor = core::detail::RuntimeScaleGovernor(
            applied_runtime_scale,
            {bounds.max_rate_per_s, bounds.max_rate_change_per_s2});
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        state_.active_runtime_velocity_scale = applied_runtime_scale;
        state_.active_runtime_scale_rate_per_s = 0.0;
        state_.active_runtime_scale_target = applied_runtime_scale;
      }
      publish_snapshot(core::ExecutionState::kPaused,
                       get_execution_snapshot().progress, latest_positions);
      std::unique_lock<std::mutex> lock(execution_mutex_);
      execution_cv_.wait(lock, [this, &st, applied_runtime_scale]() {
        if (st.stop_requested() || !state_.pause_requested.load())
          return true;
        std::lock_guard scale_lock(runtime_velocity_scale_mutex_);
        return controlled_stop_runtime_scale_freeze_depth_ == 0 &&
               runtime_velocity_scale_.load(std::memory_order_acquire) !=
                   applied_runtime_scale;
      });
      last_tick_ns = clock_.now();
      if (st.stop_requested() || state_.pause_requested.load()) {
        continue;
      }
      publish_snapshot(core::ExecutionState::kExecuting,
                       get_execution_snapshot().progress, latest_positions);
      continue;
    }

    std::int64_t now_ns;
    double available_target_scale;
    {
      // Snapshot intent at the same time anchor used for integration. A new
      // target arriving during sampling must wait until the next tick, never
      // be backdated into the interval that has already elapsed.
      std::lock_guard scale_lock(runtime_velocity_scale_mutex_);
      now_ns = clock_.now();
      available_target_scale =
          runtime_velocity_scale_.load(std::memory_order_acquire);
    }
    // Integrate the already-applied law, then accept the newest target for
    // future time. A wake must not retroactively change the preceding interval.
    const auto advanced =
        common_clock ? common_clock->sample_now()
                     : core::detail::sample_runtime_scale_time(
                           {trajectory_time_s, governor, last_tick_ns}, now_ns);
    if (!advanced) {
      publish_snapshot(
          core::ExecutionState::kFailed, get_execution_snapshot().progress,
          latest_positions,
          core::ErrorDTO{core::kErrorExecutionRejected,
                         "Native runtime time law became invalid",
                         {{"reason", "native_runtime_scale_advance_invalid"}}});
      break;
    }
    now_ns = advanced->timestamp_ns;
    last_tick_ns = now_ns;
    governor = advanced->governor;
    if (common_clock)
      available_target_scale = governor.target_scale();
    applied_runtime_scale = governor.scale();
    trajectory_time_s = std::min(total_duration_s, advanced->trajectory_time_s);
    (void)execution_progress_publication_->publish(
        execution_generation,
        static_cast<std::int64_t>(std::max(0.0, trajectory_time_s) * 1e9),
        now_ns, now_ns);

    const double progress =
        total_duration_s <= 1e-9
            ? 1.0
            : clamp_progress(trajectory_time_s / total_duration_s);
    {
      // Dispatch already validated this immutable curve. Reuse buffers and
      // skip an O(waypoints) structural scan on every controller tick.
      auto sample = core::sample_trajectory_prevalidated(
          trajectory, trajectory_time_s, sample_buffer);
      if (sample.ok()) {
        latest_positions = sample_buffer.positions_rad;
      } else {
        // Fail closed: an active execution trajectory that the sampler
        // rejects is structurally malformed. Continuing with an empty
        // position vector would publish a meaningless kExecuting
        // snapshot. Publish a structured Failed terminal and stop the
        // worker so the kernel surfaces the fault instead of silently
        // freezing the visual at an empty state.
        publish_snapshot(
            core::ExecutionState::kFailed, progress, latest_positions,
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "trajectory sampler rejected the active execution "
                "trajectory",
                {{"reason_code", "trajectory_sampler_error"},
                 {"sampler_error", sample.error().code},
                 {"time_s", std::to_string(trajectory_time_s)}}});
        break;
      }
    }
    observe_stop_qualification(
        detail::NativeStopQualificationEvent::kExecutionSampleReady);
    TimeLawSample time_law{trajectory_time_s, governor, now_ns,
                           available_target_scale};
    const auto snapshot_ns = publish_snapshot(
        state_.natural_completion_stop_requested.load(std::memory_order_acquire)
            ? core::ExecutionState::kStopping
            : core::ExecutionState::kExecuting,
        progress, latest_positions, std::nullopt, &time_law);
    if (!time_law.valid)
      break;
    update_scene_positions(latest_positions, SceneUpdateMode::kBestEffort,
                           snapshot_ns);

    if (progress >= 1.0) {
      const auto completed_positions =
          trajectory.waypoints.empty()
              ? latest_positions
              : trajectory.waypoints.back().positions_rad;
      const auto completed_snapshot_ns = publish_snapshot(
          core::ExecutionState::kCompleted, 1.0, completed_positions);
      if (!trajectory.waypoints.empty()) {
        update_scene_positions(completed_positions, SceneUpdateMode::kBlocking,
                               completed_snapshot_ns);
      }
      const auto terminal_progress_ns = clock_.now();
      (void)execution_progress_publication_->publish_terminal(
          execution_generation,
          static_cast<std::int64_t>(std::max(0.0, total_duration_s) * 1e9),
          terminal_progress_ns, terminal_progress_ns);
      completed_normally = true;
      break;
    }

    std::unique_lock<std::mutex> lock(execution_mutex_);
    execution_cv_.wait_for(
        lock, std::chrono::nanoseconds(config_.execution_tick_period_ns),
        [this, &st, coordinated = static_cast<bool>(common_clock),
         target_scale = governor.target_scale()]() {
          if (st.stop_requested() || state_.pause_requested.load())
            return true;
          if (coordinated)
            return false;
          std::lock_guard scale_lock(runtime_velocity_scale_mutex_);
          return controlled_stop_runtime_scale_freeze_depth_ == 0 &&
                 runtime_velocity_scale_.load(std::memory_order_acquire) !=
                     target_scale;
        });
  }

  if (!completed_normally) {
    (void)execution_progress_publication_->invalidate_generation(
        execution_generation);
  }
  publish_execution_inactive();
}

void RoboPlanExecutionBackend::request_execution_worker_stop() noexcept {
  {
    // The worker's paused wait observes these predicates under the same
    // mutex. Mutating them while holding it prevents a notify from landing
    // between predicate evaluation and the actual wait (a lost wake-up).
    std::lock_guard<std::mutex> lock(execution_mutex_);
    request_execution_worker_stop_locked();
  }
  execution_cv_.notify_all();
}

void RoboPlanExecutionBackend::request_execution_worker_stop_locked() noexcept {
  state_.pause_requested.store(false, std::memory_order_release);
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  if (worker_thread_.joinable())
    worker_thread_.request_stop();
}

bool RoboPlanExecutionBackend::issue_software_hard_stop(
    std::optional<std::uint64_t> expected_execution) noexcept {
  {
    std::lock_guard lock(execution_mutex_);
    if (expected_execution &&
        (*expected_execution == 0U ||
         last_execution_generation_.load(std::memory_order_acquire) !=
             *expected_execution)) {
      return false;
    }
    request_active_controlled_stop();
    request_execution_worker_stop_locked();
  }
  execution_cv_.notify_all();
  return true; // Idempotent containment request, never a claim of rest.
}

void RoboPlanExecutionBackend::publish_execution_inactive() noexcept {
  {
    // Every waiter uses execution_mutex_ with running as its predicate.
    // Publish the terminal predicate under that mutex, then wake waiters.
    std::lock_guard<std::mutex> lock(execution_mutex_);
    state_.running.store(false, std::memory_order_release);
    state_.pause_requested.store(false, std::memory_order_release);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
  }
  execution_cv_.notify_all();
}

core::ExecutionHandle RoboPlanExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO &trajectory) {
  return execute_with_clock(trajectory, {}, core::OperationParticipantId(""));
}

core::ExecutionHandle RoboPlanExecutionBackend::execute_with_clock(
    const core::TrajectoryDTO &trajectory,
    std::shared_ptr<core::detail::CoordinatedExecutionClock> common_clock,
    core::OperationParticipantId participant_id) {
  struct PendingStartGuard {
    std::shared_ptr<core::detail::CoordinatedExecutionClock> clock;
    ~PendingStartGuard() {
      if (clock)
        clock->cancel_pending_start();
    }
  } pending_start{common_clock};
  if (!trajectory.runtime_scale_bounds ||
      !core::EncodePreparedTrajectory(trajectory).ok()) {
    core::ExecutionSnapshot rejected;
    rejected.timestamp_ns = clock_.now();
    rejected.state = core::ExecutionState::kFailed;
    rejected.error = core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native execution requires a prepared bounded time law",
        {{"reason", "native_runtime_scale_preparation_missing_or_invalid"}}};
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [rejected]() { return rejected; }, 0};
  }
  // Retire the prior worker before publishing any state for the new
  // occurrence. Starting the replacement first lets the old worker's final
  // running=false publication race the new running=true fact and can strand a
  // Resume waiter. jthread move-assignment eventually joins too, but only
  // after the replacement has already started.
  if (worker_thread_.joinable()) {
    request_execution_worker_stop();
    worker_thread_.join();
  }

  {
    // Fence the prior occurrence before publishing any replacement state.
    // The new occurrence becomes addressable only when its worker is installed.
    std::lock_guard lock(execution_mutex_);
    last_execution_generation_.store(0U, std::memory_order_release);
  }
  const std::vector<double> initial_positions =
      trajectory.waypoints.empty() ? std::vector<double>{}
                                   : trajectory.waypoints.front().positions_rad;

  // Bind this execution to the exact software-hard generation that admitted
  // it. A controlled Stop that starts after a hard issue must not treat the
  // newly incremented generation as a fresh baseline and resurrect the
  // execution through a replacement trajectory.
  const auto execution_generation =
      next_execution_progress_generation_.fetch_add(1,
                                                    std::memory_order_acq_rel);
  execution_progress_publication_->invalidate();
  const auto progress_started_at_ns = clock_.now();
  const auto initial_progress = execution_progress_publication_->publish(
      execution_generation, 0, progress_started_at_ns, progress_started_at_ns);
  if (!initial_progress.published) {
    publish_snapshot(
        core::ExecutionState::kFailed, 0.0, initial_positions,
        core::ErrorDTO{core::kErrorExecutionRejected,
                       "Native execution progress authority could not be armed",
                       {{"reason", initial_progress.reason}}});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); },
                                 0};
  }
  {
    std::scoped_lock lock(runtime_velocity_scale_mutex_, state_mutex_);
    state_.active_trajectory =
        std::make_shared<const core::TrajectoryDTO>(trajectory);
    active_execution_clock_ = common_clock;
    state_.active_trajectory_generation = execution_generation;
    state_.original_path_cursor_valid = true;
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = std::max(
        0.0,
        std::min(1.0, runtime_velocity_scale_.load(std::memory_order_acquire)));
    state_.active_runtime_scale_rate_per_s = 0.0;
    state_.active_runtime_scale_target = state_.active_runtime_velocity_scale;
  }
  const auto initial_snapshot_ns = publish_snapshot(
      core::ExecutionState::kExecuting, 0.0, initial_positions);
  update_scene_positions(initial_positions, SceneUpdateMode::kBestEffort,
                         initial_snapshot_ns);

  {
    // Publish running and install its worker as one lifecycle transition.
    // Stop may advance its lock-free generation concurrently, but it cannot
    // miss the worker between observing running=true and requesting Stop.
    std::lock_guard<std::mutex> lock(execution_mutex_);
    active_execution_hard_stop_generation_.store(
        hard_stop_request_generation_.load(std::memory_order_acquire),
        std::memory_order_release);
    last_execution_generation_.store(execution_generation,
                                     std::memory_order_release);
    state_.running.store(true, std::memory_order_release);
    state_.pause_requested.store(false, std::memory_order_release);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    worker_thread_ = std::jthread(
        [this, trajectory_copy = core::TrajectoryDTO(trajectory),
         execution_generation, common_clock = std::move(common_clock),
         participant_id =
             std::move(participant_id)](std::stop_token st) mutable {
          execution_worker(st, std::move(trajectory_copy), execution_generation,
                           std::move(common_clock), std::move(participant_id));
        });
    pending_start.clock.reset();
  }

  return core::ExecutionHandle{
      [this](int64_t timeout_ns) {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        if (timeout_ns > 0) {
          return execution_cv_.wait_for(
              lock, std::chrono::nanoseconds(timeout_ns),
              [this]() { return !state_.running.load(); });
        }
        execution_cv_.wait(lock, [this]() { return !state_.running.load(); });
        return true;
      },
      [this]() { return get_execution_snapshot(); }, execution_generation};
}

core::Result<void>
RoboPlanExecutionBackend::sync_observed_state_before_planning() {
  std::vector<double> positions;
  auto controlled = state_channel_->current_controlled_state();
  if (!controlled.ok()) {
    auto error = controlled.error();
    error.details["reason"] = "observed_state_capture_failed";
    return core::Result<void>::Err(std::move(error));
  }
  positions.reserve(controlled.value().positions.size());
  for (const auto &position : controlled.value().positions) {
    const auto *angular = std::get_if<core::AngularPositionDTO>(&position);
    if (angular == nullptr) {
      return core::Result<void>::Err(
          core::ErrorDTO{core::kErrorExecutionRejected,
                         "Native motion state is not all-angular",
                         {{"reason", "observed_state_unit_mismatch"}}});
    }
    positions.push_back(angular->value_rad);
  }

  if (positions.size() != state_channel_->query_context().joint_names.size()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed-state shape does not match the qualified joint order",
        {{"reason", "observed_state_shape_mismatch"},
         {"expected_joint_count",
          std::to_string(state_channel_->query_context().joint_names.size())},
         {"observed_joint_count", std::to_string(positions.size())}}});
  }

  const auto now_ns = clock_.now();
  const auto state = get_execution_snapshot().state;
  const bool at_rest = state == core::ExecutionState::kIdle ||
                       state == core::ExecutionState::kPaused ||
                       state == core::ExecutionState::kStopped ||
                       state == core::ExecutionState::kCompleted ||
                       state == core::ExecutionState::kFailed ||
                       state == core::ExecutionState::kInterventionRequired;
  const std::span<const double> velocities =
      at_rest ? std::span<const double>(observed_at_rest_velocities_)
              : std::span<const double>{};
  auto published =
      publish_observed_state_authority(positions, velocities, now_ns);
  if (!published.applied()) {
    const std::string reason =
        published.error.has_value() &&
                published.error->details.contains("reason")
            ? published.error->details.at("reason")
            : "observed_state_publish_rejected";
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        "Native observed state was rejected by the publication lane",
        {{"reason", reason}}});
  }
  return core::Result<void>::Ok();
}

core::StopResultDTO RoboPlanExecutionBackend::stop() {
  // Advance first, even in the brief controlled-stop handoff where the old
  // worker is down and its replacement has not yet been rearmed. Otherwise a
  // hard Stop issued in that window can be rejected and then lost when the
  // controlled path starts moving again.
  request_active_controlled_stop();
  const bool execution_running = state_.running.load(std::memory_order_acquire);
  const bool controlled_stop_active =
      controlled_stop_activity_count_.load(std::memory_order_acquire) > 0U;
  if (!execution_running && !controlled_stop_active) {
    return core::StopResultDTO{false, core::ErrorDTO{core::kErrorInvalidRequest,
                                                     "No execution in flight",
                                                     {}}};
  }
  request_execution_worker_stop();
  return core::StopResultDTO{true, std::nullopt};
}

void RoboPlanExecutionBackend::request_active_controlled_stop() {
  // This is the independently progress-capable cancellation edge used by the
  // software-hard lane. Controlled-stop code snapshots the generation at
  // entry and observes a mismatch without sharing any nominal mutex, future,
  // provider call, or result-wait state.
  hard_stop_request_generation_.fetch_add(1U, std::memory_order_acq_rel);
}

// Controlled-stop capability + result surface.
//
// The native simulator supports `kSpringBackHold` and `kSoftOnPath`.
// `kHardDeceleration` is a ros2_control/JTC-only mode in the current
// scope. Declaring only supported modes keeps the kernel's arm-time
// gate honest: scopes requesting unsupported modes fail closed before
// dispatch.

core::MotionInterruptStopCapabilityDTO
RoboPlanExecutionBackend::get_motion_interrupt_stop_capability() const {
  core::MotionInterruptStopCapabilityDTO cap;
  cap.supported_stop_modes = {core::MotionInterruptStopMode::kSpringBackHold,
                              core::MotionInterruptStopMode::kSoftOnPath};
  cap.capability_profile_id = "roboplan_native_v2_soft_on_path";
  cap.hard_stop_level = "native_immediate_hold";
  cap.derivative_proof_level = "manyforge_core_native_path_ramp";
  cap.scaling_source_visibility = "native_runtime_scale";
  cap.scaling_source_probe_status = "not_applicable";
  cap.unsupported_reasons["hard_deceleration"] =
      "ros2_control_jtc_only_in_current_native_backend";
  core::MotionInterruptSoftOnPathPlannerConfigDTO soft_config;
  soft_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
  soft_config.sample_period_s = config_.soft_on_path_sample_period_s;
  soft_config.max_initial_path_deviation_rad =
      config_.soft_on_path_max_initial_path_deviation_rad;
  soft_config.max_initial_velocity_deviation_rad_s =
      config_.soft_on_path_max_initial_velocity_deviation_rad_s;
  soft_config.allow_model_velocity_fallback = true;
  // Jerk policy belongs to each prepared profile, not backend-wide defaults.
  soft_config.jerk_limits_enabled = false;
  cap.soft_on_path_planner_config = std::move(soft_config);
  return cap;
}

core::ParticipantStopStageResultDTO
RoboPlanExecutionBackend::TranslateStopStage(
    const core::ResolvedParticipantStopStageDTO &stage,
    const core::RemainingStopBudgetDTO &budget,
    const std::stop_token cancellation) {
  controlled_stop_activity_count_.fetch_add(1U, std::memory_order_acq_rel);
  struct StopActivity {
    std::atomic<std::uint32_t> &count;
    ~StopActivity() { count.fetch_sub(1U, std::memory_order_acq_rel); }
  } activity{controlled_stop_activity_count_};
  const auto started_at = clock_.now();
  if (cancellation.stop_requested()) {
    return superseded_stop_stage(stage);
  }
  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size() ||
      budget.stop_id != stage.stop_id ||
      budget.stop_generation != stage.stop_generation ||
      budget.remaining_outer_duration_ns <= 0) {
    return terminal_stop_stage_failure(
        stage, "resolved_stop_stage_budget_invalid",
        "native Stop stage has no valid remaining Kernel budget");
  }
  const auto retained_binding = binding_.load(std::memory_order_acquire);
  const auto &binding = retained_binding->value();
  const auto &path = *binding.control_path;
  std::vector<core::ResourceId> exact_resources;
  exact_resources.reserve(binding.resources.size());
  for (const auto &resource : binding.resources) {
    exact_resources.push_back(resource.resource_id);
  }
  std::sort(exact_resources.begin(), exact_resources.end());
  const auto &prepared_command =
      stage.participant_stop_plan.stage_commands[stage.stage_index];
  const auto &command = prepared_command.resolved_stage;
  const auto command_remaining =
      remaining_phase(budget, command.containment_command.command_phase_id);
  if (stage.resources != exact_resources ||
      stage.control_domain_id != path.control_domain_id ||
      stage.control_domain_revision != path.control_domain_revision ||
      stage.control_binding_id != path.control_binding_id ||
      stage.control_binding_revision != path.control_binding_revision ||
      stage.control_route_id != path.control_route_id ||
      stage.active_control_configuration_id !=
          path.active_control_configuration_id ||
      stage.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      command.command_target.provider_instance_id !=
          path.provider_instance_id ||
      command.command_target.provider_manifest_id !=
          path.provider_manifest_id ||
      command.command_target.provider_manifest_revision !=
          path.provider_manifest_revision ||
      command.command_target.runtime_unit_id != path.provider_runtime_unit_id ||
      command.command_target.provider_runtime_epoch !=
          path.provider_runtime_epoch ||
      !command_remaining.has_value() || *command_remaining <= 0) {
    return terminal_stop_stage_failure(stage,
                                       "resolved_stop_stage_binding_mismatch",
                                       "native Stop stage does not match this "
                                       "exact retained execution binding",
                                       clock_.now() - started_at);
  }

  std::optional<PreparedGoalRecord> prepared_goal;
  {
    std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
    if (prepared_goal_ && prepared_goal_->goal_id == stage.controller_goal_id) {
      prepared_goal = prepared_goal_;
    }
  }
  if (!prepared_goal.has_value() ||
      prepared_goal->generation != stage.controller_goal_generation) {
    return terminal_stop_stage_failure(
        stage, "resolved_stop_stage_dispatch_membership_unavailable",
        "native Stop cannot prove this stage belongs to an admitted prepared "
        "goal generation",
        clock_.now() - started_at);
  }
  const bool exact_participant =
      prepared_goal->operation_id == stage.operation_id &&
      prepared_goal->operation_revision == stage.operation_revision &&
      prepared_goal->participant_id == stage.participant_id &&
      prepared_goal->controllable_component_id ==
          stage.controllable_component_id &&
      prepared_goal->command_target_id == stage.command_target_id &&
      prepared_goal->capability_id == stage.capability_id &&
      prepared_goal->control_domain_id == stage.control_domain_id &&
      prepared_goal->control_domain_revision == stage.control_domain_revision &&
      prepared_goal->control_binding_id == stage.control_binding_id &&
      prepared_goal->control_binding_revision ==
          stage.control_binding_revision &&
      prepared_goal->controller_topology_id == stage.controller_topology_id &&
      prepared_goal->topology_revision == stage.topology_revision &&
      prepared_goal->control_route_id == stage.control_route_id &&
      prepared_goal->active_control_configuration_id ==
          stage.active_control_configuration_id &&
      prepared_goal->active_control_configuration_generation ==
          stage.active_control_configuration_generation &&
      prepared_goal->provider_runtime_epoch == stage.provider_runtime_epoch &&
      prepared_goal->resources == stage.resources &&
      prepared_goal->stop_evidence_policy_id == stage.stop_evidence_policy_id &&
      prepared_goal->stop_evidence_policy_revision ==
          stage.stop_evidence_policy_revision &&
      std::find(prepared_goal->stop_plans.begin(),
                prepared_goal->stop_plans.end(),
                stage.participant_stop_plan) != prepared_goal->stop_plans.end();
  if (!exact_participant) {
    return terminal_stop_stage_failure(
        stage, "resolved_stop_stage_dispatch_membership_mismatch",
        "native Stop stage differs from its admitted prepared goal",
        clock_.now() - started_at);
  }

  const bool controller_goal_cancel =
      command.containment_command.command_kind ==
      core::StopContainmentCommandKind::kControllerGoalCancel;
  const bool software_hard =
      command.containment_command.command_kind ==
          core::StopContainmentCommandKind::kRegistered &&
      command.containment_command.registered_command_kind_id.has_value() &&
      command.containment_command.registered_command_kind_id->value() ==
          "manyforge.stop.software_hard";
  const bool motion_controlled_stop =
      command.containment_command.command_kind ==
      core::StopContainmentCommandKind::kMotionControlledStop;
  if (!controller_goal_cancel && !motion_controlled_stop && !software_hard) {
    return terminal_stop_stage_failure(
        stage, "resolved_stop_command_not_supported",
        "native exact Stop port supports "
        "controlled arm Stop, controller-goal "
        "cancel, and retained software-hard Stop",
        clock_.now() - started_at);
  }

  std::optional<core::MotionInterruptStopProfileDTO> controlled_profile;
  if (motion_controlled_stop) {
    const auto &contract = command.containment_command.provider_contract_ref;
    if (!contract.has_value() ||
        contract->contract_schema_id.value() !=
            core::kMotionControlledStopContractSchemaId ||
        contract->contract_schema_revision.value() !=
            core::kMotionControlledStopContractSchemaRevision ||
        !prepared_command.validated_contract_parameters.has_value() ||
        !prepared_command.parameter_source.has_value()) {
      return terminal_stop_stage_failure(
          stage, "motion_controlled_stop_contract_invalid",
          "native controlled Stop has no exact prepared provider contract",
          clock_.now() - started_at);
    }
    std::string profile_id = "operator_soft_on_path";
    if (*prepared_command.parameter_source ==
        core::StopStageParameterSourceDTO::kParticipantProfileMapping) {
      if (!stage.participant_stop_plan.participant_stop_profile.has_value()) {
        return terminal_stop_stage_failure(
            stage, "motion_controlled_stop_profile_missing",
            "native controlled Stop has no prepared participant profile",
            clock_.now() - started_at);
      }
      profile_id =
          stage.participant_stop_plan.participant_stop_profile->profile_id;
    }
    auto decoded =
        core::map_motion_controlled_stop_contract_parameters_to_profile(
            *prepared_command.validated_contract_parameters, binding.resources,
            profile_id);
    if (!decoded.ok()) {
      return terminal_stop_stage_failure(
          stage, decoded.error().details.at("reason"), decoded.error().message,
          clock_.now() - started_at);
    }
    controlled_profile = std::move(decoded.value());
  }

  const auto dispatch_generation = prepared_goal->handle.execution_generation;
  const auto current_generation =
      last_execution_generation_.load(std::memory_order_acquire);
  const bool execution_running = state_.running.load(std::memory_order_acquire);
  // Stop acceptance is not atomic with the execution worker: this exact goal
  // may reach its accepted endpoint before the controlled stage arrives. When
  // the prepared profile allows natural completion, that is the same outcome
  // as the in-window natural-completion handoff below; fresh rest is still
  // proven after this observation. Any other terminal, a replaced/reset
  // generation, or a profile without that policy remains a failed stage.
  const bool completed_before_stage =
      motion_controlled_stop && !execution_running &&
      dispatch_generation != 0U && dispatch_generation == current_generation &&
      controlled_profile->allow_natural_completion_on_short_path &&
      execution_completed_naturally(dispatch_generation);
  if (dispatch_generation == 0U || dispatch_generation != current_generation ||
      (!execution_running && !software_hard && !completed_before_stage)) {
    auto failure = terminal_stop_stage_failure(
        stage, "prepared_goal_not_active",
        "native prepared goal no longer owns the active execution",
        clock_.now() - started_at);
    if (failure.error.has_value()) {
      failure.error->details["prepared_execution_generation"] =
          std::to_string(dispatch_generation);
      failure.error->details["current_execution_generation"] =
          std::to_string(current_generation);
      failure.error->details["current_execution_activity"] =
          execution_running ? "running" : "terminal_or_inactive";
      failure.error->details["current_execution_state"] =
          std::string(core::to_string(get_execution_snapshot().state));
    }
    return failure;
  }

  if (cancellation.stop_requested()) {
    return superseded_stop_stage(stage);
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
      0,
      {},
      std::nullopt};
  const auto fail_after_dispatch =
      [&](std::string reason, std::string message,
          std::map<std::string, std::string> details = {}) {
        result.outcome = core::ParticipantStopStageOutcomeDTO::kFailed;
        result.receiver_elapsed_duration_ns =
            std::max<std::int64_t>(0, clock_.now() - started_at);
        details = stop_stage_error_details(details, std::move(reason));
        message.erase(std::remove(message.begin(), message.end(), '\0'),
                      message.end());
        if (message.empty()) {
          message = "native Stop stage failed";
        } else if (message.size() > 1024U) {
          message.resize(1024U);
        }
        result.error =
            core::ErrorDTO{core::kErrorMotionInterruptBackendStopFailed,
                           std::move(message), std::move(details)};
        result.evidence_items.clear();
        result.evidence_items.reserve(command.evidence_requirements.size());
        for (const auto &requirement : command.evidence_requirements) {
          result.evidence_items.push_back(core::StopEvidenceItemDTO{
              requirement.requirement.evidence_item_id,
              requirement.requirement.evidence_kind,
              core::StopEvidenceItemStateDTO::kMissing,
              requirement.resolved_source, std::nullopt, std::nullopt,
              std::nullopt, std::nullopt, result.error});
        }
        return result;
      };

  std::optional<core::ControlledStopResultDTO> controlled_outcome;
  if (motion_controlled_stop) {
    const auto elapsed_ns =
        std::max<std::int64_t>(0, clock_.now() - started_at);
    const auto remaining_ns =
        std::max<std::int64_t>(0, *command_remaining - elapsed_ns);
    controlled_outcome =
        completed_before_stage
            ? natural_completion_before_stage(*controlled_profile, elapsed_ns)
            : execute_controlled_stop_until(
                  *controlled_profile, cancellation,
                  std::chrono::steady_clock::now() +
                      std::chrono::nanoseconds(remaining_ns));
    if (const auto invalid = core::controlled_stop_result_field_error(
            *controlled_profile, *controlled_outcome);
        invalid.has_value()) {
      return fail_after_dispatch(
          "motion_controlled_stop_result_invalid",
          "native controlled Stop returned an invalid result: " + *invalid);
    }
    if (!controlled_outcome->accepted) {
      result.attempt_disposition =
          core::StopStageAttemptDispositionDTO::kNotDispatched;
      return fail_after_dispatch(
          controlled_outcome->error.has_value() &&
                  controlled_outcome->error->details.contains("reason")
              ? controlled_outcome->error->details.at("reason")
              : "motion_controlled_stop_not_dispatched",
          controlled_outcome->error.has_value()
              ? controlled_outcome->error->message
              : "native controlled Stop was not accepted",
          controlled_outcome->error.has_value()
              ? controlled_outcome->error->details
              : std::map<std::string, std::string>{});
    }
    result.actual_provider_contract_mode = core::provider_contract_stop_mode(
        controlled_outcome->executed_stop_mode);
    if (controlled_outcome->executed_stop_mode !=
        controlled_profile->stop_mode) {
      result.recoverable_substitution_reason =
          controlled_outcome->fallback_reason;
    }
    if (!controlled_outcome->backend_stop_confirmed) {
      return fail_after_dispatch(
          controlled_outcome->error.has_value() &&
                  controlled_outcome->error->details.contains("reason")
              ? controlled_outcome->error->details.at("reason")
              : "motion_controlled_stop_not_confirmed",
          controlled_outcome->error.has_value()
              ? controlled_outcome->error->message
              : "native controlled Stop was not confirmed",
          controlled_outcome->error.has_value()
              ? controlled_outcome->error->details
              : std::map<std::string, std::string>{});
    }
  } else {
    core::StopResultDTO stopped;
    if (controller_goal_cancel) {
      // Gripper-like goal Stop cancels only this exact occurrence. It must not
      // advance the independently latched software-hard generation.
      request_execution_worker_stop();
      stopped = core::StopResultDTO{true, std::nullopt};
    } else {
      stopped = core::StopResultDTO{
          issue_software_hard_stop(dispatch_generation), std::nullopt};
    }
    if (!stopped.accepted) {
      return terminal_stop_stage_failure(
          stage,
          software_hard ? "software_hard_stop_not_dispatched"
                        : "controller_goal_cancel_not_dispatched",
          stopped.error.has_value() ? stopped.error->message
                                    : "native Stop command was not accepted",
          clock_.now() - started_at);
    }
  }

  // Anchor observations after the selected command has returned acceptance.
  // This deliberately excludes a terminal sample emitted before that return.
  const auto command_accepted_at_ns = clock_.now();
  const auto terminal_phase = std::min_element(
      command.evidence_requirements.begin(),
      command.evidence_requirements.end(),
      [&](const auto &left, const auto &right) {
        return remaining_phase(budget, left.requirement.phase_id).value_or(-1) <
               remaining_phase(budget, right.requirement.phase_id).value_or(-1);
      });
  if (terminal_phase == command.evidence_requirements.end()) {
    return fail_after_dispatch("stop_evidence_requirements_missing",
                               "native Stop stage has no evidence contract");
  }
  const auto evidence_remaining =
      remaining_phase(budget, terminal_phase->requirement.phase_id);
  if (!evidence_remaining.has_value() || *evidence_remaining <= 0 ||
      started_at >
          std::numeric_limits<std::int64_t>::max() - *evidence_remaining) {
    return fail_after_dispatch("stop_evidence_phase_expired",
                               "native Stop evidence phase is already expired");
  }
  const auto evidence_deadline = started_at + *evidence_remaining;
  const auto minimum_dwell_ns = std::accumulate(
      command.evidence_requirements.begin(),
      command.evidence_requirements.end(), std::int64_t{0},
      [](const std::int64_t current, const auto &requirement) {
        return std::max(current, requirement.requirement.observation_semantics
                                     .minimum_dwell_ns.value_or(0));
      });
  const auto same_execution = [&] {
    return last_execution_generation_.load(std::memory_order_acquire) ==
           dispatch_generation;
  };
  while (state_.running.load(std::memory_order_acquire) && same_execution() &&
         !cancellation.stop_requested() && clock_.now() < evidence_deadline) {
    std::unique_lock lock(execution_mutex_);
    execution_cv_.wait_for(
        lock, std::chrono::nanoseconds(config_.execution_tick_period_ns));
  }
  if (cancellation.stop_requested()) {
    return fail_after_dispatch("stop_stage_cancelled_after_dispatch",
                               "native Stop evidence wait was superseded");
  }
  if (!same_execution()) {
    return fail_after_dispatch("stop_evidence_execution_changed",
                               "The Stop execution was replaced or reset");
  }
  if (state_.running.load(std::memory_order_acquire)) {
    return fail_after_dispatch(
        "stop_evidence_phase_expired",
        "native execution did not settle in Stop budget");
  }

  result.evidence_items.reserve(command.evidence_requirements.size());
  std::optional<core::ExecutionSnapshot> terminal_snapshot;
  std::int64_t terminal_observed_at_ns = 0;
  std::optional<core::ObservedStateSnapshotDTO> rest_snapshot;
  const auto rest_freshness_ns = std::accumulate(
      command.evidence_requirements.begin(),
      command.evidence_requirements.end(),
      std::numeric_limits<std::int64_t>::max(),
      [](auto bound, const auto &row) {
        return row.requirement.evidence_kind ==
                       core::StopEvidenceRequirementKind::kMotionRest
                   ? std::min(
                         bound,
                         row.requirement.source_requirement.freshness_budget_ns)
                   : bound;
      });
  const auto exact_rest = [&](const core::ObservedStateSnapshotDTO &observed) {
    const auto now = clock_.now();
    if (!same_execution() || state_.running.load(std::memory_order_acquire) ||
        !observed.velocities.has_value() ||
        observed.resources != stage.resources ||
        observed.velocities->size() != observed.resources.size() ||
        observed.source_epoch != path.provider_runtime_epoch ||
        observed.sequence == 0U || observed.authority_generation == 0U ||
        now >= evidence_deadline ||
        observed.received_timestamp_ns <= command_accepted_at_ns ||
        observed.received_timestamp_ns > now ||
        now - observed.received_timestamp_ns > rest_freshness_ns ||
        observed.controllable_component_id != stage.controllable_component_id ||
        observed.command_target_id != stage.command_target_id ||
        observed.control_domain_id != stage.control_domain_id ||
        observed.control_domain_revision != stage.control_domain_revision ||
        observed.control_binding_id != stage.control_binding_id ||
        observed.control_binding_revision != stage.control_binding_revision ||
        observed.controller_topology_id != stage.controller_topology_id ||
        observed.topology_revision != stage.topology_revision ||
        observed.control_route_id != stage.control_route_id ||
        observed.active_control_configuration_id !=
            stage.active_control_configuration_id ||
        observed.active_control_configuration_generation !=
            stage.active_control_configuration_generation ||
        observed.provider_instance_id != path.provider_instance_id ||
        observed.provider_manifest_id != path.provider_manifest_id ||
        observed.provider_manifest_revision !=
            path.provider_manifest_revision ||
        observed.state_authority_id != path.state_authority_id ||
        observed.state_authority_class != path.state_authority_class) {
      return false;
    }
    for (std::size_t index = 0U; index < observed.resources.size(); ++index) {
      double threshold = 0.0;
      if (controlled_profile.has_value()) {
        const auto resource = std::find_if(
            binding.resources.begin(), binding.resources.end(),
            [&](const auto &candidate) {
              return candidate.resource_id == observed.resources[index];
            });
        if (resource == binding.resources.end() ||
            !resource->source_joint_name.has_value() ||
            !controlled_profile->per_joint_settle_velocity_threshold
                 .has_value()) {
          return false;
        }
        const auto configured =
            controlled_profile->per_joint_settle_velocity_threshold->find(
                *resource->source_joint_name);
        if (configured ==
            controlled_profile->per_joint_settle_velocity_threshold->end()) {
          return false;
        }
        const auto same_coordinate_kind =
            (*observed.velocities)[index].index() == configured->second.index();
        if (!same_coordinate_kind) {
          return false;
        }
        threshold = std::visit(
            [](const auto &value) {
              if constexpr (requires { value.value_rad_s; }) {
                return value.value_rad_s;
              } else {
                return value.value_m_s;
              }
            },
            configured->second);
      }
      const auto velocity = std::visit(
          [](const auto &value) {
            if constexpr (requires { value.value_rad_s; }) {
              return value.value_rad_s;
            } else {
              return value.value_m_s;
            }
          },
          (*observed.velocities)[index]);
      if (!std::isfinite(velocity) || std::abs(velocity) > threshold) {
        return false;
      }
    }
    return true;
  };
  std::int64_t rest_started_at = 0;
  std::int64_t rest_last_received_at = 0;
  std::uint64_t rest_first_sequence = 0U;
  std::uint64_t rest_last_sequence = 0U;
  std::uint64_t rest_authority_generation = 0U;
  bool rest_complete = false;
  if (std::any_of(command.evidence_requirements.begin(),
                  command.evidence_requirements.end(), [](const auto &row) {
                    return row.requirement.evidence_kind ==
                           core::StopEvidenceRequirementKind::kMotionRest;
                  })) {
    while (!cancellation.stop_requested() && clock_.now() < evidence_deadline) {
      auto observed = state_channel_->current_state(clock_.now());
      if (!same_execution()) {
        return fail_after_dispatch("stop_evidence_execution_changed",
                                   "The Stop execution was replaced or reset");
      }
      if (!observed.ok()) {
        return fail_after_dispatch("stop_evidence_source_unavailable",
                                   observed.error().message);
      }
      if (exact_rest(observed.value())) {
        const auto &sample = observed.value();
        if (rest_authority_generation != 0U &&
            sample.authority_generation != rest_authority_generation) {
          return fail_after_dispatch(
              "stop_evidence_source_changed",
              "Rest source generation changed during the Stop stage");
        }
        rest_authority_generation = sample.authority_generation;
        if (sample.sequence > rest_last_sequence &&
            sample.received_timestamp_ns >= rest_last_received_at) {
          if (rest_first_sequence == 0U ||
              sample.received_timestamp_ns - rest_last_received_at >
                  rest_freshness_ns) {
            rest_started_at = sample.received_timestamp_ns;
            rest_first_sequence = sample.sequence;
          }
          rest_last_received_at = sample.received_timestamp_ns;
          rest_last_sequence = sample.sequence;
          rest_snapshot = std::move(observed.value());
          if (rest_last_sequence > rest_first_sequence &&
              rest_last_received_at - rest_started_at >= minimum_dwell_ns) {
            rest_complete = true;
            break;
          }
        } else if (sample.sequence != rest_last_sequence ||
                   sample.received_timestamp_ns != rest_last_received_at) {
          return fail_after_dispatch(
              "stop_evidence_source_reordered",
              "Rest source ordering changed during the Stop stage");
        }
      } else {
        rest_first_sequence = 0U;
        rest_snapshot.reset();
      }
      std::unique_lock lock(execution_mutex_);
      execution_cv_.wait_for(
          lock, std::chrono::nanoseconds(config_.execution_tick_period_ns));
    }
    if (cancellation.stop_requested()) {
      return fail_after_dispatch(
          "stop_stage_cancelled_after_dispatch",
          "native Stop rest-evidence wait was superseded");
    }
    if (!rest_complete || !rest_snapshot.has_value()) {
      return fail_after_dispatch(
          "stop_evidence_not_at_rest",
          "native addressed state did not prove bounded exact rest");
    }
  }
  for (const auto &requirement : command.evidence_requirements) {
    std::uint64_t source_sequence = 0U;
    std::int64_t source_age_ns = 0;
    switch (requirement.requirement.evidence_kind) {
    case core::StopEvidenceRequirementKind::kControllerGoalTerminal: {
      if (requirement.requirement.observation_semantics.observation_kind !=
          core::StopEvidenceObservationKind::kPostCommandSample) {
        return fail_after_dispatch(
            "stop_goal_observation_unsupported",
            "Native goal terminality requires a current status observation");
      }
      if (!terminal_snapshot.has_value()) {
        // This adapter is the native goal-status authority. Read its current
        // status, preserving the original transition timestamp and sequence.
        // Observation time is separate; this never refreshes remote feedback
        // or supplies the independent motion-rest predicate.
        terminal_snapshot = prepared_goal->handle.get_snapshot();
        terminal_observed_at_ns = clock_.now();
      }
      const auto now_ns = clock_.now();
      const bool expected_terminal =
          software_hard
              ? terminal_snapshot->state == core::ExecutionState::kStopped ||
                    terminal_snapshot->state ==
                        core::ExecutionState::kCompleted ||
                    terminal_snapshot->state == core::ExecutionState::kFailed
          : controlled_outcome &&
                  controlled_outcome->completed_by_natural_completion
              ? terminal_snapshot->state == core::ExecutionState::kCompleted
              : terminal_snapshot->state == core::ExecutionState::kStopped;
      if (!expected_terminal || terminal_snapshot->sequence_number == 0U ||
          terminal_snapshot->timestamp_ns > terminal_observed_at_ns ||
          terminal_observed_at_ns < command_accepted_at_ns ||
          terminal_observed_at_ns > now_ns) {
        return fail_after_dispatch(
            "stop_goal_terminal_state_mismatch",
            "native goal did not reach the exact controlled Stop or "
            "natural-completion terminal state");
      }
      if (requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kControllerGoalStatus ||
          requirement.resolved_source.runtime_target.provider_runtime_epoch !=
              path.provider_runtime_epoch) {
        return fail_after_dispatch(
            "stop_evidence_source_mismatch",
            "native goal-terminal evidence differs from its retained source");
      }
      source_sequence = terminal_snapshot->sequence_number;
      source_age_ns = now_ns - terminal_observed_at_ns;
      result.controller_acknowledgement_sequence = source_sequence;
      break;
    }
    case core::StopEvidenceRequirementKind::kMotionRest: {
      if (!rest_snapshot.has_value()) {
        return fail_after_dispatch("stop_evidence_source_unavailable",
                                   "native rest observation is absent");
      }
      const auto now_ns = clock_.now();
      if (!exact_rest(*rest_snapshot)) {
        return fail_after_dispatch(
            "stop_evidence_not_at_rest",
            "native addressed state does not prove exact rest");
      }
      if (requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kBindingStateAuthority ||
          requirement.resolved_source.runtime_target.provider_runtime_epoch !=
              rest_snapshot->source_epoch ||
          requirement.resolved_source.state_authority_id !=
              std::optional(rest_snapshot->state_authority_id) ||
          requirement.resolved_source.state_authority_class !=
              std::optional(rest_snapshot->state_authority_class)) {
        return fail_after_dispatch(
            "stop_evidence_source_mismatch",
            "native rest evidence differs from its retained state authority");
      }
      source_sequence = rest_snapshot->sequence;
      source_age_ns = std::max<std::int64_t>(
          0, now_ns - rest_snapshot->received_timestamp_ns);
      break;
    }
    default:
      return fail_after_dispatch(
          "stop_evidence_kind_unsupported",
          "native execution does not support the retained Stop evidence kind");
    }
    if (source_age_ns >
        requirement.requirement.source_requirement.freshness_budget_ns) {
      return fail_after_dispatch("stop_evidence_source_stale",
                                 "native Stop evidence is stale");
    }
    core::JsonValueDTO evidence =
        controlled_outcome.has_value()
            ? core::motion_controlled_stop_evidence(
                  *controlled_profile, *controlled_outcome,
                  stage.stop_evidence_policy_revision.value(),
                  core::to_string(*prepared_command.parameter_source))
            : core::JsonValueDTO{true};
    if (requirement.requirement.evidence_kind ==
        core::StopEvidenceRequirementKind::kMotionRest) {
      if (!std::holds_alternative<core::JsonObjectDTO>(evidence.value))
        evidence = core::JsonValueDTO{core::JsonObjectDTO{}};
      std::get<core::JsonObjectDTO>(evidence.value)
          .emplace(
              "post_command_rest",
              core::JsonValueDTO{core::JsonObjectDTO{
                  {"first_sample_sequence",
                   core::JsonValueDTO{std::to_string(rest_first_sequence)}},
                  {"last_sample_sequence",
                   core::JsonValueDTO{std::to_string(rest_last_sequence)}},
                  {"first_sample_after_acceptance_ns",
                   core::JsonValueDTO{static_cast<double>(
                       rest_started_at - command_accepted_at_ns)}},
                  {"dwell_ns", core::JsonValueDTO{static_cast<double>(
                                   rest_last_received_at - rest_started_at)}},
              }});
    } else if (requirement.requirement.evidence_kind ==
               core::StopEvidenceRequirementKind::kControllerGoalTerminal) {
      if (!std::holds_alternative<core::JsonObjectDTO>(evidence.value))
        evidence = core::JsonValueDTO{core::JsonObjectDTO{}};
      std::get<core::JsonObjectDTO>(evidence.value)
          .emplace(
              "native_goal_observation",
              core::JsonValueDTO{core::JsonObjectDTO{
                  {"transition_age_ns", core::JsonValueDTO{static_cast<double>(
                                            terminal_observed_at_ns -
                                            terminal_snapshot->timestamp_ns)}},
                  {"observed_after_acceptance_ns",
                   core::JsonValueDTO{static_cast<double>(
                       terminal_observed_at_ns - command_accepted_at_ns)}},
              }});
    }
    result.evidence_items.push_back(core::StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id,
        requirement.requirement.evidence_kind,
        core::StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
        path.provider_runtime_epoch, source_sequence, source_age_ns,
        std::move(evidence), std::nullopt});
    result.source_epoch = path.provider_runtime_epoch;
    result.source_sequence =
        std::max(result.source_sequence.value_or(0U), source_sequence);
  }
  result.outcome = core::ParticipantStopStageOutcomeDTO::kSucceeded;
  result.receiver_elapsed_duration_ns =
      std::max<std::int64_t>(0, clock_.now() - started_at);
  bool execution_changed;
  bool expired;
  bool cancelled;
  {
    // Never hold this hard-issue lock over a status read, provider call, or
    // evidence construction. Replacement fences the old occurrence before
    // publishing new state, so this final check rejects any crossed read.
    std::lock_guard lifecycle_lock(execution_mutex_);
    execution_changed =
        !same_execution() || state_.running.load(std::memory_order_acquire);
    expired = clock_.now() >= evidence_deadline;
    cancelled = cancellation.stop_requested();
  }
  if (execution_changed)
    return fail_after_dispatch("stop_evidence_execution_changed",
                               "The Stop execution was replaced or reset");
  if (cancelled)
    return fail_after_dispatch("stop_stage_cancelled_after_dispatch",
                               "Native Stop evidence was superseded");
  if (expired)
    return fail_after_dispatch("stop_evidence_phase_expired",
                               "Native Stop evidence phase expired");
  return result;
}

core::ControlledStopResultDTO RoboPlanExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO &profile) {
  return execute_controlled_stop(profile, std::stop_token{});
}

core::ControlledStopResultDTO RoboPlanExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO &profile,
    std::stop_token stop_token) {
  return execute_controlled_stop_until(
      profile, stop_token,
      std::chrono::steady_clock::now() +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(
                  config_.soft_on_path_stop_traj_delta_s)));
}

core::ControlledStopResultDTO
RoboPlanExecutionBackend::execute_controlled_stop_until(
    const core::MotionInterruptStopProfileDTO &profile,
    std::stop_token stop_token,
    std::chrono::steady_clock::time_point command_deadline) {
  const auto retained_binding = binding_.load(std::memory_order_acquire);
  if (profile.stop_mode != core::MotionInterruptStopMode::kHardDeceleration &&
      (!profile.jerk_limits_enabled.has_value() ||
       (profile.jerk_limits_enabled == true &&
        (profile.per_joint_max_jerk.size() !=
             retained_binding->value().resources.size() ||
         std::any_of(profile.per_joint_max_jerk.begin(),
                     profile.per_joint_max_jerk.end(), [](const auto &row) {
                       return !std::holds_alternative<core::AngularJerkDTO>(
                           row.second);
                     }))))) {
    core::ControlledStopResultDTO rejected;
    rejected.executed_stop_mode = profile.stop_mode;
    rejected.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "Controlled Stop requires an explicit, fully prepared jerk profile",
        {{"reason", "controlled_stop_jerk_profile_not_prepared"}}};
    return rejected;
  }
  auto projected =
      core::project_motion_interrupt_stop_profile_to_compiled_resources(
          profile, retained_binding->value().resources);
  if (!projected.ok()) {
    core::ControlledStopResultDTO rejected;
    rejected.executed_stop_mode = profile.stop_mode;
    rejected.accepted = false;
    rejected.backend_stop_confirmed = false;
    auto details = projected.error().details;
    details.emplace("projection_error_code", projected.error().code);
    rejected.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "native controlled Stop could not project its portable joint scope: " +
            projected.error().message,
        std::move(details)};
    return rejected;
  }
  const auto hard_stop_request_generation =
      active_execution_hard_stop_generation_.load(std::memory_order_acquire);
  return execute_controlled_stop_impl(projected.value(), stop_token,
                                      hard_stop_request_generation,
                                      command_deadline);
}

core::ControlledStopResultDTO
RoboPlanExecutionBackend::execute_controlled_stop_impl(
    const core::MotionInterruptStopProfileDTO &profile,
    std::stop_token stop_token, std::uint64_t hard_stop_request_generation,
    std::chrono::steady_clock::time_point command_deadline) {
  controlled_stop_activity_count_.fetch_add(1U, std::memory_order_acq_rel);
  const auto shared_law_origin = begin_controlled_stop_runtime_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void *) {
        end_controlled_stop_runtime_scale_freeze();
        controlled_stop_activity_count_.fetch_sub(1U,
                                                  std::memory_order_acq_rel);
      });
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kNominalProviderEntry);

  // Defense in depth: the kernel's arm-time gate already rejects
  // unsupported modes, but a backend that has been mis-wired into
  // running the wrong mode at trip time MUST fail closed rather than
  // silently producing a hold for a path-preserving request.
  core::ControlledStopResultDTO result;
  result.executed_stop_mode = profile.stop_mode;
  const auto shared_command_budget_exceeded = [&] {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        core::kErrorMotionInterruptBackendStopFailed,
        "Coordinated Stop exhausted its remaining command budget",
        {{"reason", "coordinated_stop_command_budget_exceeded"},
         {"recommended_disposition", "hard_stop"}}};
    return result;
  };
  const auto controlled_stop_requested = [&]() {
    return stop_token.stop_requested() ||
           hard_stop_request_generation_.load(std::memory_order_acquire) !=
               hard_stop_request_generation;
  };

  const auto cancelled_result = [&](std::string phase,
                                    bool replay_lane_has_ceased = false) {
    // Before replacement replay, the real worker owns terminal publication;
    // never forge Stopped merely because cancellation was observed. During
    // synchronous replacement replay this caller is the command lane, so its
    // return is the actual cessation edge and may publish the terminal state.
    if (replay_lane_has_ceased) {
      const auto latest = get_execution_snapshot();
      const auto stopped_snapshot_ns =
          publish_snapshot(core::ExecutionState::kStopped, latest.progress,
                           latest.joint_positions_rad);
      if (!update_scene_positions(latest.joint_positions_rad,
                                  SceneUpdateMode::kBestEffort,
                                  stopped_snapshot_ns)) {
        schedule_terminal_scene_projection_retry(latest.joint_positions_rad,
                                                 stopped_snapshot_ns);
      }
      publish_execution_inactive();
    }
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "native controlled stop was preempted by a later intervention",
        {{"reason", "controlled_stop_cancelled"}, {"phase", std::move(phase)}}};
    return result;
  };
  std::stop_callback stop_notify(stop_token,
                                 [this]() { execution_cv_.notify_all(); });

  // Replay the same wall-time curve for both controlled modes. A slow
  // projection or scheduler wake must not cause a burst of overdue waypoints.
  // Sample at the actual current time, exactly as the ordinary native worker
  // does, and retain the trip timeline across planning and dispatch.
  const auto replay =
      [&](const core::TrajectoryDTO &trajectory,
          std::chrono::steady_clock::time_point origin,
          double lead_budget_s) -> std::optional<core::ErrorDTO> {
    const auto valid = core::validate_trajectory_for_sampling(trajectory);
    if (!valid.ok()) {
      publish_execution_inactive();
      return valid.error();
    }
    const double duration = trajectory.waypoints.back().time_from_start_s;
    core::TrajectorySampleResult sampled;
    bool first = true;
    while (!controlled_stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsed_s =
          std::chrono::duration<double>(now - origin).count();
      if (first && elapsed_s > lead_budget_s) {
        publish_execution_inactive();
        return stop_replay_elapsed(origin, lead_budget_s).error();
      }
      first = false;
      const double cursor = std::min(elapsed_s, duration);
      if (!core::sample_trajectory_prevalidated(trajectory, cursor, sampled)
               .ok()) {
        publish_execution_inactive();
        return core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "native controlled Stop could not sample its replacement",
            {{"reason", "native_stop_replacement_invalid"},
             {"recommended_disposition", "hard_stop"}}};
      }
      const auto snapshot_ns = publish_snapshot(
          core::ExecutionState::kStopping, clamp_progress(cursor / duration),
          sampled.positions_rad);
      {
        std::lock_guard velocity_lock(state_mutex_);
        state_.snapshot.joint_velocities_rad_s = sampled.velocities_rad_s;
      }
      update_scene_positions(sampled.positions_rad,
                             SceneUpdateMode::kBestEffort, snapshot_ns);
      if (cursor >= duration)
        return std::nullopt;
      std::unique_lock lock(execution_mutex_);
      execution_cv_.wait_for(
          lock, std::chrono::nanoseconds(config_.execution_tick_period_ns),
          controlled_stop_requested);
    }
    return std::nullopt; // Caller owns the existing cancellation/cessation
                         // result.
  };

  const auto await_natural_completion =
      [&](std::chrono::steady_clock::time_point origin) {
        state_.natural_completion_stop_requested.store(
            true, std::memory_order_release);
        execution_cv_.notify_all();
        const auto natural_scope =
            std::shared_ptr<void>(nullptr, [this](void *) {
              state_.natural_completion_stop_requested.store(
                  false, std::memory_order_release);
              execution_cv_.notify_all();
            });
        const auto started_ns = clock_.now();
        const double window =
            core::motion_interrupt_deceleration_time_or_default(profile);
        const auto deadline =
            origin +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    window + config_.soft_on_path_sample_period_s));
        observe_stop_qualification(
            detail::NativeStopQualificationEvent::kNominalResultWaitEnter,
            "natural_completion");
        bool completed = false;
        {
          std::unique_lock lock(execution_mutex_);
          while (!controlled_stop_requested() &&
                 !(completed = !state_.running.load())) {
            if (execution_cv_.wait_until(lock, deadline) ==
                std::cv_status::timeout) {
              completed = !state_.running.load();
              break;
            }
          }
        }
        if (controlled_stop_requested())
          return cancelled_result("natural_completion_wait");
        result.accepted = true;
        result.confirmation_latency_ns =
            std::max<std::int64_t>(0, clock_.now() - started_ns);
        result.diagnostics["natural_completion_basis"] =
            core::interrupt_structured_string(
                "accepted_trajectory_and_frozen_time_law");
        result.diagnostics["command_source"] =
            core::interrupt_structured_string(
                "native_simulated_natural_completion");
        result.diagnostics["requested_stop_mode"] =
            core::interrupt_structured_string(
                profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath
                    ? "soft_on_path"
                    : "spring_back_hold");
        result.stop_command_source =
            core::MotionInterruptStopCommandSource::kDiagnosticOther;
        if (!completed || get_execution_snapshot().state !=
                              core::ExecutionState::kCompleted) {
          result.backend_stop_confirmed = false;
          result.error = core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "Original motion did not complete within the frozen Stop window",
              {{"reason", "soft_on_path_natural_completion_timeout"},
               {"recommended_disposition", "hard_stop"}}};
          return result;
        }
        result.backend_stop_confirmed = true;
        result.completed_by_natural_completion = true;
        return result;
      };

  if (controlled_stop_requested()) {
    return cancelled_result("controlled_stop_entry");
  }

  if (shared_law_origin) {
    // Retargeting is frozen. A strictly later worker sample therefore belongs
    // to that final law, even if retarget and an older sample had equal clock
    // timestamps. Do not synthesize a measured pose or retain target history.
    std::unique_lock lock(state_mutex_);
    const bool ready =
        stop_snapshot_cv_.wait_until(lock, stop_token, command_deadline, [&] {
          return controlled_stop_requested() ||
                 state_.snapshot.state != core::ExecutionState::kExecuting ||
                 state_.snapshot.timestamp_ns > *shared_law_origin;
        });
    if (controlled_stop_requested())
      return cancelled_result("coordinated_stop_snapshot_wait");
    if (!ready) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          core::kErrorMotionInterruptBackendStopFailed,
          "Coordinated Stop did not receive a matching worker sample within "
          "its command budget",
          {{"reason", "coordinated_stop_snapshot_budget_exceeded"},
           {"recommended_disposition", "hard_stop"}}};
      return result;
    }
  }

  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath) {
    std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
    core::ExecutionSnapshot active_snapshot;
    double active_time_s = 0.0;
    double active_runtime_scale = 1.0;
    double active_scale_rate = 0.0;
    double active_scale_target = 1.0;
    observe_stop_qualification(
        detail::NativeStopQualificationEvent::kNominalAdapterLockAttempt);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      active_snapshot = state_.snapshot;
      active_trajectory = state_.active_trajectory;
      active_time_s = state_.active_trajectory_time_s;
      active_runtime_scale = state_.active_runtime_velocity_scale;
      active_scale_rate = state_.active_runtime_scale_rate_per_s;
      active_scale_target = state_.active_runtime_scale_target;
    }

    if (!active_trajectory || active_trajectory->waypoints.empty() ||
        active_snapshot.joint_positions_rad.empty()) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "native soft_on_path requires an active trajectory and observed "
          "state",
          {{"reason", "soft_on_path_missing_active_trajectory_or_state"},
           {"backend_capability_profile_id",
            "roboplan_native_v2_soft_on_path"}}};
      return result;
    }

    core::SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = active_trajectory->joint_names;
    initial.state.positions_rad = active_snapshot.joint_positions_rad;
    initial.trajectory_time_from_start_s = active_time_s;
    initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    initial.velocity_source = core::SoftOnPathStateSource::kModelDerived;
    initial.runtime_velocity_scale_at_trip = active_runtime_scale;
    initial.runtime_scale_rate_per_s_at_trip = active_scale_rate;
    initial.runtime_scale_target_at_trip = active_scale_target;
    initial.sample_age_ns =
        std::max<std::int64_t>(0, clock_.now() - active_snapshot.timestamp_ns);
    const auto stop_timeline_origin =
        std::chrono::steady_clock::now() -
        std::chrono::nanoseconds(initial.sample_age_ns);

    core::SoftOnPathPlannerConfig planner_config;
    planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
    planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
    planner_config.allow_model_velocity_fallback = true;
    planner_config.max_initial_path_deviation_rad =
        config_.soft_on_path_max_initial_path_deviation_rad;
    planner_config.max_initial_velocity_deviation_rad_s =
        config_.soft_on_path_max_initial_velocity_deviation_rad_s;
    planner_config.jerk_limits_enabled = profile.jerk_limits_enabled == true;
    if (planner_config.jerk_limits_enabled)
      for (const auto &[joint, jerk] : profile.per_joint_max_jerk)
        planner_config.per_joint_max_jerk_rad_s3.emplace(
            joint, std::get<core::AngularJerkDTO>(jerk).value_rad_s3);

    // The active curve was validated at dispatch. A short eligible tail needs
    // no Stop-time trajectory validation/projection scan or replacement build.
    if (core::natural_completion_fits_stop_window(*active_trajectory, profile,
                                                  initial, planner_config))
      return await_natural_completion(stop_timeline_origin);
    auto planned = core::plan_soft_on_path_stop(*active_trajectory, profile,
                                                initial, planner_config);
    if (!planned.ok()) {
      if (profile.allow_natural_completion_on_short_path &&
          core::soft_on_path_error_recommends_natural_completion(
              planned.error())) {
        return await_natural_completion(stop_timeline_origin);
      }
      if (core::soft_on_path_error_recommends_hard_stop(planned.error())) {
        // The planner could not prove a feasible on-path soft stop and
        // recommends a hard stop. The native backend has no hard_deceleration
        // mode (ros2_control/JTC-only); its hard-stop floor is the kernel's
        // emergency backend.stop() immediate hold. So REFUSE the soft stop here
        // (do NOT downgrade to a spring-back hold, which is slower than the
        // hard stop the situation demands) and let the kernel escalate to the
        // immediate hold. The backend-agnostic enriched kernel escalation event
        // carries the planner cause (code, reason, recommended_disposition) for
        // observability. Mirrors the ros2_control hard-stop disposition,
        // adapted to the native lane's stop-mode floor.
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = planned.error();
        return result;
      }
      const bool may_fallback = profile.recoverable_substitution_mode ==
                                core::MotionInterruptStopMode::kSpringBackHold;
      const auto recommended_fallback_it =
          planned.error().details.find("recommended_fallback");
      const bool planner_recommends_spring_back =
          recommended_fallback_it != planned.error().details.end() &&
          recommended_fallback_it->second == "spring_back_hold";
      if (may_fallback && planner_recommends_spring_back) {
        core::MotionInterruptStopProfileDTO fallback_profile = profile;
        fallback_profile.stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        fallback_profile.allow_natural_completion_on_short_path = false;
        fallback_profile.recoverable_substitution_mode.reset();
        observe_stop_qualification(
            detail::NativeStopQualificationEvent::kSubstitutionSelected,
            planned.error().details.count("reason") > 0
                ? planned.error().details.at("reason")
                : "soft_on_path_planner_rejected");
        auto fallback = execute_controlled_stop_impl(
            fallback_profile, stop_token, hard_stop_request_generation,
            command_deadline);
        fallback.executed_stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        fallback.fallback_reason = planned.error().details.count("reason") > 0
                                       ? planned.error().details.at("reason")
                                       : "soft_on_path_planner_rejected";
        fallback.diagnostics["requested_stop_mode"] =
            core::interrupt_structured_string("soft_on_path");
        fallback.diagnostics["fallback_reason"] =
            core::interrupt_structured_string(*fallback.fallback_reason);
        fallback.diagnostics["soft_on_path_planner_error"] =
            core::interrupt_structured_string(planned.error().message);
        return fallback;
      }
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = planned.error();
      return result;
    }

    const auto call_start_ns = clock_.now();
    if (controlled_stop_requested()) {
      return cancelled_result("soft_on_path_before_dispatch");
    }
    if (shared_law_origin &&
        std::chrono::steady_clock::now() >= command_deadline)
      return shared_command_budget_exceeded();
    observe_stop_qualification(
        detail::NativeStopQualificationEvent::kPhysicalDispatch,
        "soft_on_path");
    request_execution_worker_stop();
    observe_stop_qualification(
        detail::NativeStopQualificationEvent::kNominalResultWaitEnter,
        "soft_on_path");
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      const auto deadline =
          std::chrono::steady_clock::now() +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(controlled_stop_wait_timeout_s(
                  profile, config_.soft_on_path_sample_period_s)));
      bool stopped = false;
      while (!controlled_stop_requested()) {
        stopped = !state_.running.load();
        if (stopped) {
          break;
        }
        if (execution_cv_.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
          stopped = !state_.running.load();
          break;
        }
      }
      if (controlled_stop_requested()) {
        return cancelled_result("soft_on_path_worker_stop_wait");
      }
      if (!stopped) {
        result.accepted = true;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "worker did not yield to native soft_on_path before deadline",
            {{"reason", "soft_on_path_worker_stop_timeout"}}};
        return result;
      }
    }

    const auto &replacement = planned.value().replacement_trajectory;
    observe_stop_qualification(
        detail::NativeStopQualificationEvent::kNominalRearmAttempt,
        "soft_on_path");
    if (controlled_stop_requested()) {
      return cancelled_result("soft_on_path_before_rearm");
    }
    const auto elapsed = stop_replay_elapsed(stop_timeline_origin,
                                             planned.value().stop_traj_delta_s);
    if (!elapsed.ok()) {
      result.accepted =
          true; // The prior worker has already yielded; do not claim no effect.
      result.backend_stop_confirmed = false;
      result.error = elapsed.error();
      return result;
    }
    {
      // The replacement has its own elapsed time. Until it succeeds there is
      // no certified original-path endpoint to retain for continuation.
      std::lock_guard state_lock(state_mutex_);
      state_.original_path_cursor_valid = false;
    }
    state_.running.store(true);
    if (controlled_stop_requested()) {
      return cancelled_result("soft_on_path_rearm", true);
    }
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
    if (auto error = replay(replacement, stop_timeline_origin,
                            planned.value().stop_traj_delta_s)) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = std::move(*error);
      return result;
    }
    if (controlled_stop_requested())
      return cancelled_result("soft_on_path_replay", true);

    const auto call_end_ns = clock_.now();
    const auto &final_wp = replacement.waypoints.back();
    const auto stopped_snapshot_ns = publish_snapshot(
        core::ExecutionState::kStopped, 1.0, final_wp.positions_rad);
    {
      std::lock_guard<std::mutex> velocity_lock(state_mutex_);
      if (!final_wp.velocities_rad_s.empty())
        state_.snapshot.joint_velocities_rad_s = final_wp.velocities_rad_s;
      if (state_.active_trajectory == active_trajectory) {
        state_.active_trajectory_time_s =
            planned.value().forward_stop_path_time_s;
        state_.original_path_cursor_valid = true;
      }
    }
    if (!update_scene_positions(final_wp.positions_rad,
                                SceneUpdateMode::kBestEffort,
                                stopped_snapshot_ns)) {
      schedule_terminal_scene_projection_retry(final_wp.positions_rad,
                                               stopped_snapshot_ns);
    }
    publish_execution_inactive();

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.executed_stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kNativeSimulatedPathRamp;
    result.confirmation_latency_ns =
        std::max<std::int64_t>(0, call_end_ns - call_start_ns);
    result.path_deviation_max_rad = planned.value().path_deviation_max_rad;
    result.initial_path_deviation_rad =
        planned.value().initial_path_deviation_rad;
    result.stop_distance_rad = 0.0;
    if (!active_snapshot.joint_positions_rad.empty() &&
        final_wp.positions_rad.size() ==
            active_snapshot.joint_positions_rad.size()) {
      double sum = 0.0;
      for (size_t i = 0; i < final_wp.positions_rad.size(); ++i) {
        const double d =
            final_wp.positions_rad[i] - active_snapshot.joint_positions_rad[i];
        sum += d * d;
      }
      result.stop_distance_rad = std::sqrt(sum);
    }
    result.diagnostics = planned.value().diagnostics;
    result.diagnostics["command_source"] =
        core::interrupt_structured_string("native_simulated_path_ramp");
    result.diagnostics["runtime_scale_isolated"] =
        core::interrupt_structured_bool(true);
    result.diagnostics["runtime_scale_isolation"] =
        core::interrupt_structured_string("owned_scale_frozen_during_stop");
    result.diagnostics["timeline_authority_tier"] =
        core::interrupt_structured_string("owned_scale_asserted");
    result.diagnostics["soft_on_path_phase"] =
        core::interrupt_structured_string("4.5G_native");
    return result;
  }

  if (profile.stop_mode != core::MotionInterruptStopMode::kSpringBackHold) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "RoboPlanExecutionBackend supports only kSpringBackHold and "
        "kSoftOnPath in v2",
        {{"reason", "stop_mode_not_supported_by_backend"},
         {"backend_capability_profile_id", "roboplan_native_v2_soft_on_path"}}};
    return result;
  }

  std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
  core::ExecutionSnapshot active_snapshot;
  double active_time_s = 0.0;
  double active_runtime_scale = 1.0;
  double active_scale_rate = 0.0;
  double active_scale_target = 1.0;
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kNominalAdapterLockAttempt);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_snapshot = state_.snapshot;
    active_trajectory = state_.active_trajectory;
    active_time_s = state_.active_trajectory_time_s;
    active_runtime_scale = state_.active_runtime_velocity_scale;
    active_scale_rate = state_.active_runtime_scale_rate_per_s;
    active_scale_target = state_.active_runtime_scale_target;
  }

  // If no execution is in flight, there's nothing to preempt. The kernel never
  // calls this without an active trip, but defense in depth handles the case
  // anyway.
  if (!state_.running.load()) {
    if (active_snapshot.joint_positions_rad.empty()) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "execute_controlled_stop invoked with no execution in flight "
          "and no recorded observed state",
          {{"reason", "controlled_stop_no_observed_state"}}};
      return result;
    }
    // Already stopped with a recorded anchor: report immediate
    // confirmation. This collapses to the "spring-back to here" path
    // when the trip latch arrives right as the worker is winding down.
    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kNativeSimulatedStop;
    result.confirmation_latency_ns = 0;
    result.diagnostics["runtime_scale_isolation"] =
        core::interrupt_structured_string("owned_scale_frozen_during_stop");
    result.diagnostics["timeline_authority_tier"] =
        core::interrupt_structured_string("owned_scale_asserted");
    return result;
  }

  if (!active_trajectory || active_trajectory->waypoints.empty() ||
      active_snapshot.joint_positions_rad.empty()) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "native spring_back_hold requires an active trajectory and observed "
        "state",
        {{"reason", "spring_back_missing_active_trajectory_or_state"},
         {"backend_capability_profile_id", "roboplan_native_v2_soft_on_path"}}};
    return result;
  }

  core::SoftOnPathInitialStateDTO initial;
  initial.state.joint_names = active_trajectory->joint_names;
  initial.state.positions_rad = active_snapshot.joint_positions_rad;
  initial.trajectory_time_from_start_s = active_time_s;
  initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
  initial.velocity_source = core::SoftOnPathStateSource::kModelDerived;
  initial.runtime_velocity_scale_at_trip = active_runtime_scale;
  initial.runtime_scale_rate_per_s_at_trip = active_scale_rate;
  initial.runtime_scale_target_at_trip = active_scale_target;
  initial.sample_age_ns =
      std::max<std::int64_t>(0, clock_.now() - active_snapshot.timestamp_ns);
  const auto stop_timeline_origin =
      std::chrono::steady_clock::now() -
      std::chrono::nanoseconds(initial.sample_age_ns);

  core::SoftOnPathPlannerConfig planner_config;
  planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
  planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
  planner_config.allow_model_velocity_fallback = true;
  planner_config.max_initial_path_deviation_rad =
      config_.soft_on_path_max_initial_path_deviation_rad;
  planner_config.max_initial_velocity_deviation_rad_s =
      config_.soft_on_path_max_initial_velocity_deviation_rad_s;
  planner_config.jerk_limits_enabled = profile.jerk_limits_enabled == true;
  if (planner_config.jerk_limits_enabled)
    for (const auto &[joint, jerk] : profile.per_joint_max_jerk)
      planner_config.per_joint_max_jerk_rad_s3.emplace(
          joint, std::get<core::AngularJerkDTO>(jerk).value_rad_s3);

  core::MotionInterruptStopProfileDTO spring_profile = profile;
  spring_profile.deceleration_time_s =
      core::motion_interrupt_deceleration_time_or_default(profile);
  if (core::natural_completion_fits_stop_window(
          *active_trajectory, spring_profile, initial, planner_config))
    return await_natural_completion(stop_timeline_origin);
  auto planned = core::plan_spring_back_on_path_stop(
      *active_trajectory, spring_profile, initial, planner_config);
  if (!planned.ok()) {
    if (profile.allow_natural_completion_on_short_path &&
        core::soft_on_path_error_recommends_natural_completion(planned.error()))
      return await_natural_completion(stop_timeline_origin);
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = planned.error();
    return result;
  }

  const auto call_start_ns = clock_.now();
  if (controlled_stop_requested()) {
    return cancelled_result("spring_back_entry");
  }
  // Request the regular worker to stop, then replay the spring-back replacement
  // on the native command stream. This mirrors the ros2_control lane: the
  // controlled stop itself is a replacement trajectory, not cancel-first hold
  // behavior.
  if (shared_law_origin && std::chrono::steady_clock::now() >= command_deadline)
    return shared_command_budget_exceeded();
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kPhysicalDispatch,
      "spring_back_hold");
  request_execution_worker_stop();

  const auto wait_deadline_s = controlled_stop_wait_timeout_s(
      profile, config_.soft_on_path_sample_period_s);
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kNominalResultWaitEnter,
      "spring_back_hold");
  std::unique_lock<std::mutex> lock(execution_mutex_);
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(wait_deadline_s));
  bool stopped = false;
  while (!controlled_stop_requested()) {
    stopped = !state_.running.load();
    if (stopped) {
      break;
    }
    if (execution_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      stopped = !state_.running.load();
      break;
    }
  }
  lock.unlock();
  if (controlled_stop_requested()) {
    return cancelled_result("spring_back_wait");
  }

  const auto call_end_ns = clock_.now();
  result.confirmation_latency_ns =
      std::max<std::int64_t>(0, call_end_ns - call_start_ns);

  if (!stopped) {
    result.accepted = true; // command was accepted (worker was notified)
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "worker did not exit within the controlled-stop deadline",
        {{"reason", "backend_stop_not_confirmed"},
         {"deadline_s", std::to_string(wait_deadline_s)}}};
    return result;
  }

  const auto &replacement = planned.value().replacement_trajectory;
  observe_stop_qualification(
      detail::NativeStopQualificationEvent::kNominalRearmAttempt,
      "spring_back_hold");
  if (controlled_stop_requested()) {
    return cancelled_result("spring_back_before_rearm");
  }
  const auto elapsed = stop_replay_elapsed(stop_timeline_origin,
                                           planned.value().stop_traj_delta_s);
  if (!elapsed.ok()) {
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = elapsed.error();
    return result;
  }
  {
    std::lock_guard state_lock(state_mutex_);
    state_.original_path_cursor_valid = false;
  }
  state_.running.store(true);
  if (controlled_stop_requested()) {
    return cancelled_result("spring_back_rearm", true);
  }
  state_.natural_completion_stop_requested.store(false,
                                                 std::memory_order_release);
  if (auto error = replay(replacement, stop_timeline_origin,
                          planned.value().stop_traj_delta_s)) {
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = std::move(*error);
    return result;
  }
  if (controlled_stop_requested())
    return cancelled_result("spring_back_replay", true);

  const auto replay_end_ns = clock_.now();
  const auto &final_wp = replacement.waypoints.back();
  const auto stopped_snapshot_ns = publish_snapshot(
      core::ExecutionState::kStopped, 1.0, final_wp.positions_rad);
  {
    std::lock_guard<std::mutex> velocity_lock(state_mutex_);
    if (!final_wp.velocities_rad_s.empty())
      state_.snapshot.joint_velocities_rad_s = final_wp.velocities_rad_s;
    if (state_.active_trajectory == active_trajectory) {
      state_.active_trajectory_time_s = planned.value().command_path_time_s;
      state_.original_path_cursor_valid = true;
    }
  }
  if (!update_scene_positions(final_wp.positions_rad,
                              SceneUpdateMode::kBestEffort,
                              stopped_snapshot_ns)) {
    schedule_terminal_scene_projection_retry(final_wp.positions_rad,
                                             stopped_snapshot_ns);
  }
  publish_execution_inactive();

  result.accepted = true;
  result.backend_stop_confirmed = true;
  result.executed_stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  result.stop_command_source =
      core::MotionInterruptStopCommandSource::kNativeSimulatedPathRamp;
  result.confirmation_latency_ns =
      std::max<std::int64_t>(0, replay_end_ns - call_start_ns);
  result.path_deviation_max_rad = planned.value().path_deviation_max_rad;
  result.initial_path_deviation_rad =
      planned.value().initial_path_deviation_rad;
  result.stop_distance_rad = 0.0;
  if (!active_snapshot.joint_positions_rad.empty() &&
      final_wp.positions_rad.size() ==
          active_snapshot.joint_positions_rad.size()) {
    double sum = 0.0;
    for (size_t i = 0; i < final_wp.positions_rad.size(); ++i) {
      const double d =
          final_wp.positions_rad[i] - active_snapshot.joint_positions_rad[i];
      sum += d * d;
    }
    result.stop_distance_rad = std::sqrt(sum);
  }
  result.diagnostics = planned.value().diagnostics;
  result.diagnostics["command_source"] = core::interrupt_structured_string(
      "native_simulated_spring_back_path_ramp");
  result.diagnostics["runtime_scale_isolation"] =
      core::interrupt_structured_string("owned_scale_frozen_during_stop");
  result.diagnostics["timeline_authority_tier"] =
      core::interrupt_structured_string("owned_scale_asserted");
  return result;
}

core::PauseResultDTO RoboPlanExecutionBackend::pause() {
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!state_.running.load(std::memory_order_acquire)) {
      return core::PauseResultDTO{
          false, core::ErrorDTO{
                     core::kErrorInvalidRequest, "No execution in flight", {}}};
    }
    state_.pause_requested.store(true, std::memory_order_release);
    state_.natural_completion_stop_requested.store(false,
                                                   std::memory_order_release);
  }
  execution_cv_.notify_all();
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle
RoboPlanExecutionBackend::resume(const core::TrajectoryDTO &trajectory) {
  if (state_.running.load(std::memory_order_acquire)) {
    request_active_controlled_stop();
  }
  // execute_trajectory joins the prior worker before publishing replacement
  // state. This single retirement boundary is simpler than separately
  // waiting for running=false and then racing the still-returning worker.
  return execute_trajectory(trajectory);
}

core::ResetResultDTO RoboPlanExecutionBackend::reset() {
  const auto reset_timeout =
      std::chrono::duration<double>(config_.reset_completion_timeout_s);
  auto reset_positions = get_execution_snapshot().joint_positions_rad;

  execution_progress_publication_->invalidate();
  request_active_controlled_stop();
  request_execution_worker_stop();
  {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool stopped = execution_cv_.wait_for(
        lock, reset_timeout, [this]() { return !state_.running.load(); });
    if (!stopped) {
      publish_snapshot(
          core::ExecutionState::kInterventionRequired,
          get_execution_snapshot().progress,
          get_execution_snapshot().joint_positions_rad,
          core::ErrorDTO{
              core::kErrorResetFailed,
              "Native execution reset timed out waiting for worker completion",
              {{"requires_operator_intervention", "true"},
               {"reason_code", "reset_timeout"},
               {"phase", "execution_worker_reset"},
               {"timeout_s", seconds_string(reset_timeout)}}});
      return core::ResetResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorResetFailed,
              "Native execution reset timed out waiting for worker completion",
              {{"requires_operator_intervention", "true"},
               {"reason_code", "reset_timeout"},
               {"phase", "execution_worker_reset"},
               {"timeout_s", seconds_string(reset_timeout)}}}};
    }
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  {
    std::lock_guard lock(execution_mutex_);
    last_execution_generation_.store(0U, std::memory_order_release);
  }
  {
    std::scoped_lock lock(runtime_velocity_scale_mutex_, state_mutex_);
    state_.active_trajectory.reset();
    active_execution_clock_.reset();
    state_.active_trajectory_generation = 0U;
    state_.original_path_cursor_valid = false;
    state_.active_trajectory_time_s = 0.0;
    state_.active_runtime_velocity_scale = 1.0;
  }
  if (reset_positions.empty()) {
    auto controlled = state_channel_->current_controlled_state();
    if (controlled.ok()) {
      reset_positions.reserve(controlled.value().positions.size());
      for (const auto &position : controlled.value().positions) {
        const auto *angular = std::get_if<core::AngularPositionDTO>(&position);
        if (angular == nullptr || !std::isfinite(angular->value_rad)) {
          reset_positions.clear();
          break;
        }
        reset_positions.push_back(angular->value_rad);
      }
    }
  }
  publish_snapshot(core::ExecutionState::kIdle, 0.0, reset_positions);
  return core::ResetResultDTO{true, std::nullopt};
}

core::ExecutionSnapshot
RoboPlanExecutionBackend::get_execution_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_.snapshot;
}

bool RoboPlanExecutionBackend::execution_completed_naturally(
    const std::uint64_t execution_generation) const {
  // Only the worker's endpoint branch publishes Completed; replacement replay,
  // cancellation and faults publish Stopped/Failed. Dispatch and Reset change
  // the retained generation under this same lock.
  std::lock_guard<std::mutex> lock(state_mutex_);
  return execution_generation != 0U &&
         state_.active_trajectory_generation == execution_generation &&
         state_.snapshot.state == core::ExecutionState::kCompleted;
}

std::optional<double> RoboPlanExecutionBackend::stopped_trajectory_time_s(
    std::uint64_t execution_generation) const {
  std::lock_guard lock(state_mutex_);
  const auto phase = state_.snapshot.state;
  if (execution_generation == 0U ||
      controlled_stop_activity_count_.load(std::memory_order_acquire) != 0U ||
      execution_generation != state_.active_trajectory_generation ||
      !state_.original_path_cursor_valid || !state_.active_trajectory ||
      state_.active_trajectory->waypoints.empty() ||
      (phase != core::ExecutionState::kPaused &&
       phase != core::ExecutionState::kStopped &&
       phase != core::ExecutionState::kCompleted) ||
      !std::isfinite(state_.active_trajectory_time_s) ||
      state_.active_trajectory_time_s < 0.0 ||
      state_.active_trajectory_time_s >
          state_.active_trajectory->waypoints.back().time_from_start_s)
    return std::nullopt;
  // Completion publishes the exact final pose even for a sub-nanosecond
  // no-op whose first sampled clock interval has not advanced.
  return phase == core::ExecutionState::kCompleted
             ? state_.active_trajectory->waypoints.back().time_from_start_s
             : state_.active_trajectory_time_s;
}

void RoboPlanExecutionBackend::set_runtime_velocity_scale(double scale) {
  // ADR-0018: clamp + atomic store. Caller (kernel) already validated
  // finiteness, but defense-in-depth keeps the workers from ever reading
  // NaN/Inf via this path.
  if (!std::isfinite(scale))
    return;
  if (scale < 0.0)
    scale = 0.0;
  if (scale > 1.0)
    scale = 1.0;
  {
    std::scoped_lock lock(runtime_velocity_scale_mutex_, execution_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ > 0) {
      // M4: freeze the owned runtime scale during controlled stops. Slider/API
      // intent is applied after the stop so it cannot stretch, freeze, or
      // accelerate the commanded stop profile.
      deferred_runtime_velocity_scale_ = scale;
      return;
    }
    runtime_velocity_scale_.store(scale, std::memory_order_release);
  }
  // Wake the execution worker so a scale=0 hold or resume takes effect
  // promptly.
  execution_cv_.notify_all();
}

core::RuntimeScaleMechanismStatusDTO
RoboPlanExecutionBackend::runtime_velocity_scale_status() const {
  std::scoped_lock lock(runtime_velocity_scale_mutex_, state_mutex_);
  const bool active = state_.running.load(std::memory_order_acquire);
  auto target = runtime_velocity_scale_.load(std::memory_order_acquire);
  std::optional<core::CoordinatedRuntimeScaleCommandDTO> coordinated_command;
  if (active && !state_.pause_requested.load(std::memory_order_acquire) &&
      active_execution_clock_) {
    if (const auto shared = active_execution_clock_->target()) {
      target = shared->scale;
      coordinated_command = shared->command;
    }
  }
  // This is a real, synchronous observation of an in-process mechanism, not
  // a renewed timestamp on cached physical feedback. Do not couple its
  // freshness to motion frames or produce idle motion frames to keep it live.
  constexpr std::uint64_t maximum_sequence = (std::uint64_t{1} << 53U) - 1U;
  if (runtime_scale_observation_sequence_ == maximum_sequence) {
    return core::RuntimeScaleMechanismStatusDTO{
        true,
        false,
        0.0,
        0.0,
        0.0,
        maximum_sequence,
        clock_.now(),
        core::ErrorDTO{"runtime_scale_observation_exhausted",
                       "native scaler observation sequence exhausted",
                       {}}};
  }
  return core::RuntimeScaleMechanismStatusDTO{
      true,
      active,
      target,
      state_.active_runtime_velocity_scale,
      state_.active_trajectory_time_s,
      ++runtime_scale_observation_sequence_,
      clock_.now(),
      std::nullopt,
      std::move(coordinated_command)};
}

std::optional<std::int64_t>
RoboPlanExecutionBackend::begin_controlled_stop_runtime_scale_freeze() {
  std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
  ++controlled_stop_runtime_scale_freeze_depth_;
  // No prepared-membership or nominal adapter lock on this boundary. Workers
  // release the clock lock before taking the scale lock for publication.
  if (active_execution_clock_)
    return active_execution_clock_->freeze_retargeting();
  return std::nullopt;
}

bool RoboPlanExecutionBackend::end_controlled_stop_runtime_scale_freeze() {
  bool applied_deferred_scale = false;
  {
    std::scoped_lock lock(runtime_velocity_scale_mutex_, execution_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ <= 0) {
      return false;
    }
    --controlled_stop_runtime_scale_freeze_depth_;
    if (controlled_stop_runtime_scale_freeze_depth_ == 0 &&
        deferred_runtime_velocity_scale_.has_value()) {
      runtime_velocity_scale_.store(*deferred_runtime_velocity_scale_,
                                    std::memory_order_release);
      deferred_runtime_velocity_scale_.reset();
      applied_deferred_scale = true;
    }
  }
  if (applied_deferred_scale) {
    execution_cv_.notify_all();
  }
  return applied_deferred_scale;
}

} // namespace manyforge::roboplan_backend
