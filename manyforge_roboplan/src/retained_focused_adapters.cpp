#include <manyforge_roboplan/retained_focused_adapters.hpp>

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
    Eigen::VectorXd q = vector_from_std(request.robot_state.positions_rad);
    if (q.size() != scene_->getCurrentJointPositions().size()) {
      q = scene_->toFullJointPositions(compiled_group_name_, q);
    }
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
    Eigen::VectorXd q = vector_from_std(robot_state.positions_rad);
    if (q.size() != scene_->getCurrentJointPositions().size()) {
      q = scene_->toFullJointPositions(compiled_group_name_, q);
    }
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

}  // namespace manyforge::roboplan_backend
