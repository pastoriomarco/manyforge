#include <manyforge_roboplan/roboplan_planning_context.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>

#include <roboplan/core/scene_utils.hpp>
#include <stdexcept>
#include <string>
#include <variant>

// ----------------------------------------------------------------------------
// RoboPlan Scene mutation notes
//
// Older RoboPlan revisions required a ManyForge-side LIFO remove/replay
// workaround for scene-object mutation. The pinned RoboPlan main now owns the
// geometry bookkeeping invariants directly: removals repair the name->index
// map, reparenting updates parentJoint, and add*Geometry passes Pinocchio's
// JointIndex/FrameIndex arguments in the current order. This adapter therefore
// uses RoboPlan's direct scene APIs again and keeps object_states_ only as the
// semantic ManyForge view exposed to the Composer/HMI layers.
// ----------------------------------------------------------------------------

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

std::vector<std::filesystem::path> split_env_paths(const char* value) {
  std::vector<std::filesystem::path> paths;
  if (value == nullptr || *value == '\0') {
    return paths;
  }
  std::stringstream stream(value);
  std::string item;
  while (std::getline(stream, item, ':')) {
    if (!item.empty()) {
      paths.emplace_back(item);
    }
  }
  return paths;
}

std::optional<std::filesystem::path> first_existing_path(
    const std::vector<std::filesystem::path>& candidates) {
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
      return std::filesystem::canonical(candidate, ec);
    }
  }
  return std::nullopt;
}

tl::expected<std::filesystem::path, std::string> resolve_mesh_resource_uri(
    const std::string& resource_uri) {
  if (resource_uri.empty()) {
    return tl::make_unexpected("mesh_resource_uri must not be empty");
  }

  std::string path_text = resource_uri;
  constexpr const char* kFilePrefix = "file://";
  constexpr const char* kPackagePrefix = "package://";
  if (path_text.rfind(kFilePrefix, 0) == 0) {
    path_text = path_text.substr(std::char_traits<char>::length(kFilePrefix));
  } else if (path_text.rfind(kPackagePrefix, 0) == 0) {
    const std::string rest =
        path_text.substr(std::char_traits<char>::length(kPackagePrefix));
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      return tl::make_unexpected("Invalid package:// mesh URI: " + resource_uri);
    }
    const std::string package_name = rest.substr(0, slash);
    const std::filesystem::path package_rel_path = rest.substr(slash + 1);

    std::vector<std::filesystem::path> candidates;
    for (const auto& prefix : split_env_paths(std::getenv("AMENT_PREFIX_PATH"))) {
      candidates.push_back(prefix / "share" / package_name / package_rel_path);
    }
    for (const auto& prefix : split_env_paths(std::getenv("COLCON_PREFIX_PATH"))) {
      candidates.push_back(prefix / "share" / package_name / package_rel_path);
    }
    for (const auto& root : split_env_paths(std::getenv("ROS_PACKAGE_PATH"))) {
      candidates.push_back(root / package_name / package_rel_path);
      if (root.filename() == package_name) {
        candidates.push_back(root / package_rel_path);
      }
    }
    candidates.push_back(std::filesystem::path("/opt/ros/jazzy/share") /
                         package_name / package_rel_path);
    candidates.push_back(std::filesystem::path("/usr/local/share") /
                         package_name / package_rel_path);
    candidates.push_back(std::filesystem::path("/workspace") /
                         package_name / package_rel_path);

    if (auto resolved = first_existing_path(candidates)) {
      return *resolved;
    }
    return tl::make_unexpected("Could not resolve mesh URI: " + resource_uri);
  }

  const std::filesystem::path candidate(path_text);
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(candidate);
  if (candidate.is_relative()) {
    candidates.push_back(std::filesystem::current_path() / candidate);
    candidates.push_back(std::filesystem::path("/workspace") / candidate);
  }

  if (auto resolved = first_existing_path(candidates)) {
    return *resolved;
  }
  return tl::make_unexpected("Mesh resource does not exist: " + resource_uri);
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

void RoboPlanPlanningContext::set_robot_base_pose(const core::Pose3DTO& pose) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  world_from_robot_base_ = pose_to_matrix(pose);

  for (const auto& [object_id, tracked] : object_states_) {
    Eigen::Matrix4d t_scene_object = scene_placement_from_tracked_locked(tracked);
    const std::string target_frame =
        is_world_frame(tracked.frame_id) ? "universe" : tracked.frame_id;
    auto r = scene_->updateGeometryPlacement(object_id, target_frame, t_scene_object);
    if (!r.has_value()) {
      throw std::runtime_error(
          "Failed to update object '" + object_id + "' after robot base pose change: " +
          r.error());
    }
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
    const size_t waypoint_count = trajectory.waypoints.size();
    const double trajectory_duration_s =
        trajectory.waypoints.empty()
            ? 0.0
            : trajectory.waypoints.back().time_from_start_s;
    uint64_t speed_validation_duration_ns = 0;
    uint64_t collision_validation_duration_ns = 0;

    // TCP speed validation is intentionally first. It is cheap FK-only work and
    // can reject infeasible trajectories before expensive mesh collision sweeps.
    if (options.max_tcp_linear_speed_mps > 0.0 &&
        !options.tcp_frame.empty() && waypoint_count >= 2) {
      const uint64_t speed_start_ns = steady_now_ns();
      Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
      bool have_prev = false;
      double max_speed = 0.0;
      size_t max_segment_index = 0;
      double max_segment_dt_s = 0.0;

      for (size_t i = 0; i < waypoint_count; ++i) {
        const auto& wp = trajectory.waypoints[i];
        std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
        Eigen::VectorXd q = vector_from_std(wp.positions_rad);
        if (q.size() != scene_->getCurrentJointPositions().size()) {
          q = scene_->toFullJointPositions(group_name_, q);
        }

        Eigen::Matrix4d tform = scene_->forwardKinematics(q, options.tcp_frame);
        Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

        if (have_prev && i > 0) {
          const double dt =
              wp.time_from_start_s - trajectory.waypoints[i - 1].time_from_start_s;
          if (dt > 1e-9) {
            const double speed = (pos - prev_pos).norm() / dt;
            if (speed > max_speed) {
              max_speed = speed;
              max_segment_index = i - 1;
              max_segment_dt_s = dt;
            }
          }
        }

        prev_pos = pos;
        have_prev = true;
      }
      speed_validation_duration_ns = steady_now_ns() - speed_start_ns;

      if (max_speed > options.max_tcp_linear_speed_mps) {
        std::map<std::string, std::string> details;
        details["limit_type"] = "tcp_linear_speed";
        details["measured"] = std::to_string(max_speed);
        details["limit"] = std::to_string(options.max_tcp_linear_speed_mps);
        details["units"] = "m/s";
        details["tcp_frame"] = options.tcp_frame;
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["max_segment_index"] = std::to_string(max_segment_index);
        details["max_segment_dt_s"] = std::to_string(max_segment_dt_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] = "0";
        result.error = core::ErrorDTO{
            core::kErrorValidationLimitsViolation,
            "TCP linear speed exceeds limit", details};
        return result;
      }
    }

    // Collision check at each waypoint.
    //
    // Support contacts emulate a MoveIt-style per-request ACM in the RoboPlan
    // adapter. Declared support pairs are disabled in the planner projection so
    // RRT can start from a resting grasp, but validation temporarily re-enables
    // them and enforces release semantics over the generated trajectory.
    const uint64_t collision_start_ns = steady_now_ns();
    {
      std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
      bool support_contacts_exposed = false;
      auto restore_support_contacts = [&]() {
        if (!support_contacts_exposed) {
          return;
        }
        (void)set_support_contact_pairs_for_validation_locked(false);
        support_contacts_exposed = false;
      };
      const auto support_guard =
          std::shared_ptr<void>(nullptr, [&](void*) { restore_support_contacts(); });
      (void)support_guard;
      auto expose_support_contacts =
          set_support_contact_pairs_for_validation_locked(true);
      if (!expose_support_contacts.has_value()) {
        result.error = core::ErrorDTO{
            core::kErrorInternalError,
            "Failed to expose support-contact pairs for validation: " +
                expose_support_contacts.error(),
            {},
        };
        return result;
      }
      support_contacts_exposed = true;

      std::set<std::pair<std::string, std::string>> start_support_pairs;
      std::set<std::pair<std::string, std::string>> separated_support_pairs;

      for (size_t i = 0; i < waypoint_count; ++i) {
        const auto& wp = trajectory.waypoints[i];
        Eigen::VectorXd q = vector_from_std(wp.positions_rad);
        if (q.size() != scene_->getCurrentJointPositions().size()) {
          q = scene_->toFullJointPositions(group_name_, q);
        }
        if (!scene_->hasCollisions(q)) {
          if (i > 0) {
            for (const auto& pair : start_support_pairs) {
              separated_support_pairs.insert(pair);
            }
          }
          continue;
        }

        CollisionDiagnostics diagnostics = collect_collision_diagnostics_locked(q);
        if (diagnostics.pairs.empty()) {
          collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
          std::map<std::string, std::string> details;
          details["failure_stage"] = "collision_validation_unstructured";
          details["waypoint_index"] = std::to_string(i);
          details["waypoint_count"] = std::to_string(waypoint_count);
          details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
          details["speed_validation_duration_ns"] =
              std::to_string(speed_validation_duration_ns);
          details["collision_validation_duration_ns"] =
              std::to_string(collision_validation_duration_ns);
          details["collision_pair_count"] = "0";
          details["collision_diagnostics_available"] = "false";
          restore_support_contacts();
          result.error = core::ErrorDTO{
              core::kErrorValidationCollision,
              "Trajectory passes through collision, but RoboPlan did not report collision pairs",
              details,
          };
          return result;
        }
        std::vector<CollisionPairDiagnostic> blocking_pairs;
        std::set<std::pair<std::string, std::string>> colliding_support_pairs;

        for (const auto& pair : diagnostics.pairs) {
          if (is_active_support_contact_pair_locked(pair.canonical_pair)) {
            colliding_support_pairs.insert(pair.canonical_pair);
          } else {
            blocking_pairs.push_back(pair);
          }
        }

        auto fail_with_diagnostics = [&](const std::string& stage,
                                         const std::string& message) {
          collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
          std::map<std::string, std::string> details;
          details["failure_stage"] = stage;
          details["waypoint_index"] = std::to_string(i);
          details["waypoint_count"] = std::to_string(waypoint_count);
          details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
          details["speed_validation_duration_ns"] =
              std::to_string(speed_validation_duration_ns);
          details["collision_validation_duration_ns"] =
              std::to_string(collision_validation_duration_ns);
          add_collision_details(details, diagnostics);
          restore_support_contacts();
          result.error = core::ErrorDTO{
              core::kErrorValidationCollision,
              message,
              details,
          };
        };

        if (!blocking_pairs.empty()) {
          fail_with_diagnostics(
              "collision_validation",
              "Trajectory passes through collision");
          return result;
        }

        if (i == 0) {
          start_support_pairs = colliding_support_pairs;
          continue;
        }

        for (const auto& pair : colliding_support_pairs) {
          if (start_support_pairs.find(pair) == start_support_pairs.end()) {
            fail_with_diagnostics(
                "support_contact_unexpected_path_contact",
                "Support-contact pair collided after the start waypoint");
            return result;
          }
          if (separated_support_pairs.find(pair) != separated_support_pairs.end()) {
            fail_with_diagnostics(
                "support_contact_recontact_after_release",
                "Support-contact pair re-contacted after separation");
            return result;
          }
        }

        for (const auto& pair : start_support_pairs) {
          if (colliding_support_pairs.find(pair) == colliding_support_pairs.end()) {
            separated_support_pairs.insert(pair);
          }
        }
      }

      for (const auto& pair : start_support_pairs) {
        const auto policy = support_contact_policy_for_pair_locked(pair);
        const bool require_separation =
            !policy.has_value() || policy->require_separation_by_end;
        if (!require_separation ||
            separated_support_pairs.find(pair) != separated_support_pairs.end()) {
          continue;
        }
        collision_validation_duration_ns = steady_now_ns() - collision_start_ns;
        std::map<std::string, std::string> details;
        details["failure_stage"] = "support_contact_not_released";
        details["support_contact_pair"] = collision_pair_text(pair);
        details["waypoint_count"] = std::to_string(waypoint_count);
        details["trajectory_duration_s"] = std::to_string(trajectory_duration_s);
        details["speed_validation_duration_ns"] =
            std::to_string(speed_validation_duration_ns);
        details["collision_validation_duration_ns"] =
            std::to_string(collision_validation_duration_ns);
        if (policy.has_value()) {
          details["max_start_penetration_m"] =
              std::to_string(policy->max_start_penetration_m);
          details["support_contact_depth_enforced"] = "false";
        }
        restore_support_contacts();
        result.error = core::ErrorDTO{
            core::kErrorValidationCollision,
            "Support-contact pair did not separate by final waypoint",
            details,
        };
        return result;
      }

      restore_support_contacts();
    }
    collision_validation_duration_ns = steady_now_ns() - collision_start_ns;

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
  if (result.in_collision) {
    auto diagnostics = collect_collision_diagnostics_locked(q);
    if (!diagnostics.pairs.empty()) {
      result.contact_body_a = diagnostics.pairs.front().canonical_pair.first;
      result.contact_body_b = diagnostics.pairs.front().canonical_pair.second;
    }
  }

  return result;
}

core::Pose3DTO RoboPlanPlanningContext::forward_kinematics(
    const core::FkRequestDTO& request) {
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }

  const Eigen::Matrix4d t_robot_base_target =
      is_world_frame(request.target_frame)
          ? Eigen::Matrix4d::Identity()
          : scene_->forwardKinematics(q, request.target_frame);
  if (is_world_frame(request.base_frame)) {
    return matrix_to_pose(world_from_robot_base_ * t_robot_base_target);
  }

  const Eigen::Matrix4d t_robot_base_requested_base =
      scene_->forwardKinematics(q, request.base_frame);
  return matrix_to_pose(t_robot_base_requested_base.inverse() * t_robot_base_target);
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

bool RoboPlanPlanningContext::is_world_frame(const std::string& frame_id) const {
  return frame_id.empty() || frame_id == "universe" || frame_id == "world" ||
         frame_id == "World" || frame_id == "/World";
}

Eigen::Matrix4d RoboPlanPlanningContext::scene_placement_from_tracked_locked(
    const TrackedObjectState& state) {
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(state.pose);
  if (is_world_frame(state.frame_id)) {
    return world_from_robot_base_.inverse() * t_frame_object;
  }
  return parent_joint_from_frame_matrix_locked(state.frame_id) * t_frame_object;
}

Eigen::Matrix4d RoboPlanPlanningContext::parent_joint_from_frame_matrix_locked(
    const std::string& frame_id) {
  if (is_world_frame(frame_id)) {
    return Eigen::Matrix4d::Identity();
  }

  const auto maybe_frame_id = scene_->getFrameId(frame_id);
  if (!maybe_frame_id.has_value()) {
    throw std::runtime_error(
        "Could not resolve RoboPlan frame '" + frame_id + "': " +
        maybe_frame_id.error());
  }

  const auto& model = scene_->getModel();
  const auto resolved_frame_id = maybe_frame_id.value();
  if (static_cast<size_t>(resolved_frame_id) >= model.frames.size()) {
    throw std::runtime_error(
        "RoboPlan frame id for '" + frame_id + "' is out of range.");
  }

  // Pinocchio GeometryObject::placement is evaluated in the parent joint frame,
  // while ManyForge stores object poses in the named semantic frame.
  return model.frames.at(resolved_frame_id).placement.toHomogeneousMatrix();
}

Eigen::Matrix4d RoboPlanPlanningContext::frame_to_world_matrix_locked(
    const std::string& frame_id) {
  if (is_world_frame(frame_id)) {
    return Eigen::Matrix4d::Identity();
  }

  const auto& positions = scene_->getCurrentJointPositions();
  Eigen::VectorXd q(positions.size());
  for (Eigen::Index i = 0; i < positions.size(); ++i) {
    q[i] = positions[i];
  }
  return world_from_robot_base_ * scene_->forwardKinematics(q, frame_id);
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
  return !is_world_frame(frame_id) && frame_id != "base_link";
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

// Temporary RoboPlan adapter policy:
// Scene::hasCollisions(q) currently validates the whole Pinocchio geometry
// model, so detached scene object <-> detached scene object contacts can reject
// an otherwise valid robot start/goal state. ManyForge treats those contacts as
// scene-quality data, not motion blockers. Until RoboPlan exposes a robot-state
// collision query that filters static-static pairs, keep detached object pairs
// disabled in the backend projection and re-enable a pair as soon as either
// object is attached to a robot link.
std::pair<std::string, std::string> RoboPlanPlanningContext::canonical_collision_pair(
    const std::string& body1,
    const std::string& body2) const {
  if (body1 < body2) {
    return {body1, body2};
  }
  return {body2, body1};
}

std::string RoboPlanPlanningContext::collision_pair_text(
    const std::pair<std::string, std::string>& pair) const {
  return pair.first + " <-> " + pair.second;
}

RoboPlanPlanningContext::CollisionDiagnostics
RoboPlanPlanningContext::collect_collision_diagnostics_locked(
    const Eigen::VectorXd& q) {
  CollisionDiagnostics diagnostics;

  std::ostringstream capture;
  auto* previous = std::cout.rdbuf(capture.rdbuf());
  scene_->hasCollisions(q, true);
  std::cout.rdbuf(previous);

  diagnostics.raw_output = capture.str();

  std::istringstream lines(diagnostics.raw_output);
  std::string line;
  const std::string prefix = "Collision detected between ";
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    const std::string pair_text = line.substr(prefix.size());
    const std::string needle = " and ";
    const auto split = pair_text.find(needle);
    if (split == std::string::npos) {
      continue;
    }
    CollisionPairDiagnostic pair;
    pair.body_a = pair_text.substr(0, split);
    pair.body_b = pair_text.substr(split + needle.size());
    pair.canonical_pair = canonical_collision_pair(pair.body_a, pair.body_b);
    pair.text = collision_pair_text(pair.canonical_pair);
    diagnostics.pairs.push_back(std::move(pair));
  }

  return diagnostics;
}

void RoboPlanPlanningContext::add_collision_details(
    std::map<std::string, std::string>& details,
    const CollisionDiagnostics& diagnostics) const {
  details["collision_pair_count"] = std::to_string(diagnostics.pairs.size());
  std::string joined;
  for (size_t i = 0; i < diagnostics.pairs.size(); ++i) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += diagnostics.pairs[i].text;
    details["collision_pair_" + std::to_string(i + 1)] =
        diagnostics.pairs[i].text;
  }
  if (!joined.empty()) {
    details["collision_pairs"] = joined;
    details["contact_body_a"] = diagnostics.pairs.front().canonical_pair.first;
    details["contact_body_b"] = diagnostics.pairs.front().canonical_pair.second;
  }
}

tl::expected<void, std::string>
RoboPlanPlanningContext::apply_collision_pair_projection_locked(
    const std::pair<std::string, std::string>& pair) {
  const bool should_disable =
      disabled_static_collision_pairs_.find(pair) != disabled_static_collision_pairs_.end() ||
      disabled_support_contact_pairs_.find(pair) != disabled_support_contact_pairs_.end();
  auto result = scene_->setCollisions(pair.first, pair.second, !should_disable);
  if (!result.has_value()) {
    return tl::make_unexpected(result.error());
  }
  return {};
}

tl::expected<void, std::string>
RoboPlanPlanningContext::apply_static_collision_policy_for_object_locked(
    const std::string& object_id) {
  const auto target_it = object_states_.find(object_id);
  if (target_it == object_states_.end()) {
    return {};
  }

  for (const auto& [other_id, other_state] : object_states_) {
    if (other_id == object_id) {
      continue;
    }

    const auto pair = canonical_collision_pair(object_id, other_id);
    const bool should_disable =
        !target_it->second.attached && !other_state.attached;
    const bool is_disabled =
        disabled_static_collision_pairs_.find(pair) !=
        disabled_static_collision_pairs_.end();

    if (should_disable == is_disabled) {
      continue;
    }

    if (should_disable) {
      disabled_static_collision_pairs_.insert(pair);
    } else {
      disabled_static_collision_pairs_.erase(pair);
    }
    auto result = apply_collision_pair_projection_locked(pair);
    if (!result.has_value()) {
      return result;
    }
  }

  return {};
}

void RoboPlanPlanningContext::erase_static_collision_policy_for_object_locked(
    const std::string& object_id) {
  for (auto it = disabled_static_collision_pairs_.begin();
       it != disabled_static_collision_pairs_.end();) {
    if (it->first == object_id || it->second == object_id) {
      const auto pair = *it;
      it = disabled_static_collision_pairs_.erase(it);
      (void)apply_collision_pair_projection_locked(pair);
    } else {
      ++it;
    }
  }
}

tl::expected<void, std::string>
RoboPlanPlanningContext::recompute_support_contact_policy_locked() {
  std::set<std::pair<std::string, std::string>> candidate_pairs;
  std::set<std::pair<std::string, std::string>> explicit_pairs;

  // Automatic support-contact discovery:
  // an attached object is part of the moving system, so only its current
  // contacts with detached world objects are allowed to seed a support release
  // phase. Other attached-object/world-object pairs remain enabled and will
  // block planning/validation as normal obstacles.
  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.attached) {
      continue;
    }
    for (const auto& [other_id, other_state] : object_states_) {
      if (other_id == object_id || other_state.attached) {
        continue;
      }
      candidate_pairs.insert(canonical_collision_pair(object_id, other_id));
    }
  }

  // Explicit scene metadata is kept as an override for hand-authored programs
  // while RoboPlan lacks a first-class scoped-contact query. It is no longer the
  // primary authoring mechanism: current attached-object contacts are discovered
  // dynamically above.
  std::set<std::pair<std::string, std::string>> desired_pairs;
  for (const auto& [object_id, tracked] : object_states_) {
    if (!tracked.attached) {
      continue;
    }
    for (const auto& support_contact : tracked.support_contacts) {
      if (support_contact.support_object_id.empty()) {
        continue;
      }
      const auto support_it = object_states_.find(support_contact.support_object_id);
      if (support_it == object_states_.end() || support_it->second.attached) {
        continue;
      }
      explicit_pairs.insert(
          canonical_collision_pair(object_id, support_contact.support_object_id));
    }
  }

  for (const auto& pair : candidate_pairs) {
    auto result = scene_->setCollisions(pair.first, pair.second, true);
    if (!result.has_value()) {
      return tl::make_unexpected(result.error());
    }
  }

  if (!candidate_pairs.empty()) {
    const auto q = scene_->getCurrentJointPositions();
    const auto diagnostics = collect_collision_diagnostics_locked(q);
    for (const auto& diagnostic : diagnostics.pairs) {
      if (candidate_pairs.find(diagnostic.canonical_pair) != candidate_pairs.end()) {
        desired_pairs.insert(diagnostic.canonical_pair);
      }
    }
  }
  desired_pairs.insert(explicit_pairs.begin(), explicit_pairs.end());

  std::set<std::pair<std::string, std::string>> touched_pairs =
      disabled_support_contact_pairs_;
  touched_pairs.insert(candidate_pairs.begin(), candidate_pairs.end());
  touched_pairs.insert(desired_pairs.begin(), desired_pairs.end());

  disabled_support_contact_pairs_ = std::move(desired_pairs);
  for (const auto& pair : touched_pairs) {
    if (object_states_.find(pair.first) == object_states_.end() ||
        object_states_.find(pair.second) == object_states_.end()) {
      continue;
    }
    auto result = apply_collision_pair_projection_locked(pair);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

tl::expected<void, std::string>
RoboPlanPlanningContext::set_support_contact_pairs_for_validation_locked(
    bool enable) {
  const auto pairs = enable
      ? active_support_contact_pairs_for_validation_locked()
      : disabled_support_contact_pairs_;
  for (const auto& pair : pairs) {
    auto result = scene_->setCollisions(pair.first, pair.second, enable);
    if (!result.has_value()) {
      return tl::make_unexpected(result.error());
    }
  }
  return {};
}

std::set<std::pair<std::string, std::string>>
RoboPlanPlanningContext::active_support_contact_pairs_for_validation_locked() const {
  return disabled_support_contact_pairs_;
}

bool RoboPlanPlanningContext::is_active_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  const auto active_pairs = active_support_contact_pairs_for_validation_locked();
  return active_pairs.find(pair) != active_pairs.end();
}

bool RoboPlanPlanningContext::is_support_contact_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  return disabled_support_contact_pairs_.find(pair) !=
         disabled_support_contact_pairs_.end();
}

std::optional<core::SupportContactDTO>
RoboPlanPlanningContext::support_contact_policy_for_pair_locked(
    const std::pair<std::string, std::string>& pair) const {
  for (const auto& [object_id, tracked] : object_states_) {
    for (const auto& support_contact : tracked.support_contacts) {
      if (support_contact.support_object_id.empty()) {
        continue;
      }
      if (canonical_collision_pair(object_id, support_contact.support_object_id) == pair) {
        return support_contact;
      }
    }
  }
  return std::nullopt;
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

tl::expected<void, std::string> RoboPlanPlanningContext::add_geometry_locked(
    const std::string& object_id, const TrackedObjectState& state) {
  const std::string target_frame =
      is_world_frame(state.frame_id) ? "universe" : state.frame_id;
  Eigen::Matrix4d t_target_object = scene_placement_from_tracked_locked(state);

  const Eigen::Vector4d color(1.0, 0.0, 0.0, 1.0);
  if (auto* box = std::get_if<core::BoxShapeDTO>(&state.shape)) {
    roboplan::Box b(box->dimensions_m.x_m, box->dimensions_m.y_m, box->dimensions_m.z_m);
    auto r = scene_->addBoxGeometry(object_id, target_frame, b, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else if (auto* sphere = std::get_if<core::SphereShapeDTO>(&state.shape)) {
    roboplan::Sphere s(sphere->radius_m);
    auto r = scene_->addSphereGeometry(object_id, target_frame, s, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else if (auto* mesh = std::get_if<core::MeshShapeDTO>(&state.shape)) {
    auto mesh_path = resolve_mesh_resource_uri(mesh->resource_uri);
    if (!mesh_path.has_value()) {
      return tl::make_unexpected(mesh_path.error());
    }
    const Eigen::Vector3d mesh_scale(
        mesh->scale.x_m == 0.0 ? 1.0 : mesh->scale.x_m,
        mesh->scale.y_m == 0.0 ? 1.0 : mesh->scale.y_m,
        mesh->scale.z_m == 0.0 ? 1.0 : mesh->scale.z_m);
    roboplan::Mesh mesh_geometry(*mesh_path, mesh_scale);
    auto r = scene_->addMeshGeometry(
        object_id, target_frame, mesh_geometry, t_target_object, color);
    if (!r.has_value()) {
      return tl::make_unexpected(r.error());
    }
  } else {
    return tl::make_unexpected(
        "add_geometry_locked: unsupported shape type for object '" + object_id + "'");
  }
  return {};
}

core::SceneEditResultDTO RoboPlanPlanningContext::reparent_object_locked(
    const std::string& object_id, const std::string& target_frame_id, bool emit_event) {
  core::SceneEditResultDTO result;
  const std::string canonical_target_frame =
      is_world_frame(target_frame_id) ? "universe" : target_frame_id;
  const auto it = object_states_.find(object_id);
  if (it == object_states_.end()) {
    result.error = core::ErrorDTO{
        core::kErrorObjectNotFound, "Object not found: " + object_id, {}};
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  // Compute target-frame pose from current world pose.
  const Eigen::Matrix4d t_frame_object = pose_to_matrix(it->second.pose);
  const Eigen::Matrix4d t_world_frame = frame_to_world_matrix_locked(it->second.frame_id);
  const Eigen::Matrix4d t_world_object = t_world_frame * t_frame_object;
  const Eigen::Matrix4d t_world_target = frame_to_world_matrix_locked(canonical_target_frame);
  const Eigen::Matrix4d t_target_object = t_world_target.inverse() * t_world_object;

  TrackedObjectState new_state = it->second;
  new_state.pose = matrix_to_pose(t_target_object);
  new_state.frame_id = canonical_target_frame;
  new_state.attached = infer_attached_from_frame(canonical_target_frame);

  Eigen::Matrix4d t_scene_object = scene_placement_from_tracked_locked(new_state);
  const std::string scene_target_frame =
      is_world_frame(new_state.frame_id) ? "universe" : new_state.frame_id;
  auto r = scene_->updateGeometryPlacement(object_id, scene_target_frame, t_scene_object);
  if (!r.has_value()) {
    result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }
  object_states_[object_id] = new_state;
  auto policy_result = apply_static_collision_policy_for_object_locked(object_id);
  if (!policy_result.has_value()) {
    result.error = core::ErrorDTO{
        core::kErrorInternalError,
        "Failed to apply static object collision policy: " + policy_result.error(),
        {},
    };
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }
  auto support_policy_result = recompute_support_contact_policy_locked();
  if (!support_policy_result.has_value()) {
    result.error = core::ErrorDTO{
        core::kErrorInternalError,
        "Failed to apply support-contact collision policy: " +
            support_policy_result.error(),
        {},
    };
    SceneTraceEvent event;
    event.event = is_world_frame(canonical_target_frame) ? "detach" : "attach";
    event.object_id = object_id;
    event.success = false;
    event.frame_id = canonical_target_frame;
    event.attached = it->second.attached;
    event.pose_in_frame = it->second.pose;
    event.pose_in_universe = pose_in_base_from_tracked_locked(it->second, "universe");
    event.error = result.error->message;
    if (emit_event) {
      record_scene_event_locked(std::move(event));
    }
    return result;
  }

  const auto& tracked = object_states_.at(object_id);
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

    const std::string final_frame_id =
        is_world_frame(request.frame_id) ? "universe" : request.frame_id;
    const bool attached = infer_attached_from_frame(final_frame_id);

    TrackedObjectState new_state{
        request.pose,
        request.shape,
        final_frame_id,
        attached,
        {},
        request.support_contacts,
    };

    auto add_r = add_geometry_locked(request.object_id, new_state);
    if (!add_r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, add_r.error(), {}};
      return result;
    }
    object_states_[request.object_id] = new_state;
    auto policy_result =
        apply_static_collision_policy_for_object_locked(request.object_id);
    if (!policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply static object collision policy: " + policy_result.error(),
          {},
      };
      return result;
    }
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy: " +
              support_policy_result.error(),
          {},
      };
      return result;
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
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      result.error = core::ErrorDTO{
          core::kErrorObjectNotFound,
          "Could not find object '" + request.object_id + "' to remove.",
          {}};
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    const TrackedObjectState tracked_before = it->second;

    auto r = scene_->removeGeometry(request.object_id);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
      SceneTraceEvent event;
      event.event = "remove";
      event.object_id = request.object_id;
      event.success = false;
      event.error = result.error->message;
      record_scene_event_locked(std::move(event));
      return result;
    }
    erase_static_collision_policy_for_object_locked(request.object_id);
    object_states_.erase(request.object_id);
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy after remove: " +
              support_policy_result.error(),
          {},
      };
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
    event.frame_id = tracked_before.frame_id;
    event.attached = tracked_before.attached;
    event.pose_in_frame = tracked_before.pose;
    event.pose_in_universe =
        pose_in_base_from_tracked_locked(tracked_before, "universe");
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
    const auto it = object_states_.find(request.object_id);
    if (it == object_states_.end()) {
      result.error = core::ErrorDTO{
          core::kErrorObjectNotFound,
          "Could not find object '" + request.object_id + "' to update.",
          {}};
      return result;
    }

    TrackedObjectState new_state = it->second;
    new_state.pose = request.pose;
    new_state.frame_id = is_world_frame(request.frame_id) ? "universe" : request.frame_id;
    new_state.attached = infer_attached_from_frame(new_state.frame_id);

    Eigen::Matrix4d tform = scene_placement_from_tracked_locked(new_state);
    const std::string scene_target_frame =
        is_world_frame(new_state.frame_id) ? "universe" : new_state.frame_id;
    auto r = scene_->updateGeometryPlacement(request.object_id, scene_target_frame, tform);
    if (!r.has_value()) {
      result.error = core::ErrorDTO{core::kErrorInternalError, r.error(), {}};
      return result;
    }
    object_states_[request.object_id] = new_state;
    auto policy_result =
        apply_static_collision_policy_for_object_locked(request.object_id);
    if (!policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply static object collision policy: " + policy_result.error(),
          {},
      };
      return result;
    }
    auto support_policy_result = recompute_support_contact_policy_locked();
    if (!support_policy_result.has_value()) {
      result.error = core::ErrorDTO{
          core::kErrorInternalError,
          "Failed to apply support-contact collision policy: " +
              support_policy_result.error(),
          {},
      };
      return result;
    }

    const auto& tracked = object_states_.at(request.object_id);
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
