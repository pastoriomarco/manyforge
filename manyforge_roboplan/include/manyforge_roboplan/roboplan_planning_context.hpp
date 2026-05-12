#pragma once

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

#include <manyforge_core/ports/iplanning_context.hpp>

namespace manyforge::roboplan_backend {

/// Native C++ planning context wrapping RoboPlan Scene.
/// Provides state queries, FK, collision checks, validation, and scene edits.
class RoboPlanPlanningContext final : public core::IPlanningContext {
 public:
  struct SceneObjectSnapshot {
    std::string object_id;
    core::CollisionShapeDTO shape;
    core::Pose3DTO pose_in_frame;
    core::Pose3DTO pose_in_universe;
    std::string frame_id;
    bool attached = false;
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
                                  std::shared_ptr<std::mutex> scene_mutex = nullptr);

  core::RobotStateDTO get_current_state() override;

  core::ValidationResultDTO validate_trajectory(
      const core::TrajectoryDTO& trajectory,
      const core::ValidationOptionsDTO& options) override;

  core::CollisionCheckResultDTO collision_check(
      const core::CollisionCheckRequestDTO& request) override;

  core::Pose3DTO forward_kinematics(const core::FkRequestDTO& request) override;
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

 private:
  struct TrackedObjectState {
    core::Pose3DTO pose;
    core::CollisionShapeDTO shape;
    std::string frame_id;
    bool attached = false;
    std::vector<std::string> touch_links;
    std::vector<core::SupportContactDTO> support_contacts;
  };

  struct CollisionPairDiagnostic {
    std::string body_a;
    std::string body_b;
    std::string text;
    std::pair<std::string, std::string> canonical_pair;
  };

  struct CollisionDiagnostics {
    std::vector<CollisionPairDiagnostic> pairs;
    std::string raw_output;
  };

  Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose);
  core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix);
  bool is_world_frame(const std::string& frame_id) const;
  Eigen::Matrix4d scene_placement_from_tracked_locked(const TrackedObjectState& state);
  Eigen::Matrix4d parent_joint_from_frame_matrix_locked(const std::string& frame_id);
  Eigen::Matrix4d frame_to_world_matrix_locked(const std::string& frame_id);
  core::Pose3DTO pose_in_base_from_tracked_locked(const TrackedObjectState& state,
                                                  const std::string& base_frame);
  bool infer_attached_from_frame(const std::string& frame_id) const;
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

  tl::expected<void, std::string> add_geometry_locked(
      const std::string& object_id, const TrackedObjectState& state);

  std::shared_ptr<roboplan::Scene> scene_;
  std::string group_name_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, TrackedObjectState> object_states_;
  Eigen::Matrix4d world_from_robot_base_ = Eigen::Matrix4d::Identity();
  std::set<std::pair<std::string, std::string>> disabled_static_collision_pairs_;
  std::set<std::pair<std::string, std::string>> disabled_support_contact_pairs_;
  std::deque<SceneTraceEvent> scene_trace_;
};

}  // namespace manyforge::roboplan_backend
