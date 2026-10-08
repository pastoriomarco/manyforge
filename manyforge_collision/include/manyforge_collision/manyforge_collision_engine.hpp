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
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::collision {

/// Compiler-owned identity for one logical component/model collision body.
///
/// The caller supplies only retained semantic coordinates and the exact
/// compiled frame selected by the retained symbol/role map. The collision
/// package resolves the physical geometry from its already-loaded immutable
/// model; callers cannot inject replacement robot geometry through this
/// boundary.
struct RetainedComponentCollisionBodyProjectionInput final {
  core::ControllableComponentId controllable_component_id;
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  core::CollisionBodyRoleId collision_body_role_id;
  std::size_t collision_index = 0U;
  std::string compiled_frame_name;
};

/// One compiler-resolved component frame used to bind the flattened retained
/// model to the semantic cell collision-world frame.
struct RetainedComponentModelFrameProjectionInput final {
  core::ControllableComponentId controllable_component_id;
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  core::FrameRoleId frame_role_id;
  std::string compiled_frame_name;
};

/// Immutable component-model half of a collision-world snapshot.
struct RetainedComponentCollisionProjection final {
  std::vector<core::CollisionComponentModelFrame> component_model_frames;
  std::vector<core::CollisionWorldBody> component_bodies;
};

/// Exact fixed-frame bridge used to publish one carried scene-resource body.
///
/// The public parent is a stable component/model collision-body identity. The
/// pose bridge converts a scene-owner-issued carrier-frame-relative pose into
/// that body's compiled frame without exposing a backend geometry name.
struct RetainedCarriedObjectParentProjection final {
  core::CollisionBodyId parent_body_id;
  std::string compiled_parent_frame_name;
  core::Pose3DTO parent_frame_from_carrier;
};

/// One exact scalar compiled-model coordinate supplied to immutable FK.
struct RetainedKinematicCoordinateValue final {
  std::string compiled_joint_name;
  double position_si{};
};

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
/// Exact retained component geometry, multi-primitive logical bodies, and
/// authored scene triangle meshes are supported in the current production
/// profile. Component bodies map stable role identities onto the retained
/// Pinocchio geometry; they are never reconstructed as proxy shapes.
/// Undecomposed compound and convex-mesh geometry kinds still fail closed until
/// the snapshot contract carries a qualified decomposition.
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

  // Cold loading has an N*N geometry-index matrix before SRDF pruning. This
  // separate envelope is not the runtime logical-body or active-pair budget.
  static constexpr std::size_t kMaximumRetainedCollisionGeometries = 1024;

  /// Parse URDF/SRDF and load immutable Pinocchio model assets once.
  ///
  /// `scene_asset_root_fd` is the approved scene-asset root directory
  /// descriptor. The model keeps its own duplicate for its lifetime. Relative
  /// scene mesh URIs resolve only below it; without it they fail closed.
  static SharedModelPtr load_shared_model(
      const std::string& model_name,
      const std::filesystem::path& urdf_path,
      const std::filesystem::path& srdf_path,
      const std::vector<std::filesystem::path>& package_paths,
      std::string group_name,
      std::optional<int> scene_asset_root_fd = std::nullopt);

  /// Content digest (`sha256:<hex>`) of the bytes this model's collision
  /// contexts use for one relative scene mesh URI. The first read of a URI
  /// below the approved root, by a collision load or by this query, pins its
  /// digest for the model's lifetime; every later collision load of that URI
  /// must read identical bytes or fails with reason
  /// `scene_mesh_content_changed`. Errors carry details["reason"].
  [[nodiscard]] static core::Result<std::string> scene_mesh_content_digest(
      const SharedModelPtr& shared_model, const std::string& relative_uri);

  /// Create an independently-owned revision cache over a shared model.
  static SharedSceneCachePtr make_shared_scene_cache(
      SharedModelPtr shared_model);

  /// Project the exact retained native collision inventory into stable public
  /// component/model/role identities.
  ///
  /// `compiled_cell_frame_name` is the compiler-owned frame representing the
  /// semantic cell world. Every native geometry must resolve to exactly one
  /// supplied body row and every component/model owning collision geometry
  /// must have at least one supplied frame row. Missing, duplicate, ambiguous,
  /// or foreign mappings fail closed.
  [[nodiscard]] static core::Result<RetainedComponentCollisionProjection>
  project_retained_component_collision(
      SharedModelPtr shared_model,
      const std::string& compiled_cell_frame_name,
      std::span<const RetainedComponentCollisionBodyProjectionInput> bodies,
      std::span<const RetainedComponentModelFrameProjectionInput> frames);

  /// Resolve one portable carrier frame to a rigidly-related collision body
  /// selected by the addressed command target. An exact carrier-frame body
  /// wins over rigid sibling bodies; overlapping semantic aliases of the same
  /// compiled frame collapse deterministically. Distinct sibling frames and
  /// articulated relationships remain ambiguous and fail closed.
  [[nodiscard]] static core::Result<RetainedCarriedObjectParentProjection>
  resolve_retained_carried_object_parent(
      SharedModelPtr shared_model,
      const std::string& compiled_carrier_frame_name,
      std::span<const RetainedComponentCollisionBodyProjectionInput>
          target_bodies);

  /// Evaluate one compiler-resolved frame against a fresh neutral scratch
  /// configuration plus exact supplied scalar coordinates. No mutable planner
  /// or collision-engine state is read or changed.
  [[nodiscard]] static core::Result<core::Pose3DTO> resolve_retained_frame_pose(
      SharedModelPtr shared_model, const std::string& compiled_cell_frame_name,
      const std::string& compiled_target_frame_name,
      std::span<const RetainedKinematicCoordinateValue> coordinates);

  /// Read exact scalar joint values from the immutable model's neutral
  /// configuration, preserving caller order.
  [[nodiscard]] static core::Result<std::vector<double>> retained_neutral_scalar_joint_positions(
      SharedModelPtr shared_model, std::span<const std::string> compiled_joint_names);

  /// Test whether one compiler-produced frame name exists in the immutable
  /// retained model without exposing the model implementation to adapters.
  [[nodiscard]] static bool retained_frame_exists(SharedModelPtr shared_model,
                                                  const std::string& compiled_frame_name) noexcept;

  /// Select component collision bodies that are rigid with respect to every
  /// compiler-declared commandable model coordinate in the cell.
  [[nodiscard]] static core::Result<std::vector<core::CollisionBodyId>>
  select_retained_static_component_bodies(
      SharedModelPtr shared_model,
      std::span<const RetainedComponentCollisionBodyProjectionInput> bodies,
      std::span<const std::string> dynamic_compiled_joint_names);

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
      const core::CollisionCancellationToken* cancel,
      const Eigen::VectorXd& configuration);
  /// Map one query state onto this model by joint name. Equal counts never
  /// establish coordinate identity; a refusal sets a stable reason code.
  std::optional<Eigen::VectorXd> full_configuration_from_query(
      const core::CollisionQuery& query,
      std::string& error,
      std::string& reason) const;
};

}  // namespace manyforge::collision
