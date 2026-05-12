#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <Eigen/Dense>
#include <cmath>
#include <cstdlib>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
bool planner_debug_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MANYFORGE_ROBOPLAN_PLANNER_DEBUG");
    if (value == nullptr) {
      return false;
    }
    const std::string flag(value);
    return !(flag.empty() || flag == "0" || flag == "false" || flag == "FALSE");
  }();
  return enabled;
}

template <typename... Args>
void planner_debug(fmt::format_string<Args...> format, Args&&... args) {
  if (!planner_debug_enabled()) {
    return;
  }
  fmt::print(stderr, format, std::forward<Args>(args)...);
}

struct CollisionDiagnostics {
  std::vector<std::string> pairs;
  std::string raw_output;
};

struct TcpSpeedCheck {
  double max_speed_mps = 0.0;
  size_t segment_index = 0;
  double segment_dt_s = 0.0;
};

std::string join_strings(const std::vector<std::string>& values,
                         const std::string& separator) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) {
      result += separator;
    }
    result += value;
  }
  return result;
}

CollisionDiagnostics collect_collision_diagnostics(
    const std::shared_ptr<roboplan::Scene>& scene,
    const Eigen::VectorXd& q) {
  CollisionDiagnostics diagnostics;

  std::ostringstream capture;
  auto* previous = std::cout.rdbuf(capture.rdbuf());
  scene->hasCollisions(q, true);
  std::cout.rdbuf(previous);

  diagnostics.raw_output = capture.str();
  if (!diagnostics.raw_output.empty()) {
    std::cout << diagnostics.raw_output;
  }

  std::istringstream lines(diagnostics.raw_output);
  std::string line;
  const std::string prefix = "Collision detected between ";
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string pair = line.substr(prefix.size());
    const std::string needle = " and ";
    const auto split = pair.find(needle);
    if (split != std::string::npos) {
      pair.replace(split, needle.size(), " <-> ");
    }
    diagnostics.pairs.push_back(pair);
  }
  return diagnostics;
}

std::string collision_message_with_pairs(
    const std::string& message,
    const CollisionDiagnostics& diagnostics) {
  if (diagnostics.pairs.empty()) {
    return message;
  }
  return message + ": " + join_strings(diagnostics.pairs, "; ");
}

std::map<std::string, std::string> collision_details(
    std::string failure_stage,
    bool start_in_collision,
    bool goal_in_collision,
    const CollisionDiagnostics& diagnostics) {
  std::map<std::string, std::string> details;
  details["failure_stage"] = std::move(failure_stage);
  details["start_in_collision"] = start_in_collision ? "true" : "false";
  details["goal_in_collision"] = goal_in_collision ? "true" : "false";
  details["collision_pair_count"] = std::to_string(diagnostics.pairs.size());
  if (!diagnostics.pairs.empty()) {
    details["collision_pairs"] = join_strings(diagnostics.pairs, "; ");
  }
  for (size_t i = 0; i < diagnostics.pairs.size(); ++i) {
    details["collision_pair_" + std::to_string(i + 1)] = diagnostics.pairs[i];
  }
  return details;
}

TcpSpeedCheck compute_tcp_speed_check(
    const std::shared_ptr<roboplan::Scene>& scene,
    const std::string& group_name,
    const roboplan::JointTrajectory& trajectory,
    const std::string& tcp_frame) {
  TcpSpeedCheck check;
  Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
  bool have_prev = false;

  for (size_t i = 0; i < trajectory.times.size(); ++i) {
    const Eigen::VectorXd q_full =
        scene->toFullJointPositions(group_name, trajectory.positions[i]);
    Eigen::Matrix4d tform = scene->forwardKinematics(q_full, tcp_frame);
    Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

    if (have_prev && i > 0) {
      const double dt = trajectory.times[i] - trajectory.times[i - 1];
      if (dt > 1e-9) {
        const double speed = (pos - prev_pos).norm() / dt;
        if (speed > check.max_speed_mps) {
          check.max_speed_mps = speed;
          check.segment_index = i - 1;
          check.segment_dt_s = dt;
        }
      }
    }

    prev_pos = pos;
    have_prev = true;
  }

  return check;
}

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
    planner_debug("[PLANNER] plan() called, move_id={}, motion_type={}\n",
            request.move_id,
            std::holds_alternative<core::JointGoalDTO>(request.motion) ? "joint" : "pose");

    // Extract start state
    const auto& start = request.start_state;
    Eigen::VectorXd start_positions(start.positions_rad.size());
    for (size_t i = 0; i < start.positions_rad.size(); ++i) {
      start_positions[i] = start.positions_rad[i];
    }

    planner_debug("[PLANNER] start joints: [");
    for (size_t i = 0; i < start.positions_rad.size(); ++i)
      planner_debug("{}{:.4f}", (i ? ", " : ""), start.positions_rad[i]);
    planner_debug("]\n");

    roboplan::JointConfiguration start_config;
    start_config.joint_names = start.joint_names;
    start_config.positions = start_positions;

    const Eigen::VectorXd start_positions_full =
        scene_->toFullJointPositions(rrt_options_.group_name, start_positions);

    // Check start for self-collision
    bool start_collision = scene_->hasCollisions(start_positions_full);
    bool goal_collision = false;

    // FK of start pose
    {
      Eigen::Matrix4d start_fk =
          scene_->forwardKinematics(start_positions_full, request.tcp_frame);
      double r, p, ya;
      matrix_to_rpy_deg(start_fk, r, p, ya);
      planner_debug("[PLANNER] start pose:  pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f}) collision={}\n",
              start_fk(0, 3), start_fk(1, 3), start_fk(2, 3), r, p, ya,
              start_collision ? "YES" : "no");
    }

    if (start_collision) {
      auto diagnostics =
          collect_collision_diagnostics(scene_, start_positions_full);
      const std::string message = collision_message_with_pairs(
          "Planning preview rejected: start state is already in collision",
          diagnostics);
      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          message,
          collision_details("start_state", true, false, diagnostics),
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
        planner_debug("[PLANNER] goal pose:   pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f})\n",
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
        planner_debug("[PLANNER] IK FAILED\n");
        return result;
      }

      planner_debug("[PLANNER] IK solved. goal: [");
      for (int i = 0; i < goal_config.positions.size(); ++i)
        planner_debug("{}{:.4f}", (i ? ", " : ""), goal_config.positions[i]);
      planner_debug("]\n");

      // Check goal for self-collision
      const Eigen::VectorXd goal_positions_full =
          scene_->toFullJointPositions(rrt_options_.group_name, goal_config.positions);
      goal_collision = scene_->hasCollisions(goal_positions_full);
      planner_debug("[PLANNER] goal in collision: {}\n", goal_collision ? "YES" : "no");

      if (goal_collision) {
        auto diagnostics =
            collect_collision_diagnostics(scene_, goal_positions_full);
        const std::string message = collision_message_with_pairs(
            "Planning preview rejected: goal state is in collision",
            diagnostics);
        result.error = core::ErrorDTO{
            core::kErrorPlanningFailed,
            message,
            collision_details("goal_state", false, true, diagnostics),
        };
        return result;
      }

      // Compute joint-space distance
      double jdist = (goal_config.positions - start_config.positions).norm();
      planner_debug("[PLANNER] joint-space distance start->goal: {:.4f} rad\n", jdist);

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
    planner_debug("[PLANNER] calling RRT...\n");
    auto path_result = request_rrt.plan(start_config, goal_config);
    if (!path_result.has_value()) {
      planner_debug("[PLANNER] RRT FAILED: {}\n", path_result.error());

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
    planner_debug("[PLANNER] RRT succeeded\n");

    // Shortcut the path
    auto smoothed_path = shortcutter_->shortcut(path_result.value(), 0.05);

    // Time-parameterize with TOPPRA, enforcing TCP speed limit if set.
    // Iteratively reduce velocity_scale until TCP linear speed is within bounds.
    const double tcp_limit = request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
    const std::string& tcp_frame = request.tcp_frame;
    double vel_scale = request.speed_profile.velocity_scale;
    constexpr int kMaxTcpIterations = 32;
    constexpr double kTcpSpeedMargin = 0.99;  // aim slightly under the limit

    roboplan::JointTrajectory final_traj;
    TcpSpeedCheck final_tcp_check;
    int tcp_iterations_used = 0;

    for (int iter = 0; iter < kMaxTcpIterations; ++iter) {
      tcp_iterations_used = iter + 1;
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

      final_tcp_check = compute_tcp_speed_check(
          scene_, rrt_options_.group_name, final_traj, tcp_frame);

      // If within limit, done
      if (final_tcp_check.max_speed_mps <= tcp_limit) {
        break;
      }

      // Scale down proportionally: new_scale = old_scale * (limit / actual)
      double ratio = (tcp_limit * kTcpSpeedMargin) / final_tcp_check.max_speed_mps;
      vel_scale *= ratio;

      // Clamp to a reasonable minimum
      if (vel_scale < 0.001) {
        vel_scale = 0.001;
      }
    }

    if (tcp_limit > 0.0 && !tcp_frame.empty() &&
        final_tcp_check.max_speed_mps > tcp_limit) {
      std::map<std::string, std::string> details;
      details["failure_stage"] = "time_parameterization";
      details["limit_type"] = "tcp_linear_speed";
      details["measured"] = std::to_string(final_tcp_check.max_speed_mps);
      details["limit"] = std::to_string(tcp_limit);
      details["units"] = "m/s";
      details["tcp_frame"] = tcp_frame;
      details["waypoint_count"] = std::to_string(final_traj.times.size());
      details["trajectory_duration_s"] =
          final_traj.times.empty() ? "0.000000" : std::to_string(final_traj.times.back());
      details["max_segment_index"] = std::to_string(final_tcp_check.segment_index);
      details["max_segment_dt_s"] = std::to_string(final_tcp_check.segment_dt_s);
      details["toppra_iterations"] = std::to_string(tcp_iterations_used);
      details["final_velocity_scale"] = std::to_string(vel_scale);
      result.error = core::ErrorDTO{
          core::kErrorValidationLimitsViolation,
          "TCP linear speed exceeds limit after time-parameterization",
          details};
      return result;
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
