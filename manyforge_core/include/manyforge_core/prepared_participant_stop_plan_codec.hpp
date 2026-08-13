#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <optional>
#include <span>
#include <string>

namespace manyforge::core {

struct PreparedParticipantStopPlanContextDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  std::optional<MotionInterruptStopProfileDTO> participant_stop_profile;
  ParticipantStopProfilePurposeDTO participant_stop_profile_purpose{
      ParticipantStopProfilePurposeDTO::kOrdinary};
  std::optional<std::string> motion_interrupt_scope_id;
  MotionInterruptNodeCategory node_category{MotionInterruptNodeCategory::kGeneric};
};

[[nodiscard]] Result<PreparedParticipantStopPlanDTO> MakePreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan,
    PreparedParticipantStopPlanContextDTO context,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits);

[[nodiscard]] Result<void> VerifyPreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan, const PreparedParticipantStopPlanDTO& plan,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

[[nodiscard]] Result<std::string> EncodePreparedParticipantStopPlan(
    const ResolvedStopBasePlanDTO& verified_base_plan, const PreparedParticipantStopPlanDTO& plan,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

[[nodiscard]] Result<PreparedParticipantStopPlanDTO> DecodePreparedParticipantStopPlan(
    std::string_view canonical_bytes, const ResolvedStopBasePlanDTO& verified_base_plan,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

}  // namespace manyforge::core
