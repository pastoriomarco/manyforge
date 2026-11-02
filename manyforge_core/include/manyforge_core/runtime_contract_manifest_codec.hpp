#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/runtime_contract_manifest.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

[[nodiscard]] Result<RuntimeContractOfferDTO> MakeRuntimeContractOffer(
    RuntimeImplementationFamilyId runtime_implementation_id,
    std::vector<ApiContractId> provided_api_contract_ids,
    std::vector<RuntimeCapabilityContractRefDTO> provided_capabilities);

[[nodiscard]] Result<void> VerifyRuntimeContractOffer(
    const RuntimeContractOfferDTO& offer);
[[nodiscard]] Result<std::string> EncodeRuntimeContractOffer(
    const RuntimeContractOfferDTO& offer);
[[nodiscard]] Result<RuntimeContractOfferDTO> DecodeRuntimeContractOffer(
    std::string_view canonical_bytes);

[[nodiscard]] Result<RuntimeContractRequirementDTO>
MakeRuntimeContractRequirement(
    RuntimeImplementationFamilyId runtime_implementation_id,
    std::vector<ApiContractId> required_api_contract_ids,
    std::vector<RuntimeCapabilityContractRefDTO> required_capabilities);

[[nodiscard]] Result<void> VerifyRuntimeContractRequirement(
    const RuntimeContractRequirementDTO& requirement);
[[nodiscard]] Result<std::string> EncodeRuntimeContractRequirement(
    const RuntimeContractRequirementDTO& requirement);
[[nodiscard]] Result<RuntimeContractRequirementDTO>
DecodeRuntimeContractRequirement(std::string_view canonical_bytes);

[[nodiscard]] Result<RuntimeContractManifestSetDTO>
MakeRuntimeContractManifestSet(std::vector<RuntimeContractOfferDTO> offers);
[[nodiscard]] Result<void> VerifyRuntimeContractManifestSet(
    const RuntimeContractManifestSetDTO& manifest_set);
[[nodiscard]] Result<std::string> EncodeRuntimeContractManifestSet(
    const RuntimeContractManifestSetDTO& manifest_set);
[[nodiscard]] Result<RuntimeContractManifestSetDTO>
DecodeRuntimeContractManifestSet(std::string_view canonical_bytes);

/// Check immutable API/capability-ID compatibility only after both records have
/// crossed their respective verification boundaries. Implementation-family and
/// capability-definition revisions are diagnostic/catalog provenance, not
/// compatibility equality. This operation has no provider, scope, target, or
/// runtime authority.
[[nodiscard]] Result<void> CheckRuntimeContractCompatibility(
    const RuntimeContractOfferDTO& offer,
    const RuntimeContractRequirementDTO& requirement);

}  // namespace manyforge::core
