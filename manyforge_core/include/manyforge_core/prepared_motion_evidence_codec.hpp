#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

[[nodiscard]] Result<std::string> EncodePreparedMotionPlanningEvidence(
    const PreparedMotionPlanningEvidenceDTO& value,
    const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<PreparedMotionPlanningEvidenceDTO> DecodePreparedMotionPlanningEvidence(
    std::string_view canonical_bytes, const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<std::string> EncodePreparedMotionValidationEvidence(
    const PreparedMotionValidationEvidenceDTO& value);
[[nodiscard]] Result<PreparedMotionValidationEvidenceDTO> DecodePreparedMotionValidationEvidence(
    std::string_view canonical_bytes);

}  // namespace manyforge::core
