#pragma once

#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::core {

class CheckedIoCondition;

/// Own desired-trip selection of one actual node, independent of global
/// Deployment rules. Presence is required for production qualification.
struct ProgramNodeIoRuleSemanticsDTO final {
  ProgramTargetReferenceDTO target;
  MotionInterruptActivationMode activation_mode;
  MotionInterruptStopMode stop_mode;
  MotionInterruptNodeCategory node_category;
  int priority;
  std::optional<StopEscalationStageId> stop_entry_stage_id;
  std::optional<StopEscalationStageId> success_through_stage_id;
};

/// One node's actual authored condition after sealed Program/Binding resolution.
/// Its internal identity and own metadata never select a Deployment policy.
/// This value grants no runtime scope.
struct ProgramNodeIoConditionDTO final {
  ProgramNodeInstanceId node_instance_id;
  /// Compiler-generated stable internal identity, never an authored reference.
  InterruptPolicyId policy_id;
  std::shared_ptr<const CheckedIoCondition> condition;
  std::vector<InterruptSourceId> required_signal_sources;
  std::optional<ProgramNodeIoRuleSemanticsDTO> node_rule{};
};

/// Immutable Program-only projection. Deployment conditions must be joined by
/// their qualified activation owner before claiming a complete referenced-key
/// union or installing a blackboard source. No computed dependency is included
/// unless a checked condition itself references that key.
struct ProgramIoConditionProjectionDTO final {
  std::vector<ProgramNodeIoConditionDTO> program_node_conditions;
  std::vector<IoBlackboardKeyDescriptorDTO> referenced_program_blackboard_keys;
};

}  // namespace manyforge::core
