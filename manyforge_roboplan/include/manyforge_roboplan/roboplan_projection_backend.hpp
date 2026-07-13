#pragma once

#include <array>
#include <atomic>
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
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/scene_authority.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/validation.hpp>

namespace manyforge::roboplan_backend {

class RoboPlanCollisionBodyIdMapper;
class RoboPlanCollisionDiagnosticComparator;


/// Backend-internal native scene projection and collision-publication owner.
///
/// This is not a ManyForge domain port. Logical edits arrive only through
/// `RoboPlanSceneProjection`; model, observed-state, validation, and dispatch
/// authority live behind their focused interfaces.
class RoboPlanProjectionBackend final
    : public core::ICollisionWorldSnapshotPublisher {
 public:
  struct SceneObjectSnapshot {
    std::string object_id;
    core::CollisionShapeDTO shape;
    core::Pose3DTO pose_in_frame;
    core::Pose3DTO pose_in_universe;
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

  explicit RoboPlanProjectionBackend(
      std::shared_ptr<roboplan::Scene> scene,
      std::string group_name = "",
      std::shared_ptr<std::mutex> scene_mutex = nullptr,
      std::optional<core::CollisionPolicySnapshot> initial_policy_snapshot =
          std::nullopt,
      std::string robot_id = "robot0");
  ~RoboPlanProjectionBackend() override;

  core::Result<core::Pose3DTO> get_object_pose_at_revision(
      const std::string& object_id, const std::string& base_frame,
      const core::SceneRevisionDTO& expected_revision) const;

  core::SceneEditResultDTO apply_robot_base_pose(
      const core::SetRobotBasePoseRequestDTO& request);
  core::SceneEditResultDTO apply_collision_policy(
      const core::SetCollisionPolicyRequestDTO& request);
  std::vector<core::SceneRobotPlacementDTO> get_robot_placements() const;
  core::CollisionPolicySnapshot configured_collision_policy() const;

  core::SceneEditResultDTO add_object(const core::AddObjectRequestDTO& request);
  core::SceneEditResultDTO remove_object(const core::RemoveObjectRequestDTO& request);
  core::SceneEditResultDTO update_object_pose(const core::UpdateObjectPoseRequestDTO& request);
  core::SceneEditResultDTO attach_object(const core::AttachObjectRequestDTO& request);
  core::SceneEditResultDTO detach_object(const core::DetachObjectRequestDTO& request);

  std::vector<SceneObjectSnapshot> get_scene_objects();
  std::vector<SceneTraceEvent> get_scene_trace(size_t limit = 0);
  void clear_scene_trace();
  std::map<std::string, std::uint64_t> scene_update_status_counters() const;

  core::Result<core::CollisionWorldLeaseInventory> retain_planner_projection(
      const core::CollisionWorldRevision& revision,
      const std::string& retention_id);
  std::shared_ptr<const roboplan::Scene> acquire_planner_projection(
      const core::CollisionWorldRevision& revision,
      const std::string& retention_id) const;
  core::Result<core::Pose3DTO> resolve_reference_pose_on_retained_projection(
      const core::ReferenceDTO& reference,
      const std::string& base_frame,
      const core::SceneRevisionBindingDTO& binding) const;
  void release_planner_projection(const std::string& retention_id);
  core::SceneProjectionCommitResult rebuild_from_logical_snapshot(
      const core::LogicalSceneSnapshot& snapshot,
      const core::SceneRevisionDTO& revision);

  /// Recent collision-world ledger entries (spec/210 §3.8.1). Bounded
  /// ring buffer; `limit == 0` returns every retained entry, otherwise
  /// the most recent `limit` entries in newest-first order. Used by
  /// failure archives and audit tooling to reconstruct the chain of
  /// mutations that produced a given world state.
  std::vector<core::CollisionWorldLedgerEntry>
  recent_collision_world_ledger(std::size_t limit = 0) const;

  /// Current immutable collision-world snapshot (kernel-architecture sprint
  /// Chunk 2A). Built from the revision-bound RoboPlan scene projection and
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

  core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override;

 private:
  friend class RoboPlanCollisionDiagnosticComparator;

  using MismatchRecordEmitter =
      std::function<void(core::CollisionEngineMismatchRecord)>;

  /// Planner-native shadow comparison used only by the dedicated diagnostic
  /// comparator. It is intentionally private so it cannot be wired as a
  /// dispatch or runtime collision authority.
  core::CollisionResult compare_collision_diagnostic(
      const core::CollisionQuery& query);
  void configure_diagnostic_compare_engine(
      std::shared_ptr<core::ICollisionQueryEngine> engine);
  void configure_diagnostic_mismatch_emitter(MismatchRecordEmitter emitter);

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

  enum class PoseUpdateClassification {
    kGeometryChanged,
    kFreshnessOnly,
    kNoOp,
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
  std::vector<core::CollisionWorldBodyFreshness>
  runtime_body_freshness_locked() const;
  void publish_runtime_body_freshness_locked();
  bool publish_runtime_query_bundle_locked(
      core::RuntimeCollisionQueryBundlePtr bundle);
  PoseUpdateClassification classify_pose_update_locked(
      const std::string& object_id,
      const TrackedObjectState& previous_state,
      const TrackedObjectState& new_state,
      const std::string& robot_id) const;

  tl::expected<void, std::string> add_geometry_locked(
      const std::string& object_id, const TrackedObjectState& state);

  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<const roboplan::Scene> base_scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, TrackedObjectState> object_states_;
  mutable std::map<std::string, std::pair<std::string, std::string>>
      object_body_hash_cache_;
  Eigen::Matrix4d world_from_robot_base_ = Eigen::Matrix4d::Identity();
  std::set<std::pair<std::string, std::string>> disabled_static_collision_pairs_;
  std::set<std::pair<std::string, std::string>> disabled_support_contact_pairs_;
  std::deque<SceneTraceEvent> scene_trace_;

  // Diagnostic body-id mapper. Lazily rebuilt when the scene fingerprint
  // changes and used only by compare_collision_diagnostic().
  mutable std::mutex mapper_mutex_;
  std::string mapper_robot_id_ = "robot0";
  std::string mapper_scene_fingerprint_;
  std::unique_ptr<RoboPlanCollisionBodyIdMapper> mapper_;

  // Immutable policy snapshot. The diagnostic comparator copies it under
  // evaluator_mutex_ and uses query-local policy state.
  mutable std::mutex evaluator_mutex_;
  std::optional<core::CollisionPolicySnapshot> policy_snapshot_;

  // Secondary engine used only by the diagnostic comparator. It has no path
  // into dispatch admission or runtime-monitor decisions.
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

  // Publish-on-write runtime monitor bundle. Writers construct one immutable
  // coherent object under scene_mutex_ and atomically replace the handle;
  // runtime ticks never borrow a publisher or scene mutex.
  std::atomic<core::RuntimeCollisionQueryBundlePtr> published_runtime_bundle_;
  // Replaced bundles remain pinned until no reader holds them, then the NRT
  // scene writer reclaims them. This prevents the RuntimeSafety reader from
  // becoming the last owner and running vector/map destruction on its tick.
  static constexpr std::size_t kRuntimeBundleRetirementCapacity = 64;
  std::array<core::RuntimeCollisionQueryBundlePtr,
             kRuntimeBundleRetirementCapacity>
      retired_runtime_bundles_{};  // scene_mutex_
  // When the normal retirement bank is full, publication atomically becomes
  // unavailable and this dedicated NRT slot pins the displaced current bundle.
  // An explicit authority rebuild may refill the empty publication slot while
  // this owner remains pinned; further replacement stays fail-closed until an
  // NRT writer can reclaim retirement capacity.
  core::RuntimeCollisionQueryBundlePtr
      backpressured_runtime_bundle_;  // scene_mutex_
  std::uint64_t published_runtime_bundle_epoch_ = 0;  // scene_mutex_
  std::uint64_t scene_update_geometry_changed_count_ = 0;
  std::uint64_t scene_update_freshness_only_count_ = 0;
  std::uint64_t scene_update_no_op_count_ = 0;
  std::uint64_t runtime_bundle_publish_count_ = 0;
  std::uint64_t runtime_bundle_publish_backpressure_count_ = 0;

  mutable core::CollisionWorldSnapshotPtr snapshot_cache_;
  mutable std::uint64_t snapshot_cache_revision_id_ = 0;
  mutable std::string snapshot_cache_content_hash_;

  struct RetainedPlannerProjection {
    core::CollisionWorldRevision revision;
    std::shared_ptr<const roboplan::Scene> scene;
    core::Pose3DTO robot_base_pose_in_world;
    std::map<std::string, core::Pose3DTO> object_world_poses;
  };
  std::map<std::string, RetainedPlannerProjection>
      retained_planner_projections_;  // scene_mutex_
  static constexpr std::size_t kPlannerProjectionRetentionCapacity = 8;

  /// Bump the authoritative collision-world revision after a successful
  /// scene mutation. Must be called under scene_mutex_.
  void publish_collision_world_revision_locked(
      core::CollisionWorldMutationSource mutation_source,
      const std::vector<std::string>& affected_body_ids,
      std::string authority_source,
      std::optional<std::string> session_id = std::nullopt,
      bool advance_revision = true,
      bool advance_epochs = true);
};

}  // namespace manyforge::roboplan_backend
