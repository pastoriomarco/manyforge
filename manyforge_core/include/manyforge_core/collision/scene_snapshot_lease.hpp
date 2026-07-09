#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/geometry.hpp>

namespace manyforge::core {

enum class ObservedSceneObjectRole {
  kStaticObstacle,
  kSnapshotObstacle,
  kObservedTarget,
  kObservedDynamicObstacle,
  kAttachedObject,
  kSupportContact,
  kVisualizationOnly,
  kRobotRuntimeState,
};

enum class SceneSnapshotLeaseRejection {
  kNone,
  kSceneSnapshotLeaseMissing,
  kLiveSceneSnapshotUnavailable,
  kSceneSnapshotLeaseExpired,
  kObservedSceneSourceStale,
  kSceneTopologyChangedAfterValidation,
  kCollisionPolicyChangedAfterValidation,
  kTargetMovedAfterValidation,
  kObstacleMovedAfterValidation,
  kRuntimeDynamicSceneNotQualified,
};

const char* to_string(ObservedSceneObjectRole role);
const char* to_reason_code(SceneSnapshotLeaseRejection rejection);

struct SceneSnapshotLeaseObject {
  std::string object_id;
  ObservedSceneObjectRole role = ObservedSceneObjectRole::kStaticObstacle;
  std::vector<CollisionBodyId> collision_body_ids;

  // Non-pose collision identity: shape, body inventory, touch-link, support
  // contact, attachment, and other semantic object content that must not drift
  // silently after validation. Pose-bearing observed objects still use the
  // explicit pose drift checks below for their world-pose divergence.
  std::optional<std::string> object_revision_token;
  std::map<CollisionBodyId, std::string> body_content_hashes;

  std::optional<Pose3DTO> pose;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;

  std::optional<double> target_drift_tolerance_m;
  std::optional<double> target_angular_tolerance_rad;
  std::optional<double> obstacle_drift_tolerance_m;
  std::optional<double> obstacle_angular_tolerance_rad;
  bool runtime_dynamic_obstacle_allowed = false;
};

struct SceneSnapshotLease {
  std::string lease_id;
  std::uint64_t validation_epoch = 0;
  std::int64_t captured_at_ns = 0;
  std::optional<std::int64_t> lease_max_age_ns;
  CollisionWorldRevision world_revision;
  CollisionPolicyRevision policy_revision;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::vector<SceneSnapshotLeaseObject> objects;
};

struct SceneSnapshotLiveObject {
  std::string object_id;
  std::optional<ObservedSceneObjectRole> declared_role;
  std::vector<CollisionBodyId> collision_body_ids;
  std::optional<std::string> object_revision_token;
  std::map<CollisionBodyId, std::string> body_content_hashes;
  std::optional<Pose3DTO> pose;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;
  std::optional<std::int64_t> lease_max_age_ns;
  std::optional<double> target_drift_tolerance_m;
  std::optional<double> target_angular_tolerance_rad;
  std::optional<double> obstacle_drift_tolerance_m;
  std::optional<double> obstacle_angular_tolerance_rad;
  bool runtime_dynamic_obstacle_allowed = false;
};

struct RuntimeDynamicSceneCoverage {
  bool configured = false;
  bool armed = false;
  bool fresh = false;
  bool qualified = false;
  std::string coverage_id;
};

struct SceneSnapshotLeaseValidationContext {
  std::int64_t now_ns = 0;
  std::optional<std::int64_t> default_lease_max_age_ns;
  std::optional<CollisionWorldRevision> live_world_revision;
  std::optional<CollisionPolicyRevision> live_policy_revision;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::vector<SceneSnapshotLiveObject> live_objects;
  RuntimeDynamicSceneCoverage runtime_dynamic_scene_coverage;
};

struct SceneSnapshotLeaseValidationResult {
  bool accepted = false;
  SceneSnapshotLeaseRejection rejection = SceneSnapshotLeaseRejection::kNone;
  std::string reason_code;
  std::string lease_id;
  CollisionWorldRevision validated_world_revision;
  CollisionWorldRevision live_world_revision;
  CollisionPolicyRevision validated_policy_revision;
  CollisionPolicyRevision live_policy_revision;
  std::optional<std::string> object_id;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> lease_age_ns;
  std::optional<std::int64_t> source_age_ns;
  std::optional<double> target_drift_m;
  std::optional<double> target_angular_drift_rad;
  std::optional<double> obstacle_drift_m;
  std::optional<double> obstacle_angular_drift_rad;
  std::map<std::string, std::string> details;
};

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const SceneSnapshotLease& lease,
    const SceneSnapshotLeaseValidationContext& context);

SceneSnapshotLeaseValidationResult validate_scene_snapshot_lease(
    const std::optional<SceneSnapshotLease>& lease,
    const SceneSnapshotLeaseValidationContext& context);

}  // namespace manyforge::core
