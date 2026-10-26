#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::core {

enum class SceneSnapshotLeaseRejection {
  kNone,
  kSceneSnapshotLeaseMissing,
  kLiveSceneSnapshotUnavailable,
  kSceneSnapshotLeaseExpired,
  kObservedSceneSourceStale,
  kProjectionStructureChangedAfterValidation,
  kSceneSnapshotStructureInvalid,
};

const char* to_reason_code(SceneSnapshotLeaseRejection rejection);

// Validation-time object identity. Dispatch accepts a newer scene for every
// object; these fields are evidence only, compared solely for the optional
// per-object divergence detail.
struct SceneSnapshotLeaseObject {
  std::string object_id;
  std::vector<CollisionBodyId> collision_body_ids;
  std::optional<std::string> object_revision_token;
  std::map<CollisionBodyId, std::string> body_content_hashes;
  std::optional<Pose3DTO> pose;
};

struct SceneSnapshotLease {
  std::string lease_id;
  std::uint64_t validation_epoch = 0;
  std::int64_t captured_at_ns = 0;
  std::optional<std::int64_t> lease_max_age_ns;
  ProjectionRevisionDTO projection_revision;
  CollisionWorldRevision world_revision;
  CollisionPolicyRevision policy_revision;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::vector<SceneSnapshotLeaseObject> objects;
};

struct SceneSnapshotLiveObject {
  std::string object_id;
  std::vector<CollisionBodyId> collision_body_ids;
  std::optional<std::string> object_revision_token;
  std::map<CollisionBodyId, std::string> body_content_hashes;
  std::optional<Pose3DTO> pose;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;
  std::optional<std::int64_t> lease_max_age_ns;
};

struct SceneSnapshotLeaseValidationContext {
  std::int64_t now_ns = 0;
  std::optional<std::int64_t> default_lease_max_age_ns;
  std::optional<ProjectionRevisionDTO> live_projection_revision;
  std::optional<CollisionWorldRevision> live_world_revision;
  std::optional<CollisionPolicyRevision> live_policy_revision;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::vector<SceneSnapshotLiveObject> live_objects;
  // Debug only: compare lease and live objects and record what changed per
  // object (bounded). Off, no per-object comparison runs.
  bool record_object_divergence = false;
};

struct SceneSnapshotLeaseValidationResult {
  bool accepted = false;
  SceneSnapshotLeaseRejection rejection = SceneSnapshotLeaseRejection::kNone;
  std::string reason_code;
  std::string lease_id;
  std::optional<ProjectionRevisionDTO> validated_projection_revision;
  std::optional<ProjectionRevisionDTO> live_projection_revision;
  CollisionWorldRevision validated_world_revision;
  CollisionWorldRevision live_world_revision;
  CollisionPolicyRevision validated_policy_revision;
  CollisionPolicyRevision live_policy_revision;
  std::optional<std::string> object_id;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> lease_age_ns;
  std::optional<std::int64_t> source_age_ns;
  std::map<std::string, std::string> details;
};

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const SceneSnapshotLease& lease,
    const SceneSnapshotLeaseValidationContext& context);

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const std::optional<SceneSnapshotLease>& lease,
    const SceneSnapshotLeaseValidationContext& context);

}  // namespace manyforge::core
