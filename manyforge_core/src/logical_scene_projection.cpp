#include <algorithm>
#include <limits>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Value>
Result<Value> projection_error(std::string message) {
  return Result<Value>::Err(ErrorDTO{
      std::string(to_string(ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", "resolved_scene_transaction_mismatch"}},
  });
}

template <typename Value, typename Id, typename Projection>
auto find_exact(std::vector<Value>& values, const Id& id, Projection projection) {
  const auto found =
      std::lower_bound(values.begin(), values.end(), id,
                       [&](const Value& value, const Id& key) { return projection(value) < key; });
  return found != values.end() && projection(*found) == id ? found : values.end();
}

bool only_provider_evidence_changed(const SceneObjectSnapshotDTO& previous,
                                    const SceneObjectSnapshotDTO& next) {
  auto normalized = previous;
  normalized.provider_evidence = next.provider_evidence;
  return normalized == next;
}

bool exact_content_identity(const LogicalSceneSnapshotDTO& snapshot) {
  try {
    return logical_scene_identity::ComputeLogicalSceneContentHash(snapshot) ==
           snapshot.revision.content_hash;
  } catch (...) {
    return false;
  }
}

bool exact_policy_identity(const CollisionPolicySnapshot& policy) {
  if (!logical_scene_detail::LocallyValidConfiguredPolicy(policy)) {
    return false;
  }
  try {
    return logical_scene_identity::ComputeCollisionPolicyContentDigest(policy).value() ==
           policy.revision.content_hash;
  } catch (...) {
    return false;
  }
}

}  // namespace

Result<LogicalSceneSnapshotDTO> ApplyResolvedSceneTransaction(
    const LogicalSceneSnapshotDTO& current, const SceneTransactionDTO& transaction) {
  const bool resolution_evidence_required =
      !std::holds_alternative<ResolvedRemoveSceneObjectDTO>(
          transaction.resolved_operation) &&
      !std::holds_alternative<ResolvedCollisionPolicyDTO>(
          transaction.resolved_operation);
  if (!current.locally_valid() || !exact_content_identity(current) ||
      transaction.request_id.empty() || !transaction.attribution.locally_valid() ||
      transaction.transaction_sequence == 0U ||
      transaction.transaction_sequence <= current.transaction_sequence ||
      !transaction.prior_projection_revision.valid() ||
      !transaction.proposed_projection_revision.valid() ||
      transaction.prior_projection_revision.scene_revision != current.revision ||
      transaction.proposed_projection_revision.retained_bundle_id !=
          transaction.prior_projection_revision.retained_bundle_id ||
      transaction.proposed_projection_revision.graph_revision !=
          transaction.prior_projection_revision.graph_revision ||
      transaction.proposed_projection_revision.scene_revision.scene_id !=
          current.revision.scene_id ||
      !IsExactLowercaseSha256Digest(
          transaction.original_request_digest.value()) ||
      transaction.resolution_evidence_digest.has_value() !=
          resolution_evidence_required ||
      (transaction.resolution_evidence_digest.has_value() &&
       !IsExactLowercaseSha256Digest(
           transaction.resolution_evidence_digest->value()))) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "resolved scene transaction identity is incomplete or nonsequential");
  }

  LogicalSceneSnapshotDTO candidate = current;
  candidate.transaction_sequence = transaction.transaction_sequence;
  bool content_changed = false;
  bool topology_changed = false;
  bool policy_changed = false;

  const auto operation_valid = std::visit(
      [&](const auto& operation) -> bool {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, ResolvedUpsertSceneObjectDTO>) {
          if (!operation.object.locally_valid()) {
            return false;
          }
          const auto found = find_exact(
              candidate.objects, operation.object.scene_resource_id,
              [](const SceneObjectSnapshotDTO& object) { return object.scene_resource_id; });
          if (found == candidate.objects.end()) {
            if (operation.attachment) return false;
            const auto insertion = std::lower_bound(
                candidate.objects.begin(), candidate.objects.end(),
                operation.object.scene_resource_id,
                [](const SceneObjectSnapshotDTO& object, const SceneResourceId& id) {
                  return object.scene_resource_id < id;
                });
            candidate.objects.insert(insertion, operation.object);
            content_changed = true;
            topology_changed = true;
            return true;
          }
          if (operation.attachment) {
            const auto& next = *operation.attachment;
            const auto* carried = std::get_if<CarriedSceneObjectPlacementDTO>(&found->placement);
            const auto previous =
                find_exact(candidate.carried_object_attachments, next.carried_object_attachment_id,
                           [](const CarriedObjectAttachmentDTO& value) {
                             return value.carried_object_attachment_id;
                           });
            const auto* placement =
                std::get_if<CarriedSceneObjectPlacementDTO>(&operation.object.placement);
            if (!carried || !placement || !next.locally_valid() ||
                previous == candidate.carried_object_attachments.end() ||
                carried->carried_object_attachment_id != next.carried_object_attachment_id ||
                carried->carried_object_attachment_revision !=
                    previous->carried_object_attachment_revision ||
                placement->carried_object_attachment_id != next.carried_object_attachment_id ||
                placement->carried_object_attachment_revision !=
                    next.carried_object_attachment_revision ||
                next.scene_resource_id != operation.object.scene_resource_id ||
                next.controllable_component_id != previous->controllable_component_id ||
                next.command_target_id != previous->command_target_id ||
                next.carrier_frame_ref != previous->carrier_frame_ref ||
                next.touch_body_roles != previous->touch_body_roles)
              return false;
            *previous = next;
          }
          if (*found == operation.object) {
            return true;
          }
          topology_changed = !only_provider_evidence_changed(*found, operation.object);
          *found = operation.object;
          content_changed = true;
          return true;
        } else if constexpr (std::is_same_v<T, ResolvedRemoveSceneObjectDTO>) {
          const auto found = find_exact(
              candidate.objects, operation.scene_resource_id,
              [](const SceneObjectSnapshotDTO& object) { return object.scene_resource_id; });
          if (found == candidate.objects.end()) {
            return false;
          }
          candidate.objects.erase(found);
          content_changed = true;
          topology_changed = true;
          return true;
        } else if constexpr (std::is_same_v<T, ResolvedAttachCarriedObjectDTO>) {
          if (!operation.object.locally_valid() || !operation.attachment.locally_valid() ||
              operation.object.scene_resource_id != operation.attachment.scene_resource_id) {
            return false;
          }
          const auto object = find_exact(
              candidate.objects, operation.object.scene_resource_id,
              [](const SceneObjectSnapshotDTO& value) { return value.scene_resource_id; });
          if (object == candidate.objects.end()) {
            return false;
          }
          const auto attachment = std::lower_bound(
              candidate.carried_object_attachments.begin(),
              candidate.carried_object_attachments.end(),
              operation.attachment.carried_object_attachment_id,
              [](const CarriedObjectAttachmentDTO& value, const CarriedObjectAttachmentId& id) {
                return value.carried_object_attachment_id < id;
              });
          if (attachment != candidate.carried_object_attachments.end() &&
              attachment->carried_object_attachment_id ==
                  operation.attachment.carried_object_attachment_id) {
            return false;
          }
          *object = operation.object;
          candidate.carried_object_attachments.insert(attachment, operation.attachment);
          content_changed = true;
          topology_changed = true;
          policy_changed = true;
          return true;
        } else if constexpr (std::is_same_v<T, ResolvedDetachCarriedObjectDTO>) {
          if (!operation.object.locally_valid()) {
            return false;
          }
          const auto object = find_exact(
              candidate.objects, operation.object.scene_resource_id,
              [](const SceneObjectSnapshotDTO& value) { return value.scene_resource_id; });
          const auto attachment =
              find_exact(candidate.carried_object_attachments, operation.removed_attachment_id,
                         [](const CarriedObjectAttachmentDTO& value) {
                           return value.carried_object_attachment_id;
                         });
          if (object == candidate.objects.end() ||
              attachment == candidate.carried_object_attachments.end() ||
              attachment->scene_resource_id != operation.object.scene_resource_id ||
              attachment->carried_object_attachment_revision !=
                  operation.removed_attachment_revision) {
            return false;
          }
          *object = operation.object;
          candidate.carried_object_attachments.erase(attachment);
          content_changed = true;
          topology_changed = true;
          policy_changed = true;
          return true;
        } else {
          if (!exact_policy_identity(operation.configured_policy) ||
              !exact_policy_identity(operation.effective_policy)) {
            return false;
          }
          if (candidate.configured_collision_policy == operation.configured_policy) {
            return true;
          }
          candidate.configured_collision_policy = operation.configured_policy;
          content_changed = true;
          policy_changed = true;
          return true;
        }
      },
      transaction.resolved_operation);
  if (!operation_valid) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "resolved scene operation is inconsistent with its predecessor");
  }

  const auto& proposed = transaction.proposed_projection_revision.scene_revision;
  if (content_changed) {
    if (current.revision.revision_id == std::numeric_limits<std::uint64_t>::max() ||
        proposed.revision_id != current.revision.revision_id + 1U ||
        (topology_changed &&
         candidate.topology_epoch == std::numeric_limits<std::uint64_t>::max()) ||
        (policy_changed && candidate.policy_epoch == std::numeric_limits<std::uint64_t>::max())) {
      return projection_error<LogicalSceneSnapshotDTO>(
          "resolved scene successor counters are exhausted or nonsequential");
    }
    if (topology_changed) {
      ++candidate.topology_epoch;
    }
    if (policy_changed) {
      ++candidate.policy_epoch;
    }
    candidate.revision = proposed;
  } else if (proposed != current.revision) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "no-op scene transaction changed the composite scene revision");
  }

  candidate.revision = proposed;
  if (!candidate.locally_valid() || !exact_content_identity(candidate)) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "resolved scene operation does not reproduce the proposed content");
  }
  return Result<LogicalSceneSnapshotDTO>::Ok(std::move(candidate));
}

}  // namespace manyforge::core
