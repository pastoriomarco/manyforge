#pragma once

#include <string_view>
#include <vector>

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
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

/// True when the runtime result's active publication accounts for every
/// committed Apply in the inventory: no record's publication receipt names a
/// publication newer than the active one. A false result means the runtime
/// result predates a committed Apply (it was observed before that Apply
/// published), so pairing them would show a committed Apply whose
/// publication is not the active one.
[[nodiscard]] bool ManagerRuntimeAccountsForCommittedApplies(
    const manyforge::component_manager::ManagerActiveRuntimeConvergenceResult
        &runtime,
    const std::vector<manyforge::component_manager::PersistedManagerApplyStatus>
        &apply_inventory) noexcept;

/// Builds one immutable Manager-owned source value from an independently
/// verified active bundle, the Manager's own runtime observation result, and
/// one transactionally consistent durable Apply-journal inventory.
///
/// The active publication and the Apply history are one generation: the
/// builder refuses a runtime result that predates a committed Apply in the
/// inventory (see ManagerRuntimeAccountsForCommittedApplies), so no published
/// value ever shows a committed Apply its active publication does not hold.
///
/// `active.canonical_empty` compares the verified active cell/bundle/graph
/// exactly to the sealed construction-issued canonical-empty admission.
/// This is a source publication, not the public headless aggregate. It does
/// not add receiver freshness, alarm, scene, Kernel, lifecycle, or robot facts.
[[nodiscard]] manyforge::content_identity::CanonicalValue
BuildManagerSourcePayload(
    const manyforge::deployment::RetainedBundle &active_bundle,
    const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
        &canonical_empty_admission,
    const manyforge::component_manager::ManagerActiveRuntimeConvergenceResult
        &runtime,
    const manyforge::component_manager::SqliteManagerApplyJournal
        &apply_journal,
    ManagerApplyMutationAvailability apply_availability);

/// As above, from an inventory the caller already read once from
/// `apply_journal` (so a caller's own check and the payload see one read).
[[nodiscard]] manyforge::content_identity::CanonicalValue
BuildManagerSourcePayload(
    const manyforge::deployment::RetainedBundle &active_bundle,
    const manyforge::deployment::CanonicalEmptyRetainedBundleAdmission
        &canonical_empty_admission,
    const manyforge::component_manager::ManagerActiveRuntimeConvergenceResult
        &runtime,
    const manyforge::component_manager::SqliteManagerApplyJournal
        &apply_journal,
    const std::vector<manyforge::component_manager::PersistedManagerApplyStatus>
        &apply_inventory,
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
