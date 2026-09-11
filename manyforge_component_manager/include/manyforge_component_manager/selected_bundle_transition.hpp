#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/projection_apply.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::size_t kMaximumObsoleteManagedCommandSurfaces = 64U;

/// Immutable Manager-private materialization of the selected retained bundle.
///
/// The canonical content digest and byte count are obtained from one
/// independently verified sealed bundle. This record is private selection
/// state; it is deliberately distinct from the public active pointer.
struct ManagerApplyPrivateMaterializationDTO final {
  manyforge::core::ContentDigest scene_activated_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionStageGeneration projection_stage_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest retained_bundle_canonical_content_digest;
  std::uint64_t retained_bundle_canonical_byte_size;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPrivateMaterializationDTO&,
                         const ManagerApplyPrivateMaterializationDTO&) = default;
};

enum class ObsoleteManagedCommandSurfaceDisposition {
  kRetired,
  kAlreadyAbsent,
  kNonselectable,
};

/// Exact terminal observation for one obsolete ManyForge-managed command
/// surface. The first admitted runtime-only transition has no such rows.
/// For native kNonselectable replacement, runtime_epoch/source_report_sequence
/// identify the exact successor proving the old product no longer selectable.
struct ObsoleteManagedCommandSurfaceEvidenceDTO final {
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::ContentDigest product_identity;
  manyforge::core::ContentDigest configuration_identity;
  std::uint64_t source_report_sequence;
  ObsoleteManagedCommandSurfaceDisposition disposition;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ObsoleteManagedCommandSurfaceEvidenceDTO&,
                         const ObsoleteManagedCommandSurfaceEvidenceDTO&) = default;
};

/// Complete proof that every obsolete ManyForge-managed command surface is
/// retired or otherwise nonselectable before public publication.
struct ManagerApplyObsoleteCommandSurfacesRetiredDTO final {
  manyforge::core::ContentDigest private_materialization_content_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO> command_surfaces;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyObsoleteCommandSurfacesRetiredDTO&,
                         const ManagerApplyObsoleteCommandSurfacesRetiredDTO&) = default;
};

/// Exact durable public-pointer receipt bound to the preceding private and
/// retirement evidence.
struct ManagerApplyPublicationReceiptDTO final {
  manyforge::core::ContentDigest obsolete_managed_command_surfaces_retired_evidence_digest;
  manyforge::core::ActiveBundlePublicationReceiptDTO publication_receipt;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPublicationReceiptDTO&,
                         const ManagerApplyPublicationReceiptDTO&) = default;
};

/// Complete successful scene-owner Finalize evidence bound to one durable
/// publication receipt.
struct ManagerApplySceneFinalizedDTO final {
  manyforge::core::ContentDigest publication_receipt_digest;
  manyforge::core::ProjectionApplyFinalizeResultDTO finalized_result;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool SameManagerApplySceneFinalized(
    const ManagerApplySceneFinalizedDTO& left, const ManagerApplySceneFinalizedDTO& right) noexcept;

[[nodiscard]] const char* ToString(ObsoleteManagedCommandSurfaceDisposition value) noexcept;

}  // namespace manyforge::component_manager
