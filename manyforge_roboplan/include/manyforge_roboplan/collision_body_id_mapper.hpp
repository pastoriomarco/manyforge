#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_roboplan/roboplan_projection_backend.hpp>

namespace manyforge::roboplan_backend {

/// Revision-scoped raw-name to CollisionBodyId mapper for the RoboPlan adapter.
///
/// The mapper is a property of one CollisionWorldSnapshot revision (per the
/// collision-ownership plan), not a process-lifetime singleton. It is rebuilt
/// atomically whenever the scene state relevant to body identity changes
/// (scene edit, attach/detach, pose-source apply that mutates the planning
/// collision scene, deployment reload).
///
/// Chunk 1 scope: maps known scene-object raw names (the object_id strings
/// recorded in RoboPlan's Scene by add/attach/detach) and raw robot collision
/// names discovered from RoboPlan's Pinocchio collision model. Unknown raw
/// names still return nullopt and the wrapping layer reports
/// `engine_status = kDiagnosticIncomplete` on the new CollisionResult while
/// preserving the raw names in the legacy CollisionCheckResultDTO. The kernel
/// continues to consume the legacy DTO in Chunk 1, so existing pass/fail
/// behavior is unchanged.
///
/// This mapper owns a collision-mapping revision only. It is not the full
/// collision-world revision from Chunk 2's ledger.
class RoboPlanCollisionBodyIdMapper {
 public:
  RoboPlanCollisionBodyIdMapper();

  /// Rebuild the mapping from the current scene state. Bumps the revision
  /// counter and recomputes the content hash. Lookup state is replaced
  /// atomically.
  ///
  /// `robot_id` is the deployment-configured robot identifier. For multi-
  /// robot deployments the caller invokes rebuild with the active robot
  /// scope (the collision-world snapshot in Chunk 1 is cell-scoped only
  /// implicitly; the explicit cell scope lands in Chunk 2).
  void rebuild(
      const std::string& robot_id,
      const std::vector<RoboPlanProjectionBackend::SceneObjectSnapshot>&
          scene_objects,
      const std::vector<std::pair<std::string, core::CollisionBodyId>>&
          robot_body_mappings);

  /// O(1) lookup. Returns nullopt when the raw name is not in the current
  /// revision's known set. Callers MUST treat nullopt as
  /// `engine_status = kDiagnosticIncomplete`.
  std::optional<core::CollisionBodyId> lookup(
      const std::string& raw_name) const;

  /// Returns the current collision-mapping revision (revision_id +
  /// content_hash). Always valid; pre-rebuild the revision_id is 0 and the
  /// content_hash is the empty string. This is diagnostic adapter state, not
  /// the authoritative collision-world revision.
  core::CollisionWorldRevision current_revision() const;

  /// Format helpers — pure functions, exposed for tests and external use
  /// (e.g. the visualizer that wants to render a body by its normalized id).

  static core::CollisionBodyId make_robot_link_id(const std::string& robot_id,
                                                  const std::string& link_name,
                                                  int index);
  static core::CollisionBodyId make_attached_object_id(
      const std::string& robot_id,
      const std::string& object_id,
      int shape_index);
  static core::CollisionBodyId make_detached_object_id(
      const std::string& object_id, int shape_index);
  static core::CollisionBodyId make_fixture_id(const std::string& fixture_id,
                                               int shape_index);

 private:
  std::unordered_map<std::string, core::CollisionBodyId> raw_to_id_;
  core::CollisionWorldRevision revision_{};
};

}  // namespace manyforge::roboplan_backend
