#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::component_manager {

/// Immutable Manager-private materialization of the selected retained bundle.
///
/// The canonical content digest and byte count are obtained from one
/// independently verified sealed bundle. This record is private selection
/// state; it is deliberately distinct from the public active pointer.
struct ManagerApplyPrivateMaterializationDTO final {
  manyforge::core::ContentDigest authoritative_scene_snapshot_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest retained_bundle_canonical_content_digest;
  std::uint64_t retained_bundle_canonical_byte_size;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPrivateMaterializationDTO&,
                         const ManagerApplyPrivateMaterializationDTO&) = default;
};

/// Exact durable public-pointer receipt bound to the private materialization.
struct ManagerApplyPublicationReceiptDTO final {
  manyforge::core::ContentDigest private_materialization_content_digest;
  manyforge::core::ActiveBundlePublicationReceiptDTO publication_receipt;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPublicationReceiptDTO&,
                         const ManagerApplyPublicationReceiptDTO&) = default;
};

}  // namespace manyforge::component_manager
