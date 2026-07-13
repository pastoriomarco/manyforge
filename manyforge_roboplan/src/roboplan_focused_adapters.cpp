#include <manyforge_roboplan/roboplan_focused_adapters.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

core::ErrorDTO adapter_error(core::ErrorCode code, std::string message,
                             std::string reason) {
  return core::ErrorDTO{core::to_string(code), std::move(message),
                        {{"reason", std::move(reason)}}};
}

bool same_revision(const core::SceneRevisionDTO& left,
                   const core::SceneRevisionDTO& right) {
  return left.revision_id == right.revision_id &&
         left.content_hash == right.content_hash;
}

Eigen::VectorXd vector_from_std(const std::vector<double>& values) {
  Eigen::VectorXd result(static_cast<Eigen::Index>(values.size()));
  for (std::size_t index = 0; index < values.size(); ++index) {
    result[static_cast<Eigen::Index>(index)] = values[index];
  }
  return result;
}

bool is_world_frame(const std::string& frame_id) {
  return frame_id.empty() || frame_id == "universe" || frame_id == "world" ||
         frame_id == "World" || frame_id == "/World";
}

Eigen::Matrix4d pose_to_matrix(const core::Pose3DTO& pose) {
  Eigen::Quaterniond quaternion(
      pose.orientation.w, pose.orientation.x, pose.orientation.y,
      pose.orientation.z);
  if (quaternion.norm() <= std::numeric_limits<double>::epsilon()) {
    throw std::invalid_argument("pose quaternion must be nonzero");
  }
  quaternion.normalize();
  Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
  result.block<3, 3>(0, 0) = quaternion.toRotationMatrix();
  result(0, 3) = pose.position_m.x_m;
  result(1, 3) = pose.position_m.y_m;
  result(2, 3) = pose.position_m.z_m;
  return result;
}

core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix) {
  core::Pose3DTO pose;
  pose.position_m = {matrix(0, 3), matrix(1, 3), matrix(2, 3)};
  Eigen::Quaterniond quaternion(matrix.block<3, 3>(0, 0));
  quaternion.normalize();
  pose.orientation = {quaternion.x(), quaternion.y(), quaternion.z(),
                      quaternion.w()};
  return pose;
}

core::Result<Eigen::Matrix4d> robot_base_pose_from_authority(
    const core::ISceneAuthority& authority, const std::string& robot_id) {
  const auto placements = authority.robot_placements();
  const auto placement = std::find_if(
      placements.begin(), placements.end(), [&](const auto& candidate) {
        return candidate.robot_id == robot_id;
      });
  if (placement == placements.end()) {
    return core::Result<Eigen::Matrix4d>::Err(adapter_error(
        core::ErrorCode::kInvariantViolation,
        "Qualified robot placement is unavailable",
        "robot_placement_unavailable"));
  }
  try {
    return core::Result<Eigen::Matrix4d>::Ok(
        pose_to_matrix(placement->pose_in_world));
  } catch (const std::exception& error) {
    return core::Result<Eigen::Matrix4d>::Err(adapter_error(
        core::ErrorCode::kInvariantViolation, error.what(),
        "robot_placement_invalid"));
  }
}

core::ValidationResultDTO validation_error(
    core::ErrorCode code, std::string message, std::string reason,
    std::map<std::string, std::string> details = {}) {
  details.insert_or_assign("reason", std::move(reason));
  return core::ValidationResultDTO{
      false,
      core::ErrorDTO{core::to_string(code), std::move(message),
                     std::move(details)}};
}

core::SceneObjectSnapshotDTO convert_scene_object(
    const RoboPlanProjectionBackend::SceneObjectSnapshot& source) {
  core::SceneObjectSnapshotDTO target;
  target.object_id = source.object_id;
  target.shape = source.shape;
  target.pose_in_frame = source.pose_in_frame;
  target.pose_in_universe = source.pose_in_universe;
  target.frame_id = source.frame_id;
  target.attached = source.attached;
  target.touch_links = source.touch_links;
  target.support_contacts = source.support_contacts;
  target.source_id = source.source_id;
  target.source_timestamp_ns = source.source_timestamp_ns;
  target.source_freshness_budget_ns = source.source_freshness_budget_ns;
  target.scene_admission_role = source.scene_admission_role;
  target.scene_admission_lease_max_age_ns =
      source.scene_admission_lease_max_age_ns;
  target.scene_admission_target_drift_tolerance_m =
      source.scene_admission_target_drift_tolerance_m;
  target.scene_admission_target_angular_tolerance_rad =
      source.scene_admission_target_angular_tolerance_rad;
  target.scene_admission_obstacle_drift_tolerance_m =
      source.scene_admission_obstacle_drift_tolerance_m;
  target.scene_admission_obstacle_angular_tolerance_rad =
      source.scene_admission_obstacle_angular_tolerance_rad;
  target.scene_admission_allow_runtime_dynamic_obstacle =
      source.scene_admission_allow_runtime_dynamic_obstacle;
  return target;
}

}  // namespace

void RoboPlanSceneProjection::mark_desynchronized(std::string reason) {
  std::lock_guard<std::mutex> lock(health_mutex_);
  health_.health = core::SceneProjectionHealth::kDesynchronized;
  health_.projected_revision =
      services_->published_collision_world_revision();
  health_.reason = std::move(reason);
}

core::SceneProjectionCommitResult
RoboPlanSceneProjection::apply_transaction(
    const core::SceneTransactionDTO& transaction,
    const core::SceneRevisionDTO& proposed_revision) {
  std::lock_guard<std::mutex> commit_lock(commit_mutex_);
  core::SceneProjectionCommitResult result;
  if (!projection_health().ready()) {
    result.edit.error = adapter_error(
        core::ErrorCode::kInvariantViolation,
        "RoboPlan scene projection is desynchronized",
        "scene_projection_desynchronized");
    result.rollback_proven = false;
    return result;
  }
  const auto before = services_->published_collision_world_revision();
  if (!same_revision(before, transaction.expected_revision)) {
    result.edit.error = adapter_error(
        core::ErrorCode::kSceneChangedDuringValidation,
        "RoboPlan scene projection revision changed before commit",
        "scene_projection_expected_revision_mismatch");
    result.acknowledged_revision = before;
    return result;
  }

  try {
    result.edit = std::visit(
        [&](const auto& operation) -> core::SceneEditResultDTO {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, core::AddObjectRequestDTO>) {
            return services_->add_object(operation);
          } else if constexpr (std::is_same_v<Operation,
                                              core::RemoveObjectRequestDTO>) {
            return services_->remove_object(operation);
          } else if constexpr (std::is_same_v<
                                   Operation,
                                   core::UpdateObjectPoseRequestDTO>) {
            return services_->update_object_pose(operation);
          } else if constexpr (std::is_same_v<Operation,
                                              core::AttachObjectRequestDTO>) {
            return services_->attach_object(operation);
          } else if constexpr (std::is_same_v<
                                   Operation,
                                   core::DetachObjectRequestDTO>) {
            return services_->detach_object(operation);
          } else if constexpr (std::is_same_v<
                                   Operation,
                                   core::SetRobotBasePoseRequestDTO>) {
            return services_->apply_robot_base_pose(operation);
          } else {
            return services_->apply_collision_policy(operation);
          }
        },
        transaction.operation);
  } catch (const std::exception& error) {
    result.edit.error = adapter_error(
        core::ErrorCode::kInternalError, error.what(),
        "scene_projection_exception");
  } catch (...) {
    result.edit.error = adapter_error(
        core::ErrorCode::kInternalError,
        "RoboPlan scene projection raised a non-standard exception",
        "scene_projection_nonstandard_exception");
  }

  const auto after = services_->published_collision_world_revision();
  result.acknowledged_revision = after;
  const bool revision_changed = !same_revision(before, after);
  if (!result.edit.success) {
    // Atomic uniqueness of the revision is not rollback evidence: a native
    // operation can fail before publishing a new revision. Trust only the
    // operation's explicit proof that no mutable projection state was touched.
    result.rollback_proven = result.edit.rollback_proven;
    if (!result.rollback_proven) {
      mark_desynchronized(revision_changed
                              ? "failed_projection_commit_changed_revision"
                              : "failed_projection_commit_rollback_unproven");
    }
    return result;
  }

  if (revision_changed) {
    result.edit.collision_world_revision_changed = true;
    result.edit.update_status = "geometry_changed";
    if (after.revision_id != proposed_revision.revision_id) {
      result.edit.success = false;
      result.edit.error = adapter_error(
          core::ErrorCode::kInvariantViolation,
          "RoboPlan projection did not acknowledge the proposed revision",
          "scene_projection_proposed_revision_mismatch");
      result.rollback_proven = false;
      mark_desynchronized("successful_projection_revision_mismatch");
    }
  } else if (!result.edit.update_status) {
    result.edit.update_status = result.edit.collision_world_freshness_changed
                                    ? "freshness_only"
                                    : "no_op";
  }
  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    if (health_.health == core::SceneProjectionHealth::kReady) {
      health_.projected_revision = after;
      health_.reason.clear();
    }
  }
  return result;
}

core::Result<core::CollisionWorldLeaseInventory>
RoboPlanSceneProjection::retain_projection(
    const core::SceneRevisionDTO& revision,
    const std::string& retention_id) {
  if (!projection_health().ready()) {
    return core::Result<core::CollisionWorldLeaseInventory>::Err(
        adapter_error(core::ErrorCode::kInvariantViolation,
                      "RoboPlan scene projection is desynchronized",
                      "scene_projection_desynchronized"));
  }
  return services_->retain_planner_projection(revision, retention_id);
}

void RoboPlanSceneProjection::release_projection(
    const std::string& retention_id) {
  services_->release_planner_projection(retention_id);
}

core::SceneProjectionHealthDTO RoboPlanSceneProjection::projection_health()
    const {
  std::lock_guard<std::mutex> lock(health_mutex_);
  auto health = health_;
  if (health.projected_revision.revision_id == 0) {
    health.projected_revision =
        services_->published_collision_world_revision();
  }
  return health;
}

core::SceneProjectionCommitResult
RoboPlanSceneProjection::rebuild_projection(
    const core::LogicalSceneSnapshot& snapshot,
    const core::SceneRevisionDTO& proposed_revision) {
  std::lock_guard<std::mutex> commit_lock(commit_mutex_);
  auto result =
      services_->rebuild_from_logical_snapshot(snapshot, proposed_revision);
  {
    std::lock_guard<std::mutex> health_lock(health_mutex_);
    if (result.edit.success &&
        same_revision(result.acknowledged_revision, proposed_revision)) {
      health_.health = core::SceneProjectionHealth::kReady;
      health_.projected_revision = result.acknowledged_revision;
      health_.reason.clear();
    } else {
      health_.health = core::SceneProjectionHealth::kDesynchronized;
      health_.projected_revision = result.acknowledged_revision;
      health_.reason = "scene_projection_rebuild_failed";
    }
  }
  return result;
}

std::vector<core::SceneObjectSnapshotDTO>
RoboPlanSceneProjection::scene_objects() const {
  auto source = services_->get_scene_objects();
  std::vector<core::SceneObjectSnapshotDTO> result;
  result.reserve(source.size());
  for (const auto& object : source) {
    result.push_back(convert_scene_object(object));
  }
  return result;
}

std::vector<core::SceneRobotPlacementDTO>
RoboPlanSceneProjection::robot_placements() const {
  return services_->get_robot_placements();
}

core::CollisionPolicySnapshot
RoboPlanSceneProjection::configured_collision_policy() const {
  return services_->configured_collision_policy();
}

core::Result<core::Pose3DTO> RoboPlanSceneProjection::object_pose(
    const std::string& object_id, const std::string& base_frame,
    const core::SceneRevisionDTO& expected_revision) const {
  return services_->get_object_pose_at_revision(
      object_id, base_frame, expected_revision);
}

core::CollisionWorldSnapshotPtr RoboPlanSceneProjection::snapshot() const {
  return services_->snapshot();
}

core::CollisionWorldRevision
RoboPlanSceneProjection::published_collision_world_revision() const {
  return services_->published_collision_world_revision();
}

std::optional<core::CollisionPolicySnapshot>
RoboPlanSceneProjection::collision_policy_snapshot() const {
  return services_->collision_policy_snapshot();
}

core::CollisionWorldPolicySnapshotBundle
RoboPlanSceneProjection::collision_world_policy_snapshot_bundle() const {
  return services_->collision_world_policy_snapshot_bundle();
}

std::optional<core::CollisionWorldLeaseInventory>
RoboPlanSceneProjection::current_scene_snapshot_lease_inventory() const {
  return services_->current_scene_snapshot_lease_inventory();
}

core::RuntimeCollisionQueryBundlePtr
RoboPlanSceneProjection::published_runtime_collision_query_bundle() const {
  return services_->published_runtime_collision_query_bundle();
}

std::shared_ptr<const roboplan::Scene>
RoboPlanSceneProjection::acquire_native_projection(
    const core::SceneRevisionBindingDTO& binding) const {
  if (!binding.valid() || !projection_health().ready()) {
    return nullptr;
  }
  return services_->acquire_planner_projection(
      binding.revision, binding.projection_lease_id);
}

core::Result<core::Pose3DTO> RoboPlanSceneProjection::resolve_reference_pose(
    const core::ReferenceDTO& reference, const std::string& base_frame,
    const core::SceneRevisionBindingDTO& binding) const {
  if (!binding.valid() || !projection_health().ready()) {
    return core::Result<core::Pose3DTO>::Err(adapter_error(
        core::ErrorCode::kSceneChangedDuringValidation,
        "Reference resolution requires a retained planner projection",
        "reference_projection_binding_unavailable"));
  }
  return services_->resolve_reference_pose_on_retained_projection(
      reference, base_frame, binding);
}

std::vector<RoboPlanProjectionBackend::SceneTraceEvent>
RoboPlanSceneProjection::scene_trace(std::size_t limit) const {
  return services_->get_scene_trace(limit);
}

void RoboPlanSceneProjection::clear_scene_trace() {
  services_->clear_scene_trace();
}

std::map<std::string, std::uint64_t>
RoboPlanSceneProjection::scene_update_status_counters() const {
  return services_->scene_update_status_counters();
}

core::Result<core::Pose3DTO> RoboPlanKinematicsAdapter::forward_kinematics(
    const core::FkRequestDTO& request) const {
  auto world_from_robot_base =
      robot_base_pose_from_authority(*scene_authority_, robot_id_);
  if (!world_from_robot_base.ok()) {
    return core::Result<core::Pose3DTO>::Err(
        world_from_robot_base.error());
  }
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
    if (q.size() != scene_->getCurrentJointPositions().size()) {
      q = scene_->toFullJointPositions(group_name_, q);
    }
    const Eigen::Matrix4d robot_base_from_target =
        is_world_frame(request.target_frame)
            ? Eigen::Matrix4d::Identity()
            : scene_->forwardKinematics(q, request.target_frame);
    if (is_world_frame(request.base_frame)) {
      return core::Result<core::Pose3DTO>::Ok(matrix_to_pose(
          world_from_robot_base.value() * robot_base_from_target));
    }
    const auto robot_base_from_requested_base =
        scene_->forwardKinematics(q, request.base_frame);
    return core::Result<core::Pose3DTO>::Ok(matrix_to_pose(
        robot_base_from_requested_base.inverse() * robot_base_from_target));
  } catch (const std::exception& error) {
    return core::Result<core::Pose3DTO>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest, error.what(),
        "forward_kinematics_failed"));
  }
}

core::Result<std::map<std::string, core::Pose3DTO>>
RoboPlanKinematicsAdapter::forward_kinematics_batch(
    const core::RobotStateDTO& robot_state,
    const std::vector<std::string>& target_frames,
    const std::string& base_frame) const {
  auto world_from_robot_base =
      robot_base_pose_from_authority(*scene_authority_, robot_id_);
  if (!world_from_robot_base.ok()) {
    return core::Result<std::map<std::string, core::Pose3DTO>>::Err(
        world_from_robot_base.error());
  }
  try {
    std::map<std::string, core::Pose3DTO> result;
    if (target_frames.empty()) {
      return core::Result<std::map<std::string, core::Pose3DTO>>::Ok(
          std::move(result));
    }
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    Eigen::VectorXd q = vector_from_std(robot_state.positions_rad);
    if (q.size() != scene_->getCurrentJointPositions().size()) {
      q = scene_->toFullJointPositions(group_name_, q);
    }
    Eigen::Matrix4d requested_base_from_robot_base =
        Eigen::Matrix4d::Identity();
    if (is_world_frame(base_frame)) {
      requested_base_from_robot_base = world_from_robot_base.value();
    } else {
      requested_base_from_robot_base =
          scene_->forwardKinematics(q, base_frame).inverse();
    }
    for (const auto& target_frame : target_frames) {
      const Eigen::Matrix4d robot_base_from_target =
          is_world_frame(target_frame)
              ? Eigen::Matrix4d::Identity()
              : scene_->forwardKinematics(q, target_frame);
      result.emplace(
          target_frame,
          matrix_to_pose(requested_base_from_robot_base *
                         robot_base_from_target));
    }
    return core::Result<std::map<std::string, core::Pose3DTO>>::Ok(
        std::move(result));
  } catch (const std::exception& error) {
    return core::Result<std::map<std::string, core::Pose3DTO>>::Err(
        adapter_error(core::ErrorCode::kInvalidRequest, error.what(),
                      "forward_kinematics_batch_failed"));
  }
}

core::Result<core::JointLimitsDTO> RoboPlanKinematicsAdapter::joint_limits(
    const std::string&, const std::string& group_id) const {
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    const auto group = scene_->getJointGroupInfo(group_id);
    if (!group) {
      return core::Result<core::JointLimitsDTO>::Err(adapter_error(
          core::ErrorCode::kInvalidRequest, group.error(),
          "joint_limits_group_unavailable"));
    }
    core::JointLimitsDTO limits;
    limits.joint_names = group->joint_names;
    if (auto position =
            scene_->getPositionLimitVectors(group_id, /*collapsed=*/false)) {
      const auto& [lower, upper] = *position;
      limits.position_lower_rad.assign(lower.data(),
                                       lower.data() + lower.size());
      limits.position_upper_rad.assign(upper.data(),
                                       upper.data() + upper.size());
    }
    if (auto velocity = scene_->getVelocityLimitVectors(group_id)) {
      const auto& upper = velocity->second;
      limits.velocity_max_rad_s.assign(upper.data(),
                                       upper.data() + upper.size());
    }
    if (auto acceleration = scene_->getAccelerationLimitVectors(group_id)) {
      const auto& upper = acceleration->second;
      limits.acceleration_max_rad_s2.assign(
          upper.data(), upper.data() + upper.size());
    }
    return core::Result<core::JointLimitsDTO>::Ok(std::move(limits));
  } catch (const std::exception& error) {
    return core::Result<core::JointLimitsDTO>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest, error.what(),
        "joint_limits_unavailable"));
  }
}

core::Result<std::vector<std::string>>
RoboPlanKinematicsAdapter::group_joint_names(
    const std::string& group_id) const {
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    const auto group = scene_->getJointGroupInfo(group_id);
    if (!group) {
      return core::Result<std::vector<std::string>>::Err(adapter_error(
          core::ErrorCode::kInvalidRequest, group.error(),
          "group_joint_names_unavailable"));
    }
    return core::Result<std::vector<std::string>>::Ok(group->joint_names);
  } catch (const std::exception& error) {
    return core::Result<std::vector<std::string>>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest, error.what(),
        "group_joint_names_unavailable"));
  }
}

core::Result<core::Pose3DTO>
RoboPlanReferencePoseResolver::resolve_reference_pose(
    const core::ReferenceDTO& reference, const std::string& base_frame,
    const core::SceneRevisionBindingDTO& scene_binding) const {
  return projection_->resolve_reference_pose(reference, base_frame,
                                              scene_binding);
}

RoboPlanTrajectoryValidator::RoboPlanTrajectoryValidator(
    std::shared_ptr<roboplan::Scene> scene, std::string group_name,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)),
      group_name_(std::move(group_name)),
      scene_mutex_(std::move(scene_mutex)) {
  if (!scene_ || !scene_mutex_ || group_name_.empty()) {
    throw std::invalid_argument(
        "trajectory validator requires a scene, mutex, and group name");
  }
  std::lock_guard<std::mutex> lock(*scene_mutex_);
  const auto group = scene_->getJointGroupInfo(group_name_);
  if (!group) {
    throw std::invalid_argument("trajectory validator group is unavailable: " +
                                group.error());
  }
  model_joint_names_ = group->joint_names;
  if (auto position =
          scene_->getPositionLimitVectors(group_name_, /*collapsed=*/false)) {
    position_lower_rad_.assign(position->first.data(),
                               position->first.data() + position->first.size());
    position_upper_rad_.assign(position->second.data(),
                               position->second.data() + position->second.size());
  }
  if (auto velocity = scene_->getVelocityLimitVectors(group_name_)) {
    velocity_limits_rad_s_.assign(
        velocity->second.data(),
        velocity->second.data() + velocity->second.size());
  }
  if (auto acceleration = scene_->getAccelerationLimitVectors(group_name_)) {
    acceleration_limits_rad_s2_.assign(
        acceleration->second.data(),
        acceleration->second.data() + acceleration->second.size());
  }
  if (model_joint_names_.empty() ||
      position_lower_rad_.size() != model_joint_names_.size() ||
      position_upper_rad_.size() != model_joint_names_.size() ||
      velocity_limits_rad_s_.size() != model_joint_names_.size() ||
      acceleration_limits_rad_s2_.size() != model_joint_names_.size()) {
    throw std::invalid_argument(
        "trajectory validator requires complete qualified joint limits");
  }
  continuous_joints_.reserve(model_joint_names_.size());
  for (std::size_t index = 0; index < model_joint_names_.size(); ++index) {
    if (model_joint_names_[index].empty() ||
        std::find(model_joint_names_.begin(),
                  model_joint_names_.begin() +
                      static_cast<std::ptrdiff_t>(index),
                  model_joint_names_[index]) !=
            model_joint_names_.begin() +
                static_cast<std::ptrdiff_t>(index)) {
      throw std::invalid_argument(
          "trajectory validator requires nonempty, unique model joints");
    }
    velocity_limits_rad_s_[index] =
        std::abs(velocity_limits_rad_s_[index]);
    acceleration_limits_rad_s2_[index] =
        std::abs(acceleration_limits_rad_s2_[index]);
    continuous_joints_.push_back(
        static_cast<std::uint8_t>(!std::isfinite(position_lower_rad_[index]) &&
                                  !std::isfinite(position_upper_rad_[index])));
  }
}

core::ValidationResultDTO
RoboPlanTrajectoryValidator::validate_non_collision(
    const core::TrajectoryDTO& trajectory,
    const core::ValidationOptionsDTO& options,
    const core::CollisionCancellationToken& cancel) const {
  const auto cancelled = [&]() {
    return validation_error(
        core::ErrorCode::kValidationDeferred,
        "Non-collision validation was cancelled",
        "noncollision_validation_cancelled",
        {{"cancellation_reason", cancel.reason()}});
  };
  if (cancel.is_cancel_requested()) {
    return cancelled();
  }

  if (!std::isfinite(options.max_tcp_linear_speed_mps) ||
      options.max_tcp_linear_speed_mps < 0.0) {
    return validation_error(
        core::ErrorCode::kInvalidRequest,
        "TCP linear speed limit must be finite and non-negative",
        "tcp_linear_speed_limit_invalid");
  }
  if (options.max_tcp_linear_speed_mps > 0.0 &&
      (options.tcp_frame.empty() || options.base_frame.empty())) {
    return validation_error(
        core::ErrorCode::kInvalidRequest,
        "TCP speed validation requires nonempty TCP and base frames",
        "tcp_speed_frames_missing");
  }

  if (trajectory.joint_names.empty() || trajectory.waypoints.empty()) {
    return validation_error(
        core::ErrorCode::kInvalidRequest,
        "Trajectory must contain joints and at least one waypoint",
        "trajectory_empty");
  }
  const bool canonical_joint_order =
      trajectory.joint_names == model_joint_names_;
  std::vector<std::size_t> trajectory_to_model;
  if (!canonical_joint_order) {
    trajectory_to_model.reserve(trajectory.joint_names.size());
    for (std::size_t index = 0; index < trajectory.joint_names.size(); ++index) {
      const auto& joint_name = trajectory.joint_names[index];
      if (joint_name.empty() ||
          std::find(trajectory.joint_names.begin(),
                    trajectory.joint_names.begin() +
                        static_cast<std::ptrdiff_t>(index),
                    joint_name) !=
              trajectory.joint_names.begin() +
                  static_cast<std::ptrdiff_t>(index)) {
        return validation_error(
            core::ErrorCode::kInvalidRequest,
            "Trajectory joint names must be nonempty and unique",
            "trajectory_joint_names_invalid");
      }
      const auto model_joint =
          std::find(model_joint_names_.begin(), model_joint_names_.end(),
                    joint_name);
      if (model_joint == model_joint_names_.end()) {
        return validation_error(
            core::ErrorCode::kInvalidRequest,
            "Trajectory contains a joint outside the qualified group",
            "trajectory_joint_not_in_group",
            {{"joint", joint_name}, {"group_id", group_name_}});
      }
      trajectory_to_model.push_back(static_cast<std::size_t>(
          std::distance(model_joint_names_.begin(), model_joint)));
    }
  }
  const auto model_index_for = [&](std::size_t trajectory_index) {
    return canonical_joint_order ? trajectory_index
                                 : trajectory_to_model[trajectory_index];
  };

  constexpr double kLimitTolerance = 1e-8;
  double previous_time = -1.0;
  for (std::size_t waypoint_index = 0;
       waypoint_index < trajectory.waypoints.size(); ++waypoint_index) {
    if (cancel.is_cancel_requested()) {
      return cancelled();
    }
    const auto& waypoint = trajectory.waypoints[waypoint_index];
    if (!std::isfinite(waypoint.time_from_start_s) ||
        waypoint.time_from_start_s < 0.0 ||
        (waypoint_index > 0 &&
         waypoint.time_from_start_s <= previous_time)) {
      return validation_error(
          core::ErrorCode::kInvalidRequest,
          "Trajectory waypoint times must be finite and strictly increasing",
          "trajectory_time_invalid",
          {{"waypoint_index", std::to_string(waypoint_index)}});
    }
    if (waypoint.positions_rad.size() != trajectory.joint_names.size() ||
        (!waypoint.velocities_rad_s.empty() &&
         waypoint.velocities_rad_s.size() != trajectory.joint_names.size()) ||
        (!waypoint.accelerations_rad_s2.empty() &&
         waypoint.accelerations_rad_s2.size() !=
             trajectory.joint_names.size())) {
      return validation_error(
          core::ErrorCode::kInvalidRequest,
          "Trajectory waypoint dimensions do not match its joint order",
          "trajectory_waypoint_shape_mismatch",
          {{"waypoint_index", std::to_string(waypoint_index)}});
    }

    const auto* previous =
        waypoint_index > 0 ? &trajectory.waypoints[waypoint_index - 1]
                           : nullptr;
    const auto* before_previous =
        waypoint_index > 1 ? &trajectory.waypoints[waypoint_index - 2]
                           : nullptr;
    const double segment_duration =
        previous != nullptr ? waypoint.time_from_start_s - previous_time : 0.0;
    const double previous_segment_duration =
        before_previous != nullptr
            ? previous->time_from_start_s - before_previous->time_from_start_s
            : 0.0;

    for (std::size_t joint_index = 0;
         joint_index < trajectory.joint_names.size(); ++joint_index) {
      const auto model_joint_index = model_index_for(joint_index);
      const double position = waypoint.positions_rad[joint_index];
      if (!std::isfinite(position)) {
        return validation_error(
            core::ErrorCode::kInvalidRequest,
            "Trajectory contains a non-finite joint position",
            "trajectory_value_not_finite",
            {{"waypoint_index", std::to_string(waypoint_index)},
             {"joint", trajectory.joint_names[joint_index]},
             {"dimension", "position"}});
      }
      const double lower_limit = position_lower_rad_[model_joint_index];
      const double upper_limit = position_upper_rad_[model_joint_index];
      const bool continuous = continuous_joints_[model_joint_index] != 0U;
      if (position < lower_limit - kLimitTolerance ||
          position > upper_limit + kLimitTolerance) {
        return validation_error(
            core::ErrorCode::kValidationLimitsViolation,
            "Trajectory joint position exceeds the model limit",
            "trajectory_position_limit_exceeded",
            {{"waypoint_index", std::to_string(waypoint_index)},
             {"joint", trajectory.joint_names[joint_index]},
             {"limit_type", "joint_position"},
             {"measured", std::to_string(position)},
             {"lower_limit", std::to_string(lower_limit)},
             {"upper_limit", std::to_string(upper_limit)}});
      }
      if (!waypoint.velocities_rad_s.empty()) {
        const double velocity = waypoint.velocities_rad_s[joint_index];
        if (!std::isfinite(velocity)) {
          return validation_error(
              core::ErrorCode::kInvalidRequest,
              "Trajectory contains a non-finite joint velocity",
              "trajectory_value_not_finite",
              {{"waypoint_index", std::to_string(waypoint_index)},
               {"joint", trajectory.joint_names[joint_index]},
               {"dimension", "velocity"}});
        }
        const double velocity_limit =
            velocity_limits_rad_s_[model_joint_index];
        if (std::abs(velocity) > velocity_limit + kLimitTolerance) {
          return validation_error(
              core::ErrorCode::kValidationLimitsViolation,
              "Trajectory joint velocity exceeds the model limit",
              "trajectory_velocity_limit_exceeded",
              {{"waypoint_index", std::to_string(waypoint_index)},
               {"joint", trajectory.joint_names[joint_index]},
               {"limit_type", "joint_velocity"},
               {"measured", std::to_string(velocity)},
               {"limit", std::to_string(velocity_limit)}});
        }
      }
      if (!waypoint.accelerations_rad_s2.empty()) {
        const double acceleration =
            waypoint.accelerations_rad_s2[joint_index];
        if (!std::isfinite(acceleration)) {
          return validation_error(
              core::ErrorCode::kInvalidRequest,
              "Trajectory contains a non-finite joint acceleration",
              "trajectory_value_not_finite",
              {{"waypoint_index", std::to_string(waypoint_index)},
               {"joint", trajectory.joint_names[joint_index]},
               {"dimension", "acceleration"}});
        }
        const double acceleration_limit =
            acceleration_limits_rad_s2_[model_joint_index];
        if (std::abs(acceleration) >
            acceleration_limit + kLimitTolerance) {
          return validation_error(
              core::ErrorCode::kValidationLimitsViolation,
              "Trajectory joint acceleration exceeds the model limit",
              "trajectory_acceleration_limit_exceeded",
              {{"waypoint_index", std::to_string(waypoint_index)},
               {"joint", trajectory.joint_names[joint_index]},
               {"limit_type", "joint_acceleration"},
               {"measured", std::to_string(acceleration)},
               {"limit", std::to_string(acceleration_limit)}});
        }
      }

      if (previous != nullptr) {
        double delta = waypoint.positions_rad[joint_index] -
                       previous->positions_rad[joint_index];
        if (continuous) {
          delta = std::remainder(delta, 2.0 * std::numbers::pi);
        }
        const double segment_velocity = delta / segment_duration;
        const double velocity_limit =
            velocity_limits_rad_s_[model_joint_index];
        if (std::abs(segment_velocity) >
            velocity_limit + kLimitTolerance) {
          return validation_error(
              core::ErrorCode::kValidationLimitsViolation,
              "Trajectory segment exceeds the model velocity limit",
              "trajectory_segment_velocity_limit_exceeded",
              {{"segment_index", std::to_string(waypoint_index - 1)},
               {"joint", trajectory.joint_names[joint_index]},
               {"limit_type", "joint_velocity"},
               {"measured", std::to_string(segment_velocity)},
               {"limit", std::to_string(velocity_limit)}});
        }

        if (before_previous != nullptr) {
          const double acceleration_interval =
              0.5 * (previous_segment_duration + segment_duration);
          double previous_delta =
              previous->positions_rad[joint_index] -
              before_previous->positions_rad[joint_index];
          if (continuous) {
            previous_delta =
                std::remainder(previous_delta, 2.0 * std::numbers::pi);
          }
          const double previous_velocity =
              previous_delta / previous_segment_duration;
          const double acceleration =
              (segment_velocity - previous_velocity) /
              acceleration_interval;
          const double acceleration_limit =
              acceleration_limits_rad_s2_[model_joint_index];
          if (std::abs(acceleration) >
              acceleration_limit + kLimitTolerance) {
            return validation_error(
                core::ErrorCode::kValidationLimitsViolation,
                "Trajectory segments exceed the model acceleration limit",
                "trajectory_segment_acceleration_limit_exceeded",
                {{"segment_index", std::to_string(waypoint_index - 1)},
                 {"joint", trajectory.joint_names[joint_index]},
                 {"limit_type", "joint_acceleration"},
                 {"measured", std::to_string(acceleration)},
                 {"limit", std::to_string(acceleration_limit)}});
          }
        }
      }
    }
    previous_time = waypoint.time_from_start_s;
  }

  if (options.max_tcp_linear_speed_mps > 0.0 &&
      !options.tcp_frame.empty() && trajectory.waypoints.size() >= 2) {
    const auto speed_started = std::chrono::steady_clock::now();
    try {
      std::lock_guard<std::mutex> lock(*scene_mutex_);
      Eigen::Vector3d previous_position = Eigen::Vector3d::Zero();
      bool have_previous = false;
      double max_speed = 0.0;
      std::size_t max_segment_index = 0;
      double max_segment_duration_s = 0.0;
      for (std::size_t waypoint_index = 0;
           waypoint_index < trajectory.waypoints.size(); ++waypoint_index) {
        if (cancel.is_cancel_requested()) {
          return cancelled();
        }
        const auto& waypoint = trajectory.waypoints[waypoint_index];
        auto q = vector_from_std(waypoint.positions_rad);
        if (q.size() != scene_->getCurrentJointPositions().size()) {
          q = scene_->toFullJointPositions(group_name_, q);
        }
        const Eigen::Matrix4d robot_base_from_tcp =
            scene_->forwardKinematics(q, options.tcp_frame);
        Eigen::Matrix4d requested_base_from_robot_base =
            Eigen::Matrix4d::Identity();
        if (!is_world_frame(options.base_frame)) {
          requested_base_from_robot_base =
              scene_->forwardKinematics(q, options.base_frame).inverse();
        }
        const Eigen::Matrix4d tcp_transform =
            requested_base_from_robot_base * robot_base_from_tcp;
        const Eigen::Vector3d tcp_position{
            tcp_transform(0, 3), tcp_transform(1, 3),
            tcp_transform(2, 3)};
        if (have_previous) {
          const double duration =
              waypoint.time_from_start_s -
              trajectory.waypoints[waypoint_index - 1].time_from_start_s;
          const double speed =
              (tcp_position - previous_position).norm() / duration;
          if (speed > max_speed) {
            max_speed = speed;
            max_segment_index = waypoint_index - 1;
            max_segment_duration_s = duration;
          }
        }
        previous_position = tcp_position;
        have_previous = true;
      }
      if (max_speed >
          options.max_tcp_linear_speed_mps + kLimitTolerance) {
        const auto speed_duration_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - speed_started)
                .count();
        return validation_error(
            core::ErrorCode::kValidationLimitsViolation,
            "TCP linear speed exceeds the configured limit",
            "tcp_linear_speed_limit_exceeded",
            {{"limit_type", "tcp_linear_speed"},
             {"segment_index", std::to_string(max_segment_index)},
             {"max_segment_index", std::to_string(max_segment_index)},
             {"max_segment_dt_s",
              std::to_string(max_segment_duration_s)},
             {"tcp_frame", options.tcp_frame},
             {"base_frame", options.base_frame},
             {"measured", std::to_string(max_speed)},
             {"limit", std::to_string(options.max_tcp_linear_speed_mps)},
             {"units", "m/s"},
             {"waypoint_count",
              std::to_string(trajectory.waypoints.size())},
             {"trajectory_duration_s",
              std::to_string(
                  trajectory.waypoints.back().time_from_start_s)},
             {"speed_validation_duration_ns",
              std::to_string(speed_duration_ns)},
             {"collision_validation_duration_ns", "0"}});
      }
    } catch (const std::exception& error) {
      return validation_error(
          core::ErrorCode::kInternalError,
          "TCP speed validation could not evaluate forward kinematics",
          "tcp_speed_validation_failed",
          {{"exception", error.what()}});
    }
  }

  return core::ValidationResultDTO{true, std::nullopt};
}

core::Result<core::TrajectoryDTO>
RoboPlanCollisionTrajectoryProjector::expand_for_collision(
    const core::TrajectoryDTO& trajectory,
    const core::RobotStateDTO& observed_start_state) const {
  if (trajectory.waypoints.empty()) {
    return core::Result<core::TrajectoryDTO>::Ok(trajectory);
  }
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    const auto current_q = scene_->getCurrentJointPositions();
    const auto full_q_size = static_cast<std::size_t>(current_q.size());
    if (full_q_size == 0) {
      return core::Result<core::TrajectoryDTO>::Ok(trajectory);
    }

    Eigen::VectorXd baseline =
        vector_from_std(observed_start_state.positions_rad);
    if (baseline.size() != current_q.size()) {
      baseline = scene_->toFullJointPositions(group_name_, baseline);
    }
    if (baseline.size() != current_q.size() || !baseline.allFinite()) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Observed start state cannot seed full collision coordinates",
          {{"reason", "collision_projection_observed_state_invalid"},
           {"expected_position_count", std::to_string(full_q_size)},
           {"observed_position_count",
            std::to_string(observed_start_state.positions_rad.size())}}});
    }

    const auto& model = scene_->getModel();
    std::vector<std::string> full_joint_names(full_q_size);
    for (pinocchio::JointIndex joint_id = 1;
         joint_id < static_cast<pinocchio::JointIndex>(model.names.size());
         ++joint_id) {
      if (joint_id >= static_cast<pinocchio::JointIndex>(model.nqs.size()) ||
          joint_id >= static_cast<pinocchio::JointIndex>(model.idx_qs.size()) ||
          model.nqs[joint_id] != 1) {
        continue;
      }
      const auto q_index = model.idx_qs[joint_id];
      if (q_index >= 0 && static_cast<std::size_t>(q_index) < full_q_size) {
        full_joint_names[static_cast<std::size_t>(q_index)] =
            model.names[static_cast<std::size_t>(joint_id)];
      }
    }
    for (std::size_t index = 0; index < full_joint_names.size(); ++index) {
      if (full_joint_names[index].empty()) {
        full_joint_names[index] = "q[" + std::to_string(index) + "]";
      }
    }

    const bool positions_already_full = std::all_of(
        trajectory.waypoints.begin(), trajectory.waypoints.end(),
        [&](const auto& waypoint) {
          return waypoint.positions_rad.size() == full_q_size;
        });
    if (positions_already_full) {
      auto expanded = trajectory;
      expanded.joint_names = std::move(full_joint_names);
      return core::Result<core::TrajectoryDTO>::Ok(std::move(expanded));
    }

    const auto group = scene_->getJointGroupInfo(group_name_);
    if (!group) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Collision projection robot group is unavailable",
          {{"reason", "active_group_unavailable"},
           {"group", group_name_},
           {"detail", group.error()}}});
    }
    if (group->joint_names.size() !=
        static_cast<std::size_t>(group->q_indices.size())) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Collision projection group has unsupported mimic/fixed entries",
          {{"reason", "active_group_contains_mimic_or_fixed_entries"},
           {"group", group_name_}}});
    }
    std::map<std::string, Eigen::Index> q_index_by_joint;
    for (std::size_t index = 0; index < group->joint_names.size(); ++index) {
      const auto q_index =
          group->q_indices[static_cast<Eigen::Index>(index)];
      if (q_index < 0 || q_index >= baseline.size()) {
        return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
            core::kErrorValidationCollision,
            "Collision projection joint index is out of range",
            {{"reason", "active_group_q_index_out_of_range"},
             {"group", group_name_},
             {"joint", group->joint_names[index]}}});
      }
      q_index_by_joint.emplace(group->joint_names[index], q_index);
    }

    core::TrajectoryDTO expanded;
    expanded.joint_names = std::move(full_joint_names);
    expanded.waypoints.reserve(trajectory.waypoints.size());
    for (const auto& waypoint : trajectory.waypoints) {
      if (waypoint.positions_rad.size() != trajectory.joint_names.size()) {
        return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
            core::kErrorValidationCollision,
            "Collision projection waypoint shape is inconsistent",
            {{"reason", "waypoint_position_count_mismatch"},
             {"joint_count", std::to_string(trajectory.joint_names.size())},
             {"position_count",
              std::to_string(waypoint.positions_rad.size())}}});
      }
      auto expanded_waypoint = waypoint;
      expanded_waypoint.positions_rad.assign(
          baseline.data(), baseline.data() + baseline.size());
      for (std::size_t index = 0; index < trajectory.joint_names.size();
           ++index) {
        const auto q_index =
            q_index_by_joint.find(trajectory.joint_names[index]);
        if (q_index == q_index_by_joint.end()) {
          return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
              core::kErrorValidationCollision,
              "Collision projection trajectory joint is outside the active group",
              {{"reason", "trajectory_joint_not_in_active_group"},
               {"group", group_name_},
               {"joint", trajectory.joint_names[index]}}});
        }
        expanded_waypoint.positions_rad[static_cast<std::size_t>(
            q_index->second)] = waypoint.positions_rad[index];
      }
      expanded.waypoints.push_back(std::move(expanded_waypoint));
    }
    return core::Result<core::TrajectoryDTO>::Ok(std::move(expanded));
  } catch (const std::exception& error) {
    return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
        core::kErrorValidationCollision,
        "Failed to expand trajectory for collision validation",
        {{"reason", "collision_projection_exception"},
         {"exception", error.what()}}});
  }
}

core::CollisionResult RoboPlanCollisionDiagnosticComparator::compare(
    const core::CollisionQuery& query) const {
  return services_->compare_collision_diagnostic(query);
}

void RoboPlanCollisionDiagnosticComparator::set_compare_engine(
    std::shared_ptr<core::ICollisionQueryEngine> engine) {
  services_->configure_diagnostic_compare_engine(std::move(engine));
}

void RoboPlanCollisionDiagnosticComparator::set_mismatch_record_emitter(
    MismatchRecordEmitter emitter) {
  services_->configure_diagnostic_mismatch_emitter(std::move(emitter));
}

}  // namespace manyforge::roboplan_backend
