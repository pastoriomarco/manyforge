#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kStopEvidencePolicyApiVersion =
    "manyforge.stop_evidence_policy.v2";
inline constexpr std::size_t kMaxStopEscalationStages = 8U;
inline constexpr std::size_t kMaxStopEvidencePolicyItems = 64U;
inline constexpr std::int64_t kMaxStopEvidencePolicyDurationNs =
    9007199254740991LL;

enum class StopEvidenceDisposition : std::uint8_t {
  kStoppable = 0,
  kNotStoppable,
};

enum class StopContainmentCommandKind : std::uint8_t {
  kControllerGoalCancel = 0,
  kMotionControlledStop,
  kCapabilityCommandCancel,
  kSafeOutputCommand,
  kLogicalCancel,
  kRegistered,
};

enum class StopEscalationStageKind : std::uint8_t {
  kNominal = 0,
  kControlledFallback,
  kSoftwareHard,
  kSafetySystemRequest,
};

enum class StopEvidenceConjunction : std::uint8_t {
  kAllOf = 0,
};

enum class StopEvidenceRequirementKind : std::uint8_t {
  kControllerGoalTerminal = 0,
  kMotionRest,
  kDiscreteAcknowledgement,
  kIoSafeOutput,
  kLogicalCancellationNoContinuingEffect,
  kRegistered,
};

enum class StopEvidenceSourceKind : std::uint8_t {
  kControllerGoalStatus = 0,
  kBindingStateAuthority,
  kCommandTargetStatus,
  kProviderEvidence,
};

enum class StopEvidenceObservationKind : std::uint8_t {
  kCommandResult = 0,
  kPostCommandSample,
  kPostCommandDwell,
};

[[nodiscard]] constexpr std::string_view to_string(
    StopEvidenceDisposition value) noexcept {
  switch (value) {
    case StopEvidenceDisposition::kStoppable:
      return "stoppable";
    case StopEvidenceDisposition::kNotStoppable:
      return "not_stoppable";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopContainmentCommandKind value) noexcept {
  switch (value) {
    case StopContainmentCommandKind::kControllerGoalCancel:
      return "controller_goal_cancel";
    case StopContainmentCommandKind::kMotionControlledStop:
      return "motion_controlled_stop";
    case StopContainmentCommandKind::kCapabilityCommandCancel:
      return "capability_command_cancel";
    case StopContainmentCommandKind::kSafeOutputCommand:
      return "safe_output_command";
    case StopContainmentCommandKind::kLogicalCancel:
      return "logical_cancel";
    case StopContainmentCommandKind::kRegistered:
      return "registered";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopEscalationStageKind value) noexcept {
  switch (value) {
    case StopEscalationStageKind::kNominal:
      return "nominal";
    case StopEscalationStageKind::kControlledFallback:
      return "controlled_fallback";
    case StopEscalationStageKind::kSoftwareHard:
      return "software_hard";
    case StopEscalationStageKind::kSafetySystemRequest:
      return "safety_system_request";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopEvidenceConjunction value) noexcept {
  switch (value) {
    case StopEvidenceConjunction::kAllOf:
      return "all_of";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopEvidenceRequirementKind value) noexcept {
  switch (value) {
    case StopEvidenceRequirementKind::kControllerGoalTerminal:
      return "controller_goal_terminal";
    case StopEvidenceRequirementKind::kMotionRest:
      return "motion_rest";
    case StopEvidenceRequirementKind::kDiscreteAcknowledgement:
      return "discrete_acknowledgement";
    case StopEvidenceRequirementKind::kIoSafeOutput:
      return "io_safe_output";
    case StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect:
      return "logical_cancellation_no_continuing_effect";
    case StopEvidenceRequirementKind::kRegistered:
      return "registered";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopEvidenceSourceKind value) noexcept {
  switch (value) {
    case StopEvidenceSourceKind::kControllerGoalStatus:
      return "controller_goal_status";
    case StopEvidenceSourceKind::kBindingStateAuthority:
      return "binding_state_authority";
    case StopEvidenceSourceKind::kCommandTargetStatus:
      return "command_target_status";
    case StopEvidenceSourceKind::kProviderEvidence:
      return "provider_evidence";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    StopEvidenceObservationKind value) noexcept {
  switch (value) {
    case StopEvidenceObservationKind::kCommandResult:
      return "command_result";
    case StopEvidenceObservationKind::kPostCommandSample:
      return "post_command_sample";
    case StopEvidenceObservationKind::kPostCommandDwell:
      return "post_command_dwell";
  }
  return {};
}

/// A role selected from the exact provider manifest plus the closed schema
/// that types its command or evidence payload. The retained bundle, not this
/// local reference alone, fixes the schema bytes and provider-manifest bytes.
struct ProviderContractRefDTO {
  ProviderContractRoleId provider_contract_role_id;
  SchemaId contract_schema_id;
  SchemaRevision contract_schema_revision;

  friend bool operator==(const ProviderContractRefDTO&,
                         const ProviderContractRefDTO&) = default;
};

/// One declarative containment-command requirement. It does not contain a
/// concrete endpoint or invocation plan. The deployment compiler must later
/// emit one deterministic template per legal route. Readiness resolves it
/// into an active-configuration-scoped base plan; Kernel Prepare later binds
/// operation/profile values into the executable participant plan. Stop-time
/// fallback discovery is forbidden.
struct StopContainmentCommandDTO {
  StopContainmentCommandKind command_kind;
  std::optional<ProviderContractRefDTO> provider_contract_ref;
  std::optional<RegisteredStopCommandKindId> registered_command_kind_id;

  friend bool operator==(const StopContainmentCommandDTO&,
                         const StopContainmentCommandDTO&) = default;
};

struct StopEvidenceSourceRequirementDTO {
  StopEvidenceSourceKind source_kind;
  std::int64_t freshness_budget_ns = 0;
  std::optional<EndpointRoleId> endpoint_role_id;
  std::vector<StateAuthorityClass> allowed_authority_classes;

  friend bool operator==(const StopEvidenceSourceRequirementDTO&,
                         const StopEvidenceSourceRequirementDTO&) = default;
};

struct StopEvidenceObservationDTO {
  StopEvidenceObservationKind observation_kind;
  std::optional<std::int64_t> minimum_dwell_ns;

  friend bool operator==(const StopEvidenceObservationDTO&,
                         const StopEvidenceObservationDTO&) = default;
};

struct StopEvidenceRequirementDTO {
  StopEvidenceItemId evidence_item_id;
  StopEvidenceRequirementKind evidence_kind;
  std::optional<ProviderContractRefDTO> provider_contract_ref;
  std::optional<RegisteredStopEvidenceKindId> registered_evidence_kind_id;
  StopEvidenceSourceRequirementDTO source_requirement;
  StopEvidenceObservationDTO observation_semantics;

  friend bool operator==(const StopEvidenceRequirementDTO&,
                         const StopEvidenceRequirementDTO&) = default;
};

/// One fixed member of the policy's ordered escalation chain. This is
/// configuration, not a runtime list of candidates: Kernel alone advances
/// through the retained order; each robot is bounded by its own Stop-time
/// deadlines (Q51), never by a shared policy budget.
struct StopEscalationStageDTO {
  StopEscalationStageId stop_escalation_stage_id;
  StopEscalationStageKind stage_kind;
  ContainmentGranularity containment_granularity;
  StopContainmentCommandDTO containment_command;
  StopEvidenceConjunction evidence_conjunction;
  std::vector<StopEvidenceRequirementDTO> evidence_requirements;

  friend bool operator==(const StopEscalationStageDTO&,
                         const StopEscalationStageDTO&) = default;
};

/// Exact normalized payload for `manyforge.stop_evidence_policy.v2`.
///
/// A qualified catalog payload is still declarative. It does not prove a
/// provider contract exists, expand containment topology, resolve endpoints,
/// construct an invocation plan, decide whether the authored all-of evidence
/// is sufficient for the selected command/provider, or authorize a runtime
/// Stop. Those checks require the retained bundle, exact
/// binding/provider/topology, and active configuration at
/// compiler/readiness/admission boundaries.
struct StopEvidencePolicyDTO {
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  StopEvidenceDisposition disposition;
  ContainmentGranularity containment_granularity;
  std::vector<StopEscalationStageDTO> escalation_stages;

  friend bool operator==(const StopEvidencePolicyDTO&,
                         const StopEvidencePolicyDTO&) = default;
};

}  // namespace manyforge::core
