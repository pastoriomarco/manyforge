#pragma once

#include <functional>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/execution_sample_association.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <memory>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <map>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace manyforge::core {

/// One exact goal's cancel result shared by its participant and Stop helper.
struct GoalCancelAttempt final {
  std::optional<std::uint64_t> execution_id;
  std::mutex mutex;
  bool started{false};
  std::promise<ExecutionPortCommandResultDTO> result;
  std::shared_future<ExecutionPortCommandResultDTO> completion{result.get_future().share()};
};

// ---------------------------------------------------------------------------
// Core Stop-stage helper.
//
// Realizes one robot participant's Stop stages over `IExecutionPort` with the
// Kernel's existing stage contract (`IControlDomainExecutionPort`). It is the
// single owner of the Stop policy previously duplicated in the native and ROS
// backends: stage identity and membership, the on-path soft Stop and its
// spring-back substitution (levels 1-2), natural completion, the controlled
// hard stop (level 3) and the controller cancel (level 4), and
// the outcome mapping. Stop selection, escalation and the
// deadline watchdog stay with the Kernel; the helper never extends a deadline.
// ---------------------------------------------------------------------------

/// Stop-visible projection of the goal the participant dispatched through the
/// port (both backends' prepared-goal record): the admitted prepared
/// participant's identity, its ordinary, fault and interrupt-scope Stop plans,
/// and the trajectory exactly as submitted. Built once at dispatch.
struct StopStageDispatchedGoalDTO final {
  ExecutionGoalKey key;
  /// The trajectory exactly as submitted (path time, complete derivatives).
  TrajectoryDTO trajectory;
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  PreparedProviderRuntimeOccurrenceDTO control_provider;
  std::vector<ResourceId> resources;
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  /// Ordinary, fault and interrupt-scope plans admitted with the goal.
  std::vector<PreparedParticipantStopPlanDTO> stop_plans;
  double planned_controller_offset_s{0.0};
};

/// Lane and deployment facts the helper needs. None has a helper default: each
/// comes from the lane facts, characterization or deployment.
struct StopStageHelperConfig final {
  /// The participant binding's resources (source and compiled joint names).
  std::vector<RuntimeResourceBindingDTO> resources;
  /// Trip-evidence authority: kMeasured on real robots (measured
  /// position and velocity), kCommanded on Isaac and offline (the controller's
  /// reference, velocity from the cursor at the frozen factor).
  StateAuthorityClass trip_authority{StateAuthorityClass::kCommanded};
  /// Where a soft Stop's lead-in starts when the trip evidence carries
  /// measured velocity (kMeasured trip authority): the controller's command
  /// cursor (default, every lane) or the measured position's projection. Per
  /// robot data: the Real profile row's `stop_lead_in_anchor`.
  StopLeadInAnchor lead_in_anchor{StopLeadInAnchor::kCommanded};
  /// Stop planner lane facts (`main`'s backend configuration).
  double stop_traj_delta_s{0.0};
  double sample_period_s{0.0};
  std::optional<double> max_initial_path_deviation_rad;
  std::optional<double> max_initial_velocity_deviation_rad_s;
  /// Lane-measured wall time from a replacement's dispatch until the
  /// mechanism starts it (sample transport included). Required lane input:
  /// there is no default; the helper refuses construction without it.
  std::optional<double> replacement_start_allowance_s{};
  /// The lane's JTC `max_deceleration_on_cancel` per compiled joint. Empty
  /// disables level 3 (the hard stop is then the cancel, level 4).
  std::map<std::string, double> max_deceleration_on_cancel_rad_s2;
  /// Completion tolerance (`main`'s `complete_move_on_signal_timeout_ns`).
  Duration complete_move_on_signal_timeout_ns{500'000'000};
  /// As `main`'s `stop_completion_timeout_s`: the lane's bound on one
  /// hard-stop stage (levels 3 and 4), measured from the stage start. Required.
  Duration hard_stop_completion_timeout_ns{0};
  /// As `main`'s `joint_state_max_age_s`: while a Stop stage waits on the
  /// controller, a newest controller sample older than this (local receipt)
  /// is stale controller state and fails the stage at once. 0: the lane
  /// declares no maximum age.
  Duration controller_state_max_age_ns{0};
};

/// Blocks until `deadline`, until `cancellation` is requested, or earlier when
/// the port may have new facts (production: the next controller sample or goal
/// event). Tests drive time and port facts through it.
using StopStageWaiter = std::function<void(TimePoint deadline, const std::stop_token& cancellation)>;

/// Stops the speed-factor publication and returns the frozen factor:
/// the last value sent on this binding epoch (or the lane's initial factor when
/// none was sent). Idempotent.
using StopSpeedFactorFreeze = std::function<std::optional<double>(std::chrono::steady_clock::time_point)>;

class ParticipantStopStageHelper final : public IControlDomainExecutionPort {
 public:
  ParticipantStopStageHelper(IExecutionPort& port, const IClock& clock, StopStageWaiter waiter,
                             StopSpeedFactorFreeze freeze_speed_factor,
                             StopStageHelperConfig config);

  /// Records the goal the participant submitted through the port with its
  /// sample association (shared with the participant). Replaces any
  /// earlier goal and its Stop replacements.
  void BindDispatchedGoal(StopStageDispatchedGoalDTO goal,
                          std::shared_ptr<ExecutionSampleAssociation> association,
                          std::shared_ptr<GoalCancelAttempt> cancel = {});
  /// Remove only an exact goal whose Submit returned known-not-dispatched.
  void ForgetKnownUnsent(const ExecutionGoalKey& key);

  /// Pending moving replacement, recorded in the sole sent-goal list. All
  /// storage is reserved before the transport call; Record cannot allocate.
  [[nodiscard]] bool ReserveReplacement(const ExecutionGoalKey& source);
  void RecordReplacement(ExecutionGoalKey key, TrajectoryDTO trajectory,
                         std::shared_ptr<ExecutionSampleAssociation> association,
                         double planned_controller_offset_s,
                         std::shared_ptr<GoalCancelAttempt> cancel) noexcept;
  [[nodiscard]] bool HasPendingReplacement() const;
  /// The owning participant has exact rejected/no-effect or scoped Stop proof.
  void RetireSettledReplacement(const ExecutionGoalKey& key) noexcept;
  [[nodiscard]] bool CanCompleteNaturally(const ExecutionGoalKey& source) const;
  /// Requests cancellation newest-first for every possibly live retained key.
  [[nodiscard]] bool CancelLiveGoals(std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max());
  [[nodiscard]] ExecutionPortCommandResultDTO CancelExactGoal(
      const ExecutionGoalKey& key, std::shared_ptr<GoalCancelAttempt> attempt,
      std::chrono::steady_clock::time_point deadline);
  /// Exact supersession/recognition was checked under the participant mutex.
  /// Swap prebuilt Stop authority and compact only that proven predecessor.
  void AdoptReplacement(std::optional<StopStageDispatchedGoalDTO>& goal) noexcept;

  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation) override;

  /// Whether the dispatched goal or one of its Stop replacements may still run
  /// (its goal record is not terminal).
  [[nodiscard]] bool HasLiveGoal() const { return !LiveGoals().empty(); }

  /// The dispatched trajectory's own cursor during and after its newest
  /// on-path Stop replacement (soft Stop, spring-back), mapped with the Stop
  /// planner's sample mapping: from `sample` once it is associated with the
  /// replacement, its end once the replacement succeeded (the hold freezes
  /// it). nullopt when no on-path replacement was sent, when the newest
  /// replacement left the path (controlled hard stop) or ended otherwise, or
  /// when `sample` is not (yet) the replacement's.
  [[nodiscard]] std::optional<double> OriginalPathCursor(
      const std::optional<ExecutionControllerSampleDTO>& sample) const;

  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStageForTakeover(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver& observer) override;
  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStageWithDeadline(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver* observer,
      std::chrono::steady_clock::time_point deadline);

 private:
  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStageImpl(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver* observer,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

  struct SentGoal {
    ExecutionGoalKey key;
    TrajectoryDTO trajectory;
    /// On-path replacements only: the mapping back to the dispatched path.
    std::optional<StopReplacementCursorMapDTO> cursor_map;
    /// The goal's sample association (shared by copies).
    std::shared_ptr<ExecutionSampleAssociation> association;
    bool moving_replacement{false};
    /// Exact terminal port fact read before the bounded native ring evicts it.
    std::optional<ExecutionGoalObservationDTO> terminal_observation{};
    double planned_controller_offset_s{0.0};
    std::shared_ptr<GoalCancelAttempt> cancel{};
  };
  struct Run;

  [[nodiscard]] ParticipantStopStageResultDTO RunControlledStage(Run& run);
  [[nodiscard]] ParticipantStopStageResultDTO RunReplacement(
      Run& run, const SoftOnPathStopPlanDTO& plan, const ExecutionControllerSampleDTO& trip,
      double frozen_factor, TimePoint stop_deadline,
      const std::function<Result<SoftOnPathStopPlanDTO>(double)>& replan);
  /// The fast spring-back, sent at once with no planning or checks.
  [[nodiscard]] ParticipantStopStageResultDTO RunFastSpringBack(
      Run& run, const ExecutionControllerSampleDTO& trip,
      const std::vector<double>& trip_positions, double frozen_factor, TimePoint stop_deadline);
  /// EXPERIMENTAL release timer: the cancellable pre-send hold; a result ends
  /// the stage (cancelled or stale state), nullopt sends.
  [[nodiscard]] std::optional<ParticipantStopStageResultDTO> HoldForRelease(
      Run& run, StopReplacementStartDTO& start, TimePoint dispatched_at, TimePoint stop_deadline,
      const std::string& mode);
  /// One dispatched Stop replacement, from its send result to its settlement.
  [[nodiscard]] ParticipantStopStageResultDTO FinishReplacement(
      Run& run, const ExecutionPortCommandResultDTO& sent, const ExecutionGoalKey& next,
      TrajectoryDTO sent_trajectory, std::optional<StopReplacementCursorMapDTO> cursor_map,
      const std::optional<ExecutionControllerSampleDTO>& baseline, TimePoint dispatched_at,
      const ExecutionControllerSampleDTO& trip, const std::string& mode,
      double initial_path_deviation_rad, TimePoint stop_deadline, double controller_offset_s,
      double frozen_factor, std::shared_ptr<GoalCancelAttempt> cancel_slot);
  [[nodiscard]] ParticipantStopStageResultDTO RunNaturalCompletion(Run& run,
                                                                   TimePoint stop_deadline,
                                                                   std::string basis);
  [[nodiscard]] ParticipantStopStageResultDTO RunHardStage(Run& run,
                                                           bool controlled_hard_stop_allowed);
  /// Level 3; nullopt with `fallback_reason` set when level 4 must follow.
  [[nodiscard]] std::optional<ParticipantStopStageResultDTO> TryControlledHardStop(
      Run& run, const SentGoal& live, const ExecutionControllerSampleDTO& sample, double factor,
      std::string& fallback_reason);
  [[nodiscard]] ParticipantStopStageResultDTO RunCancel(Run& run);
  /// No goal left to command: the controller holds; observe that hold.
  [[nodiscard]] ParticipantStopStageResultDTO RunEndedGoalHold(Run& run);
  [[nodiscard]] ParticipantStopStageResultDTO Succeed(Run& run, TimePoint finish_receipt,
                                                      std::string finish_basis);
  [[nodiscard]] ParticipantStopStageResultDTO Fail(const Run& run, ErrorDTO error) const;
  /// The stale-controller-state fault, when the lane declares a maximum
  /// age and the newest sample is older (or none exists).
  [[nodiscard]] std::optional<ErrorDTO> StaleControllerState() const;
  /// Waits like the free `await`, but every poll first checks the controller
  /// state's age; a stale state ends the wait unsuccessfully and is
  /// retained on `run` as the stage's failure.
  template <typename Done>
  bool Await(Run& run, TimePoint deadline, Done&& done);
  [[nodiscard]] bool CancelGoals(const std::vector<SentGoal>& goals,
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
  /// Non-terminal sent goals, newest first.
  [[nodiscard]] std::vector<SentGoal> LiveGoals() const;
  [[nodiscard]] std::optional<std::uint64_t> SourceExecutionId(
      const ExecutionGoalKey& key) const;
  /// Next replacement key of the dispatched goal (generation advances).
  [[nodiscard]] ExecutionGoalKey NextKey() const;
  /// Records a replacement that may have reached the mechanism.
  void Record(SentGoal goal);

  IExecutionPort& port_;
  const IClock& clock_;
  StopStageWaiter waiter_;
  StopSpeedFactorFreeze freeze_speed_factor_;
  StopStageHelperConfig config_;
  mutable std::mutex mutex_;
  std::optional<StopStageDispatchedGoalDTO> goal_;
  /// The dispatched goal followed by every Stop replacement sent for it.
  mutable std::vector<SentGoal> sent_;
};

}  // namespace manyforge::core
