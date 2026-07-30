#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

// These I3 identities deliberately live in a separate header until the
// retained-product DTO family is linked into the complete core surface.  They
// remain StrongString domains and therefore cannot be interchanged with an
// equal-spelled graph, catalog, or raw-content identity.
struct DeploymentArtifactIdTag final {};
using DeploymentArtifactId = StrongString<DeploymentArtifactIdTag>;

struct DeploymentRevisionTag final {};
using DeploymentRevision = StrongString<DeploymentRevisionTag>;

struct RetainedBundleIdTag final {};
using RetainedBundleId = StrongString<RetainedBundleIdTag>;

struct ProductIdTag final {};
using ProductId = StrongString<ProductIdTag>;

struct ProductContractIdTag final {};
using ProductContractId = StrongString<ProductContractIdTag>;

struct ProductContractRevisionTag final {};
using ProductContractRevision = StrongString<ProductContractRevisionTag>;

struct ArtifactRoleIdTag final {};
using ArtifactRoleId = StrongString<ArtifactRoleIdTag>;

inline constexpr std::string_view kRetainedBundleApiVersion = "manyforge.retained_bundle.v1";
inline constexpr std::string_view kProductContractApiVersion = "manyforge.product_contract.v1";

enum class RetainedProductKind : std::uint8_t {
  kPlanningCollisionModel = 0,
  kViewerModel,
  kControlDomainControl,
  kSymbolRoleMap,
  kRuntimeUnitConfiguration,
};

[[nodiscard]] constexpr std::string_view to_string(RetainedProductKind kind) noexcept {
  switch (kind) {
    case RetainedProductKind::kPlanningCollisionModel:
      return "planning_collision_model";
    case RetainedProductKind::kViewerModel:
      return "viewer_model";
    case RetainedProductKind::kControlDomainControl:
      return "control_domain_control";
    case RetainedProductKind::kSymbolRoleMap:
      return "symbol_role_map";
    case RetainedProductKind::kRuntimeUnitConfiguration:
      return "runtime_unit_configuration";
  }
  return {};
}

/// Exact five-field immutable artifact-reference value.
struct RetainedArtifactRefDTO final {
  ArtifactRoleId artifact_role_id;
  std::string media_type;
  std::uint64_t byte_size;
  ContentDigest content_digest;
  ContentAddressedStoreKey content_addressed_store_key;

  friend bool operator==(const RetainedArtifactRefDTO&, const RetainedArtifactRefDTO&) = default;
};

/// Exact five-field semantic retained-product value.
struct RetainedProductDTO final {
  ProductId product_id;
  RetainedProductKind product_kind;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  std::vector<RetainedArtifactRefDTO> artifact_refs;

  friend bool operator==(const RetainedProductDTO&, const RetainedProductDTO&) = default;
};

/// Path-independent node-environment identity retained in every bundle.
struct RetainedNodeEnvironmentDTO final {
  std::string catalog_id;
  std::string content_version;
  ContentDigest catalog_digest;
  ContentDigest environment_digest;

  friend bool operator==(const RetainedNodeEnvironmentDTO&,
                         const RetainedNodeEnvironmentDTO&) = default;
};

/// Exact provenance projection for one semantic product.
struct RetainedOutputRecordDTO final {
  ProductId product_id;
  RetainedProductKind product_kind;
  ContentDigest content_digest;

  friend bool operator==(const RetainedOutputRecordDTO&, const RetainedOutputRecordDTO&) = default;
};

}  // namespace manyforge::core
