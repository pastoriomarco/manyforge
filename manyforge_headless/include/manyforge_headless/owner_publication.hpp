#pragma once

#include <string_view>

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_scene/canonical_empty_scene_owner.hpp>

#include <manyforge_headless/scene_runtime_state_owner.hpp>
#include <manyforge_headless/source_contract.hpp>

namespace manyforge::headless {

enum class ManagerApplyMutationAvailability {
  kAvailable,
  kKernelOwnerUnavailable,
  kFullStackRestartRequired,
};

[[nodiscard]] std::string_view
ToString(ManagerApplyMutationAvailability availability) noexcept;

/// Builds one immutable Manager-owned source value from an independently
/// verified active bundle, the Manager's own runtime observation result, and
/// one transactionally consistent durable Apply-journal inventory.
///
/// This is a source publication, not the public headless aggregate. It does
/// not add receiver freshness, alarm, scene, Kernel, lifecycle, or robot facts.
[[nodiscard]] manyforge::content_identity::CanonicalValue
BuildManagerSourcePayload(
    const manyforge::deployment::RetainedBundle &active_bundle,
    const manyforge::component_manager::ManagerActiveRuntimeConvergenceResult
        &runtime,
    const manyforge::component_manager::SqliteManagerApplyJournal
        &apply_journal,
    ManagerApplyMutationAvailability apply_availability);

/// Builds the separate logical-scene owner's immutable source value. The
/// source remains absent until bootstrap Finalize has established a real
/// active snapshot; an Apply transition continues to publish its frozen
/// predecessor until the scene owner selects the candidate at Finalize.
[[nodiscard]] manyforge::content_identity::CanonicalValue
BuildLogicalSceneSourcePayload(
    const manyforge::scene::CanonicalEmptySceneAccessStatus &access,
    const std::optional<manyforge::core::LogicalSceneSnapshotRoot> &snapshot);

/// Builds the distinct scene/runtime owner's initialization and pose-source
/// lifetime publication. It does not duplicate logical-scene geometry or
/// perform reset, seed, replay, or pose-apply work.
[[nodiscard]] manyforge::content_identity::CanonicalValue
BuildSceneRuntimeSourcePayload(std::string_view cell_id,
                               const SceneRuntimeStateSnapshot &snapshot);

} // namespace manyforge::headless
