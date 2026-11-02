#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Ordinary calls retain rest-boundary checks. A typed source context admits
/// only its exact unchanged prefix and PVA, never dispatch authorization.
[[nodiscard]] Result<std::string> EncodePreparedTrajectory(
    const TrajectoryDTO& value, const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<TrajectoryDTO> DecodePreparedTrajectory(
    std::string_view canonical_bytes, const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<std::string> EncodePreparedPlanRequest(const PlanRequestDTO& value);
[[nodiscard]] Result<PlanRequestDTO> DecodePreparedPlanRequest(std::string_view canonical_bytes);
[[nodiscard]] Result<std::string> EncodePreparedPlanResult(
    const PlanResultDTO& value, const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<PlanResultDTO> DecodePreparedPlanResult(
    std::string_view canonical_bytes, const MovingReplacementInputDTO* replacement = nullptr);
[[nodiscard]] Result<std::string> EncodePreparedControlledState(const ControlledStateDTO& value);
[[nodiscard]] Result<ControlledStateDTO> DecodePreparedControlledState(
    std::string_view canonical_bytes);

}  // namespace manyforge::core
