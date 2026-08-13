#pragma once

#include <cstdint>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/resolved_stop_base_plan.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kPreparedParticipantStopPlanApiVersion =
    "manyforge.prepared_participant_stop_plan.v1";
inline constexpr std::string_view kParticipantStopInvocationPlanIdentityDomain =
    "participant-stop-invocation-plan-digest";

enum class StopStageParameterSourceDTO : std::uint8_t {
  kDeploymentDefault = 0,
  kParticipantProfileMapping,
};

[[nodiscard]] constexpr std::string_view to_string(StopStageParameterSourceDTO value) noexcept {
  switch (value) {
    case StopStageParameterSourceDTO::kDeploymentDefault:
      return "deployment_default";
    case StopStageParameterSourceDTO::kParticipantProfileMapping:
      return "participant_profile_mapping";
  }
  return {};
}

enum class ParticipantStopProfilePurposeDTO : std::uint8_t {
  kOrdinary = 0,
  kOperator,
  kFault,
  kMotionInterruptScope,
};

[[nodiscard]] constexpr std::string_view to_string(
    ParticipantStopProfilePurposeDTO value) noexcept {
  switch (value) {
    case ParticipantStopProfilePurposeDTO::kOrdinary:
      return "ordinary";
    case ParticipantStopProfilePurposeDTO::kOperator:
      return "operator";
    case ParticipantStopProfilePurposeDTO::kFault:
      return "fault";
    case ParticipantStopProfilePurposeDTO::kMotionInterruptScope:
      return "motion_interrupt_scope";
  }
  return {};
}

/// One exact verified base-stage row plus the complete selected provider
/// parameters. The nested row is retained byte-equivalently rather than
/// partially restated by another wire contract.
struct PreparedStopStageCommandDTO final {
  ResolvedStopEscalationStageDTO resolved_stage;
  std::optional<JsonObjectDTO> validated_contract_parameters;
  std::optional<StopStageParameterSourceDTO> parameter_source;

  friend bool operator==(const PreparedStopStageCommandDTO&,
                         const PreparedStopStageCommandDTO&) = default;
};

/// Immutable, content-digested Kernel Prepare product for one participant and
/// one exact optional Stop profile. Multiple interrupt-scope profiles produce
/// separate values and are never merged.
struct PreparedParticipantStopPlanDTO final {
  ContentDigest participant_stop_invocation_plan_digest;
  ResolvedStopBasePlanId resolved_stop_base_plan_id;
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
  StopDeadlineBudgetDTO deadline_budget;
  std::vector<PreparedStopStageCommandDTO> stage_commands;

  friend bool operator==(const PreparedParticipantStopPlanDTO&,
                         const PreparedParticipantStopPlanDTO&) = default;
};

}  // namespace manyforge::core
