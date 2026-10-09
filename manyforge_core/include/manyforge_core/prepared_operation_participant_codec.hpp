#pragma once

#include <manyforge_core/execution/prepared_motion_slot.hpp>
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
/// Sealed transport inputs of a prepared motion participant (SX-032): the
/// verified canonical participant bytes (same verification as
/// `EncodePreparedOperationParticipant`), its digest and the canonical bytes of
/// its motion payload trajectory. Refuses a non-motion payload. Cold path.
[[nodiscard]] Result<SealedPreparedMotionInputs> MakeSealedPreparedMotionInputs(
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
