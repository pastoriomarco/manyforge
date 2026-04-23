#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <Eigen/Dense>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <roboplan/core/scene_utils.hpp>
#include <stdexcept>
#include <string>
#include <variant>

namespace manyforge::roboplan_backend {

namespace {

uint64_t steady_now_ns() {
  const auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             tp.time_since_epoch())
      .count();
}

Eigen::VectorXd vector_from_std(const std::vector<double>& positions) {
  Eigen::VectorXd q(positions.size());
  for (size_t i = 0; i < positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = positions[i];
  }
  return q;
}

}  // namespace

RoboPlanPlanningContext::RoboPlanPlanningContext(
    std::shared_ptr<roboplan::Scene> scene, std::string group_name,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)),
      group_name_(std::move(group_name)),
      scene_mutex_(std::move(scene_mutex)) {
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
}

core::RobotStateDTO RoboPlanPlanningContext::get_current_state() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::RobotStateDTO state;
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get current state: " + maybe_group_info.error());
  }
  const auto& group_info = maybe_group_info.value();
  state.joint_names = group_info.joint_names;

  Eigen::VectorXd positions = scene_->getCurrentJointPositions()(group_info.q_indices);
  if (group_info.has_continuous_dofs) {
    auto maybe_collapsed =
        roboplan::collapseContinuousJointPositions(*scene_, group_name_, positions);
    if (!maybe_collapsed) {
      throw std::runtime_error("Failed to get current state: " + maybe_collapsed.error());
    }
    positions = maybe_collapsed.value();
  }

  state.positions_rad.resize(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    state.positions_rad[i] = positions[i];
  }

  state.velocities_rad_s.assign(state.positions_rad.size(), 0.0);
  return state;
}

core::ValidationResultDTO RoboPlanPlanningContext::validate_trajectory(
    const core::TrajectoryDTO& trajectory,
    const core::ValidationOptionsDTO& options) {
  core::ValidationResultDTO result;

  try {
    // Collision check at each waypoint.
    for (const auto& wp : trajectory.waypoints) {
      std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
      Eigen::VectorXd q = vector_from_std(wp.positions_rad);
      if (q.size() != scene_->getCurrentJointPositions().size()) {
        q = scene_->toFullJointPositions(group_name_, q);
      }
      if (scene_->hasCollisions(q)) {
        result.error = core::ErrorDTO{
            core::kErrorValidationCollision,
            "Trajectory passes through collision", {}};
        return result;
      }
    }

    // TCP linear speed limit check (spec 230 §3.3, ADR-0015).
    if (options.max_tcp_linear_speed_mps > 0.0 &&
        !options.tcp_frame.empty() && trajectory.waypoints.size() >= 2) {
      Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
      bool have_prev = false;

      for (size_t i = 0; i < trajectory.waypoints.size(); ++i) {
        const auto& wp = trajectory.waypoints[i];
        std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
        Eigen::VectorXd q = vector_from_std(wp.positions_rad);
        if (q.size() != scene_->getCurrentJointPositions().size()) {
          q = scene_->toFullJointPositions(group_name_, q);
        }

        Eigen::Matrix4d tform = scene_->forwardKinematics(q, options.tcp_frame);
        Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

        if (have_prev && i > 0) {
          double dt = wp.time_from_start_s -
                      trajectory.waypoints[i - 1].time_from_start_s;
          if (dt > 1e-9) {
            double speed = (pos - prev_pos).norm() / dt;
            if (speed > options.max_tcp_linear_speed_mps) {
              std::map<std::string, std::string> details;
              details["limit_type"] = "tcp_linear_speed";
              details["measured"] = std::to_string(speed);
              details["limit"] = std::to_string(options.max_tcp_linear_speed_mps);
              details["units"] = "m/s";
              result.error = core::ErrorDTO{
                  core::kErrorValidationLimitsViolation,
                  "TCP linear speed exceeds limit", details};
              return result;
            }
          }
        }

        prev_pos = pos;
        have_prev = true;
      }
    }

    result.valid = true;
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
  }

  return result;
}

core::CollisionCheckResultDTO RoboPlanPlanningContext::collision_check(
    const core::CollisionCheckRequestDTO& request) {
  core::CollisionCheckResultDTO result;

  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }
  result.in_collision = scene_->hasCollisions(q);

  return result;
}

core::Pose3DTO RoboPlanPlanningContext::forward_kinematics(
    const core::FkRequestDTO& request) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  Eigen::Matrix4d tform = scene_->forwardKinematics(q, request.target_frame);
  return matrix_to_pose(tform);
}

core::Pose3DTO RoboPlanPlanningContext::get_object_pose(
    const std::string& object_id, const std::string& base_frame) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    throw std::runtime_error("Object not found: " + object_id);
  }

  Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
  Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(it->second.frame_id);
  Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
  Eigen::Matrix4d t_base_object = t_world_base.inverse() * t_world_object;
  return matrix_to_pose(t_base_object);
}

core::ObjectStateDTO RoboPlanPlanningContext::get_object_state(
    const std::string& object_id) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::ObjectStateDTO state;
  state.object_id = object_id;

  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    return state;
  }

  state.exists = true;
  state.attached = it->second.attached;
  state.frame_id = it->second.frame_id;
  return state;
}

core::Result<core::Pose3DTO> RoboPlanPlanningContext::resolve_reference_pose(
    const core::ReferenceDTO& reference, const std::string& base_frame) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    std::map<std::string, std::string> details;
    details["reference_id"] = reference.id.empty() ? "inline_pose" : reference.id;

    switch (reference.type) {
      case core::ReferenceDTO::Type::kObject: {
        details["reference_type"] = "object";
        const auto it = object_states_.find(reference.id);
        if (it == object_states_.end()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Object reference not found", details});
        }

        Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
        Eigen::Matrix4d t_world_frame =
            frame_to_world_matrix_locked(it->second.frame_id);
        Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
        Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
        return core::Result<core::Pose3DTO>::Ok(
            matrix_to_pose(t_world_base.inverse() * t_world_object));
      }

      case core::ReferenceDTO::Type::kFrame:
      case core::ReferenceDTO::Type::kRobotLink: {
        details["reference_type"] =
            reference.type == core::ReferenceDTO::Type::kFrame ? "frame" : "robot_link";
        if (reference.id.empty()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Reference id is empty", details});
        }
        try {
          Eigen::Matrix4d t_world_ref = frame_to_world_matrix_locked(reference.id);
          Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
          return core::Result<core::Pose3DTO>::Ok(
              matrix_to_pose(t_world_base.inverse() * t_world_ref));
        } catch (const std::exception&) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Reference frame not found", details});
        }
      }

      case core::ReferenceDTO::Type::kPose: {
        details["reference_type"] = "pose";
        if (!reference.pose.has_value()) {
          return core::Result<core::Pose3DTO>::Err(
              core::ErrorDTO{core::kErrorMissingReference,
                             "Pose reference is missing inline pose data", details});
        }
        return core::Result<core::Pose3DTO>::Ok(reference.pose.value());
      }
    }
  } catch (const std::exception& e) {
    return core::Result<core::Pose3DTO>::Err(
        core::ErrorDTO{core::kErrorInternalError, e.what(), {}});
  }

  return core::Result<core::Pose3DTO>::Err(
      core::ErrorDTO{core::kErrorInvalidRequest,
                     "Unsupported reference type", {}});
}

core::JointLimitsDTO RoboPlanPlanningContext::get_joint_limits(
    const std::string& /*robot_id*/, const std::string& /*group_id*/) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::JointLimitsDTO limits;
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get joint limits: " + maybe_group_info.error());
  }
  limits.joint_names = maybe_group_info->joint_names;

  auto pos_limits = scene_->getPositionLimitVectors(group_name_);
  if (pos_limits.has_value()) {
    const auto& [lower, upper] = pos_limits.value();
    limits.position_lower_rad.resize(lower.size());
    limits.position_upper_rad.resize(upper.size());
    for (Eigen::Index i = 0; i < lower.size(); ++i) {
      limits.position_lower_rad[i] = lower[i];
      limits.position_upper_rad[i] = upper[i];
    }
  }

  auto vel_limits = scene_->getVelocityLimitVectors(group_name_);
  if (vel_limits.has_value()) {
    const auto& [lower, upper] = vel_limits.value();
    limits.velocity_max_rad_s.resize(upper.size());
    for (Eigen::Index i = 0; i < upper.size(); ++i) {
      limits.velocity_max_rad_s[i] = upper[i];
    }
  }

  auto acc_limits = scene_->getAccelerationLimitVectors(group_name_);
  if (acc_limits.has_value()) {
    const auto& [lower, upper] = acc_limits.value();
    limits.acceleration_max_rad_s2.resize(upper.size());
    for (Eigen::Index i = 0; i < upper.size(); ++i) {
      limits.acceleration_max_rad_s2[i] = upper[i];
    }
  }

  return limits;
}

std::vector<std::string> RoboPlanPlanningContext::get_scene_object_ids() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  std::vector<std::string> object_ids;
  object_ids.reserve(object_states_.size());
  for (const auto& [object_id, tracked] : object_states_) {
    static_cast<void>(tracked);
    object_ids.push_back(object_id);
  }
  return object_ids;
}

std::vector<std::string> RoboPlanPlanningContext::get_joint_names() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto maybe_group_info = scene_->getJointGroupInfo(group_name_);
  if (!maybe_group_info) {
    throw std::runtime_error("Failed to get joint names: " + maybe_group_info.error());
  }
  return maybe_group_info->joint_names;
}

std::vector<RoboPlanPlanningContext::SceneObjectSnapshot>
RoboPlanPlanningContext::get_scene_objects() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  std::vector<SceneObjectSnapshot> objects;
  objects.reserve(object_states_.size());
  for (const auto& [object_id, tracked] : object_states_) {
    objects.push_back(SceneObjectSnapshot{
        object_id,
        tracked.shape,
        tracked.pose,
        pose_in_base_from_tracked_locked(tracked, "universe"),
        tracked.frame_id,
        tracked.attached,
    });
  }
  return objects;
}

Eigen::Matrix4d RoboPlanPlanningContext::pose_to_matrix(const core::Pose3DTO& pose) {
  Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
  tform(0, 3) = pose.position_m.x_m;
  tform(1, 3) = pose.position_m.y_m;
  tform(2, 3) = pose.position_m.z_m;

  double x = pose.orientation.x;
  double y = pose.orientation.y;
  double z = pose.orientation.z;
  double w = pose.orientation.w;
  tform(0, 0) = 1 - 2*y*y - 2*z*z;
  tform(0, 1) = 2*x*y - 2*z*w;
  tform(0, 2) = 2*x*z + 2*y*w;
  tform(1, 0) = 2*x*y + 2*z*w;
  tform(1, 1) = 1 - 2*x*x - 2*z*z;
  tform(1, 2) = 2*y*z - 2*x*w;
  tform(2, 0) = 2*x*z - 2*y*w;
  tform(2, 1) = 2*y*z + 2*x*w;
  tform(2, 2) = 1 - 2*x*x - 2*y*y;

  return tform;
}

core::Pose3DTO RoboPlanPlanningContext::matrix_to_pose(const Eigen::Matrix4d& tform) {
  core::Pose3DTO pose;
  pose.position_m.x_m = tform(0, 3);
  pose.position_m.y_m = tform(1, 3);
  pose.position_m.z_m = tform(2, 3);

  Eigen::Matrix3d R = tform.block<3, 3>(0, 0);
  double trace = R.trace();
  double x, y, z, w;

  if (trace > 0) {
    double s = std::sqrt(trace + 1.0) * 2;
    w = 0.25 * s;
    x = (R(2, 1) - R(1, 2)) / s;
    y = (R(0, 2) - R(2, 0)) / s;
    z = (R(1, 0) - R(0, 1)) / s;
  } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
    double s = std::sqrt(1.0 + R(0, 0) - R(1, 1) - R(2, 2)) * 2;
    w = (R(2, 1) - R(1, 2)) / s;
    x = 0.25 * s;
    y = (R(0, 1) + R(1, 0)) / s;
    z = (R(0, 2) + R(2, 0)) / s;
  } else if (R(1, 1) > R(2, 2)) {
    double s = std::sqrt(1.0 + R(1, 1) - R(0, 0) - R(2, 2)) * 2;
    w = (R(0, 2) - R(2, 0)) / s;
    x = (R(0, 1) + R(1, 0)) / s;
    y = 0.25 * s;
    z = (R(1, 2) + R(2, 1)) / s;
  } else {
    double s = std::sqrt(1.0 + R(2, 2) - R(0, 0) - R(1, 1)) * 2;
    w = (R(1, 0) - R(0, 1)) / s;
    x = (R(0, 2) + R(2, 0)) / s;
    y = (R(1, 2) + R(2, 1)) / s;
    z = 0.25 * s;
  }

  pose.orientation.x = x;
  pose.orientation.y = y;
  pose.orientation.z = z;
  pose.orientation.w = w;
  return pose;
}

Eigen::Matrix4d RoboPlanPlanningContext::frame_to_world_matrix_locked(
    const std::string& frame_id) {
  if (frame_id.empty() || frame_id == "universe") {
    return Eigen::Matrix4d::Identity();
  }

  const auto& positions = scene_->getCurrentJointPositions();
  Eigen::VectorXd q(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    q[i] = positions[i];
  }
  return scene_->forwardKinematics(q, frame_id);
}

core::Pose3DTO RoboPlanPlanningContext::pose_in_base_from_tracked_locked(
    const TrackedObjectState& state, const std::string& base_frame) {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  const Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(state.frame_id);
  const Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  const Eigen::Matrix4d t_world_base = frame_to_world_matrix_locked(base_frame);
  return matrix_to_pose(t_world_base.inverse() * t_world_object);
}

bool RoboPlanPlanningContext::infer_attached_from_frame(
    const std::string& frame_id) const {
  return !frame_id.empty() && frame_id != "universe" && frame_id != "base_link";
}

tl::expected<void, std::string> RoboPlanPlanningContext::set_touch_links_locked(
    const std::string& object_id,
    const std::vector<std::string>& touch_links,
    bool enable) {
  for (const auto& link_name : touch_links) {
    auto result = scene_->setCollisions(object_id, link_name, enable);
    if (!result.has_value()) {
      return tl::make_unexpected(result.error());
    }
  }
  return {};
}

tl::expected<void, std::string> RoboPlanPlanningContext::clear_touch_links_locked(
    const std::string& object_id,
    TrackedObjectState& tracked) {
  if (tracked.touch_links.empty()) {
    return {};
  }
  auto result = set_touch_links_locked(object_id, tracked.touch_links, true);
  if (!result.has_value()) {
    return result;
  }
  tracked.touch_links.clear();
  return {};
}

void RoboPlanPlanningContext::record_scene_event_locked(SceneTraceEvent event) {
  if (event.timestamp_ns == 0) {
    event.timestamp_ns = steady_now_ns();
  }
  scene_trace_.push_back(std::move(event));
  constexpr size_t kMaxSceneTraceEvents = 4096;
  while (scene_trace_.size() > kMaxSceneTraceEvents) {
    scene_trace_.pop_front();
  }
}

core::SceneEditResultDTO RoboPlanPlanningContext::reparent_object_locked(
    const std::string& object_id, const std::string& target_frame_id, bool emit_event) {
  core::SceneEditResultDTO result;
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    result.error = core::ErrorDTO{
        core::kErrorObjectNotFound, "Object not found: " + object_id, {}};
    SceneTraceEvent event;
    event.event = target_frame_id == "universe" ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = target_frame_id;
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
  Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(it->second.frame_id);
  Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  Eigen::Matrix4d t_world_target = frame_to_world_matrix_locked(target_frame_id);
  Eigen::Matrix4d t_target_object = t_world_target.inverse() * t_world_object;

  auto r = scene_->updateGeometryPlacement(object_id, target_frame_id, t_target_object);
  if (!r.has_value()) {
    result.error = core::ErrorDTO{core::kErrorObjectNotFound, r.error(), {}};
    SceneTraceEvent event;
    event.event = target_frame_id == "universe" ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = target_frame_id;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  auto& tracked = object_states_[object_id];
  tracked.pose = matrix_to_pose(t_target_object);
  tracked.frame_id = target_frame_id;
  tracked.attached = infer_attached_from_frame(target_frame_id);
  result.success = true;
  SceneTraceEvent event;
  event.event = tracked.attached ? "attach" : "detach";
  event.object_id = object_id;
  event.success = true;
  event.frame_id = tracked.frame_id;
  event.attached = tracked.attached;
  event.pose_in_frame = tracked.pose;
  event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
  if (emit_event) {
    record_scene_event_locked(std::move(event));
  }
  return result;
}

core::SceneEditResultDTO RoboPlanPlanningContext::add_object(
    const core::AddObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const bool attached_request = infer_attached_from_frame(request.frame_id);
    const std::string add_frame_id = attached_request ? "universe" : request.frame_id;
    Eigen::Matrix4d tform = pose_to_matrix(request.pose);
    if (attached_request) {
      tform = frame_to_world_matrix_locked(request.frame_id) * tform;
    }
    Eigen::Vector4d color(1.0, 0.0, 0.0, 1.0);

    if (auto* box = std::get_if<core::BoxShapeDTO>(&request.shape)) {
      roboplan::Box b(box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
      auto r = scene_->addBoxGeometry(request.object_id, add_frame_id, b, tform, color);
      if (!r.has_value()) {
        result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
        return result;
      }
    } else if (auto* sphere = std::get_if<core::SphereShapeDTO>(&request.shape)) {
      roboplan::Sphere s(sphere->radius_m);
      auto r = scene_->addSphereGeometry(request.object_id, add_frame_id, s, tform, color);
      if (!r.has_value()) {
        result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
        return result;
      }
    } else {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                     "Unsupported shape type for native backend", {}};
      return result;
    }

    object_states_[request.object_id] = TrackedObjectState{
        matrix_to_pose(tform), request.shape, add_frame_id, false, {}};

    if (attached_request) {
      auto reparent_result =
          reparent_object_locked(request.object_id, request.frame_id, false);
      if (!reparent_result.success) {
        const auto cleanup_result = scene_->removeGeometry(request.object_id);
        (void)cleanup_result;
        object_states_.erase(request.object_id);
        return reparent_result;
      }
    }

    result.success = true;
    const auto& tracked = object_states_.at(request.object_id);
    SceneTraceEvent event;
    event.event = "add";
    event.object_id = request.object_id;
    event.success = true;
    event.frame_id = tracked.frame_id;
    event.attached = tracked.attached;
    event.pose_in_frame = tracked.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "add";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanPlanningContext::remove_object(
    const core::RemoveObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    std::optional<TrackedObjectState> tracked_before;
    if (const auto it = object_states_.find(request.object_id);
        it != object_states_.end()) {
      tracked_before = it->second;
    }
    auto r = scene_->removeGeometry(request.object_id);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorObjectNotFound, r.error(), {}};
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    SceneTraceEvent event;
    event.event = "remove";
    event.object_id = request.object_id;
    event.success = true;
    if (tracked_before.has_value()) {
      event.frame_id = tracked_before->frame_id;
      event.attached = tracked_before->attached;
      event.pose_in_frame = tracked_before->pose;
      event.pose_in_universe =
          pose_in_base_from_tracked_locked(*tracked_before, "universe");
    }
    object_states_.erase(request.object_id);
    result.success = true;
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "remove";
    event.object_id = request.object_id;
    event.success = false;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanPlanningContext::update_object_pose(
    const core::UpdateObjectPoseRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    Eigen::Matrix4d tform = pose_to_matrix(request.pose);
    auto r = scene_->updateGeometryPlacement(request.object_id, request.frame_id, tform);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorObjectNotFound, r.error(), {}};
      return result;
    }
    auto& tracked = object_states_[request.object_id];
    tracked.pose = request.pose;
    tracked.frame_id = request.frame_id;
    tracked.attached = infer_attached_from_frame(request.frame_id);
    result.success = true;
    SceneTraceEvent event;
    event.event = "update_pose";
    event.object_id = request.object_id;
    event.success = true;
    event.frame_id = tracked.frame_id;
    event.attached = tracked.attached;
    event.pose_in_frame = tracked.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(tracked, "universe");
    record_scene_event_locked(std::move(event));
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "update_pose";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
  }

  return result;
}

core::SceneEditResultDTO RoboPlanPlanningContext::attach_object(
    const core::AttachObjectRequestDTO& request) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    if (request.frame_id.empty()) {
      auto result = core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorInvalidRequest,
                         "Attach target frame_id must not be empty", {}}};
      SceneTraceEvent event;
      event.event = "attach";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      auto result = core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorObjectNotFound,
                         "Object not found: " + request.object_id, {}}};
      SceneTraceEvent event;
      event.event = "attach";
      event.object_id = request.object_id;
      event.success = false;
      event.frame_id = request.frame_id;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    auto& tracked = it->second;
    auto clear_result = clear_touch_links_locked(request.object_id, tracked);
    if (!clear_result.has_value()) {
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorInternalError,
              "Failed to clear existing touch links before attach: " + clear_result.error(),
              {},
          },
      };
    }
    auto result = reparent_object_locked(request.object_id, request.frame_id);
    if (!result.success) {
      return result;
    }
    if (!request.touch_links.empty()) {
      auto collision_result =
          set_touch_links_locked(request.object_id, request.touch_links, false);
      if (!collision_result.has_value()) {
        return core::SceneEditResultDTO{
            false,
            core::ErrorDTO{
                core::kErrorInternalError,
                "Failed to set touch links for attached object: " + collision_result.error(),
                {},
            },
        };
      }
      tracked.touch_links = request.touch_links;
    }
    return result;
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "attach";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id;
    event.error = e.what();
    record_scene_event_locked(std::move(event));
    return core::SceneEditResultDTO{
        false, core::ErrorDTO{core::kErrorInternalError, e.what(), {}}};
  }
}

core::SceneEditResultDTO RoboPlanPlanningContext::detach_object(
    const core::DetachObjectRequestDTO& request) {
  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    const std::string target_frame = request.frame_id.empty() ? "universe" : request.frame_id;
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{core::kErrorObjectNotFound,
                         "Object not found: " + request.object_id, {}},
      };
    }
    auto& tracked = it->second;
    auto clear_result = clear_touch_links_locked(request.object_id, tracked);
    if (!clear_result.has_value()) {
      return core::SceneEditResultDTO{
          false,
          core::ErrorDTO{
              core::kErrorInternalError,
              "Failed to clear touch links before detach: " + clear_result.error(),
              {},
          },
      };
    }
    return reparent_object_locked(request.object_id, target_frame);
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    SceneTraceEvent event;
    event.event = "detach";
    event.object_id = request.object_id;
    event.success = false;
    event.frame_id = request.frame_id.empty()
                         ? std::optional<std::string>("universe")
                         : std::optional<std::string>(request.frame_id);
    event.error = e.what();
    record_scene_event_locked(std::move(event));
    return core::SceneEditResultDTO{
        false, core::ErrorDTO{core::kErrorInternalError, e.what(), {}}};
  }
}

std::vector<RoboPlanPlanningContext::SceneTraceEvent>
RoboPlanPlanningContext::get_scene_trace(size_t limit) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  if (limit == 0 || limit >= scene_trace_.size()) {
    return {scene_trace_.begin(), scene_trace_.end()};
  }
  return {scene_trace_.end() - static_cast<std::ptrdiff_t>(limit), scene_trace_.end()};
}

void RoboPlanPlanningContext::clear_scene_trace() {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  scene_trace_.clear();
}

}  // namespace manyforge::roboplan_backend
