#pragma once

#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

enum class IoRuleIdleBehaviourDTO { kLatch, kIgnore };

/// Exact motion subjects resolved from the qualified Applied Program group
/// table and sealed binding. These are selection facts, never execution claims.
struct IoActivationMotionSubjectDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControlDomainId control_domain_id;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  friend auto operator<=>(const IoActivationMotionSubjectDTO&,
                          const IoActivationMotionSubjectDTO&) = default;
};
struct IoActivationAllMotionScopeDTO final {
  friend bool operator==(const IoActivationAllMotionScopeDTO&,
                         const IoActivationAllMotionScopeDTO&) = default;
};
struct IoActivationExecutionGroupsScopeDTO final {
  ProgramBindingId program_binding_id;
  std::vector<std::string> group_ids;
  std::vector<IoActivationMotionSubjectDTO> subjects;
  friend bool operator==(const IoActivationExecutionGroupsScopeDTO&,
                         const IoActivationExecutionGroupsScopeDTO&) = default;
};
struct IoActivationEverythingScopeDTO final {
  friend bool operator==(const IoActivationEverythingScopeDTO&,
                         const IoActivationEverythingScopeDTO&) = default;
};
using IoActivationRuleScopeSelectionDTO =
    std::variant<IoActivationAllMotionScopeDTO, IoActivationExecutionGroupsScopeDTO,
                 IoActivationEverythingScopeDTO>;

/// One actual qualified global rule. Node rules are separate. There is no
/// legacy trigger/template, caller Stop profile, or desired-trip threshold.
struct IoActivationRulePolicyBindingDTO final {
  InterruptPolicyId policy_id;
  std::shared_ptr<const CheckedIoCondition> condition;
  std::vector<InterruptSourceId> required_signal_sources;
  IoActivationRuleScopeSelectionDTO scope;
  /// Absence preserves the Run-in-flight idle default, evaluated by Kernel.
  std::optional<IoRuleIdleBehaviourDTO> idle_behaviour;
  int effective_priority;
  std::optional<StopEscalationStageId> stop_entry_stage_id{};
  InterruptPostLatchDisposition post_latch_disposition{
      InterruptPostLatchDisposition::kAwaitOperatorAck};
  /// With no Applied Program, the whole rule waits without a checked
  /// condition or fabricated key types/values. Canonical literal user keys.
  std::vector<std::string> waiting_for_program_keys{};
  /// Authored `on_trip: full_stop`: a trip with motion in scope is a cell
  /// Full Stop (as scope `everything`). `gate` keeps the scoped Stop.
  /// The release (post_latch_disposition) and idle latch are compiled with it.
  bool full_stop_on_trip{false};
  /// Notify publishes condition state without a motion effect or latch.
  bool notify_on_trip{false};
  friend bool operator==(const IoActivationRulePolicyBindingDTO&,
                         const IoActivationRulePolicyBindingDTO&) = default;
};

/// Immutable installed catalog, owned by the same Kernel activation lifetime.
/// Catalog generation is the actual signal service generation, not a second
/// counter. The host supplies this only under its actual Applied composition
/// pin; Kernel rechecks configuration, admission fence and service identity.
struct IoActivationRuleCatalogDTO final {
  IoRuleStateContextDTO context;
  std::vector<IoActivationRulePolicyBindingDTO> rules;
  std::optional<ProgramBindingId> program_binding_id{};
  /// Exact canonical union derived from the above checked condition inputs.
  std::vector<std::string> referenced_program_blackboard_keys;
  friend bool operator==(const IoActivationRuleCatalogDTO&,
                         const IoActivationRuleCatalogDTO&) = default;
};

enum class IoRuleStateProjectionStatusDTO { kPending, kReady };
/// Actual successful Applied identity, including a qualified empty product
/// that has no signal service and therefore no catalog generation.
struct IoRuleAppliedContextDTO final {
  RequestOwnerEpoch owner_epoch;
  ActiveBundlePublicationGeneration active_publication_generation;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  friend bool operator==(const IoRuleAppliedContextDTO&,
                         const IoRuleAppliedContextDTO&) = default;
};
/// The no-service alternative of the same Applied rule product. Every rule
/// waits for a Program; no signal catalog generation or observer is invented.
struct IoRuleWaitingForProgramCatalogDTO final {
  IoRuleAppliedContextDTO context;
  std::vector<IoActivationRulePolicyBindingDTO> rules;
  friend bool operator==(const IoRuleWaitingForProgramCatalogDTO&,
                         const IoRuleWaitingForProgramCatalogDTO&) = default;
};
struct IoRuleStateProjectionDTO final {
  IoRuleStateProjectionStatusDTO status;
  /// Present only at a coherent ready cut, with no older queued/in-flight
  /// transition. Projection failure does not erase actual rule/latch state.
  std::optional<IoRuleStateBaselineDTO> baseline;
  /// Same-lock actual catalog snapshot for both existing events siblings.
  /// Consumers must not pair two separately captured getter results.
  std::shared_ptr<const IoActivationRuleCatalogDTO> catalog{};
  /// Present only after actual successful Apply and exact empty qualification;
  /// catalog and baseline are then absent. Never inferred from their absence.
  std::optional<IoRuleAppliedContextDTO> verified_empty{};
  /// Same-lock no-service waiting product, exclusive with catalog and
  /// verified_empty. Its state baseline is pending, never fabricated clear.
  std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program{};
};

}  // namespace manyforge::core
