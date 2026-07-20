#include <manyforge_core/scene_authority.hpp>

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

ErrorDTO scene_error(ErrorCode code, std::string message,
                     std::map<std::string, std::string> details = {}) {
  return ErrorDTO{to_string(code), std::move(message), std::move(details)};
}

bool same_revision(const SceneRevisionDTO& left,
                   const SceneRevisionDTO& right) {
  return left.revision_id == right.revision_id &&
         left.content_hash == right.content_hash;
}

bool same_support_contact_policy(const SupportContactPolicyEntry& left,
                                 const SupportContactPolicyEntry& right) {
  return left.pair == right.pair && left.mode == right.mode &&
         left.release_evidence_mode == right.release_evidence_mode &&
         left.enforce_max_start_penetration ==
             right.enforce_max_start_penetration &&
         left.max_start_penetration_m == right.max_start_penetration_m &&
         left.release_tolerance_m == right.release_tolerance_m &&
         left.min_release_progress_m == right.min_release_progress_m &&
         left.max_contact_penetration_m ==
             right.max_contact_penetration_m &&
         left.require_separation_by_end == right.require_separation_by_end &&
         left.require_strictly_decreasing_penetration ==
             right.require_strictly_decreasing_penetration &&
         left.allow_recontact == right.allow_recontact;
}

bool same_contact_grasp_policy(const ContactGraspPolicyEntry& left,
                               const ContactGraspPolicyEntry& right) {
  return left.policy_id == right.policy_id &&
         left.selected_object_id == right.selected_object_id &&
         left.allowed_robot_links == right.allowed_robot_links &&
         left.max_penetration_m == right.max_penetration_m &&
         left.terminal_window_start_fraction ==
             right.terminal_window_start_fraction;
}

void canonicalize_mode_scoped_contact_fields(
    SupportContactPolicyEntry& contact) {
  if (contact.mode == CollisionContactPolicyMode::kContactRelease) {
    contact.max_contact_penetration_m = 0.0;
    if (contact.release_evidence_mode ==
        ContactReleaseEvidenceMode::kBinaryContact) {
      contact.enforce_max_start_penetration = false;
      contact.max_start_penetration_m = 0.0;
      contact.release_tolerance_m = 0.0;
      contact.min_release_progress_m = 0.0;
      contact.require_strictly_decreasing_penetration = false;
    }
    return;
  }

  contact.release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
  contact.enforce_max_start_penetration = false;
  contact.max_start_penetration_m = 0.0;
  contact.release_tolerance_m = 0.0;
  contact.min_release_progress_m = 0.0;
  contact.require_separation_by_end = false;
  contact.require_strictly_decreasing_penetration = false;
  contact.allow_recontact = false;
  if (contact.mode == CollisionContactPolicyMode::kContactProcess) {
    contact.max_contact_penetration_m = 0.0;
  }
}

void canonicalize_mode_scoped_contact_fields(
    CollisionPolicySnapshot& policy) {
  for (auto& contact : policy.support_contacts) {
    canonicalize_mode_scoped_contact_fields(contact);
  }
}

bool same_configured_collision_policy(const CollisionPolicySnapshot& left,
                                      const CollisionPolicySnapshot& right) {
  return left.policy_id == right.policy_id &&
         left.revision.revision_id == right.revision.revision_id &&
         left.revision.content_hash == right.revision.content_hash &&
         left.allowed_static_static_pairs ==
             right.allowed_static_static_pairs &&
         left.allowed_touch_link_pairs == right.allowed_touch_link_pairs &&
         left.support_contacts.size() == right.support_contacts.size() &&
         std::equal(left.support_contacts.begin(), left.support_contacts.end(),
                    right.support_contacts.begin(),
                    same_support_contact_policy) &&
         left.contact_grasps.size() == right.contact_grasps.size() &&
         std::equal(left.contact_grasps.begin(), left.contact_grasps.end(),
                    right.contact_grasps.begin(), same_contact_grasp_policy) &&
         left.require_full_diagnostics == right.require_full_diagnostics;
}

}  // namespace

SceneAuthority::SceneAuthority(
    IPlannerSceneProjection& projection,
    ICollisionWorldSnapshotPublisher& projection_publisher,
    SceneAuthorityConfig config)
    : projection_(projection),
      projection_publisher_(projection_publisher),
      config_(std::move(config)) {
  if (config_.world_frame.empty()) {
    throw std::invalid_argument("scene authority world frame must not be empty");
  }
  if (config_.retained_lease_capacity == 0) {
    throw std::invalid_argument(
        "scene authority retained lease capacity must be positive");
  }

  const auto revision =
      projection_publisher_.published_collision_world_revision();
  ErrorDTO error;
  if (revision.revision_id == 0 || revision.content_hash.empty() ||
      !refresh_committed_publication(
          revision, projection_.configured_collision_policy(),
          projection_.robot_placements(),
          projection_.scene_objects(), 0, 1, std::nullopt, std::nullopt,
          error)) {
    throw std::runtime_error(
        error.message.empty()
            ? "scene projection did not publish a valid initial revision"
            : error.message);
  }
}

SceneUpdateStatus SceneAuthority::classify_update(
    const SceneEditResultDTO& edit) {
  if (edit.update_status == "freshness_only" ||
      (!edit.collision_world_revision_changed &&
       edit.collision_world_freshness_changed)) {
    return SceneUpdateStatus::kFreshnessOnly;
  }
  if (edit.update_status == "no_op" ||
      (!edit.collision_world_revision_changed &&
       !edit.collision_world_freshness_changed)) {
    return SceneUpdateStatus::kNoOp;
  }
  return SceneUpdateStatus::kGeometryChanged;
}

SceneCommitResultDTO SceneAuthority::commit(SceneTransactionDTO transaction) {
  std::lock_guard<std::mutex> lock(transaction_mutex_);
  return commit_locked(std::move(transaction));
}

SceneCommitResultDTO SceneAuthority::commit_locked(
    SceneTransactionDTO transaction) {
  SceneCommitResultDTO result;
  result.transaction_sequence = ++next_transaction_sequence_;
  transaction.transaction_sequence = result.transaction_sequence;
  if (transaction.transaction_id.empty()) {
    transaction.transaction_id =
        "scene_tx_" + std::to_string(++next_transaction_id_);
  }
  if (transaction.authority_source.empty()) {
    transaction.authority_source = "scene_authority";
  }

  const auto current = publication_.load(std::memory_order_acquire);
  if (!current || !current->logical) {
    result.error = scene_error(
        ErrorCode::kInternalError, "Logical scene publication is unavailable",
        {{"reason", "scene_authority_publication_unavailable"}});
    return result;
  }
  result.committed_revision = current->logical->revision;

  if (desynchronized_.load(std::memory_order_acquire) ||
      !projection_.projection_health().ready()) {
    result.error = scene_error(
        ErrorCode::kInvariantViolation,
        "Planner scene projection is desynchronized",
        {{"reason", "scene_projection_desynchronized"}});
    return result;
  }
  if (!same_revision(transaction.expected_revision,
                     current->logical->revision)) {
    result.error = scene_error(
        ErrorCode::kSceneChangedDuringValidation,
        "Scene transaction expected revision does not match the committed scene",
        {{"reason", "scene_transaction_revision_mismatch"},
         {"expected_revision_id",
          std::to_string(transaction.expected_revision.revision_id)},
         {"current_revision_id",
          std::to_string(current->logical->revision.revision_id)}});
    return result;
  }

  if (const auto* update =
          std::get_if<UpdateObjectPoseRequestDTO>(&transaction.operation);
      update != nullptr && update->expected_attached.has_value()) {
    const auto object = std::find_if(
        current->logical->objects.begin(), current->logical->objects.end(),
        [&](const SceneObjectSnapshotDTO& candidate) {
          return candidate.object_id == update->object_id;
        });
    if (object != current->logical->objects.end() &&
        object->attached != *update->expected_attached) {
      result.error = scene_error(
          ErrorCode::kSceneObjectStateConflict,
          "Scene object attachment state changed before pose update admission",
          {{"reason", "scene_object_attachment_precondition_failed"},
           {"object_id", update->object_id},
           {"expected_attached",
            *update->expected_attached ? "true" : "false"},
           {"current_attached", object->attached ? "true" : "false"},
           {"current_frame_id", object->frame_id}});
      return result;
    }
  }

  SceneRevisionDTO proposed;
  proposed.revision_id = current->logical->revision.revision_id + 1;
  auto projection_result =
      projection_.apply_transaction(transaction, proposed);
  result.projection_revision = projection_result.acknowledged_revision;
  if (!projection_result.edit.success) {
    if (!projection_result.rollback_proven) {
      desynchronized_.store(true, std::memory_order_release);
    }
    result.error = projection_result.edit.error.value_or(scene_error(
        ErrorCode::kInternalError, "Planner scene projection commit failed",
        {{"reason", projection_result.rollback_proven
                        ? "scene_projection_commit_failed"
                        : "scene_projection_commit_uncertain"}}));
    return result;
  }

  result.update_status = classify_update(projection_result.edit);
  const std::uint64_t admission_epoch =
      current->logical->admission_epoch +
      (result.update_status != SceneUpdateStatus::kNoOp &&
               transaction.recovery_admission_impact ==
                   SceneRecoveryAdmissionImpact::kInvalidate
           ? 1
           : 0);
  const std::uint64_t expected_revision_id =
      result.update_status == SceneUpdateStatus::kGeometryChanged
          ? proposed.revision_id
          : current->logical->revision.revision_id;
  if (projection_result.acknowledged_revision.revision_id !=
          expected_revision_id ||
      projection_result.acknowledged_revision.content_hash.empty()) {
    desynchronized_.store(true, std::memory_order_release);
    result.error = scene_error(
        ErrorCode::kInvariantViolation,
        "Planner scene projection acknowledged an invalid revision",
        {{"reason", "scene_projection_revision_mismatch"},
         {"expected_revision_id", std::to_string(expected_revision_id)},
         {"projection_revision_id",
          std::to_string(
              projection_result.acknowledged_revision.revision_id)}});
    return result;
  }

  ErrorDTO publication_error;
  auto configured_collision_policy =
      current->logical->configured_collision_policy;
  if (const auto* policy =
          std::get_if<SetCollisionPolicyRequestDTO>(&transaction.operation)) {
    configured_collision_policy = policy->policy;
    // Presence-aware frontends omit fields that do not apply to the selected
    // contact mode, while a directly constructed C++ DTO necessarily carries
    // its member defaults. The projection validates the authored request
    // before this point; commit the same canonical representation it publishes
    // so ignored typed defaults cannot change logical policy identity.
    canonicalize_mode_scoped_contact_fields(configured_collision_policy);
  }
  if (!refresh_committed_publication(
          projection_result.acknowledged_revision,
          std::move(configured_collision_policy),
          projection_.robot_placements(),
          projection_.scene_objects(), result.transaction_sequence,
          admission_epoch,
          std::nullopt, std::nullopt, publication_error)) {
    desynchronized_.store(true, std::memory_order_release);
    result.error = std::move(publication_error);
    return result;
  }

  result.success = true;
  result.committed_revision = projection_result.acknowledged_revision;
  return result;
}

bool SceneAuthority::refresh_committed_publication(
    const SceneRevisionDTO& expected_revision,
    CollisionPolicySnapshot configured_collision_policy,
    std::vector<SceneRobotPlacementDTO> robot_placements,
    std::vector<SceneObjectSnapshotDTO> objects,
    std::uint64_t transaction_sequence,
    std::uint64_t admission_epoch,
    std::optional<std::uint64_t> expected_topology_epoch,
    std::optional<std::uint64_t> expected_policy_epoch,
    ErrorDTO& error) {
  auto collision =
      projection_publisher_.collision_world_policy_snapshot_bundle();
  auto runtime =
      projection_publisher_.published_runtime_collision_query_bundle();
  auto inventory =
      projection_publisher_.current_scene_snapshot_lease_inventory();
  if (!collision.world ||
      !same_revision(collision.world->revision, expected_revision)) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection collision publication does not match its acknowledged revision",
        {{"reason", "scene_projection_collision_publication_mismatch"}});
    return false;
  }
  if (!collision.policy.has_value()) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection did not publish the committed collision policy",
        {{"reason", "scene_projection_collision_policy_missing"}});
    return false;
  }
  if (!runtime || !runtime->world ||
      !same_revision(runtime->world->revision, expected_revision)) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection runtime publication does not match its acknowledged revision",
        {{"reason", "scene_projection_runtime_publication_mismatch"}});
    return false;
  }
  if (!inventory || !inventory->bundle.world ||
      !same_revision(inventory->bundle.world->revision, expected_revision)) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection lease inventory does not match its acknowledged revision",
        {{"reason", "scene_projection_lease_inventory_mismatch"}});
    return false;
  }
  if ((expected_topology_epoch.has_value() &&
       inventory->topology_epoch != *expected_topology_epoch) ||
      (expected_policy_epoch.has_value() &&
       inventory->policy_epoch != *expected_policy_epoch)) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection publication changed authority-owned epochs",
        {{"reason", "scene_projection_epoch_mismatch"},
         {"expected_topology_epoch",
          expected_topology_epoch.has_value()
              ? std::to_string(*expected_topology_epoch)
              : "unconstrained"},
         {"projection_topology_epoch",
          std::to_string(inventory->topology_epoch)},
         {"expected_policy_epoch",
          expected_policy_epoch.has_value()
              ? std::to_string(*expected_policy_epoch)
              : "unconstrained"},
         {"projection_policy_epoch",
          std::to_string(inventory->policy_epoch)}});
    return false;
  }
  const auto projected_configured_policy =
      projection_.configured_collision_policy();
  if (!same_configured_collision_policy(projected_configured_policy,
                                        configured_collision_policy)) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection configured collision policy does not match the logical commit",
        {{"reason", "scene_projection_configured_policy_mismatch"},
         {"expected_policy_id", configured_collision_policy.policy_id},
         {"projection_policy_id", projected_configured_policy.policy_id},
         {"expected_policy_revision_id",
          std::to_string(configured_collision_policy.revision.revision_id)},
         {"projection_policy_revision_id",
          std::to_string(projected_configured_policy.revision.revision_id)}});
    return false;
  }

  std::sort(robot_placements.begin(), robot_placements.end(),
            [](const SceneRobotPlacementDTO& left,
               const SceneRobotPlacementDTO& right) {
              return left.robot_id < right.robot_id;
            });
  const auto duplicate_robot = std::adjacent_find(
      robot_placements.begin(), robot_placements.end(),
      [](const SceneRobotPlacementDTO& left,
         const SceneRobotPlacementDTO& right) {
        return left.robot_id == right.robot_id;
      });
  if (duplicate_robot != robot_placements.end() ||
      std::any_of(robot_placements.begin(), robot_placements.end(),
                  [](const auto& placement) {
                    return placement.robot_id.empty();
                  })) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection published invalid logical robot placement identity",
        {{"reason", "scene_projection_invalid_robot_placement"}});
    return false;
  }

  std::sort(objects.begin(), objects.end(),
            [](const SceneObjectSnapshotDTO& left,
               const SceneObjectSnapshotDTO& right) {
              return left.object_id < right.object_id;
            });
  const auto duplicate = std::adjacent_find(
      objects.begin(), objects.end(),
      [](const SceneObjectSnapshotDTO& left,
         const SceneObjectSnapshotDTO& right) {
        return left.object_id == right.object_id;
      });
  if (duplicate != objects.end()) {
    error = scene_error(
        ErrorCode::kInvariantViolation,
        "Projection published duplicate logical object identity",
        {{"reason", "scene_projection_duplicate_object"},
         {"object_id", duplicate->object_id}});
    return false;
  }

  auto logical = std::make_shared<LogicalSceneSnapshot>();
  logical->revision = expected_revision;
  logical->transaction_sequence = transaction_sequence;
  logical->admission_epoch = admission_epoch;
  logical->topology_epoch = inventory->topology_epoch;
  logical->policy_epoch = inventory->policy_epoch;
  logical->world_frame = config_.world_frame;
  logical->configured_collision_policy =
      std::move(configured_collision_policy);
  logical->robot_placements = std::move(robot_placements);
  logical->objects = std::move(objects);

  auto next = std::make_shared<CommittedPublication>();
  next->logical = std::move(logical);
  next->collision = std::move(collision);
  next->inventory = std::move(inventory);
  next->runtime = std::move(runtime);
  publication_.store(std::move(next), std::memory_order_release);
  return true;
}

Result<SceneRevisionDTO> SceneAuthority::rebuild_projection() {
  std::lock_guard<std::mutex> lock(transaction_mutex_);
  const auto current = publication_.load(std::memory_order_acquire);
  if (!current || !current->logical) {
    return Result<SceneRevisionDTO>::Err(scene_error(
        ErrorCode::kInternalError,
        "Logical scene publication is unavailable for projection rebuild",
        {{"reason", "scene_authority_publication_unavailable"}}));
  }

  desynchronized_.store(true, std::memory_order_release);
  const auto rebuilt = projection_.rebuild_projection(
      *current->logical, current->logical->revision);
  if (!rebuilt.edit.success ||
      !same_revision(rebuilt.acknowledged_revision,
                     current->logical->revision) ||
      !projection_.projection_health().ready()) {
    auto error = rebuilt.edit.error.value_or(scene_error(
        ErrorCode::kInvariantViolation,
        "Planner scene projection rebuild did not restore the committed revision",
        {{"reason", "scene_projection_rebuild_failed"}}));
    error.details.try_emplace(
        "expected_revision_id",
        std::to_string(current->logical->revision.revision_id));
    error.details.try_emplace(
        "acknowledged_revision_id",
        std::to_string(rebuilt.acknowledged_revision.revision_id));
    return Result<SceneRevisionDTO>::Err(std::move(error));
  }

  ErrorDTO publication_error;
  if (!refresh_committed_publication(
          current->logical->revision,
          current->logical->configured_collision_policy,
          projection_.robot_placements(),
          projection_.scene_objects(),
          current->logical->transaction_sequence,
          current->logical->admission_epoch,
          current->logical->topology_epoch,
          current->logical->policy_epoch, publication_error)) {
    return Result<SceneRevisionDTO>::Err(std::move(publication_error));
  }
  desynchronized_.store(false, std::memory_order_release);
  return Result<SceneRevisionDTO>::Ok(current->logical->revision);
}

SceneRevisionDTO SceneAuthority::current_revision() const {
  const auto current = publication_.load(std::memory_order_acquire);
  return current && current->logical ? current->logical->revision
                                     : SceneRevisionDTO{};
}

SceneAuthorityStatusDTO SceneAuthority::authority_status() const {
  const auto current = publication_.load(std::memory_order_acquire);
  SceneAuthorityStatusDTO status;
  if (!current || !current->logical) {
    return status;
  }
  status.revision = current->logical->revision;
  status.transaction_sequence = current->logical->transaction_sequence;
  status.admission_epoch = current->logical->admission_epoch;
  status.topology_epoch = current->logical->topology_epoch;
  status.policy_epoch = current->logical->policy_epoch;
  return status;
}

LogicalSceneSnapshotPtr SceneAuthority::logical_snapshot() const {
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->logical : nullptr;
}

template <typename Request>
SceneEditResultDTO SceneAuthority::commit_typed(Request request,
                                                const char* source) {
  // One-shot convenience edits bind to "current" by contract. Acquire the
  // writer mutex before reading that revision so concurrent convenience edits
  // serialize instead of manufacturing a stale optimistic transaction.
  std::lock_guard<std::mutex> lock(transaction_mutex_);
  SceneTransactionDTO transaction;
  transaction.expected_revision = current_revision();
  transaction.operation = std::move(request);
  transaction.authority_source = source;
  return edit_result(commit_locked(std::move(transaction)));
}

SceneEditResultDTO SceneAuthority::edit_result(
    const SceneCommitResultDTO& commit_result) {
  SceneEditResultDTO result;
  result.success = commit_result.success;
  result.error = commit_result.error;
  result.update_status = scene_update_status_name(commit_result.update_status);
  result.collision_world_revision_changed =
      commit_result.update_status == SceneUpdateStatus::kGeometryChanged;
  result.collision_world_freshness_changed =
      commit_result.update_status == SceneUpdateStatus::kFreshnessOnly;
  return result;
}

SceneEditResultDTO SceneAuthority::add_object(
    const AddObjectRequestDTO& request) {
  return commit_typed(request, "scene_authority.add_object");
}

SceneEditResultDTO SceneAuthority::remove_object(
    const RemoveObjectRequestDTO& request) {
  return commit_typed(request, "scene_authority.remove_object");
}

SceneEditResultDTO SceneAuthority::update_object_pose(
    const UpdateObjectPoseRequestDTO& request) {
  std::lock_guard<std::mutex> lock(transaction_mutex_);
  SceneTransactionDTO transaction;
  transaction.expected_revision = current_revision();
  transaction.operation = request;
  transaction.authority_source = "scene_authority.update_object_pose";
  if (request.source_id.has_value()) {
    transaction.recovery_admission_impact =
        SceneRecoveryAdmissionImpact::kLiveObservation;
  }
  return edit_result(commit_locked(std::move(transaction)));
}

SceneEditResultDTO SceneAuthority::attach_object(
    const AttachObjectRequestDTO& request) {
  return commit_typed(request, "scene_authority.attach_object");
}

SceneEditResultDTO SceneAuthority::detach_object(
    const DetachObjectRequestDTO& request) {
  return commit_typed(request, "scene_authority.detach_object");
}

SceneEditResultDTO SceneAuthority::set_robot_base_pose(
    const SetRobotBasePoseRequestDTO& request) {
  return commit_typed(request, "scene_authority.set_robot_base_pose");
}

SceneEditResultDTO SceneAuthority::set_collision_policy(
    const SetCollisionPolicyRequestDTO& request) {
  return commit_typed(request, "scene_authority.set_collision_policy");
}

std::vector<SceneObjectSnapshotDTO> SceneAuthority::scene_objects() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return {};
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current && current->logical
             ? current->logical->objects
             : std::vector<SceneObjectSnapshotDTO>{};
}

std::vector<SceneRobotPlacementDTO> SceneAuthority::robot_placements() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return {};
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current && current->logical
             ? current->logical->robot_placements
             : std::vector<SceneRobotPlacementDTO>{};
}

Result<Pose3DTO> SceneAuthority::object_pose(
    const std::string& object_id, const std::string& base_frame) const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return Result<Pose3DTO>::Err(scene_error(
        ErrorCode::kInvariantViolation,
        "Planner scene projection is desynchronized",
        {{"reason", "scene_projection_desynchronized"}}));
  }
  const auto current = publication_.load(std::memory_order_acquire);
  if (!current || !current->logical) {
    return Result<Pose3DTO>::Err(scene_error(
        ErrorCode::kInternalError,
        "Logical scene publication is unavailable",
        {{"reason", "scene_authority_publication_unavailable"}}));
  }
  return projection_.object_pose(object_id, base_frame,
                                 current->logical->revision);
}

ObjectStateDTO SceneAuthority::object_state(const std::string& object_id) const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return ObjectStateDTO{object_id, false, false, std::nullopt};
  }
  const auto current = publication_.load(std::memory_order_acquire);
  if (!current || !current->logical) {
    return ObjectStateDTO{object_id, false, false, std::nullopt};
  }
  const auto object = std::lower_bound(
      current->logical->objects.begin(), current->logical->objects.end(),
      object_id,
      [](const SceneObjectSnapshotDTO& candidate, const std::string& id) {
        return candidate.object_id < id;
      });
  if (object == current->logical->objects.end() ||
      object->object_id != object_id) {
    return ObjectStateDTO{object_id, false, false, std::nullopt};
  }
  return ObjectStateDTO{object_id, true, object->attached, object->frame_id};
}

CollisionWorldSnapshotPtr SceneAuthority::snapshot() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->collision.world : nullptr;
}

CollisionWorldRevision SceneAuthority::published_collision_world_revision()
    const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return {};
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current && current->collision.world
             ? current->collision.world->revision
             : CollisionWorldRevision{};
}

std::optional<CollisionPolicySnapshot>
SceneAuthority::collision_policy_snapshot() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->collision.policy : std::nullopt;
}

CollisionWorldPolicySnapshotBundle
SceneAuthority::collision_world_policy_snapshot_bundle() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return {};
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->collision : CollisionWorldPolicySnapshotBundle{};
}

std::optional<CollisionWorldLeaseInventory>
SceneAuthority::current_scene_snapshot_lease_inventory() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->inventory : std::nullopt;
}

std::optional<CollisionWorldLeaseInventory>
SceneAuthority::retain_scene_snapshot_lease_inventory(
    std::string retention_id) {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  const auto revision = current_revision();
  std::lock_guard<std::mutex> lock(retention_mutex_);
  const bool generated_id = retention_id.empty();
  if (generated_id) {
    do {
      retention_id =
          "scene_lease_" +
          std::to_string(next_retention_sequence_.fetch_add(
              1, std::memory_order_relaxed));
    } while (std::any_of(
        retained_inventories_.begin(), retained_inventories_.end(),
        [&](const auto& inventory) {
          return inventory.retention_id == retention_id;
        }));
  }
  const auto existing = std::find_if(
      retained_inventories_.begin(), retained_inventories_.end(),
      [&](const auto& inventory) {
        return inventory.retention_id == retention_id;
      });
  if (existing != retained_inventories_.end()) {
    if (!existing->bundle.world ||
        !same_revision(existing->bundle.world->revision, revision)) {
      return std::nullopt;
    }
    return *existing;
  }
  if (retained_inventories_.size() >= config_.retained_lease_capacity) {
    return std::nullopt;
  }
  auto retained = projection_.retain_projection(revision, retention_id);
  if (!retained.ok() || !retained.value().bundle.world ||
      !same_revision(retained.value().bundle.world->revision, revision)) {
    projection_.release_projection(retention_id);
    return std::nullopt;
  }
  if (desynchronized_.load(std::memory_order_acquire)) {
    projection_.release_projection(retention_id);
    return std::nullopt;
  }
  auto inventory = std::move(retained.value());
  inventory.retention_id = retention_id;
  retained_inventories_.push_back(inventory);
  return inventory;
}

void SceneAuthority::release_scene_snapshot_lease_inventory(
    const std::string& retention_id) {
  {
    std::lock_guard<std::mutex> lock(retention_mutex_);
    std::erase_if(retained_inventories_, [&](const auto& inventory) {
      return inventory.retention_id == retention_id;
    });
  }
  projection_.release_projection(retention_id);
}

CollisionWorldLeaseRetentionStats
SceneAuthority::scene_snapshot_lease_retention_stats() const {
  std::lock_guard<std::mutex> lock(retention_mutex_);
  CollisionWorldLeaseRetentionStats stats;
  stats.retained_count = retained_inventories_.size();
  stats.retained_capacity = config_.retained_lease_capacity;
  stats.retained_ids_oldest_first.reserve(retained_inventories_.size());
  for (const auto& inventory : retained_inventories_) {
    stats.retained_ids_oldest_first.push_back(inventory.retention_id);
  }
  return stats;
}

RuntimeCollisionQueryBundlePtr
SceneAuthority::published_runtime_collision_query_bundle() const {
  if (desynchronized_.load(std::memory_order_acquire)) {
    return nullptr;
  }
  const auto current = publication_.load(std::memory_order_acquire);
  return current ? current->runtime : nullptr;
}

}  // namespace manyforge::core
