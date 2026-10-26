#include <manyforge_core/collision/scene_snapshot_lease.hpp>

#include <manyforge_core/types/reason_codes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace manyforge::core {
namespace {

constexpr double kEpsilon = 1e-12;
constexpr std::size_t kMaxRecordedObjectDivergences = 16;

std::string to_string(std::uint64_t value) { return std::to_string(value); }

std::string to_string(std::int64_t value) { return std::to_string(value); }

std::string to_string(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

bool revisions_equal(const CollisionWorldRevision& lhs,
                     const CollisionWorldRevision& rhs) {
  return lhs.revision_id == rhs.revision_id &&
         lhs.content_hash == rhs.content_hash;
}

bool revisions_equal(const CollisionPolicyRevision& lhs,
                     const CollisionPolicyRevision& rhs) {
  return lhs.revision_id == rhs.revision_id &&
         lhs.content_hash == rhs.content_hash;
}

bool sorted_vectors_equal(std::vector<CollisionBodyId> lhs,
                          std::vector<CollisionBodyId> rhs) {
  std::sort(lhs.begin(), lhs.end());
  std::sort(rhs.begin(), rhs.end());
  return lhs == rhs;
}

double norm(const QuaternionDTO& q) {
  return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

bool finite_pose(const Pose3DTO& pose) {
  return std::isfinite(pose.position_m.x_m) &&
         std::isfinite(pose.position_m.y_m) &&
         std::isfinite(pose.position_m.z_m) &&
         std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) &&
         std::isfinite(pose.orientation.z) &&
         std::isfinite(pose.orientation.w);
}

struct PoseDelta {
  double translation_m = 0.0;
  double angle_rad = 0.0;
};

std::optional<PoseDelta> pose_delta(const Pose3DTO& a, const Pose3DTO& b) {
  if (!finite_pose(a) || !finite_pose(b)) {
    return std::nullopt;
  }
  const double dx = a.position_m.x_m - b.position_m.x_m;
  const double dy = a.position_m.y_m - b.position_m.y_m;
  const double dz = a.position_m.z_m - b.position_m.z_m;
  const double na = norm(a.orientation);
  const double nb = norm(b.orientation);
  if (!std::isfinite(na) || !std::isfinite(nb) || na <= kEpsilon ||
      nb <= kEpsilon) {
    return std::nullopt;
  }
  double dot = (a.orientation.x * b.orientation.x +
                a.orientation.y * b.orientation.y +
                a.orientation.z * b.orientation.z +
                a.orientation.w * b.orientation.w) /
               (na * nb);
  dot = std::clamp(std::abs(dot), 0.0, 1.0);
  return PoseDelta{std::sqrt(dx * dx + dy * dy + dz * dz),
                   2.0 * std::acos(dot)};
}

std::int64_t age_ns(std::int64_t now_ns, std::int64_t then_ns) {
  return now_ns - then_ns;
}

bool positive_budget(std::int64_t budget_ns) { return budget_ns > 0; }

SceneSnapshotLeaseValidationResult base_result(
    const SceneSnapshotLease& lease,
    const SceneSnapshotLeaseValidationContext& context) {
  SceneSnapshotLeaseValidationResult result;
  result.lease_id = lease.lease_id;
  result.validated_projection_revision = lease.projection_revision;
  result.live_projection_revision = context.live_projection_revision;
  result.validated_world_revision = lease.world_revision;
  result.validated_policy_revision = lease.policy_revision;
  if (context.live_world_revision.has_value()) {
    result.live_world_revision = *context.live_world_revision;
  }
  if (context.live_policy_revision.has_value()) {
    result.live_policy_revision = *context.live_policy_revision;
  }
  result.lease_age_ns = age_ns(context.now_ns, lease.captured_at_ns);
  return result;
}

void add_revision_details(SceneSnapshotLeaseValidationResult& result,
                          const SceneSnapshotLease& lease,
                          const SceneSnapshotLeaseValidationContext& context) {
  result.details["validated_retained_bundle_id"] =
      lease.projection_revision.retained_bundle_id.value();
  result.details["validated_graph_revision"] =
      lease.projection_revision.graph_revision.value();
  result.details["validated_scene_id"] =
      lease.projection_revision.scene_revision.scene_id.value();
  result.details["validated_scene_revision_id"] =
      to_string(lease.projection_revision.scene_revision.revision_id);
  result.details["validated_scene_content_hash"] =
      lease.projection_revision.scene_revision.content_hash.value();
  if (context.live_projection_revision.has_value()) {
    result.details["live_retained_bundle_id"] =
        context.live_projection_revision->retained_bundle_id.value();
    result.details["live_graph_revision"] =
        context.live_projection_revision->graph_revision.value();
    result.details["live_scene_id"] =
        context.live_projection_revision->scene_revision.scene_id.value();
    result.details["live_scene_revision_id"] = to_string(
        context.live_projection_revision->scene_revision.revision_id);
    result.details["live_scene_content_hash"] =
        context.live_projection_revision->scene_revision.content_hash.value();
  }
  result.details["validated_world_revision_id"] =
      to_string(lease.world_revision.revision_id);
  result.details["validated_world_content_hash"] =
      lease.world_revision.content_hash;
  result.details["validated_policy_revision_id"] =
      to_string(lease.policy_revision.revision_id);
  result.details["validated_policy_content_hash"] =
      lease.policy_revision.content_hash;
  if (context.live_world_revision.has_value()) {
    result.details["live_world_revision_id"] =
        to_string(context.live_world_revision->revision_id);
    result.details["live_world_content_hash"] =
        context.live_world_revision->content_hash;
  }
  if (context.live_policy_revision.has_value()) {
    result.details["live_policy_revision_id"] =
        to_string(context.live_policy_revision->revision_id);
    result.details["live_policy_content_hash"] =
        context.live_policy_revision->content_hash;
  }
  result.details["validated_topology_epoch"] =
      to_string(lease.topology_epoch);
  result.details["live_topology_epoch"] = to_string(context.topology_epoch);
  result.details["validated_policy_epoch"] = to_string(lease.policy_epoch);
  result.details["live_policy_epoch"] = to_string(context.policy_epoch);
}

SceneSnapshotLeaseValidationResult reject(
    SceneSnapshotLeaseValidationResult result,
    SceneSnapshotLeaseRejection rejection) {
  result.accepted = false;
  result.rejection = rejection;
  result.reason_code = to_reason_code(rejection);
  return result;
}

bool source_is_stale(std::int64_t now_ns,
                     std::optional<std::int64_t> source_timestamp_ns,
                     std::optional<std::int64_t> source_budget_ns,
                     std::int64_t& out_age_ns) {
  if (source_timestamp_ns.has_value()) {
    out_age_ns = age_ns(now_ns, *source_timestamp_ns);
    if (out_age_ns < 0) {
      return true;
    }
  }
  if (!source_budget_ns.has_value()) {
    return false;
  }
  if (!source_timestamp_ns.has_value() ||
      !positive_budget(*source_budget_ns)) {
    out_age_ns = std::numeric_limits<std::int64_t>::max();
    return true;
  }
  return out_age_ns > *source_budget_ns;
}

using LiveObjectMap =
    std::unordered_map<std::string, const SceneSnapshotLiveObject*>;

LiveObjectMap live_object_map(
    const std::vector<SceneSnapshotLiveObject>& live_objects) {
  LiveObjectMap out;
  out.reserve(live_objects.size());
  for (const auto& object : live_objects) {
    out.emplace(object.object_id, &object);
  }
  return out;
}

std::optional<std::string> duplicate_live_object_id(
    const std::vector<SceneSnapshotLiveObject>& live_objects) {
  std::unordered_set<std::string> seen;
  seen.reserve(live_objects.size());
  for (const auto& object : live_objects) {
    if (!seen.insert(object.object_id).second) {
      return object.object_id;
    }
  }
  return std::nullopt;
}

std::optional<std::string> duplicate_lease_object_id(
    const std::vector<SceneSnapshotLeaseObject>& lease_objects) {
  std::unordered_set<std::string> seen;
  seen.reserve(lease_objects.size());
  for (const auto& object : lease_objects) {
    if (!seen.insert(object.object_id).second) {
      return object.object_id;
    }
  }
  return std::nullopt;
}

// Debug detail (R4): per lease object, what differs in the live snapshot.
// Bounded so a large changing scene cannot grow the evidence without limit.
void add_object_divergence_details(
    SceneSnapshotLeaseValidationResult& result,
    const SceneSnapshotLease& lease, const LiveObjectMap& live_by_id) {
  std::size_t divergent = 0;
  for (const auto& object : lease.objects) {
    std::map<std::string, std::string> changes;
    const auto it = live_by_id.find(object.object_id);
    if (it == live_by_id.end()) {
      changes["missing"] = "true";
    } else {
      const auto& live = *it->second;
      if (!sorted_vectors_equal(object.collision_body_ids,
                                live.collision_body_ids)) {
        changes["bodies"] = "true";
      }
      if (object.object_revision_token != live.object_revision_token) {
        changes["token"] = "true";
      }
      for (const auto& [body_id, expected_hash] : object.body_content_hashes) {
        const auto live_hash = live.body_content_hashes.find(body_id);
        if (live_hash == live.body_content_hashes.end() ||
            live_hash->second != expected_hash) {
          changes["body_hash"] = body_id;
          break;
        }
      }
      if (object.pose.has_value() && live.pose.has_value()) {
        const auto delta = pose_delta(*object.pose, *live.pose);
        if (!delta.has_value()) {
          changes["pose_delta_m"] = "invalid";
        } else if (delta->translation_m > 0.0 || delta->angle_rad > 0.0) {
          changes["pose_delta_m"] = to_string(delta->translation_m);
          changes["pose_delta_rad"] = to_string(delta->angle_rad);
        }
      } else if (object.pose.has_value() != live.pose.has_value()) {
        changes["pose_delta_m"] = "unavailable";
      }
    }
    if (changes.empty()) {
      continue;
    }
    if (divergent++ >= kMaxRecordedObjectDivergences) {
      continue;
    }
    const std::string prefix = "object_divergence." + object.object_id + ".";
    for (auto& [key, value] : changes) {
      result.details[prefix + key] = std::move(value);
    }
  }
  if (divergent > kMaxRecordedObjectDivergences) {
    result.details["object_divergence_truncated_count"] =
        std::to_string(divergent - kMaxRecordedObjectDivergences);
  }
}

}  // namespace

const char* to_reason_code(SceneSnapshotLeaseRejection rejection) {
  switch (rejection) {
    case SceneSnapshotLeaseRejection::kNone:
      return "";
    case SceneSnapshotLeaseRejection::kSceneSnapshotLeaseMissing:
      return reason_codes::kSceneSnapshotLeaseMissing;
    case SceneSnapshotLeaseRejection::kLiveSceneSnapshotUnavailable:
      return reason_codes::kLiveSceneSnapshotUnavailable;
    case SceneSnapshotLeaseRejection::kSceneSnapshotLeaseExpired:
      return reason_codes::kSceneSnapshotLeaseExpired;
    case SceneSnapshotLeaseRejection::kObservedSceneSourceStale:
      return reason_codes::kObservedSceneSourceStale;
    case SceneSnapshotLeaseRejection::
        kProjectionStructureChangedAfterValidation:
      return reason_codes::kProjectionStructureChangedAfterValidation;
    case SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid:
      return reason_codes::kSceneSnapshotStructureInvalid;
  }
  return "unknown_scene_snapshot_lease_rejection";
}

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const std::optional<SceneSnapshotLease>& lease,
    const SceneSnapshotLeaseValidationContext& context) {
  if (!lease.has_value()) {
    SceneSnapshotLeaseValidationResult result;
    result.rejection =
        SceneSnapshotLeaseRejection::kSceneSnapshotLeaseMissing;
    result.reason_code = to_reason_code(result.rejection);
    return result;
  }
  return validate_scene_snapshot_lease(*lease, context);
}

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const SceneSnapshotLease& lease,
    const SceneSnapshotLeaseValidationContext& context) {
  auto result = base_result(lease, context);
  add_revision_details(result, lease, context);

  if (!context.live_projection_revision.has_value() ||
      !context.live_projection_revision->valid() ||
      !context.live_world_revision.has_value() ||
      !context.live_policy_revision.has_value()) {
    return reject(std::move(result),
                  SceneSnapshotLeaseRejection::
                      kLiveSceneSnapshotUnavailable);
  }

  if (!lease.projection_revision.valid() ||
      lease.projection_revision.retained_bundle_id !=
          context.live_projection_revision->retained_bundle_id ||
      lease.projection_revision.graph_revision !=
          context.live_projection_revision->graph_revision ||
      lease.projection_revision.scene_revision.scene_id !=
          context.live_projection_revision->scene_revision.scene_id) {
    return reject(
        std::move(result),
        SceneSnapshotLeaseRejection::
            kProjectionStructureChangedAfterValidation);
  }
  if (lease.projection_revision.scene_revision !=
      context.live_projection_revision->scene_revision) {
    result.details["accepted_live_scene_revision_divergence"] = "true";
  }

  const std::int64_t lease_age =
      result.lease_age_ns.value_or(std::numeric_limits<std::int64_t>::max());
  const auto lease_budget = lease.lease_max_age_ns.has_value()
                                ? lease.lease_max_age_ns
                                : context.default_lease_max_age_ns;
  if (lease_age < 0 ||
      (lease_budget.has_value() &&
       (!positive_budget(*lease_budget) || lease_age > *lease_budget))) {
    if (lease_budget.has_value()) {
      result.details["lease_max_age_ns"] = to_string(*lease_budget);
    }
    if (lease_age < 0) {
      result.details["lease_age_negative"] = "true";
    }
    return reject(std::move(result),
                  SceneSnapshotLeaseRejection::kSceneSnapshotLeaseExpired);
  }

  // Dispatch accepts the latest scene for every object (2026-10-26): the
  // runtime monitor checks each tick against the latest published world and
  // rules. Divergence is recorded as revision-level evidence only.
  const bool topology_epoch_diverged =
      lease.topology_epoch != context.topology_epoch;
  if (topology_epoch_diverged) {
    result.details["live_topology_epoch_diverged"] = "true";
  }
  if (!revisions_equal(lease.policy_revision, *context.live_policy_revision)) {
    result.details["accepted_live_policy_revision_divergence"] = "true";
  }
  if (!revisions_equal(lease.world_revision,
                       *context.live_world_revision)) {
    result.details["accepted_live_world_revision_divergence"] = "true";
  }

  if (auto duplicate = duplicate_lease_object_id(lease.objects);
      duplicate.has_value()) {
    result.object_id = *duplicate;
    result.details["duplicate_lease_object_id"] = "true";
    return reject(std::move(result),
                  SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid);
  }
  if (auto duplicate = duplicate_live_object_id(context.live_objects);
      duplicate.has_value()) {
    result.object_id = *duplicate;
    result.details["duplicate_live_object_id"] = "true";
    return reject(std::move(result),
                  SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid);
  }
  for (const auto& object : lease.objects) {
    if (object.object_id.empty() || object.collision_body_ids.empty()) {
      result.object_id = object.object_id;
      result.details["missing_collision_object_identity"] = "true";
      return reject(std::move(result),
                    SceneSnapshotLeaseRejection::kSceneSnapshotStructureInvalid);
    }
  }

  // Freshness is judged on the latest evidence only: a source silent past its
  // budget means no data, which no later tick can correct. Objects added after
  // planning are covered because the pass runs over the live snapshot.
  for (const auto& live_object : context.live_objects) {
    std::int64_t live_source_age = 0;
    if (source_is_stale(context.now_ns, live_object.source_timestamp_ns,
                        live_object.source_freshness_budget_ns,
                        live_source_age)) {
      result.object_id = live_object.object_id;
      result.source_id = live_object.source_id;
      result.source_age_ns = live_source_age;
      if (live_object.source_freshness_budget_ns.has_value()) {
        result.details["source_freshness_budget_ns"] =
            to_string(*live_object.source_freshness_budget_ns);
      }
      return reject(std::move(result),
                    SceneSnapshotLeaseRejection::kObservedSceneSourceStale);
    }
  }

  if (context.record_object_divergence) {
    add_object_divergence_details(result, lease,
                                  live_object_map(context.live_objects));
  }

  result.accepted = true;
  result.rejection = SceneSnapshotLeaseRejection::kNone;
  result.reason_code.clear();
  if (topology_epoch_diverged) {
    result.details["accepted_live_topology_epoch_divergence"] = "true";
  }
  return result;
}

}  // namespace manyforge::core
