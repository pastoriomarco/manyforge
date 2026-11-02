#pragma once

#include <manyforge_core/types/io_motion_rule.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::core {

/// One closed factual representation shared by public, native and durable
/// operation projections. It never derives a policy result or recovery grant.
[[nodiscard]] Result<std::string> EncodeIoRuleOutcomes(
    std::span<const IoMotionRuleOutcomeDTO> outcomes);
[[nodiscard]] Result<std::vector<IoMotionRuleOutcomeDTO>> DecodeIoRuleOutcomes(
    std::string_view canonical_json);
/// Historical containment permits a positive execution origin no later than
/// the current control revision. Every other addressed identity remains exact.
/// Live rule admission validates the exact prepared execution separately.
[[nodiscard]] Result<void> ValidateIoRuleOutcomeParticipant(
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id,
    const ControllableComponentId& controllable_component_id,
    const CommandTargetId& command_target_id, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation,
    std::span<const IoMotionRuleOutcomeDTO> outcomes);
[[nodiscard]] Result<void> ValidateIoRuleOutcomeParticipant(
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantStateDTO& participant);

}  // namespace manyforge::core
