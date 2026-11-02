#include <manyforge_core/cache/trajectory_cache_compatibility.hpp>

#include <manyforge_core/ports/irobot_kinematics.hpp>
#include <manyforge_core/types/reason_codes.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>
#include <variant>

namespace manyforge::core {
namespace {

CacheCompatibilityResult compatible_cache() {
  return CacheCompatibilityResult{true, ""};
}

double quaternion_angular_distance_rad(const QuaternionDTO& a,
                                       const QuaternionDTO& b) {
  const double norm_a =
      std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double norm_b =
      std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (norm_a <= 0.0 || norm_b <= 0.0) {
    return std::numeric_limits<double>::infinity();
  }
  const double dot = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) /
                     (norm_a * norm_b);
  const double clamped = std::min(1.0, std::max(-1.0, std::abs(dot)));
  return 2.0 * std::acos(clamped);
}

double translation_distance_m(const Pose3DTO& a, const Pose3DTO& b) {
  const double dx = a.position_m.x_m - b.position_m.x_m;
  const double dy = a.position_m.y_m - b.position_m.y_m;
  const double dz = a.position_m.z_m - b.position_m.z_m;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool names_cover_positions(const std::vector<std::string>& names,
                           size_t position_count) {
  return !names.empty() && names.size() == position_count &&
         std::all_of(names.begin(), names.end(),
                     [](const std::string& name) { return !name.empty(); });
}

CacheCompatibilityResult cached_joint_goal_matches_request(
    const TrajectoryWaypointDTO& end_wp,
    const std::vector<ResourceId>& trajectory_resources,
    const std::vector<std::string>& trajectory_joint_names,
    const ResolvedJointGoalDTO& joint_goal,
    double tolerance_rad) {
  if (trajectory_resources.size() != trajectory_joint_names.size() ||
      trajectory_joint_names.size() != end_wp.positions_rad.size() ||
      joint_goal.resources.empty() ||
      joint_goal.resources.size() != joint_goal.joint_names.size() ||
      joint_goal.resources.size() != joint_goal.positions_rad.size()) {
    return incompatible_cache("goal_unverifiable");
  }
  for (size_t goal_idx = 0; goal_idx < joint_goal.resources.size();
       ++goal_idx) {
    const auto& goal_resource = joint_goal.resources[goal_idx];
    if (std::count(joint_goal.resources.begin(), joint_goal.resources.end(),
                   goal_resource) != 1 ||
        std::count(trajectory_resources.begin(), trajectory_resources.end(),
                   goal_resource) != 1) {
      return incompatible_cache("goal_unverifiable");
    }
    const auto trajectory_resource =
        std::find(trajectory_resources.begin(), trajectory_resources.end(),
                  goal_resource);
    const auto trajectory_index = static_cast<size_t>(
        std::distance(trajectory_resources.begin(), trajectory_resource));
    if (joint_goal.joint_names[goal_idx] !=
        trajectory_joint_names[trajectory_index]) {
      return incompatible_cache("goal_unverifiable");
    }
    if (std::abs(end_wp.positions_rad[trajectory_index] -
                 joint_goal.positions_rad[goal_idx]) > tolerance_rad) {
      return incompatible_cache("goal_changed");
    }
  }
  return compatible_cache();
}

CacheCompatibilityResult cached_pose_goal_matches_request(
    const IRobotKinematics& kinematics,
    const TrajectoryDTO& trajectory,
    const TrajectoryWaypointDTO& end_wp,
    const Pose3DTO& requested_goal,
    const ExecuteRequestDTO& request) {
  const auto& tcp_frame = compiled_frame_name(request.binding.tcp_frame);
  const auto& base_frame = compiled_frame_name(request.binding.base_frame);
  if (tcp_frame.empty() || base_frame.empty()) {
    return incompatible_cache("goal_unverifiable");
  }
  RobotStateDTO final_state;
  final_state.joint_names = trajectory.joint_names;
  final_state.positions_rad = end_wp.positions_rad;
  final_state.velocities_rad_s = end_wp.velocities_rad_s;

  try {
    auto final_tcp_pose_result = kinematics.forward_kinematics(
        FkRequestDTO{final_state, tcp_frame, base_frame});
    if (!final_tcp_pose_result.ok()) {
      return incompatible_cache("fk_failed");
    }
    const auto& final_tcp_pose = final_tcp_pose_result.value();
    if (translation_distance_m(final_tcp_pose, requested_goal) >
        request.tolerances.translation_tol_m) {
      return incompatible_cache("goal_changed");
    }
    if (quaternion_angular_distance_rad(final_tcp_pose.orientation,
                                        requested_goal.orientation) >
        request.tolerances.rotation_tol_rad) {
      return incompatible_cache("goal_changed");
    }
  } catch (const std::exception&) {
    return incompatible_cache("fk_failed");
  } catch (...) {
    return incompatible_cache(reason_codes::kUnknownNonStdException);
  }

  return compatible_cache();
}

}  // namespace

CacheCompatibilityResult incompatible_cache(std::string reason) {
  return CacheCompatibilityResult{false, std::move(reason)};
}

CacheCompatibilityResult cached_trajectory_matches_request_goal(
    const IRobotKinematics& kinematics,
    const TrajectoryDTO& trajectory,
    const ExecuteRequestDTO& request) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }
  if (trajectory.resources != request.binding.resources ||
      trajectory.joint_names !=
          request.binding.model_group_binding.compiled_joint_names) {
    return incompatible_cache("resource_projection_changed");
  }

  const auto& end_wp = trajectory.waypoints.back();
  if (const auto* joint_goal =
          std::get_if<ResolvedJointGoalDTO>(&request.motion)) {
    return cached_joint_goal_matches_request(
        end_wp, trajectory.resources, trajectory.joint_names, *joint_goal,
        request.tolerances.joint_position_tol_rad);
  }

  if (const auto* pose_goal = std::get_if<PoseGoalDTO>(&request.motion)) {
    return cached_pose_goal_matches_request(kinematics, trajectory, end_wp,
                                            pose_goal->goal, request);
  }

  if (const auto* cartesian_path =
          std::get_if<CartesianPathDTO>(&request.motion)) {
    if (cartesian_path->waypoints.empty()) {
      return incompatible_cache("goal_unverifiable");
    }
    return cached_pose_goal_matches_request(kinematics, trajectory, end_wp,
                                            cartesian_path->waypoints.back(),
                                            request);
  }

  return incompatible_cache("goal_unverifiable");
}

CacheCompatibilityResult cached_trajectory_matches_current_start(
    const TrajectoryDTO& trajectory,
    const RobotStateDTO& current_state,
    double tolerance_rad) {
  if (trajectory.waypoints.empty()) {
    return incompatible_cache("empty_trajectory");
  }
  const auto& start_wp = trajectory.waypoints.front();
  if (start_wp.positions_rad.size() != current_state.positions_rad.size()) {
    return incompatible_cache("start_state_changed");
  }

  const bool trajectory_names_complete =
      names_cover_positions(trajectory.joint_names, start_wp.positions_rad.size());
  const bool state_names_complete = names_cover_positions(
      current_state.joint_names, current_state.positions_rad.size());

  if (trajectory_names_complete && state_names_complete) {
    for (size_t state_idx = 0; state_idx < current_state.joint_names.size();
         ++state_idx) {
      const auto& state_name = current_state.joint_names[state_idx];
      const auto matches = std::count(trajectory.joint_names.begin(),
                                      trajectory.joint_names.end(), state_name);
      if (matches != 1) {
        return incompatible_cache("start_state_changed");
      }
      const auto traj_it = std::find(trajectory.joint_names.begin(),
                                     trajectory.joint_names.end(), state_name);
      const auto traj_idx =
          static_cast<size_t>(std::distance(trajectory.joint_names.begin(),
                                            traj_it));
      if (std::abs(start_wp.positions_rad[traj_idx] -
                   current_state.positions_rad[state_idx]) > tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }

  if (trajectory.joint_names.empty() && current_state.joint_names.empty()) {
    for (size_t i = 0; i < start_wp.positions_rad.size(); ++i) {
      if (std::abs(start_wp.positions_rad[i] -
                   current_state.positions_rad[i]) > tolerance_rad) {
        return incompatible_cache("start_state_changed");
      }
    }
    return compatible_cache();
  }
  return incompatible_cache("start_state_changed");
}

}  // namespace manyforge::core
