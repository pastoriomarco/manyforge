#include <algorithm>
#include <limits>
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

}  // namespace

Result<LogicalSceneSnapshotDTO> ApplyResolvedSceneTransaction(
    const LogicalSceneSnapshotDTO& current, const SceneTransactionDTO& transaction) {
  const auto& prior = transaction.prior_projection_revision;
  const auto& proposed_projection = transaction.proposed_projection_revision;
  if (transaction.transaction_sequence <= current.transaction_sequence ||
      prior.scene_revision != current.revision ||
      proposed_projection.retained_bundle_id != prior.retained_bundle_id ||
      proposed_projection.graph_revision != prior.graph_revision ||
      proposed_projection.scene_revision.scene_id != current.revision.scene_id) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "resolved scene transaction does not follow its predecessor");
  }

  LogicalSceneSnapshotDTO candidate = current;
  candidate.transaction_sequence = transaction.transaction_sequence;
  bool content_changed = false;
  bool topology_changed = false;
  bool policy_changed = false;

  const auto object_of = [](const SceneObjectSnapshotDTO& object) {
    return object.scene_resource_id;
  };
  const auto attachment_of = [](const CarriedObjectAttachmentDTO& attachment) {
    return attachment.carried_object_attachment_id;
  };
  const auto applied = std::visit(
      [&](const auto& operation) -> bool {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, ResolvedUpsertSceneObjectDTO>) {
          const auto found =
              find_exact(candidate.objects, operation.object.scene_resource_id, object_of);
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
            const auto previous =
                find_exact(candidate.carried_object_attachments,
                           operation.attachment->carried_object_attachment_id, attachment_of);
            if (previous == candidate.carried_object_attachments.end()) return false;
            *previous = *operation.attachment;
          }
          if (*found == operation.object) {
            return true;
          }
          topology_changed = !only_provider_evidence_changed(*found, operation.object);
          *found = operation.object;
          content_changed = true;
          return true;
        } else if constexpr (std::is_same_v<T, ResolvedRemoveSceneObjectDTO>) {
          const auto found =
              find_exact(candidate.objects, operation.scene_resource_id, object_of);
          if (found == candidate.objects.end()) {
            return false;
          }
          candidate.objects.erase(found);
          content_changed = true;
          topology_changed = true;
          return true;
        } else if constexpr (std::is_same_v<T, ResolvedAttachCarriedObjectDTO>) {
          const auto object =
              find_exact(candidate.objects, operation.object.scene_resource_id, object_of);
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
          const auto object =
              find_exact(candidate.objects, operation.object.scene_resource_id, object_of);
          const auto attachment = find_exact(candidate.carried_object_attachments,
                                             operation.removed_attachment_id, attachment_of);
          if (object == candidate.objects.end() ||
              attachment == candidate.carried_object_attachments.end()) {
            return false;
          }
          *object = operation.object;
          candidate.carried_object_attachments.erase(attachment);
          content_changed = true;
          topology_changed = true;
          policy_changed = true;
          return true;
        } else {
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
  if (!applied) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "resolved scene operation does not apply to its predecessor");
  }

  const auto& proposed = proposed_projection.scene_revision;
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
  } else if (proposed != current.revision) {
    return projection_error<LogicalSceneSnapshotDTO>(
        "no-op scene transaction changed the composite scene revision");
  }
  // The owner's proposed revision, content hash included, is trusted.
  candidate.revision = proposed;
  return Result<LogicalSceneSnapshotDTO>::Ok(std::move(candidate));
}

}  // namespace manyforge::core
