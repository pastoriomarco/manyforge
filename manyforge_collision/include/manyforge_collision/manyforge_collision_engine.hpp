#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <pinocchio/multibody/geometry.hpp>
#include <pinocchio/multibody/model.hpp>

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/types/collision.hpp>

namespace manyforge::collision {

/// ManyForge-owned scene-aware collision engine.
///
/// The engine consumes immutable CollisionWorldSnapshot values through the
/// backend-agnostic ICollisionQueryEngine port. It owns its Pinocchio model,
/// GeometryModel, GeometryData, and contact buffers; no query touches
/// RoboPlan planner-context APIs, RoboPlan scene locks, or adapter state.
///
/// Primitive robot proxies plus authored scene triangle meshes are supported
/// in the current production profile. Compounds and convex meshes still fail
/// closed until the snapshot contract carries a qualified decomposition.
class ManyForgeCollisionEngine final : public core::ICollisionQueryEngine {
 public:
  /// Construct an independent collision query engine from a ManyForge-owned
  /// model descriptor: URDF/SRDF paths, package-resolution paths, and the
  /// planning group used for compact RobotStateDTO vectors.
  ManyForgeCollisionEngine(
      const std::string& model_name,
      const std::filesystem::path& urdf_path,
      const std::filesystem::path& srdf_path,
      const std::vector<std::filesystem::path>& package_paths,
      std::string group_name);

  core::CollisionResult check(
      const core::CollisionWorldSnapshot& snapshot,
      const core::CollisionQuery& query,
      const core::CollisionPolicySnapshot& policy,
      const core::CollisionCancellationToken& cancel) override;

  const core::CollisionEngineCapabilityDescriptor& capability()
      const override {
    return capability_;
  }

  /// Engine identity for traceability + mismatch records. Stable for
  /// the lifetime of the engine.
  std::string engine_id() const override { return engine_id_; }
  std::string engine_version() const override { return engine_version_; }

  /// Test helper: returns a stable pointer to this engine's immutable
  /// Pinocchio model. Used by integration tests to assert worker instances do
  /// not share mutable planner state.
  const void* internal_model_identity() const {
    return static_cast<const void*>(&model_);
  }

 private:
  pinocchio::Model model_;
  pinocchio::GeometryModel base_collision_model_;
  std::vector<std::filesystem::path> package_paths_;
  std::vector<std::string> group_joint_names_;
  std::string group_name_;
  std::string engine_id_;
  std::string engine_version_;
  core::CollisionEngineCapabilityDescriptor capability_;

  std::optional<core::CollisionWorldRevision> synced_snapshot_revision_;
  std::vector<std::string> synced_scene_geometry_names_;
  std::map<std::string, core::CollisionBodyId> synced_raw_name_to_body_id_;
  /// Full raw-geometry-name -> body-id map (scene + robot links), rebuilt
  /// once per synced snapshot revision instead of parsed on every check.
  std::map<std::string, core::CollisionBodyId> synced_raw_to_body_map_;
  std::set<std::string> builtin_collision_geometry_names_;
  std::optional<pinocchio::GeometryModel> active_collision_model_;
  std::optional<pinocchio::GeometryData> active_collision_data_;
  /// Reusable FK scratch for collision queries: constructed once per
  /// engine (the kinematic model never changes after construction);
  /// pinocchio writes per-query results into it. Accessed only under
  /// scene_mutex_ like every other check-path member — the engine is
  /// kSingleContextExclusive by contract.
  std::optional<pinocchio::Data> reusable_model_data_;

  /// Internal mutex serializes concurrent check() calls on the same
  /// instance. Separate instances do not share this mutex.
  mutable std::mutex scene_mutex_;

  std::optional<std::string> sync_snapshot_locked(
      const core::CollisionWorldSnapshot& snapshot);
  core::CollisionResult check_locked(
      const core::CollisionQuery& query,
      const core::CollisionWorldSnapshot* snapshot,
      const core::CollisionCancellationToken* cancel);
  std::optional<Eigen::VectorXd> full_configuration_from_query(
      const core::CollisionQuery& query,
      std::string& error) const;
};

}  // namespace manyforge::collision
