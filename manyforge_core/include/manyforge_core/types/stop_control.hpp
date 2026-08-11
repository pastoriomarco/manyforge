#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_core/types/retained_artifact.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view
    kMotionControlledStopContractSchemaId =
        "manyforge.value_schema.provider_contract.motion_controlled_stop.v1";
inline constexpr std::string_view
    kMotionControlledStopContractSchemaRevision =
        "sha256:afbbcda3b81eabff2bbdffb5658fb1fcd3331cde896cadf0681793a2f9db90dd";
inline constexpr std::size_t kMaxMotionControlledStopResourceLimits = 256U;

enum class MotionControlledStopMode : std::uint8_t {
  kSoftOnPath = 0,
  kSpringBackHold,
  kHardDeceleration,
};

[[nodiscard]] constexpr std::string_view to_string(
    MotionControlledStopMode value) noexcept {
  switch (value) {
    case MotionControlledStopMode::kSoftOnPath:
      return "soft_on_path";
    case MotionControlledStopMode::kSpringBackHold:
      return "spring_back_hold";
    case MotionControlledStopMode::kHardDeceleration:
      return "hard_deceleration";
  }
  return {};
}

struct ControlledStopResourceLimitDTO final {
  ResourceId resource_id;
  CoordinateAccelerationDTO max_deceleration;
  CoordinateVelocityDTO settle_velocity_threshold;

  friend bool operator==(const ControlledStopResourceLimitDTO&,
                         const ControlledStopResourceLimitDTO&) = default;
};

struct HardStopResourceLimitDTO final {
  ResourceId resource_id;
  CoordinateVelocityDTO settle_velocity_threshold;

  friend bool operator==(const HardStopResourceLimitDTO&,
                         const HardStopResourceLimitDTO&) = default;
};

struct SoftOnPathStopParametersDTO final {
  MotionControlledStopMode stop_mode{MotionControlledStopMode::kSoftOnPath};
  double deceleration_time_s{};
  std::vector<ControlledStopResourceLimitDTO> per_resource_limits;
  bool allow_natural_completion_on_short_path{};
  std::optional<MotionControlledStopMode> recoverable_substitution_mode;

  friend bool operator==(const SoftOnPathStopParametersDTO&,
                         const SoftOnPathStopParametersDTO&) = default;
};

struct SpringBackHoldStopParametersDTO final {
  MotionControlledStopMode stop_mode{
      MotionControlledStopMode::kSpringBackHold};
  double deceleration_time_s{};
  std::vector<ControlledStopResourceLimitDTO> per_resource_limits;

  friend bool operator==(const SpringBackHoldStopParametersDTO&,
                         const SpringBackHoldStopParametersDTO&) = default;
};

struct HardDecelerationStopParametersDTO final {
  MotionControlledStopMode stop_mode{
      MotionControlledStopMode::kHardDeceleration};
  std::vector<HardStopResourceLimitDTO> per_resource_limits;

  friend bool operator==(const HardDecelerationStopParametersDTO&,
                         const HardDecelerationStopParametersDTO&) = default;
};

using MotionControlledStopContractParametersDTO =
    std::variant<SoftOnPathStopParametersDTO,
                 SpringBackHoldStopParametersDTO,
                 HardDecelerationStopParametersDTO>;

enum class StopActionClass : std::uint8_t {
  kControlled = 0,
  kSoftwareHard,
  kSafetySystemRequest,
};

enum class SafetySystemRequestActionClass : std::uint8_t {
  kProtectiveStopRequest = 0,
  kEmergencyStopRequest,
};

[[nodiscard]] constexpr std::string_view to_string(
    StopActionClass value) noexcept {
  switch (value) {
    case StopActionClass::kControlled:
      return "controlled";
    case StopActionClass::kSoftwareHard:
      return "software_hard";
    case StopActionClass::kSafetySystemRequest:
      return "safety_system_request";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    SafetySystemRequestActionClass value) noexcept {
  switch (value) {
    case SafetySystemRequestActionClass::kProtectiveStopRequest:
      return "protective_stop_request";
    case SafetySystemRequestActionClass::kEmergencyStopRequest:
      return "emergency_stop_request";
  }
  return {};
}

struct RegisteredStopCommandDefinitionDTO final {
  RegisteredStopCommandKindId registered_command_kind_id;
  StopActionClass stop_action_class;
  std::optional<SafetySystemRequestActionClass>
      safety_system_request_action_class;

  friend bool operator==(const RegisteredStopCommandDefinitionDTO&,
                         const RegisteredStopCommandDefinitionDTO&) = default;
};

/// Complete immutable table fixed by spec 240. Consumers classify a
/// registered command only through this table, never through token spelling.
inline const std::array<RegisteredStopCommandDefinitionDTO, 4U>
    kRegisteredStopCommandDefinitions{{
        {RegisteredStopCommandKindId("manyforge.stop.controlled.provider"),
         StopActionClass::kControlled, std::nullopt},
        {RegisteredStopCommandKindId("manyforge.stop.safety.emergency_request"),
         StopActionClass::kSafetySystemRequest,
         SafetySystemRequestActionClass::kEmergencyStopRequest},
        {RegisteredStopCommandKindId("manyforge.stop.safety.protective_request"),
         StopActionClass::kSafetySystemRequest,
         SafetySystemRequestActionClass::kProtectiveStopRequest},
        {RegisteredStopCommandKindId("manyforge.stop.software_hard"),
         StopActionClass::kSoftwareHard, std::nullopt},
    }};

[[nodiscard]] inline std::span<const RegisteredStopCommandDefinitionDTO>
registered_stop_command_definitions() noexcept {
  return kRegisteredStopCommandDefinitions;
}

[[nodiscard]] inline const RegisteredStopCommandDefinitionDTO*
find_registered_stop_command_definition(
    const RegisteredStopCommandKindId& registered_command_kind_id) noexcept {
  const auto found = std::lower_bound(
      kRegisteredStopCommandDefinitions.begin(),
      kRegisteredStopCommandDefinitions.end(), registered_command_kind_id,
      [](const RegisteredStopCommandDefinitionDTO& candidate,
         const RegisteredStopCommandKindId& key) {
        return candidate.registered_command_kind_id < key;
      });
  return found == kRegisteredStopCommandDefinitions.end() ||
                 found->registered_command_kind_id != registered_command_kind_id
             ? nullptr
             : &*found;
}

inline constexpr std::string_view
    kStopImplementationQualificationArtifactRole =
        "stop_implementation_qualification_evidence";
inline constexpr std::string_view
    kStopImplementationQualificationMediaType =
        "application/vnd.manyforge.stop-implementation-qualification-evidence.v1+json";
inline constexpr std::string_view
    kControlledStopSubstitutionQualificationArtifactRole =
        "stop_controlled_substitution_qualification_evidence";
inline constexpr std::string_view
    kControlledStopSubstitutionQualificationMediaType =
        "application/vnd.manyforge.stop-controlled-substitution-qualification-evidence.v1+json";
inline constexpr std::string_view kStopInterfaceCommissioningArtifactRole =
    "stop_interface_commissioning_evidence";
inline constexpr std::string_view kStopInterfaceCommissioningMediaType =
    "application/vnd.manyforge.stop-interface-commissioning-evidence.v1+json";

struct StopImplementationQualificationDTO final {
  RegisteredStopCommandKindId registered_command_kind_id;
  ProviderContractRefDTO provider_contract_ref;
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  std::optional<EndpointRoleId> endpoint_role_id;
  StopInvocationLaneId invocation_lane_id;
  RetainedArtifactRefDTO qualification_artifact_ref;

  friend bool operator==(const StopImplementationQualificationDTO&,
                         const StopImplementationQualificationDTO&) = default;
};

struct ControlledStopSubstitutionQualificationDTO final {
  ProviderContractRefDTO provider_contract_ref;
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  MotionControlledStopMode requested_stop_mode{
      MotionControlledStopMode::kSoftOnPath};
  MotionControlledStopMode executed_stop_mode{
      MotionControlledStopMode::kSpringBackHold};
  RetainedArtifactRefDTO qualification_artifact_ref;

  friend bool operator==(
      const ControlledStopSubstitutionQualificationDTO&,
      const ControlledStopSubstitutionQualificationDTO&) = default;
};

struct StopStageParametersDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  StopEscalationStageId stop_escalation_stage_id;
  ProviderContractRefDTO provider_contract_ref;
  JsonObjectDTO default_contract_parameters;

  friend bool operator==(const StopStageParametersDTO&,
                         const StopStageParametersDTO&) = default;
};

}  // namespace manyforge::core
