#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <functional>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/types/collision.hpp>

namespace manyforge::roboplan_backend {

class RoboPlanCollisionBodyIdMapper;


/// Native C++ planning context wrapping RoboPlan Scene.
/// Provides state queries, FK, collision checks, validation, and scene edits.
class RoboPlanPlanningContext final
    : public core::IPlanningContext,
      public core::ICollisionWorldSnapshotPublisher {
 public:
  struct SceneObjectSnapshot {
    std::string object_id;
    core::CollisionShapeDTO shape;
    core::Pose3DTO pose_in_frame;
    core::Pose3DTO pose_in_universe;
    std::string frame_id;
    bool attached = false;
    std::vector<core::SupportContactDTO> support_contacts;
  };

  struct SceneTraceEvent {
    uint64_t timestamp_ns = 0;
    std::string event;
    std::string object_id;
    bool success = false;
    std::optional<std::string> frame_id;
    bool attached = false;
    std::optional<core::Pose3DTO> pose_in_frame;
    std::optional<core::Pose3DTO> pose_in_universe;
    std::optional<std::string> error;
  };

  explicit RoboPlanPlanningContext(std::shared_ptr<roboplan::Scene> scene,
                                  std::string group_name = "",
                                  std::shared_ptr<std::mutex> scene_mutex = nullptr,
                                  std::optional<core::CollisionPolicySnapshot>
                                      initial_policy_snapshot = std::nullopt);
  ~RoboPlanPlanningContext() override;

  core::RobotStateDTO get_current_state() override;

  core::ValidationResultDTO validate_trajectory(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options) override;

  core::Result<core::TrajectoryDTO> expand_trajectory_for_collision_validation(
      const core::TrajectoryDTO& trajectory) override;

  core::CollisionCheckResultDTO collision_check(
      const core::CollisionCheckRequestDTO& request) override;

  core::Pose3DTO forward_kinematics(const core::FkRequestDTO& request) override;
  std::map<std::string, core::Pose3DTO> forward_kinematics_batch(
      const core::RobotStateDTO& robot_state,
      const std::vector<std::string>& target_frames,
      const std::string& base_frame);
  core::Pose3DTO get_object_pose(const std::string& object_id,
                                 const std::string& base_frame) override;
  core::ObjectStateDTO get_object_state(const std::string& object_id) override;
  core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO& reference,
      const std::string& base_frame) override;

  core::JointLimitsDTO get_joint_limits(const std::string& robot_id,
                                        const std::string& group_id) override;

  void set_robot_base_pose(const core::Pose3DTO& pose);

  core::SceneEditResultDTO add_object(const core::AddObjectRequestDTO& request) override;
  core::SceneEditResultDTO remove_object(const core::RemoveObjectRequestDTO& request) override;
  core::SceneEditResultDTO update_object_pose(const core::UpdateObjectPoseRequestDTO& request) override;
  core::SceneEditResultDTO attach_object(const core::AttachObjectRequestDTO& request) override;
  core::SceneEditResultDTO detach_object(const core::DetachObjectRequestDTO& request) override;

  std::vector<std::string> get_scene_object_ids();
  std::vector<std::string> get_joint_names();
  std::vector<SceneObjectSnapshot> get_scene_objects();
  std::vector<SceneTraceEvent> get_scene_trace(size_t limit = 0);
  void clear_scene_trace();

  /// Set the robot id namespace used when constructing normalized
  /// CollisionBodyId values for scene objects. Must be called by the kernel
  /// factory / deployment wiring before collision_check_normalized() is
  /// invoked. Defaults to "robot0" for single-robot v0 deployments.
  void set_collision_mapping_robot_id(const std::string& robot_id);

  /// Normalized collision check (spec §3.8.1). Wraps the existing
  /// collision_check() and constructs a CollisionResult populated from the
  /// legacy result plus body-id normalization via the revision-scoped
  /// mapper.
  ///
  /// Chunk 1 of the collision-ownership plan: kernel still consumes the
  /// legacy CollisionCheckResultDTO; this method exists alongside as the
  /// diagnostic-richer path. The mapper is rebuilt lazily when the scene
  /// state relevant to body identity changes.
  ///
  /// The Chunk 1 mapper covers authored scene objects plus raw robot collision
  /// geometry names discovered from the RoboPlan/Pinocchio collision model.
  /// Unknown raw names produce an empty normalized_pairs vector and
  /// engine_status = kDiagnosticIncomplete. The legacy contact_body_a/b fields
  /// preserve the raw names for debugging.
  core::CollisionResult collision_check_normalized(
      const core::CollisionQuery& query);

  /// Install (or replace) the immutable collision policy snapshot used by
  /// collision_check_normalized to classify normalized pairs and produce a
  /// CollisionDecision (Chunk 3 of the collision-ownership plan).
  /// In Chunks 3-4 this stores only the immutable snapshot. The actual
  /// evaluator instance is created per normalized query so support-contact
  /// state cannot leak across independent diagnostics; the Chunk 5 validation
  /// coordinator will own trajectory-scoped evaluator state.
  void set_collision_policy_snapshot(core::CollisionPolicySnapshot snapshot);

  /// Install a secondary collision query engine to run in diagnostic
  /// compare mode alongside the legacy RoboPlan path. The engine is accepted
  /// through the backend-agnostic ICollisionQueryEngine port; RoboPlan does
  /// not depend on the concrete ManyForge collision engine class.
  ///
  /// Passing `nullptr` disables compare mode.
  void set_collision_engine_for_compare(
      std::shared_ptr<core::ICollisionQueryEngine> engine);

  /// Register a structured-record emitter that receives canonical
  /// `CollisionEngineMismatchRecord` values (Chunk 8 groundwork of the
  /// collision-ownership plan). Deployment / kernel wiring typically points
  /// this at a CollisionEngineMismatchLedger so qualification harnesses can
  /// inspect every mismatch with the schema required by spec
  /// §"Engine Capability And Mismatch Policy". Pass an empty function to clear.
  using MismatchRecordEmitter =
      std::function<void(core::CollisionEngineMismatchRecord)>;
  void set_engine_mismatch_record_emitter(MismatchRecordEmitter emitter);

  /// Compatibility hook for the future Chunk 5 validation coordinator. With the
  /// current per-query evaluator integration there is no planning-context-local
  /// state to reset.
  void reset_collision_policy_state();

  /// Current authoritative collision-world revision (spec/210 §3.8.1,
  /// COLLISION_CHECKING_OWNERSHIP_PLAN Chunk 2). Replaces the Chunk 1
  /// adapter-local diagnostic accessor. Now backed by an in-context
  /// publisher that bumps on every successful scene mutation; the body-id
  /// mapper derives its revision from this publisher.
  ///
  /// Overrides IPlanningContext::current_collision_world_revision().
  core::CollisionWorldRevision current_collision_world_revision() override;

  /// Recent collision-world ledger entries (spec/210 §3.8.1). Bounded
  /// ring buffer; `limit == 0` returns every retained entry, otherwise
  /// the most recent `limit` entries in newest-first order. Used by
  /// failure archives and audit tooling to reconstruct the chain of
  /// mutations that produced a given world state.
  std::vector<core::CollisionWorldLedgerEntry>
  recent_collision_world_ledger(std::size_t limit = 0) const;

  /// Current immutable collision-world snapshot (kernel-architecture sprint
  /// Chunk 2A). Built from the authoritative planning-context scene state and
  /// cached by collision-world revision. Returns nullptr only when the
  /// snapshot builder rejects the current world, which callers must treat as a
  /// fail-closed diagnostic condition.
  core::CollisionWorldSnapshotPtr snapshot() const override;

  /// Leaf revision probe for runtime monitors. Updated atomically with scene
  /// mutation publication but readable without borrowing scene_mutex_.
  core::CollisionWorldRevision published_collision_world_revision()
      const override;

  /// Current policy overlay paired with the collision-world snapshot.
  ///
  /// The snapshot is rebuilt from current scene-object state on every call so
  /// attach/detach, static object filtering, touch-link allowances, and
  /// support-contact discovery are borrowed atomically with the active world
  /// revision used by the kernel validation pass.
  std::optional<core::CollisionPolicySnapshot>
  collision_policy_snapshot() const override;

  core::CollisionWorldPolicySnapshotBundle
  collision_world_policy_snapshot_bundle() const override;

  std::optional<core::CollisionWorldLeaseInventory>
  current_scene_snapshot_lease_inventory() const override;

  std::optional<core::CollisionWorldLeaseInventory>
  retain_scene_snapshot_lease_inventory(std::string retention_id = {}) override;

  void release_scene_snapshot_lease_inventory(
      const std::string& retention_id) override;

  core::CollisionWorldLeaseRetentionStats
  scene_snapshot_lease_retention_stats() const override;

  core::CollisionWorldPolicySnapshotBundle
  published_runtime_collision_snapshot_bundle() const override;

 private:
  struct TrackedObjectState {
    core::Pose3DTO pose;
    core::CollisionShapeDTO shape;
    std::string frame_id;
    bool attached = false;
    std::vector<std::string> touch_links;
    std::vector<core::SupportContactDTO> support_contacts;
    std::optional<std::string> source_id;
    std::optional<std::int64_t> source_timestamp_ns;
    std::optional<std::int64_t> source_freshness_budget_ns;
    std::optional<core::ObservedSceneObjectRole> scene_admission_role;
    std::optional<std::int64_t> scene_admission_lease_max_age_ns;
    std::optional<double> scene_admission_target_drift_tolerance_m;
    std::optional<double> scene_admission_target_angular_tolerance_rad;
    std::optional<double> scene_admission_obstacle_drift_tolerance_m;
    std::optional<double> scene_admission_obstacle_angular_tolerance_rad;
    bool scene_admission_allow_runtime_dynamic_obstacle = false;
  };

  struct CollisionPairDiagnostic {
    std::string body_a;
    std::string body_b;
    std::string text;
    std::pair<std::string, std::string> canonical_pair;
    std::optional<std::pair<std::string, std::string>> canonical_object_pair;
  };

  struct CollisionDiagnostics {
    std::vector<CollisionPairDiagnostic> pairs;
    std::string raw_output;
  };

  Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose) const;
  core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix) const;
  bool is_world_frame(const std::string& frame_id) const;
  Eigen::Matrix4d scene_placement_from_tracked_locked(
      const TrackedObjectState& state) const;
  Eigen::Matrix4d parent_joint_from_frame_matrix_locked(
      const std::string& frame_id) const;
  Eigen::Matrix4d frame_to_world_matrix_locked(const std::string& frame_id) const;
  core::Pose3DTO pose_in_base_from_tracked_locked(const TrackedObjectState& state,
                                                  const std::string& base_frame) const;
  bool infer_attached_from_frame(const std::string& frame_id) const;
  std::vector<std::string> normalize_touch_links_locked(
      const std::vector<std::string>& touch_links) const;
  tl::expected<void, std::string> set_touch_links_locked(
      const std::string& object_id,
      const std::vector<std::string>& touch_links,
      bool enable);
  tl::expected<void, std::string> clear_touch_links_locked(
      const std::string& object_id,
      TrackedObjectState& tracked);
  std::pair<std::string, std::string> canonical_collision_pair(
      const std::string& body1,
      const std::string& body2) const;
  std::string collision_pair_text(
      const std::pair<std::string, std::string>& pair) const;
  std::optional<std::pair<std::string, std::string>>
  canonical_object_collision_pair_locked(
      const std::string& body1,
      const std::string& body2,
      const RoboPlanCollisionBodyIdMapper& diagnostic_mapper) const;
  CollisionDiagnostics collect_collision_diagnostics_locked(
      const Eigen::VectorXd& q);
  void add_collision_details(
      std::map<std::string, std::string>& details,
      const CollisionDiagnostics& diagnostics) const;
  tl::expected<void, std::string> apply_collision_pair_projection_locked(
      const std::pair<std::string, std::string>& pair);
  tl::expected<void, std::string> apply_static_collision_policy_for_object_locked(
      const std::string& object_id);
  void erase_static_collision_policy_for_object_locked(const std::string& object_id);
  tl::expected<void, std::string> recompute_support_contact_policy_locked();
  tl::expected<void, std::string> set_support_contact_pairs_for_validation_locked(
      bool enable);
  std::set<std::pair<std::string, std::string>>
  active_support_contact_pairs_for_validation_locked() const;
  bool is_active_support_contact_pair_locked(
      const std::pair<std::string, std::string>& pair) const;
  bool is_support_contact_pair_locked(
      const std::pair<std::string, std::string>& pair) const;
  std::optional<core::SupportContactDTO> support_contact_policy_for_pair_locked(
      const std::pair<std::string, std::string>& pair) const;
  core::SceneEditResultDTO reparent_object_locked(const std::string& object_id,
                                                  const std::string& target_frame_id,
                                                  bool emit_event = true);
  void record_scene_event_locked(SceneTraceEvent event);
  std::vector<SceneObjectSnapshot> scene_objects_locked();
  std::vector<std::pair<std::string, core::CollisionBodyId>>
  robot_body_mappings_locked(const std::string& robot_id);
  std::vector<core::CollisionWorldBody>
  robot_snapshot_bodies_locked(const std::string& robot_id) const;
  std::vector<core::CollisionWorldBody>
  object_snapshot_bodies_locked(const std::string& robot_id) const;
  std::string robot_model_frame_id_locked() const;
  Eigen::Matrix4d robot_model_frame_world_matrix_locked(
      const std::string& model_frame_id) const;
  core::CollisionGeometryDescriptor geometry_descriptor_for_shape_locked(
      const core::CollisionBodyId& body_id,
      core::CollisionOwnerKind owner_kind,
      const core::CollisionShapeDTO& shape) const;
  core::CollisionWorldSnapshotPtr build_collision_world_snapshot_locked() const;
  core::CollisionWorldPolicySnapshotBundle
  current_collision_world_policy_snapshot_bundle_locked(
      const std::string& robot_id) const;
  core::CollisionWorldLeaseInventory
  build_scene_snapshot_lease_inventory_locked(
      const std::string& robot_id,
      std::string retention_id,
      std::int64_t captured_at_ns) const;
  std::vector<core::SceneSnapshotLiveObject>
  scene_snapshot_live_objects_locked(const std::string& robot_id) const;
  std::string object_revision_token_locked(
      const std::string& object_id,
      const TrackedObjectState& state,
      const std::string& robot_id) const;
  void prune_scene_snapshot_lease_retention_locked(std::int64_t now_ns);
  std::vector<std::string> object_body_ids_for_state_locked(
      const std::string& object_id,
      const TrackedObjectState& state) const;
  std::vector<std::string> object_body_ids_for_transition_locked(
      const std::string& object_id,
      const TrackedObjectState& before,
      const TrackedObjectState& after) const;
  std::string compute_collision_world_content_hash_locked(
      const std::string& robot_id);
  core::CollisionPolicySnapshot build_collision_policy_snapshot_locked(
      const std::string& robot_id) const;

  tl::expected<void, std::string> add_geometry_locked(
      const std::string& object_id, const TrackedObjectState& state);

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, TrackedObjectState> object_states_;
  mutable std::map<std::string, std::pair<std::string, std::string>>
      object_body_hash_cache_;
  Eigen::Matrix4d world_from_robot_base_ = Eigen::Matrix4d::Identity();
  std::set<std::pair<std::string, std::string>> disabled_static_collision_pairs_;
  std::set<std::pair<std::string, std::string>> disabled_support_contact_pairs_;
  std::deque<SceneTraceEvent> scene_trace_;

  // Collision body-id mapper (Chunk 1 of the collision-ownership plan).
  // Lazily rebuilt when the scene fingerprint changes; protected by its
  // own mutex so multiple collision_check_normalized callers can be
  // serialized without contending on scene_mutex_.
  mutable std::mutex mapper_mutex_;
  std::string mapper_robot_id_ = "robot0";
  std::string mapper_scene_fingerprint_;
  std::unique_ptr<RoboPlanCollisionBodyIdMapper> mapper_;

  // Immutable policy snapshot (Chunk 3). Empty until set_collision_policy_snapshot
  // is called; collision_check_normalized copies it under evaluator_mutex_ and
  // uses a query-local CollisionPolicyEvaluator. This deliberately avoids
  // planning-context-global support-contact state until Chunk 5 introduces a
  // validation-scoped coordinator.
  mutable std::mutex evaluator_mutex_;
  std::optional<core::CollisionPolicySnapshot> policy_snapshot_;

  // Secondary collision engine compare mode. Shared pointer so the comparator
  // lifetime can outlive a single planning context; nullptr disables compare
  // mode. Emitter receives mismatch records when both engines run.
  mutable std::mutex compare_engine_mutex_;
  std::shared_ptr<core::ICollisionQueryEngine> compare_engine_;
  MismatchRecordEmitter mismatch_record_emitter_;

  // Collision-world revision publisher (Chunk 2). Bumped on every successful
  // scene mutation; the ledger records each transition. Owned under
  // scene_mutex_ so mutation success and publish are atomic with the
  // backend write that caused them.
  std::uint64_t world_revision_id_ = 0;
  std::string world_content_hash_;
  std::uint64_t topology_epoch_ = 0;
  std::uint64_t policy_epoch_ = 0;
  std::deque<core::CollisionWorldLedgerEntry> world_ledger_;
  static constexpr std::size_t kCollisionWorldLedgerCapacity = 32;
  mutable std::mutex published_revision_mutex_;
  core::CollisionWorldRevision published_revision_;

  // Publish-on-write runtime monitor bundle. Writers update this cache while
  // holding scene_mutex_; runtime ticks read it through this leaf mutex only.
  mutable std::mutex published_runtime_bundle_mutex_;
  core::CollisionWorldPolicySnapshotBundle published_runtime_bundle_;

  mutable core::CollisionWorldSnapshotPtr snapshot_cache_;
  mutable std::uint64_t snapshot_cache_revision_id_ = 0;
  mutable std::string snapshot_cache_content_hash_;

  mutable std::mutex retained_lease_inventory_mutex_;
  std::deque<core::CollisionWorldLeaseInventory> retained_lease_inventories_;
  std::uint64_t next_lease_retention_sequence_ = 0;
  static constexpr std::size_t kSceneSnapshotLeaseRetentionCapacity = 8;
  static constexpr std::int64_t kSceneSnapshotLeaseRetentionMaxAgeNs =
      30'000'000'000LL;

  /// Bump the authoritative collision-world revision after a successful
  /// scene mutation. Must be called under scene_mutex_.
  void publish_collision_world_revision_locked(
      core::CollisionWorldMutationSource mutation_source,
      const std::vector<std::string>& affected_body_ids,
      std::string authority_source,
      std::optional<std::string> session_id = std::nullopt);
};

}  // namespace manyforge::roboplan_backend
