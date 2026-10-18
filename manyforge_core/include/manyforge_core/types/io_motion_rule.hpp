#pragma once

#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/types/io_rule.hpp>
#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <set>

namespace manyforge::core {

enum class StopCertificateResultDTO : std::uint8_t;
enum class OperationStateKind;
enum class IoRuleStopScopeDTO { kAffectedResources, kFullCell };

/// Exact compiler-resolved maximum scope. Absence never means all resources.
struct IoRulePolicyScopeDTO final {
  std::vector<ControllableComponentId> controllable_component_ids;
  std::vector<CommandTargetId> command_target_ids;
  std::vector<ControlDomainId> control_domain_ids;
  std::vector<ControlBindingId> control_binding_ids;
  std::vector<ResourceId> resource_ids;
  IoRuleStopScopeDTO stop_scope;
};

/// Actual compiled policy semantics. Absence is unqualified, never an implicit
/// desired-trip action. Activation-owned static policies cannot borrow an
/// operation registration's lifetime.
struct IoMotionRulePolicySemanticsDTO final {
  InterruptPolicyScopeKind scope_kind;
  InterruptPreemptionAction preemption_action;
  std::set<OperationStateKind> affected_phases;
  InterruptPostLatchDisposition post_latch_disposition;
  bool effective_maskable;
  std::vector<InterruptPolicyId> maskable_by;
};

/// Existing activation composition resolves this from its sealed policy catalog
/// for the exact participant registration. This value is not public request
/// input or a separately mutable runtime policy registry.
struct IoMotionRulePolicyBindingDTO final {
  InterruptPolicyId policy_id;
  std::shared_ptr<const CheckedIoCondition> condition;
  std::vector<InterruptSourceId> required_sources;
  std::set<MotionInterruptActivationMode> allowed_activation_modes;
  std::set<MotionInterruptStopMode> allowed_stop_modes;
  /// Already resolved policy override/source priority from the sealed catalog.
  int effective_priority;
  std::optional<IoMotionRulePolicySemanticsDTO> semantics{};
  std::optional<IoRulePolicyScopeDTO> maximum_scope{};
  /// Absent selects the first qualified row of the actual retained robot chain.
  std::optional<StopEscalationStageId> stop_entry_stage_id{};
  std::optional<StopEscalationStageId> success_through_stage_id{};
};

enum class IoRuleArbitrationDispositionDTO {
  kActive,
  kQueued,
  kSuppressedByActiveMask,
  kSuperseded,
};

/// Typed facts remain separate: condition/source evidence, actual Stop
/// acceptance, actual certificate, and terminal motion cause. Pending evidence
/// and an accepted Stop alone can never populate signal_trip.
struct IoMotionRuleOutcomeDTO final {
  IoRuleRegistrationDTO registration;
  IoRuleEvidenceDTO evidence;
  std::optional<StopId> stop_id{};
  std::optional<StopGeneration> stop_generation{};
  std::optional<StopCertificateId> stop_certificate_id{};
  std::optional<StopCertificateRevision> stop_certificate_revision{};
  std::optional<StopCertificateResultDTO> stop_result{};
  std::optional<MotionExecutionTerminalCause> terminal_cause{};
  std::optional<ErrorDTO> error{};
  std::optional<IoRuleArbitrationDispositionDTO> arbitration_disposition{};
  friend bool operator==(const IoMotionRuleOutcomeDTO&, const IoMotionRuleOutcomeDTO&) = default;
};

}  // namespace manyforge::core
