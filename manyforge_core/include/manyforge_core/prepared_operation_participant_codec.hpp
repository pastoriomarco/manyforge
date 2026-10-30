#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Provider outputs are sealed without controller topology or physical Stop evidence.
[[nodiscard]] Result<PreparedOperationParticipantDTO> MakePreparedIoOutputParticipant(
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id, const ProgramRunId& program_run_id,
    const CapabilityCommandIntentDTO& command, const RuntimeParticipantBinding& binding);
[[nodiscard]] Result<PreparedOperationParticipantDTO> MakePreparedIoOutputParticipant(
    PreparedOperationParticipantDTO candidate, const RuntimeParticipantBinding& binding);
[[nodiscard]] Result<void> VerifyPreparedIoOutputParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding);
[[nodiscard]] Result<std::string> EncodePreparedIoOutputParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding);

[[nodiscard]] Result<PreparedOperationParticipantDTO> MakePreparedOperationParticipant(
    PreparedOperationParticipantDTO candidate, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);
[[nodiscard]] Result<void> VerifyPreparedOperationParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);
[[nodiscard]] Result<std::string> EncodePreparedOperationParticipant(
    const PreparedOperationParticipantDTO& value, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

}  // namespace manyforge::core
