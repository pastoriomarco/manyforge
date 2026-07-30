#pragma once

#include <cstddef>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kProviderManifestApiVersion =
    "manyforge.provider_manifest.v1";
inline constexpr std::size_t kMaxProviderManifestCapabilities = 256U;
inline constexpr std::size_t kMaxProviderManifestContracts = 256U;
inline constexpr std::size_t kMaxProviderManifestEndpointRoles = 256U;

/// One exact capability implemented by a reusable provider-manifest revision.
///
/// Required endpoint roles are manifest-scoped logical roles. An empty set
/// denotes a focused direct/in-process provider port; it never authorizes
/// endpoint discovery or token-based dispatch.
struct ProviderCapabilityOfferDTO {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  std::vector<EndpointRoleId> required_endpoint_role_ids;

  friend bool operator==(const ProviderCapabilityOfferDTO&,
                         const ProviderCapabilityOfferDTO&) = default;
};

/// The one provider-defined Stop command supported by a contract role.
///
/// Endpoint roles are legal alternatives, not an all-of set. The retained
/// compiler product must resolve exactly one declared role when the command is
/// endpoint-based. An empty set denotes the selected provider instance's
/// focused direct command port.
struct ProviderStopCommandSupportDTO {
  StopContainmentCommandKind command_kind;
  std::optional<RegisteredStopCommandKindId> registered_command_kind_id;
  std::vector<EndpointRoleId> command_endpoint_role_ids;

  friend bool operator==(const ProviderStopCommandSupportDTO&,
                         const ProviderStopCommandSupportDTO&) = default;
};

/// The one provider-defined Stop evidence kind supported by a contract role.
///
/// Endpoint roles are legal alternatives. Endpoint-based evidence must select
/// one exact declared role; an empty set is legal only when the qualified Stop
/// source matrix permits an intrinsic endpointless observation.
struct ProviderStopEvidenceSupportDTO {
  StopEvidenceRequirementKind evidence_kind;
  std::optional<RegisteredStopEvidenceKindId> registered_evidence_kind_id;
  std::vector<EndpointRoleId> evidence_endpoint_role_ids;

  friend bool operator==(const ProviderStopEvidenceSupportDTO&,
                         const ProviderStopEvidenceSupportDTO&) = default;
};

/// One manifest-scoped provider-contract role.
///
/// V1 permits at most one provider-defined Stop command kind and one evidence
/// kind per role. A provider that supports another kind declares another role,
/// even when both roles select the same exact value schema.
struct ProviderContractDeclarationDTO {
  ProviderContractRefDTO provider_contract_ref;
  std::optional<ProviderStopCommandSupportDTO> stop_command_support;
  std::optional<ProviderStopEvidenceSupportDTO> stop_evidence_support;

  friend bool operator==(const ProviderContractDeclarationDTO&,
                         const ProviderContractDeclarationDTO&) = default;
};

/// Exact normalized payload for `manyforge.provider_manifest.v1`.
///
/// The API version is fixed by this type and `kProviderManifestApiVersion`.
/// This reusable catalog declaration does not identify a deployed provider
/// instance or carry scope targets, runtime dependencies, process/container
/// choices, concrete endpoints, concurrency/scaling settings, or qualification
/// evidence. Bindings, compiler products, and runtime observations own those
/// facts.
struct ProviderManifestDTO {
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  std::vector<ProviderCapabilityOfferDTO> offered_capabilities;
  std::vector<ProviderContractDeclarationDTO> provider_contracts;

  friend bool operator==(const ProviderManifestDTO&,
                         const ProviderManifestDTO&) = default;
};

}  // namespace manyforge::core
