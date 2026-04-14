#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <Eigen/Dense>
#include <cmath>
#include <fmt/format.h>
#include <stdexcept>
#include <variant>

namespace {
// Rotation matrix to RPY (ZYX convention, in degrees)
void matrix_to_rpy_deg(const Eigen::Matrix4d& tform, double& roll, double& pitch, double& yaw) {
  pitch = std::atan2(-tform(2,0), std::sqrt(tform(0,0)*tform(0,0) + tform(1,0)*tform(1,0)));
  if (std::abs(std::cos(pitch)) > 1e-6) {
    roll  = std::atan2(tform(2,1), tform(2,2));
    yaw   = std::atan2(tform(1,0), tform(0,0));
  } else {
    roll  = std::atan2(-tform(1,2), tform(1,1));
    yaw   = 0.0;
  }
  roll  *= 180.0 / M_PI;
  pitch *= 180.0 / M_PI;
  yaw   *= 180.0 / M_PI;
}
}  // namespace

namespace manyforge::roboplan_backend {

RoboPlanPlannerBackend::RoboPlanPlannerBackend(
    std::shared_ptr<roboplan::Scene> scene,
    const roboplan::RRTOptions& rrt_options,
    std::shared_ptr<roboplan::PathShortcutter> shortcutter,
    std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra,
    std::shared_ptr<roboplan::SimpleIk> ik)
    : scene_(std::move(scene)),
      rrt_options_(rrt_options),
      shortcutter_(std::move(shortcutter)),
      toppra_(std::move(toppra)),
      ik_(std::move(ik)) {}

core::PlanResultDTO RoboPlanPlannerBackend::plan(const core::PlanRequestDTO& request) {
  core::PlanResultDTO result;

  try {
    fmt::print(stderr, "[PLANNER] plan() called, move_id={}, motion_type={}\n",
            request.move_id,
            std::holds_alternative<core::JointGoalDTO>(request.motion) ? "joint" : "pose");

    // Extract start state
    const auto& start = request.start_state;
    Eigen::VectorXd start_positions(start.positions_rad.size());
    for (size_t i = 0; i < start.positions_rad.size(); ++i) {
      start_positions[i] = start.positions_rad[i];
    }

    fmt::print(stderr, "[PLANNER] start joints: [");
    for (size_t i = 0; i < start.positions_rad.size(); ++i)
      fmt::print(stderr, "{}{:.4f}", (i ? ", " : ""), start.positions_rad[i]);
    fmt::print(stderr, "]\n");

    roboplan::JointConfiguration start_config;
    start_config.joint_names = start.joint_names;
    start_config.positions = start_positions;

    // Check start for self-collision
    bool start_collision = scene_->hasCollisions(start_positions);
    bool goal_collision = false;

    // FK of start pose
    {
      Eigen::Matrix4d start_fk = scene_->forwardKinematics(start_positions, request.tcp_frame);
      double r, p, ya;
      matrix_to_rpy_deg(start_fk, r, p, ya);
      fmt::print(stderr, "[PLANNER] start pose:  pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f}) collision={}\n",
              start_fk(0, 3), start_fk(1, 3), start_fk(2, 3), r, p, ya,
              start_collision ? "YES" : "no");
    }

    if (start_collision) {
      scene_->hasCollisions(start_positions, true);
      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          "Planning preview rejected: start state is already in collision",
          {
              {"failure_stage", "start_state"},
              {"start_in_collision", "true"},
          },
      };
      return result;
    }

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

      {
        double r, p, ya;
        matrix_to_rpy_deg(tform, r, p, ya);
        fmt::print(stderr, "[PLANNER] goal pose:   pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f})\n",
                tform(0, 3), tform(1, 3), tform(2, 3), r, p, ya);
      }

      roboplan::CartesianConfiguration cart_config;
      cart_config.base_frame = request.base_frame;
      cart_config.tip_frame = request.tcp_frame;
      cart_config.tform = tform;

      goal_config.joint_names = start.joint_names;
      goal_config.positions = start_positions;  // seed with current

      if (!ik_->solveIk(cart_config, start_config, goal_config)) {
        double r, p, ya;
        matrix_to_rpy_deg(tform, r, p, ya);
        auto str = fmt::format(
                 "IK failed: goal pos=({:.3f},{:.3f},{:.3f}) rpy=({:.1f},{:.1f},{:.1f}) frame={}->{}",
                 tform(0,3), tform(1,3), tform(2,3), r, p, ya,
                 request.base_frame, request.tcp_frame);
        result.error = core::ErrorDTO{core::kErrorIkFailed, str, {}};
        fmt::print(stderr, "[PLANNER] IK FAILED\n");
        return result;
      }

      fmt::print(stderr, "[PLANNER] IK solved. goal: [");
      for (int i = 0; i < goal_config.positions.size(); ++i)
        fmt::print(stderr, "{}{:.4f}", (i ? ", " : ""), goal_config.positions[i]);
      fmt::print(stderr, "]\n");

      // Check goal for self-collision
      goal_collision = scene_->hasCollisions(goal_config.positions);
      fmt::print(stderr, "[PLANNER] goal in collision: {}\n", goal_collision ? "YES" : "no");

      if (goal_collision) {
        scene_->hasCollisions(goal_config.positions, true);
        result.error = core::ErrorDTO{
            core::kErrorPlanningFailed,
            "Planning preview rejected: goal state is in collision",
            {
                {"failure_stage", "goal_state"},
                {"start_in_collision", "false"},
                {"goal_in_collision", "true"},
            },
        };
        return result;
      }

      // Compute joint-space distance
      double jdist = (goal_config.positions - start_config.positions).norm();
      fmt::print(stderr, "[PLANNER] joint-space distance start->goal: {:.4f} rad\n", jdist);

    } else {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                     "Unsupported motion type for native planner", {}};
      return result;
    }

    // Plan path with an RRT instance configured for this request's timeout.
    // This gives the native backend a hard, cooperative planning bound instead
    // of relying only on a post-hoc stop request in the kernel.
    auto request_rrt_options = rrt_options_;
    if (request.planning_timeout_s > 0.0) {
      if (request_rrt_options.max_planning_time > 0.0) {
        request_rrt_options.max_planning_time =
            std::min(request_rrt_options.max_planning_time,
                     request.planning_timeout_s);
      } else {
        request_rrt_options.max_planning_time = request.planning_timeout_s;
      }
    }
    roboplan::RRT request_rrt(scene_, request_rrt_options);

    // Plan path with RRT
    fmt::print(stderr, "[PLANNER] calling RRT...\n");
    auto path_result = request_rrt.plan(start_config, goal_config);
    if (!path_result.has_value()) {
      fmt::print(stderr, "[PLANNER] RRT FAILED: {}\n", path_result.error());

      // Build detailed error message
      std::string err_msg = "RRT failed: " + path_result.error();
      err_msg += " | start=[";
      for (int i = 0; i < start_config.positions.size(); ++i) {
        if (i) err_msg += ",";
        err_msg += fmt::format("{:.3f}", start_config.positions[i]);
      }
      err_msg += "] goal=[";
      for (int i = 0; i < goal_config.positions.size(); ++i) {
        if (i) err_msg += ",";
        err_msg += fmt::format("{:.3f}", goal_config.positions[i]);
      }
      err_msg += "]";

      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          err_msg,
          {
              {"failure_stage", "rrt"},
              {"start_in_collision", start_collision ? "true" : "false"},
              {"goal_in_collision", goal_collision ? "true" : "false"},
          },
      };
      return result;
    }
    fmt::print(stderr, "[PLANNER] RRT succeeded\n");

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
