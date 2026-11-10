#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <manyforge_core/execution_sample_association.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/participant_stop_stage_helper.hpp>
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::core {
namespace {

constexpr std::size_t kMaximumStopStageErrorDetails = 8U;

std::string format(double value) { return std::to_string(value); }

/// The Kernel's bounded error shape (at most 8 details of bounded size): keep
/// the decision-relevant keys first, as the native backend did.
ErrorDTO stage_error(std::string reason, std::string message,
                     const std::map<std::string, std::string>& details = {}) {
  std::map<std::string, std::string> source = details;
  source.insert_or_assign("reason", std::move(reason));
  std::map<std::string, std::string> projected;
  const auto add = [&](const std::string& key) {
    const auto found = source.find(key);
    if (found == source.end() || projected.size() >= kMaximumStopStageErrorDetails - 1U ||
        key.empty() || key.size() > 64U || key.find('\0') != std::string::npos) {
      return;
    }
    auto value = found->second;
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());
    if (value.size() > 256U) value.resize(256U);
    projected.emplace(found->first, std::move(value));
  };
  constexpr std::array preferred{"reason",
                                 "recommended_disposition",
                                 "stop_disposition",
                                 "trigger_reason",
                                 "validation_failure_reason",
                                 "retimer_validation_failure_reason",
                                 "ali_validation_failure_reason",
                                 "handoff_path_time_s",
                                 "joint"};
  for (const auto* key : preferred) add(key);
  for (const auto& [key, value] : source) add(key);
  if (projected.size() < source.size()) projected.insert_or_assign("details_truncated", "true");
  message.erase(std::remove(message.begin(), message.end(), '\0'), message.end());
  if (message.empty()) message = "Stop stage failed";
  if (message.size() > 1024U) message.resize(1024U);
  return ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed), std::move(message),
                  std::move(projected)};
}

ErrorDTO stage_error(const ErrorDTO& cause, std::string fallback_reason) {
  const auto reason = cause.details.find("reason");
  return stage_error(reason == cause.details.end() ? std::move(fallback_reason) : reason->second,
                     cause.message, cause.details);
}

const PreparedStopStageCommandDTO& selected_command(const ResolvedParticipantStopStageDTO& stage) {
  return stage.participant_stop_plan.stage_commands[stage.stage_index];
}

std::vector<StopEvidenceItemDTO> unsatisfied_items(const PreparedStopStageCommandDTO& command,
                                                   StopEvidenceItemStateDTO state,
                                                   const ErrorDTO& error) {
  std::vector<StopEvidenceItemDTO> items;
  for (const auto& requirement : command.resolved_stage.evidence_requirements) {
    items.push_back(StopEvidenceItemDTO{requirement.requirement.evidence_item_id,
                                        requirement.requirement.evidence_kind, state,
                                        requirement.resolved_source, std::nullopt, std::nullopt,
                                        std::nullopt, std::nullopt, error});
  }
  return items;
}

ParticipantStopStageResultDTO failed_result(const ResolvedParticipantStopStageDTO& stage,
                                            StopStageAttemptDispositionDTO disposition,
                                            ErrorDTO error, std::int64_t elapsed_ns) {
  ParticipantStopStageResultDTO result{stage,
                                       disposition,
                                       ParticipantStopStageOutcomeDTO::kFailed,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::max<std::int64_t>(0, elapsed_ns),
                                       {},
                                       std::nullopt};
  if (stage.stage_index < stage.participant_stop_plan.stage_commands.size()) {
    result.evidence_items =
        unsatisfied_items(selected_command(stage), StopEvidenceItemStateDTO::kMissing, error);
  }
  result.error = std::move(error);
  return result;
}

ParticipantStopStageResultDTO superseded_result(const ResolvedParticipantStopStageDTO& stage) {
  const ErrorDTO error{"stop_stage_superseded_before_dispatch",
                       "Stop stage was cancelled before dispatch",
                       {}};
  auto result = failed_result(stage, StopStageAttemptDispositionDTO::kNotDispatched, error, 0);
  result.outcome = ParticipantStopStageOutcomeDTO::kSupersededNoEffect;
  for (auto& item : result.evidence_items) item.state = StopEvidenceItemStateDTO::kInapplicable;
  return result;
}

bool terminal(const std::optional<ExecutionGoalObservationDTO>& goal) {
  return !goal.has_value() || goal->phase == ExecutionGoalPhase::kTerminal;
}

bool finite_vector(const std::vector<double>& values, std::size_t size) {
  return values.size() == size &&
         std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

/// A complete, finite sample of the goal's binding epoch in lane joint order.
bool usable_sample(const ExecutionControllerSampleDTO& sample, std::size_t joints,
                   ExecutionBindingEpoch epoch) {
  return sample.binding_epoch == epoch && std::isfinite(sample.cursor_s) &&
         finite_vector(sample.reference_positions_rad, joints) &&
         finite_vector(sample.reference_velocities_rad_s, joints) &&
         finite_vector(sample.measured_positions_rad, joints);
}

bool zero_reference_velocity(const ExecutionControllerSampleDTO& sample) {
  return std::all_of(sample.reference_velocities_rad_s.begin(),
                     sample.reference_velocities_rad_s.end(),
                     [](double velocity) { return velocity == 0.0; });
}

std::optional<std::vector<double>> reorder(const std::vector<double>& values,
                                           const std::vector<std::string>& from,
                                           const std::vector<std::string>& to) {
  std::vector<double> result;
  result.reserve(to.size());
  for (const auto& name : to) {
    const auto found = std::find(from.begin(), from.end(), name);
    if (found == from.end() || static_cast<std::size_t>(found - from.begin()) >= values.size()) {
      return std::nullopt;
    }
    result.push_back(values[static_cast<std::size_t>(found - from.begin())]);
  }
  return result;
}

double joint_distance(const std::vector<double>& left, const std::vector<double>& right) {
  double sum = 0.0;
  for (std::size_t index = 0U; index < std::min(left.size(), right.size()); ++index) {
    sum += (left[index] - right[index]) * (left[index] - right[index]);
  }
  return std::sqrt(sum);
}

/// Waits through `waiter` until `done()` holds, `deadline` passes or the stage
/// is cancelled. Returns `done()`'s final value.
template <typename Done>
bool await(const IClock& clock, const StopStageWaiter& waiter, const std::stop_token& cancellation,
           TimePoint deadline, Done&& done) {
  while (true) {
    if (done()) return true;
    if (cancellation.stop_requested() || clock.now() >= deadline) return false;
    waiter(deadline, cancellation);
  }
}

/// Bounded diagnostics (`InterruptStructuredValue` limits): oversized keys and
/// non-finite numbers are skipped, long strings truncated, extra keys dropped.
void put(InterruptStructuredValue& values, std::string key, InterruptStructuredScalar value) {
  if (key.empty() || key.size() > kMaxInterruptStructuredValueKeyBytes) return;
  if (auto* number = std::get_if<double>(&value); number != nullptr && !std::isfinite(*number)) {
    return;
  }
  if (auto* text = std::get_if<std::string>(&value);
      text != nullptr && text->size() > kMaxInterruptStructuredValueStringBytes) {
    text->resize(kMaxInterruptStructuredValueStringBytes);
  }
  if (values.size() >= kMaxInterruptStructuredValueEntries && !values.contains(key)) return;
  values.insert_or_assign(std::move(key), std::move(value));
}

}  // namespace

/// One TranslateStopStage call.
struct ParticipantStopStageHelper::Run {
  const ResolvedParticipantStopStageDTO& stage;
  const PreparedStopStageCommandDTO& command;
  std::stop_token cancellation;
  TimePoint started{};
  /// This robot's own Stop-time bound: its controlled deadline for a
  /// controlled stage, its lane's stop-completion timeout for a hard stage.
  /// Once a Stop trajectory (controlled, or the hard stage's brake) is
  /// sent, a normal motion's no-report bound for it (`FinishReplacement`,
  /// `TryControlledHardStop`).
  TimePoint terminal_deadline{};
  /// The Stop request (t0), recovered from the Kernel's elapsed time.
  TimePoint stop_requested_at{};
  StopStageDispatchedGoalDTO goal;
  ExecutionLaneFactsDTO lane;
  /// Requested (source-addressed) profile of a motion-controlled stage.
  std::optional<MotionInterruptStopProfileDTO> profile;
  std::optional<ControlledStopResultDTO> outcome;
  bool dispatched{false};
  /// Goal whose terminal result is goal-terminal evidence.
  std::optional<ExecutionGoalKey> finish_goal;
  /// Trip measured positions (stop distance diagnostic).
  std::vector<double> trip_measured_positions;
  /// Hard stop observability (`main`'s `hard_stop` keys).
  std::optional<std::string> hard_stop_level;
  std::optional<std::string> hard_stop_fallback_reason;
  /// The Stop goal's recognition delay (dispatch to anchor).
  std::optional<Duration> recognition_delay_ns;
  /// Stale controller state seen while waiting; it is the stage failure.
  std::optional<ErrorDTO> stale_state_error;
  IStopStageSpringReturnObserver* spring_observer{};
  std::optional<SpringBackReturnPointDTO> spring_return_point{};
  std::vector<std::string> refused_modes{};
  bool mode_not_carried_out{};
  double source_offset_s{0.0};
  std::optional<TimePoint> stop_ingress_at{};
  std::chrono::steady_clock::time_point leaf_deadline{};
  std::optional<double> spring_return_cursor{};
  std::vector<double> spring_return_positions{};
  bool spring_return_target_frozen{};
};

ParticipantStopStageHelper::ParticipantStopStageHelper(IExecutionPort& port, const IClock& clock,
                                                       StopStageWaiter waiter,
                                                       StopSpeedFactorFreeze freeze_speed_factor,
                                                       StopStageHelperConfig config)
    : port_(port),
      clock_(clock),
      waiter_(std::move(waiter)),
      freeze_speed_factor_(std::move(freeze_speed_factor)),
      config_(std::move(config)) {
  const auto positive = [](double value) { return std::isfinite(value) && value > 0.0; };
  if (!waiter_ || !freeze_speed_factor_ || config_.resources.empty() ||
      !positive(config_.stop_traj_delta_s) || !positive(config_.sample_period_s) ||
      !config_.replacement_start_allowance_s.has_value() ||
      !std::isfinite(*config_.replacement_start_allowance_s) ||
      *config_.replacement_start_allowance_s < 0.0 ||
      config_.complete_move_on_signal_timeout_ns < 0 ||
      config_.hard_stop_completion_timeout_ns <= 0 ||
      std::any_of(config_.max_deceleration_on_cancel_rad_s2.begin(),
                  config_.max_deceleration_on_cancel_rad_s2.end(),
                  [&](const auto& limit) { return !positive(limit.second); })) {
    throw std::invalid_argument("ParticipantStopStageHelper requires complete lane facts");
  }
}

void ParticipantStopStageHelper::BindDispatchedGoal(
    StopStageDispatchedGoalDTO goal, std::shared_ptr<ExecutionSampleAssociation> association,
    std::shared_ptr<GoalCancelAttempt> cancel) {
  if (!association) throw std::invalid_argument("A dispatched goal requires its association");
  if (!cancel) cancel = std::make_shared<GoalCancelAttempt>();
  std::lock_guard lock(mutex_);
  sent_.assign(1U, SentGoal{goal.key, goal.trajectory, std::nullopt, std::move(association),
                           false, {}, goal.planned_controller_offset_s});
  if (cancel) sent_.front().cancel = std::move(cancel);
  goal_ = std::move(goal);
}

void ParticipantStopStageHelper::ForgetKnownUnsent(const ExecutionGoalKey& key) {
  std::lock_guard lock(mutex_);
  if (!goal_ || goal_->key != key || sent_.size() != 1U || sent_.front().key != key) return;
  sent_.clear();
  goal_.reset();
}

ExecutionGoalKey ParticipantStopStageHelper::NextKey() const {
  std::lock_guard lock(mutex_);
  auto key = sent_.back().key;
  key.controller_goal_generation =
      ControllerGoalGeneration(key.controller_goal_generation.value() + 1U);
  return key;
}

void ParticipantStopStageHelper::Record(SentGoal goal) {
  if (!goal.cancel) goal.cancel = std::make_shared<GoalCancelAttempt>();
  std::lock_guard lock(mutex_);
  sent_.push_back(std::move(goal));
}

std::optional<std::uint64_t> ParticipantStopStageHelper::SourceExecutionId(
    const ExecutionGoalKey& key) const {
  std::lock_guard lock(mutex_);
  const auto it = std::find_if(sent_.rbegin(), sent_.rend(),
                               [&](const SentGoal& goal) { return goal.key == key; });
  return it != sent_.rend() && it->cancel ? it->cancel->execution_id : std::nullopt;
}

bool ParticipantStopStageHelper::ReserveReplacement(const ExecutionGoalKey& source) {
  std::lock_guard lock(mutex_);
  if (!goal_ || goal_->key != source || sent_.size() != 1U || sent_.front().key != source ||
      sent_.front().moving_replacement)
    return false;
  sent_.reserve(kExecutionGoalRecordCapacity);
  return true;
}

void ParticipantStopStageHelper::RecordReplacement(
    ExecutionGoalKey key, TrajectoryDTO trajectory,
    std::shared_ptr<ExecutionSampleAssociation> association,
    double planned_controller_offset_s, std::shared_ptr<GoalCancelAttempt> cancel) noexcept {
  std::lock_guard lock(mutex_);
  sent_.push_back(SentGoal{std::move(key), std::move(trajectory), std::nullopt,
                           std::move(association), true, {}, planned_controller_offset_s, std::move(cancel)});
}

void ParticipantStopStageHelper::RetireSettledReplacement(const ExecutionGoalKey& key) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(sent_.begin(), sent_.end(), [&](const auto& goal) {
    return goal.moving_replacement && goal.key == key;
  });
  if (found != sent_.end()) sent_.erase(found);
}

bool ParticipantStopStageHelper::HasPendingReplacement() const {
  std::lock_guard lock(mutex_);
  return std::any_of(sent_.begin(), sent_.end(),
                     [](const auto& goal) { return goal.moving_replacement; });
}

bool ParticipantStopStageHelper::CanCompleteNaturally(const ExecutionGoalKey& source) const {
  std::lock_guard lock(mutex_);
  const bool pending = std::any_of(sent_.begin(), sent_.end(),
                                   [](const auto& goal) { return goal.moving_replacement; });
  if (!pending) return true;
  const auto original = std::find_if(sent_.begin(), sent_.end(),
                                     [&](const auto& goal) { return goal.key == source; });
  if (original == sent_.end()) return false;
  const auto predecessor = port_.ReadGoal(source);
  if (!predecessor) return false;
  for (auto newer = std::next(original); newer != sent_.end(); ++newer) {
    auto observed = port_.ReadGoal(newer->key);
    if (!observed) observed = newer->terminal_observation;
    // Rejection proves the newer command was prevented. Missing/unknown or a
    // cancel result alone is not settlement proof for the original shortcut.
    if (!observed || observed->key != newer->key ||
        observed->binding_epoch != predecessor->binding_epoch ||
        observed->binding_epoch != port_.ReadReadiness().binding_epoch ||
        observed->phase != ExecutionGoalPhase::kTerminal ||
        observed->result != ExecutionGoalResult::kRejected)
      return false;
  }
  return true;
}

ExecutionPortCommandResultDTO ParticipantStopStageHelper::CancelExactGoal(
    const ExecutionGoalKey& key, std::shared_ptr<GoalCancelAttempt> attempt,
    std::chrono::steady_clock::time_point deadline) {
  bool first;
  {
    std::lock_guard lock(attempt->mutex);
    first = !attempt->started;
    if (first) attempt->started = true;
  }
  if (first) {
    ExecutionPortCommandResultDTO result;
    try {
      result = port_.Cancel(key, deadline);
    } catch (...) {
      result.dispatch = ExecutionPortDispatch::kEffectUncertain;
    }
    attempt->result.set_value(result);
  }
  if (attempt->completion.wait_until(deadline) != std::future_status::ready)
    return {ExecutionPortDispatch::kEffectUncertain, std::nullopt,
            stage_error("stop_evidence_phase_expired", "Cancel outcome was not confirmed before its deadline")};
  return attempt->completion.get();
}

bool ParticipantStopStageHelper::CancelGoals(const std::vector<SentGoal>& goals,
    std::chrono::steady_clock::time_point deadline) {
  bool sent = false;
  for (const auto& goal : goals) {
    try {
      sent |= CancelExactGoal(goal.key, goal.cancel, deadline).dispatch != ExecutionPortDispatch::kNotDispatched;
    } catch (...) {
      // A thrown send is uncertain; still request the other retained key.
      sent = true;
    }
  }
  return sent;
}

bool ParticipantStopStageHelper::CancelLiveGoals(std::chrono::steady_clock::time_point deadline) {
  return CancelGoals(LiveGoals(), deadline);
}

void ParticipantStopStageHelper::AdoptReplacement(
    std::optional<StopStageDispatchedGoalDTO>& goal) noexcept {
  static_assert(std::is_nothrow_swappable_v<StopStageDispatchedGoalDTO>);
  static_assert(std::is_nothrow_move_assignable_v<SentGoal>);
  std::lock_guard lock(mutex_);
  goal_.swap(goal);
  // The caller just proved this exact predecessor superseded by the own-
  // recognized successor. No unresolved record is removed here.
  sent_.erase(sent_.begin(), std::prev(sent_.end()));
  sent_.front().moving_replacement = false;
}

std::vector<ParticipantStopStageHelper::SentGoal> ParticipantStopStageHelper::LiveGoals() const {
  std::vector<SentGoal> sent;
  {
    std::lock_guard lock(mutex_);
    sent = sent_;
  }
  const bool pending = std::any_of(sent.begin(), sent.end(),
                                   [](const auto& goal) { return goal.moving_replacement; });
  std::vector<SentGoal> live;
  for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
    auto observed = port_.ReadGoal(it->key);
    if (pending) {
      if (observed && observed->key == it->key &&
          observed->binding_epoch == port_.ReadReadiness().binding_epoch &&
          observed->phase == ExecutionGoalPhase::kTerminal) {
        std::lock_guard lock(mutex_);
        const auto retained = std::find_if(sent_.begin(), sent_.end(),
                                           [&](const auto& goal) { return goal.key == it->key; });
        if (retained != sent_.end()) retained->terminal_observation = observed;
      } else if (!observed && it->terminal_observation) {
        observed = it->terminal_observation;
      }
    }
    if (!terminal(observed) ||
        (pending && (!observed || !observed->result ||
                     observed->binding_epoch != port_.ReadReadiness().binding_epoch ||
                     observed->result == ExecutionGoalResult::kEndedUnknown)))
      live.push_back(*it);
  }
  return live;
}

std::optional<double> ParticipantStopStageHelper::OriginalPathCursor(
    const std::optional<ExecutionControllerSampleDTO>& sample) const {
  SentGoal newest;
  {
    std::lock_guard lock(mutex_);
    if (sent_.size() < 2U) return std::nullopt;
    newest = sent_.back();
  }
  if (!newest.cursor_map.has_value()) return std::nullopt;
  const auto& map = *newest.cursor_map;
  const auto goal = port_.ReadGoal(newest.key);
  if (!goal.has_value()) return std::nullopt;
  if (goal->phase == ExecutionGoalPhase::kTerminal) {
    return goal->result == ExecutionGoalResult::kSucceeded
               ? std::optional(map.original_path_s.empty() ? map.trip_path_s
                                                           : map.original_path_s.back())
               : std::nullopt;
  }
  if (!sample.has_value() || sample->binding_epoch != goal->binding_epoch ||
      !std::isfinite(sample->cursor_s) || !newest.association->Observe(*sample, goal)) {
    return std::nullopt;
  }
  return original_path_cursor(map, sample->cursor_s - newest.planned_controller_offset_s);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::TranslateStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation) {
  return TranslateStopStageImpl(stage, budget, cancellation, nullptr);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::TranslateStopStageForTakeover(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation, IStopStageSpringReturnObserver& observer) {
  return TranslateStopStageImpl(stage, budget, cancellation, &observer);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::TranslateStopStageWithDeadline(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation, IStopStageSpringReturnObserver* observer,
    std::chrono::steady_clock::time_point deadline) {
  return TranslateStopStageImpl(stage, budget, cancellation, observer, deadline);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::TranslateStopStageImpl(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation, IStopStageSpringReturnObserver* observer,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
  const auto wall_started = std::chrono::steady_clock::now();
  const auto started = clock_.now();
  if (cancellation.stop_requested()) return superseded_result(stage);
  const auto refuse = [&](std::string reason, std::string message) {
    return failed_result(stage, StopStageAttemptDispositionDTO::kNotDispatched,
                         stage_error(std::move(reason), std::move(message)),
                         clock_.now() - started);
  };
  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size() ||
      budget.stop_id != stage.stop_id || budget.stop_generation != stage.stop_generation ||
      budget.elapsed_since_stop_ns < 0 ||
      (budget.stop_ingress_age_ns &&
       (*budget.stop_ingress_age_ns < 0 || *budget.stop_ingress_age_ns > kMaximumStopDurationNs)) ||
      (budget.remaining_controlled_deadline_ns.has_value() &&
       *budget.remaining_controlled_deadline_ns <= 0)) {
    return refuse("resolved_stop_stage_budget_invalid",
                  "Stop stage has no valid per-robot Stop timing");
  }
  const auto& command = selected_command(stage);
  if (command.resolved_stage.evidence_requirements.empty()) {
    return refuse("stop_evidence_requirements_missing", "Stop stage has no evidence contract");
  }
  // A controlled stage runs to this robot's own controlled deadline;
  // every hard stage is bounded by the lane's stop-completion timeout.
  const std::int64_t terminal_remaining =
      budget.remaining_controlled_deadline_ns.value_or(config_.hard_stop_completion_timeout_ns);

  std::optional<StopStageDispatchedGoalDTO> goal;
  {
    std::lock_guard lock(mutex_);
    goal = goal_;
  }
  if (!goal.has_value() || goal->key.controller_goal_id != stage.controller_goal_id ||
      goal->key.controller_goal_generation != stage.controller_goal_generation) {
    return refuse("resolved_stop_stage_dispatch_membership_unavailable",
                  "Stop cannot prove this stage belongs to the dispatched goal generation");
  }
  const auto& target = command.resolved_stage.command_target;
  const bool exact_participant =
      goal->operation_id == stage.operation_id &&
      goal->operation_revision == stage.operation_revision &&
      goal->participant_id == stage.participant_id &&
      goal->controllable_component_id == stage.controllable_component_id &&
      goal->command_target_id == stage.command_target_id &&
      goal->capability_id == stage.capability_id &&
      goal->control_domain_id == stage.control_domain_id &&
      goal->control_domain_revision == stage.control_domain_revision &&
      goal->control_binding_id == stage.control_binding_id &&
      goal->control_binding_revision == stage.control_binding_revision &&
      goal->controller_topology_id == stage.controller_topology_id &&
      goal->topology_revision == stage.topology_revision &&
      goal->control_route_id == stage.control_route_id &&
      goal->active_control_configuration_id == stage.active_control_configuration_id &&
      goal->active_control_configuration_generation ==
          stage.active_control_configuration_generation &&
      goal->control_provider.runtime_epoch == stage.provider_runtime_epoch &&
      goal->resources == stage.resources &&
      goal->stop_evidence_policy_id == stage.stop_evidence_policy_id &&
      goal->stop_evidence_policy_revision == stage.stop_evidence_policy_revision &&
      target.provider_instance_id == goal->control_provider.provider_instance_id &&
      target.provider_manifest_id == goal->control_provider.provider_manifest_id &&
      target.provider_manifest_revision == goal->control_provider.provider_manifest_revision &&
      target.runtime_unit_id == goal->control_provider.runtime_unit_id &&
      target.provider_runtime_epoch == goal->control_provider.runtime_epoch &&
      std::find(goal->stop_plans.begin(), goal->stop_plans.end(), stage.participant_stop_plan) !=
          goal->stop_plans.end();
  if (!exact_participant) {
    return refuse("resolved_stop_stage_dispatch_membership_mismatch",
                  "Stop stage differs from its dispatched goal");
  }

  const auto& containment = command.resolved_stage.containment_command;
  const bool controlled =
      containment.command_kind == StopContainmentCommandKind::kMotionControlledStop;
  const bool software_hard =
      containment.command_kind == StopContainmentCommandKind::kRegistered &&
      containment.registered_command_kind_id.has_value() &&
      containment.registered_command_kind_id->value() == "manyforge.stop.software_hard";
  const bool goal_cancel =
      containment.command_kind == StopContainmentCommandKind::kControllerGoalCancel;
  if (!controlled && !software_hard && !goal_cancel) {
    return refuse("resolved_stop_command_not_supported",
                  "Stop stage supports controlled arm Stop, controller-goal cancel and the "
                  "software-hard Stop");
  }
  if (cancellation.stop_requested()) return superseded_result(stage);

  Run run{stage,
          command,
          cancellation,
          started,
          started + terminal_remaining,
          started - budget.elapsed_since_stop_ns,
          std::move(*goal),
          port_.ReadLaneFacts(),
          std::nullopt,
          std::nullopt,
          false,
          std::nullopt,
          {},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
  run.spring_observer = observer;
  run.leaf_deadline = std::min(deadline.value_or(std::chrono::steady_clock::time_point::max()),
      wall_started + std::chrono::nanoseconds(std::max<Duration>(0, terminal_remaining)));
  if (budget.stop_ingress_age_ns)
    run.stop_ingress_at = started - *budget.stop_ingress_age_ns;
  if (run.lane.supports_scheduled_replacement) run.lane.experiment.release_timer = true;
  // The lane's hard stop is the controlled hard stop (level 3) with the
  // cancel (level 4) as its floor; a controller-goal cancel is level 4 itself.
  if (!controlled) return RunHardStage(run, software_hard);
  return RunControlledStage(run);
}

std::optional<ErrorDTO> ParticipantStopStageHelper::StaleControllerState() const {
  if (config_.controller_state_max_age_ns <= 0) return std::nullopt;
  // The sample before `now` (see ExecutionPortParticipant::Evaluate).
  const auto sample = port_.ReadSample();
  const auto now = clock_.now();
  if (sample.has_value() && sample->local_receipt_stamp_ns <= now &&
      now - sample->local_receipt_stamp_ns <= config_.controller_state_max_age_ns) {
    return std::nullopt;
  }
  return stage_error(
      "stale_controller_state", "The controller state is older than the lane's maximum age",
      {{"max_age_s", format(static_cast<double>(config_.controller_state_max_age_ns) / 1e9)},
       {"sample_age_s",
        sample.has_value()
            ? format(static_cast<double>(now - sample->local_receipt_stamp_ns) / 1e9)
            : std::string("none")}});
}

template <typename Done>
bool ParticipantStopStageHelper::Await(Run& run, const TimePoint deadline, Done&& done) {
  const bool ended = await(clock_, waiter_, run.cancellation, deadline, [&] {
    if (run.stale_state_error.has_value()) return true;
    if (auto stale = StaleControllerState()) {
      run.stale_state_error = std::move(stale);
      return true;
    }
    return done();
  });
  return ended && !run.stale_state_error.has_value();
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::Fail(const Run& run,
                                                               ErrorDTO error) const {
  // Stale controller state is the stage's failure whatever wait it ended.
  if (run.stale_state_error.has_value()) {
    auto stale = *run.stale_state_error;
    const auto reason = error.details.find("reason");
    if (reason != error.details.end() &&
        stale.details.size() < kMaximumStopStageErrorDetails) {
      stale.details.emplace("ended_wait_reason", reason->second);
    }
    error = std::move(stale);
  }
  if (run.hard_stop_level.has_value() && error.details.size() < kMaximumStopStageErrorDetails) {
    error.details.emplace("hard_stop_level", *run.hard_stop_level);
  }
  for (std::size_t index = 0U; index < run.refused_modes.size(); ++index) {
    if (error.details.size() >= kMaximumStopStageErrorDetails) {
      const auto removable =
          std::find_if(error.details.begin(), error.details.end(), [](const auto& row) {
            return row.first != "reason" && row.first != "recommended_disposition" &&
                   row.first != "stop_disposition" && !row.first.starts_with("fallback_attempt_");
          });
      if (removable != error.details.end()) error.details.erase(removable);
    }
    error.details.insert_or_assign("fallback_attempt_" + std::to_string(index + 1U),
                                   run.refused_modes[index]);
  }
  auto result = failed_result(run.stage,
                              run.dispatched ? StopStageAttemptDispositionDTO::kDispatched
                                             : StopStageAttemptDispositionDTO::kNotDispatched,
                              std::move(error), clock_.now() - run.started);
  if (run.dispatched && run.profile.has_value()) {
    const auto executed =
        run.outcome.has_value() ? run.outcome->executed_stop_mode : run.profile->stop_mode;
    result.actual_provider_contract_mode = provider_contract_stop_mode(executed);
    if (executed != run.profile->stop_mode && run.outcome.has_value()) {
      result.recoverable_substitution_reason = run.outcome->fallback_reason;
    }
  }
  return result;
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::Succeed(Run& run,
                                                                  const TimePoint finish_receipt,
                                                                  std::string finish_basis) {
  const auto& requirements = run.command.resolved_stage.evidence_requirements;
  const bool needs_goal_terminal =
      std::any_of(requirements.begin(), requirements.end(), [](const auto& requirement) {
        return requirement.requirement.evidence_kind ==
               StopEvidenceRequirementKind::kControllerGoalTerminal;
      });
  std::optional<ExecutionControllerSampleDTO> sample;
  std::optional<ExecutionGoalObservationDTO> finish_goal;
  std::optional<TimePoint> finish_goal_observed_at;
  const auto finish_key = run.finish_goal.value_or(run.goal.key);
  // One post-finish sample of the controller records the provenance of the
  // finish (the controller reports the finish; no rest check).
  const bool observed = Await(run, run.terminal_deadline, [&] {
    sample = port_.ReadSample();
    if (needs_goal_terminal) {
      finish_goal = port_.ReadGoal(finish_key);
      // Freshness belongs to this current read, not the permanent terminal
      // transition. Keep its original receipt unchanged for ordering/reporting.
      if (finish_goal.has_value()) {
        finish_goal_observed_at = clock_.now();
      } else {
        finish_goal_observed_at.reset();
      }
    }
    return sample.has_value() && sample->local_receipt_stamp_ns >= finish_receipt &&
           sample->local_receipt_count.nonzero() &&
           (!needs_goal_terminal ||
            (finish_goal.has_value() && finish_goal->phase == ExecutionGoalPhase::kTerminal));
  });
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (!observed) {
    return Fail(run, stage_error(needs_goal_terminal ? "stop_goal_terminal_state_mismatch"
                                                     : "stop_evidence_source_unavailable",
                                 "No post-finish observation arrived in the Stop budget"));
  }
  if (needs_goal_terminal &&
      (finish_goal->key != finish_key || !finish_goal->binding_epoch.nonzero() ||
       finish_goal->binding_epoch != sample->binding_epoch ||
       finish_goal->binding_epoch != port_.ReadReadiness().binding_epoch)) {
    return Fail(run, stage_error("stop_evidence_source_mismatch",
                                 "Goal-terminal evidence differs from its current binding"));
  }
  const auto now = clock_.now();
  if (run.outcome.has_value()) {
    run.outcome->backend_stop_confirmed = true;
    run.outcome->confirmation_latency_ns = std::max<std::int64_t>(0, now - run.started);
    if (!run.trip_measured_positions.empty() &&
        sample->measured_positions_rad.size() == run.trip_measured_positions.size()) {
      run.outcome->stop_distance_rad =
          joint_distance(sample->measured_positions_rad, run.trip_measured_positions);
    }
  }
  const auto epoch = run.command.resolved_stage.command_target.provider_runtime_epoch;
  const auto sequence = sample->local_receipt_count.value();
  auto result = Fail(run, ErrorDTO{});
  result.error.reset();
  result.evidence_items.clear();
  for (const auto& requirement : requirements) {
    JsonObjectDTO evidence;
    if (run.outcome.has_value()) {
      evidence = std::get<JsonObjectDTO>(
          motion_controlled_stop_evidence(
              *run.profile, *run.outcome, run.stage.stop_evidence_policy_revision.value(),
              run.command.parameter_source.has_value() ? to_string(*run.command.parameter_source)
                                                       : std::string_view("deployment_default"))
              .value);
    }
    if (run.hard_stop_level.has_value()) {
      evidence.insert_or_assign("hard_stop_level", JsonValueDTO{*run.hard_stop_level});
    }
    if (run.hard_stop_fallback_reason.has_value()) {
      evidence.insert_or_assign("hard_stop_fallback_reason",
                                JsonValueDTO{*run.hard_stop_fallback_reason});
    }
    if (run.recognition_delay_ns.has_value()) {
      evidence.insert_or_assign("goal_recognition_delay_ns",
                                JsonValueDTO{static_cast<double>(*run.recognition_delay_ns)});
    }
    std::int64_t age = 0;
    switch (requirement.requirement.evidence_kind) {
      case StopEvidenceRequirementKind::kMotionRest:
        if (requirement.resolved_source.source_kind !=
            StopEvidenceSourceKind::kBindingStateAuthority) {
          return Fail(run, stage_error("stop_evidence_source_mismatch",
                                       "Rest evidence names another source kind"));
        }
        age = std::max<std::int64_t>(0, now - sample->local_receipt_stamp_ns);
        evidence.insert_or_assign(
            "post_command_sample",
            JsonValueDTO{JsonObjectDTO{
                {"finish_authority", JsonValueDTO{std::string("controller_reported")}},
                {"finish_basis", JsonValueDTO{finish_basis}},
                {"sample_sequence", JsonValueDTO{std::to_string(sequence)}},
                {"sample_after_finish_ns",
                 JsonValueDTO{static_cast<double>(sample->local_receipt_stamp_ns -
                                                  finish_receipt)}}}});
        break;
      case StopEvidenceRequirementKind::kControllerGoalTerminal:
        if (requirement.resolved_source.source_kind !=
            StopEvidenceSourceKind::kControllerGoalStatus) {
          return Fail(run, stage_error("stop_evidence_source_mismatch",
                                       "Goal-terminal evidence names another source kind"));
        }
        age = std::max<std::int64_t>(0, now - *finish_goal_observed_at);
        {
          JsonObjectDTO goal_observation{
              {"finish_basis", JsonValueDTO{finish_basis}},
              {"mechanism_goal_id", JsonValueDTO{finish_goal->mechanism_goal_id.value()}}};
          if (finish_goal->terminal_local_receipt_stamp.has_value()) {
            goal_observation.emplace(
                "terminal_local_receipt_stamp_ns",
                JsonValueDTO{static_cast<double>(*finish_goal->terminal_local_receipt_stamp)});
            goal_observation.emplace(
                "terminal_transition_age_ns",
                JsonValueDTO{static_cast<double>(std::max<std::int64_t>(
                    0, now - *finish_goal->terminal_local_receipt_stamp))});
          }
          evidence.insert_or_assign("goal_observation", JsonValueDTO{std::move(goal_observation)});
        }
        result.controller_acknowledgement_sequence = sequence;
        break;
      default:
        return Fail(run, stage_error("stop_evidence_kind_unsupported",
                                     "The execution port does not provide this Stop evidence"));
    }
    if (requirement.resolved_source.runtime_target.provider_runtime_epoch != epoch) {
      return Fail(run, stage_error("stop_evidence_source_mismatch",
                                   "Stop evidence differs from its retained source"));
    }
    if (age > requirement.requirement.source_requirement.freshness_budget_ns) {
      return Fail(run, stage_error("stop_evidence_source_stale", "Stop evidence is stale"));
    }
    result.evidence_items.push_back(StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
        StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source, epoch, sequence, age,
        JsonValueDTO{std::move(evidence)}, std::nullopt});
  }
  result.outcome = ParticipantStopStageOutcomeDTO::kSucceeded;
  result.source_epoch = epoch;
  result.source_sequence = sequence;
  return result;
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunControlledStage(Run& run) {
  const auto& command = run.command;
  const auto& contract = command.resolved_stage.containment_command.provider_contract_ref;
  if (!contract.has_value() ||
      contract->contract_schema_id.value() != kMotionControlledStopContractSchemaId ||
      contract->contract_schema_revision.value() != kMotionControlledStopContractSchemaRevision ||
      !command.validated_contract_parameters.has_value() ||
      !command.parameter_source.has_value()) {
    return Fail(run, stage_error("motion_controlled_stop_contract_invalid",
                                 "Controlled Stop has no exact prepared provider contract"));
  }
  std::string profile_id = "operator_soft_on_path";
  if (*command.parameter_source == StopStageParameterSourceDTO::kParticipantProfileMapping) {
    if (!run.stage.participant_stop_plan.participant_stop_profile.has_value()) {
      return Fail(run, stage_error("motion_controlled_stop_profile_missing",
                                   "Controlled Stop has no prepared participant profile"));
    }
    profile_id = run.stage.participant_stop_plan.participant_stop_profile->profile_id;
  }
  auto decoded = map_motion_controlled_stop_contract_parameters_to_profile(
      *command.validated_contract_parameters, config_.resources, profile_id);
  if (!decoded.ok()) {
    return Fail(run, stage_error(decoded.error(), "motion_controlled_stop_contract_invalid"));
  }
  run.profile = std::move(decoded.value());
  const auto& requested = *run.profile;
  // A profile selecting the hard stop gets the cancel (level 4) directly.
  if (requested.stop_mode == MotionInterruptStopMode::kHardDeceleration) {
    return RunHardStage(run, false);
  }
  // An explicit jerk choice; only authored angular limits are frozen.
  if (!requested.jerk_limits_enabled.has_value() ||
      std::any_of(requested.per_joint_max_jerk.begin(), requested.per_joint_max_jerk.end(),
                  [](const auto& row) {
                    return !std::holds_alternative<AngularJerkDTO>(row.second);
                  })) {
    return Fail(run, stage_error("controlled_stop_jerk_profile_not_prepared",
                                 "Controlled Stop requires an explicit jerk choice and angular "
                                 "jerk limits"));
  }
  auto projected = project_motion_interrupt_stop_profile_to_compiled_resources(requested,
                                                                               config_.resources);
  if (!projected.ok()) {
    return Fail(run, stage_error(projected.error(), "controlled_stop_projection_invalid"));
  }
  auto profile = std::move(projected.value());

  if (HasPendingReplacement() && !CanCompleteNaturally(run.goal.key))
    return Fail(run,
                stage_error("pending_replacement_has_no_controlled_source",
                            "The pending replacement leaves no qualified source or goal slot"));

  // This robot's Stop deadline from its own profile, measured from the
  // Stop request; the Kernel's per-robot value is the same bound (never
  // extended).
  const auto stop_deadline =
      std::min(run.terminal_deadline,
               run.stop_requested_at +
                   controlled_stop_deadline_ns(requested, config_.complete_move_on_signal_timeout_ns));

  // Freeze outside Core state locks, bounded by the same stage deadline.
  const auto remaining = std::max<Duration>(0, stop_deadline - clock_.now());
  const auto deadline = std::min(run.leaf_deadline,
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(remaining));
  run.leaf_deadline = deadline;
  const auto frozen = freeze_speed_factor_(deadline);
  if (!frozen) return Fail(run, stage_error("stop_evidence_phase_expired",
                                            "Stop scale freeze was not confirmed before its deadline"));
  const double factor = run.lane.supports_scale ? *frozen : 1.0;

  const auto original = port_.ReadGoal(run.goal.key);
  if (terminal(original)) {
    if (original.has_value() && original->result == ExecutionGoalResult::kSucceeded &&
        requested.allow_natural_completion_on_short_path) {
      return RunNaturalCompletion(run, stop_deadline, "goal_completed_before_stop_stage");
    }
    return Fail(run, stage_error("prepared_goal_not_active",
                                 "The dispatched goal no longer owns the active execution"));
  }
  const auto joints = run.lane.joint_names.size();
  std::shared_ptr<ExecutionSampleAssociation> association;
  {
    std::lock_guard lock(mutex_);
    association = sent_.front().association;
  }
  if (run.lane.supports_scale) {
    // The factor the controller reports as applied, from the latest
    // sample associated with the dispatched goal, is exactly 0 at the Stop
    // request (the arm stands still on its path): the Stop is the plain cancel
    // (the Kernel's hard stop) and ends Stopped with no
    // error. Any non-zero applied factor (e.g. a ramp-down in progress)
    // follows the frozen-factor Stop below.
    const auto sample = port_.ReadSample();
    if (sample.has_value() && usable_sample(*sample, joints, original->binding_epoch) &&
        association->Observe(*sample, original) && sample->applied_factor.has_value() &&
        *sample->applied_factor == 0.0) {
      return Fail(run, stage_error("controlled_stop_applied_factor_zero",
                                   "The controller reports an applied speed factor of 0: the Stop "
                                   "is the controller cancel",
                                   {{"stop_disposition", "applied_factor_zero_hard_stop"}}));
    }
  }
  if (!std::isfinite(factor) || factor <= 0.0 || factor > 1.0) {
    return Fail(run, stage_error("controlled_stop_frozen_factor_unusable",
                                 "A Stop ramp cannot be expressed in path time at this factor",
                                 {{"frozen_factor", format(factor)}}));
  }

  // Trip evidence: the latest sample associated with the dispatched goal that
  // confirms the frozen factor (reporting the last value sent proves
  // every earlier value was applied).
  std::optional<ExecutionControllerSampleDTO> trip;
  const bool tripped = Await(run, run.terminal_deadline, [&] {
    const auto sample = port_.ReadSample();
    const auto goal = port_.ReadGoal(run.goal.key);
    if (!sample.has_value() || !goal.has_value() ||
        !usable_sample(*sample, joints, goal->binding_epoch) ||
        !association->Observe(*sample, goal) ||
        (run.lane.supports_scale &&
         (!sample->applied_factor.has_value() || *sample->applied_factor != frozen))) {
      return false;
    }
    trip = sample;
    return true;
  });
  if (run.cancellation.stop_requested()) return superseded_result(run.stage);
  if (!tripped) {
    return Fail(run, stage_error("controlled_stop_no_observed_state",
                                 "No controller sample of the dispatched goal confirmed the "
                                 "frozen factor before the Stop deadline"));
  }
  run.trip_measured_positions = trip->measured_positions_rad;
  if (run.lane.supports_scheduled_replacement)
    run.source_offset_s = trip->cursor_s - estimate_stop_source_cursor(
        run.goal.trajectory, *trip, run.lane, run.goal.planned_controller_offset_s);

  // Trip evidence per authority.
  SoftOnPathInitialStateDTO initial;
  initial.state.joint_names = run.lane.joint_names;
  const bool measured = config_.trip_authority == StateAuthorityClass::kMeasured;
  initial.state.positions_rad =
      measured ? trip->measured_positions_rad : trip->reference_positions_rad;
  const bool measured_velocity = measured && trip->measured_velocities_rad_s.has_value() &&
                                 finite_vector(*trip->measured_velocities_rad_s, joints);
  if (measured_velocity) initial.state.velocities_rad_s = *trip->measured_velocities_rad_s;
  initial.position_source = SoftOnPathStateSource::kMeasuredFeedback;
  initial.velocity_source = measured_velocity ? SoftOnPathStateSource::kMeasuredFeedback
                                              : SoftOnPathStateSource::kModelDerived;
  initial.trajectory_time_from_start_s = trip->cursor_s - run.source_offset_s;
  initial.runtime_velocity_scale_at_trip = factor;
  initial.sample_age_ns = std::max<std::int64_t>(0, clock_.now() - trip->local_receipt_stamp_ns);


  // At most the primary and two enabled lower controlled modes. The hard
  // stage remains the Kernel's existing final escalation, never a retry here.
  std::vector<MotionInterruptStopMode> modes{profile.stop_mode};
  modes.insert(modes.end(), profile.fallback_stop_modes.begin(), profile.fallback_stop_modes.end());
  std::size_t first = 0U;
  if (run.spring_observer) {
    const auto spring = std::find_if(modes.begin(), modes.end(), [](const auto mode) {
      return mode == MotionInterruptStopMode::kSpringBackHold ||
             mode == MotionInterruptStopMode::kSpringBackFast;
    });
    if (spring != modes.end()) first = static_cast<std::size_t>(spring - modes.begin());
  }
  const auto cannot_carry_out = [](const ErrorDTO& error) {
    const auto reason = error.details.find("reason");
    if (reason == error.details.end()) return false;
    // Existing planner feasibility refusals only. Its hard-stop recommendation
    // also covers drift, invalid source/cursor and configuration faults; those
    // and any new/unrecognised reason retain immediate hard escalation.
    constexpr std::array<std::string_view, 17U> refusals{
        "deceleration_time_not_larger_than_handoff",
        "soft_on_path_remaining_time_short",
        "soft_on_path_cannot_stop_on_path_within_budget",
        "soft_on_path_deceleration_time_infeasible",
        "soft_on_path_jerk_limit_exceeded",
        "soft_on_path_acceleration_limit_exceeded",
        "soft_on_path_interpolation_invalid",
        "soft_on_path_retimer_not_dispatchable",
        "soft_on_path_coordinate_kind_unsupported",
        "spring_back_phase_time_not_larger_than_handoff",
        "spring_back_stop_deadline_exceeded",
        "spring_back_return_rate_cap_infeasible",
        "spring_back_forward_interpolation_invalid",
        "spring_back_return_jerk_limit_exceeded",
        "spring_back_return_acceleration_limit_exceeded",
        "spring_back_return_interpolation_invalid",
        "spring_back_coordinate_kind_unsupported"};
    return std::find(refusals.begin(), refusals.end(), reason->second) != refusals.end();
  };
  const auto remember = [&](const MotionInterruptStopMode mode, const ErrorDTO& error) {
    const auto reason = error.details.find("reason");
    // Three controlled attempts, each within the existing error-string bound.
    auto recorded = std::string(stop_mode_string(mode)) + ":" +
                    (reason == error.details.end() ? error.code : reason->second);
    if (recorded.size() > 256U) recorded.resize(256U);
    run.refused_modes.push_back(std::move(recorded));
  };
  const auto make_outcome = [&](const MotionInterruptStopMode mode) {
    ControlledStopResultDTO outcome;
    outcome.accepted = true;
    outcome.executed_stop_mode = mode;
    outcome.stop_command_source = MotionInterruptStopCommandSource::kFjtPreemptPathRamp;
    if (mode != requested.stop_mode) {
      outcome.fallback_reason =
          run.refused_modes.empty()
              ? "takeover_spring_return"
              : run.refused_modes.back().substr(run.refused_modes.back().find(':') + 1U);
      put(outcome.diagnostics, "requested_stop_mode",
          interrupt_structured_string(std::string(stop_mode_string(requested.stop_mode))));
      put(outcome.diagnostics, "recoverable_substitution_reason",
          interrupt_structured_string(*outcome.fallback_reason));
      put(outcome.diagnostics, "recoverable_substitution_pair",
          interrupt_structured_string(std::string(stop_mode_string(requested.stop_mode)) + "->" +
                                      std::string(stop_mode_string(mode))));
    }
    for (std::size_t index = 0U; index < run.refused_modes.size(); ++index) {
      put(outcome.diagnostics, "fallback_attempt_" + std::to_string(index + 1U),
          interrupt_structured_string(run.refused_modes[index]));
    }
    return outcome;
  };

  for (std::size_t index = first; index < modes.size(); ++index) {
    if (run.cancellation.stop_requested()) return superseded_result(run.stage);
    if (clock_.now() >= stop_deadline) {
      return Fail(run, stage_error("controlled_stop_deadline_exceeded",
                                   "No remaining time for another controlled Stop mode"));
    }
    auto attempt = profile;
    attempt.stop_mode = modes[index];
    attempt.fallback_stop_modes.clear();
    attempt.allow_natural_completion_on_short_path =
        index == 0U && requested.allow_natural_completion_on_short_path;
    run.outcome = make_outcome(attempt.stop_mode);
    run.spring_return_point.reset();
    run.mode_not_carried_out = false;
    std::optional<ParticipantStopStageResultDTO> result;
    if (attempt.stop_mode != MotionInterruptStopMode::kSoftOnPath &&
        !run.spring_return_target_frozen) {
      // Preserve trip evidence and braking; only freeze the return target at
      // original Kernel ingress, including the stock controller's unscaled P.
      run.spring_return_positions = initial.state.positions_rad;
      if (run.stop_ingress_at && !measured) {
        const double period_s = run.lane.supports_scheduled_replacement
            ? std::trunc(1e9 / run.lane.controller_update_rate_hz) / 1e9 : 0.0;
        const double cursor = trip->cursor_s - run.source_offset_s - factor *
            static_cast<double>(trip->local_receipt_stamp_ns - *run.stop_ingress_at) / 1e9 +
            period_s;
        const auto target = sample_trajectory(run.goal.trajectory, cursor);
        if (!target.ok() || run.goal.trajectory.waypoints.empty())
          return Fail(run, stage_error("spring_back_return_target_invalid",
                                       "The Stop arrival target cannot be sampled"));
        run.spring_return_cursor = std::clamp(cursor,
            run.goal.trajectory.waypoints.front().time_from_start_s,
            run.goal.trajectory.waypoints.back().time_from_start_s);
        // Source is in trajectory order; the fast builder accepts lane order.
        for (std::size_t j = 0; j < run.goal.trajectory.joint_names.size(); ++j) {
          const auto found = std::find(run.lane.joint_names.begin(),
              run.lane.joint_names.end(), run.goal.trajectory.joint_names[j]);
          if (found == run.lane.joint_names.end())
            return Fail(run, stage_error("spring_back_fast_joint_mapping_invalid",
                                         "The return target does not cover the active joints"));
          run.spring_return_positions.at(found - run.lane.joint_names.begin()) =
              target.value().positions_rad.at(j);
        }
      }
      run.spring_return_target_frozen = true;
    }
    if (attempt.stop_mode != MotionInterruptStopMode::kSoftOnPath) {
      put(run.outcome->diagnostics, "spring_return_target_source", interrupt_structured_string(
          run.spring_return_cursor ? "kernel_stop_ingress_command_model"
                                   : measured ? "measured_trip_approximation"
                                              : "legacy_trip_target"));
      if (run.spring_return_cursor)
        put(run.outcome->diagnostics, "spring_return_path_time_s",
            interrupt_structured_number(*run.spring_return_cursor));
      if (run.stop_ingress_at)
        put(run.outcome->diagnostics, "spring_return_ingress_age_ns",
            interrupt_structured_number(static_cast<double>(run.started - *run.stop_ingress_at)));
      double delta = 0.0;
      for (std::size_t j = 0; j < run.spring_return_positions.size(); ++j)
        delta = std::max(delta, std::abs(run.spring_return_positions[j] - initial.state.positions_rad[j]));
      put(run.outcome->diagnostics, "spring_return_vs_trip_max_rad", interrupt_structured_number(delta));
    }
    if (attempt.stop_mode == MotionInterruptStopMode::kSpringBackFast) {
      result = RunFastSpringBack(run, *trip, run.spring_return_positions, factor, stop_deadline);
    } else {
      SoftOnPathPlannerConfig planner;
      planner.stop_traj_delta_s = config_.stop_traj_delta_s;
      planner.sample_period_s = config_.sample_period_s;
      planner.lead_in_anchor = config_.lead_in_anchor;
      planner.allow_model_velocity_fallback = !measured_velocity;
      planner.max_initial_path_deviation_rad = config_.max_initial_path_deviation_rad;
      planner.max_initial_velocity_deviation_rad_s = config_.max_initial_velocity_deviation_rad_s;
      planner.jerk_limits_enabled = attempt.jerk_limits_enabled == true;
      if (attempt.stop_mode == MotionInterruptStopMode::kSpringBackHold)
        planner.spring_back_return_path_time_s = run.spring_return_cursor;
      if (planner.jerk_limits_enabled) {
        for (const auto& [joint, jerk] : attempt.per_joint_max_jerk) {
          planner.per_joint_max_jerk_rad_s3.emplace(joint,
                                                    std::get<AngularJerkDTO>(jerk).value_rad_s3);
        }
      }
      attempt.deceleration_time_s = motion_interrupt_deceleration_time_or_default(attempt);
      if (run.spring_observer && attempt.stop_mode == MotionInterruptStopMode::kSpringBackHold) {
        auto forward = attempt;
        forward.stop_mode = MotionInterruptStopMode::kSoftOnPath;
        forward.deceleration_time_s = 0.5 * *attempt.deceleration_time_s;
        auto resolved =
            resolve_soft_on_path_stop_initial_state(run.goal.trajectory, forward, initial, planner);
        if (!resolved.ok()) {
          remember(attempt.stop_mode, resolved.error());
          if (!cannot_carry_out(resolved.error()) || index + 1U == modes.size())
            return Fail(run, stage_error(resolved.error(), "spring_back_initial_state_invalid"));
          continue;
        }
        run.spring_return_point = SpringBackReturnPointDTO{run.goal.key, trip->binding_epoch, *trip,
                                                           std::move(resolved.value())};
      }
      auto planned =
          attempt.stop_mode == MotionInterruptStopMode::kSoftOnPath
              ? plan_soft_on_path_stop(run.goal.trajectory, attempt, initial, planner)
              : plan_spring_back_on_path_stop(run.goal.trajectory, attempt, initial, planner);
      if (!planned.ok() && attempt.allow_natural_completion_on_short_path &&
          soft_on_path_error_recommends_natural_completion(planned.error())) {
        return RunNaturalCompletion(run, stop_deadline, "planner_remaining_path_short");
      }
      if (!planned.ok()) {
        auto cause = planned.error();
        remember(attempt.stop_mode, cause);
        if (soft_on_path_error_recommends_hard_stop(cause))
          cause.details.insert_or_assign("stop_disposition", "planner_selected_hard_stop");
        if (!cannot_carry_out(cause) || index + 1U == modes.size())
          return Fail(run, stage_error(cause, "controlled_stop_planner_failed"));
        continue;
      }
      for (const auto& [key, value] : planned.value().diagnostics)
        put(run.outcome->diagnostics, key, value);
      run.outcome->initial_path_deviation_rad = planned.value().initial_path_deviation_rad;
      // Actual C: the same attempt, limits and trip evidence, planned from the
      // command state at the same seam (rate f), so its lead-in ends at C and
      // the separately frozen return target stays unchanged.
      const auto replan = [&](const double lead_in_s) {
        auto later = planner;
        later.stop_traj_delta_s = lead_in_s;
        later.allow_model_velocity_fallback = true;
        auto command = initial;
        command.velocity_source = SoftOnPathStateSource::kModelDerived;
        command.trajectory_time_from_start_s = planned.value().command_path_time_s;
        auto law = attempt.stop_mode == MotionInterruptStopMode::kSoftOnPath
                       ? plan_soft_on_path_stop(run.goal.trajectory, attempt, command, later)
                       : plan_spring_back_on_path_stop(run.goal.trajectory, attempt, command, later);
        if (!law.ok()) run.mode_not_carried_out = cannot_carry_out(law.error());
        return law;
      };
      result = RunReplacement(run, planned.value(), *trip, factor, stop_deadline, replan);
    }
    if (result->outcome != ParticipantStopStageOutcomeDTO::kFailed || run.dispatched ||
        run.stale_state_error || run.cancellation.stop_requested() || !result->error ||
        !run.mode_not_carried_out) {
      return std::move(*result);
    }
    remember(attempt.stop_mode, *result->error);
    // Only a refused, non-effectful attempt can advance. Once any replacement
    // may have reached the controller, its execution faults go directly hard.
    if (index + 1U == modes.size()) {
      auto exhausted = *result->error;
      exhausted.details.insert_or_assign("recommended_disposition", "hard_stop");
      return Fail(run, std::move(exhausted));
    }
  }
  return Fail(run,
              stage_error("controlled_stop_modes_exhausted", "No controlled Stop mode remains"));
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunReplacement(
    Run& run, const SoftOnPathStopPlanDTO& plan, const ExecutionControllerSampleDTO& trip,
    const double frozen_factor, const TimePoint stop_deadline,
    const std::function<Result<SoftOnPathStopPlanDTO>(double)>& replan) {
  const std::string mode =
      run.outcome->executed_stop_mode == MotionInterruptStopMode::kSoftOnPath ? "soft_on_path"
                                                                              : "spring_back";
  // The original trajectory's own points up to the
  // trip point (the lead-in end), then the checked ramp in path time at the
  // frozen factor.
  auto lead_in_end =
      stop_trip_path_s(plan.command_path_time_s, frozen_factor, plan.stop_traj_delta_s);
  auto continuation = convert_stop_trajectory_to_path_time(plan.replacement_trajectory,
                                                           frozen_factor);
  if (!continuation.ok()) return Fail(run, stage_error(continuation.error(), mode + "_join_failed"));
  if (run.cancellation.stop_requested()) return superseded_result(run.stage);
  const auto next = NextKey();
  const auto baseline = port_.ReadSample();
  // The join is computed immediately before the send, so the local time
  // since the trip sample is measured at the dispatch itself; checked against
  // `main`'s acceptance-within-lead-in rule: the mechanism must start
  // the replacement before the planned trip point.
  const auto dispatched_at = clock_.now();
  auto start = compute_stop_replacement_start(
      {trip.cursor_s - run.source_offset_s, frozen_factor,
       static_cast<double>(dispatched_at - trip.local_receipt_stamp_ns) / 1e9,
       *config_.replacement_start_allowance_s, lead_in_end}, trip, run.lane);
  double first_time_s = 0.0;
  if (start.ok() && start.value().scheduled) {
    const double period_s = static_cast<Duration>(1e9 / run.lane.controller_update_rate_hz) / 1e9;
    first_time_s = period_s - start.value().controller_offset_s;
    // C carries one unscaled lookahead P, so its bound adds the same P.
    const auto eligible = compute_stop_join_elapsed_s(
        {start.value().elapsed_path_s + first_time_s, 1.0, 0.0, 0.0, lead_in_end + period_s});
    if (!eligible.ok()) start = Result<StopReplacementStartDTO>::Err(eligible.error());
  }
  if (!start.ok()) {
    const auto reason = start.error().details.find("reason");
    run.mode_not_carried_out =
        reason != start.error().details.end() && reason->second == "stop_delta_budget_exceeded";
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded", start.error().message,
                                 start.error().details));
  }
  // Actual C: a takeover cursor C at or past the trip point re-plans the law
  // from the command state source(C), with its lead-in ending at C; C, the
  // seam, the return target and every deadline stay fixed.
  std::optional<SoftOnPathStopPlanDTO> replanned;
  if (start.value().scheduled && !(start.value().elapsed_path_s + first_time_s < lead_in_end)) {
    const double lead_in_s =
        (start.value().elapsed_path_s + first_time_s - plan.command_path_time_s) / frozen_factor;
    auto law = replan(lead_in_s);
    if (!law.ok()) {
      auto cause = law.error();
      if (soft_on_path_error_recommends_hard_stop(cause))
        cause.details.insert_or_assign("stop_disposition", "planner_selected_hard_stop");
      return Fail(run, stage_error(cause, "controlled_stop_planner_failed"));
    }
    // The law runs its remaining time from the takeover tick T (local clock
    // through the trip sample's receipt); it must end by the Stop deadline.
    const auto takeover_ns =
        trip.local_receipt_stamp_ns +
        (start.value().scheduled->controller_start_stamp_ns - trip.source_stamp_ns) -
        std::llround(start.value().controller_offset_s / frozen_factor * 1e9);
    const auto end_ns = takeover_ns + std::llround(
        (law.value().replacement_trajectory.waypoints.back().time_from_start_s - lead_in_s) * 1e9);
    if (end_ns > std::min(stop_deadline, run.terminal_deadline)) {
      run.mode_not_carried_out = true;
      return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded",
                                   "The re-planned Stop law ends past the Stop deadline",
                                   {{"recommended_disposition", "hard_stop"}}));
    }
    continuation =
        convert_stop_trajectory_to_path_time(law.value().replacement_trajectory, frozen_factor);
    if (!continuation.ok())
      return Fail(run, stage_error(continuation.error(), mode + "_join_failed"));
    lead_in_end = stop_trip_path_s(plan.command_path_time_s, frozen_factor, lead_in_s);
    for (const auto& [key, value] : law.value().diagnostics)
      put(run.outcome->diagnostics, key, value);
    replanned = std::move(law.value());
  }
  const auto& law = replanned ? *replanned : plan;
  const auto elapsed = start.value().elapsed_path_s;
  auto joined = build_stop_replacement_join(run.goal.trajectory, elapsed,
                                            continuation.value(), plan.command_path_time_s,
                                            lead_in_end);
  if (joined.ok() && start.value().scheduled)
    joined = stop_replacement_v0_payload(joined.value(), first_time_s);
  if (!joined.ok()) return Fail(run, stage_error(joined.error(), mode + "_join_failed"));
  if (const auto held = HoldForRelease(run, start.value(), dispatched_at, stop_deadline, mode)) {
    return *held;
  }
  const auto cancel_slot = std::make_shared<GoalCancelAttempt>();
  cancel_slot->execution_id = SourceExecutionId(run.goal.key);
  if (!cancel_slot->execution_id || *cancel_slot->execution_id == 0U)
    return Fail(run, stage_error("stop_execution_identity_missing",
                                 "Stop replacement has no owning execution identity"));
  auto dispatch_token = std::make_shared<ExecutionDispatchToken>(cancel_slot->execution_id);
  const auto sent = start.value().scheduled
                        ? port_.ReplaceScheduled(run.goal.key, next, joined.value(),
                                                 *start.value().scheduled, dispatch_token)
                        : port_.Replace(run.goal.key, next, joined.value(), dispatch_token);
  std::optional<StopReplacementCursorMapDTO> cursor_map;
  if (sent.dispatch != ExecutionPortDispatch::kNotDispatched &&
      law.replacement_path_time_s.size() == continuation.value().waypoints.size()) {
    std::vector<double> continuation_times;
    continuation_times.reserve(continuation.value().waypoints.size());
    for (const auto& point : continuation.value().waypoints) {
      continuation_times.push_back(point.time_from_start_s);
    }
    cursor_map = StopReplacementCursorMapDTO{elapsed, lead_in_end,
                                             plan.command_path_time_s,
                                             std::move(continuation_times),
                                             law.replacement_path_time_s};
  }
  if (sent.dispatch != ExecutionPortDispatch::kNotDispatched) {
    put(run.outcome->diagnostics, "join_elapsed_path_s", interrupt_structured_number(elapsed));
    put(run.outcome->diagnostics, "frozen_factor", interrupt_structured_number(frozen_factor));
    put(run.outcome->diagnostics, "replacement_start_allowance_s",
        interrupt_structured_number(*config_.replacement_start_allowance_s));
  }
  return FinishReplacement(run, sent, next, std::move(joined.value()), std::move(cursor_map),
                           baseline, dispatched_at, trip, mode, plan.path_deviation_max_rad,
                           stop_deadline, start.value().controller_offset_s, frozen_factor, cancel_slot);
}

// ManyMove's controlled stop for searches and probes stopped
// by an interrupt. The active trajectory's points already passed (negative
// times) plus one point at the trip position with zero velocity and
// acceleration at the deceleration time; the controller interpolates the rest
// (brake, overshoot, return). Sent at once: no planner, connector, limit or
// collision check, by design.
ParticipantStopStageResultDTO ParticipantStopStageHelper::RunFastSpringBack(
    Run& run, const ExecutionControllerSampleDTO& trip,
    const std::vector<double>& trip_positions, const double frozen_factor,
    const TimePoint stop_deadline) {
  const auto& active = run.goal.trajectory;
  const auto& profile = *run.profile;
  const std::size_t width = active.joint_names.size();
  std::vector<double> stop_positions(width);
  for (std::size_t index = 0U; index < width; ++index) {
    const auto found = std::find(run.lane.joint_names.begin(), run.lane.joint_names.end(),
                                 active.joint_names[index]);
    if (found == run.lane.joint_names.end() ||
        static_cast<std::size_t>(found - run.lane.joint_names.begin()) >= trip_positions.size()) {
      return Fail(run, stage_error("spring_back_fast_joint_mapping_invalid",
                                   "The trip position does not cover the active trajectory"));
    }
    stop_positions[index] =
        trip_positions[static_cast<std::size_t>(found - run.lane.joint_names.begin())];
  }
  // The Stop takes the deceleration time in wall time at the frozen factor.
  const double stop_path_s =
      motion_interrupt_deceleration_time_or_default(profile) * frozen_factor;
  const auto dispatched_at = clock_.now();
  const double elapsed =
      trip.cursor_s - run.source_offset_s +
      frozen_factor * static_cast<double>(dispatched_at - trip.local_receipt_stamp_ns) / 1e9;
  if (!std::isfinite(stop_path_s) || stop_path_s <= 0.0 || !std::isfinite(elapsed) ||
      active.waypoints.empty()) {
    return Fail(run, stage_error("spring_back_fast_input_invalid",
                                 "The fast spring-back has no usable trip or duration"));
  }
  if (profile.stop_mode == MotionInterruptStopMode::kSpringBackFast &&
      profile.allow_natural_completion_on_short_path &&
      active.waypoints.back().time_from_start_s - elapsed <= stop_path_s) {
    // As ManyMove: a move ending within the Stop time finishes by itself.
    return RunNaturalCompletion(run, stop_deadline, "spring_back_fast_remaining_path_short");
  }
  TrajectoryDTO stop;
  StopReplacementStartDTO scheduled_start;
  if (run.lane.supports_scheduled_replacement) {
    // The same scheduled first-command placement as every capable Stop;
    // preserve the complete fast curve and the frozen return target.
    auto start = compute_stop_replacement_start(
        {trip.cursor_s - run.source_offset_s, frozen_factor,
         static_cast<double>(dispatched_at - trip.local_receipt_stamp_ns) / 1e9,
         *config_.replacement_start_allowance_s, std::nullopt},
        trip, run.lane);
    const double first_time_s = start.ok()
        ? static_cast<Duration>(1e9 / run.lane.controller_update_rate_hz) / 1e9 -
              start.value().controller_offset_s : 0.0;
    const double entry = start.ok() ? start.value().elapsed_path_s + first_time_s : 0.0;
    auto built = start.ok() ? build_scheduled_spring_back_fast(active, entry, stop_positions,
                                                               entry + stop_path_s)
                            : Result<TrajectoryDTO>::Err(start.error());
    if (!built.ok()) {
      return Fail(run, stage_error("spring_back_fast_input_invalid", built.error().message,
                                   built.error().details));
    }
    for (auto& point : built.value().waypoints)
      point.time_from_start_s += first_time_s;
    start.value().scheduled->experiment->spring_back_fast = true;
    if (const auto held = HoldForRelease(run, start.value(), dispatched_at, stop_deadline,
                                         "spring_back_fast")) {
      return *held;
    }
    stop = std::move(built.value());
    scheduled_start = std::move(start.value());
  } else {
    stop.resources = active.resources;
    stop.joint_names = active.joint_names;
    for (const auto& point : active.waypoints) {
      if (point.time_from_start_s >= elapsed) break;
      auto passed = point;
      passed.time_from_start_s -= elapsed;
      stop.waypoints.push_back(std::move(passed));
    }
  }
  const auto& scheduled = scheduled_start.scheduled;
  const auto passed_points = scheduled ? 0U : stop.waypoints.size();
  if (stop.waypoints.empty()) {
    // Nothing passed yet: the message starts now, at the trip position, so the
    // controller takes it at once (only a first point in the future is a
    // scheduled splice on the active timeline).
    stop.waypoints.push_back(TrajectoryWaypointDTO{0.0, stop_positions,
                                                   std::vector<double>(width, 0.0),
                                                   std::vector<double>(width, 0.0)});
  }
  if (!scheduled) {
    stop.waypoints.push_back(TrajectoryWaypointDTO{stop_path_s, stop_positions,
                                                   std::vector<double>(width, 0.0),
                                                   std::vector<double>(width, 0.0)});
  }
  if (run.cancellation.stop_requested()) return superseded_result(run.stage);
  run.outcome->stop_command_source = MotionInterruptStopCommandSource::kFjtPreemptHold;
  if (run.spring_observer) {
    SoftOnPathResolvedInitialStateDTO return_state;
    return_state.command_path_time_s = trip.cursor_s - run.source_offset_s;
    return_state.spring_back_return_path_time_s = run.spring_return_cursor;
    return_state.path_time_rate = frozen_factor;
    return_state.runtime_velocity_scale_at_trip = frozen_factor;
    return_state.joint_names = active.joint_names;
    return_state.spring_back_return_point = TrajectoryWaypointDTO{
        0.0, stop_positions, std::vector<double>(width, 0.0), std::vector<double>(width, 0.0)};
    run.spring_return_point =
        SpringBackReturnPointDTO{run.goal.key, trip.binding_epoch, trip, std::move(return_state)};
  }
  put(run.outcome->diagnostics, "stop_mode", interrupt_structured_string("spring_back_fast"));
  put(run.outcome->diagnostics, "join_elapsed_path_s", interrupt_structured_number(elapsed));
  put(run.outcome->diagnostics, "frozen_factor", interrupt_structured_number(frozen_factor));
  put(run.outcome->diagnostics, "passed_points",
      interrupt_structured_number(static_cast<double>(passed_points)));
  if (scheduled) {
    put(run.outcome->diagnostics, "spring_back_fast_shape",
        interrupt_structured_string("scheduled_v0"));
    put(run.outcome->diagnostics, "scheduled_start_path_s",
        interrupt_structured_number(scheduled_start.elapsed_path_s));
  }
  const auto next = NextKey();
  const auto baseline = port_.ReadSample();
  const auto cancel_slot = std::make_shared<GoalCancelAttempt>();
  cancel_slot->execution_id = SourceExecutionId(run.goal.key);
  if (!cancel_slot->execution_id || *cancel_slot->execution_id == 0U)
    return Fail(run, stage_error("stop_execution_identity_missing",
                                 "Stop replacement has no owning execution identity"));
  auto dispatch_token = std::make_shared<ExecutionDispatchToken>(cancel_slot->execution_id);
  const auto sent = scheduled ? port_.ReplaceScheduled(run.goal.key, next, stop, *scheduled, dispatch_token)
                              : port_.Replace(run.goal.key, next, stop, dispatch_token);
  return FinishReplacement(run, sent, next, std::move(stop), std::nullopt, baseline,
                           dispatched_at, trip, "spring_back_fast", 0.0, stop_deadline,
                           scheduled_start.controller_offset_s, frozen_factor, cancel_slot);
}

// EXPERIMENTAL release timer: hold before the send through the stage's own
// cancellable wait (no lock held, no port call). The ceiling is the existing
// Stop deadline, terminal deadline and lead-in acceptance window
// (`FinishReplacement`), all from the same `dispatched_at`. A release beyond it
// is not held; cancellation or stale state during the hold ends the stage as
// in any other wait; a wake past the ceiling (oversleep) sends nothing and is
// refused as the existing lead-in budget refusal, before any effect.
std::optional<ParticipantStopStageResultDTO> ParticipantStopStageHelper::HoldForRelease(
    Run& run, StopReplacementStartDTO& start, const TimePoint dispatched_at,
    const TimePoint stop_deadline, const std::string& mode) {
  if (!start.scheduled) return std::nullopt;
  const auto period_ns = static_cast<Duration>(1e9 / run.lane.controller_update_rate_hz);
  const auto ceiling = std::min({stop_deadline, run.terminal_deadline, dispatched_at +
      static_cast<Duration>(*config_.replacement_start_allowance_s * 1e9) + period_ns});
  const auto until = plan_replacement_release(start, clock_, ceiling);
  if (!until) {
    run.mode_not_carried_out = true;
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded",
                                 "The scheduled Stop release is no longer available"));
  }
  (void)Await(run, *until, [] { return false; });
  if (run.cancellation.stop_requested()) return superseded_result(run.stage);
  if (run.stale_state_error) {
    return Fail(run, stage_error("release_hold_ended", "Stale controller state during the hold"));
  }
  if (const auto woke = clock_.now(); woke > std::min(ceiling, *until + period_ns / 2)) {
    run.mode_not_carried_out = true;
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded",
                                 "The release hold ended past the Stop's existing ceiling",
                                 {{"recommended_disposition", "hard_stop"},
                                  {"release_overslept_ns", std::to_string(woke - *until)}}));
  }
  return std::nullopt;
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::FinishReplacement(
    Run& run, const ExecutionPortCommandResultDTO& sent, const ExecutionGoalKey& next,
    TrajectoryDTO sent_trajectory, std::optional<StopReplacementCursorMapDTO> cursor_map,
    const std::optional<ExecutionControllerSampleDTO>& baseline, const TimePoint dispatched_at,
    const ExecutionControllerSampleDTO& trip, const std::string& mode,
    const double initial_path_deviation_rad, const TimePoint stop_deadline, double controller_offset_s,
    const double frozen_factor, std::shared_ptr<GoalCancelAttempt> cancel_slot) {
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    const auto original = port_.ReadGoal(run.goal.key);
    if (original.has_value() && original->result == ExecutionGoalResult::kSucceeded &&
        run.profile->allow_natural_completion_on_short_path) {
      // The goal reached its endpoint before the replacement (as `main`).
      run.outcome.reset();
      return RunNaturalCompletion(run, stop_deadline,
                                  "natural_completion_before_replacement_dispatch");
    }
    run.outcome.reset();
    run.mode_not_carried_out = original.has_value() && !terminal(original) &&
                               sent.refusal != ExecutionPortRefusal::kStaleKey;
    return Fail(run, stage_error(mode + "_path_ramp_dispatch_failed",
                                 sent.error.has_value() ? sent.error->message
                                                        : "The Stop replacement was refused"));
  }
  run.dispatched = true;
  run.finish_goal = next;
  // Once sent, the Stop trajectory finishes like a normal
  // motion, on the controller's report (success, or its goal-tolerance
  // verdict after `goal_time`). Its no-report bound is a normal motion's for
  // the trajectory as the controller runs it (path time at the frozen
  // factor), never past the Kernel's watchdog of this stage.
  const double wall_duration_s =
      sent_trajectory.waypoints.empty()
          ? 0.0
          : std::max(0.0, sent_trajectory.waypoints.back().time_from_start_s) / frozen_factor;
  const auto no_report_deadline =
      std::min(dispatched_at + stop_trajectory_no_report_ns(wall_duration_s),
               run.stop_requested_at +
                   controlled_stage_watchdog_ns(stop_deadline - run.stop_requested_at));
  const auto association = std::make_shared<ExecutionSampleAssociation>(
      port_.ReadReadiness().binding_epoch, baseline, dispatched_at);
  Record(SentGoal{next, std::move(sent_trajectory), std::move(cursor_map), association,
                  false, {}, controller_offset_s, std::move(cancel_slot)});
  if (sent.dispatch == ExecutionPortDispatch::kEffectUncertain) {
    return Fail(run, stage_error(mode + "_path_ramp_dispatch_uncertain",
                                 "The Stop replacement may already affect the controller"));
  }

  // `main`'s replacement-within-lead-in check, with a proof: the
  // replacement took over only once its own feedback or its own result is
  // observed within the lane lead-in (`stop_traj_delta_s`); acceptance alone
  // is never proof. `replacement_start_allowance_s` stays only the join
  // timing estimate. The proof counts by when it was
  // received (the anchor's local receipt), not by when the poll wakes.
  bool rejected = false;
  const auto lead_in_ns = static_cast<Duration>(config_.stop_traj_delta_s * 1e9);
  const auto accept_deadline = std::min(run.terminal_deadline, dispatched_at + lead_in_ns);
  const bool anchored = Await(run, accept_deadline, [&] {
    const auto goal = port_.ReadGoal(next);
    if (goal.has_value() && goal->result == ExecutionGoalResult::kRejected) {
      rejected = true;
      return true;
    }
    return association->ObserveGoal(goal);
  });
  const auto recognition = association->recognition_delay_ns();
  // Eligibility uses the actual capped window (the lead-in, or less
  // when the stage deadline comes first).
  const bool accepted =
      anchored && recognition.has_value() && *recognition <= accept_deadline - dispatched_at;
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (rejected) {
    return Fail(run, stage_error(mode + "_path_ramp_dispatch_failed",
                                 "The controller rejected the Stop replacement"));
  }
  if (!accepted) {
    for (const auto& goal : LiveGoals()) {
      if (goal.key == next) {
        (void)CancelExactGoal(next, goal.cancel, run.leaf_deadline);
        break;
      }
    }
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded",
                                 "The Stop replacement showed no own feedback or result within "
                                 "the lead-in",
                                 {{"recommended_disposition", "hard_stop"}}));
  }

  if (sent.dispatch == ExecutionPortDispatch::kDispatched && run.spring_observer &&
      run.spring_return_point) {
    // This is the chosen mode's verified return target, not proof of arrival.
    static_cast<void>(run.spring_observer->SpringReturnPointReady(*run.spring_return_point));
  }

  // Finish: the replacement's result, like a normal motion's; the path
  // deviation is measured from the controller samples meanwhile (a
  // diagnostic). The post-finish observation shares the same bound.
  run.terminal_deadline = no_report_deadline;
  double deviation = initial_path_deviation_rad;
  std::size_t deviation_samples = 0U;
  std::optional<ErrorDTO> deviation_error;
  TimePoint last_receipt = trip.local_receipt_stamp_ns;
  std::optional<ExecutionGoalObservationDTO> finished;
  const bool done = Await(run, no_report_deadline, [&] {
    const auto sample = port_.ReadSample();
    if (sample.has_value() && sample->local_receipt_stamp_ns > last_receipt &&
        finite_vector(sample->measured_positions_rad, run.lane.joint_names.size())) {
      last_receipt = sample->local_receipt_stamp_ns;
      const auto measured = measure_soft_on_path_path_deviation(
          run.goal.trajectory,
          RobotStateDTO{run.lane.joint_names, sample->measured_positions_rad, {}});
      if (measured.ok()) {
        deviation = std::max(deviation, measured.value());
        ++deviation_samples;
      } else {
        deviation_error = measured.error();
      }
    }
    const auto goal = port_.ReadGoal(next);
    (void)association->ObserveGoal(goal);
    if (goal.has_value() && goal->phase == ExecutionGoalPhase::kTerminal) {
      finished = goal;
      return true;
    }
    return false;
  });
  run.recognition_delay_ns = association->recognition_delay_ns();
  run.outcome->path_deviation_max_rad = deviation;
  put(run.outcome->diagnostics, "path_deviation_max_rad", interrupt_structured_number(deviation));
  put(run.outcome->diagnostics, "path_deviation_samples",
      interrupt_structured_number(static_cast<double>(deviation_samples)));
  put(run.outcome->diagnostics, "path_deviation_source",
      interrupt_structured_string("controller_state_measured_positions"));
  if (deviation_error.has_value()) {
    put(run.outcome->diagnostics, "path_deviation_error_reason",
        interrupt_structured_string(deviation_error->details.contains("reason")
                                        ? deviation_error->details.at("reason")
                                        : deviation_error->code));
  }
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (!done) {
    return Fail(run, stage_error("backend_stop_not_confirmed",
                                 "The controller did not report the Stop replacement's result "
                                 "within a normal motion's bound",
                                 {{"mode", mode}}));
  }
  if (finished->result == ExecutionGoalResult::kSucceeded) {
    return Succeed(run, finished->terminal_local_receipt_stamp.value_or(clock_.now()),
                   mode + "_replacement_succeeded");
  }
  if (finished->result == ExecutionGoalResult::kAbortedGoalTolerance &&
      run.lane.goal_tolerance_policy == ExecutionGoalTolerancePolicy::kKept) {
    // `main`'s confirmed-but-not-settled case; the Kernel ends the
    // participant InterventionRequired without a second Stop command.
    return Fail(run, stage_error("stop_goal_tolerance_violated",
                                 "The Stop ramp ended outside the controller's goal tolerance",
                                 {{"mode", mode}}));
  }
  return Fail(run, stage_error("controller_terminal_result_unsuccessful",
                               "The Stop replacement terminated unsuccessfully",
                               {{"mode", mode},
                                {"goal_result", to_string(finished->result.value())}}));
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunNaturalCompletion(
    Run& run, const TimePoint stop_deadline, std::string basis) {
  if (!CanCompleteNaturally(run.goal.key))
    return Fail(run, stage_error("pending_replacement_not_settled",
                                 "A newer possibly sent motion still requires containment"));
  ControlledStopResultDTO outcome;
  outcome.accepted = true;
  outcome.completed_by_natural_completion = true;
  outcome.executed_stop_mode = run.profile->stop_mode;
  outcome.stop_command_source = MotionInterruptStopCommandSource::kDiagnosticOther;
  put(outcome.diagnostics, "command_source", interrupt_structured_string("natural_completion"));
  put(outcome.diagnostics, "natural_completion_basis", interrupt_structured_string(basis));
  put(outcome.diagnostics, "requested_stop_mode",
      interrupt_structured_string(stop_mode_string(run.profile->stop_mode)));
  run.outcome = std::move(outcome);
  run.dispatched = true;
  run.finish_goal = run.goal.key;
  std::optional<ExecutionGoalObservationDTO> goal;
  const bool ended = Await(run, stop_deadline, [&] {
    goal = port_.ReadGoal(run.goal.key);
    return terminal(goal) && CanCompleteNaturally(run.goal.key);
  });
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (!ended || !goal.has_value() || goal->result != ExecutionGoalResult::kSucceeded) {
    return Fail(run, stage_error("soft_on_path_natural_completion_timeout",
                                 "The original motion did not complete within the Stop deadline"));
  }
  return Succeed(run, goal->terminal_local_receipt_stamp.value_or(clock_.now()), basis);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunEndedGoalHold(Run& run) {
  // Idempotent containment of a goal that already ended (as both backends):
  // no command; the controller holds, which a later sample at zero reference
  // velocity shows.
  run.dispatched = true;
  run.finish_goal = run.goal.key;
  run.hard_stop_level = "goal_already_terminal";
  TimePoint ended_at = run.started;
  std::optional<ExecutionGoalKey> newest;
  std::shared_ptr<ExecutionSampleAssociation> association;
  std::vector<SentGoal> sent;
  {
    std::lock_guard lock(mutex_);
    sent = sent_;
  }
  for (const auto& goal_record : sent) {
    const auto goal = port_.ReadGoal(goal_record.key);
    if (goal.has_value() && goal->terminal_local_receipt_stamp.has_value()) {
      ended_at = std::max(ended_at, *goal->terminal_local_receipt_stamp);
      newest = goal_record.key;
      association = goal_record.association;
      (void)association->ObserveGoal(goal);
    }
  }
  if (newest.has_value()) run.finish_goal = newest;
  if (association == nullptr && !sent.empty()) association = sent.front().association;
  TimePoint finish_receipt{};
  const bool held = Await(run, run.terminal_deadline, [&] {
    const auto sample = port_.ReadSample();
    // The hold is a sample of the ended goal's epoch whose
    // controller source stamp is newer than the newest evidence associated
    // with that goal (or, with none, than the newest sample before it was
    // sent); a local receipt after the result alone never orders it.
    if (!sample.has_value() || sample->local_receipt_stamp_ns < ended_at ||
        association == nullptr || !association->NewerThanEvidence(*sample) ||
        !zero_reference_velocity(*sample)) {
      return false;
    }
    finish_receipt = sample->local_receipt_stamp_ns;
    return true;
  });
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (!held) {
    return Fail(run, stage_error("stop_evidence_phase_expired",
                                 "The ended goal showed no hold in the Stop budget"));
  }
  return Succeed(run, finish_receipt, "goal_already_terminal");
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunHardStage(
    Run& run, const bool controlled_hard_stop_allowed) {
  // The hard stop is bounded by the lane's stop
  // completion timeout from the stage start; the Kernel arms no watchdog of
  // its own for it. A brake that is sent finishes like a normal motion
  // instead (`TryControlledHardStop`).
  run.terminal_deadline =
      std::min(run.terminal_deadline, run.started + config_.hard_stop_completion_timeout_ns);
  // On any hard-stop trigger the speed ramp stops.
  const auto hard_remaining = std::max<Duration>(0, run.terminal_deadline - clock_.now());
  const auto deadline = std::min(run.leaf_deadline,
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(hard_remaining));
  run.leaf_deadline = deadline;
  if (!freeze_speed_factor_(deadline))
    return Fail(run, stage_error("stop_evidence_phase_expired",
                                 "Stop scale freeze was not confirmed before its deadline"));
  if (run.profile.has_value()) {
    ControlledStopResultDTO outcome;
    outcome.accepted = true;
    outcome.executed_stop_mode = MotionInterruptStopMode::kHardDeceleration;
    outcome.stop_command_source = MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
    run.outcome = std::move(outcome);
  }
  const auto live = LiveGoals();
  if (live.empty()) return RunEndedGoalHold(run);

  // Freeze at the controller's actual applied factor, pinned once.
  const auto sample = port_.ReadSample();
  std::optional<double> applied;
  if (sample.has_value() && sample->applied_factor.has_value()) applied = sample->applied_factor;
  if (run.lane.supports_scale && applied.has_value())
    (void)port_.SetScale(*applied, port_.ReserveScaleSequence());
  const double factor = run.lane.supports_scale ? applied.value_or(0.0) : 1.0;

  std::string fallback_reason;
  if (!controlled_hard_stop_allowed) {
    fallback_reason.clear();
  } else if (config_.max_deceleration_on_cancel_rad_s2.empty()) {
    fallback_reason = "controlled_hard_stop_limits_unavailable";
  } else if (!(factor > 0.0)) {
    fallback_reason = "controlled_hard_stop_factor_zero";
  } else if (!sample.has_value()) {
    fallback_reason = "controlled_hard_stop_no_sample";
  } else {
    auto attempt = TryControlledHardStop(run, live.front(), *sample, factor, fallback_reason);
    if (attempt.has_value()) return std::move(*attempt);
  }
  if (!fallback_reason.empty()) run.hard_stop_fallback_reason = fallback_reason;
  return RunCancel(run);
}

std::optional<ParticipantStopStageResultDTO> ParticipantStopStageHelper::TryControlledHardStop(
    Run& run, const SentGoal& live, const ExecutionControllerSampleDTO& sample,
    const double factor, std::string& fallback_reason) {
  if (HasPendingReplacement() && LiveGoals().size() >= kExecutionGoalRecordCapacity) {
    fallback_reason = "pending_replacement_goal_capacity";
    return std::nullopt;
  }
  const auto goal = port_.ReadGoal(live.key);
  if (!goal.has_value() || !usable_sample(sample, run.lane.joint_names.size(), goal->binding_epoch) ||
      !live.association->Observe(sample, goal)) {
    fallback_reason = "controlled_hard_stop_sample_not_associated";
    return std::nullopt;
  }
  if (!(run.lane.controller_update_rate_hz > 0.0)) {
    fallback_reason = "controlled_hard_stop_controller_period_unknown";
    return std::nullopt;
  }
  const auto& names = live.trajectory.joint_names;
  auto positions = reorder(sample.reference_positions_rad, run.lane.joint_names, names);
  auto velocities = reorder(sample.reference_velocities_rad_s, run.lane.joint_names, names);
  if (!positions.has_value() || !velocities.has_value()) {
    fallback_reason = "controlled_hard_stop_joint_mismatch";
    return std::nullopt;
  }
  const auto join_started_at = run.lane.supports_scheduled_replacement ? clock_.now() : 0;
  std::optional<StopReplacementStartDTO> start;
  double first_time_s = 0.0;
  if (run.lane.supports_scheduled_replacement) {
    auto planned = compute_stop_replacement_start(
        {estimate_stop_source_cursor(live.trajectory, sample, run.lane,
                                     live.planned_controller_offset_s), factor,
         static_cast<double>(join_started_at - sample.local_receipt_stamp_ns) / 1e9,
         *config_.replacement_start_allowance_s, std::nullopt}, sample, run.lane);
    if (!planned.ok()) {
      fallback_reason = "controlled_hard_stop_elapsed_invalid";
      return std::nullopt;
    }
    start = std::move(planned.value());
    first_time_s = static_cast<Duration>(1e9 / run.lane.controller_update_rate_hz) / 1e9 -
                   start->controller_offset_s;
    const auto source = sample_trajectory(live.trajectory, start->elapsed_path_s + first_time_s);
    if (!source.ok()) {
      fallback_reason = "controlled_hard_stop_brake_invalid";
      return std::nullopt;
    }
    positions = source.value().positions_rad;
    velocities = source.value().velocities_rad_s;
  }
  // The brake's unchanged law uses the first commanded source P/V, including the
  // existing sampler endpoint clamp. Its initial A is the brake's own law.
  const auto brake = plan_controlled_hard_stop_brake(
      {names, *positions, *velocities, factor, config_.max_deceleration_on_cancel_rad_s2,
       1.0 / run.lane.controller_update_rate_hz});
  if (!brake.ok()) {
    fallback_reason = "controlled_hard_stop_brake_invalid";
    return std::nullopt;
  }
  const auto next = NextKey();
  const auto pre_dispatch = port_.ReadSample();
  // The join is computed immediately before the send (local time since
  // the sample measured at the dispatch itself).
  const auto dispatched_at = start ? join_started_at : clock_.now();
  const auto elapsed = start ? Result<double>::Ok(start->elapsed_path_s)
                            : compute_stop_join_elapsed_s(
      {sample.cursor_s, factor,
       static_cast<double>(dispatched_at - sample.local_receipt_stamp_ns) / 1e9,
       *config_.replacement_start_allowance_s, std::nullopt});
  if (!elapsed.ok()) {
    fallback_reason = "controlled_hard_stop_elapsed_invalid";
    return std::nullopt;
  }
  auto joined = start ? Result<TrajectoryDTO>::Ok(brake.value().trajectory)
                      : build_stop_replacement_join(live.trajectory, elapsed.value(),
                          brake.value().trajectory, sample.cursor_s, sample.cursor_s);
  if (start) {
    for (auto& point : joined.value().waypoints)
      point.time_from_start_s += first_time_s;
    run.mode_not_carried_out = false;
    if (const auto held = HoldForRelease(run, *start, dispatched_at, run.terminal_deadline,
                                         "controlled_hard_stop")) {
      if (!run.mode_not_carried_out) return held;
      fallback_reason = "controlled_hard_stop_release_expired";
      return std::nullopt;
    }
  }
  if (!joined.ok()) {
    fallback_reason = "controlled_hard_stop_join_failed";
    return std::nullopt;
  }
  const auto cancel_slot = std::make_shared<GoalCancelAttempt>();
  cancel_slot->execution_id = SourceExecutionId(live.key);
  if (!cancel_slot->execution_id || *cancel_slot->execution_id == 0U)
    return Fail(run, stage_error("stop_execution_identity_missing",
                                 "Stop replacement has no owning execution identity"));
  auto dispatch_token = std::make_shared<ExecutionDispatchToken>(cancel_slot->execution_id);
  const auto sent = start ? port_.ReplaceScheduled(live.key, next, joined.value(),
                                                   *start->scheduled, dispatch_token)
                          : port_.Replace(live.key, next, joined.value(), dispatch_token);
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    fallback_reason = "controlled_hard_stop_refused";
    return std::nullopt;
  }
  run.dispatched = true;
  run.finish_goal = next;
  const auto association =
      std::make_shared<ExecutionSampleAssociation>(goal->binding_epoch, pre_dispatch, dispatched_at);
  Record(SentGoal{next, joined.value(), std::nullopt, association, false, {},
                  start ? start->controller_offset_s : 0.0, cancel_slot});
  // The brake's final point in lane joint order.
  const auto final_point =
      reorder(joined.value().waypoints.back().positions_rad, names, run.lane.joint_names);
  if (!final_point.has_value()) {
    fallback_reason = "controlled_hard_stop_joint_mismatch";
    return std::nullopt;
  }

  // The brake has taken over only when its own
  // first anchor (its feedback, or its own terminal result when that
  // arrives first) is observed within the lane lead-in (`stop_traj_delta_s`);
  // otherwise level 4 (the cancel) follows. Acceptance alone is never proof
  // and a cursor restart never counts. The proof counts by when it
  // was received (the anchor's local receipt within dispatch + lead-in), not
  // by when the poll wakes.
  bool rejected = false;
  const auto lead_in_ns = static_cast<Duration>(config_.stop_traj_delta_s * 1e9);
  const auto install_deadline = std::min(run.terminal_deadline, dispatched_at + lead_in_ns);
  const bool anchored = Await(run, install_deadline, [&] {
    const auto brake_goal = port_.ReadGoal(next);
    if (brake_goal.has_value() && brake_goal->result == ExecutionGoalResult::kRejected) {
      rejected = true;
      return true;
    }
    return association->ObserveGoal(brake_goal);
  });
  const auto recognition = association->recognition_delay_ns();
  // Eligibility uses the actual capped window (the lead-in, or less
  // when the stage deadline comes first).
  const bool started =
      anchored && recognition.has_value() && *recognition <= install_deadline - dispatched_at;
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (rejected || !started) {
    fallback_reason =
        rejected ? "controlled_hard_stop_rejected" : "controlled_hard_stop_not_installed";
    return std::nullopt;
  }
  run.hard_stop_level = "controlled_hard_stop";
  run.recognition_delay_ns = association->recognition_delay_ns();
  // The installed brake is a trajectory with the
  // arrival check, so it finishes like a normal motion: on the controller's
  // report, with a normal motion's no-report bound for the brake in wall time
  // at the frozen factor (its path time / factor), not the lane's hard-stop
  // timeout. That timeout bounds only the cancel/hold floor below.
  run.terminal_deadline =
      dispatched_at +
      stop_trajectory_no_report_ns(
          joined.value().waypoints.empty()
              ? 0.0
              : std::max(0.0, joined.value().waypoints.back().time_from_start_s) / factor);
  // Level-3 finish, as JTC realizes it: this brake's
  // own kSucceeded plus a sample newer (controller source stamp) than the
  // newest evidence associated with the brake (or, with none, than the newest
  // sample seen before the Replace was sent) whose reference velocity is
  // exactly zero at the brake's final point (JTC's success hold). No cursor
  // condition; the result's local receipt is never given a source stamp.
  constexpr double kBrakeFinalPointToleranceRad = 1e-9;
  TimePoint finish_receipt{};
  std::optional<ExecutionGoalResult> ended_unsuccessfully;
  const bool finished = Await(run, run.terminal_deadline, [&] {
    const auto brake_goal = port_.ReadGoal(next);
    // The brake's newly read own feedback is evidence before any
    // NewerThanEvidence test in this poll.
    (void)association->ObserveGoal(brake_goal);
    if (brake_goal.has_value() && brake_goal->phase == ExecutionGoalPhase::kTerminal &&
        brake_goal->result != ExecutionGoalResult::kSucceeded) {
      // A brake that ends any other way is not finished; level 4 follows
      // at once instead of waiting for the hard-stop bound.
      ended_unsuccessfully = brake_goal->result.value_or(ExecutionGoalResult::kEndedUnknown);
      return true;
    }
    const auto latest = port_.ReadSample();
    if (!latest.has_value() ||
        !usable_sample(*latest, run.lane.joint_names.size(), goal->binding_epoch)) {
      return false;
    }
    if (brake_goal.has_value() && brake_goal->phase == ExecutionGoalPhase::kTerminal &&
        association->NewerThanEvidence(*latest) && zero_reference_velocity(*latest) &&
        std::equal(latest->reference_positions_rad.begin(), latest->reference_positions_rad.end(),
                   final_point->begin(), final_point->end(), [&](double actual, double target) {
                     return std::abs(actual - target) <= kBrakeFinalPointToleranceRad;
                   })) {
      finish_receipt = latest->local_receipt_stamp_ns;
      return true;
    }
    // Not the finish: the sample may still be the brake's own evidence.
    (void)association->Observe(*latest, brake_goal);
    return false;
  });
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (ended_unsuccessfully.has_value()) {
    fallback_reason = std::string("controlled_hard_stop_ended_") + to_string(*ended_unsuccessfully);
    // The floor (cancel or hold) keeps the lane's hard-stop timeout, from now.
    run.terminal_deadline = clock_.now() + config_.hard_stop_completion_timeout_ns;
    return std::nullopt;
  }
  if (!finished) {
    return Fail(run, stage_error("stop_evidence_phase_expired",
                                 "The controlled hard stop got no finishing report within a "
                                 "normal motion's bound"));
  }
  return Succeed(run, finish_receipt, "controlled_hard_stop_succeeded_then_hold");
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunCancel(Run& run) {
  run.hard_stop_level = "jtc_cancel_decelerate_on_cancel";
  const auto live = LiveGoals();
  // Nothing left to cancel (e.g. a brake that ended unsuccessfully): the
  // controller holds; the floor observes that hold.
  if (live.empty()) return RunEndedGoalHold(run);
  // The newest sample before the cancel orders every counted sample.
  auto baseline = port_.ReadSample();
  // The switch baseline is only a sample that the association
  // of a cancelled goal attributes to that goal. A sample before any such
  // goal's first anchor (e.g. the controller's pre-goal hold) is no
  // baseline: no switch can then be established, and without the factor-0
  // basis the stage fails closed at its deadline.
  std::optional<ExecutionControllerSampleDTO> switch_baseline;
  if (baseline.has_value()) {
    for (const auto& goal : live) {
      if (goal.association != nullptr &&
          goal.association->Observe(*baseline, port_.ReadGoal(goal.key))) {
        switch_baseline = baseline;
        break;
      }
    }
  }
  // Always cancel, also at factor 0; every goal that may still run.
  const bool pending_replacement = HasPendingReplacement();
  bool any_sent = CancelGoals(live, run.leaf_deadline);
  if (live.empty() || !any_sent) {
    return Fail(run, stage_error("hard_stop_cancel_not_dispatched",
                                 "The controller cancel was not dispatched"));
  }
  run.dispatched = true;
  auto newest = live.front().key;
  std::array<bool, kExecutionGoalRecordCapacity> restoration_attempted{};
  run.finish_goal = newest;
  // Cancel finish and sample ordering. Every counted sample is received after
  // this cancel was sent and is newer (controller source stamp, same epoch)
  // than the sample read before it. (1) Cancel braking: the positively
  // acknowledged cancel, the observed switch to the cancel trajectory (cursor
  // restart) and later its reference at zero velocity, counted only when the
  // cancelled goal's own result is CANCELED; or (2) applied factor 0 plus the
  // positive acknowledgement of this cancel. A SUCCEEDED result is the
  // natural completion: the already-contained path observes the hold. No
  // measured-rest check.
  std::optional<TimePoint> switched_source;
  TimePoint finish_receipt{};
  std::string basis;
  bool completed_naturally = false;
  const bool finished = Await(run, run.terminal_deadline, [&] {
    auto goal = port_.ReadGoal(newest);
    // A pending successor can reject after its cancel was latched. The old
    // key may then be current again; its first cancel could have been stale.
    // Retry each retained predecessor once, inside the original deadline.
    if (pending_replacement && newest == live.front().key && goal &&
        goal->phase == ExecutionGoalPhase::kTerminal &&
        goal->result == ExecutionGoalResult::kRejected) {
      const auto readiness = port_.ReadReadiness();
      for (std::size_t index = 1U; index < live.size() && index < restoration_attempted.size();
           ++index) {
        if (restoration_attempted[index]) continue;
        const auto predecessor = port_.ReadGoal(live[index].key);
        if (!predecessor || predecessor->phase == ExecutionGoalPhase::kTerminal ||
            predecessor->binding_epoch != readiness.binding_epoch || !readiness.controller_active ||
            !readiness.controller_bound || !readiness.hardware_active)
          continue;
        restoration_attempted[index] = true;
        auto fresh_baseline = port_.ReadSample();
        std::optional<ExecutionControllerSampleDTO> fresh_switch_baseline;
        if (fresh_baseline && live[index].association &&
            live[index].association->Observe(*fresh_baseline, predecessor))
          fresh_switch_baseline = fresh_baseline;
        // The key's one cancel slot is one-shot. If its first attempt sent
        // nothing (stale while superseded), this retry gets a fresh slot;
        // otherwise it joins the existing one.
        auto slot = live[index].cancel;
        if (slot && slot->completion.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            slot->completion.get().dispatch == ExecutionPortDispatch::kNotDispatched) {
          auto fresh = std::make_shared<GoalCancelAttempt>();
          fresh->execution_id = slot->execution_id;
          {
            std::lock_guard lock(mutex_);
            for (auto& sent : sent_)
              if (sent.key == live[index].key && sent.cancel == slot) sent.cancel = fresh;
          }
          slot = std::move(fresh);
        }
        bool possibly_sent = true;
        try {
          possibly_sent =
              CancelExactGoal(live[index].key, slot, run.leaf_deadline).dispatch != ExecutionPortDispatch::kNotDispatched;
        } catch (...) {
          // Keep uncertainty and let only this key's actual evidence finish.
        }
        if (!possibly_sent) continue;
        newest = live[index].key;
        run.finish_goal = newest;
        baseline = std::move(fresh_baseline);
        switch_baseline = std::move(fresh_switch_baseline);
        switched_source.reset();
        goal = port_.ReadGoal(newest);
        break;
      }
    }
    if (goal.has_value() && goal->phase == ExecutionGoalPhase::kTerminal &&
        goal->result == ExecutionGoalResult::kSucceeded &&
        (!pending_replacement || LiveGoals().empty())) {
      completed_naturally = true;
      return true;
    }
    const auto sample = port_.ReadSample();
    if (!goal.has_value() || !goal->cancel_sent_local_stamp.has_value() || !sample.has_value() ||
        sample->local_receipt_stamp_ns <= *goal->cancel_sent_local_stamp ||
        !usable_sample(*sample, run.lane.joint_names.size(), goal->binding_epoch) ||
        (baseline.has_value() && baseline->binding_epoch == sample->binding_epoch &&
         sample->source_stamp_ns <= baseline->source_stamp_ns)) {
      return false;
    }
    const bool acknowledged = goal->cancel_ack_local_receipt_stamp.has_value();
    if (!switched_source.has_value() && switch_baseline.has_value() &&
        switch_baseline->binding_epoch == sample->binding_epoch &&
        sample->cursor_s < switch_baseline->cursor_s) {
      switched_source = sample->source_stamp_ns;
    }
    if (acknowledged && switched_source.has_value() &&
        sample->source_stamp_ns >= *switched_source && zero_reference_velocity(*sample) &&
        goal->result == ExecutionGoalResult::kCanceled) {
      basis = "cancel_reference_at_rest";
    } else if (acknowledged && sample->applied_factor.has_value() &&
               *sample->applied_factor == 0.0) {
      basis = "factor_zero_cancel_acknowledged";
    } else {
      return false;
    }
    if (pending_replacement) {
      const auto unresolved = LiveGoals();
      if (std::any_of(unresolved.begin(), unresolved.end(),
                      [&](const auto& retained) { return retained.key != newest; }))
        return false;
    }
    finish_receipt = sample->local_receipt_stamp_ns;
    return true;
  });
  if (run.cancellation.stop_requested()) {
    return Fail(run, stage_error("stop_stage_cancelled_after_dispatch",
                                 "Stop stage was superseded after dispatch"));
  }
  if (completed_naturally) return RunEndedGoalHold(run);
  if (!finished) {
    return Fail(run, stage_error("stop_evidence_phase_expired",
                                 "The controller cancel did not finish braking in the Stop budget"));
  }
  return Succeed(run, finish_receipt, basis);
}

}  // namespace manyforge::core
