#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <manyforge_core/execution_sample_association.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/participant_stop_stage_helper.hpp>
#include <manyforge_core/stop_timing.hpp>
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
  /// This robot's own Stop-time bound (Q51, never extended): its controlled
  /// deadline for a controlled stage, its lane's stop-completion timeout for a
  /// hard stage (Q57).
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
  /// Hard stop observability (§5, `main`'s `hard_stop` keys).
  std::optional<std::string> hard_stop_level;
  std::optional<std::string> hard_stop_fallback_reason;
  /// Q50 "measure": the Stop goal's recognition delay (dispatch to anchor).
  std::optional<Duration> recognition_delay_ns;
  /// Q59: stale controller state seen while waiting; it is the stage failure.
  std::optional<ErrorDTO> stale_state_error;
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
    StopStageDispatchedGoalDTO goal, std::shared_ptr<ExecutionSampleAssociation> association) {
  if (!association) throw std::invalid_argument("A dispatched goal requires its association");
  std::lock_guard lock(mutex_);
  sent_.assign(1U, SentGoal{goal.key, goal.trajectory, std::nullopt, std::move(association)});
  goal_ = std::move(goal);
}

ExecutionGoalKey ParticipantStopStageHelper::NextKey() const {
  std::lock_guard lock(mutex_);
  auto key = sent_.back().key;
  key.controller_goal_generation =
      ControllerGoalGeneration(key.controller_goal_generation.value() + 1U);
  return key;
}

void ParticipantStopStageHelper::Record(SentGoal goal) {
  std::lock_guard lock(mutex_);
  sent_.push_back(std::move(goal));
}

std::vector<ParticipantStopStageHelper::SentGoal> ParticipantStopStageHelper::LiveGoals() const {
  std::vector<SentGoal> sent;
  {
    std::lock_guard lock(mutex_);
    sent = sent_;
  }
  std::vector<SentGoal> live;
  for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
    if (!terminal(port_.ReadGoal(it->key))) live.push_back(*it);
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
  return original_path_cursor(map, sample->cursor_s);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::TranslateStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation) {
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
      (budget.remaining_controlled_deadline_ns.has_value() &&
       *budget.remaining_controlled_deadline_ns <= 0)) {
    return refuse("resolved_stop_stage_budget_invalid",
                  "Stop stage has no valid per-robot Stop timing");
  }
  const auto& command = selected_command(stage);
  if (command.resolved_stage.evidence_requirements.empty()) {
    return refuse("stop_evidence_requirements_missing", "Stop stage has no evidence contract");
  }
  // Q51/Q57: a controlled stage runs to this robot's own controlled deadline;
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
  // Q42: the lane's hard stop is the controlled hard stop (level 3) with the
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
  // Q59: stale controller state is the stage's failure whatever wait it ended.
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
  // finish (items 1/26: the controller reports the finish; no rest check).
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
  // Q42: a profile selecting the hard stop gets the cancel (level 4) directly.
  if (requested.stop_mode == MotionInterruptStopMode::kHardDeceleration) {
    return RunHardStage(run, false);
  }
  // Q36: an explicit jerk choice; only authored angular limits are frozen.
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
  const auto& profile = projected.value();

  // §4.2 step 1 (Q17/Q38): speed publication stops; the factor is frozen.
  const double frozen = freeze_speed_factor_();
  const double factor = run.lane.supports_scale ? frozen : 1.0;
  // Q28/Q51: this robot's Stop deadline from its own profile, measured from the
  // Stop request; the Kernel's per-robot value is the same bound (never
  // extended).
  const auto stop_deadline =
      std::min(run.terminal_deadline,
               run.stop_requested_at +
                   controlled_stop_deadline_ns(requested, config_.complete_move_on_signal_timeout_ns));

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
    // Q45: the factor the controller reports as applied, from the latest
    // sample associated with the dispatched goal, is exactly 0 at the Stop
    // request (the arm stands still on its path): the Stop is the plain cancel
    // (the Kernel's hard stop, finished per Q35) and ends Stopped with no
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
  // confirms the frozen factor (§3.2: reporting the last value sent proves
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

  // §4.2 step 1: trip evidence per authority.
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
  initial.trajectory_time_from_start_s = trip->cursor_s;
  initial.runtime_velocity_scale_at_trip = factor;
  initial.sample_age_ns = std::max<std::int64_t>(0, clock_.now() - trip->local_receipt_stamp_ns);

  SoftOnPathPlannerConfig planner;
  planner.stop_traj_delta_s = config_.stop_traj_delta_s;
  planner.sample_period_s = config_.sample_period_s;
  planner.allow_model_velocity_fallback = !measured_velocity;
  planner.max_initial_path_deviation_rad = config_.max_initial_path_deviation_rad;
  planner.max_initial_velocity_deviation_rad_s = config_.max_initial_velocity_deviation_rad_s;
  planner.jerk_limits_enabled = profile.jerk_limits_enabled == true;
  if (planner.jerk_limits_enabled) {
    for (const auto& [joint, jerk] : profile.per_joint_max_jerk) {
      planner.per_joint_max_jerk_rad_s3.emplace(joint, std::get<AngularJerkDTO>(jerk).value_rad_s3);
    }
  }
  auto spring_profile = profile;
  spring_profile.deceleration_time_s = motion_interrupt_deceleration_time_or_default(profile);

  auto outcome = ControlledStopResultDTO{};
  outcome.accepted = true;
  outcome.executed_stop_mode = profile.stop_mode;
  outcome.stop_command_source = MotionInterruptStopCommandSource::kFjtPreemptPathRamp;
  Result<SoftOnPathStopPlanDTO> planned =
      profile.stop_mode == MotionInterruptStopMode::kSoftOnPath
          ? plan_soft_on_path_stop(run.goal.trajectory, profile, initial, planner)
          : plan_spring_back_on_path_stop(run.goal.trajectory, spring_profile, initial, planner);
  if (!planned.ok() && profile.allow_natural_completion_on_short_path &&
      soft_on_path_error_recommends_natural_completion(planned.error())) {
    // Item 2: `main`'s predicate, decided once by the planner.
    return RunNaturalCompletion(run, stop_deadline, "planner_remaining_path_short");
  }
  if (!planned.ok() && profile.stop_mode == MotionInterruptStopMode::kSoftOnPath) {
    const auto cause = planned.error();
    if (soft_on_path_error_recommends_hard_stop(cause)) {
      // Q33 (as the native backend): the Stop planner chose the hard stop as
      // the right Stop; the Kernel's hard stop then ends Stopped, no error.
      auto selected = cause;
      selected.details.insert_or_assign("stop_disposition", "planner_selected_hard_stop");
      return Fail(run, stage_error(selected, "soft_on_path_planner_recommends_hard_stop"));
    }
    const auto fallback = cause.details.find("recommended_fallback");
    if (profile.recoverable_substitution_mode == MotionInterruptStopMode::kSpringBackHold &&
        fallback != cause.details.end() && fallback->second == "spring_back_hold") {
      // Level 2 (item 3): the profile's allowed substitution, selected before
      // any dispatch; it runs through the same Replace timeline.
      const auto reason = cause.details.contains("reason") ? cause.details.at("reason")
                                                           : "soft_on_path_planner_rejected";
      spring_profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
      spring_profile.allow_natural_completion_on_short_path = false;
      spring_profile.recoverable_substitution_mode.reset();
      planned = plan_spring_back_on_path_stop(run.goal.trajectory, spring_profile, initial,
                                              planner);
      outcome.executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
      outcome.fallback_reason = reason;
      put(outcome.diagnostics, "requested_stop_mode", interrupt_structured_string("soft_on_path"));
      put(outcome.diagnostics, "recoverable_substitution_reason",
          interrupt_structured_string(reason));
      put(outcome.diagnostics, "recoverable_substitution_pair",
          interrupt_structured_string("soft_on_path->spring_back_hold"));
      put(outcome.diagnostics, "soft_on_path_error_code", interrupt_structured_string(cause.code));
      put(outcome.diagnostics, "soft_on_path_error_message",
          interrupt_structured_string(cause.message));
      put(outcome.diagnostics, "soft_on_path_error_reason", interrupt_structured_string(reason));
      for (const auto& [key, value] : cause.details) {
        put(outcome.diagnostics, "soft_on_path_error_detail_" + key,
            interrupt_structured_string(value));
      }
    }
  }
  if (!planned.ok()) {
    return Fail(run, stage_error(planned.error(),
                                 outcome.executed_stop_mode == MotionInterruptStopMode::kSoftOnPath
                                     ? "soft_on_path_planner_failed"
                                     : "spring_back_planner_failed"));
  }
  for (const auto& [key, value] : planned.value().diagnostics) put(outcome.diagnostics, key, value);
  outcome.initial_path_deviation_rad = planned.value().initial_path_deviation_rad;
  run.outcome = std::move(outcome);
  return RunReplacement(run, planned.value(), *trip, factor, stop_deadline);
}

ParticipantStopStageResultDTO ParticipantStopStageHelper::RunReplacement(
    Run& run, const SoftOnPathStopPlanDTO& plan, const ExecutionControllerSampleDTO& trip,
    const double frozen_factor, const TimePoint stop_deadline) {
  const std::string mode =
      run.outcome->executed_stop_mode == MotionInterruptStopMode::kSoftOnPath ? "soft_on_path"
                                                                              : "spring_back";
  // §4.2 steps 3-4 (Q17, Q20): the original trajectory's own points up to the
  // trip point (the lead-in end), then the checked ramp in path time at the
  // frozen factor.
  const auto lead_in_end =
      stop_trip_path_s(plan.command_path_time_s, frozen_factor, plan.stop_traj_delta_s);
  auto continuation = convert_stop_trajectory_to_path_time(plan.replacement_trajectory,
                                                           frozen_factor);
  if (!continuation.ok()) return Fail(run, stage_error(continuation.error(), mode + "_join_failed"));
  if (run.cancellation.stop_requested()) return superseded_result(run.stage);
  const auto next = NextKey();
  const auto baseline = port_.ReadSample();
  // Q26: the join is computed immediately before the send, so the local time
  // since the trip sample is measured at the dispatch itself; checked against
  // `main`'s acceptance-within-lead-in rule (Q22): the mechanism must start
  // the replacement before the planned trip point.
  const auto dispatched_at = clock_.now();
  const auto elapsed = compute_stop_join_elapsed_s(
      {trip.cursor_s, frozen_factor,
       static_cast<double>(dispatched_at - trip.local_receipt_stamp_ns) / 1e9,
       *config_.replacement_start_allowance_s, lead_in_end});
  if (!elapsed.ok()) {
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded", elapsed.error().message,
                                 elapsed.error().details));
  }
  auto joined = build_stop_replacement_join(run.goal.trajectory, elapsed.value(),
                                            continuation.value(), plan.command_path_time_s,
                                            lead_in_end);
  if (!joined.ok()) return Fail(run, stage_error(joined.error(), mode + "_join_failed"));
  const auto sent = port_.Replace(run.goal.key, next, joined.value());
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
    return Fail(run, stage_error(mode + "_path_ramp_dispatch_failed",
                                 sent.error.has_value() ? sent.error->message
                                                        : "The Stop replacement was refused"));
  }
  run.dispatched = true;
  run.finish_goal = next;
  std::vector<double> continuation_times;
  continuation_times.reserve(continuation.value().waypoints.size());
  for (const auto& point : continuation.value().waypoints) {
    continuation_times.push_back(point.time_from_start_s);
  }
  std::optional<StopReplacementCursorMapDTO> cursor_map;
  if (plan.replacement_path_time_s.size() == continuation_times.size()) {
    cursor_map = StopReplacementCursorMapDTO{elapsed.value(), lead_in_end,
                                             plan.command_path_time_s,
                                             std::move(continuation_times),
                                             plan.replacement_path_time_s};
  }
  const auto association = std::make_shared<ExecutionSampleAssociation>(
      port_.ReadReadiness().binding_epoch, baseline, dispatched_at);
  Record(SentGoal{next, joined.value(), std::move(cursor_map), association});
  put(run.outcome->diagnostics, "join_elapsed_path_s", interrupt_structured_number(elapsed.value()));
  put(run.outcome->diagnostics, "frozen_factor", interrupt_structured_number(frozen_factor));
  put(run.outcome->diagnostics, "replacement_start_allowance_s",
      interrupt_structured_number(*config_.replacement_start_allowance_s));

  // `main`'s replacement-within-lead-in check (Q22), with Q59's proof: the
  // replacement took over only once its own feedback or its own result is
  // observed within the lane lead-in (`stop_traj_delta_s`); acceptance alone
  // is never proof. `replacement_start_allowance_s` stays only the join
  // timing estimate (Q26/Q55). SX-263: the proof counts by when it was
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
  // SX-268: eligibility uses the actual capped window (the lead-in, or less
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
    (void)port_.Cancel(next);
    return Fail(run, stage_error(mode + "_stop_delta_budget_exceeded",
                                 "The Stop replacement showed no own feedback or result within "
                                 "the lead-in",
                                 {{"recommended_disposition", "hard_stop"}}));
  }

  // Finish: the replacement's result; the path deviation is measured from the
  // controller samples meanwhile (Q22 diagnostic).
  double deviation = plan.path_deviation_max_rad;
  std::size_t deviation_samples = 0U;
  std::optional<ErrorDTO> deviation_error;
  TimePoint last_receipt = trip.local_receipt_stamp_ns;
  std::optional<ExecutionGoalObservationDTO> finished;
  const bool done = Await(run, stop_deadline, [&] {
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
                                 "The controller did not finish the Stop replacement within the "
                                 "Stop deadline",
                                 {{"mode", mode}}));
  }
  if (finished->result == ExecutionGoalResult::kSucceeded) {
    return Succeed(run, finished->terminal_local_receipt_stamp.value_or(clock_.now()),
                   mode + "_replacement_succeeded");
  }
  if (finished->result == ExecutionGoalResult::kAbortedGoalTolerance &&
      run.lane.goal_tolerance_policy == ExecutionGoalTolerancePolicy::kKept) {
    // Q18: `main`'s confirmed-but-not-settled case; the Kernel ends the
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
    return terminal(goal);
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
    // Q50 (SX-264): the hold is a sample of the ended goal's epoch whose
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
  // Q28/Q57 (as `main`): the hard stop is bounded by the lane's stop
  // completion timeout from the stage start; the Kernel arms no watchdog of
  // its own for it.
  run.terminal_deadline =
      std::min(run.terminal_deadline, run.started + config_.hard_stop_completion_timeout_ns);
  // Q35: on any hard-stop trigger the speed ramp stops.
  (void)freeze_speed_factor_();
  if (run.profile.has_value()) {
    ControlledStopResultDTO outcome;
    outcome.accepted = true;
    outcome.executed_stop_mode = MotionInterruptStopMode::kHardDeceleration;
    outcome.stop_command_source = MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
    run.outcome = std::move(outcome);
  }
  const auto live = LiveGoals();
  if (live.empty()) return RunEndedGoalHold(run);

  // Q35: freeze at the controller's actual applied factor, pinned once.
  const auto sample = port_.ReadSample();
  std::optional<double> applied;
  if (sample.has_value() && sample->applied_factor.has_value()) applied = sample->applied_factor;
  if (run.lane.supports_scale && applied.has_value()) (void)port_.SetScale(*applied);
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
  const auto positions = reorder(sample.reference_positions_rad, run.lane.joint_names, names);
  const auto velocities = reorder(sample.reference_velocities_rad_s, run.lane.joint_names, names);
  if (!positions.has_value() || !velocities.has_value()) {
    fallback_reason = "controlled_hard_stop_joint_mismatch";
    return std::nullopt;
  }
  // Q42: the closed-form brake from the latest commanded state, the factor
  // applied once, delivered through the Q20 join.
  const auto brake = plan_controlled_hard_stop_brake(
      {names, *positions, *velocities, factor, config_.max_deceleration_on_cancel_rad_s2,
       1.0 / run.lane.controller_update_rate_hz});
  if (!brake.ok()) {
    fallback_reason = "controlled_hard_stop_brake_invalid";
    return std::nullopt;
  }
  const auto next = NextKey();
  const auto pre_dispatch = port_.ReadSample();
  // Q26: the join is computed immediately before the send (local time since
  // the sample measured at the dispatch itself).
  const auto dispatched_at = clock_.now();
  const auto elapsed = compute_stop_join_elapsed_s(
      {sample.cursor_s, factor,
       static_cast<double>(dispatched_at - sample.local_receipt_stamp_ns) / 1e9,
       *config_.replacement_start_allowance_s, std::nullopt});
  if (!elapsed.ok()) {
    fallback_reason = "controlled_hard_stop_elapsed_invalid";
    return std::nullopt;
  }
  // The brake leaves the path at the sample itself (no lead-in).
  const auto joined = build_stop_replacement_join(live.trajectory, elapsed.value(),
                                                  brake.value().trajectory, sample.cursor_s,
                                                  sample.cursor_s);
  if (!joined.ok()) {
    fallback_reason = "controlled_hard_stop_join_failed";
    return std::nullopt;
  }
  const auto sent = port_.Replace(live.key, next, joined.value());
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    fallback_reason = "controlled_hard_stop_refused";
    return std::nullopt;
  }
  run.dispatched = true;
  run.finish_goal = next;
  const auto association =
      std::make_shared<ExecutionSampleAssociation>(goal->binding_epoch, pre_dispatch, dispatched_at);
  Record(SentGoal{next, joined.value(), std::nullopt, association});
  // The brake's final point in lane joint order (Q50 level-3 finish).
  const auto final_point =
      reorder(joined.value().waypoints.back().positions_rad, names, run.lane.joint_names);
  if (!final_point.has_value()) {
    fallback_reason = "controlled_hard_stop_joint_mismatch";
    return std::nullopt;
  }

  // Q59: the brake is sent at once and has taken over only when its own
  // first anchor (Q50: its feedback, or its own terminal result when that
  // arrives first) is observed within the lane lead-in (`stop_traj_delta_s`);
  // otherwise level 4 (the cancel) follows. Acceptance alone is never proof
  // and a cursor restart never counts. SX-263: the proof counts by when it
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
  // SX-268: eligibility uses the actual capped window (the lead-in, or less
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
  // Q50 level-3 finish, as JTC realizes it (Track 2 SX-162/164): this brake's
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
    // Q50: the brake's newly read own feedback is evidence before any
    // NewerThanEvidence test in this poll (Track 2 SX-202).
    (void)association->ObserveGoal(brake_goal);
    if (brake_goal.has_value() && brake_goal->phase == ExecutionGoalPhase::kTerminal &&
        brake_goal->result != ExecutionGoalResult::kSucceeded) {
      // Q42: a brake that ends any other way is not finished; level 4 follows
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
    return std::nullopt;
  }
  if (!finished) {
    return Fail(run, stage_error("stop_evidence_phase_expired",
                                 "The controlled hard stop did not finish braking in the Stop "
                                 "budget"));
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
  const auto baseline = port_.ReadSample();
  // SX-218 (Q50): the switch baseline is only a sample that the association
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
  // Q35: always cancel, also at factor 0; every goal that may still run.
  bool any_sent = false;
  for (const auto& goal : live) {
    any_sent |= port_.Cancel(goal.key).dispatch != ExecutionPortDispatch::kNotDispatched;
  }
  if (live.empty() || !any_sent) {
    return Fail(run, stage_error("hard_stop_cancel_not_dispatched",
                                 "The controller cancel was not dispatched"));
  }
  run.dispatched = true;
  const auto newest = live.front().key;
  run.finish_goal = newest;
  // Q5/Q35 finish with Q50 ordering. Every counted sample is received after
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
    const auto goal = port_.ReadGoal(newest);
    if (goal.has_value() && goal->phase == ExecutionGoalPhase::kTerminal &&
        goal->result == ExecutionGoalResult::kSucceeded) {
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
