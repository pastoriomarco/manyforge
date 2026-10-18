#include <manyforge_roboplan/retained_focused_adapters.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO adapter_error(
    core::ErrorCode code, std::string message, std::string reason) {
  return core::ErrorDTO{std::string(core::to_string(code)),
                        std::move(message),
                        {{"reason", std::move(reason)}}};
}

[[nodiscard]] Eigen::VectorXd vector_from_std(
    const std::vector<double>& values) {
  Eigen::VectorXd result(static_cast<Eigen::Index>(values.size()));
  for (std::size_t index = 0U; index < values.size(); ++index) {
    result[static_cast<Eigen::Index>(index)] = values[index];
  }
  return result;
}

[[nodiscard]] bool is_world_frame(const std::string& frame_id) {
  return frame_id.empty() || frame_id == "universe" || frame_id == "world" ||
         frame_id == "World" || frame_id == "/World";
}

[[nodiscard]] core::Pose3DTO matrix_to_pose(const Eigen::Matrix4d& matrix) {
  core::Pose3DTO pose;
  pose.position_m = {matrix(0, 3), matrix(1, 3), matrix(2, 3)};
  Eigen::Quaterniond quaternion(matrix.block<3, 3>(0, 0));
  quaternion.normalize();
  pose.orientation = {quaternion.x(), quaternion.y(), quaternion.z(),
                      quaternion.w()};
  return pose;
}

[[nodiscard]] core::ValidationResultDTO validation_error(
    core::ErrorCode code, std::string message, std::string reason,
    std::map<std::string, std::string> details = {}) {
  details.insert_or_assign("reason", std::move(reason));
  return core::ValidationResultDTO{
      false,
      core::ErrorDTO{std::string(core::to_string(code)), std::move(message),
                     std::move(details)}};
}

[[nodiscard]] const core::RuntimeModelGroupBindingDTO&
exact_angular_group(const core::RuntimeParticipantBinding& binding) {
  const auto& value = binding.value();
  if (!value.control_path.has_value() || value.model_groups.size() != 1U ||
      value.resources.empty()) {
    throw std::invalid_argument(
        "focused native adapter requires one exact motion target");
  }
  const auto& group = value.model_groups.front();
  if (group.compiled_group_name.empty()) {
    throw std::invalid_argument(
        "focused native adapter has an invalid retained model group");
  }
  for (const auto& resource : value.resources) {
    if (resource.coordinate_kind !=
            core::CoordinateKind::kAngular ||
        std::count(group.expanded_compiled_joint_names.begin(),
                   group.expanded_compiled_joint_names.end(),
                   resource.compiled_resource_name) != 1) {
      throw std::invalid_argument(
          "focused native adapter requires an all-angular resource order");
    }
  }
  return group;
}

[[nodiscard]] std::vector<std::string> exact_resource_joint_names(
    const core::RuntimeParticipantBinding& binding) {
  static_cast<void>(exact_angular_group(binding));
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto& resource : binding.value().resources) {
    result.push_back(resource.compiled_resource_name);
  }
  return result;
}

[[nodiscard]] bool exact_state_shape(
    const core::RobotStateDTO& state,
    const std::vector<std::string>& joint_names) noexcept {
  return state.joint_names == joint_names &&
         state.positions_rad.size() == joint_names.size() &&
         (state.velocities_rad_s.empty() ||
          state.velocities_rad_s.size() == joint_names.size()) &&
         std::all_of(state.positions_rad.begin(), state.positions_rad.end(),
                     [](double value) { return std::isfinite(value); }) &&
         std::all_of(state.velocities_rad_s.begin(),
                     state.velocities_rad_s.end(),
                     [](double value) { return std::isfinite(value); });
}

}  // namespace

RetainedRoboPlanKinematicsAdapter::RetainedRoboPlanKinematicsAdapter(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex)
    : binding_(std::move(binding)),
      scene_(std::move(scene)),
      scene_mutex_(std::move(scene_mutex)) {
  if (!scene_ || !scene_mutex_) {
    throw std::invalid_argument(
        "retained kinematics requires a scene and shared mutex");
  }
  const auto& group = exact_angular_group(binding_);
  compiled_group_name_ = group.compiled_group_name;
  compiled_joint_names_ = exact_resource_joint_names(binding_);
  for (const auto& frame : binding_.value().frame_bindings) {
    permitted_frames_.push_back(frame.compiled_frame_name);
  }
  std::sort(permitted_frames_.begin(), permitted_frames_.end());
  permitted_frames_.erase(
      std::unique(permitted_frames_.begin(), permitted_frames_.end()),
      permitted_frames_.end());
  if (permitted_frames_.empty()) {
    throw std::invalid_argument(
        "retained kinematics requires exact frame bindings");
  }

  std::lock_guard<std::mutex> lock(*scene_mutex_);
  const auto scene_group = scene_->getJointGroupInfo(compiled_group_name_);
  if (!scene_group || scene_group->joint_names != compiled_joint_names_) {
    throw std::invalid_argument(
        "RoboPlan group differs from the retained target");
  }
}

core::Result<core::Pose3DTO>
RetainedRoboPlanKinematicsAdapter::forward_kinematics(
    const core::FkRequestDTO& request) const {
  if (!exact_state_shape(request.robot_state, compiled_joint_names_) ||
      (!is_world_frame(request.target_frame) &&
       !std::binary_search(permitted_frames_.begin(), permitted_frames_.end(),
                           request.target_frame)) ||
      (!is_world_frame(request.base_frame) &&
       !std::binary_search(permitted_frames_.begin(), permitted_frames_.end(),
                           request.base_frame))) {
    return core::Result<core::Pose3DTO>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest,
        "Forward kinematics request crosses its retained target",
        "retained_fk_binding_mismatch"));
  }
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    // The state is exactly this target's group in group order; equal counts
    // with the whole model never make it a full-model vector.
    const Eigen::VectorXd q = scene_->toFullJointPositions(
        compiled_group_name_,
        vector_from_std(request.robot_state.positions_rad));
    const Eigen::Matrix4d root_from_target =
        is_world_frame(request.target_frame)
            ? Eigen::Matrix4d::Identity()
            : scene_->forwardKinematics(q, request.target_frame);
    if (is_world_frame(request.base_frame)) {
      return core::Result<core::Pose3DTO>::Ok(
          matrix_to_pose(root_from_target));
    }
    const auto root_from_base =
        scene_->forwardKinematics(q, request.base_frame);
    return core::Result<core::Pose3DTO>::Ok(
        matrix_to_pose(root_from_base.inverse() * root_from_target));
  } catch (const std::exception& error) {
    return core::Result<core::Pose3DTO>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest, error.what(),
        "forward_kinematics_failed"));
  }
}

core::Result<std::map<std::string, core::Pose3DTO>>
RetainedRoboPlanKinematicsAdapter::forward_kinematics_batch(
    const core::RobotStateDTO& robot_state,
    const std::vector<std::string>& target_frames,
    const std::string& base_frame) const {
  if (!exact_state_shape(robot_state, compiled_joint_names_) ||
      (!is_world_frame(base_frame) &&
       !std::binary_search(permitted_frames_.begin(), permitted_frames_.end(),
                           base_frame)) ||
      std::any_of(target_frames.begin(), target_frames.end(),
                  [&](const std::string& frame) {
                    return !is_world_frame(frame) &&
                           !std::binary_search(permitted_frames_.begin(),
                                               permitted_frames_.end(), frame);
                  })) {
    return core::Result<std::map<std::string, core::Pose3DTO>>::Err(
        adapter_error(core::ErrorCode::kInvalidRequest,
                      "Batch kinematics request crosses its retained target",
                      "retained_fk_binding_mismatch"));
  }
  try {
    std::map<std::string, core::Pose3DTO> result;
    if (target_frames.empty()) {
      return core::Result<std::map<std::string, core::Pose3DTO>>::Ok(
          std::move(result));
    }
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    const Eigen::VectorXd q = scene_->toFullJointPositions(
        compiled_group_name_, vector_from_std(robot_state.positions_rad));
    Eigen::Matrix4d base_from_root = Eigen::Matrix4d::Identity();
    if (!is_world_frame(base_frame)) {
      base_from_root = scene_->forwardKinematics(q, base_frame).inverse();
    }
    for (const auto& target_frame : target_frames) {
      const Eigen::Matrix4d root_from_target =
          is_world_frame(target_frame)
              ? Eigen::Matrix4d::Identity()
              : scene_->forwardKinematics(q, target_frame);
      result.emplace(target_frame,
                     matrix_to_pose(base_from_root * root_from_target));
    }
    return core::Result<std::map<std::string, core::Pose3DTO>>::Ok(
        std::move(result));
  } catch (const std::exception &error) {
    return core::Result<std::map<std::string, core::Pose3DTO>>::Err(
        adapter_error(core::ErrorCode::kInvalidRequest, error.what(),
                      "forward_kinematics_batch_failed"));
  }
}

core::Result<core::JointLimitsDTO>
RetainedRoboPlanKinematicsAdapter::joint_limits(
    const std::string &component_id,
    const std::string &command_target_id) const {
  if (component_id != binding_.value().controllable_component_id.value() ||
      command_target_id != binding_.value().command_target_id.value()) {
    return core::Result<core::JointLimitsDTO>::Err(
        adapter_error(core::ErrorCode::kInvalidRequest,
                      "Joint-limit query crosses its retained target",
                      "retained_joint_limits_binding_mismatch"));
  }
  try {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    core::JointLimitsDTO limits;
    limits.joint_names = compiled_joint_names_;
    const auto expected_size = compiled_joint_names_.size();
    if (auto position = scene_->getPositionLimitVectors(compiled_group_name_,
                                                        /*collapsed=*/false)) {
      if (static_cast<std::size_t>(position->first.size()) != expected_size ||
          static_cast<std::size_t>(position->second.size()) != expected_size) {
        throw std::runtime_error("retained position-limit dimension mismatch");
      }
      limits.position_lower_rad.assign(position->first.data(),
                                       position->first.data() +
                                           position->first.size());
      limits.position_upper_rad.assign(position->second.data(),
                                       position->second.data() +
                                           position->second.size());
      for (std::size_t index = 0U; index < expected_size; ++index) {
        if (!std::isfinite(limits.position_lower_rad[index]) ||
            !std::isfinite(limits.position_upper_rad[index]) ||
            limits.position_lower_rad[index] >
                limits.position_upper_rad[index]) {
          throw std::runtime_error("retained position limits are invalid");
        }
      }
    } else {
      throw std::runtime_error("retained position limits are unavailable");
    }
    if (auto velocity = scene_->getVelocityLimitVectors(compiled_group_name_)) {
      if (static_cast<std::size_t>(velocity->second.size()) != expected_size) {
        throw std::runtime_error("retained velocity-limit dimension mismatch");
      }
      limits.velocity_max_rad_s.assign(velocity->second.data(),
                                       velocity->second.data() +
                                           velocity->second.size());
      if (std::any_of(limits.velocity_max_rad_s.begin(),
                      limits.velocity_max_rad_s.end(), [](double value) {
                        return !std::isfinite(value) || value <= 0.0;
                      })) {
        throw std::runtime_error("retained velocity limits are invalid");
      }
    } else {
      throw std::runtime_error("retained velocity limits are unavailable");
    }
    if (auto acceleration =
            scene_->getAccelerationLimitVectors(compiled_group_name_)) {
      std::vector<double> acceleration_values(acceleration->second.data(),
                                              acceleration->second.data() +
                                                  acceleration->second.size());
      if (acceleration_values.size() != expected_size) {
        throw std::runtime_error(
            "retained acceleration-limit dimension mismatch");
      }
      const auto missing = [](double value) {
        return value == std::numeric_limits<double>::max();
      };
      const bool any_missing = std::any_of(acceleration_values.begin(),
                                           acceleration_values.end(), missing);
      const bool all_missing = std::all_of(acceleration_values.begin(),
                                           acceleration_values.end(), missing);
      if (any_missing && !all_missing) {
        throw std::runtime_error(
            "retained acceleration limits are only partially declared");
      }
      if (!all_missing) {
        if (std::any_of(acceleration_values.begin(), acceleration_values.end(),
                        [](double value) {
                          return !std::isfinite(value) || value <= 0.0;
                        })) {
          throw std::runtime_error("retained acceleration limits are invalid");
        }
        limits.acceleration_max_rad_s2 = std::move(acceleration_values);
      }
    }
    return core::Result<core::JointLimitsDTO>::Ok(std::move(limits));
  } catch (const std::exception &error) {
    return core::Result<core::JointLimitsDTO>::Err(
        adapter_error(core::ErrorCode::kInvalidRequest, error.what(),
                      "joint_limits_unavailable"));
  }
}

core::Result<std::vector<std::string>>
RetainedRoboPlanKinematicsAdapter::group_joint_names(
    const std::string &command_target_id) const {
  if (command_target_id != binding_.value().command_target_id.value()) {
    return core::Result<std::vector<std::string>>::Err(adapter_error(
        core::ErrorCode::kInvalidRequest,
        "Joint-name query crosses its retained target",
        "retained_joint_names_binding_mismatch"));
  }
  return core::Result<std::vector<std::string>>::Ok(compiled_joint_names_);
}

std::unique_ptr<RoboPlanTrajectoryValidator>
RoboPlanTrajectoryValidator::create(
    const core::RuntimeParticipantBinding& binding,
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex) {
  const auto& group = exact_angular_group(binding);
  const auto resource_joint_names = exact_resource_joint_names(binding);
  auto result = std::unique_ptr<RoboPlanTrajectoryValidator>(
      new RoboPlanTrajectoryValidator(
          std::move(scene), group.compiled_group_name,
          std::move(scene_mutex)));
  if (result->model_joint_names_ != resource_joint_names) {
    throw std::invalid_argument(
        "RoboPlan validation group differs from the retained target");
  }
  return result;
}

std::unique_ptr<RoboPlanCollisionTrajectoryProjector>
RoboPlanCollisionTrajectoryProjector::create(
    const core::RuntimeParticipantBinding& binding,
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex) {
  const auto& group = exact_angular_group(binding);
  const auto resource_joint_names = exact_resource_joint_names(binding);
  if (!scene || !scene_mutex) {
    throw std::invalid_argument(
        "collision trajectory projection requires a scene and mutex");
  }
  {
    std::lock_guard<std::mutex> lock(*scene_mutex);
    const auto scene_group =
        scene->getJointGroupInfo(group.compiled_group_name);
    if (!scene_group || scene_group->joint_names !=
                            resource_joint_names) {
      throw std::invalid_argument(
          "RoboPlan collision group differs from the retained target");
    }
  }
  return std::unique_ptr<RoboPlanCollisionTrajectoryProjector>(
      new RoboPlanCollisionTrajectoryProjector(
          std::move(scene), group.compiled_group_name,
          std::move(scene_mutex)));
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
  if (trajectory.cartesian_fidelity_metrics &&
      (trajectory.cartesian_waypoint_times_s.empty() ||
       !core::valid_cartesian_fidelity_metrics(
           *trajectory.cartesian_fidelity_metrics))) {
    return validation_error(
        core::ErrorCode::kInvalidRequest,
        "Cartesian candidate fidelity evidence is invalid",
        "cartesian_fidelity_evidence_invalid");
  }
  std::optional<core::MovingReplacementValidationEvidenceDTO> moving_metrics;
  if (options.retained_prefix) {
    const auto &prefix = *options.retained_prefix;
    const double duration = trajectory.waypoints.back().time_from_start_s;
    if (!std::isfinite(prefix.end_time_s) || !(prefix.end_time_s > 0.0) ||
        !std::isfinite(duration) || !(prefix.end_time_s < duration) ||
        trajectory.waypoints.front().time_from_start_s != 0.0 ||
        !std::isfinite(prefix.max_tcp_linear_speed_mps) ||
        !(prefix.max_tcp_linear_speed_mps > 0.0) ||
        !(options.max_tcp_linear_speed_mps > 0.0) || prefix.tcp_frame.empty() ||
        prefix.base_frame.empty() || options.tcp_frame.empty() ||
        options.base_frame.empty()) {
      return validation_error(core::ErrorCode::kInvalidRequest,
                              "Moving replacement requires an interior prefix "
                              "boundary and resolved TCP limits",
                              "trajectory_retained_prefix_invalid");
    }
    moving_metrics.emplace();
    moving_metrics->unchanged_prefix.trajectory_duration_s = prefix.end_time_s;
    moving_metrics->changed_future.trajectory_duration_s =
        duration - prefix.end_time_s;
    for (auto *piece :
         {&moving_metrics->unchanged_prefix, &moving_metrics->changed_future}) {
      piece->peak_abs_joint_velocity_rad_s.assign(trajectory.joint_names.size(),
                                                  0.0);
      piece->peak_abs_joint_acceleration_rad_s2.assign(
          trajectory.joint_names.size(), 0.0);
    }
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
  std::vector<double> peak_joint_velocity(trajectory.joint_names.size(), 0.0);
  std::vector<double> peak_joint_acceleration(trajectory.joint_names.size(), 0.0);
  bool joint_acceleration_measured = false;
  bool prefix_boundary_seen = false;
  const auto record_piece_joint_peak = [&](std::size_t joint, double value,
                                           bool acceleration, double time,
                                           bool knot) {
    if (!moving_metrics)
      return;
    const double boundary = options.retained_prefix->end_time_s;
    const auto record = [&](core::TrajectoryValidationMetricsDTO &piece) {
      auto &peaks = acceleration ? piece.peak_abs_joint_acceleration_rad_s2
                                 : piece.peak_abs_joint_velocity_rad_s;
      peaks[joint] = std::max(peaks[joint], std::abs(value));
    };
    if (time <= boundary)
      record(moving_metrics->unchanged_prefix);
    if (time > boundary || (knot && time == boundary))
      record(moving_metrics->changed_future);
  };
  double previous_time = -1.0;
  for (std::size_t waypoint_index = 0;
       waypoint_index < trajectory.waypoints.size(); ++waypoint_index) {
    if (cancel.is_cancel_requested()) {
      return cancelled();
    }
    const auto& waypoint = trajectory.waypoints[waypoint_index];
    if (options.retained_prefix &&
        waypoint.time_from_start_s == options.retained_prefix->end_time_s)
      prefix_boundary_seen = true;
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
        peak_joint_velocity[joint_index] =
            std::max(peak_joint_velocity[joint_index], std::abs(velocity));
        record_piece_joint_peak(joint_index, velocity, false,
                                waypoint.time_from_start_s, true);
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
        peak_joint_acceleration[joint_index] = std::max(
            peak_joint_acceleration[joint_index], std::abs(acceleration));
        record_piece_joint_peak(joint_index, acceleration, true,
                                waypoint.time_from_start_s, true);
        joint_acceleration_measured = true;
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
        peak_joint_velocity[joint_index] = std::max(
            peak_joint_velocity[joint_index], std::abs(segment_velocity));
        record_piece_joint_peak(joint_index, segment_velocity, false,
                                waypoint.time_from_start_s, false);
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
          peak_joint_acceleration[joint_index] = std::max(
              peak_joint_acceleration[joint_index], std::abs(acceleration));
          record_piece_joint_peak(joint_index, acceleration, true,
                                  waypoint.time_from_start_s, false);
          joint_acceleration_measured = true;
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
  if (options.retained_prefix && !prefix_boundary_seen) {
    return validation_error(
        core::ErrorCode::kInvalidRequest,
        "Moving replacement prefix boundary must be an exact knot",
        "trajectory_retained_prefix_boundary_missing");
  }

  std::optional<double> peak_tcp_linear_speed_mps;
  if (options.max_tcp_linear_speed_mps > 0.0 &&
      !options.tcp_frame.empty() && trajectory.waypoints.size() >= 2) {
    if (trajectory.joint_names.size() != model_joint_names_.size()) {
      return validation_error(
          core::ErrorCode::kInvalidRequest,
          "TCP-speed validation requires every qualified group joint by name",
          "trajectory_joint_group_incomplete",
          {{"group_id", group_name_}});
    }
    const auto speed_started = std::chrono::steady_clock::now();
    try {
      std::lock_guard<std::mutex> lock(*scene_mutex_);
      Eigen::Vector3d previous_position = Eigen::Vector3d::Zero();
      bool have_previous = false;
      double max_speed = 0.0;
      std::size_t max_segment_index = 0;
      double max_segment_duration_s = 0.0;
      double prefix_max_speed = 0.0;
      std::size_t prefix_max_segment_index = 0;
      double prefix_max_segment_duration_s = 0.0;
      double future_max_speed = 0.0;
      std::size_t future_max_segment_index = 0;
      double future_max_segment_duration_s = 0.0;
      const auto position_in_frames = [&](const Eigen::VectorXd &q,
                                          const std::string &tcp_frame,
                                          const std::string &base_frame) {
        const Eigen::Matrix4d robot_base_from_tcp =
            scene_->forwardKinematics(q, tcp_frame);
        Eigen::Matrix4d requested_base_from_robot_base =
            Eigen::Matrix4d::Identity();
        if (!is_world_frame(base_frame)) {
          requested_base_from_robot_base =
              scene_->forwardKinematics(q, base_frame).inverse();
        }
        const Eigen::Matrix4d tcp_transform =
            requested_base_from_robot_base * robot_base_from_tcp;
        return Eigen::Vector3d{tcp_transform(0, 3), tcp_transform(1, 3),
                               tcp_transform(2, 3)};
      };
      for (std::size_t waypoint_index = 0;
           waypoint_index < trajectory.waypoints.size(); ++waypoint_index) {
        if (cancel.is_cancel_requested()) {
          return cancelled();
        }
        const auto& waypoint = trajectory.waypoints[waypoint_index];
        Eigen::VectorXd group_q(
            static_cast<Eigen::Index>(model_joint_names_.size()));
        for (std::size_t joint_index = 0;
             joint_index < trajectory.joint_names.size(); ++joint_index) {
          group_q[static_cast<Eigen::Index>(model_index_for(joint_index))] =
              waypoint.positions_rad[joint_index];
        }
        const Eigen::VectorXd q =
            scene_->toFullJointPositions(group_name_, group_q);
        const bool prefix_point =
            options.retained_prefix &&
            waypoint.time_from_start_s <= options.retained_prefix->end_time_s;
        const auto &tcp_frame = prefix_point
                                    ? options.retained_prefix->tcp_frame
                                    : options.tcp_frame;
        const auto &base_frame = prefix_point
                                     ? options.retained_prefix->base_frame
                                     : options.base_frame;
        const Eigen::Vector3d tcp_position =
            position_in_frames(q, tcp_frame, base_frame);
        if (moving_metrics && !tcp_position.allFinite()) {
          return validation_error(
              core::ErrorCode::kInvalidRequest,
              "Moving replacement TCP position is non-finite",
              "tcp_speed_validation_nonfinite");
        }
        if (have_previous) {
          const double duration =
              waypoint.time_from_start_s -
              trajectory.waypoints[waypoint_index - 1].time_from_start_s;
          const double speed =
              (tcp_position - previous_position).norm() / duration;
          if (moving_metrics && !std::isfinite(speed)) {
            return validation_error(
                core::ErrorCode::kInvalidRequest,
                "Moving replacement TCP speed is non-finite",
                "tcp_speed_validation_nonfinite");
          }
          if (speed > max_speed) {
            max_speed = speed;
            max_segment_index = waypoint_index - 1;
            max_segment_duration_s = duration;
          }
          if (moving_metrics) {
            auto &piece_speed =
                prefix_point ? prefix_max_speed : future_max_speed;
            auto &piece_index = prefix_point ? prefix_max_segment_index
                                             : future_max_segment_index;
            auto &piece_duration = prefix_point ? prefix_max_segment_duration_s
                                                : future_max_segment_duration_s;
            if (speed > piece_speed) {
              piece_speed = speed;
              piece_index = waypoint_index - 1;
              piece_duration = duration;
            }
          }
        }
        previous_position = tcp_position;
        if (options.retained_prefix &&
            waypoint.time_from_start_s == options.retained_prefix->end_time_s &&
            (options.retained_prefix->tcp_frame != options.tcp_frame ||
             options.retained_prefix->base_frame != options.base_frame)) {
          // Restart only TCP position history in the successor frames at the
          // exact shared knot. Joint secant/triple history stays intact.
          previous_position =
              position_in_frames(q, options.tcp_frame, options.base_frame);
          if (!previous_position.allFinite()) {
            return validation_error(
                core::ErrorCode::kInvalidRequest,
                "Moving replacement seam TCP position is non-finite",
                "tcp_speed_validation_nonfinite");
          }
        }
        have_previous = true;
      }
      peak_tcp_linear_speed_mps = max_speed;
      double speed_limit = options.max_tcp_linear_speed_mps;
      std::string measured_tcp_frame = options.tcp_frame;
      std::string measured_base_frame = options.base_frame;
      if (moving_metrics) {
        moving_metrics->unchanged_prefix.peak_tcp_linear_speed_mps =
            prefix_max_speed;
        moving_metrics->changed_future.peak_tcp_linear_speed_mps =
            future_max_speed;
        if (prefix_max_speed >
            options.retained_prefix->max_tcp_linear_speed_mps +
                kLimitTolerance) {
          max_speed = prefix_max_speed;
          max_segment_index = prefix_max_segment_index;
          max_segment_duration_s = prefix_max_segment_duration_s;
          speed_limit = options.retained_prefix->max_tcp_linear_speed_mps;
          measured_tcp_frame = options.retained_prefix->tcp_frame;
          measured_base_frame = options.retained_prefix->base_frame;
        } else {
          max_speed = future_max_speed;
          max_segment_index = future_max_segment_index;
          max_segment_duration_s = future_max_segment_duration_s;
        }
      }
      if (max_speed > speed_limit + kLimitTolerance) {
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
             {"max_segment_dt_s", std::to_string(max_segment_duration_s)},
             {"tcp_frame", measured_tcp_frame},
             {"base_frame", measured_base_frame},
             {"measured", std::to_string(max_speed)},
             {"limit", std::to_string(speed_limit)},
             {"units", "m/s"},
             {"waypoint_count", std::to_string(trajectory.waypoints.size())},
             {"trajectory_duration_s",
              std::to_string(trajectory.waypoints.back().time_from_start_s)},
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

  core::TrajectoryValidationMetricsDTO metrics{
      trajectory.waypoints.back().time_from_start_s,
      std::move(peak_joint_velocity),
      joint_acceleration_measured ? std::move(peak_joint_acceleration)
                                  : std::vector<double>{},
      peak_tcp_linear_speed_mps,
      trajectory.cartesian_fidelity_metrics};
  core::ValidationResultDTO result{true, std::nullopt, std::move(metrics)};
  if (moving_metrics) {
    if (!joint_acceleration_measured) {
      moving_metrics->unchanged_prefix.peak_abs_joint_acceleration_rad_s2
          .clear();
      moving_metrics->changed_future.peak_abs_joint_acceleration_rad_s2.clear();
    }
    result.moving_replacement = std::move(moving_metrics);
  }
  return result;
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
      if (q_index < 0 || q_index >= current_q.size()) {
        return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
            core::kErrorValidationCollision,
            "Collision projection joint index is out of range",
            {{"reason", "active_group_q_index_out_of_range"},
             {"group", group_name_},
             {"joint", group->joint_names[index]}}});
      }
      q_index_by_joint.emplace(group->joint_names[index], q_index);
    }

    // Coordinates are mapped by name. A state or trajectory whose length
    // equals the full model is still never taken as full-model positional.
    Eigen::VectorXd baseline = current_q;
    const auto& observed_names = observed_start_state.joint_names;
    const auto& observed_positions = observed_start_state.positions_rad;
    bool observed_valid = observed_names.size() == observed_positions.size();
    if (observed_valid && observed_names == full_joint_names) {
      baseline = vector_from_std(observed_positions);
    } else if (observed_valid &&
               observed_names.size() == q_index_by_joint.size()) {
      std::set<std::string> seen;
      for (std::size_t index = 0; observed_valid && index < observed_names.size();
           ++index) {
        const auto found = q_index_by_joint.find(observed_names[index]);
        observed_valid = found != q_index_by_joint.end() &&
                         seen.insert(observed_names[index]).second;
        if (observed_valid) baseline[found->second] = observed_positions[index];
      }
    } else {
      observed_valid = false;
    }
    if (!observed_valid || !baseline.allFinite()) {
      return core::Result<core::TrajectoryDTO>::Err(core::ErrorDTO{
          core::kErrorValidationCollision,
          "Observed start state cannot seed full collision coordinates",
          {{"reason", "collision_projection_observed_state_invalid"},
           {"expected_position_count", std::to_string(full_q_size)},
           {"observed_position_count",
            std::to_string(observed_positions.size())}}});
    }

    if (trajectory.joint_names == full_joint_names) {
      return core::Result<core::TrajectoryDTO>::Ok(trajectory);
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

std::shared_ptr<const RetainedKinematicModelGeneration>
make_retained_motion_model_generation(
    const core::RuntimeParticipantBinding& motion_binding,
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex,
    std::shared_ptr<RetainedKinematicStateProjector> state_projector) {
  if (!scene || !scene_mutex || !state_projector) {
    throw std::invalid_argument(
        "retained model generation requires one scene, mutex and projector");
  }
  auto generation = std::make_shared<RetainedKinematicModelGeneration>();
  generation->kinematics = std::make_shared<RetainedRoboPlanKinematicsAdapter>(
      motion_binding, scene, scene_mutex);
  generation->trajectory_validator =
      RoboPlanTrajectoryValidator::create(motion_binding, scene, scene_mutex);
  generation->collision_trajectory_projector =
      RoboPlanCollisionTrajectoryProjector::create(motion_binding, scene,
                                                   scene_mutex);
  generation->scene = std::move(scene);
  generation->scene_mutex = std::move(scene_mutex);
  generation->state_projector = std::move(state_projector);
  return generation;
}

RetainedKinematicModelProxy::RetainedKinematicModelProxy(
    std::shared_ptr<const RetainedKinematicModelGeneration> generation)
    : generation_(std::move(generation)) {
  if (!generation_ || !generation_->kinematics ||
      !generation_->trajectory_validator ||
      !generation_->collision_trajectory_projector) {
    throw std::invalid_argument(
        "retained kinematic model proxy requires one complete generation");
  }
}

void RetainedKinematicModelProxy::ReplaceGeneration(
    std::shared_ptr<const RetainedKinematicModelGeneration> candidate) {
  if (!candidate || !candidate->kinematics ||
      !candidate->trajectory_validator ||
      !candidate->collision_trajectory_projector) {
    throw std::invalid_argument(
        "retained kinematic model replacement is incomplete");
  }
  std::unique_lock lock(mutex_);
  generation_ = std::move(candidate);
}

std::shared_ptr<const RetainedKinematicModelGeneration>
RetainedKinematicModelProxy::current_generation() const {
  std::shared_lock lock(mutex_);
  return generation_;
}

core::Result<core::Pose3DTO> RetainedKinematicModelProxy::forward_kinematics(
    const core::FkRequestDTO& request) const {
  std::shared_lock lock(mutex_);
  return generation_->kinematics->forward_kinematics(request);
}

core::Result<std::map<std::string, core::Pose3DTO>>
RetainedKinematicModelProxy::forward_kinematics_batch(
    const core::RobotStateDTO& robot_state,
    const std::vector<std::string>& target_frames,
    const std::string& base_frame) const {
  std::shared_lock lock(mutex_);
  return generation_->kinematics->forward_kinematics_batch(
      robot_state, target_frames, base_frame);
}

core::Result<core::JointLimitsDTO> RetainedKinematicModelProxy::joint_limits(
    const std::string& component_id,
    const std::string& command_target_id) const {
  std::shared_lock lock(mutex_);
  return generation_->kinematics->joint_limits(component_id,
                                               command_target_id);
}

core::Result<std::vector<std::string>>
RetainedKinematicModelProxy::group_joint_names(
    const std::string& command_target_id) const {
  std::shared_lock lock(mutex_);
  return generation_->kinematics->group_joint_names(command_target_id);
}

core::ValidationResultDTO RetainedKinematicModelProxy::validate_non_collision(
    const core::TrajectoryDTO& trajectory,
    const core::ValidationOptionsDTO& options,
    const core::CollisionCancellationToken& cancel) const {
  std::shared_lock lock(mutex_);
  return generation_->trajectory_validator->validate_non_collision(
      trajectory, options, cancel);
}

core::Result<core::TrajectoryDTO>
RetainedKinematicModelProxy::expand_for_collision(
    const core::TrajectoryDTO& trajectory,
    const core::RobotStateDTO& observed_start_state) const {
  std::shared_lock lock(mutex_);
  return generation_->collision_trajectory_projector->expand_for_collision(
      trajectory, observed_start_state);
}

}  // namespace manyforge::roboplan_backend
