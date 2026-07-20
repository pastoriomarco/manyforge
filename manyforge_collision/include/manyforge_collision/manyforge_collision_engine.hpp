#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <pinocchio/multibody/geometry.hpp>
#include <pinocchio/multibody/model.hpp>

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/types/collision.hpp>

namespace manyforge::collision {

namespace detail {

/// Aggregate selected colliding geometry-pair distance evidence for one
/// logical pair. One non-finite component permanently makes the aggregate
/// incomplete; a finite sibling cannot restore a valid deepest-distance proof.
struct SelectedCollidingDistanceEvidence {
  bool complete = true;
  std::optional<double> deepest_finite_m;

  void observe(double signed_distance_m) noexcept {
    if (!std::isfinite(signed_distance_m)) {
      complete = false;
      return;
    }
    if (!deepest_finite_m || signed_distance_m < *deepest_finite_m) {
      deepest_finite_m = signed_distance_m;
    }
  }
};

}  // namespace detail

/// ManyForge-owned scene-aware collision engine.
///
/// The engine consumes immutable CollisionWorldSnapshot values through the
/// backend-agnostic ICollisionQueryEngine port. It owns its Pinocchio model,
/// GeometryModel, GeometryData, and contact buffers; no query touches
/// RoboPlan planner-context APIs, RoboPlan scene locks, or adapter state.
///
/// Primitive robot proxies, multi-primitive bodies, and authored scene triangle
/// meshes are supported in the current production profile. Undecomposed
/// compound and convex-mesh geometry kinds still fail closed until the snapshot
/// contract carries a qualified decomposition.
class ManyForgeCollisionEngine final : public core::ICollisionQueryEngine {
 public:
  /// Direct mechanism counters for the most recent query executed by this
  /// mutable engine context. These distinguish complete collision enumeration
  /// from the narrower optional signed-distance work. They are intended for
  /// focused qualification and regression tests, not policy decisions.
  struct QueryMechanismCounters {
    core::CollisionDistanceRequestMode distance_request_mode =
        core::CollisionDistanceRequestMode::kNone;
    std::uint64_t collision_api_call_count = 0;
    /// Phase ordinals are zero when absent. A successful collision pass is 1;
    /// the first optional distance evaluation must therefore be 2.
    std::uint64_t collision_enumeration_completion_ordinal = 0;
    std::uint64_t active_collision_pair_count = 0;
    std::uint64_t colliding_geometry_pair_count = 0;
    std::uint64_t colliding_logical_pair_count = 0;
    std::uint64_t distance_api_call_count = 0;
    std::uint64_t first_distance_evaluation_ordinal = 0;
    std::uint64_t distance_evaluated_geometry_pair_count = 0;
    std::uint64_t distance_evaluated_logical_pair_count = 0;
    std::uint64_t fk_placement_invocation_count = 0;
  };

  /// Immutable robot-model assets shared by all collision contexts created by
  /// one kernel factory. The concrete type is intentionally opaque: callers
  /// may share ownership but cannot mutate model state.
  struct SharedModel;

  /// Revision-preparation domain. Workers in one non-real-time pool share a
  /// cache; the immediate runtime monitor receives a separate cache so lower-
  /// priority preparation can never block its safety context.
  struct SharedSceneCache;

  using SharedModelPtr = std::shared_ptr<const SharedModel>;
  using SharedSceneCachePtr = std::shared_ptr<SharedSceneCache>;

  /// Parse URDF/SRDF and load immutable Pinocchio model assets once.
  static SharedModelPtr load_shared_model(
      const std::string& model_name,
      const std::filesystem::path& urdf_path,
      const std::filesystem::path& srdf_path,
      const std::vector<std::filesystem::path>& package_paths,
      std::string group_name);

  /// Create an independently-owned revision cache over a shared model.
  static SharedSceneCachePtr make_shared_scene_cache(
      SharedModelPtr shared_model);

  /// Construct an independent collision query engine from a ManyForge-owned
  /// model descriptor: URDF/SRDF paths, package-resolution paths, and the
  /// planning group used for compact RobotStateDTO vectors.
  ManyForgeCollisionEngine(
      const std::string& model_name,
      const std::filesystem::path& urdf_path,
      const std::filesystem::path& srdf_path,
      const std::vector<std::filesystem::path>& package_paths,
      std::string group_name);

  /// Construct one mutable query context over shared immutable assets.
  ManyForgeCollisionEngine(SharedModelPtr shared_model,
                           SharedSceneCachePtr scene_cache);

  core::CollisionResult check(
      const core::CollisionWorldSnapshot& snapshot,
      const core::CollisionQuery& query,
      const core::CollisionPolicySnapshot& policy,
      const core::CollisionCancellationToken& cancel) override;

  core::RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> canonical_joint_names) noexcept override;
  core::RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const core::CollisionWorldSnapshot& snapshot) noexcept override;
  core::RuntimeCollisionQueryResultView check_runtime(
      const core::CollisionWorldSnapshot& snapshot,
      const core::RuntimeCollisionQueryRequest& request) noexcept override;

  const core::CollisionEngineCapabilityDescriptor& capability()
      const override {
    return capability_;
  }

  /// Engine identity for traceability + mismatch records. Stable for
  /// the lifetime of the engine.
  std::string engine_id() const override { return engine_id_; }
  std::string engine_version() const override { return engine_version_; }

  /// Test helpers for ownership and cache-reuse regressions.
  const void* internal_model_identity() const;
  const void* internal_scene_cache_identity() const;
  const void* internal_prepared_scene_identity() const;
  std::uint64_t internal_prepared_scene_build_count() const;
  QueryMechanismCounters internal_last_query_mechanism_counters() const;

 private:
  struct PreparedScene;

  SharedModelPtr shared_model_;
  SharedSceneCachePtr scene_cache_;
  std::string engine_id_;
  std::string engine_version_;
  core::CollisionEngineCapabilityDescriptor capability_;

  std::shared_ptr<const PreparedScene> active_prepared_scene_;
  std::optional<pinocchio::GeometryData> active_collision_data_;
  /// Reusable FK scratch for collision queries: constructed once per
  /// engine (the kinematic model never changes after construction);
  /// pinocchio writes per-query results into it. Accessed only under
  /// scene_mutex_ like every other check-path member — the engine is
  /// kSingleContextExclusive by contract.
  std::optional<pinocchio::Data> reusable_model_data_;

  std::vector<std::size_t> runtime_joint_q_indices_;
  Eigen::VectorXd runtime_q_neutral_;
  Eigen::VectorXd runtime_q_scratch_;
  bool runtime_query_prepared_ = false;
  bool runtime_query_is_full_configuration_ = false;
  std::vector<core::RuntimeCollisionPairObservation>
      runtime_collision_observations_;
  std::vector<core::RuntimeCollisionPairObservation>
      runtime_clearance_observations_;
  QueryMechanismCounters last_query_mechanism_counters_;

  /// Internal mutex serializes concurrent check() calls on the same
  /// instance. Separate instances do not share this mutex.
  mutable std::mutex scene_mutex_;

  std::optional<std::string> sync_snapshot_locked(
      const core::CollisionWorldSnapshot& snapshot,
      bool allow_resource_io = true);
  core::CollisionResult check_locked(
      const core::CollisionQuery& query,
      const core::CollisionWorldSnapshot* snapshot,
      const core::CollisionCancellationToken* cancel);
  std::optional<Eigen::VectorXd> full_configuration_from_query(
      const core::CollisionQuery& query,
      std::string& error) const;
};

}  // namespace manyforge::collision
