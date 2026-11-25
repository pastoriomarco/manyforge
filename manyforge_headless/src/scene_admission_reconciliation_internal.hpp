#pragma once

#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/scene_transaction.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_scene/logical_scene_owner.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless::detail {

// A normal pre-acceptance refusal has no retained transaction and leaves the
// Scene's existing gates open. It cannot create a new admission settlement.
// A not-attempted projection alone is insufficient: acquisition may have lost
// its reply after the Kernel retained a claim. Unavailable reconciliation of
// older history is also unresolved even without an outcome for this request.
template <class SceneOwner>
core::SceneCommitResultDTO CommitSceneWithAdmissionObservation(
    SceneOwner &owner, const core::SceneTransactionRequestDTO &request,
    std::atomic<bool> &admission_reconciled) {
  try {
    auto result = owner.Commit(request);
    if (!result.success) {
      bool needs_reconciliation = true;
      if (result.projection_status ==
              core::SceneProjectionCommitStatusDTO::kNotAttempted &&
          result.error.has_value() &&
          result.error->code != "kernel_owner_unavailable" &&
          // The ID's retained outcome belongs to another request: the lookup
          // would refuse it, and that outcome keeps reconciliation required.
          result.error->code != "request_id_conflict" &&
          !owner.ReadCommitResult(request).has_value()) {
        const auto access = owner.ReadAccessStatus();
        needs_reconciliation =
            !access.scene_writes_open || !access.projection_leases_open;
      }
      if (needs_reconciliation)
        admission_reconciled.store(false, std::memory_order_release);
    }
    return result;
  } catch (...) {
    // Includes unknown acquire/release outcomes and an unreadable journal.
    admission_reconciled.store(false, std::memory_order_release);
    throw;
  }
}

// The `scene_owner_read` view: the owner's current projection revision and
// its logical snapshot, both from one owner read (`ReadPublication`, under the
// owner's lock): the Apply lane changes the Scene on its own thread, so two
// separate reads could name two revisions. A closed projection gate (no
// active projection yet, maintenance initialising, or desynchronized after an
// uncertain commit) is refused with the owner's own code for that state, the
// one its aggregate health names; any other failure propagates unchanged.
template <class SceneOwner>
std::pair<core::ProjectionRevisionDTO,
          std::shared_ptr<const core::LogicalSceneSnapshotDTO>>
ReadSceneOwnerView(const SceneOwner &owner) {
  const auto read = owner.ReadPublication();
  if (!read.access.projection_leases_open)
    throw core::TypedError(
        "active_native_projection_initialization_pending",
        "logical-scene projection is closed while its initialization or a "
        "scene transaction is incomplete");
  if (!read.access.active_projection.has_value() || !read.snapshot.has_value())
    throw scene::LogicalSceneOwnerConflictError(
        "live logical-scene snapshot is unavailable");
  return {*read.access.active_projection, read.snapshot->shared()};
}

// A `scene_owner_read` naming `scene_resource_ids` (sorted, unique): the
// snapshot with only the named objects, each carried one with its
// attachment; an id naming no object adds nothing. Every other member is the
// snapshot's own, so a node reading one object costs that object, not the
// Scene.
inline core::LogicalSceneSnapshotDTO SelectSceneResources(
    const core::LogicalSceneSnapshotDTO &snapshot,
    const std::vector<core::SceneResourceId> &scene_resource_ids) {
  core::LogicalSceneSnapshotDTO result{
      snapshot.revision,       snapshot.cell_id,
      snapshot.transaction_sequence, snapshot.topology_epoch,
      snapshot.policy_epoch,   snapshot.configured_collision_policy,
      {},                      {}};
  for (const auto &id : scene_resource_ids) {
    const auto object = std::lower_bound(
        snapshot.objects.begin(), snapshot.objects.end(), id,
        [](const core::SceneObjectSnapshotDTO &row,
           const core::SceneResourceId &key) {
          return row.scene_resource_id < key;
        });
    if (object == snapshot.objects.end() || object->scene_resource_id != id)
      continue;
    result.objects.push_back(*object);
    const auto *carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&object->placement);
    if (carried == nullptr) continue;
    const auto attachment = std::lower_bound(
        snapshot.carried_object_attachments.begin(),
        snapshot.carried_object_attachments.end(),
        carried->carried_object_attachment_id,
        [](const core::CarriedObjectAttachmentDTO &row,
           const core::CarriedObjectAttachmentId &key) {
          return row.carried_object_attachment_id < key;
        });
    if (attachment != snapshot.carried_object_attachments.end() &&
        attachment->carried_object_attachment_id ==
            carried->carried_object_attachment_id)
      result.carried_object_attachments.push_back(*attachment);
  }
  std::sort(result.carried_object_attachments.begin(),
            result.carried_object_attachments.end(),
            [](const core::CarriedObjectAttachmentDTO &left,
               const core::CarriedObjectAttachmentDTO &right) {
              return left.carried_object_attachment_id <
                     right.carried_object_attachment_id;
            });
  return result;
}

} // namespace manyforge::headless::detail
