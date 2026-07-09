#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/collision.hpp>

namespace manyforge::core {

/// Box shape (spec §3.9)
struct BoxShapeDTO {
  Vector3DTO dimensions_m;  // full dimensions, matching URDF/ROS conventions
};

/// Sphere shape (spec §3.9)
struct SphereShapeDTO {
  double radius_m;  // > 0
};

/// Cylinder shape (spec §3.9)
struct CylinderShapeDTO {
  double radius_m;   // > 0
  double height_m;   // > 0
};

/// Mesh shape (spec §3.9)
struct MeshShapeDTO {
  std::string resource_uri;
  Vector3DTO scale;  // default {1,1,1}
};

/// Collision shape as a tagged union (spec §3.9)
using CollisionShapeDTO = std::variant<BoxShapeDTO, SphereShapeDTO, CylinderShapeDTO, MeshShapeDTO>;

/// Declared support contact for an authored scene object.
///
/// This is a semantic ManyForge policy, not a backend collision-matrix knob:
/// the object may start a release motion in contact with support_object_id, but
/// trajectory validation must prove that the pair separates by the end of the
/// motion and does not re-contact after separation.
struct SupportContactDTO {
  std::string support_object_id;
  CollisionContactPolicyMode mode =
      CollisionContactPolicyMode::kContactRelease;
  bool enforce_max_start_penetration = true;
  double max_start_penetration_m = 0.0;
  double release_tolerance_m = 1e-5;
  double min_release_progress_m = 0.0;
  double min_separation_clearance_m = 0.0;
  bool require_separation_by_end = true;
  bool require_strictly_decreasing_penetration = true;
  bool allow_recontact = false;
};

/// Add object request (spec §3.9)
struct AddObjectRequestDTO {
  std::string object_id;  // unique within the backend-owned scene
  CollisionShapeDTO shape;
  Pose3DTO pose;          // expressed in frame_id
  std::string frame_id;   // reference frame for the object pose
  std::vector<SupportContactDTO> support_contacts;
};

/// Remove object request (spec §3.9)
struct RemoveObjectRequestDTO {
  std::string object_id;
};

/// Update object pose request (spec §3.9)
struct UpdateObjectPoseRequestDTO {
  std::string object_id;
  Pose3DTO pose;          // expressed in frame_id
  std::string frame_id;
  /// Optional observation timestamp in the kernel steady-clock domain.
  ///
  /// Pose-source updates set this together with `source_freshness_budget_ns` so
  /// runtime collision monitoring can fail closed when dynamic object poses go
  /// stale. Authored/manual scene edits leave both fields unset, which clears
  /// any previous source authority for the object.
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;

  /// Optional deployment-authored observed-scene admission policy. These fields
  /// are evidence/configuration carried with a normal revision-visible scene
  /// mutation; the kernel still owns lease validation and dispatch admission.
  std::optional<std::string> scene_admission_role;
  std::optional<std::int64_t> scene_admission_lease_max_age_ns;
  std::optional<double> scene_admission_target_drift_tolerance_m;
  std::optional<double> scene_admission_target_angular_tolerance_rad;
  std::optional<double> scene_admission_obstacle_drift_tolerance_m;
  std::optional<double> scene_admission_obstacle_angular_tolerance_rad;
  bool scene_admission_allow_runtime_dynamic_obstacle = false;
};

/// Attach an existing object to a frame while preserving its current world pose.
struct AttachObjectRequestDTO {
  std::string object_id;
  std::string frame_id;
  std::vector<std::string> touch_links;
};

/// Detach/reparent an existing object into a target frame while preserving world pose.
struct DetachObjectRequestDTO {
  std::string object_id;
  std::string frame_id;
};

/// Object state query result (spec §3.9)
struct ObjectStateDTO {
  std::string object_id;
  bool exists = false;
  bool attached = false;
  std::optional<std::string> frame_id;
};

/// Scene edit result (spec §3.9)
struct SceneEditResultDTO {
  SceneEditResultDTO() = default;
  SceneEditResultDTO(bool ok, std::optional<ErrorDTO> maybe_error = std::nullopt)
      : success(ok), error(std::move(maybe_error)) {}

  bool success = false;
  std::optional<ErrorDTO> error;  // present when success is false
  /// Additive status for successful scene edits. Existing callers should keep
  /// using `success`; high-rate observed-source paths use this to distinguish
  /// real geometry writes from freshness-only/no-op updates.
  std::optional<std::string> update_status;
  bool collision_world_revision_changed = false;
  bool collision_world_freshness_changed = false;
};

}  // namespace manyforge::core
