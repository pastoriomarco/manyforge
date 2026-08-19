#pragma once

#include <cstddef>
#include <manyforge_core/types/strong_id.hpp>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kRuntimeContractOfferApiVersion =
    "manyforge.runtime_contract_offer.v1";
inline constexpr std::string_view kRuntimeContractRequirementApiVersion =
    "manyforge.runtime_contract_requirement.v1";
inline constexpr std::string_view kRuntimeContractManifestSetApiVersion =
    "manyforge.runtime_contract_manifest_set.v1";

inline constexpr std::string_view kRuntimeContractOfferIdentityDomain =
    "runtime-contract-offer";
inline constexpr std::string_view kRuntimeContractRequirementIdentityDomain =
    "runtime-contract-requirement";
inline constexpr std::string_view kRuntimeContractManifestSetIdentityDomain =
    "runtime-contract-manifest-set";

inline constexpr std::size_t kMaxRuntimeContractApiContractIds = 256U;
inline constexpr std::size_t kMaxRuntimeContractCapabilityRefs = 256U;
inline constexpr std::size_t kMaxRuntimeContractManifestOffers = 256U;

// Descriptive aliases retained for callers that name the rows rather than
// their serialized field names.
inline constexpr std::size_t kMaxRuntimeContractApiContracts =
    kMaxRuntimeContractApiContractIds;
inline constexpr std::size_t kMaxRuntimeContractCapabilities =
    kMaxRuntimeContractCapabilityRefs;
inline constexpr std::size_t kMaxRuntimeContractOffers =
    kMaxRuntimeContractManifestOffers;

/// One exact capability contract claim from the spec-240 built-in registry.
struct RuntimeCapabilityContractRefDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;

  friend bool operator==(const RuntimeCapabilityContractRefDTO&,
                         const RuntimeCapabilityContractRefDTO&) = default;
};

/// Closed runtime implementation offer. The API version is fixed by the type
/// and is therefore not a mutable DTO member.
struct RuntimeContractOfferDTO final {
  RuntimeContractOfferDigest offer_digest;
  RuntimeImplementationFamilyId runtime_implementation_id;
  std::vector<ApiContractId> provided_api_contract_ids;
  std::vector<RuntimeCapabilityContractRefDTO> provided_capabilities;

  friend bool operator==(const RuntimeContractOfferDTO&,
                         const RuntimeContractOfferDTO&) = default;
};

/// Closed per-runtime requirement. The API version is fixed by the type.
struct RuntimeContractRequirementDTO final {
  RuntimeContractRequirementDigest requirement_digest;
  RuntimeImplementationFamilyId runtime_implementation_id;
  std::vector<ApiContractId> required_api_contract_ids;
  std::vector<RuntimeCapabilityContractRefDTO> required_capabilities;

  friend bool operator==(const RuntimeContractRequirementDTO&,
                         const RuntimeContractRequirementDTO&) = default;
};

/// Complete package offer snapshot. At least one offer is required.
struct RuntimeContractManifestSetDTO final {
  RuntimeContractManifestSetDigest manifest_set_digest;
  std::vector<RuntimeContractOfferDTO> offers;

  friend bool operator==(const RuntimeContractManifestSetDTO&,
                         const RuntimeContractManifestSetDTO&) = default;
};

}  // namespace manyforge::core
