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
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace manyforge::core {

// ---------------------------------------------------------------------------
// Core Stop-stage helper (shared execution design §4.2, §7.3).
//
// Realizes one robot participant's Stop stages over `IExecutionPort` with the
// Kernel's existing stage contract (`IControlDomainExecutionPort`). It is the
// single owner of the Stop policy previously duplicated in the native and ROS
// backends: stage identity and membership, the on-path soft Stop and its
// spring-back substitution (levels 1-2), natural completion, the controlled
// hard stop (level 3, Q42) and the controller cancel (level 4, Q2/Q35), and
// the outcome mapping (Q18, Q33, item 25). Stop selection, escalation and the
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
};

/// Lane and deployment facts the helper needs. None has a helper default: each
/// comes from the lane facts, characterization or deployment.
struct StopStageHelperConfig final {
  /// The participant binding's resources (source and compiled joint names).
  std::vector<RuntimeResourceBindingDTO> resources;
  /// Trip-evidence authority (§4.2 step 1): kMeasured on real robots (measured
  /// position and velocity), kCommanded on Isaac and offline (the controller's
  /// reference, velocity from the cursor at the frozen factor).
  StateAuthorityClass trip_authority{StateAuthorityClass::kCommanded};
  /// Stop planner lane facts (`main`'s backend configuration).
  double stop_traj_delta_s{0.0};
  double sample_period_s{0.0};
  std::optional<double> max_initial_path_deviation_rad;
  std::optional<double> max_initial_velocity_deviation_rad_s;
  /// Q26/Q42: lane-measured wall time from a replacement's dispatch until the
  /// mechanism starts it (sample transport included). Required lane input:
  /// there is no default; the helper refuses construction without it.
  std::optional<double> replacement_start_allowance_s{};
  /// Q42: the lane's JTC `max_deceleration_on_cancel` per compiled joint. Empty
  /// disables level 3 (the hard stop is then the cancel, level 4).
  std::map<std::string, double> max_deceleration_on_cancel_rad_s2;
  /// Q28 completion tolerance (`main`'s `complete_move_on_signal_timeout_ns`).
  Duration complete_move_on_signal_timeout_ns{500'000'000};
  /// Q28 (as `main`'s `stop_completion_timeout_s`): the lane's bound on one
  /// hard-stop stage (levels 3 and 4), measured from the stage start. Required.
  Duration hard_stop_completion_timeout_ns{0};
  /// Q59 (`main`'s `joint_state_max_age_s`): while a Stop stage waits on the
  /// controller, a newest controller sample older than this (local receipt)
  /// is stale controller state and fails the stage at once. 0: the lane
  /// declares no maximum age.
  Duration controller_state_max_age_ns{0};
};

/// Blocks until `deadline`, until `cancellation` is requested, or earlier when
/// the port may have new facts (production: the next controller sample or goal
/// event). Tests drive time and port facts through it.
using StopStageWaiter = std::function<void(TimePoint deadline, const std::stop_token& cancellation)>;

/// Stops the speed-factor publication (Q17/Q38) and returns the frozen factor:
/// the last value sent on this binding epoch (or the lane's initial factor when
/// none was sent). Idempotent.
using StopSpeedFactorFreeze = std::function<double()>;

class ParticipantStopStageHelper final : public IControlDomainExecutionPort {
 public:
  ParticipantStopStageHelper(IExecutionPort& port, const IClock& clock, StopStageWaiter waiter,
                             StopSpeedFactorFreeze freeze_speed_factor,
                             StopStageHelperConfig config);

  /// Records the goal the participant submitted through the port with its
  /// sample association (Q50, shared with the participant). Replaces any
  /// earlier goal and its Stop replacements.
  void BindDispatchedGoal(StopStageDispatchedGoalDTO goal,
                          std::shared_ptr<ExecutionSampleAssociation> association);

  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation) override;

  /// Whether the dispatched goal or one of its Stop replacements may still run
  /// (its goal record is not terminal).
  [[nodiscard]] bool HasLiveGoal() const { return !LiveGoals().empty(); }

  /// §3.3: the dispatched trajectory's own cursor during and after its newest
  /// on-path Stop replacement (soft Stop, spring-back), mapped with the Stop
  /// planner's sample mapping: from `sample` once it is associated with the
  /// replacement, its end once the replacement succeeded (the hold freezes
  /// it). nullopt when no on-path replacement was sent, when the newest
  /// replacement left the path (controlled hard stop) or ended otherwise, or
  /// when `sample` is not (yet) the replacement's.
  [[nodiscard]] std::optional<double> OriginalPathCursor(
      const std::optional<ExecutionControllerSampleDTO>& sample) const;

 private:
  struct SentGoal {
    ExecutionGoalKey key;
    TrajectoryDTO trajectory;
    /// On-path replacements only: the mapping back to the dispatched path.
    std::optional<StopReplacementCursorMapDTO> cursor_map;
    /// The goal's sample association (Q50; shared by copies).
    std::shared_ptr<ExecutionSampleAssociation> association;
  };
  struct Run;

  [[nodiscard]] ParticipantStopStageResultDTO RunControlledStage(Run& run);
  [[nodiscard]] ParticipantStopStageResultDTO RunReplacement(
      Run& run, const SoftOnPathStopPlanDTO& plan, const ExecutionControllerSampleDTO& trip,
      double frozen_factor, TimePoint stop_deadline);
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
  /// Q59: the stale-controller-state fault, when the lane declares a maximum
  /// age and the newest sample is older (or none exists).
  [[nodiscard]] std::optional<ErrorDTO> StaleControllerState() const;
  /// Waits like the free `await`, but every poll first checks the controller
  /// state's age (Q59); a stale state ends the wait unsuccessfully and is
  /// retained on `run` as the stage's failure.
  template <typename Done>
  bool Await(Run& run, TimePoint deadline, Done&& done);
  /// Non-terminal sent goals, newest first.
  [[nodiscard]] std::vector<SentGoal> LiveGoals() const;
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
  std::vector<SentGoal> sent_;
};

}  // namespace manyforge::core
