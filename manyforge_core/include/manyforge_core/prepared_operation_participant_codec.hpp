#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::core {

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
[[nodiscard]] Result<PreparedOperationParticipantDTO> DecodePreparedOperationParticipant(
    std::string_view canonical_bytes, const RuntimeParticipantBinding& binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

}  // namespace manyforge::core
