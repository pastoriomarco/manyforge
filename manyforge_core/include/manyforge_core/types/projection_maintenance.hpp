#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>

namespace manyforge::core {

/// Exact Manager operation accepted by Scene for one fresh maintenance
/// initialization. Scene, not Manager, selects the immutable logical snapshot
/// and derives the target projection revision from it.
struct ProjectionMaintenanceInitializationRequestDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO expected_active_projection_revision;
  RetainedBundleDTO candidate_retained_bundle;
  std::vector<ProjectionNativeRuntimeBindingDTO> native_runtime_bindings;

  [[nodiscard]] bool valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() ||
        !expected_active_projection_revision.valid() ||
        candidate_retained_bundle.retained_bundle_id.empty() ||
        candidate_retained_bundle.cell_graph.graph_revision.empty() ||
        candidate_retained_bundle.cell_graph.cell_id.empty() ||
        !std::is_sorted(native_runtime_bindings.begin(),
                        native_runtime_bindings.end(),
                        CanonicalProjectionNativeBindingLess)) {
      return false;
    }
    for (std::size_t index = 0U; index < native_runtime_bindings.size();
         ++index) {
      if (!native_runtime_bindings[index].valid() ||
          (index != 0U &&
           !CanonicalProjectionNativeBindingLess(
               native_runtime_bindings[index - 1U],
               native_runtime_bindings[index]))) {
        return false;
      }
    }
    return true;
  }

  friend bool operator==(
      const ProjectionMaintenanceInitializationRequestDTO &,
      const ProjectionMaintenanceInitializationRequestDTO &) = default;
};

enum class ProjectionMaintenanceInitializationState {
  kInitializing,
  kInitialized,
  kCompleted,
  kFailed,
  kInterrupted,
};

/// Scene-owned status for one operation. Missing terminal evidence after a
/// Scene-owner restart is reported as interrupted with unknown effects; Read
/// never resumes native work.
struct ProjectionMaintenanceInitializationStatusDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionMaintenanceInitializationState state;
  ProjectionRevisionDTO predecessor_projection_revision;
  ProjectionRevisionDTO target_projection_revision;
  LogicalSceneSnapshotIdentityDTO authoritative_snapshot_identity;
  std::vector<ProjectionNativeRuntimeBindingDTO> native_runtime_bindings;
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<ErrorDTO> first_error;
  bool effects_unknown{};

  [[nodiscard]] bool valid() const noexcept {
    if (apply_id.empty() || !apply_generation.nonzero() ||
        !predecessor_projection_revision.valid() ||
        !target_projection_revision.valid() ||
        !authoritative_snapshot_identity.valid() ||
        target_projection_revision.scene_revision !=
            authoritative_snapshot_identity.revision ||
        !std::is_sorted(native_runtime_bindings.begin(),
                        native_runtime_bindings.end(),
                        CanonicalProjectionNativeBindingLess)) {
      return false;
    }
    for (std::size_t index = 0U; index < native_runtime_bindings.size();
         ++index) {
      if (!native_runtime_bindings[index].valid() ||
          (index != 0U &&
           !CanonicalProjectionNativeBindingLess(
               native_runtime_bindings[index - 1U],
               native_runtime_bindings[index]))) {
        return false;
      }
    }
    switch (state) {
    case ProjectionMaintenanceInitializationState::kInitializing:
    case ProjectionMaintenanceInitializationState::kInitialized:
      return !publication_receipt.has_value() && !first_error.has_value() &&
             !effects_unknown;
    case ProjectionMaintenanceInitializationState::kCompleted:
      return publication_receipt.has_value() && !first_error.has_value() &&
             !effects_unknown &&
             publication_receipt->matches_named_apply(
                 apply_id, apply_generation,
                 target_projection_revision);
    case ProjectionMaintenanceInitializationState::kFailed:
      return !publication_receipt.has_value() && first_error.has_value() &&
             !effects_unknown;
    case ProjectionMaintenanceInitializationState::kInterrupted:
      return !publication_receipt.has_value() && first_error.has_value() &&
             effects_unknown;
    }
    return false;
  }

  [[nodiscard]] bool ready_for_publication() const noexcept {
    return valid() &&
           state == ProjectionMaintenanceInitializationState::kInitialized;
  }

  friend bool operator==(
      const ProjectionMaintenanceInitializationStatusDTO &,
      const ProjectionMaintenanceInitializationStatusDTO &) = default;
};

struct ProjectionMaintenanceInitializationQueryDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero();
  }

  friend bool operator==(
      const ProjectionMaintenanceInitializationQueryDTO &,
      const ProjectionMaintenanceInitializationQueryDTO &) = default;
};

struct ProjectionMaintenanceCompletionDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO target_projection_revision;
  LogicalSceneSnapshotIdentityDTO authoritative_snapshot_identity;
  ActiveBundlePublicationReceiptDTO publication_receipt;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           target_projection_revision.valid() &&
           authoritative_snapshot_identity.valid() &&
           target_projection_revision.scene_revision ==
               authoritative_snapshot_identity.revision &&
           publication_receipt.matches_named_apply(
               apply_id, apply_generation,
               target_projection_revision);
  }

  friend bool operator==(const ProjectionMaintenanceCompletionDTO &,
                         const ProjectionMaintenanceCompletionDTO &) = default;
};

} // namespace manyforge::core
