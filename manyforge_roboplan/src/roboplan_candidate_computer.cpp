#include <manyforge_roboplan/roboplan_candidate_computer.hpp>

#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <manyforge_roboplan/joint_cap_contract.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <roboplan/core/path_utils.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_cartesian_planning/cartesian_path_planner.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>
#include <roboplan_toppra/toppra.hpp>

namespace manyforge::roboplan_backend {
namespace {

using core::ErrorCode;
using core::ErrorDTO;

[[nodiscard]] ErrorDTO make_error(ErrorCode code, std::string message,
                                  std::string reason,
                                  std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  return ErrorDTO{std::string(core::to_string(code)), std::move(message),
                  std::move(details)};
}

template <typename Value>
[[nodiscard]] core::Result<Value> fail(
    ErrorCode code, std::string message, std::string reason,
    std::map<std::string, std::string> details = {}) {
  return core::Result<Value>::Err(make_error(
      code, std::move(message), std::move(reason), std::move(details)));
}

[[nodiscard]] bool finite(double value) noexcept {
  return std::isfinite(value);
}

[[nodiscard]] bool finite_positive(double value) noexcept {
  return finite(value) && value > 0.0;
}

[[nodiscard]] bool finite_nonnegative(double value) noexcept {
  return finite(value) && value >= 0.0;
}

template <typename Value>
[[nodiscard]] bool unique_nonempty(const std::vector<Value>& values) {
  return !values.empty() &&
         std::set<Value>(values.begin(), values.end()).size() == values.size();
}

[[nodiscard]] bool unique_nonempty_strings(
    const std::vector<std::string>& values) {
  return !values.empty() &&
         std::all_of(values.begin(), values.end(),
                     [](const std::string& value) { return !value.empty(); }) &&
         std::set<std::string>(values.begin(), values.end()).size() ==
             values.size();
}

[[nodiscard]] Eigen::VectorXd eigen_vector(
    const std::vector<double>& values) {
  return Eigen::Map<const Eigen::VectorXd>(
      values.data(), static_cast<Eigen::Index>(values.size()));
}

[[nodiscard]] std::vector<double> standard_vector(
    const Eigen::VectorXd& values) {
  return {values.data(), values.data() + values.size()};
}

[[nodiscard]] Eigen::Matrix4d native_pose(const core::Pose3DTO& pose) {
  Eigen::Quaterniond orientation(
      pose.orientation.w, pose.orientation.x, pose.orientation.y,
      pose.orientation.z);
  const double scale = orientation.coeffs().cwiseAbs().maxCoeff();
  orientation.coeffs() /= scale;
  orientation.normalize();
  Eigen::Matrix4d transform = Eigen::Matrix4d::Identity();
  transform.block<3, 3>(0, 0) =
      orientation.normalized().toRotationMatrix();
  transform(0, 3) = pose.position_m.x_m;
  transform(1, 3) = pose.position_m.y_m;
  transform(2, 3) = pose.position_m.z_m;
  return transform;
}

[[nodiscard]] bool valid_pose(const core::Pose3DTO& pose) noexcept {
  const auto& q = pose.orientation;
  const double scale =
      std::max({std::abs(q.x), std::abs(q.y), std::abs(q.z), std::abs(q.w)});
  const double scaled_norm =
      scale > 0.0 && finite(scale)
          ? std::hypot(std::hypot(q.x / scale, q.y / scale),
                       std::hypot(q.z / scale, q.w / scale))
          : 0.0;
  return finite(pose.position_m.x_m) && finite(pose.position_m.y_m) &&
         finite(pose.position_m.z_m) && finite(q.x) && finite(q.y) &&
         finite(q.z) && finite(q.w) && finite_positive(scale) &&
         finite_positive(scaled_norm);
}

[[nodiscard]] std::optional<ErrorDTO> control_error(
    const RoboPlanCandidateControl& control, std::string stage) {
  if (control.cancellation_requested && control.cancellation_requested()) {
    return make_error(ErrorCode::kPlanningFailed,
                      "RoboPlan candidate computation was cancelled",
                      "planning_cancelled_by_interrupt",
                      {{"failure_stage", std::move(stage)}});
  }
  if (control.deadline_expired && control.deadline_expired()) {
    return make_error(ErrorCode::kPlanningTimeout,
                      "RoboPlan candidate computation exceeded its deadline",
                      "planning_timeout",
                      {{"failure_stage", std::move(stage)}});
  }
  return std::nullopt;
}

[[nodiscard]] std::uint32_t effective_seed(
    std::uint32_t base_seed, std::uint32_t attempt_index) noexcept {
  if (attempt_index == 0U) {
    return base_seed;
  }
  std::uint64_t value =
      (static_cast<std::uint64_t>(base_seed) << 32U) | attempt_index;
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  value ^= value >> 31U;
  return static_cast<std::uint32_t>(value ^ (value >> 32U));
}

void anchor_trajectory_endpoints(
    roboplan::JointTrajectory& trajectory,
    const Eigen::VectorXd& start_positions,
    const Eigen::VectorXd& goal_positions) {
  if (trajectory.positions.empty()) {
    return;
  }
  if (trajectory.positions.front().size() == start_positions.size()) {
    trajectory.positions.front() = start_positions;
  }
  if (!trajectory.times.empty()) {
    trajectory.times.front() = 0.0;
  }
  if (!trajectory.velocities.empty()) {
    trajectory.velocities.front().setZero();
  }
  if (!trajectory.accelerations.empty()) {
    trajectory.accelerations.front().setZero();
  }
  if (trajectory.positions.size() > 1U) {
    if (trajectory.positions.back().size() == goal_positions.size()) {
      trajectory.positions.back() = goal_positions;
    }
    if (trajectory.velocities.size() == trajectory.positions.size()) {
      trajectory.velocities.back().setZero();
    }
    if (trajectory.accelerations.size() == trajectory.positions.size()) {
      trajectory.accelerations.back().setZero();
    }
  }
}

[[nodiscard]] double maximum_tcp_speed(
    const std::shared_ptr<roboplan::Scene>& scene,
    std::string_view group_name, std::string_view base_frame,
    std::string_view tcp_frame,
    const roboplan::JointTrajectory& trajectory) {
  double maximum = 0.0;
  Eigen::Vector3d previous = Eigen::Vector3d::Zero();
  bool have_previous = false;
  for (std::size_t index = 0U; index < trajectory.times.size(); ++index) {
    const Eigen::VectorXd full = scene->toFullJointPositions(
        std::string(group_name), trajectory.positions[index]);
    const Eigen::Matrix4d pose = scene->forwardKinematics(
        full, std::string(tcp_frame), std::string(base_frame));
    const Eigen::Vector3d current = pose.block<3, 1>(0, 3);
    if (have_previous) {
      const double duration =
          trajectory.times[index] - trajectory.times[index - 1U];
      if (duration > 0.0) {
        maximum = std::max(maximum, (current - previous).norm() / duration);
      }
    }
    previous = current;
    have_previous = true;
  }
  return maximum;
}

[[nodiscard]] core::Result<core::TrajectoryDTO> portable_trajectory(
    const roboplan::JointTrajectory& native,
    const RoboPlanCandidateBindingDTO& binding,
    std::size_t maximum_samples) {
  const std::size_t sample_count = native.positions.size();
  if (native.joint_names != binding.compiled_joint_names ||
      native.times.size() != sample_count || sample_count < 2U ||
      sample_count > maximum_samples ||
      (!native.velocities.empty() &&
       native.velocities.size() != sample_count) ||
      (!native.accelerations.empty() &&
       native.accelerations.size() != sample_count)) {
    return fail<core::TrajectoryDTO>(
        ErrorCode::kPlanningFailed,
        "RoboPlan returned a trajectory with an invalid bounded shape",
        "native_trajectory_shape_invalid",
        {{"sample_count", std::to_string(sample_count)}});
  }

  core::TrajectoryDTO result;
  result.resources = binding.resources;
  result.joint_names = binding.compiled_joint_names;
  result.waypoints.reserve(sample_count);
  double previous_time = -1.0;
  for (std::size_t index = 0U; index < sample_count; ++index) {
    const bool dimensions_valid =
        native.positions[index].size() ==
            static_cast<Eigen::Index>(binding.resources.size()) &&
        (native.velocities.empty() ||
         native.velocities[index].size() ==
             static_cast<Eigen::Index>(binding.resources.size())) &&
        (native.accelerations.empty() ||
         native.accelerations[index].size() ==
             static_cast<Eigen::Index>(binding.resources.size()));
    const bool finite_values = native.positions[index].allFinite() &&
                               (native.velocities.empty() ||
                                native.velocities[index].allFinite()) &&
                               (native.accelerations.empty() ||
                                native.accelerations[index].allFinite());
    const double time = native.times[index];
    if (!dimensions_valid || !finite_values || !finite(time) || time < 0.0 ||
        (index > 0U && time <= previous_time)) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed,
          "RoboPlan returned an invalid trajectory sample",
          "native_trajectory_sample_invalid",
          {{"sample_index", std::to_string(index)}});
    }
    core::TrajectoryWaypointDTO waypoint;
    waypoint.time_from_start_s = time;
    waypoint.positions_rad = standard_vector(native.positions[index]);
    if (!native.velocities.empty()) {
      waypoint.velocities_rad_s = standard_vector(native.velocities[index]);
    }
    if (!native.accelerations.empty()) {
      waypoint.accelerations_rad_s2 =
          standard_vector(native.accelerations[index]);
    }
    result.waypoints.push_back(std::move(waypoint));
    previous_time = time;
  }
  return core::Result<core::TrajectoryDTO>::Ok(std::move(result));
}

[[nodiscard]] bool frame_exists(const roboplan::Scene& scene,
                                const core::ResolvedFrameDTO& frame) {
  return scene.getFrameId(core::compiled_frame_name(frame)).has_value();
}

[[nodiscard]] bool frame_identity_valid(
    const core::ResolvedFrameDTO& frame) {
  if (core::compiled_frame_name(frame).empty()) {
    return false;
  }
  if (const auto* cell = std::get_if<core::ResolvedCellFrameDTO>(&frame)) {
    return !cell->cell_id.empty();
  }
  if (const auto* component =
          std::get_if<core::ResolvedComponentModelFrameDTO>(&frame)) {
    return !component->controllable_component_id.empty() &&
           !component->model_instance_id.empty() &&
           !component->frame_role_id.empty() &&
           !component->model_revision.empty();
  }
  const auto& passive =
      std::get<core::ResolvedPassiveModelFrameDTO>(frame);
  return !passive.model_instance_id.empty() &&
         !passive.frame_role_id.empty() && !passive.model_revision.empty();
}

[[nodiscard]] bool contains_frame(
    const std::vector<core::ResolvedFrameDTO>& frames,
    const core::ResolvedFrameDTO& frame) {
  return std::count(frames.begin(), frames.end(), frame) == 1;
}

[[nodiscard]] bool unique_frames(
    const std::vector<core::ResolvedFrameDTO>& frames) {
  if (frames.empty() || frames.size() > 4096U) {
    return false;
  }
  for (std::size_t index = 0U; index < frames.size(); ++index) {
    if (!frame_identity_valid(frames[index]) ||
        std::count(frames.begin(), frames.end(), frames[index]) != 1) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] core::Result<void> validate_binding(
    const roboplan::Scene& scene,
    const RoboPlanCandidateBindingDTO& binding,
    const RoboPlanCandidateComputerConfig& config) {
  if (binding.controllable_component_id.empty() ||
      binding.command_target_id.empty() || binding.model_instance_id.empty() ||
      binding.model_revision.empty() || binding.compiled_group_name.empty() ||
      !unique_nonempty(binding.resources) ||
      !unique_nonempty_strings(binding.compiled_joint_names) ||
      binding.resources.size() != binding.compiled_joint_names.size() ||
      !unique_frames(binding.permitted_frames) ||
      !std::all_of(binding.permitted_frames.begin(),
                   binding.permitted_frames.end(),
                   [&](const auto& frame) { return frame_exists(scene, frame); }) ||
      config.maximum_trajectory_samples < 2U ||
      config.maximum_trajectory_samples > 1'000'000U ||
      !finite_positive(config.trajectory_sample_period_s)) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kInvalidRequest,
        "RoboPlan candidate binding is incomplete or inconsistent",
        "native_candidate_binding_invalid"));
  }
  const auto group = scene.getJointGroupInfo(binding.compiled_group_name);
  if (!group.has_value() ||
      group->joint_names != binding.compiled_joint_names ||
      group->nq_collapsed != binding.compiled_joint_names.size()) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kInvalidRequest,
        "RoboPlan group differs from the retained resource projection",
        "native_candidate_group_mismatch"));
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] bool valid_planning_profile(
    core::PlanningProfileDTO profile) noexcept {
  switch (profile) {
    case core::PlanningProfileDTO::kFirstFeasible:
    case core::PlanningProfileDTO::kBalanced:
    case core::PlanningProfileDTO::kQualityBounded:
      return true;
  }
  return false;
}

[[nodiscard]] core::Result<void> validate_request_binding(
    const RoboPlanCandidateBindingDTO& binding,
    const RoboPlanCandidateRequestDTO& request) {
  if (request.controllable_component_id !=
          binding.controllable_component_id ||
      request.command_target_id != binding.command_target_id ||
      request.model_instance_id != binding.model_instance_id ||
      request.model_revision != binding.model_revision ||
      request.resources != binding.resources ||
      !contains_frame(binding.permitted_frames, request.base_frame) ||
      !contains_frame(binding.permitted_frames, request.tcp_frame)) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kInvalidRequest,
        "RoboPlan request does not match the exact retained participant binding",
        "native_candidate_request_binding_mismatch"));
  }
  const auto state_validation =
      core::validate_controlled_state(request.start_state);
  if (!state_validation.ok()) {
    return core::Result<void>::Err(state_validation.error());
  }
  if (request.start_state.controllable_component_id !=
          binding.controllable_component_id ||
      request.start_state.command_target_id != binding.command_target_id ||
      request.start_state.resources != binding.resources ||
      request.start_state.joint_names != binding.compiled_joint_names) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kStartStateMismatch,
        "RoboPlan start state does not match the exact retained participant",
        "native_candidate_start_state_binding_mismatch"));
  }
  const auto& speed = request.speed_profile;
  const auto& tolerances = request.tolerances;
  if (!finite_positive(request.planning_timeout_s) ||
      !finite_positive(speed.velocity_scale) || speed.velocity_scale > 1.0 ||
      !finite_positive(speed.acceleration_scale) ||
      speed.acceleration_scale > 1.0 ||
      (speed.max_tcp_linear_speed_mps.has_value() &&
       !finite_positive(*speed.max_tcp_linear_speed_mps)) ||
      !finite_nonnegative(tolerances.joint_position_tol_rad) ||
      !finite_nonnegative(tolerances.translation_tol_m) ||
      !finite_nonnegative(tolerances.rotation_tol_rad) ||
      !valid_planning_profile(request.planning_profile)) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kInvalidRequest,
        "RoboPlan request has invalid tolerances, limits, timeout, or profile",
        "native_candidate_request_numeric_invalid"));
  }
  return core::Result<void>::Ok();
}

struct ResolvedDynamicsCaps final {
  std::map<std::string, double> velocity_by_joint;
  std::map<std::string, double> acceleration_by_joint;
  core::PerJointDynamicsLimitsDTO portable_by_joint;
};

[[nodiscard]] core::Result<ResolvedDynamicsCaps> resolve_dynamics_caps(
    const RoboPlanCandidateBindingDTO& binding,
    const core::PerResourceDynamicsLimitsDTO& requested) {
  ResolvedDynamicsCaps result;
  std::optional<std::size_t> previous_index;
  for (const auto& entry : requested) {
    const auto resource = std::find(binding.resources.begin(),
                                    binding.resources.end(),
                                    entry.resource_id);
    if (resource == binding.resources.end()) {
      return fail<ResolvedDynamicsCaps>(
          ErrorCode::kUnsupportedCapability,
          "Dynamics cap names a resource outside the planning target",
          "unsupported_joint_dynamics_limit",
          {{"resource_id", entry.resource_id.value()}});
    }
    const auto index = static_cast<std::size_t>(
        std::distance(binding.resources.begin(), resource));
    if (entry.compiled_joint_name != binding.compiled_joint_names[index] ||
        (previous_index.has_value() && index <= *previous_index)) {
      return fail<ResolvedDynamicsCaps>(
          ErrorCode::kInvalidRequest,
          "Dynamics caps do not preserve the resolved resource/joint order",
          "invalid_joint_dynamics_limit",
          {{"resource_id", entry.resource_id.value()},
           {"compiled_joint_name", entry.compiled_joint_name}});
    }
    previous_index = index;
    const auto& limit = entry.limit;
    const auto malformed = [](const std::optional<double>& value) {
      return value.has_value() && !finite_positive(*value);
    };
    if (malformed(limit.max_velocity_rad_s) ||
        malformed(limit.max_acceleration_rad_s2) ||
        malformed(limit.max_jerk_rad_s3)) {
      return fail<ResolvedDynamicsCaps>(
          ErrorCode::kInvalidRequest,
          "Dynamics caps must be finite and strictly positive",
          "invalid_joint_dynamics_limit",
          {{"resource_id", entry.resource_id.value()},
           {"compiled_joint_name", entry.compiled_joint_name}});
    }
    if (limit.max_jerk_rad_s3.has_value()) {
      return fail<ResolvedDynamicsCaps>(
          ErrorCode::kUnsupportedCapability,
          "RoboPlan TOPP-RA cannot enforce a per-resource jerk cap",
          "unsupported_joint_dynamics_limit",
          {{"resource_id", entry.resource_id.value()},
           {"compiled_joint_name", entry.compiled_joint_name},
           {"dimension", "jerk"},
           {"backend", "roboplan_toppra"}});
    }
    if (limit.max_velocity_rad_s.has_value()) {
      result.velocity_by_joint.emplace(
          entry.compiled_joint_name, *limit.max_velocity_rad_s);
    }
    if (limit.max_acceleration_rad_s2.has_value()) {
      result.acceleration_by_joint.emplace(
          entry.compiled_joint_name, *limit.max_acceleration_rad_s2);
    }
    result.portable_by_joint.emplace(entry.compiled_joint_name, limit);
  }
  return core::Result<ResolvedDynamicsCaps>::Ok(std::move(result));
}

[[nodiscard]] double rotation_distance(const Eigen::Matrix3d& lhs,
                                       const Eigen::Matrix3d& rhs) {
  Eigen::Quaterniond relative(lhs.transpose() * rhs);
  relative.normalize();
  return 2.0 *
         std::acos(std::clamp(std::abs(relative.w()), 0.0, 1.0));
}

void normalize_ik_goal_for_limits(
    const std::shared_ptr<roboplan::Scene>& scene,
    std::string_view group_name, const Eigen::VectorXd& start,
    Eigen::VectorXd& goal) {
  const auto limits =
      scene->getPositionLimitVectors(std::string(group_name), false);
  if (!limits.has_value() || limits->first.size() != goal.size() ||
      limits->second.size() != goal.size() || start.size() != goal.size()) {
    return;
  }
  constexpr long double kTwoPi =
      2.0L * 3.141592653589793238462643383279502884L;
  constexpr double kLimitTolerance = 1.0e-6;
  for (Eigen::Index index = 0; index < goal.size(); ++index) {
    const double lower = limits->first[index];
    const double upper = limits->second[index];
    const double value = goal[index];
    if (!finite(lower) || !finite(upper) || !finite(value) ||
        !finite(start[index]) || lower >= upper ||
        upper - lower < kTwoPi - kLimitTolerance) {
      continue;
    }
    const long double first = std::ceil(
        (static_cast<long double>(lower) - value) / kTwoPi);
    const long double last = std::floor(
        (static_cast<long double>(upper) - value) / kTwoPi);
    if (!std::isfinite(first) || !std::isfinite(last) || first > last) {
      continue;
    }
    const long double ideal = std::round(
        (static_cast<long double>(start[index]) - value) / kTwoPi);
    const long double turn = std::clamp(ideal, first, last);
    const long double candidate =
        static_cast<long double>(value) + turn * kTwoPi;
    if (!std::isfinite(candidate) ||
        candidate < static_cast<long double>(lower) - kLimitTolerance ||
        candidate > static_cast<long double>(upper) + kLimitTolerance ||
        candidate < std::numeric_limits<double>::lowest() ||
        candidate > std::numeric_limits<double>::max()) {
      continue;
    }
    goal[index] = static_cast<double>(candidate);
  }
}

struct DensifiedCartesianPath final {
  std::vector<Eigen::Matrix4d> poses;
  std::vector<std::size_t> source_waypoint_dense_indices;
};

[[nodiscard]] core::Result<DensifiedCartesianPath> densify_cartesian_path(
    const core::CartesianPathDTO& path,
    const RoboPlanCandidateControl& control) {
  constexpr std::size_t kMaximumInputWaypoints = 4096U;
  constexpr std::size_t kMaximumDensifiedWaypoints = 100'000U;
  constexpr std::size_t kControlStride = 64U;
  if (path.waypoints.size() < 2U ||
      path.waypoints.size() > kMaximumInputWaypoints ||
      !finite_positive(path.max_translation_step_m) ||
      !finite_positive(path.max_rotation_step_rad) ||
      path.speed_mode != core::CartesianSpeedModeDTO::kBounded ||
      path.fallback_policy != core::CartesianFallbackPolicyDTO::kFailClosed ||
      !finite_positive(path.max_linear_speed_mps) ||
      !finite_positive(path.max_angular_speed_rad_s) ||
      !finite_positive(path.max_linear_acceleration_mps2) ||
      !finite_positive(path.max_angular_acceleration_rad_s2)) {
    return fail<DensifiedCartesianPath>(
        ErrorCode::kInvalidRequest,
        "Cartesian request shape, policy, steps, or limits are invalid",
        "cartesian_request_invalid");
  }

  std::vector<Eigen::Matrix4d> source;
  source.reserve(path.waypoints.size());
  for (std::size_t index = 0U; index < path.waypoints.size(); ++index) {
    if (index % kControlStride == 0U) {
      if (auto error = control_error(control, "cartesian_path_densification");
          error.has_value()) {
        return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
      }
    }
    if (!valid_pose(path.waypoints[index])) {
      return fail<DensifiedCartesianPath>(
          ErrorCode::kInvalidRequest,
          "Cartesian waypoint contains a non-finite or zero-norm pose",
          "cartesian_waypoint_not_finite",
          {{"waypoint_index", std::to_string(index)}});
    }
    source.push_back(native_pose(path.waypoints[index]));
  }

  DensifiedCartesianPath result;
  result.poses.reserve(source.size());
  result.source_waypoint_dense_indices.reserve(source.size());
  result.poses.push_back(source.front());
  result.source_waypoint_dense_indices.push_back(0U);
  for (std::size_t segment = 1U; segment < source.size(); ++segment) {
    if (auto error = control_error(control, "cartesian_path_densification");
        error.has_value()) {
      return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
    }
    const auto& from = source[segment - 1U];
    const auto& to = source[segment];
    const double translation =
        (to.block<3, 1>(0, 3) - from.block<3, 1>(0, 3)).norm();
    const double rotation = rotation_distance(
        from.block<3, 3>(0, 0), to.block<3, 3>(0, 0));
    const double required = std::max(
        1.0, std::ceil(std::max(translation / path.max_translation_step_m,
                                rotation / path.max_rotation_step_rad)));
    const std::size_t remaining =
        kMaximumDensifiedWaypoints - result.poses.size();
    if (!finite(required) || required > static_cast<double>(remaining)) {
      return fail<DensifiedCartesianPath>(
          ErrorCode::kInvalidRequest,
          "Cartesian path exceeds its bounded densification budget",
          "cartesian_path_too_dense",
          {{"segment_index", std::to_string(segment - 1U)},
           {"maximum_waypoints",
            std::to_string(kMaximumDensifiedWaypoints)}});
    }
    const auto subdivisions = static_cast<std::size_t>(required);
    Eigen::Quaterniond from_orientation(from.block<3, 3>(0, 0));
    Eigen::Quaterniond to_orientation(to.block<3, 3>(0, 0));
    if (from_orientation.dot(to_orientation) < 0.0) {
      to_orientation.coeffs() *= -1.0;
    }
    for (std::size_t step = 1U; step <= subdivisions; ++step) {
      if (step % kControlStride == 0U) {
        if (auto error =
                control_error(control, "cartesian_path_densification");
            error.has_value()) {
          return core::Result<DensifiedCartesianPath>::Err(
              std::move(*error));
        }
      }
      const double alpha = static_cast<double>(step) /
                           static_cast<double>(subdivisions);
      Eigen::Matrix4d waypoint = Eigen::Matrix4d::Identity();
      waypoint.block<3, 3>(0, 0) =
          from_orientation.slerp(alpha, to_orientation)
              .normalized()
              .toRotationMatrix();
      waypoint.block<3, 1>(0, 3) =
          (1.0 - alpha) * from.block<3, 1>(0, 3) +
          alpha * to.block<3, 1>(0, 3);
      result.poses.push_back(std::move(waypoint));
    }
    result.source_waypoint_dense_indices.push_back(result.poses.size() - 1U);
  }
  return core::Result<DensifiedCartesianPath>::Ok(std::move(result));
}

struct CartesianTrajectoryEnvelope final {
  bool complete{true};
  double joint_velocity_ratio{1.0};
  double joint_acceleration_ratio{1.0};
  double linear_speed_ratio{1.0};
  double angular_speed_ratio{1.0};
  double linear_acceleration_ratio{1.0};
  double angular_acceleration_ratio{1.0};
};

[[nodiscard]] double required_time_scale(
    const CartesianTrajectoryEnvelope& envelope) {
  return std::max(
      {1.0, envelope.joint_velocity_ratio, envelope.linear_speed_ratio,
       envelope.angular_speed_ratio,
       std::sqrt(envelope.joint_acceleration_ratio),
       std::sqrt(envelope.linear_acceleration_ratio),
       std::sqrt(envelope.angular_acceleration_ratio)});
}

void scale_trajectory_time(roboplan::JointTrajectory& trajectory,
                           double scale) {
  const double acceleration_scale = scale * scale;
  for (auto& time : trajectory.times) {
    time *= scale;
  }
  for (auto& velocity : trajectory.velocities) {
    velocity /= scale;
  }
  for (auto& acceleration : trajectory.accelerations) {
    acceleration /= acceleration_scale;
  }
}

void scale_envelope_time(CartesianTrajectoryEnvelope& envelope,
                         double scale) {
  const double acceleration_scale = scale * scale;
  envelope.joint_velocity_ratio /= scale;
  envelope.linear_speed_ratio /= scale;
  envelope.angular_speed_ratio /= scale;
  envelope.joint_acceleration_ratio /= acceleration_scale;
  envelope.linear_acceleration_ratio /= acceleration_scale;
  envelope.angular_acceleration_ratio /= acceleration_scale;
}

[[nodiscard]] detail::CartesianTrajectoryContractInput
cartesian_contract_input(const RoboPlanCandidateBindingDTO& binding,
                         const RoboPlanCandidateRequestDTO& request,
                         const ResolvedDynamicsCaps& caps) {
  return detail::CartesianTrajectoryContractInput{
      binding.compiled_group_name,
      core::compiled_frame_name(request.base_frame),
      core::compiled_frame_name(request.tcp_frame),
      request.speed_profile,
      caps.portable_by_joint};
}

[[nodiscard]] CartesianPlanningControl cartesian_control(
    const RoboPlanCandidateControl& control) {
  return CartesianPlanningControl{control.cancellation_requested,
                                  control.deadline_expired};
}

[[nodiscard]] core::Result<CartesianTrajectoryEnvelope>
measure_cartesian_envelope(
    const detail::CartesianTrajectoryContractInput& contract,
    const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    const Eigen::VectorXd& model_velocity_limits,
    const Eigen::VectorXd& model_acceleration_limits,
    const CartesianPlanningControl& control) {
  CartesianTrajectoryEnvelope envelope;
  if (trajectory.positions.size() < 2U ||
      trajectory.times.size() != trajectory.positions.size() ||
      trajectory.velocities.size() != trajectory.positions.size() ||
      trajectory.accelerations.size() != trajectory.positions.size() ||
      model_velocity_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size()) ||
      model_acceleration_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size())) {
    envelope.complete = false;
    return core::Result<CartesianTrajectoryEnvelope>::Ok(
        std::move(envelope));
  }
  auto joint = detail::measure_cartesian_joint_dynamics_envelope(
      contract, trajectory,
      std::span<const double>(
          model_velocity_limits.data(),
          static_cast<std::size_t>(model_velocity_limits.size())),
      std::span<const double>(
          model_acceleration_limits.data(),
          static_cast<std::size_t>(model_acceleration_limits.size())),
      control);
  if (!joint.ok()) {
    return core::Result<CartesianTrajectoryEnvelope>::Err(joint.error());
  }
  auto frame = detail::measure_cartesian_frame_dynamics_envelope(
      contract, path, scene, trajectory, control);
  if (!frame.ok()) {
    return core::Result<CartesianTrajectoryEnvelope>::Err(frame.error());
  }
  if (!joint.value().complete || !frame.value().complete) {
    envelope.complete = false;
    return core::Result<CartesianTrajectoryEnvelope>::Ok(
        std::move(envelope));
  }
  envelope.joint_velocity_ratio = joint.value().velocity_ratio;
  envelope.joint_acceleration_ratio = joint.value().acceleration_ratio;
  envelope.linear_speed_ratio = frame.value().linear_speed_ratio;
  envelope.angular_speed_ratio = frame.value().angular_speed_ratio;
  envelope.linear_acceleration_ratio =
      frame.value().linear_acceleration_ratio;
  envelope.angular_acceleration_ratio =
      frame.value().angular_acceleration_ratio;
  return core::Result<CartesianTrajectoryEnvelope>::Ok(std::move(envelope));
}

[[nodiscard]] core::Result<roboplan::JointTrajectory>
plan_cartesian_candidate(
    const std::shared_ptr<roboplan::Scene>& scene,
    const RoboPlanCandidateBindingDTO& binding,
    const RoboPlanCandidateRequestDTO& request,
    const core::CartesianPathDTO& path,
    const ResolvedDynamicsCaps& caps,
    const RoboPlanCandidateComputerConfig& config,
    const RoboPlanCandidateControl& control) {
  if (!finite_positive(request.tolerances.translation_tol_m) ||
      !finite_positive(request.tolerances.rotation_tol_rad)) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kInvalidRequest,
        "Cartesian planning requires positive translation and rotation tolerances",
        "cartesian_numeric_option_invalid");
  }
  auto dense = densify_cartesian_path(path, control);
  if (!dense.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(dense.error());
  }
  auto start = core::angular_position_projection(request.start_state);
  if (!start.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(start.error());
  }
  const Eigen::VectorXd group_start = eigen_vector(start.value());
  const Eigen::VectorXd full_start = scene->toFullJointPositions(
      binding.compiled_group_name, group_start);
  const Eigen::Matrix4d actual_start = scene->forwardKinematics(
      full_start, core::compiled_frame_name(request.tcp_frame),
      core::compiled_frame_name(request.base_frame));
  const double start_translation_error =
      (actual_start.block<3, 1>(0, 3) -
       dense.value().poses.front().block<3, 1>(0, 3))
          .norm();
  const double start_rotation_error = rotation_distance(
      actual_start.block<3, 3>(0, 0),
      dense.value().poses.front().block<3, 3>(0, 0));
  if (start_translation_error > request.tolerances.translation_tol_m ||
      start_rotation_error > request.tolerances.rotation_tol_rad) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kInvalidRequest,
        "Cartesian first waypoint does not match the captured start state",
        "cartesian_start_waypoint_mismatch",
        {{"translation_error_m", std::to_string(start_translation_error)},
         {"rotation_error_rad", std::to_string(start_rotation_error)}});
  }

  const auto group = scene->getJointGroupInfo(binding.compiled_group_name);
  const auto model_velocity_limits =
      scene->getVelocityLimitVectors(binding.compiled_group_name);
  const auto model_acceleration_limits =
      scene->getAccelerationLimitVectors(binding.compiled_group_name);
  if (!group.has_value() ||
      group->joint_names != binding.compiled_joint_names ||
      !model_velocity_limits.has_value() ||
      !model_acceleration_limits.has_value() ||
      model_velocity_limits->second.size() !=
          static_cast<Eigen::Index>(binding.resources.size()) ||
      model_acceleration_limits->second.size() !=
          static_cast<Eigen::Index>(binding.resources.size())) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kUnsupportedCapability,
        "Cartesian planning requires a complete retained model dynamics projection",
        "cartesian_model_dynamics_limits_unavailable");
  }

  roboplan::CartesianPlannerOptions options;
  options.group_name = binding.compiled_group_name;
  options.dt = config.trajectory_sample_period_s;
  options.speed_mode = roboplan::CartesianSpeedMode::Bounded;
  options.max_linear_speed = path.max_linear_speed_mps;
  if (request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    options.max_linear_speed =
        std::min(options.max_linear_speed,
                 *request.speed_profile.max_tcp_linear_speed_mps);
  }
  options.max_angular_speed = path.max_angular_speed_rad_s;
  options.max_linear_acceleration = path.max_linear_acceleration_mps2;
  options.max_angular_acceleration = path.max_angular_acceleration_rad_s2;
  options.max_position_error = request.tolerances.translation_tol_m;
  options.max_orientation_error = request.tolerances.rotation_tol_rad;
  options.velocity_scale = request.speed_profile.velocity_scale;
  options.acceleration_scale = request.speed_profile.acceleration_scale;
  options.toppra_blend_deviation = 0.0;

  if (auto error = control_error(control, "before_cartesian_tracking");
      error.has_value()) {
    return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
  }
  roboplan::CartesianPathPlanner planner(scene, options);
  const roboplan::CartesianPath native_path(
      {core::compiled_frame_name(request.base_frame)},
      {core::compiled_frame_name(request.tcp_frame)},
      {dense.value().poses});
  roboplan::JointConfiguration native_start;
  native_start.positions = full_start;
  auto planned = planner.plan(native_path, native_start);
  if (!planned.has_value()) {
    if (auto error = control_error(control, "cartesian_tracking");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    return core::Result<roboplan::JointTrajectory>::Err(
        map_cartesian_backend_failure(planned.error(), 0.0, 0U));
  }
  auto trajectory = std::move(planned.value());
  const auto contract = cartesian_contract_input(binding, request, caps);
  const auto contract_control = cartesian_control(control);
  if (const auto error =
          detail::validate_and_normalize_cartesian_trajectory_output(
              trajectory, binding.compiled_joint_names, contract_control);
      error.has_value()) {
    return core::Result<roboplan::JointTrajectory>::Err(*error);
  }

  auto interpolant = detail::sample_cartesian_interpolant_path(
      contract, scene, trajectory, request.tolerances.translation_tol_m,
      request.tolerances.rotation_tol_rad, contract_control);
  if (!interpolant.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(interpolant.error());
  }
  const double certified_translation =
      request.tolerances.translation_tol_m -
      interpolant.value().unsampled_translation_bound_m;
  const double certified_rotation =
      request.tolerances.rotation_tol_rad -
      interpolant.value().unsampled_rotation_bound_rad;
  if (!finite_positive(certified_translation) ||
      !finite_positive(certified_rotation)) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kCartesianIncomplete,
        "Cartesian continuous-path fidelity margin cannot be represented",
        "cartesian_resource_budget_exceeded",
        {{"failure_stage", "cartesian_output_validation"},
         {"resource", "interpolant_fidelity_margin"}});
  }
  auto fidelity = verify_cartesian_path_fidelity_controlled(
      dense.value().poses, interpolant.value().poses,
      certified_translation, certified_rotation, contract_control);
  if (!fidelity.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(fidelity.error());
  }
  if (!fidelity.value().complete) {
    std::map<std::string, std::string> details{
        {"reason", "cartesian_path_tolerance_exceeded"},
        {"failure_stage", "cartesian_output_validation"},
        {"fraction_completed",
         std::to_string(fidelity.value().fraction_completed)},
        {"max_translation_error_m",
         std::to_string(fidelity.value().max_translation_error_m)},
        {"max_rotation_error_rad",
         std::to_string(fidelity.value().max_rotation_error_rad)}};
    if (fidelity.value().first_failing_path_sample_index.has_value()) {
      details["first_failing_path_sample_index"] = std::to_string(
          *fidelity.value().first_failing_path_sample_index);
    }
    return core::Result<roboplan::JointTrajectory>::Err(make_error(
        ErrorCode::kCartesianIncomplete,
        "Cartesian trajectory deviated from the requested path",
        "cartesian_path_tolerance_exceeded", std::move(details)));
  }

  auto envelope = measure_cartesian_envelope(
      contract, path, scene, trajectory, model_velocity_limits->second,
      model_acceleration_limits->second, contract_control);
  if (!envelope.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(envelope.error());
  }
  if (!envelope.value().complete) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kValidationLimitsViolation,
        "Cartesian trajectory does not have a complete dynamics envelope",
        "cartesian_trajectory_envelope_incomplete");
  }
  constexpr double kNumericalTolerance = 1.0 + 1.0e-6;
  constexpr double kRetimeHeadroom = 1.001;
  const double scale = required_time_scale(envelope.value());
  if (scale > kNumericalTolerance) {
    const double applied = scale * kRetimeHeadroom;
    scale_trajectory_time(trajectory, applied);
    scale_envelope_time(envelope.value(), applied);
  }
  if (!envelope.value().complete ||
      required_time_scale(envelope.value()) > kNumericalTolerance) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kValidationLimitsViolation,
        "Cartesian trajectory exceeds a requested dynamics limit after retiming",
        "cartesian_trajectory_limit_violation");
  }
  return core::Result<roboplan::JointTrajectory>::Ok(
      std::move(trajectory));
}

[[nodiscard]] core::Result<roboplan::JointTrajectory>
retime_joint_candidate(
    const std::shared_ptr<roboplan::Scene>& scene,
    const RoboPlanCandidateBindingDTO& binding,
    const RoboPlanCandidateRequestDTO& request,
    const ResolvedDynamicsCaps& caps,
    const RoboPlanCandidateComputerConfig& config,
    const RoboPlanCandidateControl& control,
    const roboplan::JointPath& path,
    const Eigen::VectorXd& start,
    const Eigen::VectorXd& goal) {
  roboplan::PathParameterizerTOPPRA parameterizer(
      scene, binding.compiled_group_name);
  constexpr double kModelLimitQualificationMargin = 0.995;
  constexpr double kSpeedMargin = 0.99;
  constexpr int kMaximumRetimeIterations = 32;
  double velocity_scale = request.speed_profile.velocity_scale *
                          kModelLimitQualificationMargin;
  double acceleration_scale = request.speed_profile.acceleration_scale *
                              kModelLimitQualificationMargin;
  const bool has_tcp_cap =
      request.speed_profile.max_tcp_linear_speed_mps.has_value();
  const bool has_joint_caps = !caps.velocity_by_joint.empty() ||
                              !caps.acceleration_by_joint.empty();
  double final_tcp_speed = 0.0;
  JointCapMeasurement final_joint_caps;
  roboplan::JointTrajectory final_trajectory;
  for (int iteration = 0; iteration < kMaximumRetimeIterations; ++iteration) {
    if (auto error = control_error(control, "joint_time_parameterization");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    roboplan::TOPPRAOptions options;
    options.dt = config.trajectory_sample_period_s;
    options.mode = roboplan::SplineFittingMode::Hermite;
    options.velocity_scale = velocity_scale;
    options.acceleration_scale = acceleration_scale;
    options.max_output_samples = config.maximum_trajectory_samples;
    auto generated = parameterizer.generate(path, options);
    if (!generated.has_value()) {
      return fail<roboplan::JointTrajectory>(
          ErrorCode::kPlanningFailed,
          "RoboPlan TOPP-RA could not time-parameterize the candidate",
          "native_time_parameterization_failed",
          {{"planner_error", generated.error()}});
    }
    final_trajectory = std::move(generated.value());
    anchor_trajectory_endpoints(final_trajectory, start, goal);
    if (auto error = control_error(control, "joint_dynamics_validation");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    final_tcp_speed = has_tcp_cap
                          ? maximum_tcp_speed(
                                scene, binding.compiled_group_name,
                                core::compiled_frame_name(request.base_frame),
                                core::compiled_frame_name(request.tcp_frame),
                                final_trajectory)
                          : 0.0;
    final_joint_caps = measure_joint_cap_peaks(
        final_trajectory, caps.velocity_by_joint,
        caps.acceleration_by_joint);
    if (has_joint_caps && !final_joint_caps.samples_complete) {
      return fail<roboplan::JointTrajectory>(
          ErrorCode::kUnsupportedCapability,
          "Trajectory lacks samples needed to prove a dynamics cap",
          "unsupported_joint_dynamics_limit",
          {{"dimension", final_joint_caps.incomplete_dimension},
           {"compiled_joint_name", final_joint_caps.incomplete_joint}});
    }
    const bool tcp_ok =
        !has_tcp_cap ||
        final_tcp_speed <=
            *request.speed_profile.max_tcp_linear_speed_mps;
    const bool joints_ok = final_joint_caps.worst_vel_ratio <= 1.0 &&
                           final_joint_caps.worst_acc_ratio <= 1.0;
    if (tcp_ok && joints_ok) {
      return core::Result<roboplan::JointTrajectory>::Ok(
          std::move(final_trajectory));
    }

    double velocity_factor = 1.0;
    double acceleration_factor = 1.0;
    if (!tcp_ok) {
      velocity_factor = std::min(
          velocity_factor,
          (*request.speed_profile.max_tcp_linear_speed_mps * kSpeedMargin) /
              final_tcp_speed);
    }
    if (final_joint_caps.worst_vel_ratio > 1.0) {
      velocity_factor = std::min(
          velocity_factor,
          kSpeedMargin / final_joint_caps.worst_vel_ratio);
    }
    if (final_joint_caps.worst_acc_ratio > 1.0) {
      acceleration_factor = std::min(
          acceleration_factor,
          kSpeedMargin / final_joint_caps.worst_acc_ratio);
      velocity_factor = std::min(
          velocity_factor,
          kSpeedMargin / std::sqrt(final_joint_caps.worst_acc_ratio));
    }
    velocity_scale =
        std::max(0.001, velocity_scale * velocity_factor);
    acceleration_scale =
        std::max(0.001, acceleration_scale * acceleration_factor);
  }

  if (has_tcp_cap &&
      final_tcp_speed > *request.speed_profile.max_tcp_linear_speed_mps) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kValidationLimitsViolation,
        "TCP speed cap could not be enforced within the retiming budget",
        "tcp_linear_speed_limit_violation",
        {{"measured_mps", std::to_string(final_tcp_speed)},
         {"limit_mps", std::to_string(
                           *request.speed_profile.max_tcp_linear_speed_mps)}});
  }
  return fail<roboplan::JointTrajectory>(
      ErrorCode::kUnsupportedCapability,
      "Per-resource dynamics cap could not be enforced within the retiming budget",
      "unsupported_joint_dynamics_limit",
      {{"worst_velocity_ratio",
        std::to_string(final_joint_caps.worst_vel_ratio)},
       {"worst_acceleration_ratio",
        std::to_string(final_joint_caps.worst_acc_ratio)}});
}

[[nodiscard]] core::Result<std::vector<double>> solve_ik_locked(
    const std::shared_ptr<roboplan::Scene>& scene,
    const RoboPlanCandidateBindingDTO& binding,
    const core::Pose3DTO& goal,
    const core::ResolvedFrameDTO& base_frame,
    const core::ResolvedFrameDTO& tcp_frame,
    const core::ControlledStateDTO& seed_state,
    double timeout_s,
    const RoboPlanCandidateControl& control,
    core::PlanningProfileDTO profile,
    const core::TolerancesDTO* tolerances = nullptr) {
  if (!valid_pose(goal) || !finite_positive(timeout_s) ||
      !contains_frame(binding.permitted_frames, base_frame) ||
      !contains_frame(binding.permitted_frames, tcp_frame)) {
    return fail<std::vector<double>>(
        ErrorCode::kInvalidRequest,
        "RoboPlan IK input is malformed", "native_ik_request_invalid");
  }
  const auto state_validation = core::validate_controlled_state(seed_state);
  if (!state_validation.ok() ||
      seed_state.controllable_component_id !=
          binding.controllable_component_id ||
      seed_state.command_target_id != binding.command_target_id ||
      seed_state.resources != binding.resources ||
      seed_state.joint_names != binding.compiled_joint_names) {
    return fail<std::vector<double>>(
        ErrorCode::kStartStateMismatch,
        "RoboPlan IK seed does not match the exact participant binding",
        "native_ik_seed_binding_mismatch");
  }
  auto positions = core::angular_position_projection(seed_state);
  if (!positions.ok()) {
    return core::Result<std::vector<double>>::Err(positions.error());
  }
  if (auto error = control_error(control, "before_ik"); error.has_value()) {
    return core::Result<std::vector<double>>::Err(std::move(*error));
  }

  auto options = planning_profile_ik_options(
      production_planning_ik_options(binding.compiled_group_name), profile);
  options.max_time = std::min(options.max_time, timeout_s);
  if (tolerances != nullptr) {
    if (finite_positive(tolerances->translation_tol_m)) {
      options.max_linear_error_norm =
          std::min(options.max_linear_error_norm,
                   tolerances->translation_tol_m);
    }
    if (finite_positive(tolerances->rotation_tol_rad)) {
      options.max_angular_error_norm =
          std::min(options.max_angular_error_norm,
                   tolerances->rotation_tol_rad);
    }
  }
  roboplan::SimpleIk solver(scene, options);
  const roboplan::CartesianConfiguration native_goal{
      core::compiled_frame_name(base_frame),
      core::compiled_frame_name(tcp_frame), native_pose(goal)};
  const roboplan::JointConfiguration native_seed{
      binding.compiled_joint_names, eigen_vector(positions.value()), {}, {}};
  roboplan::JointConfiguration solution;
  if (!solver.solveIk(native_goal, native_seed, solution)) {
    return fail<std::vector<double>>(
        ErrorCode::kIkFailed, "RoboPlan could not resolve the pose goal",
        "native_ik_no_solution");
  }
  if (auto error = control_error(control, "after_ik"); error.has_value()) {
    return core::Result<std::vector<double>>::Err(std::move(*error));
  }
  if (solution.joint_names != binding.compiled_joint_names ||
      solution.positions.size() !=
          static_cast<Eigen::Index>(binding.resources.size()) ||
      !solution.positions.allFinite()) {
    return fail<std::vector<double>>(
        ErrorCode::kInvariantViolation,
        "RoboPlan IK returned a non-canonical result",
        "native_ik_result_invalid");
  }
  return core::Result<std::vector<double>>::Ok(
      standard_vector(solution.positions));
}

}  // namespace

RoboPlanCandidateComputer::RoboPlanCandidateComputer(
    std::shared_ptr<roboplan::Scene> scene,
    RoboPlanCandidateBindingDTO binding,
    RoboPlanCandidateComputerConfig config)
    : scene_(std::move(scene)),
      binding_(std::move(binding)),
      config_(std::move(config)) {
  if (!scene_) {
    throw std::invalid_argument("RoboPlan candidate scene must not be null");
  }
  const auto validation = validate_binding(*scene_, binding_, config_);
  if (!validation.ok()) {
    throw std::invalid_argument(validation.error().message);
  }
}

core::Result<std::vector<double>> RoboPlanCandidateComputer::solve_ik(
    const core::Pose3DTO& goal,
    const core::ResolvedFrameDTO& base_frame,
    const core::ResolvedFrameDTO& tcp_frame,
    const core::ControlledStateDTO& seed_state,
    double timeout_s,
  const RoboPlanCandidateControl& control) {
  std::lock_guard lock(computation_mutex_);
  try {
    if (config_.rng_seed.has_value()) {
      scene_->setRngSeed(*config_.rng_seed);
    }
    return solve_ik_locked(scene_, binding_, goal, base_frame, tcp_frame,
                           seed_state,
                           timeout_s, control,
                           core::PlanningProfileDTO::kFirstFeasible);
  } catch (const std::exception& error) {
    return fail<std::vector<double>>(
        ErrorCode::kPlanningFailed,
        "RoboPlan IK adapter failed: " + std::string(error.what()),
        "native_ik_adapter_failure");
  }
}

core::Result<core::TrajectoryDTO> RoboPlanCandidateComputer::compute(
    const RoboPlanCandidateRequestDTO& request,
    const RoboPlanCandidateControl& control) {
  const auto request_validation = validate_request_binding(binding_, request);
  if (!request_validation.ok()) {
    return core::Result<core::TrajectoryDTO>::Err(
        request_validation.error());
  }
  auto caps = resolve_dynamics_caps(binding_, request.dynamics_limits);
  if (!caps.ok()) {
    return core::Result<core::TrajectoryDTO>::Err(caps.error());
  }

  std::lock_guard lock(computation_mutex_);
  try {
    if (auto error = control_error(control, "before_scene_access");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }
    const auto& planning_scene = scene_;
    const std::optional<std::uint32_t> request_seed =
        config_.rng_seed.has_value()
            ? std::optional<std::uint32_t>(effective_seed(
                  *config_.rng_seed, request.planning_attempt_index))
            : std::nullopt;
    if (request_seed.has_value()) {
      planning_scene->setRngSeed(*request_seed);
    }
    auto start_positions =
        core::angular_position_projection(request.start_state);
    if (!start_positions.ok()) {
      return core::Result<core::TrajectoryDTO>::Err(
          start_positions.error());
    }
    const Eigen::VectorXd native_start_positions =
        eigen_vector(start_positions.value());
    const roboplan::JointConfiguration native_start{
        binding_.compiled_joint_names, native_start_positions, {}, {}};
    const Eigen::VectorXd full_start = planning_scene->toFullJointPositions(
        binding_.compiled_group_name, native_start_positions);
    if (planning_scene->hasCollisions(full_start)) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed,
          "Planning start state is in collision",
          "native_start_state_collision",
          {{"failure_stage", "start_state"}});
    }
    if (auto error = control_error(control, "start_collision_check");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }

    if (const auto* cartesian =
            std::get_if<core::CartesianPathDTO>(&request.motion)) {
      auto trajectory = plan_cartesian_candidate(
          planning_scene, binding_, request, *cartesian, caps.value(),
          config_, control);
      if (!trajectory.ok()) {
        return core::Result<core::TrajectoryDTO>::Err(trajectory.error());
      }
      return portable_trajectory(trajectory.value(), binding_,
                                 config_.maximum_trajectory_samples);
    }

    roboplan::JointConfiguration native_goal;
    native_goal.joint_names = binding_.compiled_joint_names;
    bool resolved_from_pose = false;
    if (const auto* joint =
            std::get_if<core::ResolvedJointGoalDTO>(&request.motion)) {
      if (joint->resources != binding_.resources ||
          joint->joint_names != binding_.compiled_joint_names ||
          joint->positions_rad.size() != binding_.resources.size() ||
          !std::all_of(joint->positions_rad.begin(),
                       joint->positions_rad.end(), finite)) {
        return fail<core::TrajectoryDTO>(
            ErrorCode::kInvalidRequest,
            "Resolved joint goal differs from the exact participant binding",
            "native_joint_goal_binding_mismatch");
      }
      native_goal.positions = eigen_vector(joint->positions_rad);
    } else if (const auto* pose =
                   std::get_if<core::PoseGoalDTO>(&request.motion)) {
      auto solution = solve_ik_locked(
          planning_scene, binding_, pose->goal, request.base_frame,
          request.tcp_frame, request.start_state,
          request.planning_timeout_s, control, request.planning_profile,
          &request.tolerances);
      if (!solution.ok()) {
        return core::Result<core::TrajectoryDTO>::Err(solution.error());
      }
      native_goal.positions = eigen_vector(solution.value());
      normalize_ik_goal_for_limits(
          planning_scene, binding_.compiled_group_name,
          native_start.positions, native_goal.positions);
      resolved_from_pose = true;
    } else if (std::holds_alternative<core::NamedTargetDTO>(request.motion)) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kInvalidRequest,
          "Named targets must be resolved before candidate computation",
          "unresolved_named_target");
    } else {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kUnsupportedCapability,
          "RoboPlan candidate motion type is unsupported",
          "native_motion_type_unsupported");
    }

    if (native_goal.positions.size() != native_start.positions.size() ||
        !native_goal.positions.allFinite()) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kInvalidRequest,
          "Resolved goal configuration has an invalid shape",
          "native_goal_shape_invalid");
    }
    const Eigen::VectorXd full_goal = planning_scene->toFullJointPositions(
        binding_.compiled_group_name, native_goal.positions);
    if (planning_scene->hasCollisions(full_goal)) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed,
          "Planning goal state is in collision",
          "native_goal_state_collision",
          {{"failure_stage", "goal_state"},
           {"goal_source", resolved_from_pose ? "pose" : "joint"}});
    }
    if (auto error = control_error(control, "goal_collision_check");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }

    auto rrt_options = planning_profile_rrt_options(
        production_rrt_options(binding_.compiled_group_name,
                               request.planning_timeout_s),
        request.planning_profile);
    roboplan::RRT planner(planning_scene, rrt_options);
    if (request_seed.has_value()) {
      planner.setRngSeed(*request_seed);
    }
    auto path = planner.plan(native_start, native_goal);
    if (auto error = control_error(control, "rrt"); error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }
    if (!path.has_value()) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed,
          "RoboPlan could not find a collision-free joint path",
          "native_rrt_failed",
          {{"planner_error", path.error()}});
    }

    const auto shortcut_options = production_shortcutting_options(
        binding_.compiled_group_name, request_seed);
    roboplan::PathShortcutter shortcutter(planning_scene,
                                         shortcut_options);
    auto shortened = shortcutter.shortcut(path.value());
    if (auto error = control_error(control, "shortcut"); error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }
    auto trajectory = retime_joint_candidate(
        planning_scene, binding_, request, caps.value(), config_, control,
        shortened, native_start.positions, native_goal.positions);
    if (!trajectory.ok()) {
      return core::Result<core::TrajectoryDTO>::Err(trajectory.error());
    }
    if (auto error = control_error(control, "trajectory_conversion");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }
    return portable_trajectory(trajectory.value(), binding_,
                               config_.maximum_trajectory_samples);
  } catch (const std::exception& error) {
    return fail<core::TrajectoryDTO>(
        ErrorCode::kPlanningFailed,
        "RoboPlan candidate adapter failed: " + std::string(error.what()),
        "native_candidate_adapter_failure");
  }
}

}  // namespace manyforge::roboplan_backend
