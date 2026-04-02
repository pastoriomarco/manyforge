#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <Eigen/Dense>
#include <stdexcept>
#include <variant>

namespace manyforge::roboplan_backend {

RoboPlanPlannerBackend::RoboPlanPlannerBackend(
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<roboplan::RRT> rrt,
    std::shared_ptr<roboplan::PathShortcutter> shortcutter,
    std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra,
    std::shared_ptr<roboplan::SimpleIk> ik)
    : scene_(std::move(scene)),
      rrt_(std::move(rrt)),
      shortcutter_(std::move(shortcutter)),
      toppra_(std::move(toppra)),
      ik_(std::move(ik)) {}

core::PlanResultDTO RoboPlanPlannerBackend::plan(const core::PlanRequestDTO& request) {
  core::PlanResultDTO result;

  try {
    // Extract start state
    const auto& start = request.start_state;
    Eigen::VectorXd start_positions(start.positions_rad.size());
    for (size_t i = 0; i < start.positions_rad.size(); ++i) {
      start_positions[i] = start.positions_rad[i];
    }

    roboplan::JointConfiguration start_config;
    start_config.joint_names = start.joint_names;
    start_config.positions = start_positions;

    // Determine goal configuration based on motion type
    roboplan::JointConfiguration goal_config;

    if (auto* joint_goal = std::get_if<core::JointGoalDTO>(&request.motion)) {
      // Joint goal — direct conversion
      Eigen::VectorXd goal_positions(joint_goal->positions_rad.size());
      for (size_t i = 0; i < joint_goal->positions_rad.size(); ++i) {
        goal_positions[i] = joint_goal->positions_rad[i];
      }
      goal_config.joint_names = joint_goal->joint_names;
      goal_config.positions = goal_positions;

    } else if (auto* pose_goal = std::get_if<core::PoseGoalDTO>(&request.motion)) {
      // Pose goal — solve IK first
      Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
      tform(0, 3) = pose_goal->goal.position_m.x_m;
      tform(1, 3) = pose_goal->goal.position_m.y_m;
      tform(2, 3) = pose_goal->goal.position_m.z_m;

      // Quaternion to rotation matrix
      double x = pose_goal->goal.orientation.x;
      double y = pose_goal->goal.orientation.y;
      double z = pose_goal->goal.orientation.z;
      double w = pose_goal->goal.orientation.w;
      tform(0, 0) = 1 - 2*y*y - 2*z*z;
      tform(0, 1) = 2*x*y - 2*z*w;
      tform(0, 2) = 2*x*z + 2*y*w;
      tform(1, 0) = 2*x*y + 2*z*w;
      tform(1, 1) = 1 - 2*x*x - 2*z*z;
      tform(1, 2) = 2*y*z - 2*x*w;
      tform(2, 0) = 2*x*z - 2*y*w;
      tform(2, 1) = 2*y*z + 2*x*w;
      tform(2, 2) = 1 - 2*x*x - 2*y*y;

      roboplan::CartesianConfiguration cart_config;
      cart_config.base_frame = request.base_frame;
      cart_config.tip_frame = request.tcp_frame;
      cart_config.tform = tform;

      goal_config.joint_names = start.joint_names;
      goal_config.positions = start_positions;  // seed with current

      if (!ik_->solveIk(cart_config, start_config, goal_config)) {
        result.error = core::ErrorDTO{core::kErrorIkFailed, "IK failed to find solution", {}};
        return result;
      }

    } else {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                     "Unsupported motion type for native planner", {}};
      return result;
    }

    // Plan path with RRT
    auto path_result = rrt_->plan(start_config, goal_config);
    if (!path_result.has_value()) {
      result.error = core::ErrorDTO{core::kErrorPlanningFailed,
                                     path_result.error(), {}};
      return result;
    }

    // Shortcut the path
    auto smoothed_path = shortcutter_->shortcut(path_result.value(), 0.05);

    // Time-parameterize with TOPPRA, enforcing TCP speed limit if set.
    // Iteratively reduce velocity_scale until TCP linear speed is within bounds.
    const double tcp_limit = request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
    const std::string& tcp_frame = request.tcp_frame;
    double vel_scale = request.speed_profile.velocity_scale;
    constexpr int kMaxTcpIterations = 5;
    constexpr double kTcpSpeedMargin = 0.99;  // aim slightly under the limit

    roboplan::JointTrajectory final_traj;

    for (int iter = 0; iter < kMaxTcpIterations; ++iter) {
      auto traj_result = toppra_->generate(
          smoothed_path, 0.01,
          roboplan::SplineFittingMode::Hermite,
          vel_scale,
          request.speed_profile.acceleration_scale);

      if (!traj_result.has_value()) {
        result.error = core::ErrorDTO{core::kErrorPlanningFailed,
                                       "TOPPRA failed: " + traj_result.error(), {}};
        return result;
      }

      final_traj = traj_result.value();

      // If no TCP speed limit, accept on first pass
      if (tcp_limit <= 0.0 || tcp_frame.empty()) {
        break;
      }

      // Compute max TCP linear speed across trajectory segments
      double max_tcp_speed = 0.0;
      Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
      bool have_prev = false;

      for (size_t i = 0; i < final_traj.times.size(); ++i) {
        Eigen::Matrix4d tform = scene_->forwardKinematics(
            final_traj.positions[i], tcp_frame);
        Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

        if (have_prev && i > 0) {
          double dt = final_traj.times[i] - final_traj.times[i - 1];
          if (dt > 1e-9) {
            double speed = (pos - prev_pos).norm() / dt;
            if (speed > max_tcp_speed) {
              max_tcp_speed = speed;
            }
          }
        }

        prev_pos = pos;
        have_prev = true;
      }

      // If within limit, done
      if (max_tcp_speed <= tcp_limit) {
        break;
      }

      // Scale down proportionally: new_scale = old_scale * (limit / actual)
      double ratio = (tcp_limit * kTcpSpeedMargin) / max_tcp_speed;
      vel_scale *= ratio;

      // Clamp to a reasonable minimum
      if (vel_scale < 0.001) {
        vel_scale = 0.001;
      }
    }

    // Convert to TrajectoryDTO
    result.trajectory = convert_trajectory(final_traj);

  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorPlanningFailed, e.what(), {}};
  }

  return result;
}

core::TrajectoryDTO RoboPlanPlannerBackend::convert_trajectory(
    const roboplan::JointTrajectory& traj) {
  core::TrajectoryDTO dto;
  dto.joint_names = traj.joint_names;

  for (size_t i = 0; i < traj.times.size(); ++i) {
    core::TrajectoryWaypointDTO wp;
    wp.time_from_start_s = traj.times[i];

    wp.positions_rad.resize(traj.positions[i].size());
    for (Eigen::Index j = 0; j < traj.positions[i].size(); ++j) {
      wp.positions_rad[j] = traj.positions[i][j];
    }

    if (i < traj.velocities.size()) {
      wp.velocities_rad_s.resize(traj.velocities[i].size());
      for (Eigen::Index j = 0; j < traj.velocities[i].size(); ++j) {
        wp.velocities_rad_s[j] = traj.velocities[i][j];
      }
    }

    if (i < traj.accelerations.size()) {
      wp.accelerations_rad_s2.resize(traj.accelerations[i].size());
      for (Eigen::Index j = 0; j < traj.accelerations[i].size(); ++j) {
        wp.accelerations_rad_s2[j] = traj.accelerations[i][j];
      }
    }

    dto.waypoints.push_back(std::move(wp));
  }

  return dto;
}

}  // namespace manyforge::roboplan_backend
