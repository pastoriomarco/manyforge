#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <manyforge_core/collision/scene_snapshot_lease.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/scene_edit.hpp>

namespace manyforge::core {

using SceneRevisionDTO = CollisionWorldRevision;

struct SceneRevisionBindingDTO {
  SceneRevisionDTO revision;
  std::string projection_lease_id;

  [[nodiscard]] bool valid() const noexcept {
    return revision.revision_id != 0 && !revision.content_hash.empty() &&
           !projection_lease_id.empty();
  }
};

using SceneOperationDTO =
    std::variant<AddObjectRequestDTO, RemoveObjectRequestDTO,
                 UpdateObjectPoseRequestDTO, AttachObjectRequestDTO,
                 DetachObjectRequestDTO, SetRobotBasePoseRequestDTO,
                 SetCollisionPolicyRequestDTO>;

/// Whether a successful logical-scene transaction invalidates non-moving
/// recovery admission. The default is deliberately fail-closed. Trusted
/// perception/pose-source writers may mark a transaction as a live observation
/// so high-rate object/octomap changes remain revision-visible without making
/// an already-stopped recovery offer flicker.
enum class SceneRecoveryAdmissionImpact {
  kInvalidate,
  kLiveObservation,
};

struct SceneTransactionDTO {
  std::string transaction_id;
  std::uint64_t transaction_sequence = 0;
  SceneRevisionDTO expected_revision;
  SceneOperationDTO operation;
  std::string authority_source;
  std::optional<std::string> session_id;
  SceneRecoveryAdmissionImpact recovery_admission_impact =
      SceneRecoveryAdmissionImpact::kInvalidate;
};

enum class SceneUpdateStatus {
  kGeometryChanged,
  kFreshnessOnly,
  kNoOp,
};

inline const char* scene_update_status_name(SceneUpdateStatus status) noexcept {
  switch (status) {
    case SceneUpdateStatus::kGeometryChanged:
      return "geometry_changed";
    case SceneUpdateStatus::kFreshnessOnly:
      return "freshness_only";
    case SceneUpdateStatus::kNoOp:
      return "no_op";
  }
  return "unknown";
}

enum class SceneProjectionHealth {
  kReady,
  kDesynchronized,
};

struct SceneProjectionHealthDTO {
  SceneProjectionHealth health = SceneProjectionHealth::kReady;
  SceneRevisionDTO projected_revision;
  std::string reason;

  [[nodiscard]] bool ready() const noexcept {
    return health == SceneProjectionHealth::kReady;
  }
};

struct SceneObjectSnapshotDTO {
  std::string object_id;
  CollisionShapeDTO shape;
  Pose3DTO pose_in_frame;
  Pose3DTO pose_in_universe;
  std::string frame_id;
  bool attached = false;
  std::vector<std::string> touch_links;
  std::vector<SupportContactDTO> support_contacts;
  std::optional<std::string> source_id;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<std::int64_t> source_freshness_budget_ns;
  std::optional<ObservedSceneObjectRole> scene_admission_role;
  std::optional<std::int64_t> scene_admission_lease_max_age_ns;
  std::optional<double> scene_admission_target_drift_tolerance_m;
  std::optional<double> scene_admission_target_angular_tolerance_rad;
  std::optional<double> scene_admission_obstacle_drift_tolerance_m;
  std::optional<double> scene_admission_obstacle_angular_tolerance_rad;
  bool scene_admission_allow_runtime_dynamic_obstacle = false;
};

struct SceneRobotPlacementDTO {
  std::string robot_id;
  Pose3DTO pose_in_world;
};

struct LogicalSceneSnapshot {
  SceneRevisionDTO revision;
  std::uint64_t transaction_sequence = 0;
  /// Monotonic token for non-moving recovery admission. Structural, policy,
  /// robot-placement, and authored/manual pose changes advance it; continuous
  /// observed-source pose/freshness updates do not.
  std::uint64_t admission_epoch = 0;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::string world_frame = "universe";
  /// Caller-authored policy before projection-derived static/touch/support
  /// allowances are added. Rebuilds must use this configured form so derived
  /// allowances cannot become stale baseline permissions.
  CollisionPolicySnapshot configured_collision_policy;
  std::vector<SceneRobotPlacementDTO> robot_placements;
  std::vector<SceneObjectSnapshotDTO> objects;
};

using LogicalSceneSnapshotPtr = std::shared_ptr<const LogicalSceneSnapshot>;

struct SceneAuthorityStatusDTO {
  SceneRevisionDTO revision;
  std::uint64_t transaction_sequence = 0;
  std::uint64_t admission_epoch = 0;
  std::uint64_t topology_epoch = 0;
  std::uint64_t policy_epoch = 0;
};

struct SceneProjectionCommitResult {
  SceneEditResultDTO edit;
  SceneRevisionDTO acknowledged_revision;
  bool rollback_proven = true;
};

struct SceneCommitResultDTO {
  bool success = false;
  std::optional<ErrorDTO> error;
  std::uint64_t transaction_sequence = 0;
  SceneRevisionDTO committed_revision;
  SceneRevisionDTO projection_revision;
  SceneUpdateStatus update_status = SceneUpdateStatus::kNoOp;
};

}  // namespace manyforge::core
