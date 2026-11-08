#include <algorithm>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/execution_sample_association.hpp>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/robot_at_target.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

std::string format(double value) { return std::to_string(value); }

/// The speed factor slews at most 4 per second.
constexpr double kMaxRuntimeScaleSlewPerS = 4.0;

/// `main`'s backend fault shape: a Core error code with the stable reason
/// token, kept also under `reason` for the Kernel's own readers.
ErrorDTO fault(ErrorCode code, std::string message, const std::string& reason,
               std::map<std::string, std::string> details = {}) {
  details["reason_code"] = reason;
  details["reason"] = reason;
  details["surface"] = "execution_port_participant";
  return ErrorDTO{std::string(to_string(code)), std::move(message), std::move(details)};
}

// One bounded diagnostic per hold transition; no per-tick logging.
void trace_scale_hold(const char* stage, std::uint64_t owner, const ExecutionGoalKey& goal,
                      double factor, double cursor, double rate) {
  const auto written = std::fprintf(stderr,
                                    "manyforge_moving_scale_hold stage=%s owner=%llu goal=%.160s "
                                    "factor=%.17g cursor_s=%.17g rate_per_s=%.17g\n",
                                    stage, static_cast<unsigned long long>(owner),
                                    goal.controller_goal_id.value().c_str(), factor, cursor, rate);
  static_cast<void>(written);
}

const char* refusal_name(ExecutionPortRefusal refusal) {
  switch (refusal) {
    case ExecutionPortRefusal::kStaleKey:
      return "stale_key";
    case ExecutionPortRefusal::kInvalidInput:
      return "invalid_input";
    case ExecutionPortRefusal::kUnsupported:
      return "unsupported";
    case ExecutionPortRefusal::kNotReady:
      return "not_ready";
  }
  return "unknown";
}

bool ready(const ExecutionReadinessDTO& readiness) {
  return readiness.controller_active && readiness.controller_bound && readiness.hardware_active;
}

bool finite(const std::vector<double>& values, std::size_t size) {
  return values.size() == size &&
         std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

/// `values` in lane joint order, reordered to `to`; nullopt when incomplete.
std::optional<std::vector<double>> reorder(const std::vector<double>& values,
                                           const std::vector<std::string>& from,
                                           const std::vector<std::string>& to) {
  if (values.size() != from.size()) return std::nullopt;
  std::vector<double> result;
  result.reserve(to.size());
  for (const auto& name : to) {
    const auto found = std::find(from.begin(), from.end(), name);
    if (found == from.end()) return std::nullopt;
    result.push_back(values[static_cast<std::size_t>(found - from.begin())]);
  }
  return result;
}

double duration_s(const TrajectoryDTO& trajectory) {
  return trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
}

bool same_replacement_source(const PreparedMotionReplacementSourceDTO& a,
                             const PreparedMotionReplacementSourceDTO& b) {
  const auto& x = a.progress;
  const auto& y = b.progress;
  return a.execution_id == b.execution_id && a.execution_generation == b.execution_generation &&
         a.goal == b.goal && a.binding_epoch == b.binding_epoch &&
         a.prepared_contract_digest == b.prepared_contract_digest &&
         a.controller_sample == b.controller_sample &&
         a.controller_period_ns == b.controller_period_ns && x.sequence == y.sequence &&
         x.execution_generation == y.execution_generation &&
         x.trajectory_time_ns == y.trajectory_time_ns && x.source_kind == y.source_kind &&
         x.authority_profile == y.authority_profile &&
         x.source_timestamp_ns == y.source_timestamp_ns &&
         x.received_timestamp_ns == y.received_timestamp_ns &&
         x.freshness_budget_ns == y.freshness_budget_ns && x.valid == y.valid &&
         x.terminal == y.terminal;
}

StopStageDispatchedGoalDTO stop_goal_for(const PreparedOperationParticipantDTO& participant,
                                         const ExecutionGoalKey& key,
                                         const TrajectoryDTO& trajectory) {
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  std::vector<PreparedParticipantStopPlanDTO> stop_plans{participant.controller().ordinary_stop_plan};
  if (participant.controller().fault_stop_plan) stop_plans.push_back(*participant.controller().fault_stop_plan);
  for (const auto& scope : motion.interrupt_scopes) stop_plans.push_back(scope.stop_plan);
  return StopStageDispatchedGoalDTO{key,
                                    trajectory,
                                    participant.operation_id,
                                    participant.operation_revision,
                                    participant.participant_id,
                                    participant.controllable_component_id,
                                    participant.command_target_id,
                                    participant.capability_id,
                                    participant.binding_identity.control_domain_id,
                                    participant.binding_identity.control_domain_revision,
                                    participant.binding_identity.controller.control_binding_id,
                                    participant.binding_identity.controller.control_binding_revision,
                                    participant.controller().controller_topology_id,
                                    participant.controller().topology_revision,
                                    participant.controller().control_route_id,
                                    participant.controller().active_control_configuration_id,
                                    participant.controller().active_control_configuration_generation,
                                    participant.controller().control_provider,
                                    participant.resources,
                                    participant.controller().stop_evidence_policy_id,
                                    participant.controller().stop_evidence_policy_revision,
                                    std::move(stop_plans)};
}

/// A Stop stage that names a goal this participant
/// never sent, with no goal of it live and no other Stop stage running, has
/// nothing to stop: contained, no further checks (no fresh sample, no rest).
/// The caller's dispatch fence keeps that goal from ever going out. The
/// evidence is that fence observation itself: `fence_sequence` (the
/// participant's Stop-stage count, nonzero) at age 0. nullopt only for a
/// stage outside its plan or naming another runtime epoch; the ordinary
/// stage then runs (and refuses as before).
std::optional<ParticipantStopStageResultDTO> no_goal_containment(
    const ResolvedParticipantStopStageDTO& stage, const IClock& clock,
    const TimePoint received_at, const std::uint64_t fence_sequence) {
  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size()) return std::nullopt;
  const auto& command = stage.participant_stop_plan.stage_commands[stage.stage_index];
  const auto& requirements = command.resolved_stage.evidence_requirements;
  if (requirements.empty() || fence_sequence == 0U) return std::nullopt;
  const auto epoch = command.resolved_stage.command_target.provider_runtime_epoch;
  ParticipantStopStageResultDTO result{stage,
                                       StopStageAttemptDispositionDTO::kDispatched,
                                       ParticipantStopStageOutcomeDTO::kSucceeded,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       epoch,
                                       fence_sequence,
                                       std::max<std::int64_t>(0, clock.now() - received_at),
                                       {},
                                       std::nullopt};
  for (const auto& requirement : requirements) {
    if (requirement.resolved_source.runtime_target.provider_runtime_epoch != epoch)
      return std::nullopt;
    if (requirement.requirement.evidence_kind ==
        StopEvidenceRequirementKind::kControllerGoalTerminal)
      result.controller_acknowledgement_sequence = fence_sequence;
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source, epoch, fence_sequence,
        0,
        JsonValueDTO{JsonObjectDTO{
            {"finish_basis", JsonValueDTO{std::string("no_goal_dispatched")}},
            {"fence_sequence", JsonValueDTO{std::to_string(fence_sequence)}}}},
        std::nullopt});
  }
  // A controlled stage reports its requested mode: nothing was substituted.
  if (command.validated_contract_parameters) {
    const auto mode = command.validated_contract_parameters->find("stop_mode");
    const auto* text = mode == command.validated_contract_parameters->end()
                           ? nullptr
                           : std::get_if<std::string>(&mode->second.value);
    for (const auto candidate :
         {MotionControlledStopMode::kSoftOnPath, MotionControlledStopMode::kSpringBackHold,
          MotionControlledStopMode::kHardDeceleration, MotionControlledStopMode::kSpringBackFast}) {
      if (text != nullptr && *text == to_string(candidate))
        result.actual_provider_contract_mode = candidate;
    }
  }
  return result;
}

}  // namespace

/// One dispatched goal and the policy state that follows it.
struct ExecutionPortParticipant::Execution {
  std::uint64_t generation{};
  ExecutionGoalKey key;
  /// The token of this goal's send (Dispatch, or a moving replacement's
  /// DispatchReplacement); `dispatch_pending` while that send runs unlocked.
  std::shared_ptr<ExecutionDispatchToken> dispatch_token;
  bool dispatch_pending{false};
  std::shared_ptr<GoalCancelAttempt> cancel_attempt{std::make_shared<GoalCancelAttempt>()};
  std::optional<double> scale_hold_until_cursor_s;
  TrajectoryDTO trajectory;
  std::shared_ptr<IExecutionPort> port;
  std::shared_ptr<ParticipantStopStageHelper> stop_helper;
  ExecutionBindingEpoch epoch{0U};
  /// The goal's sample association, shared with the Stop-stage helper.
  std::shared_ptr<ExecutionSampleAssociation> association;
  bool recognition_recorded{false};
  /// A controller cycle later than the lane margin; reported to the
  /// Kernel as a critical fault.
  std::optional<ErrorDTO> critical_fault;
  std::optional<TimePoint> tracking_breach_since;
  /// When the successful result was first observed (measured completion gate).
  std::optional<TimePoint> success_observed_at;
  /// A Stop stage was entered: the Kernel's Stop owns the motion from here on.
  bool stopping{false};
  std::optional<ParticipantStopStageOutcomeDTO> last_stage_outcome;
  std::optional<ErrorDTO> last_stage_error;
  bool terminal{false};
  ExecutionSnapshot snapshot;
  /// Original-path cursor retained at the end (continuation).
  std::optional<double> stopped_cursor_s;
  double cursor_s{0.0};
  // Same existing Stop join clock mapping, without a second progress authority.
  StopReplacementCursorMapDTO delivery_clock{};
  // The actual associated sample of the successful progress publication.
  // Its sequence belongs to progress_, not to a second observation owner.
  struct PublishedProgressSample {
    std::uint64_t sequence;
    ExecutionControllerSampleDTO sample;
  };
  std::optional<PublishedProgressSample> published_progress_sample;

  void RecordProgressSample(const ExecutionProgressPublishResult publication,
                            ExecutionControllerSampleDTO sample) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<
                  std::optional<PublishedProgressSample>>);
    published_progress_sample.reset();
    if (publication.published)
      published_progress_sample = PublishedProgressSample{publication.sequence, std::move(sample)};
  }
};

struct ExecutionPortParticipant::PendingDispatch {
  PendingDispatch(ExecutionGoalKey value, ContentDigest prepared_digest,
                  std::shared_ptr<ExecutionDispatchToken> dispatch_token)
      : key(std::move(value)), digest(std::move(prepared_digest)), token(std::move(dispatch_token)) {}
  ExecutionGoalKey key;
  ContentDigest digest;
  std::shared_ptr<ExecutionDispatchToken> token;
  std::promise<ExecutionHandle> result;
  std::shared_future<ExecutionHandle> completion{result.get_future().share()};
};

struct ExecutionPortParticipant::Replacement {
  Replacement(std::uint64_t id, const PreparedMotionReplacementSourceDTO& captured)
      : candidate_id(id), source(captured) {}
  std::uint64_t candidate_id{};
  PreparedMotionReplacementSourceDTO source;
  std::shared_ptr<Execution> execution;
  std::optional<StopStageDispatchedGoalDTO> stop_goal;
  std::optional<PreparedGoalRecord> prepared_goal;
  ExecutionGoalKey record_key;
  TrajectoryDTO record_trajectory;
  std::optional<ExecutionPortCommandResultDTO> command;
  std::optional<std::int64_t> submitted_at_ns;
  double source_cursor_s{};
  double divergence_cursor_s{};
  std::optional<std::int64_t> recognition_budget_ns;
};

ExecutionPortParticipant::ExecutionPortParticipant(
    RuntimeParticipantBinding binding, std::shared_ptr<IExecutionPort> port, const IClock& clock,
    IScheduler& scheduler, StopStageWaiter waiter,
    ExecutionPortParticipantStateAuthority state_authority, ExecutionPortParticipantConfig config)
    : clock_(clock),
      scheduler_(scheduler),
      waiter_(std::move(waiter)),
      state_authority_(std::move(state_authority)),
      config_(std::move(config)),
      // The controller's own cursor on every lane; a measured lane
      // publishes it under the measured-physical authority, whose cursor
      // expires after the freshness budget.
      progress_(config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? ExecutionProgressSourceKind::kMeasuredProjection
                    : ExecutionProgressSourceKind::kBackendCommanded,
                config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? ExecutionProgressAuthorityProfile::kMeasuredPhysical
                    : ExecutionProgressAuthorityProfile::kCommandedSimulation,
                config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? "execution_port_measured_progress_v1"
                    : "execution_port_commanded_progress_v1",
                config_.progress_freshness_budget_ns),
      binding_(std::move(binding)) {
  const auto non_negative = [](double value) { return std::isfinite(value) && value >= 0.0; };
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (!port || !waiter_ || state_authority_.publication == nullptr ||
      !state_authority_.sync_before_planning ||
      (config_.stop.trip_authority != StateAuthorityClass::kMeasured &&
       config_.stop.trip_authority != StateAuthorityClass::kCommanded) ||
      (measured && (!config_.final_tolerance_rad.has_value() ||
                    !non_negative(*config_.final_tolerance_rad) ||
                    !config_.start_tolerance_rad.has_value() ||
                    !non_negative(*config_.start_tolerance_rad) ||
                    config_.progress_freshness_budget_ns <= 0 ||
                    config_.completion_state_timeout_ns <= 0)) ||
      (config_.tracking.has_value() && (!non_negative(config_.tracking->tolerance_rad) ||
                                        !non_negative(config_.tracking->timeout_s))) ||
      !std::isfinite(config_.late_cycle_margin_s) || config_.late_cycle_margin_s <= 0.0 ||
      !std::isfinite(config_.initial_speed_factor) || config_.initial_speed_factor < 0.0 ||
      config_.initial_speed_factor > 1.0 || config_.progress_freshness_budget_ns < 0) {
    throw std::invalid_argument("ExecutionPortParticipant requires complete lane facts");
  }
  // The Stop-stage helper's resources are the binding's.
  config_.stop.resources = binding_.value().resources;
  target_scale_ = config_.initial_speed_factor;
  if (auto rebound = RebindPort(std::move(port)); !rebound.ok()) {
    throw std::invalid_argument(rebound.error().message);
  }
}

ExecutionPortParticipant::~ExecutionPortParticipant() {
  {
    std::lock_guard lock(scale_mutex_);
    if (scheduler_detach_complete_) return;
    ++ramp_generation_;
    if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
    ramp_handle_ = 0U;
  }
  scheduler_.drain(SchedulerPriority::ControllerCommand);
}

Result<void> ExecutionPortParticipant::DetachSchedulerForHostShutdown(
    const std::chrono::steady_clock::time_point deadline) {
  ScheduleHandle ramp_to_cancel = 0U;
  {
    std::lock_guard lock(scale_mutex_);
    if (scheduler_detach_complete_) return Result<void>::Ok();
    if (scheduler_detach_started_)
      return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
          "Participant scheduler detachment is pending", "scheduler_detach_pending"));
    scheduler_detach_started_ = true;
    ++ramp_generation_;
    ramp_to_cancel = ramp_handle_;
    ramp_handle_ = 0U;
    ramp_.reset();
  }
  if (ramp_to_cancel != 0U) scheduler_.cancel(ramp_to_cancel);
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = now < deadline
      ? std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
      : std::chrono::nanoseconds::zero();
  if (!scheduler_.drain_for(SchedulerPriority::ControllerCommand, remaining))
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
        "Participant scheduler did not drain before shutdown", "scheduler_detach_timeout"));
  {
    std::lock_guard lock(scale_mutex_);
    scheduler_detach_complete_ = true;
  }
  return Result<void>::Ok();
}

std::shared_ptr<ParticipantStopStageHelper> ExecutionPortParticipant::MakeHelper(
    IExecutionPort& port) {
  return std::make_shared<ParticipantStopStageHelper>(
      port, clock_, waiter_, [this, &port](auto deadline) { return FreezeSpeedFactor(port, deadline); }, config_.stop);
}

Result<void> ExecutionPortParticipant::RebindPort(std::shared_ptr<IExecutionPort> port) {
  return RebindPortImpl(std::move(port), false);
}

Result<void> ExecutionPortParticipant::RebindPortAfterOwnerRebuild(
    std::shared_ptr<IExecutionPort> port) {
  return RebindPortImpl(std::move(port), true);
}

struct ExecutionPortParticipant::OwnerRebindStage final {
  ExecutionPortParticipant* owner{};
  std::shared_ptr<IExecutionPort> port;
  std::shared_ptr<IExecutionPort> previous_port;
  std::shared_ptr<ParticipantStopStageHelper> helper;
  ExecutionLaneFactsDTO lane;
  std::unique_lock<std::mutex> participant_lock;
  std::unique_lock<std::mutex> scale_lock;
  std::optional<std::uint64_t> execution_id;
  std::optional<std::uint64_t> unknown_candidate;
  bool unknown_execution{false};
  bool committed{false};
  bool timer_retired{false};
};

Result<std::shared_ptr<ExecutionPortParticipant::OwnerRebindStage>>
ExecutionPortParticipant::StageOwnerRebind(std::shared_ptr<IExecutionPort> port) {
  using StageResult = Result<std::shared_ptr<OwnerRebindStage>>;
  if (!port) return StageResult::Err(
      fault(ErrorCode::kExecutionRejected, "A rebinding requires its port instance", "execution_port_missing"));
  auto stage = std::make_shared<OwnerRebindStage>();
  stage->owner = this;
  stage->port = std::move(port);
  stage->lane = stage->port->ReadLaneFacts();
  stage->helper = MakeHelper(*stage->port);
  {
    std::lock_guard lock(mutex_);
    stage->previous_port = port_;
  }
  return StageResult::Ok(std::move(stage));
}

Result<void> ExecutionPortParticipant::RetireOwnerRebindTimer(
    const std::shared_ptr<OwnerRebindStage>& stage) {
  if (!stage || stage->owner != this || stage->committed || stage->participant_lock.owns_lock())
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
        "The staged port rebind is no longer current", "execution_port_rebind_busy"));
  // The host has already verified the exact old owner and closed its SDK gate.
  // Keep the handle until cancel returns; an exception leaves it owned here.
  std::lock_guard lock(mutex_);
  if (port_ != stage->previous_port)
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
        "The staged port rebind is no longer current", "execution_port_rebind_busy"));
  std::lock_guard scale_lock(scale_mutex_);
  if (scheduler_detach_started_)
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
        "Participant scheduler is detached", "scheduler_not_ready"));
  if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
  ++ramp_generation_;
  ramp_handle_ = 0U;
  ramp_.reset();
  stage->timer_retired = true;
  return Result<void>::Ok();
}

Result<void> ExecutionPortParticipant::LockOwnerRebind(
    const std::shared_ptr<OwnerRebindStage>& stage) {
  const auto refuse = [](std::string reason, std::string message) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected, std::move(message), std::move(reason)));
  };
  if (!stage || stage->owner != this || stage->committed || stage->participant_lock.owns_lock())
    return refuse("execution_port_rebind_busy", "The staged port rebind is no longer current");
  stage->participant_lock = std::unique_lock<std::mutex>(mutex_);
  try {
    const auto& lane = stage->lane;
    const auto& resources = binding_.value().resources;
    if (lane.joint_names.size() != resources.size() ||
        !std::all_of(lane.joint_names.begin(), lane.joint_names.end(),
                     [&](const auto& name) {
                       return std::count(lane.joint_names.begin(), lane.joint_names.end(), name) ==
                           std::count_if(resources.begin(), resources.end(), [&](const auto& resource) {
                             return name == resource.compiled_resource_name;
                           });
                     }) ||
        (lane.supports_scale && !(lane.controller_update_rate_hz > 0.0))) {
      UnlockOwnerRebind(stage);
      return refuse("execution_port_lane_facts_mismatch",
                    "The port's lane facts do not cover the participant binding");
    }
    if (port_ != stage->previous_port) {
      UnlockOwnerRebind(stage);
      return refuse("execution_port_rebind_busy", "The staged port rebind is no longer current");
    }
    bool retained_unknown_replacement = false;
    if (execution_ && execution_->terminal &&
        execution_->snapshot.state == ExecutionState::kInterventionRequired && replacement_ &&
        replacement_->command && replacement_->command->dispatch != ExecutionPortDispatch::kNotDispatched &&
        !replacement_->execution->terminal) {
      const auto goal = port_->ReadGoal(replacement_->execution->key);
      retained_unknown_replacement = goal && goal->key == replacement_->execution->key &&
          goal->binding_epoch == replacement_->source.binding_epoch &&
          goal->phase == ExecutionGoalPhase::kTerminal && goal->result == ExecutionGoalResult::kEndedUnknown;
    }
    if ((execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U ||
        (replacement_ && replacement_->command &&
         replacement_->command->dispatch != ExecutionPortDispatch::kNotDispatched &&
         !replacement_->execution->terminal && !retained_unknown_replacement)) {
      UnlockOwnerRebind(stage);
      return refuse("execution_port_rebind_busy", "A port cannot be rebound while motion or a Stop is in flight");
    }
    stage->execution_id = prepared_membership_.active_execution_id();
    stage->unknown_candidate.reset();
    stage->unknown_execution = stage->execution_id && execution_ && execution_->terminal &&
        execution_->snapshot.state == ExecutionState::kInterventionRequired;
    if (retained_unknown_replacement) stage->unknown_candidate = replacement_->candidate_id;
    stage->scale_lock = std::unique_lock<std::mutex>(scale_mutex_);
    if (!stage->timer_retired || ramp_handle_ != 0U || ramp_) {
      UnlockOwnerRebind(stage);
      return refuse("execution_port_rebind_busy", "The old owner timer is not retired");
    }
    return Result<void>::Ok();
  } catch (...) {
    UnlockOwnerRebind(stage);
    throw;
  }
}

bool ExecutionPortParticipant::LockedOwnerRebindNeedsMotionProof(
    const std::shared_ptr<OwnerRebindStage>& stage) const noexcept {
  if (!stage || stage->owner != this || !stage->participant_lock.owns_lock() ||
      !stage->scale_lock.owns_lock()) std::terminate();
  return stage->unknown_execution;
}

void ExecutionPortParticipant::CommitLockedOwnerRebind(
    const std::shared_ptr<OwnerRebindStage>& stage) noexcept {
  // The owning host must hold this guard through every companion swap.
  if (!stage || stage->owner != this || stage->committed ||
      !stage->participant_lock.owns_lock() || !stage->scale_lock.owns_lock()) std::terminate();
  static_assert(std::is_nothrow_swappable_v<ExecutionLaneFactsDTO>);
  last_sent_scale_.reset();
  scale_frozen_ = false;
  if (stage->unknown_execution) {
    if (!owner_rebuild_observation_ || owner_rebuild_observation_->first != *stage->execution_id) {
      owner_rebuild_observation_ = std::make_pair(*stage->execution_id, state_authority_.publication->next_sequence());
      retired_owner_rest_qualified_ = false;
      retired_unknown_candidate_.reset();
    }
    if (!retired_owner_helper_) {
      retired_owner_port_ = port_;
      retired_owner_helper_ = helper_;
    }
    if (stage->unknown_candidate) retired_unknown_candidate_ = stage->unknown_candidate;
  } else {
    // No retained unknown motion needs a new standstill proof. Do not carry a
    // marker from an unrelated earlier owner into a known or idle execution.
    owner_rebuild_observation_.reset();
    retired_owner_rest_qualified_ = false;
    retired_unknown_candidate_.reset();
  }
  port_.swap(stage->port);
  helper_.swap(stage->helper);
  using std::swap;
  swap(lane_, stage->lane);
  timing_ = {};
  stage->committed = true;
}

void ExecutionPortParticipant::UnlockOwnerRebind(
    const std::shared_ptr<OwnerRebindStage>& stage) noexcept {
  if (!stage || stage->owner != this) return;
  if (stage->scale_lock.owns_lock()) stage->scale_lock.unlock();
  if (stage->participant_lock.owns_lock()) stage->participant_lock.unlock();
}

Result<void> ExecutionPortParticipant::RebindPortImpl(std::shared_ptr<IExecutionPort> port,
                                                       const bool owner_rebuilt) {
  const auto refuse = [](std::string reason, std::string message) {
    return Result<void>::Err(
        fault(ErrorCode::kExecutionRejected, std::move(message), std::move(reason)));
  };
  if (!port) return refuse("execution_port_missing", "A rebinding requires its port instance");
  const auto lane = port->ReadLaneFacts();
  std::vector<std::string> compiled;
  for (const auto& resource : binding_.value().resources) {
    compiled.push_back(resource.compiled_resource_name);
  }
  if (lane.joint_names.size() != compiled.size() ||
      !std::is_permutation(lane.joint_names.begin(), lane.joint_names.end(), compiled.begin()) ||
      (lane.supports_scale && !(lane.controller_update_rate_hz > 0.0))) {
    return refuse("execution_port_lane_facts_mismatch",
                  "The port's lane facts do not cover the participant binding");
  }
  std::lock_guard lock(mutex_);
  bool retained_unknown_replacement = false;
  if (owner_rebuilt && execution_ && execution_->terminal &&
      execution_->snapshot.state == ExecutionState::kInterventionRequired && replacement_ &&
      replacement_->command &&
      replacement_->command->dispatch != ExecutionPortDispatch::kNotDispatched &&
      !replacement_->execution->terminal) {
    const auto goal = port_->ReadGoal(replacement_->execution->key);
    retained_unknown_replacement = goal && goal->key == replacement_->execution->key &&
        goal->binding_epoch == replacement_->source.binding_epoch &&
        goal->phase == ExecutionGoalPhase::kTerminal &&
        goal->result == ExecutionGoalResult::kEndedUnknown;
  }
  if ((execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U ||
      (replacement_ && replacement_->command &&
       replacement_->command->dispatch != ExecutionPortDispatch::kNotDispatched &&
       !replacement_->execution->terminal && !retained_unknown_replacement)) {
    return refuse("execution_port_rebind_busy",
                  "A port cannot be rebound while motion or a Stop is in flight");
  }
  const auto id = prepared_membership_.active_execution_id();
  const bool unknown_execution = owner_rebuilt && id && execution_ && execution_->terminal &&
                                 execution_->snapshot.state == ExecutionState::kInterventionRequired;
  auto helper = MakeHelper(*port);
  {
    std::lock_guard scale_lock(scale_mutex_);
    ++ramp_generation_;
    if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
    ramp_handle_ = 0U;
    ramp_.reset();
    // A new binding epoch starts at the configured initial factor.
    last_sent_scale_.reset();
    scale_frozen_ = false;
  }
  if (unknown_execution) {
    if (!owner_rebuild_observation_ || owner_rebuild_observation_->first != *id) {
      owner_rebuild_observation_ =
          std::make_pair(*id, state_authority_.publication->next_sequence());
      retired_owner_rest_qualified_ = false;
      retired_unknown_candidate_.reset();
    }
    if (!retired_owner_helper_) {
      retired_owner_port_ = port_;
      retired_owner_helper_ = helper_;
    }
    if (retained_unknown_replacement)
      retired_unknown_candidate_ = replacement_->candidate_id;
  } else if (owner_rebuilt && !id) {
    owner_rebuild_observation_ =
        std::make_pair(0U, state_authority_.publication->next_sequence());
    retired_owner_rest_qualified_ = false;
  }
  port_ = std::move(port);
  helper_ = std::move(helper);
  lane_ = lane;
  scale_availability_view_.store(
      std::make_shared<const ScaleAvailabilityView>(ScaleAvailabilityView{port_, lane_.supports_scale}),
      std::memory_order_release);
  timing_ = {};
  return Result<void>::Ok();
}

std::optional<std::pair<std::uint64_t, std::uint64_t>>
ExecutionPortParticipant::owner_rebuild_observation() const {
  std::lock_guard lock(mutex_);
  return owner_rebuild_observation_;
}

void ExecutionPortParticipant::record_owner_rebuild_rest(
    const std::uint64_t execution_id) noexcept {
  std::lock_guard lock(mutex_);
  if (owner_rebuild_observation_ && owner_rebuild_observation_->first == execution_id)
    retired_owner_rest_qualified_ = true;
}

bool ExecutionPortParticipant::owner_rebuild_unknown_candidate(
    const std::uint64_t source_id, const std::uint64_t candidate_id) const {
  std::lock_guard lock(mutex_);
  return owner_rebuild_observation_ && owner_rebuild_observation_->first == source_id &&
         retired_owner_rest_qualified_ && retired_unknown_candidate_ == candidate_id &&
         stop_stages_in_flight_ == 0U;
}

Result<void> ExecutionPortParticipant::RebindRoute(
    RuntimeParticipantBinding binding, std::vector<RuntimeParticipantBinding> program_aliases) {
  std::lock_guard lock(mutex_);
  if (prepared_membership_.active_execution_id() || (execution_ && !execution_->terminal) ||
      stop_stages_in_flight_ != 0U) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Route publication cannot rebind an active participant",
                                   "execution_port_route_rebind_busy"));
  }
  if (binding.value().resources != binding_.value().resources ||
      program_aliases.size() > 64U ||
      std::any_of(program_aliases.begin(), program_aliases.end(), [&](const auto& alias) {
        return !runtime_participant_binding_physically_contains(binding, alias);
      })) {
    return Result<void>::Err(fault(ErrorCode::kInvalidRequest,
                                   "Route publication crosses the participant's physical binding",
                                   "execution_port_route_rebind_mismatch"));
  }
  binding_ = std::move(binding);
  program_aliases_ = std::move(program_aliases);
  return Result<void>::Ok();
}

ExecutionPortTimingMetrics ExecutionPortParticipant::timing_metrics() const {
  std::lock_guard lock(mutex_);
  return timing_;
}

void ExecutionPortParticipant::ReportCriticalFault(ErrorDTO error) {
  std::lock_guard lock(mutex_);
  if (!execution_ || execution_->terminal || execution_->critical_fault) return;
  execution_->critical_fault = std::move(error);
}

bool ExecutionPortParticipant::GoalInFlight() const {
  std::lock_guard lock(mutex_);
  return (execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U ||
         (helper_ && helper_->HasLiveGoal());
}

std::optional<std::string_view> ExecutionPortParticipant::BindingFieldError(
    const PreparedOperationParticipantDTO& participant) const {
  const auto kind = PreparedParticipantPayloadKindDTO::kMotionExecution;
  if (!prepared_participant_backend_binding_field_error(participant, binding_, kind)) {
    return std::nullopt;
  }
  if (std::any_of(program_aliases_.begin(), program_aliases_.end(), [&](const auto& alias) {
        return !prepared_participant_backend_binding_field_error(participant, alias, kind);
      })) {
    return std::nullopt;
  }
  return prepared_participant_backend_physical_binding_field_error(participant, binding_, kind);
}

PrepareParticipantResultDTO ExecutionPortParticipant::Prepare(
    const PreparedOperationParticipantDTO& participant, const std::uint64_t execution_id) {
  const auto make = [&](PrepareParticipantResultStateDTO state, std::optional<ErrorDTO> error) {
    auto result = MakePrepareParticipantResult(participant, state, std::move(error));
    if (!result.ok()) throw std::invalid_argument(result.error().message);
    return std::move(result.value());
  };
  const auto reject = [&](std::string reason) {
    return make(PrepareParticipantResultStateDTO::kRejected,
                fault(ErrorCode::kExecutionRejected, "Execution port participant rejected the "
                                                     "prepared participant",
                      reason));
  };
  std::lock_guard lock(mutex_);
  if (const auto invalid = BindingFieldError(participant)) return reject(std::string(*invalid));
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  if (motion.trajectory.resources != participant.resources) {
    return reject("prepared_trajectory_resource_scope_mismatch");
  }
  if (motion.trajectory.waypoints.empty()) return reject("empty_trajectory");
  // The speed ramp runs at the trajectory's own prepared bounds.
  const auto& bounds = motion.trajectory.runtime_scale_bounds;
  if (!bounds || !std::isfinite(bounds->max_rate_per_s) || bounds->max_rate_per_s <= 0.0) {
    return reject("prepared_runtime_scale_bounds_missing");
  }
  if (const auto invalid = prepared_membership_.prepare(execution_id, participant)) {
    return reject(std::string(*invalid));
  }
  return make(PrepareParticipantResultStateDTO::kPrepared, std::nullopt);
}

bool ExecutionPortParticipant::SourceMatchesLocked(
    const PreparedMotionReplacementSourceDTO& source) const {
  return execution_ && prepared_goal_ &&
         prepared_membership_.active_execution_id() == source.execution_id &&
         execution_->generation == source.execution_generation && execution_->key == source.goal &&
         execution_->epoch == source.binding_epoch &&
         prepared_goal_->goal_id == source.goal.controller_goal_id &&
         prepared_goal_->generation == source.goal.controller_goal_generation &&
         prepared_goal_->prepared_contract_digest == source.prepared_contract_digest;
}

bool ExecutionPortParticipant::ReplacementScaleCompatibleLocked(
    const ExecutionControllerSampleDTO& sample, const TrajectoryDTO& candidate) const {
  if (!lane_.supports_scale) return true;
  if (!sample.applied_factor || !std::isfinite(*sample.applied_factor) ||
      *sample.applied_factor < 0.0 || *sample.applied_factor > 1.0 ||
      !candidate.runtime_scale_bounds)
    return false;
  const auto& next = *candidate.runtime_scale_bounds;
  if (!std::isfinite(next.max_rate_per_s) || next.max_rate_per_s <= 0.0 ||
      !std::isfinite(next.max_rate_change_per_s2) || next.max_rate_change_per_s2 <= 0.0)
    return false;
  std::lock_guard scale_lock(scale_mutex_);
  if (scale_frozen_) return false;
  if (!ramp_) return true;
  if (preparation_scale_hold_ && preparation_scale_hold_->confirmed && replacement_ &&
      preparation_scale_hold_->candidate_id == replacement_->candidate_id &&
      preparation_scale_hold_->source_goal == execution_->key &&
      *sample.applied_factor == preparation_scale_hold_->factor &&
      ramp_->value == preparation_scale_hold_->factor && ramp_handle_ == 0U)
    return true;

  if (!std::isfinite(ramp_->value) || ramp_->value < 0.0 || ramp_->value > 1.0 ||
      !std::isfinite(ramp_->target) || ramp_->target < 0.0 || ramp_->target > 1.0 ||
      !std::isfinite(ramp_->rate_per_s) || ramp_->rate_per_s <= 0.0 ||
      ramp_->rate_per_s > next.max_rate_per_s || !execution_->trajectory.runtime_scale_bounds)
    return false;
  // Reuse the same admitted ramp envelope only across unchanged evolution.
  // This conservative compatibility check is not an observation of f_ddot or
  // a stronger proof than ordinary runtime scaling already supplies.
  const auto& previous = *execution_->trajectory.runtime_scale_bounds;
  return std::isfinite(previous.max_rate_change_per_s2) && previous.max_rate_change_per_s2 > 0.0 &&
         previous.max_rate_change_per_s2 <= next.max_rate_change_per_s2;
}

Result<PreparedMotionReplacementSourceDTO> ExecutionPortParticipant::CaptureReplacementSource(
    const std::uint64_t execution_id, const std::uint64_t generation, const ExecutionGoalKey& goal,
    const ContentDigest& digest) const {
  std::lock_guard lock(mutex_);
  const auto refuse = [] {
    return Result<PreparedMotionReplacementSourceDTO>::Err(fault(
        ErrorCode::kExecutionRejected, "The resident motion cannot supply a replacement source",
        "motion_replacement_source_unavailable"));
  };
  PreparedMotionReplacementSourceDTO source{
      execution_id, generation, goal, execution_ ? execution_->epoch : ExecutionBindingEpoch{0U},
      digest,       {},         {}};
  const double period_ns = lane_.controller_update_rate_hz > 0.0
                               ? 1e9 / lane_.controller_update_rate_hz : 0.0;
  if (lane_.supports_scheduled_replacement && std::isfinite(period_ns) && period_ns >= 1.0 &&
      period_ns < static_cast<double>(std::numeric_limits<std::int64_t>::max()))
    source.controller_period_ns = static_cast<std::int64_t>(period_ns);
  if (!SourceMatchesLocked(source) || execution_->terminal || execution_->stopping ||
      execution_->critical_fault || stop_stages_in_flight_ != 0U ||
      helper_->HasPendingReplacement() || !execution_->published_progress_sample)
    return refuse();
  const auto& paired = *execution_->published_progress_sample;
  const auto& sample = paired.sample;
  source.progress = progress_.acquire();
  const auto hazard = progress_.hazard_authority_claimed(generation);
  if (!source.progress.valid || source.progress.terminal || !hazard || *hazard ||
      source.progress.sequence != paired.sequence ||
      source.progress.execution_generation != generation ||
      source.progress.source_timestamp_ns != sample.source_stamp_ns ||
      source.progress.received_timestamp_ns != sample.local_receipt_stamp_ns)
    return refuse();
  const auto readiness = port_->ReadReadiness();
  const auto observed = port_->ReadGoal(goal);
  const auto now = clock_.now();
  if (!ready(readiness) || readiness.binding_epoch != source.binding_epoch || !observed ||
      observed->key != goal || observed->binding_epoch != source.binding_epoch ||
      observed->phase == ExecutionGoalPhase::kTerminal ||
      sample.binding_epoch != source.binding_epoch || !std::isfinite(sample.cursor_s) ||
      sample.local_receipt_stamp_ns > now ||
      (config_.progress_freshness_budget_ns > 0 &&
       now - sample.local_receipt_stamp_ns > config_.progress_freshness_budget_ns))
    return refuse();
  source.controller_sample = sample;
  return Result<PreparedMotionReplacementSourceDTO>::Ok(std::move(source));
}

Result<void> ExecutionPortParticipant::BeginReplacementPreparationScaleHold(
    const PreparedMotionReplacementSourceDTO& source, const std::uint64_t candidate_id,
    const std::int64_t deadline, const std::stop_token cancellation) {
  const auto refuse = [](const char* reason) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Moving preparation scaling hold was refused", reason));
  };
  {
    std::unique_lock lock(mutex_);
    if (cancellation.stop_requested() || clock_.now() >= deadline)
      return refuse("motion_replacement_scale_hold_budget_expired");
    if (!SourceMatchesLocked(source) || execution_->terminal || execution_->stopping ||
        execution_->critical_fault || stop_stages_in_flight_ != 0U || candidate_id == 0U)
      return refuse("motion_replacement_scale_hold_source_stale");
    if (!lane_.supports_scale) return Result<void>::Ok();
    const auto factor = source.controller_sample.applied_factor;
    if (!factor || !std::isfinite(*factor) || *factor <= 0.0 || *factor > 1.0)
      return refuse("motion_replacement_scale_hold_factor_invalid");
    std::unique_lock scale_lock(scale_mutex_);
    if (scale_frozen_ ||
        (preparation_scale_hold_ && (preparation_scale_hold_->candidate_id != candidate_id ||
                                     preparation_scale_hold_->source_goal != source.goal)))
      return refuse("motion_replacement_scale_hold_owner_changed");
    if (!preparation_scale_hold_) {
      const bool tick_pending = ramp_handle_ != 0U;
      ++ramp_generation_;
      if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
      ramp_handle_ = 0U;
      const bool settled = !tick_pending && ramp_ && ramp_->value == *factor &&
                           ramp_->target == *factor && last_sent_scale_ == factor &&
                           source.controller_sample.local_receipt_stamp_ns <= clock_.now() &&
                           (config_.progress_freshness_budget_ns == 0 ||
                            clock_.now() - source.controller_sample.local_receipt_stamp_ns <=
                                config_.progress_freshness_budget_ns);
      preparation_scale_hold_ = PreparationScaleHold{candidate_id, source.goal, *factor, settled};
      if (settled)
        trace_scale_hold("confirmed", candidate_id, source.goal, *factor,
                         source.controller_sample.cursor_s, ramp_->rate_per_s);
      if (!settled) {
        const auto port = port_;
        const auto sequence = port->ReserveScaleSequence();
        scale_lock.unlock();
        lock.unlock();
        const auto pin = port->SetScale(*factor, sequence);
        lock.lock();
        scale_lock.lock();
        if (port_ != port || !port->IsLatestScaleSequence(sequence) ||
            !preparation_scale_hold_ || preparation_scale_hold_->candidate_id != candidate_id ||
            preparation_scale_hold_->source_goal != source.goal || scale_frozen_ ||
            !SourceMatchesLocked(source) || !execution_ || execution_->terminal ||
            execution_->stopping || stop_stages_in_flight_ != 0U)
          return refuse("motion_replacement_scale_hold_source_stale");
        if (pin.dispatch == ExecutionPortDispatch::kNotDispatched)
          return refuse("motion_replacement_scale_hold_pin_refused");
        last_sent_scale_ = *factor;
        if (ramp_) ramp_->value = *factor;
      }
    }
    if (preparation_scale_hold_->confirmed) return Result<void>::Ok();
  }
  // Same existing observation/wait seam and preparation deadline; no new timer.
  while (!cancellation.stop_requested() && clock_.now() < deadline) {
    {
      std::lock_guard lock(mutex_);
      std::lock_guard scale_lock(scale_mutex_);
      if (!preparation_scale_hold_ || preparation_scale_hold_->candidate_id != candidate_id ||
          scale_frozen_ || !SourceMatchesLocked(source) || execution_->terminal ||
          execution_->stopping)
        return refuse("motion_replacement_scale_hold_owner_changed");
      const auto sample = port_->ReadSample();
      const auto goal = port_->ReadGoal(source.goal);
      const auto now = clock_.now();
      if (sample && sample->binding_epoch == source.binding_epoch &&
          sample->source_stamp_ns > source.controller_sample.source_stamp_ns &&
          sample->local_receipt_stamp_ns <= now &&
          (config_.progress_freshness_budget_ns == 0 ||
           now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns) &&
          sample->applied_factor == std::optional<double>(preparation_scale_hold_->factor) &&
          execution_->association->Observe(*sample, goal)) {
        preparation_scale_hold_->confirmed = true;
        trace_scale_hold("confirmed", candidate_id, source.goal, preparation_scale_hold_->factor,
                         sample->cursor_s, ramp_ ? ramp_->rate_per_s : 0.0);
        return Result<void>::Ok();
      }
    }
    waiter_(deadline, cancellation);
  }
  return refuse("motion_replacement_scale_hold_unconfirmed");
}

Result<void> ExecutionPortParticipant::EndReplacementPreparationScaleHold(
    const std::uint64_t candidate_id) {
  std::lock_guard lock(mutex_);
  std::lock_guard scale_lock(scale_mutex_);
  if (!preparation_scale_hold_ || preparation_scale_hold_->candidate_id != candidate_id)
    return Result<void>::Ok();
  if (!preparation_scale_hold_->confirmed && !scale_frozen_)
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Moving scale pin has no observed confirmation",
                                   "motion_replacement_scale_hold_unconfirmed"));
  trace_scale_hold("retired", candidate_id, preparation_scale_hold_->source_goal,
                   preparation_scale_hold_->factor, execution_ ? execution_->cursor_s : 0.0,
                   ramp_ ? ramp_->rate_per_s : 0.0);
  preparation_scale_hold_.reset();
  if (ramp_) ramp_->target = target_scale_;
  if (!scale_frozen_ && execution_ && !execution_->terminal && !execution_->stopping &&
      !execution_->scale_hold_until_cursor_s && ramp_ && ramp_->value != ramp_->target) {
    last_ramp_tick_ = clock_.now();
    ScheduleRampTickLocked();
  }
  return Result<void>::Ok();
}

PrepareParticipantResultDTO ExecutionPortParticipant::PrepareReplacement(
    const PreparedOperationParticipantDTO& participant, const std::uint64_t candidate_id,
    const PreparedMotionReplacementSourceDTO& source) {
  const auto reject = [&](std::string reason) {
    return MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kRejected,
                                        fault(ErrorCode::kExecutionRejected,
                                              "Motion replacement preparation was refused", reason))
        .value();
  };
  if (!std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload))
    return reject("motion_replacement_payload_invalid");
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  if (!motion.plan_request.moving_replacement ||
      motion.plan_request.moving_replacement->source_goal != source.goal ||
      motion.plan_request.moving_replacement->source_binding_epoch != source.binding_epoch ||
      !std::isfinite(motion.plan_request.moving_replacement->source_cursor_s) ||
      !std::isfinite(motion.plan_request.moving_replacement->divergence_cursor_s) ||
      motion.plan_request.moving_replacement->source_cursor_s !=
          static_cast<double>(source.progress.trajectory_time_ns) / 1e9 ||
      !(motion.plan_request.moving_replacement->divergence_cursor_s >
        motion.plan_request.moving_replacement->source_cursor_s) ||
      motion.trajectory.resources != participant.resources || motion.trajectory.waypoints.empty() ||
      !motion.trajectory.runtime_scale_bounds ||
      !std::isfinite(motion.trajectory.runtime_scale_bounds->max_rate_per_s) ||
      motion.trajectory.runtime_scale_bounds->max_rate_per_s <= 0.0)
    return reject("motion_replacement_trajectory_invalid");
  // Large immutable curves and metadata are copied before taking the mutex
  // shared with Stop. No candidate storage is allocated after the send.
  auto pending = std::make_unique<Replacement>(candidate_id, source);
  pending->source_cursor_s = motion.plan_request.moving_replacement->source_cursor_s;
  pending->divergence_cursor_s = motion.plan_request.moving_replacement->divergence_cursor_s;
  pending->execution = std::make_shared<Execution>();
  pending->execution->cancel_attempt->execution_id = candidate_id;
  pending->execution->trajectory = motion.trajectory;
  const auto prefix_count =
      motion.plan_request.moving_replacement->source_prefix_trajectory.waypoints.size();
  if (prefix_count > 0U && prefix_count < motion.trajectory.waypoints.size())
    pending->execution->scale_hold_until_cursor_s =
        motion.trajectory.waypoints[prefix_count].time_from_start_s;

  pending->stop_goal = stop_goal_for(participant, {}, motion.trajectory);
  pending->record_trajectory = motion.trajectory;
  auto accepted =
      MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kPrepared)
          .value();
  std::lock_guard lock(mutex_);
  if (const auto invalid = BindingFieldError(participant)) return reject(std::string(*invalid));
  if (!SourceMatchesLocked(source) || execution_->terminal || execution_->stopping ||
      execution_->critical_fault || stop_stages_in_flight_ != 0U ||
      helper_->HasPendingReplacement())
    return reject("motion_replacement_source_changed");
  if (replacement_ && prepared_membership_.active_execution_id() != replacement_->candidate_id) {
    if (replacement_->candidate_id == candidate_id &&
        same_replacement_source(replacement_->source, source) &&
        prepared_membership_.replacement_matches(candidate_id, participant))
      return accepted;
    return reject("motion_replacement_in_flight");
  }
  if (preparation_scale_hold_ && preparation_scale_hold_->candidate_id == candidate_id &&
      !pending->execution->scale_hold_until_cursor_s)
    return reject("motion_replacement_connector_boundary_invalid");
  if (preparation_scale_hold_ && preparation_scale_hold_->candidate_id != candidate_id)
    return reject("motion_replacement_scale_hold_owner_changed");
  if (!helper_->ReserveReplacement(source.goal)) return reject("motion_replacement_goal_capacity");
  if (const auto invalid =
          prepared_membership_.prepare_replacement(source.execution_id, candidate_id, participant))
    return reject(std::string(*invalid));
  pending->execution->generation = next_execution_generation_++;
  pending->execution->epoch = source.binding_epoch;
  pending->execution->port = port_;
  pending->execution->stop_helper = helper_;
  replacement_ = std::move(pending);
  return accepted;
}

PreparedMotionReplacementDispatchDTO ExecutionPortParticipant::DispatchReplacement(
    const PreparedMotionReplacementSourceDTO& source, const std::uint64_t candidate_id,
    const PreparedOperationParticipantDTO& participant, const ExecutionGoalKey& successor,
    const std::int64_t preparation_deadline_ns, const ReplacementSendRecheck& recheck_before_send) {
  const auto reject = [](std::string reason, std::map<std::string, std::string> details = {}) {
    return PreparedMotionReplacementDispatchDTO{
        {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey,
         fault(ErrorCode::kExecutionRejected, "Motion replacement dispatch was refused", reason,
               std::move(details))},
        std::nullopt};
  };
  std::unique_lock lock(mutex_);
  // The existing dispatch decision (membership, replay / goal conflict,
  // source). The experimental release timer reruns it after its unlocked wait.
  const auto guards = [&]() -> std::optional<PreparedMotionReplacementDispatchDTO> {
    if (!replacement_ || replacement_->candidate_id != candidate_id ||
        !same_replacement_source(replacement_->source, source) ||
        !(prepared_membership_.replacement_matches(candidate_id, participant) ||
          (prepared_membership_.active_execution_id() == candidate_id &&
           prepared_membership_.matches(participant))))
      return reject("motion_replacement_membership_changed");
    if (replacement_->command) {
      const auto& replay_goal = prepared_membership_.active_execution_id() == candidate_id
                                    ? prepared_goal_
                                    : replacement_->prepared_goal;
      if (!replay_goal || replay_goal->goal_id != successor.controller_goal_id ||
          replay_goal->generation != successor.controller_goal_generation)
        return reject("motion_replacement_goal_conflict");
      return PreparedMotionReplacementDispatchDTO{*replacement_->command, replay_goal->handle,
                                                  replacement_->submitted_at_ns};
    }
    if (!SourceMatchesLocked(source) || execution_->terminal || execution_->stopping ||
        execution_->critical_fault || stop_stages_in_flight_ != 0U || successor == source.goal ||
        successor.controller_goal_id.empty() || !successor.controller_goal_generation.nonzero() ||
        !config_.stop.replacement_start_allowance_s)
      return reject("motion_replacement_source_changed");
    return std::nullopt;
  };
  // Another attempt's send may be in flight without the lock (only a
  // duplicate call; the Kernel's dispatch gate sends once). Decide after it
  // has recorded its outcome, as when the lock was held across the send: a
  // completed send then replays or conflicts.
  const auto decide = [&]() -> std::optional<PreparedMotionReplacementDispatchDTO> {
    while (replacement_ && replacement_->candidate_id == candidate_id &&
           replacement_->execution->dispatch_pending) {
      const auto in_flight = replacement_->execution->dispatch_token;
      lock.unlock();
      (void)in_flight->WaitUntil(std::chrono::steady_clock::time_point::max());
      lock.lock();
    }
    return guards();
  };
  if (auto decided = decide()) return std::move(*decided);
  auto& pending = *replacement_;
  auto candidate = pending.execution;
  // Stage the goal/handle and failure receipt before choosing the actual join
  // origin. None of these allocations consumes unaccounted source lead.
  const auto stage_keys = [&] {
    candidate->key = successor;
    candidate->snapshot.state = ExecutionState::kExecuting;
    candidate->snapshot.state_source = "execution_port";
    pending.record_key = successor;
    pending.stop_goal->key = successor;
  };
  stage_keys();
  ExecutionHandle handle{
      [this, candidate](std::int64_t timeout) { return WaitForCompletion(candidate, timeout); },
      [this, candidate] {
        std::lock_guard lock(mutex_);
        return candidate->snapshot;
      },
      candidate->generation,
      successor.controller_goal_id,
      successor.controller_goal_generation,
      participant.prepared_contract_digest};
  pending.prepared_goal =
      PreparedGoalRecord{successor.controller_goal_id, successor.controller_goal_generation,
                         participant.prepared_contract_digest, handle};
  auto uncertain = ExecutionPortCommandResultDTO{
      ExecutionPortDispatch::kEffectUncertain, std::nullopt,
      fault(ErrorCode::kExecutionAborted, "Motion replacement send outcome is unknown",
            "motion_replacement_send_threw")};
  std::string source_qualification_check;
  const auto source_sample_current = [&](const auto& sample, TimePoint now) {
    const auto readiness = port_->ReadReadiness();
    const auto predecessor = port_->ReadGoal(source.goal);
    const auto check = [&](bool qualified, const char* name) {
      if (!qualified) source_qualification_check = name;
      return qualified;
    };
    return check(ready(readiness) && readiness.binding_epoch == source.binding_epoch,
                 "readiness") &&
           check(predecessor && predecessor->key == source.goal &&
                     predecessor->binding_epoch == source.binding_epoch &&
                     predecessor->phase != ExecutionGoalPhase::kTerminal,
                 "predecessor") &&
           check(sample && sample->binding_epoch == source.binding_epoch &&
                     std::isfinite(sample->cursor_s),
                 "sample") &&
           // A cursor behind the captured source cursor is a controller that
           // already restarted (end hold); a join from it would move back.
           check(original_path_cursor(execution_->delivery_clock, sample->cursor_s) >=
                     pending.source_cursor_s,
                 "cursor_restart") &&
           check(sample->local_receipt_stamp_ns <= now, "sample_future_stamp") &&
           check(
               config_.stop.controller_state_max_age_ns == 0 ||
                   now - sample->local_receipt_stamp_ns <= config_.stop.controller_state_max_age_ns,
               "controller_state_age") &&
           check(config_.progress_freshness_budget_ns == 0 ||
                     now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns,
                 "progress_age") &&
           check(execution_->association->Observe(*sample, predecessor), "association") &&
           check(ReplacementScaleCompatibleLocked(*sample, candidate->trajectory),
                 "scale_compatibility");
  };
  // A refused dispatch already creates a structured error. Retain the exact
  // failed qualification and configured scale state there without another
  // controller read, retry, or change to admission.
  const auto source_refusal = [&](const auto& sample, TimePoint now, const char* stage) {
    const auto number = [](double value) {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.17g", value);
      return std::string(buffer);
    };
    std::map<std::string, std::string> details{{"qualification_check", source_qualification_check},
                                               {"qualification_stage", stage}};
    if (sample) {
      details["sample_cursor_s"] = number(sample->cursor_s);
      details["sample_age_ns"] = std::to_string(now - sample->local_receipt_stamp_ns);
      if (sample->applied_factor) details["applied_factor"] = number(*sample->applied_factor);
    }
    const auto bounds = [&](const char* label, const auto& trajectory) {
      if (trajectory.runtime_scale_bounds) {
        details[std::string(label) + "_rate_per_s"] =
            number(trajectory.runtime_scale_bounds->max_rate_per_s);
        details[std::string(label) + "_rate_change_per_s2"] =
            number(trajectory.runtime_scale_bounds->max_rate_change_per_s2);
      }
    };
    bounds("source", execution_->trajectory);
    bounds("candidate", candidate->trajectory);
    {
      std::lock_guard scale_lock(scale_mutex_);
      details["scale_frozen"] = scale_frozen_ ? "true" : "false";
      details["ramp_present"] = ramp_ ? "true" : "false";
      details["ramp_tick_pending"] = ramp_handle_ != 0U ? "true" : "false";
      if (ramp_) {
        details["ramp_value"] = number(ramp_->value);
        details["ramp_target"] = number(ramp_->target);
        details["ramp_rate_per_s"] = number(ramp_->rate_per_s);
      }
    }
    return reject("motion_replacement_scale_or_source_unqualified", std::move(details));
  };
  const auto elapsed_at = [&](const ExecutionControllerSampleDTO& sample, TimePoint now,
                              double allowance) {
    return compute_stop_join_elapsed_s(
        {original_path_cursor(execution_->delivery_clock, sample.cursor_s),
         lane_.supports_scale ? sample.applied_factor.value_or(-1.0) : 1.0,
         static_cast<double>(now - sample.local_receipt_stamp_ns) / 1e9, allowance,
         pending.divergence_cursor_s});
  };
  // Same existing Stop join contract: actual coherent source state immediately
  // before construction, with the existing lane start allowance only.
  const auto baseline = port_->ReadSample();
  const auto join_started_at = clock_.now();
  if (!source_sample_current(baseline, join_started_at))
    return source_refusal(baseline, join_started_at, "baseline");
  // Scheduled lanes pair the header with the SAME estimated source origin.
  // Moving takeover retains its existing instantaneous-factor extrapolation;
  // a compatible evolving ramp is not frozen or newly refused here.
  auto start = compute_stop_replacement_start(
      {original_path_cursor(execution_->delivery_clock, baseline->cursor_s),
       lane_.supports_scale ? baseline->applied_factor.value_or(-1.0) : 1.0,
       static_cast<double>(join_started_at - baseline->local_receipt_stamp_ns) / 1e9,
       *config_.stop.replacement_start_allowance_s, pending.divergence_cursor_s},
      *baseline, lane_);
  if (!start.ok()) return reject("motion_replacement_delivery_lead_expired");
  const auto elapsed = start.value().elapsed_path_s;
  auto delivery =
      build_stop_replacement_join(execution_->trajectory, elapsed, candidate->trajectory,
                                  pending.source_cursor_s, pending.divergence_cursor_s);
  // EXPERIMENTAL (MANYFORGE_EXPERIMENTAL_V0_PAYLOAD): no negative-time prefix.
  if (delivery.ok() && start.value().scheduled && start.value().scheduled->experiment &&
      start.value().scheduled->experiment->v0_payload)
    delivery = stop_replacement_v0_payload(delivery.value());
  if (!delivery.ok()) return reject("motion_replacement_delivery_join_failed");
  // EXPERIMENTAL release timer: wait WITHOUT the participant lock, before
  // anything of this attempt is committed to the pending replacement. Then the
  // existing decision runs again (a Stop, retirement or a duplicate dispatch
  // that already sent wins, the latter as a replay), the pending state is
  // rebound by identity, and every existing check below runs at the actual
  // send. `join_started_at` stays before the wait, so the allowance and the
  // preparation deadline charge for it. The timer may charge one nominal
  // period for grid rounding; every other source and deadline check stays.
  auto join_allowance_s = *config_.stop.replacement_start_allowance_s;
  auto allowance_ns =
      static_cast<TimePoint>(std::floor(*config_.stop.replacement_start_allowance_s * 1e9));
  if (start.value().scheduled && start.value().scheduled->experiment &&
      start.value().scheduled->experiment->release_local_ns) {
    // compute_stop_replacement_start already validated this positive period.
    const auto period_ns = static_cast<TimePoint>(1e9 / lane_.controller_update_rate_hz);
    if (allowance_ns > INT64_MAX - period_ns ||
        join_started_at > INT64_MAX - allowance_ns - period_ns)
      return reject("motion_replacement_delivery_lead_expired");
    allowance_ns += period_ns;
    join_allowance_s += static_cast<double>(period_ns) / 1e9;
  }
  if (const auto until = plan_replacement_release(
          start.value(), clock_,
          std::min(join_started_at + allowance_ns, preparation_deadline_ns - 1))) {
    const auto* const staged = replacement_.get();
    lock.unlock();
    while (clock_.now() < *until) waiter_(*until, std::stop_token{});
    lock.lock();
    if (auto decided = decide()) return std::move(*decided);
    if (replacement_.get() != staged || replacement_->execution != candidate)
      return reject("motion_replacement_membership_changed");
    // Another attempt (any successor) may have staged its keys and handle
    // meanwhile; this attempt sends, so its own staging is restored.
    stage_keys();
    pending.prepared_goal = PreparedGoalRecord{successor.controller_goal_id,
                                               successor.controller_goal_generation,
                                               participant.prepared_contract_digest, handle};
  }
  candidate->association =
      std::make_shared<ExecutionSampleAssociation>(source.binding_epoch, baseline, join_started_at);
  candidate->delivery_clock.elapsed_s = elapsed - pending.source_cursor_s;
  // Only the delivery origin changes; canonical product/evidence stays intact.
  delivery.value().runtime_scale_bounds = candidate->trajectory.runtime_scale_bounds;
  delivery.value().cartesian_fidelity_metrics = candidate->trajectory.cartesian_fidelity_metrics;
  delivery.value().cartesian_waypoint_times_s = candidate->trajectory.cartesian_waypoint_times_s;
  for (auto& milestone : delivery.value().cartesian_waypoint_times_s)
    milestone -= candidate->delivery_clock.elapsed_s;
  pending.record_trajectory = std::move(delivery.value());
  pending.stop_goal->trajectory = pending.record_trajectory;
  pending.stop_goal->planned_controller_offset_s = start.value().controller_offset_s;
  if (config_.observe_goal_context) {
    try { config_.observe_goal_context(successor, participant, source.binding_epoch); } catch (...) {}
  }
  const auto final_sample = port_->ReadSample();
  const auto final_now = clock_.now();
  if (!source_sample_current(final_sample, final_now))
    return source_refusal(final_sample, final_now, "final");
  if (!elapsed_at(*final_sample, final_now, *config_.stop.replacement_start_allowance_s).ok())
    return reject("motion_replacement_delivery_lead_expired");
  pending.recognition_budget_ns.reset();
  const double factor = lane_.supports_scale ? final_sample->applied_factor.value_or(-1.0) : 1.0;
  if (factor > 0.0) {
    const auto actual_now = elapsed_at(*final_sample, final_now, 0.0);
    const double remaining_ns =
        actual_now.ok() ? (pending.divergence_cursor_s - actual_now.value()) / factor * 1e9 : -1.0;
    // Association receipts are ordered from join_started_at. Express the
    // same remaining source lead on that origin rather than charging join
    // work twice. This adds elapsed time, not another delivery allowance.
    const double association_window_ns =
        remaining_ns + static_cast<double>(final_now - join_started_at);
    if (!(remaining_ns > 0.0) || !std::isfinite(association_window_ns) ||
        association_window_ns >= static_cast<double>(INT64_MAX))
      return reject("motion_replacement_recognition_window_invalid");
    pending.recognition_budget_ns = static_cast<std::int64_t>(association_window_ns);
  }
  // Bound join/copy/proof work by the same allowance used for the timer hold.
  // This preserves the chosen join estimate; it is not zero-latency proof or
  // an additional allowance for controller delivery.
  const auto submitted_at = clock_.now();
  if (submitted_at < join_started_at || static_cast<double>(submitted_at - join_started_at) / 1e9 >
                                            join_allowance_s)
    return reject("motion_replacement_join_origin_expired");
  if (submitted_at >= preparation_deadline_ns)
    return reject("motion_replacement_preparation_deadline_expired");
  candidate->snapshot.timestamp_ns = submitted_at;
  // Make the possible effect visible to Stop before the participant lock is
  // released (as Dispatch): the successor joins the helper's sent set and its
  // dispatch token is the one Stop fences (`TranslateStopStageImpl`,
  // `CancelExecution`). The port admits the send only through this token, in
  // its action leaf. A Stop that enters during the send therefore either
  // fences the token first (the port never sends; the record is removed below
  // before the token finishes, and the Stop stage waits for that) or finds it
  // admitted, with the successor already in the sent set, and stops it
  // together with the source.
  auto candidate_token = std::make_shared<ExecutionDispatchToken>(candidate_id);
  candidate->dispatch_token = candidate_token;
  candidate->dispatch_pending = true;
  const auto send_helper = candidate->stop_helper;
  const auto send_port = candidate->port;
  // The sent trajectory is the prepared Stop goal's identical copy; the record
  // takes the other one. RetirePreparedExecution keeps this replacement while
  // the send is in flight.
  const TrajectoryDTO& send_trajectory = pending.stop_goal->trajectory;
  send_helper->RecordReplacement(std::move(pending.record_key), std::move(pending.record_trajectory),
                                 candidate->association,
                                 pending.stop_goal->planned_controller_offset_s,
                                 candidate->cancel_attempt);
  // Diagnostics only, captured under the lock and written after it.
  const auto trace_source_cursor_s =
      original_path_cursor(execution_->delivery_clock, final_sample->cursor_s);
  const auto trace_divergence_s = pending.divergence_cursor_s;
  const auto trace_recognition_budget_ns = pending.recognition_budget_ns.value_or(0);
  lock.unlock();
  ExecutionPortCommandResultDTO command;
  std::optional<ErrorDTO> revoked;
  if (recheck_before_send) revoked = recheck_before_send();
  if (revoked) {
    command = {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey,
               std::move(revoked)};
  } else {
    try {
      command = start.value().scheduled
                    ? send_port->ReplaceScheduled(source.goal, successor, send_trajectory,
                                                  *start.value().scheduled, candidate_token)
                    : send_port->Replace(source.goal, successor, send_trajectory, candidate_token);
    } catch (...) {
      command = std::move(uncertain);
    }
    if (command.dispatch == ExecutionPortDispatch::kNotDispatched &&
        candidate_token->state() == ExecutionDispatchToken::State::kFenced)
      command.error = fault(ErrorCode::kExecutionRejected,
                            "A Stop fenced the motion replacement before its send",
                            "motion_replacement_stop_fenced");
  }
  const bool dispatched = command.dispatch != ExecutionPortDispatch::kNotDispatched;
  const auto submitted_at_ns = dispatched ? std::optional<std::int64_t>(submitted_at) : std::nullopt;
  lock.lock();
  candidate->dispatch_pending = false;
  // Known not sent: undo the visible record before the token finishes.
  if (!dispatched) send_helper->RetireSettledReplacement(successor);
  // Record the outcome only on the replacement this attempt sent for.
  if (replacement_ && replacement_->execution == candidate) {
    replacement_->submitted_at_ns = submitted_at_ns;
    replacement_->command = command;
  }
  candidate_token->Finish(command.dispatch);
  lock.unlock();
  const auto written = std::fprintf(stderr,
      "manyforge_moving_dispatch owner=%llu submitted_ns=%lld origin_ns=%lld recognition_budget_ns=%lld source_cursor_s=%.17g divergence_s=%.17g factor=%.17g\n",
      static_cast<unsigned long long>(candidate_id), static_cast<long long>(submitted_at_ns.value_or(0)),
      static_cast<long long>(join_started_at), static_cast<long long>(trace_recognition_budget_ns),
      trace_source_cursor_s, trace_divergence_s, factor);
  static_cast<void>(written);
  return {std::move(command), std::move(handle), submitted_at_ns};
}

PreparedMotionReplacementObservationDTO ExecutionPortParticipant::ObserveReplacementLocked() const {
  PreparedMotionReplacementObservationDTO observed;
  const auto& pending = *replacement_;
  if (prepared_membership_.active_execution_id() == pending.candidate_id) {
    observed.phase = PreparedMotionReplacementPhaseDTO::kCommitted;
    return observed;
  }
  observed.predecessor = port_->ReadGoal(pending.source.goal);
  if (!pending.command) {
    observed.phase = PreparedMotionReplacementPhaseDTO::kPrepared;
    return observed;
  }
  if (pending.command->dispatch == ExecutionPortDispatch::kNotDispatched) {
    observed.phase = PreparedMotionReplacementPhaseDTO::kNoEffect;
    observed.error = pending.command->error;
    return observed;
  }
  observed.successor = port_->ReadGoal(pending.execution->key);
  const auto readiness = port_->ReadReadiness();
  const bool binding = ready(readiness) && readiness.binding_epoch == pending.source.binding_epoch;
  if (observed.successor && observed.successor->key == pending.execution->key &&
      observed.successor->binding_epoch == pending.source.binding_epoch) {
    pending.execution->association->ObserveGoal(observed.successor);
    observed.recognition_delay_ns = pending.execution->association->recognition_delay_ns();
    if (binding && observed.successor->phase == ExecutionGoalPhase::kTerminal &&
        observed.successor->result == ExecutionGoalResult::kRejected) {
      observed.phase = PreparedMotionReplacementPhaseDTO::kNoEffect;
      return observed;
    }
  }
  observed.phase = PreparedMotionReplacementPhaseDTO::kPossiblySent;
  const bool superseded = observed.predecessor &&
                          observed.predecessor->key == pending.source.goal &&
                          observed.predecessor->binding_epoch == pending.source.binding_epoch &&
                          observed.predecessor->phase == ExecutionGoalPhase::kTerminal &&
                          observed.predecessor->result == ExecutionGoalResult::kPreemptedBy &&
                          observed.predecessor->preempted_by == pending.execution->key;
  const bool successor_ok = observed.successor &&
                            observed.successor->key == pending.execution->key &&
                            observed.successor->binding_epoch == pending.source.binding_epoch &&
                            (observed.successor->phase != ExecutionGoalPhase::kTerminal ||
                             observed.successor->result == ExecutionGoalResult::kSucceeded);
  if (!binding || !observed.predecessor || !observed.successor || !successor_ok ||
      (observed.predecessor->phase == ExecutionGoalPhase::kTerminal && !superseded)) {
    observed.phase = PreparedMotionReplacementPhaseDTO::kEffectUncertain;
    observed.error =
        fault(ErrorCode::kExecutionAborted,
              "Motion replacement lacks exact continuing predecessor/successor evidence",
              "motion_replacement_effect_uncertain");
  } else if (superseded && !observed.successor->mechanism_goal_id.empty() &&
             (observed.successor->accepted_stamp_ns || observed.successor->latest_feedback)) {
    // Exact acceptance authorizes the request. Delayed action feedback is
    // not evidence that the controller failed to replace the trajectory.
    // Keep ownership/progress pending until an actual associated sample;
    // recognition delay remains an observation, not another source deadline.
    const auto sample = port_->ReadSample();
    const auto now = clock_.now();
    if (sample && sample->binding_epoch == pending.source.binding_epoch &&
        std::isfinite(sample->cursor_s) && sample->local_receipt_stamp_ns <= now &&
        (config_.progress_freshness_budget_ns == 0 ||
         now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns) &&
        pending.execution->association->Observe(*sample, observed.successor)) {
      observed.phase = PreparedMotionReplacementPhaseDTO::kReadyToCommit;
    }
  }
  return observed;
}

Result<PreparedMotionReplacementObservationDTO> ExecutionPortParticipant::ObserveReplacement(
    const PreparedMotionReplacementSourceDTO& source, const std::uint64_t candidate_id) const {
  std::lock_guard lock(mutex_);
  if (!replacement_ || replacement_->candidate_id != candidate_id ||
      !same_replacement_source(replacement_->source, source))
    return Result<PreparedMotionReplacementObservationDTO>::Err(
        fault(ErrorCode::kInvalidRequest, "Motion replacement observation names stale membership",
              "motion_replacement_stale"));
  return Result<PreparedMotionReplacementObservationDTO>::Ok(ObserveReplacementLocked());
}

Result<void> ExecutionPortParticipant::AdoptReplacement(
    const PreparedMotionReplacementSourceDTO& source, const std::uint64_t candidate_id,
    const PreparedMotionReplacementCommitDTO commit) {
  std::lock_guard lock(mutex_);
  if (!replacement_ || replacement_->candidate_id != candidate_id ||
      !same_replacement_source(replacement_->source, source) || !SourceMatchesLocked(source) ||
      execution_->terminal || execution_->stopping || execution_->critical_fault ||
      stop_stages_in_flight_ != 0U || !commit.owner_commit ||
      !prepared_membership_.can_adopt(source.execution_id, candidate_id) ||
      ObserveReplacementLocked().phase != PreparedMotionReplacementPhaseDTO::kReadyToCommit)
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Motion replacement lost its adoption proof",
                                   "motion_replacement_not_ready"));
  const auto sample = port_->ReadSample();
  const auto successor = port_->ReadGoal(replacement_->execution->key);
  const auto now = clock_.now();
  if (!sample || !std::isfinite(sample->cursor_s) ||
      sample->binding_epoch != source.binding_epoch || sample->local_receipt_stamp_ns > now ||
      (config_.progress_freshness_budget_ns > 0 &&
       now - sample->local_receipt_stamp_ns > config_.progress_freshness_budget_ns) ||
      !replacement_->execution->association->Observe(*sample, successor))
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Motion replacement has no own controller cursor",
                                   "motion_replacement_cursor_unavailable"));
  // The accepted goal is already sent. A later own feedback receipt does
  // not prove late installation, so it cannot invalidate the accepted swap.
  if (!ReplacementScaleCompatibleLocked(*sample, replacement_->execution->trajectory))
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Motion replacement cannot preserve the active scale ramp",
                                   "motion_replacement_scale_unqualified"));
  ObserveSample(*replacement_->execution, *sample,
                original_path_cursor(replacement_->execution->delivery_clock, sample->cursor_s),
                now, false);
  // Retaining the paired sample after the owner CAS must not allocate.
  auto published_sample = *sample;
  // All proof reads/allocation precede this last-fallible ownership CAS. The
  // caller rechecks its exact gate/deadline inside the callback before CAS.
  auto accepted = commit.owner_commit(commit.context);
  if (!accepted.ok()) return accepted;
  static_assert(std::is_nothrow_swappable_v<PreparedGoalRecord>);
  execution_->terminal = true;
  execution_->snapshot.state = ExecutionState::kStopped;
  execution_->snapshot.timestamp_ns = now;
  ++execution_->snapshot.sequence_number;
  execution_ = replacement_->execution;
  {
    std::lock_guard scale_lock(scale_mutex_);
    if (preparation_scale_hold_ && preparation_scale_hold_->candidate_id == candidate_id) {
      trace_scale_hold("adopted", candidate_id, execution_->key, preparation_scale_hold_->factor,
                       *execution_->scale_hold_until_cursor_s, ramp_ ? ramp_->rate_per_s : 0.0);
      preparation_scale_hold_.reset();
    } else
      execution_->scale_hold_until_cursor_s.reset();
  }

  prepared_goal_.swap(replacement_->prepared_goal);
  prepared_membership_.adopt_replacement();
  helper_->AdoptReplacement(replacement_->stop_goal);
  // The retained receipt is still the committed candidate; execution_ is its
  // only live presenter. No old handle can publish into this generation.
  progress_.invalidate();
  const auto published = progress_.publish(
      execution_->generation, static_cast<std::int64_t>(execution_->cursor_s * 1e9),
      sample->source_stamp_ns, sample->local_receipt_stamp_ns);
  execution_->RecordProgressSample(published, std::move(published_sample));
  // Legacy compatible evolving ramps continue unchanged. A prepared hold
  // stays pinned until this adopted execution observes its connector end.
  return Result<void>::Ok();
}

ExecutionPortCommandResultDTO ExecutionPortParticipant::CancelExecution(
    Execution& execution, std::unique_lock<std::mutex>& lock,
    std::chrono::steady_clock::time_point deadline) {
  const auto attempt = execution.cancel_attempt;
  const bool unadopted =
      replacement_ && prepared_membership_.active_execution_id() != replacement_->candidate_id;
  // A replacement send in flight (DispatchReplacement, unlocked): fenced
  // here, the port never sends it and only the exact goal is cancelled;
  // already admitted, it may be sent and is in the sent set, so every live
  // goal is cancelled.
  const bool send_admitted =
      unadopted && replacement_->execution->dispatch_pending &&
      replacement_->execution->dispatch_token->Fence() == ExecutionDispatchToken::State::kAdmitted;
  const bool cancel_all =
      unadopted && (send_admitted || (replacement_->command && replacement_->command->dispatch !=
                                                                   ExecutionPortDispatch::kNotDispatched));
  const auto helper = execution.stop_helper;
  const auto key = execution.key;
  lock.unlock();
  ExecutionPortCommandResultDTO result;
  try {
    if (cancel_all) {
      const bool sent = helper->CancelLiveGoals(deadline);
      result.dispatch = sent ? ExecutionPortDispatch::kEffectUncertain
                             : ExecutionPortDispatch::kNotDispatched;
    } else {
      result = helper->CancelExactGoal(key, attempt, deadline);
    }
  } catch (...) {
    result.dispatch = ExecutionPortDispatch::kEffectUncertain;
  }

  lock.lock();
  return result;
}

Result<void> ExecutionPortParticipant::RetirePreparedExecution(const std::uint64_t execution_id) {
  std::lock_guard lock(mutex_);
  const auto active = prepared_membership_.active_execution_id();
  if (replacement_ && replacement_->candidate_id == execution_id && active != execution_id) {
    if (stop_stages_in_flight_ != 0U)
      return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                     "Replacement Stop work is in flight",
                                     "prepared_replacement_not_quiescent"));
    // A send in flight may still have effects; its sender still uses it.
    if (replacement_->execution->dispatch_pending)
      return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                     "The replacement may still have effects",
                                     "prepared_replacement_not_quiescent"));
    if (replacement_->command &&
        replacement_->command->dispatch != ExecutionPortDispatch::kNotDispatched &&
        !replacement_->execution->terminal) {
      const bool rebuilt_unknown = retired_owner_rest_qualified_ &&
                                   retired_unknown_candidate_ == execution_id;
      if (!rebuilt_unknown &&
          ObserveReplacementLocked().phase != PreparedMotionReplacementPhaseDTO::kNoEffect)
        return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                       "The replacement may still have effects",
                                       "prepared_replacement_not_quiescent"));
      if (!rebuilt_unknown) helper_->RetireSettledReplacement(replacement_->execution->key);
    }
    if (replacement_->execution->terminal)
      helper_->RetireSettledReplacement(replacement_->execution->key);
    if (!prepared_membership_.retire(execution_id))
      return Result<void>::Err(fault(ErrorCode::kInvalidRequest, "Replacement retirement is stale",
                                     "prepared_execution_stale"));
    replacement_.reset();
    retired_unknown_candidate_.reset();
    if (!prepared_membership_.active_execution_id()) {
      retired_owner_helper_.reset();
      retired_owner_port_.reset();
    }
    return Result<void>::Ok();
  }
  if (active == execution_id &&
      ((execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U)) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Prepared execution still has active motion or Stop work",
                                   "prepared_execution_not_quiescent"));
  }
  if (!prepared_membership_.retire(execution_id)) {
    return Result<void>::Err(fault(ErrorCode::kInvalidRequest,
                                   "Prepared retirement names an unknown execution",
                                   "prepared_execution_stale"));
  }
  if (active == execution_id) {
    owner_rebuild_observation_.reset();
    prepared_goal_.reset();
    // A committed candidate is no longer pending after its exact owner
    // retirement. Do not carry its receipt into an unrelated ordinary goal.
    if (replacement_ && replacement_->candidate_id == execution_id) replacement_.reset();
    if (!replacement_) {
      retired_owner_rest_qualified_ = false;
      retired_owner_helper_.reset();
      retired_owner_port_.reset();
    }
  }
  return Result<void>::Ok();
}

ExecutionHandle ExecutionPortParticipant::RejectedHandle(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& goal_id,
    const ControllerGoalGeneration generation, ErrorDTO error) const {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = ExecutionState::kFailed;
  snapshot.error = std::move(error);
  return ExecutionHandle{[](std::int64_t) { return true; },
                         [snapshot = std::move(snapshot)] { return snapshot; },
                         0U,
                         goal_id,
                         generation,
                         participant.prepared_contract_digest};
}

Result<void> ExecutionPortParticipant::ApplyToolContext(
    const PreparedOperationParticipantDTO& participant) {
  const auto refused = [](std::string reason, std::map<std::string, std::string> details = {}) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Execution port participant refused the tool-context action",
                                   std::move(reason), std::move(details)));
  };
  if (!config_.apply_tool_context ||
      !std::holds_alternative<PreparedMotionExecutionDTO>(participant.payload))
    return Result<void>::Ok();
  // Tool context (its own action, before the motion is sent and counts as
  // running): no goal or Stop is in flight, so the robot is at rest. The lane
  // sends what changed and waits until the robot settled; a tool context the
  // robot did not confirm refuses this motion (fail closed). The vendor send
  // may be slow, so it runs WITHOUT the participant lock: a Stop pressed
  // meanwhile enters at once, and a Stop that started since the action began
  // wins, here or at Dispatch (no goal is sent). A TCP offset send rebinds
  // the lane on a new port; Dispatch reads readiness, a fresh sample and the
  // start state there (a moved robot still refuses).
  std::unique_lock lock(mutex_);
  if (!prepared_membership_.matches(participant))
    return refused("prepared_participant_not_admitted");
  if (execution_ && !execution_->terminal) return refused("execution_port_execution_in_flight");
  if (stop_stages_in_flight_ != 0U) return refused("execution_port_stop_in_flight");
  const auto stops_before = stop_stages_started_;
  tool_context_stops_before_ = stops_before;
  lock.unlock();
  const auto applied = config_.apply_tool_context(participant);
  if (!applied.ok()) {
    std::map<std::string, std::string> details;
    for (const auto& [name, value] : applied.error().details)
      details[name == "reason" ? "cause" : name] = value;
    details["port_error"] = applied.error().message;
    return refused("tool_context_not_applied", std::move(details));
  }
  lock.lock();
  if (stop_stages_in_flight_ != 0U || stop_stages_started_ != stops_before)
    return refused("execution_port_stop_in_flight");
  return Result<void>::Ok();
}

ExecutionHandle ExecutionPortParticipant::Dispatch(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& controller_goal_id,
    const ControllerGoalGeneration controller_goal_generation) {
  const auto rejected = [&](std::string reason, std::map<std::string, std::string> details = {}) {
    return RejectedHandle(participant, controller_goal_id, controller_goal_generation,
                          fault(ErrorCode::kExecutionRejected,
                                "Execution port participant rejected the prepared dispatch",
                                reason, std::move(details)));
  };
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (measured) AwaitFreshStartSample();
  std::unique_lock lock(mutex_);
  if (controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
    return rejected("controller_goal_identity_invalid");
  }
  // A Stop stage found this goal unsent and contained it; it never goes
  // out afterwards (also when that Stop ran during the fresh-sample wait
  // above, before this lock).
  if (stopped_before_dispatch_ ==
      ExecutionGoalKey{controller_goal_id, controller_goal_generation}) {
    return rejected("execution_port_stop_in_flight");
  }
  if (const auto invalid = BindingFieldError(participant)) return rejected(std::string(*invalid));
  if (!prepared_membership_.matches(participant)) {
    return rejected("prepared_participant_not_admitted");
  }
  if (pending_dispatch_) {
    const auto pending = pending_dispatch_;
    if (pending->key == ExecutionGoalKey{controller_goal_id, controller_goal_generation} &&
        pending->digest == participant.prepared_contract_digest) {
      lock.unlock();
      return pending->completion.get();
    }
    if (pending->key.controller_goal_id == controller_goal_id)
      return rejected("controller_goal_identity_conflict");
    return rejected("execution_port_execution_in_flight");
  }
  // Replay protection: an exact repetition returns the same handle and never
  // sends a second goal; any other use of the goal identity is refused.
  if (prepared_goal_ && prepared_goal_->goal_id == controller_goal_id) {
    if (prepared_goal_->generation == controller_goal_generation &&
        prepared_goal_->prepared_contract_digest == participant.prepared_contract_digest) {
      return prepared_goal_->handle;
    }
    return rejected("controller_goal_identity_conflict");
  }
  if (execution_ && !execution_->terminal) return rejected("execution_port_execution_in_flight");
  if (stop_stages_in_flight_ != 0U) return rejected("execution_port_stop_in_flight");
  // The tool-context action ran before this dispatch without the lock
  // (`ApplyToolContext`); a Stop that started since then wins: no goal.
  if (const auto stops_before = std::exchange(tool_context_stops_before_, std::nullopt);
      stops_before && stop_stages_started_ != *stops_before)
    return rejected("execution_port_stop_in_flight");

  // Positions, velocities and accelerations in path time. A prepared
  // trajectory carries its runtime-scale bounds (checked at Prepare),
  // which the prepared codec admits only with complete derivatives.
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  auto* port = port_.get();
  auto readiness = port->ReadReadiness();
  if (!ready(readiness)) return rejected("execution_port_not_ready");
  const ExecutionGoalKey key{controller_goal_id, controller_goal_generation};
  auto baseline = port->ReadSample();
  if (measured) {
    if (auto refusal = StartStateError(baseline, readiness.binding_epoch, motion.trajectory)) {
      return RejectedHandle(participant, controller_goal_id, controller_goal_generation,
                            std::move(*refusal));
    }
  }
  if (config_.observe_goal_context) {
    try { config_.observe_goal_context(key, participant, readiness.binding_epoch); } catch (...) {}
  }
  const auto dispatched_at = clock_.now();
  // Dispatched (or possibly sent): the goal is observed through the port.
  auto execution = std::make_shared<Execution>();
  execution->cancel_attempt->execution_id = prepared_membership_.active_execution_id();
  execution->generation = next_execution_generation_++;
  execution->key = key;
  execution->trajectory = motion.trajectory;
  execution->port = port_;
  execution->stop_helper = helper_;
  execution->epoch = readiness.binding_epoch;
  execution->association = std::make_shared<ExecutionSampleAssociation>(
      readiness.binding_epoch, baseline, dispatched_at);
  // Executing is published at dispatch.
  execution->snapshot.timestamp_ns = dispatched_at;
  execution->snapshot.state = ExecutionState::kExecuting;
  execution->snapshot.state_source = "execution_port";
  if (!execution->trajectory.waypoints.empty()) {
    execution->snapshot.joint_positions_rad = execution->trajectory.waypoints.front().positions_rad;
  }
  execution->dispatch_token =
      std::make_shared<ExecutionDispatchToken>(prepared_membership_.active_execution_id());
  execution->dispatch_pending = true;
  auto pending = std::make_shared<PendingDispatch>(key, participant.prepared_contract_digest,
                                                   execution->dispatch_token);
  const auto dispatch_helper = helper_;

  const auto generation = execution->generation;
  ExecutionHandle handle{
      [this, execution](std::int64_t timeout_ns) { return WaitForCompletion(execution, timeout_ns); },
      [this, execution] {
        std::lock_guard snapshot_lock(mutex_);
        return execution->snapshot;
      },
      generation,
      controller_goal_id,
      controller_goal_generation,
      participant.prepared_contract_digest};
  struct CompletionGuard {
    std::shared_ptr<PendingDispatch> pending;
    ExecutionPortDispatch dispatch{ExecutionPortDispatch::kEffectUncertain};
    bool finished{false};
    ~CompletionGuard() noexcept {
      if (finished) return;
      try { pending->token->Finish(dispatch); } catch (...) {}
      try {
        pending->result.set_exception(std::make_exception_ptr(
            std::runtime_error("pending dispatch completion failed")));
      } catch (...) {}
    }
  } completion{pending};
  dispatch_helper->BindDispatchedGoal(stop_goal_for(participant, key, execution->trajectory),
                                     execution->association, execution->cancel_attempt);
  pending_dispatch_ = pending;
  execution_ = execution;
  const auto dispatch_port = execution->port;
  lock.unlock();
  ExecutionPortCommandResultDTO sent;
  try {
    sent = dispatch_port->Submit(key, execution->trajectory, execution->dispatch_token);
  } catch (...) {
    sent.dispatch = execution->dispatch_token->state() == ExecutionDispatchToken::State::kAdmitted
                        ? ExecutionPortDispatch::kEffectUncertain
                        : ExecutionPortDispatch::kNotDispatched;
  }
  completion.dispatch = sent.dispatch;
  lock.lock();
  execution->dispatch_pending = false;
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    dispatch_helper->ForgetKnownUnsent(key);
    std::map<std::string, std::string> details;
    if (sent.refusal) details["port_refusal"] = refusal_name(*sent.refusal);
    if (sent.error) details["port_error"] = sent.error->message;
    auto result = rejected("execution_port_submit_refused", std::move(details));
    if (execution_ == execution) execution_.reset();
    if (pending_dispatch_ == pending) pending_dispatch_.reset();
    execution->dispatch_token->Finish(sent.dispatch);
    pending->result.set_value(result);
    completion.finished = true;
    return result;
  }
  progress_.invalidate();
  (void)progress_.publish(execution->generation, 0, dispatched_at, dispatched_at);

  if (lane_.supports_scale && !execution->stopping) {
    // The ramp of this motion starts from the factor last sent on this
    // epoch (the lane's initial factor when none was) at the trajectory's
    // prepared bounds; after a Stop the frozen factor is released here.
    std::lock_guard scale_lock(scale_mutex_);
    scale_frozen_ = false;
    preparation_scale_hold_.reset();
    // Linear slew at the trajectory's rate, at most 4/s; no
    // separate rate-change limit.
    ramp_ = ScaleRamp{last_sent_scale_.value_or(config_.initial_speed_factor), target_scale_,
                      std::min(execution->trajectory.runtime_scale_bounds->max_rate_per_s,
                               kMaxRuntimeScaleSlewPerS)};
    if (ramp_->value != ramp_->target) {
      last_ramp_tick_ = clock_.now();
      ScheduleRampTickLocked();
    }
  }

  prepared_goal_ = PreparedGoalRecord{controller_goal_id, controller_goal_generation,
                                      participant.prepared_contract_digest, handle};
  if (pending_dispatch_ == pending) pending_dispatch_.reset();
  execution->dispatch_token->Finish(sent.dispatch);
  pending->result.set_value(handle);
  completion.finished = true;
  return handle;
}

void ExecutionPortParticipant::AwaitFreshStartSample() {
  std::shared_ptr<IExecutionPort> port;
  std::size_t joints = 0U;
  {
    std::lock_guard lock(mutex_);
    port = port_;
    joints = lane_.joint_names.size();
  }
  if (!port) return;
  const auto deadline = clock_.now() + config_.completion_state_timeout_ns;
  while (true) {
    const auto sample = port->ReadSample();
    const auto now = clock_.now();  // after the sample, as in Evaluate
    if (sample && sample->binding_epoch == port->ReadReadiness().binding_epoch &&
        finite(sample->measured_positions_rad, joints) && sample->local_receipt_stamp_ns <= now &&
        now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns) {
      return;
    }
    if (now >= deadline) return;
    waiter_(deadline, std::stop_token{});
  }
}

std::optional<ErrorDTO> ExecutionPortParticipant::StartStateError(
    const std::optional<ExecutionControllerSampleDTO>& sample, const ExecutionBindingEpoch epoch,
    const TrajectoryDTO& trajectory) const {
  // As `main`'s observed-robot dispatch: a fresh complete sample first, then
  // the first waypoint within the start tolerance.
  const auto now = clock_.now();
  const bool fresh = sample && sample->binding_epoch == epoch &&
                     finite(sample->measured_positions_rad, lane_.joint_names.size()) &&
                     sample->local_receipt_stamp_ns <= now &&
                     now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns;
  if (!fresh) {
    return fault(ErrorCode::kExecutionRejected,
                 "No fresh complete controller state sample before dispatch",
                 "stale_controller_state",
                 {{"wait_for_state_timeout_s",
                   format(static_cast<double>(config_.completion_state_timeout_ns) / 1e9)},
                  {"required_joint_count", std::to_string(lane_.joint_names.size())}});
  }
  if (trajectory.waypoints.empty()) return std::nullopt;
  const auto observed =
      reorder(sample->measured_positions_rad, lane_.joint_names, trajectory.joint_names);
  if (!observed) {
    return fault(ErrorCode::kStartStateMismatch,
                 "Start state mismatch: the controller sample does not cover the trajectory joints",
                 "start_state_mismatch");
  }
  const auto& start = trajectory.waypoints.front().positions_rad;
  const auto count = std::min(observed->size(), start.size());
  double max_error = 0.0;
  std::size_t max_index = 0U;
  for (std::size_t index = 0U; index < count; ++index) {
    const double error = std::abs((*observed)[index] - start[index]);
    if (error > max_error) {
      max_error = error;
      max_index = index;
    }
  }
  if (max_error <= *config_.start_tolerance_rad) return std::nullopt;
  // E.g. a jog or Move-to target computed before the robot was
  // moved from its own interface. Refused before effect.
  return fault(ErrorCode::kStartStateMismatch,
               "Start state mismatch: the robot is not where this motion starts "
               "(max joint error " + format(max_error) + " rad exceeds tolerance " +
                   format(*config_.start_tolerance_rad) + "); preview again",
               "start_state_mismatch",
               {{"max_joint_error_rad", format(max_error)},
                {"start_tolerance_rad", format(*config_.start_tolerance_rad)},
                {"observed_position_rad", format((*observed)[max_index])},
                {"expected_position_rad", format(start[max_index])},
                {"joint_name", max_index < trajectory.joint_names.size()
                                   ? trajectory.joint_names[max_index]
                                   : "joint[" + std::to_string(max_index) + "]"}});
}

ExecutionHandle ExecutionPortParticipant::execute_trajectory(const TrajectoryDTO&) {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = ExecutionState::kFailed;
  snapshot.error = fault(ErrorCode::kExecutionRejected,
                         "The execution port participant executes prepared dispatches only",
                         "execution_port_prepared_dispatch_required");
  return ExecutionHandle{[](std::int64_t) { return true; }, [snapshot] { return snapshot; }, 0U};
}

Result<void> ExecutionPortParticipant::sync_observed_state_before_planning() {
  return state_authority_.sync_before_planning();
}

StopResultDTO ExecutionPortParticipant::stop() {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::nanoseconds(config_.stop.hard_stop_completion_timeout_ns);
  std::unique_lock lock(mutex_);
  if (!execution_ || execution_->terminal) {
    return StopResultDTO{false, ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
  }
  const auto execution = execution_;
  const auto pending = pending_dispatch_ && pending_dispatch_->key == execution->key
      ? pending_dispatch_ : std::shared_ptr<PendingDispatch>{};
  const auto fence = pending ? pending->token->Fence() : ExecutionDispatchToken::State::kAdmitted;
  lock.unlock();
  if (pending && fence == ExecutionDispatchToken::State::kFenced) {
    const auto answer = pending->token->WaitUntil(deadline);
    if (!answer || *answer == ExecutionPortDispatch::kNotDispatched)
      return StopResultDTO{false, fault(ErrorCode::kStopFailed,
          "The controller cancel was not dispatched", "hard_stop_cancel_not_dispatched")};
  }
  lock.lock();
  if (execution_ != execution || execution->terminal)
    return StopResultDTO{false, ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
  const auto sent = CancelExecution(*execution, lock, deadline);
  if (execution_ != execution)
    return StopResultDTO{false, ErrorDTO{kErrorInvalidRequest, "Execution changed during cancellation", {}}};
  if (execution_ == execution && !execution->terminal && execution->critical_fault) {
    // The Kernel's hard stop after a critical fault ends
    // this execution InterventionRequired with the fault.
    Finish(*execution, ExecutionState::kInterventionRequired, execution->critical_fault);
  }
  if (sent.error && sent.error->details.contains("reason") &&
      sent.error->details.at("reason") == "stop_evidence_phase_expired")
    return StopResultDTO{false, sent.error};
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    return StopResultDTO{
        false, fault(ErrorCode::kStopFailed, "The controller cancel was not dispatched",
                     "hard_stop_cancel_not_dispatched",
                     {{"port_refusal", sent.refusal ? refusal_name(*sent.refusal) : "none"}})};
  }
  return StopResultDTO{true, std::nullopt};
}

ResetResultDTO ExecutionPortParticipant::reset() {
  return ResetResultDTO{false,
                        fault(ErrorCode::kExecutionRejected,
                              "Reset is the Kernel's: its Stop stages, then motion-free retirement",
                              "execution_port_backend_reset_unsupported")};
}

ExecutionSnapshot ExecutionPortParticipant::get_execution_snapshot() const {
  std::lock_guard lock(mutex_);
  if (!execution_) {
    ExecutionSnapshot idle;
    idle.timestamp_ns = clock_.now();
    return idle;
  }
  return execution_->snapshot;
}

std::optional<double> ExecutionPortParticipant::stopped_trajectory_time_s(
    const std::uint64_t execution_generation) const {
  std::lock_guard lock(mutex_);
  if (!execution_ || execution_generation == 0U ||
      execution_->generation != execution_generation || !execution_->terminal ||
      stop_stages_in_flight_ != 0U) {
    return std::nullopt;
  }
  return execution_->stopped_cursor_s;
}

void ExecutionPortParticipant::retire_execution_progress_generation(
    const std::uint64_t execution_generation) noexcept {
  (void)progress_.invalidate_generation(execution_generation);
}

ExecutionProgressHazardClaimResult ExecutionPortParticipant::claim_execution_progress_hazard(
    const std::uint64_t execution_generation) noexcept {
  return progress_.claim_hazard_authority(execution_generation);
}

// --- Monitoring ---------------------------------------------------------------

bool ExecutionPortParticipant::WaitForCompletion(const std::shared_ptr<Execution>& execution,
                                                 const std::int64_t timeout_ns) {
  {
    std::unique_lock lock(mutex_);
    if (replacement_ && replacement_->execution == execution && execution_ != execution &&
        prepared_membership_.active_execution_id() != replacement_->candidate_id)
      return false;
    Evaluate(*execution, lock);
    if (execution->terminal) return true;
  }
  waiter_(clock_.now() + std::max<std::int64_t>(0, timeout_ns), std::stop_token{});
  std::unique_lock lock(mutex_);
  Evaluate(*execution, lock);
  return execution->terminal;
}

void ExecutionPortParticipant::Finish(Execution& execution, const ExecutionState state,
                                      std::optional<ErrorDTO> error) {
  execution.terminal = true;
  execution.published_progress_sample.reset();
  execution.snapshot.timestamp_ns = clock_.now();
  execution.snapshot.state = state;
  execution.snapshot.error = std::move(error);
  ++execution.snapshot.sequence_number;
  if (state == ExecutionState::kCompleted) {
    execution.snapshot.progress = 1.0;
    execution.stopped_cursor_s = duration_s(execution.trajectory);
    // Completion against a runtime collision hazard: the first authority
    // boundary wins (`ExecutionProgressPublication`).
    const auto now = clock_.now();
    (void)progress_.publish_terminal(
        execution.generation, static_cast<std::int64_t>(*execution.stopped_cursor_s * 1e9), now,
        now);
  } else {
    (void)progress_.invalidate_generation(execution.generation);
  }
  std::lock_guard scale_lock(scale_mutex_);
  ++ramp_generation_;
  if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
  ramp_handle_ = 0U;
  ramp_.reset();
}

std::optional<double> ExecutionPortParticipant::ObserveTiming(IExecutionPort& port,
                                                              const ExecutionBindingEpoch epoch) {
  // Controller lateness (controller-manager periodicity) is kept apart
  // from telemetry loss (`controller_state` stamp gaps); best effort.
  const auto timing = port.ReadTiming();
  if (timing.binding_epoch != epoch) return std::nullopt;
  ++timing_.windows;
  std::optional<double> late;
  if (timing.period_max_s && std::isfinite(*timing.period_max_s)) {
    timing_.max_period_s = std::max(timing_.max_period_s, *timing.period_max_s);
    if (*timing.period_max_s > config_.late_cycle_margin_s) {
      ++timing_.windows_late_beyond_margin;
      late = *timing.period_max_s;
    }
  }
  if (timing.max_state_stamp_gap_s && std::isfinite(*timing.max_state_stamp_gap_s)) {
    timing_.max_state_stamp_gap_s =
        std::max(timing_.max_state_stamp_gap_s, *timing.max_state_stamp_gap_s);
  }
  if (timing.received_samples) timing_.received_samples += *timing.received_samples;
  return late;
}

void ExecutionPortParticipant::RecordRecognition(Execution& execution) {
  if (execution.recognition_recorded) return;
  const auto delay = execution.association->recognition_delay_ns();
  if (!delay) return;
  execution.recognition_recorded = true;
  ++timing_.goals_recognized;
  timing_.last_recognition_delay_ns = *delay;
  timing_.max_recognition_delay_ns = std::max(timing_.max_recognition_delay_ns, *delay);
}

void ExecutionPortParticipant::ObserveSample(Execution& execution,
                                             const ExecutionControllerSampleDTO& sample,
                                             const double original_cursor_s, const TimePoint now,
                                             const bool publish_progress) {
  if (execution_ && execution_.get() == &execution && execution.scale_hold_until_cursor_s &&
      !execution.stopping && !execution.terminal &&
      original_cursor_s >= *execution.scale_hold_until_cursor_s) {
    std::lock_guard scale_lock(scale_mutex_);
    execution.scale_hold_until_cursor_s.reset();
    if (ramp_ && execution.trajectory.runtime_scale_bounds) {
      ramp_->rate_per_s = std::min(execution.trajectory.runtime_scale_bounds->max_rate_per_s,
                                   kMaxRuntimeScaleSlewPerS);
      ramp_->target = target_scale_;
      trace_scale_hold("released", prepared_membership_.active_execution_id().value_or(0U), execution.key,
                       ramp_->value, original_cursor_s, ramp_->rate_per_s);
      if (!scale_frozen_ && !preparation_scale_hold_ && ramp_->value != ramp_->target) {
        last_ramp_tick_ = now;
        ScheduleRampTickLocked();
      }
    }
  }
  const auto total = duration_s(execution.trajectory);
  execution.cursor_s = std::clamp(original_cursor_s, 0.0, total);
  if (publish_progress) {
    auto published_sample = sample;
    const auto published =
        progress_.publish(execution.generation, static_cast<std::int64_t>(execution.cursor_s * 1e9),
                          sample.source_stamp_ns, sample.local_receipt_stamp_ns);
    execution.RecordProgressSample(published, std::move(published_sample));
  }
  execution.snapshot.timestamp_ns = now;
  ++execution.snapshot.sequence_number;
  execution.snapshot.progress = total > 0.0 ? execution.cursor_s / total : 1.0;
  // Commanded state on commanded lanes, measured on real ones.
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (auto positions =
          reorder(measured ? sample.measured_positions_rad : sample.reference_positions_rad,
                  lane_.joint_names, execution.trajectory.joint_names)) {
    execution.snapshot.joint_positions_rad = std::move(*positions);
  }
  execution.snapshot.joint_velocities_rad_s.clear();
  if (measured && sample.measured_velocities_rad_s) {
    if (auto velocities = reorder(*sample.measured_velocities_rad_s, lane_.joint_names,
                                  execution.trajectory.joint_names)) {
      execution.snapshot.joint_velocities_rad_s = std::move(*velocities);
    }
  }
  execution.snapshot.source_timestamp_ns = sample.local_receipt_stamp_ns;
  execution.snapshot.source_age_ns = std::max<std::int64_t>(0, now - sample.local_receipt_stamp_ns);
}

void ExecutionPortParticipant::Evaluate(Execution& execution, std::unique_lock<std::mutex>& lock) {
  if (execution.dispatch_pending) return;
  if (execution.terminal) return;
  const auto cancel = [&] {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(config_.stop.hard_stop_completion_timeout_ns);
    (void)CancelExecution(execution, lock, deadline);
    return !execution.terminal && !execution.stopping && stop_stages_in_flight_ == 0U &&
        (execution_.get() == &execution || (replacement_ && replacement_->execution.get() == &execution));
  };
  auto& port = *execution.port;
  // The sample before `now`: a receipt stamped (same monotonic clock) between
  // the two reads must not look newer than `now` and fail the freshness check.
  const auto joints = lane_.joint_names.size();
  const auto sample = port.ReadSample();
  const auto now = clock_.now();
  const bool usable = sample && sample->binding_epoch == execution.epoch &&
                      std::isfinite(sample->cursor_s) &&
                      finite(sample->reference_positions_rad, joints) &&
                      finite(sample->measured_positions_rad, joints);

  if (execution.stopping) {
    // The Kernel's Stop owns the motion: no tracking or completion verdict.
    // Progress continues on the original path, through the Stop
    // planner's own mapping once a replacement runs (frozen after its hold).
    if (usable) {
      if (const auto mapped = helper_->OriginalPathCursor(sample)) {
        ObserveSample(execution, *sample, original_path_cursor(execution.delivery_clock, *mapped),
                      now);
      } else if (const auto goal = port.ReadGoal(execution.key);
                 goal && goal->phase != ExecutionGoalPhase::kTerminal &&
                 execution.association->Observe(*sample, goal)) {
        ObserveSample(execution, *sample,
                      original_path_cursor(execution.delivery_clock, sample->cursor_s), now);
      }
    }
    RecordRecognition(execution);
    if (stop_stages_in_flight_ != 0U || !execution.last_stage_outcome.has_value()) return;
    if (*execution.last_stage_outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
      const auto original = port.ReadGoal(execution.key);
      Finish(execution, ExecutionState::kStopped, std::nullopt);
      // Continuation: the original path's end when the original goal
      // itself completed (natural completion inside the Stop), else where the
      // on-path replacement left it; unknown after a hard stop.
      execution.stopped_cursor_s =
          original && original->result == ExecutionGoalResult::kSucceeded
              ? std::optional(duration_s(execution.trajectory))
              : helper_->OriginalPathCursor(std::nullopt);
    } else if (!helper_->HasLiveGoal()) {
      Finish(execution, ExecutionState::kInterventionRequired, execution.last_stage_error);
    }
    return;
  }

  if (execution.critical_fault) throw ExecutionCriticalFault(*execution.critical_fault);
  if (const auto late = ObserveTiming(port, execution.epoch)) {
    // A controller cycle later than the lane margin is a critical fault
    // handled like any critical fault: the Kernel issues the lane's hard stop and the
    // operation ends InterventionRequired.
    execution.critical_fault =
        fault(ErrorCode::kInternalError,
              "A controller cycle was later than the lane margin: " + format(*late) + " s",
              "late_control_cycle",
              {{"controller_period_s", format(*late)},
               {"late_cycle_margin_s", format(config_.late_cycle_margin_s)}});
    throw ExecutionCriticalFault(*execution.critical_fault);
  }
  const auto readiness = port.ReadReadiness();
  const auto goal = port.ReadGoal(execution.key);
  (void)execution.association->ObserveGoal(goal);
  RecordRecognition(execution);
  if (readiness.binding_epoch != execution.epoch || !ready(readiness)) {
    // As `main`: the controller binding was lost under an active goal.
    // Sending cancel does not prove this dispatched command has settled.
    if (!cancel()) return;
    Finish(execution, ExecutionState::kInterventionRequired,
           fault(ErrorCode::kExecutionAborted,
                 "Controller binding or lifecycle readiness was lost during execution",
                 "controller_binding_lost",
                 {{"binding_epoch", std::to_string(readiness.binding_epoch.value())},
                  {"dispatch_binding_epoch", std::to_string(execution.epoch.value())}}));
    return;
  }

  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  const bool pending_replacement = helper_->HasPendingReplacement();
  const bool goal_running = (goal && goal->phase != ExecutionGoalPhase::kTerminal) ||
                            (pending_replacement && helper_->HasLiveGoal());
  // As `main`'s `joint_state_max_age_s`: during the motion the newest
  // controller state must be younger than the lane's maximum age; stale state
  // ends the motion like `main`'s other in-motion state faults (the goal is
  // cancelled and the execution fails).
  if (goal_running && config_.stop.controller_state_max_age_ns > 0 &&
      !(sample && sample->binding_epoch == execution.epoch &&
        sample->local_receipt_stamp_ns <= now &&
        now - sample->local_receipt_stamp_ns <= config_.stop.controller_state_max_age_ns)) {
    if (!cancel()) return;
    Finish(execution, ExecutionState::kInterventionRequired,
           fault(ErrorCode::kExecutionAborted,
                 "The controller state is older than the lane's maximum age during execution",
                 "stale_controller_state",
                 {{"max_age_s",
                   format(static_cast<double>(config_.stop.controller_state_max_age_ns) / 1e9)},
                  {"sample_age_s",
                   sample ? format(static_cast<double>(now - sample->local_receipt_stamp_ns) / 1e9)
                          : std::string("none")}}));
    return;
  }
  bool replacement_sample = false;
  if (usable && goal_running && pending_replacement && replacement_ &&
      replacement_->execution->association)
    replacement_sample = replacement_->execution->association->Observe(
        *sample, port.ReadGoal(replacement_->execution->key));
  // A newer goal's own anchor ends attribution to the predecessor even if
  // the predecessor's result callback has not arrived yet.
  const bool own_sample = usable && goal && goal->phase != ExecutionGoalPhase::kTerminal &&
                          !replacement_sample && execution.association->Observe(*sample, goal);
  if (own_sample || replacement_sample) {
    // Progress cursor: the controller's own cursor of this goal.
    if (own_sample)
      ObserveSample(execution, *sample,
                    original_path_cursor(execution.delivery_clock, sample->cursor_s), now);

    // As `main`'s host monitor: tolerance and timeout on the
    // controller-reported error, at the sample rate.
    if (config_.tracking && config_.tracking->timeout_s > 0.0 &&
        finite(sample->error_positions_rad, joints)) {
      std::size_t peak = 0U;
      for (std::size_t index = 1U; index < joints; ++index) {
        if (std::abs(sample->error_positions_rad[index]) >
            std::abs(sample->error_positions_rad[peak])) {
          peak = index;
        }
      }
      const double error = joints == 0U ? 0.0 : std::abs(sample->error_positions_rad[peak]);
      if (error > config_.tracking->tolerance_rad) {
        if (!execution.tracking_breach_since) execution.tracking_breach_since = now;
        if (static_cast<double>(now - *execution.tracking_breach_since) / 1e9 >=
            config_.tracking->timeout_s) {
          if (!cancel()) return;
          Finish(execution, ExecutionState::kInterventionRequired,
                 fault(ErrorCode::kExecutionAborted,
                       "Controller tracking error exceeded tolerance: " + format(error) + " rad",
                       "tracking_error_exceeded",
                       {{"tracking_error_rad", format(error)},
                        {"tracking_error_tolerance_rad", format(config_.tracking->tolerance_rad)},
                        {"tracking_error_timeout_s", format(config_.tracking->timeout_s)},
                        {"trajectory_elapsed_s", format(sample->cursor_s)},
                        {"joint_name", lane_.joint_names[peak]},
                        {"signed_error_rad", format(sample->error_positions_rad[peak])}}));
          return;
        }
      } else {
        execution.tracking_breach_since.reset();
      }
    }
  }

  if (!goal) {
    if (pending_replacement && !cancel()) return;
    Finish(execution, ExecutionState::kInterventionRequired,
           fault(ErrorCode::kExecutionAborted, "The controller no longer reports the goal",
                 "controller_result_failed", {{"goal_result", "unavailable"}}));
    return;
  }
  if (goal->phase != ExecutionGoalPhase::kTerminal || !goal->result) return;
  const auto result = *goal->result;
  if (pending_replacement) {
    if (result == ExecutionGoalResult::kPreemptedBy && replacement_ &&
        goal->preempted_by == replacement_->execution->key)
      return;
    if (result == ExecutionGoalResult::kSucceeded && !helper_->CanCompleteNaturally(execution.key))
      return;
    if (result != ExecutionGoalResult::kSucceeded) {
      if (!cancel()) return;
      Finish(execution, ExecutionState::kInterventionRequired,
             fault(ErrorCode::kExecutionAborted,
                   std::string("FollowJointTrajectory goal failed: ") + to_string(result),
                   "controller_result_failed",
                   {{"goal_result", to_string(result)},
                    {"mechanism_goal_id", goal->mechanism_goal_id.value()}}));
      return;
    }
  }
  if (result == ExecutionGoalResult::kCanceled) {
    // No addressed Stop owns it: the Kernel reports it.
    Finish(execution, ExecutionState::kStopped, std::nullopt);
    return;
  }
  if (result != ExecutionGoalResult::kSucceeded) {
    // As `main`: any unsuccessful controller result fails the motion; on real
    // lanes this includes a goal-tolerance violation.
    Finish(execution,
           result == ExecutionGoalResult::kEndedUnknown ? ExecutionState::kInterventionRequired
                                                        : ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 std::string("FollowJointTrajectory goal failed: ") + to_string(result),
                 "controller_result_failed",
                 {{"goal_result", to_string(result)},
                  {"mechanism_goal_id", goal->mechanism_goal_id.value()}}));
    return;
  }
  // Completion: kSucceeded plus `main`'s authority check. Every lane
  // keeps its arrival tolerance as the JTC `goal` constraint, so kSucceeded
  // already means finished and within it; a commanded lane needs no second
  // check, the measured lane keeps `main`'s terminal gate on its own sample.
  const auto& final_positions = execution.trajectory.waypoints.back().positions_rad;
  if (!measured) {
    execution.snapshot.joint_positions_rad = final_positions;
    Finish(execution, ExecutionState::kCompleted, std::nullopt);
    return;
  }
  if (!execution.success_observed_at) execution.success_observed_at = now;
  const auto result_at = goal->terminal_local_receipt_stamp.value_or(*execution.success_observed_at);
  // A sample received after the result (as `main`) and newer by source
  // stamp than the goal's newest evidence (or, with none, than the newest
  // sample seen before the goal was sent).
  if (usable && sample->local_receipt_stamp_ns > result_at &&
      execution.association->NewerThanEvidence(*sample)) {
    const auto positions =
        reorder(sample->measured_positions_rad, lane_.joint_names, execution.trajectory.joint_names);
    // The one "robot at target" rule: the controller reported success (it
    // checked the same arrival tolerance as its `goal` constraint) and this
    // fresh measured sample lies within the lane's arrival tolerance.
    // TODO: remove this measured re-check after the live validation of the
    // kept JTC goal tolerance on Real; until then it stays, reading the same
    // lane value.
    const auto at_target =
        positions ? robot_at_target(true, *positions, final_positions, *config_.final_tolerance_rad)
                  : RobotAtTargetDTO{};
    const double max_error = at_target.max_error_rad;
    if (positions) execution.snapshot.joint_positions_rad = *positions;
    if (at_target.arrived) {
      Finish(execution, ExecutionState::kCompleted, std::nullopt);
    } else {
      Finish(execution, ExecutionState::kFailed,
             fault(ErrorCode::kExecutionAborted,
                   "Final state mismatch: max joint error " + format(max_error) +
                       " rad exceeds tolerance " + format(*config_.final_tolerance_rad),
                   "tracking_error_exceeded",
                   {{"phase", "terminal_result_state_gate"},
                    {"final_tolerance_rad", format(*config_.final_tolerance_rad)}}));
    }
    return;
  }
  if (now - result_at >= config_.completion_state_timeout_ns) {
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 "No fresh controller sample was received after the successful result",
                 "stale_controller_state",
                 {{"phase", "terminal_result_state_gate"},
                  {"wait_for_state_timeout_s",
                   format(static_cast<double>(config_.completion_state_timeout_ns) / 1e9)},
                  {"final_tolerance_rad", format(*config_.final_tolerance_rad)}}));
  }
}

// --- Stop -----------------------------------------------------------------------

ParticipantStopStageResultDTO ExecutionPortParticipant::TranslateStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation) {
  return TranslateStopStageImpl(stage, budget, cancellation, nullptr);
}

ParticipantStopStageResultDTO ExecutionPortParticipant::TranslateStopStageForTakeover(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation, IStopStageSpringReturnObserver& observer) {
  return TranslateStopStageImpl(stage, budget, cancellation, &observer);
}

ParticipantStopStageResultDTO ExecutionPortParticipant::TranslateStopStageImpl(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation, IStopStageSpringReturnObserver* observer) {
  const auto wall_received = std::chrono::steady_clock::now();
  const auto received_at = clock_.now();
  const auto initial_leaf_budget = std::max<std::int64_t>(0,
      budget.remaining_controlled_deadline_ns.value_or(config_.stop.hard_stop_completion_timeout_ns));
  const auto leaf_deadline = wall_received + std::chrono::nanoseconds(initial_leaf_budget);
  // Copy before participant state changes: allocation failure has no effect.
  auto timed_budget = budget;
  std::shared_ptr<ParticipantStopStageHelper> helper;
  std::shared_ptr<Execution> execution;
  std::optional<std::uint64_t> fence_sequence;
  std::shared_ptr<PendingDispatch> fenced_pending;
  std::shared_ptr<ExecutionDispatchToken> fenced_replacement;
  {
    std::lock_guard lock(mutex_);
    execution = execution_;
    const ExecutionGoalKey stage_goal{stage.controller_goal_id, stage.controller_goal_generation};
    helper = execution && (!execution->terminal || execution->key == stage_goal)
                 ? execution->stop_helper : helper_;
    ++stop_stages_in_flight_;
    ++stop_stages_started_;
    if (execution && !execution->terminal) execution->stopping = true;
    // No live execution and none for this stage's goal: that goal was
    // never sent. Fence it under this lock (Dispatch rechecks it, and the
    // started count across its unlocked tool-context send), so it never goes
    // out after this Stop. No other Stop stage of this participant runs.
    if (pending_dispatch_ && pending_dispatch_->key == stage_goal &&
        pending_dispatch_->token->Fence() == ExecutionDispatchToken::State::kFenced) {
      fenced_pending = pending_dispatch_;
      stopped_before_dispatch_ = stage_goal;
      if (stop_stages_in_flight_ == 1U) fence_sequence = stop_stages_started_;
    }
    if ((!execution || (execution->terminal && execution->key != stage_goal)) && !replacement_) {
      stopped_before_dispatch_ = stage_goal;
      if (stop_stages_in_flight_ == 1U) fence_sequence = stop_stages_started_;
    }
    // A moving replacement send in flight (DispatchReplacement, unlocked).
    // Fenced here: the port never sends it; wait below until its sender has
    // removed it from the sent set. Already admitted: it may be sent and is in
    // the sent set, so this stage stops it together with the source.
    if (replacement_ && replacement_->execution->dispatch_pending &&
        replacement_->execution->dispatch_token->Fence() ==
            ExecutionDispatchToken::State::kFenced)
      fenced_replacement = replacement_->execution->dispatch_token;
  }
  // Both waits are bounded by this stage's deadline; the fenced sender holds
  // no lock and does not send.
  const auto unsettled = [&] {
    if (fenced_pending) {
      const auto answer = fenced_pending->token->WaitUntil(leaf_deadline);
      if (!answer || *answer != ExecutionPortDispatch::kNotDispatched) return true;
    }
    return fenced_replacement && !fenced_replacement->WaitUntil(leaf_deadline);
  };
  if (unsettled()) {
    const auto error = fault(ErrorCode::kMotionInterruptBackendStopFailed,
                             "Pending dispatch did not establish a no-send result before Stop deadline",
                             "stop_evidence_phase_expired");
    ParticipantStopStageResultDTO result{stage, StopStageAttemptDispositionDTO::kEffectUncertain,
        ParticipantStopStageOutcomeDTO::kFailed, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::max<Duration>(0, clock_.now() - received_at), {}, error};
    if (stage.stage_index < stage.participant_stop_plan.stage_commands.size()) {
      const auto& command = stage.participant_stop_plan.stage_commands[stage.stage_index];
      for (const auto& item : command.resolved_stage.evidence_requirements)
        result.evidence_items.push_back(StopEvidenceItemDTO{
            item.requirement.evidence_item_id, item.requirement.evidence_kind,
            StopEvidenceItemStateDTO::kMissing, item.resolved_source, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, error});
    }
    std::lock_guard lock(mutex_);
    --stop_stages_in_flight_;
    return result;
  }
  if (fence_sequence && helper && !cancellation.stop_requested() &&
      (fenced_pending || !helper->HasLiveGoal())) {
    if (auto contained = no_goal_containment(stage, clock_, received_at, *fence_sequence)) {
      std::lock_guard lock(mutex_);
      --stop_stages_in_flight_;
      return std::move(*contained);
    }
  }
  // The helper's trip and occurrence remain in its physical delivery clock.
  // Only this participant owns the exact mapping back to the sealed curve.
  class CanonicalSpringObserver final : public IStopStageSpringReturnObserver {
   public:
    CanonicalSpringObserver(ExecutionPortParticipant& owner, std::shared_ptr<Execution> source,
                            IStopStageSpringReturnObserver* receiver)
        : owner_(owner), source_(std::move(source)), receiver_(receiver) {}
    bool SpringReturnPointReady(const SpringBackReturnPointDTO& point) noexcept override {
      if (!receiver_) return false;
      try {
        double canonical;
        {
          std::lock_guard lock(owner_.mutex_);
          if (!source_ || owner_.execution_ != source_ || owner_.stop_stages_in_flight_ == 0U ||
              point.source_goal != source_->key || point.binding_epoch != source_->epoch ||
              !std::isfinite(point.initial.command_path_time_s))
            return false;
          canonical =
              original_path_cursor(source_->delivery_clock,
                  point.initial.spring_back_return_path_time_s.value_or(
                      point.initial.command_path_time_s));
        }
        if (!std::isfinite(canonical)) return false;
        auto qualified = point;
        qualified.canonical_return_path_time_s = canonical;
        // The downstream observer cannot plan/dispatch and is never called
        // while the participant mutex is held.
        return receiver_->SpringReturnPointReady(qualified);
      } catch (...) {
        return false;  // Missing optional progress never changes containment.
      }
    }

   private:
    ExecutionPortParticipant& owner_;
    std::shared_ptr<Execution> source_;
    IStopStageSpringReturnObserver* receiver_;
  } qualified_observer(*this, execution, observer);
  timed_budget.remaining_controlled_deadline_ns = std::max<std::int64_t>(0,
      std::chrono::duration_cast<std::chrono::nanoseconds>(leaf_deadline -
          std::chrono::steady_clock::now()).count());
  if (timed_budget.stop_ingress_age_ns && *timed_budget.stop_ingress_age_ns >= 0 &&
      *timed_budget.stop_ingress_age_ns <= kMaximumStopDurationNs) {
    // Saturate to an invalid age rather than overflow on malformed timing.
    *timed_budget.stop_ingress_age_ns += std::min<Duration>(
        std::max<Duration>(0, clock_.now() - received_at),
        kMaximumStopDurationNs + 1 - *timed_budget.stop_ingress_age_ns);
  }
  auto result = helper->TranslateStopStageWithDeadline(
      stage, timed_budget, cancellation, observer ? &qualified_observer : nullptr, leaf_deadline);
  std::unique_lock lock(mutex_);
  if (replacement_ && prepared_membership_.active_execution_id() != replacement_->candidate_id &&
      result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
    replacement_->execution->terminal = true;
    replacement_->execution->snapshot.state = ExecutionState::kStopped;
    replacement_->execution->snapshot.timestamp_ns = clock_.now();
  }
  --stop_stages_in_flight_;
  if (execution && execution->stopping) {
    execution->last_stage_outcome = result.outcome;
    execution->last_stage_error = result.error;
    Evaluate(*execution, lock);
  }
  return result;
}

// --- Speed factor ------------------------------------------

void ExecutionPortParticipant::set_runtime_velocity_scale(const double scale) {
  apply_runtime_velocity_scale(scale, 0U);
}

void ExecutionPortParticipant::apply_runtime_velocity_scale(double scale,
                                                            const std::uint64_t sequence) {
  scale = std::isfinite(scale) ? std::clamp(scale, 0.0, 1.0) : 0.0;
  std::unique_lock lock(mutex_);
  if (!lane_.supports_scale) return;
  std::unique_lock scale_lock(scale_mutex_);
  if (scheduler_detach_started_) return;
  // OD-72: latest value wins. The ramp target is the one store: a motion
  // registering its ramp reads it under this same leaf lock, so no accepted
  // value is lost; an older value arriving after a newer one changes nothing.
  if (sequence != 0U) {
    if (sequence < applied_scale_sequence_) return;
    applied_scale_sequence_ = sequence;
  }
  target_scale_ = scale;
  if (scale_frozen_ || preparation_scale_hold_ ||
      (execution_ && execution_->scale_hold_until_cursor_s)) return;  // target retained
  if (ramp_) {
    ramp_->target = scale;
    if (ramp_->value != ramp_->target && ramp_handle_ == 0U) {
      last_ramp_tick_ = clock_.now();
      ScheduleRampTickLocked();
    }
    return;
  }
  // No motion: nothing moves, the value applies to the next goal's start.
  const auto port = port_;
  if (!port) return;
  const auto port_sequence = port->ReserveScaleSequence();
  scale_lock.unlock();
  lock.unlock();
  const auto result = port->SetScale(scale, port_sequence);
  lock.lock();
  scale_lock.lock();
  if (port_ == port && port->IsLatestScaleSequence(port_sequence) &&
      result.dispatch != ExecutionPortDispatch::kNotDispatched)
    last_sent_scale_ = scale;
}

void ExecutionPortParticipant::ScheduleRampTickLocked() {
  if (scheduler_detach_started_) return;
  const auto period_ns = static_cast<Duration>(1e9 / lane_.controller_update_rate_hz);
  const auto generation = ramp_generation_;
  ramp_handle_ = scheduler_.schedule(
      clock_.now() + std::max<Duration>(1, period_ns),
      [this, generation] { RampTick(generation); }, SchedulerPriority::ControllerCommand);
}

void ExecutionPortParticipant::RampTick(const std::uint64_t ramp_generation) {
  std::unique_lock lock(mutex_);
  std::unique_lock scale_lock(scale_mutex_);
  if (ramp_generation != ramp_generation_) return;
  ramp_handle_ = 0U;
  if (scale_frozen_ || preparation_scale_hold_ ||
      (execution_ && execution_->scale_hold_until_cursor_s) || !ramp_) return;
  // One value per host tick, computed from the time at this tick (no backlog).
  const auto now = clock_.now();
  const double step =
      ramp_->rate_per_s * static_cast<double>(std::max<Duration>(0, now - last_ramp_tick_)) / 1e9;
  last_ramp_tick_ = now;
  ramp_->value = ramp_->value < ramp_->target ? std::min(ramp_->target, ramp_->value + step)
                                              : std::max(ramp_->target, ramp_->value - step);
  const auto port = port_;
  const auto value = ramp_->value;
  if (!port) return;
  const auto sequence = port->ReserveScaleSequence();
  scale_lock.unlock();
  lock.unlock();
  const auto result = port->SetScale(value, sequence);
  lock.lock();
  scale_lock.lock();
  if (port_ == port && port->IsLatestScaleSequence(sequence) &&
      result.dispatch != ExecutionPortDispatch::kNotDispatched)
    last_sent_scale_ = value;
  if (ramp_generation == ramp_generation_ && ramp_ && !scale_frozen_ &&
      !preparation_scale_hold_ && ramp_handle_ == 0U && ramp_->value != ramp_->target)
    ScheduleRampTickLocked();
}

std::optional<double> ExecutionPortParticipant::FreezeSpeedFactor(
    IExecutionPort& port, std::chrono::steady_clock::time_point deadline) {
  double fallback;
  {
    std::lock_guard scale_lock(scale_mutex_);
    scale_frozen_ = true;
    (void)port.ReserveScaleSequence();
    ++ramp_generation_;
    if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
    ramp_handle_ = 0U;
    fallback = last_sent_scale_.value_or(config_.initial_speed_factor);
  }
  const auto snapshot = port.SnapshotScaleAfterFence(deadline);
  if (snapshot.status == ScaleSnapshot::Status::kDeadlineExpired) return std::nullopt;
  return snapshot.status == ScaleSnapshot::Status::kFactor ? snapshot.factor : fallback;
}

RuntimeScaleMechanismAvailability ExecutionPortParticipant::runtime_velocity_scale_availability()
    const noexcept {
  const auto view = scale_availability_view_.load(std::memory_order_acquire);
  if (!view || !view->supports_scale) return RuntimeScaleMechanismAvailability::kLaneUnsupported;
  const auto listening = view->port ? view->port->ScaleInputListening() : std::nullopt;
  return listening.value_or(true) ? RuntimeScaleMechanismAvailability::kAvailable
                                  : RuntimeScaleMechanismAvailability::kControllerNotListening;
}

RuntimeScaleMechanismStatusDTO ExecutionPortParticipant::runtime_velocity_scale_status() const {
  const auto availability = runtime_velocity_scale_availability();
  std::lock_guard lock(mutex_);
  RuntimeScaleMechanismStatusDTO status;
  status.supported = lane_.supports_scale;
  if (availability == RuntimeScaleMechanismAvailability::kControllerNotListening) {
    status.error = ErrorDTO{std::string(to_string(ErrorCode::kUnsupportedCapability)),
                            "the controller is not listening for runtime scale input",
                            {{"reason", kRuntimeScaleControllerNotListening}}};
  }
  status.active = execution_ && !execution_->terminal;
  {
    std::lock_guard scale_lock(scale_mutex_);
    status.commanded_scale = target_scale_;
  }
  // Published is not applied: the applied factor is the controller's own
  // report; before the first report the epoch's initial factor applies.
  status.applied_scale = config_.initial_speed_factor;
  const auto sample = port_ ? port_->ReadSample() : std::nullopt;
  if (sample && sample->applied_factor && std::isfinite(*sample->applied_factor) &&
      sample->binding_epoch == port_->ReadReadiness().binding_epoch) {
    status.applied_scale = std::clamp(*sample->applied_factor, 0.0, 1.0);
    status.acknowledgement_sequence = sample->local_receipt_count.value();
    status.acknowledgement_timestamp_ns = sample->local_receipt_stamp_ns;
  }
  if (execution_) status.virtual_trajectory_time_s = execution_->cursor_s;
  return status;
}

}  // namespace manyforge::core
