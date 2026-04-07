#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <Eigen/Dense>
#include <cmath>
#include <map>
#include <string>
#include <variant>

namespace manyforge::roboplan_backend {

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
  state.joint_names = scene_->getActuatedJointNames();

  const auto& positions = scene_->getCurrentJointPositions();
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
      Eigen::VectorXd q(wp.positions_rad.size());
      for (size_t i = 0; i < wp.positions_rad.size(); ++i) {
        q[i] = wp.positions_rad[i];
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
        Eigen::VectorXd q(wp.positions_rad.size());
        for (size_t j = 0; j < wp.positions_rad.size(); ++j) {
          q[j] = wp.positions_rad[j];
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
  Eigen::VectorXd q(request.robot_state.positions_rad.size());
  for (size_t i = 0; i < request.robot_state.positions_rad.size(); ++i) {
    q[i] = request.robot_state.positions_rad[i];
  }
  result.in_collision = scene_->hasCollisions(q);

  return result;
}

core::Pose3DTO RoboPlanPlanningContext::forward_kinematics(
    const core::FkRequestDTO& request) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q(request.robot_state.positions_rad.size());
  for (size_t i = 0; i < request.robot_state.positions_rad.size(); ++i) {
    q[i] = request.robot_state.positions_rad[i];
  }

  Eigen::Matrix4d tform = scene_->forwardKinematics(q, request.target_frame);

  core::Pose3DTO pose;
  pose.position_m.x_m = tform(0, 3);
  pose.position_m.y_m = tform(1, 3);
  pose.position_m.z_m = tform(2, 3);

  // Rotation matrix to quaternion (Shoemake)
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

core::JointLimitsDTO RoboPlanPlanningContext::get_joint_limits(
    const std::string& /*robot_id*/, const std::string& /*group_id*/) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  core::JointLimitsDTO limits;
  limits.joint_names = scene_->getActuatedJointNames();

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

core::SceneEditResultDTO RoboPlanPlanningContext::add_object(
    const core::AddObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    Eigen::Matrix4d tform = pose_to_matrix(request.pose);
    Eigen::Vector4d color(1.0, 0.0, 0.0, 1.0);

    if (auto* box = std::get_if<core::BoxShapeDTO>(&request.shape)) {
      roboplan::Box b(box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
      auto r = scene_->addBoxGeometry(request.object_id, request.frame_id, b, tform, color);
      if (!r.has_value()) {
        result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
        return result;
      }
    } else if (auto* sphere = std::get_if<core::SphereShapeDTO>(&request.shape)) {
      roboplan::Sphere s(sphere->radius_m);
      auto r = scene_->addSphereGeometry(request.object_id, request.frame_id, s, tform, color);
      if (!r.has_value()) {
        result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
        return result;
      }
    } else {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                     "Unsupported shape type for native backend", {}};
      return result;
    }

    result.success = true;
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
  }

  return result;
}

core::SceneEditResultDTO RoboPlanPlanningContext::remove_object(
    const core::RemoveObjectRequestDTO& request) {
  core::SceneEditResultDTO result;

  try {
    std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
    auto r = scene_->removeGeometry(request.object_id);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorObjectNotFound, r.error(), {}};
      return result;
    }
    result.success = true;
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
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
    result.success = true;
  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorInternalError, e.what(), {}};
  }

  return result;
}

}  // namespace manyforge::roboplan_backend
