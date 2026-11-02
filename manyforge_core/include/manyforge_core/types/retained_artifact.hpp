#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <string>

namespace manyforge::core {

struct ProductIdTag final {};
using ProductId = StrongString<ProductIdTag>;

struct ProductContractIdTag final {};
using ProductContractId = StrongString<ProductContractIdTag>;

struct ProductContractRevisionTag final {};
using ProductContractRevision = StrongString<ProductContractRevisionTag>;

struct ArtifactRoleIdTag final {};
using ArtifactRoleId = StrongString<ArtifactRoleIdTag>;

/// Exact five-field immutable reference for retained content-addressed
/// artifacts.
struct RetainedArtifactRefDTO final {
  ArtifactRoleId artifact_role_id;
  std::string media_type;
  std::uint64_t byte_size;
  ContentDigest content_digest;
  ContentAddressedStoreKey content_addressed_store_key;

  friend bool operator==(const RetainedArtifactRefDTO&,
                         const RetainedArtifactRefDTO&) = default;
};

}  // namespace manyforge::core
