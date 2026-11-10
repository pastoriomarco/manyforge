#pragma once

#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_core/types/io_rule.hpp>
#include <manyforge_core/types/io_rule_service_configuration.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/program_mock_simulation.hpp>
#include <span>

namespace manyforge::core {

/// Read-only observations and zero-claim watches; no physical command authority.
/// The existing host must validate each live Run/node-attempt before registering,
/// serialized with its authoritative teardown. ReleaseRunWatches drains watches
/// after that existing gate has closed; this port is not another Run registry.
class IIoSignalService {
 public:
  virtual ~IIoSignalService() = default;
  /// Trusted existing Kernel/host source owner; public packets cannot install.
  virtual Result<void> InstallProgramBlackboardSource(const IoProgramBlackboardSourceBindingDTO&,
                                                      const std::optional<IoSourceIncarnation>&) {
    return Result<void>::Err(BlackboardUnsupported());
  }
  /// One owner-qualified replacement baseline. Validation precedes mutation;
  /// planned Run/Reset handoff neither creates a source loss nor clears one.
  virtual Result<IoProgramBlackboardAcknowledgementDTO> PublishProgramBlackboardBaseline(
      const IoProgramBlackboardSourceBindingDTO&,
      const std::optional<IoSourceIncarnation>&,
      const IoProgramBlackboardPublicationDTO&) {
    return Result<IoProgramBlackboardAcknowledgementDTO>::Err(BlackboardUnsupported());
  }
  virtual Result<IoProgramBlackboardAcknowledgementDTO> PublishProgramBlackboard(
      const IoProgramBlackboardPublicationDTO&) {
    return Result<IoProgramBlackboardAcknowledgementDTO>::Err(BlackboardUnsupported());
  }
  virtual Result<void> RetireProgramBlackboardSource(const IoProgramBlackboardSourceBindingDTO&) {
    return Result<void>::Err(BlackboardUnsupported());
  }

  /// Positive only for an exact retained retirement record and an available
  /// accepted replacement baseline. Kernel supplies current health/context;
  /// missing or evicted proof never grants a superseded acknowledgement.
  virtual Result<bool> WasProgramBlackboardScopeRetired(const IoProgramBlackboardScopeDTO&) const {
    return Result<bool>::Err(BlackboardUnsupported());
  }

  virtual Result<IoRuleServiceConfigurationDTO> RuleServiceConfiguration() const {
    return Result<IoRuleServiceConfigurationDTO>::Err(RuleUnsupported());
  }

  virtual Result<IoRuleRegistrationDTO> ArmRule(const IoRuleArmRequestDTO&) {
    return Result<IoRuleRegistrationDTO>::Err(RuleUnsupported());
  }
  virtual Result<IoRuleConditionSnapshotDTO> ReadRuleConditionSnapshot(
      const IoRuleRegistrationDTO&) const {
    return Result<IoRuleConditionSnapshotDTO>::Err(RuleUnsupported());
  }
  virtual Result<IoRuleEvidenceDTO> ReadRuleEvidence(const IoRuleRegistrationDTO&) const {
    return Result<IoRuleEvidenceDTO>::Err(RuleUnsupported());
  }
  virtual Result<IoRuleEvidenceDTO> RetireRule(const IoRuleRegistrationDTO&) {
    return Result<IoRuleEvidenceDTO>::Err(RuleUnsupported());
  }
  virtual Result<void> EvaluateRules() { return Result<void>::Err(RuleUnsupported()); }
  virtual Result<std::optional<TimePoint>> NextRuleExpiry() const {
    return Result<std::optional<TimePoint>>::Err(RuleUnsupported());
  }
  /// Clears one output's core-held `changed_outside` signal
  /// (node `io.clear_outside_change`); refused for an output without it.
  virtual Result<void> ClearOutsideChange(const IoSignalId&) {
    return Result<void>::Err(ChangedOutsideUnsupported());
  }
  /// Clears every `changed_outside` signal of a component that became
  /// `prepared` (the value at that moment isn't a change).
  virtual Result<void> ClearComponentOutsideChanges(const ControllableComponentId&) {
    return Result<void>::Err(ChangedOutsideUnsupported());
  }
  virtual Result<IoSignalObservationDTO> ReadSignal(const IoSignalId& signal) const = 0;
  virtual Result<IoWatchRegistrationDTO> RegisterLevelWatch(
      const IoLevelWatchRequestDTO& request) = 0;
  virtual Result<IoWatchResultDTO> ReadWatch(const IoWatchRegistrationDTO& registration) const = 0;
  virtual Result<void> ReleaseWatch(const IoWatchRegistrationDTO& registration) = 0;
  // The existing Kernel gate owns late-register fencing. This service only
  // erases actual watches for the exact attempt, never installs tombstones.
  virtual Result<void> ReleaseNodeWatches(const IoWatchScopeDTO&) {
    return Result<void>::Err({"unsupported_capability",
                              "I/O scoped watch release is not installed",
                              {{"reason", "io_watch_scope_release_unsupported"}}});
  }
  virtual Result<void> ReleaseRunWatches(const RequestOwnerEpoch& runner_owner_epoch,
                                         const ProgramRunId& run_id) = 0;

  /// Called only under the existing current captured Run admission lock,
  /// serialized with ReleaseRunWatches. The service alone owns bounded per-node
  /// index/replay/timers; the Kernel must not duplicate that ordering state.
  /// Same index/attempt/rows returns the original success OR error inertly.
  /// maximum_node_entries is the actual captured node-table size, not caller
  /// capacity. There is no condition evaluator or physical command grant.
  /// Explicit initial actual paused state AND complete held-node mask must be
  /// accepted before the first node START; absence never means running.
  /// Effective pause is Run paused OR node held. Changes retain remaining time.
  /// Lower revisions are inert; equal revision with another snapshot refuses.
  virtual Result<void> SetProgramRunMockSimulationPaused(
      const ProgramRunMockSimulationPauseRequestDTO&) {
    return Result<void>::Err({"unsupported_capability",
        "Run Mock simulation clock is not installed", {{"reason", "mock_simulation_unsupported"}}});
  }

  /// End affects only the exact current captured Run/node occurrence. Older
  /// END is inert; retained original START results are never republished.
  virtual Result<void> EndProgramNodeMockSimulation(
      const ProgramNodeMockSimulationScopeDTO&, std::size_t maximum_node_entries) {
    (void)maximum_node_entries;
    return Result<void>::Err(ErrorDTO{"unsupported_capability",
        "Node Mock simulation end is not installed", {{"reason", "mock_simulation_unsupported"}}});
  }

  virtual Result<ProgramNodeMockSimulationAcknowledgementDTO>
  StartProgramNodeMockSimulation(
      const ProgramNodeMockSimulationRequestDTO&,
      std::span<const ResolvedProgramMockSimulationSignalDTO>, std::size_t maximum_node_entries) {
    (void)maximum_node_entries;
    return Result<ProgramNodeMockSimulationAcknowledgementDTO>::Err(
        {"unsupported_capability", "Node Mock simulation is not installed",
         {{"reason", "mock_simulation_unsupported"}}});
  }

 private:
  static ErrorDTO BlackboardUnsupported() {
    return {"unsupported_capability",
            "Program blackboard source is not installed",
            {{"reason", "program_bb_service_unsupported"}}};
  }
  static ErrorDTO ChangedOutsideUnsupported() {
    return {"unsupported_capability",
            "Core-held changed_outside signals are not installed",
            {{"reason", "io_changed_outside_unsupported"}}};
  }
  static ErrorDTO RuleUnsupported() {
    return {"unsupported_capability",
            "I/O rule service is not installed",
            {{"reason", "io_rule_service_unsupported"}}};
  }
};

}  // namespace manyforge::core
