#include <manyforge_roboplan/roboplan_candidate_computer.hpp>

#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>

#include <algorithm>
#include <array>
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

[[nodiscard]] ErrorDTO
make_error(ErrorCode code, std::string message, std::string reason,
           std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  return ErrorDTO{std::string(core::to_string(code)), std::move(message),
                  std::move(details)};
}

template <typename Value>
[[nodiscard]] core::Result<Value>
fail(ErrorCode code, std::string message, std::string reason,
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

[[nodiscard]] std::map<std::string, std::string>
collision_failure_details(const roboplan::Scene &scene,
                          const Eigen::VectorXd &positions,
                          std::string failure_stage, std::string source_field,
                          std::string source_value) {
  std::map<std::string, std::string> details{
      {"failure_stage", std::move(failure_stage)},
      {std::move(source_field), std::move(source_value)},
  };
  // RoboPlan's production broadphase query stops at the first collision but
  // does not populate Scene::getCollisionData().collisionResults. Recompute
  // distances only on this already-failed path so the diagnostic can name the
  // exact pair without slowing successful planning or changing admission.
  scene.computeCollisionDistances(positions);
  const auto &model = scene.getCollisionModel();
  const auto &data = scene.getCollisionData();
  const auto pair_count =
      std::min(model.collisionPairs.size(), data.collisionResults.size());
  for (std::size_t index = 0U; index < pair_count; ++index) {
    if (!data.collisionResults[index].isCollision()) {
      continue;
    }
    const auto &pair = model.collisionPairs[index];
    if (pair.first < model.geometryObjects.size() &&
        pair.second < model.geometryObjects.size()) {
      details.emplace("collision_body_a",
                      model.geometryObjects[pair.first].name);
      details.emplace("collision_body_b",
                      model.geometryObjects[pair.second].name);
    }
    return details;
  }
  const auto distance_count =
      std::min(model.collisionPairs.size(), data.distanceResults.size());
  for (std::size_t index = 0U; index < distance_count; ++index) {
    const auto distance = data.distanceResults[index].min_distance;
    if (!std::isfinite(distance) || distance > 0.0) {
      continue;
    }
    const auto &pair = model.collisionPairs[index];
    if (pair.first < model.geometryObjects.size() &&
        pair.second < model.geometryObjects.size()) {
      details.emplace("collision_body_a",
                      model.geometryObjects[pair.first].name);
      details.emplace("collision_body_b",
                      model.geometryObjects[pair.second].name);
      details.emplace("collision_signed_distance_m", std::to_string(distance));
    }
    break;
  }
  return details;
}

[[nodiscard]] std::string collision_failure_message(
    std::string message, const std::map<std::string, std::string> &details) {
  const auto body_a = details.find("collision_body_a");
  const auto body_b = details.find("collision_body_b");
  if (body_a != details.end() && body_b != details.end()) {
    message += ": " + body_a->second.substr(0U, 256U) + " <-> " +
               body_b->second.substr(0U, 256U);
  }
  return message;
}

template <typename Value>
[[nodiscard]] bool unique_nonempty(const std::vector<Value> &values) {
  return !values.empty() &&
         std::set<Value>(values.begin(), values.end()).size() == values.size();
}

[[nodiscard]] bool
unique_nonempty_strings(const std::vector<std::string> &values) {
  return !values.empty() &&
         std::all_of(values.begin(), values.end(),
                     [](const std::string &value) { return !value.empty(); }) &&
         std::set<std::string>(values.begin(), values.end()).size() ==
             values.size();
}

[[nodiscard]] Eigen::VectorXd eigen_vector(const std::vector<double> &values) {
  return Eigen::Map<const Eigen::VectorXd>(
      values.data(), static_cast<Eigen::Index>(values.size()));
}

[[nodiscard]] std::vector<double>
standard_vector(const Eigen::VectorXd &values) {
  return {values.data(), values.data() + values.size()};
}

[[nodiscard]] Eigen::Matrix4d native_pose(const core::Pose3DTO &pose) {
  Eigen::Quaterniond orientation(pose.orientation.w, pose.orientation.x,
                                 pose.orientation.y, pose.orientation.z);
  const double scale = orientation.coeffs().cwiseAbs().maxCoeff();
  orientation.coeffs() /= scale;
  orientation.normalize();
  Eigen::Matrix4d transform = Eigen::Matrix4d::Identity();
  transform.block<3, 3>(0, 0) = orientation.normalized().toRotationMatrix();
  transform(0, 3) = pose.position_m.x_m;
  transform(1, 3) = pose.position_m.y_m;
  transform(2, 3) = pose.position_m.z_m;
  return transform;
}

[[nodiscard]] bool valid_pose(const core::Pose3DTO &pose) noexcept {
  const auto &q = pose.orientation;
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

[[nodiscard]] std::optional<ErrorDTO>
control_error(const RoboPlanCandidateControl &control, std::string stage) {
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

[[nodiscard]] std::uint32_t
effective_seed(std::uint32_t base_seed, std::uint32_t attempt_index) noexcept {
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

void anchor_trajectory_endpoints(roboplan::JointTrajectory &trajectory,
                                 const Eigen::VectorXd &start_positions,
                                 const Eigen::VectorXd &goal_positions) {
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

[[nodiscard]] core::Result<core::TrajectoryDTO>
portable_trajectory(const roboplan::JointTrajectory &native,
                    const RoboPlanCandidateBindingDTO &binding,
                    std::size_t maximum_samples,
                    core::TrajectoryRuntimeScaleBoundsDTO scale_bounds) {
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
  result.runtime_scale_bounds = scale_bounds;
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
    const bool finite_values =
        native.positions[index].allFinite() &&
        (native.velocities.empty() || native.velocities[index].allFinite()) &&
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

[[nodiscard]] bool frame_exists(const roboplan::Scene &scene,
                                const core::ResolvedFrameDTO &frame) {
  return scene.getFrameId(core::compiled_frame_name(frame)).has_value();
}

[[nodiscard]] bool frame_identity_valid(const core::ResolvedFrameDTO &frame) {
  if (core::compiled_frame_name(frame).empty()) {
    return false;
  }
  if (const auto *cell = std::get_if<core::ResolvedCellFrameDTO>(&frame)) {
    return !cell->cell_id.empty();
  }
  if (const auto *component =
          std::get_if<core::ResolvedComponentModelFrameDTO>(&frame)) {
    return !component->controllable_component_id.empty() &&
           !component->model_instance_id.empty() &&
           !component->frame_role_id.empty() &&
           !component->model_revision.empty();
  }
  const auto &passive = std::get<core::ResolvedPassiveModelFrameDTO>(frame);
  return !passive.model_instance_id.empty() && !passive.frame_role_id.empty() &&
         !passive.model_revision.empty();
}

[[nodiscard]] bool
contains_frame(const std::vector<core::ResolvedFrameDTO> &frames,
               const core::ResolvedFrameDTO &frame) {
  return std::count(frames.begin(), frames.end(), frame) == 1;
}

[[nodiscard]] bool
unique_frames(const std::vector<core::ResolvedFrameDTO> &frames) {
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

[[nodiscard]] core::Result<void>
validate_binding(const roboplan::Scene &scene,
                 const RoboPlanCandidateBindingDTO &binding,
                 const RoboPlanCandidateComputerConfig &config) {
  if (binding.controllable_component_id.empty() ||
      binding.command_target_id.empty() || binding.model_instance_id.empty() ||
      binding.model_revision.empty() || binding.compiled_group_name.empty() ||
      !unique_nonempty(binding.resources) ||
      !unique_nonempty_strings(binding.compiled_joint_names) ||
      binding.resources.size() != binding.compiled_joint_names.size() ||
      !unique_frames(binding.permitted_frames) ||
      !std::all_of(
          binding.permitted_frames.begin(), binding.permitted_frames.end(),
          [&](const auto &frame) { return frame_exists(scene, frame); }) ||
      config.maximum_trajectory_samples < 2U ||
      config.maximum_trajectory_samples > 1'000'000U ||
      !finite_positive(config.trajectory_sample_period_s)) {
    return core::Result<void>::Err(
        make_error(ErrorCode::kInvalidRequest,
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

[[nodiscard]] bool
valid_planning_profile(core::PlanningProfileDTO profile) noexcept {
  switch (profile) {
  case core::PlanningProfileDTO::kFirstFeasible:
  case core::PlanningProfileDTO::kBalanced:
  case core::PlanningProfileDTO::kQualityBounded:
    return true;
  }
  return false;
}

[[nodiscard]] core::Result<void>
validate_request_binding(const RoboPlanCandidateBindingDTO &binding,
                         const RoboPlanCandidateRequestDTO &request) {
  if (request.controllable_component_id != binding.controllable_component_id ||
      request.command_target_id != binding.command_target_id ||
      request.model_instance_id != binding.model_instance_id ||
      request.model_revision != binding.model_revision ||
      request.resources != binding.resources ||
      !contains_frame(binding.permitted_frames, request.base_frame) ||
      !contains_frame(binding.permitted_frames, request.tcp_frame)) {
    return core::Result<void>::Err(
        make_error(ErrorCode::kInvalidRequest,
                   "RoboPlan request does not match the exact retained "
                   "participant binding",
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
  const auto &speed = request.speed_profile;
  const auto &tolerances = request.tolerances;
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
  core::PerJointDynamicsLimitsDTO portable_by_joint;
};

[[nodiscard]] core::Result<ResolvedDynamicsCaps>
resolve_dynamics_caps(const RoboPlanCandidateBindingDTO &binding,
                      const core::PerResourceDynamicsLimitsDTO &requested) {
  ResolvedDynamicsCaps result;
  std::optional<std::size_t> previous_index;
  for (const auto &entry : requested) {
    const auto resource = std::find(binding.resources.begin(),
                                    binding.resources.end(), entry.resource_id);
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
    const auto &limit = entry.limit;
    const auto malformed = [](const std::optional<double> &value) {
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
    result.portable_by_joint.emplace(entry.compiled_joint_name, limit);
  }
  return core::Result<ResolvedDynamicsCaps>::Ok(std::move(result));
}

[[nodiscard]] double rotation_distance(const Eigen::Matrix3d &lhs,
                                       const Eigen::Matrix3d &rhs) {
  Eigen::Quaterniond relative(lhs.transpose() * rhs);
  relative.normalize();
  return 2.0 * std::acos(std::clamp(std::abs(relative.w()), 0.0, 1.0));
}

void normalize_ik_goal_for_limits(const std::shared_ptr<roboplan::Scene> &scene,
                                  std::string_view group_name,
                                  const Eigen::VectorXd &start,
                                  Eigen::VectorXd &goal) {
  const auto limits =
      scene->getPositionLimitVectors(std::string(group_name), false);
  if (!limits.has_value() || limits->first.size() != goal.size() ||
      limits->second.size() != goal.size() || start.size() != goal.size()) {
    return;
  }
  constexpr long double kTwoPi = 2.0L * 3.141592653589793238462643383279502884L;
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
    const long double first =
        std::ceil((static_cast<long double>(lower) - value) / kTwoPi);
    const long double last =
        std::floor((static_cast<long double>(upper) - value) / kTwoPi);
    if (!std::isfinite(first) || !std::isfinite(last) || first > last) {
      continue;
    }
    const long double ideal =
        std::round((static_cast<long double>(start[index]) - value) / kTwoPi);
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

[[nodiscard]] core::Result<DensifiedCartesianPath>
densify_cartesian_path(const core::CartesianPathDTO &path,
                       const RoboPlanCandidateControl &control) {
  constexpr std::size_t kMaximumInputWaypoints = 4096U;
  constexpr std::size_t kMaximumDensifiedWaypoints = 100'000U;
  constexpr std::size_t kControlStride = 64U;
  if (path.waypoints.size() < 2U ||
      path.waypoints.size() > kMaximumInputWaypoints ||
      !finite_positive(path.max_translation_step_m) ||
      !finite_positive(path.max_rotation_step_rad) ||
      path.speed_mode != core::CartesianSpeedModeDTO::kBounded ||
      path.fallback_policy != core::CartesianFallbackPolicyDTO::kFailClosed ||
      !finite_positive(path.max_angular_speed_rad_s) ||
      !finite_positive(path.max_linear_acceleration_mps2) ||
      !finite_positive(path.max_angular_acceleration_rad_s2) ||
      !std::isfinite(path.max_blend_deviation_rad) ||
      path.max_blend_deviation_rad < 0.0) {
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
    const auto &from = source[segment - 1U];
    const auto &to = source[segment];
    const double translation =
        (to.block<3, 1>(0, 3) - from.block<3, 1>(0, 3)).norm();
    const double rotation =
        rotation_distance(from.block<3, 3>(0, 0), to.block<3, 3>(0, 0));
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
           {"maximum_waypoints", std::to_string(kMaximumDensifiedWaypoints)}});
    }
    const auto subdivisions = static_cast<std::size_t>(required);
    Eigen::Quaterniond from_orientation(from.block<3, 3>(0, 0));
    Eigen::Quaterniond to_orientation(to.block<3, 3>(0, 0));
    if (from_orientation.dot(to_orientation) < 0.0) {
      to_orientation.coeffs() *= -1.0;
    }
    for (std::size_t step = 1U; step <= subdivisions; ++step) {
      if (step % kControlStride == 0U) {
        if (auto error = control_error(control, "cartesian_path_densification");
            error.has_value()) {
          return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
        }
      }
      const double alpha =
          static_cast<double>(step) / static_cast<double>(subdivisions);
      Eigen::Matrix4d waypoint = Eigen::Matrix4d::Identity();
      waypoint.block<3, 3>(0, 0) = from_orientation.slerp(alpha, to_orientation)
                                       .normalized()
                                       .toRotationMatrix();
      waypoint.block<3, 1>(0, 3) =
          (1.0 - alpha) * from.block<3, 1>(0, 3) + alpha * to.block<3, 1>(0, 3);
      result.poses.push_back(std::move(waypoint));
    }
    result.source_waypoint_dense_indices.push_back(result.poses.size() - 1U);
  }
  return core::Result<DensifiedCartesianPath>::Ok(std::move(result));
}

void scale_trajectory_time(roboplan::JointTrajectory &trajectory,
                           double scale) {
  const double acceleration_scale = scale * scale;
  for (auto &time : trajectory.times) {
    time *= scale;
  }
  for (auto &velocity : trajectory.velocities) {
    velocity /= scale;
  }
  for (auto &acceleration : trajectory.accelerations) {
    acceleration /= acceleration_scale;
  }
}

[[nodiscard]] detail::CartesianTrajectoryContractInput
cartesian_contract_input(const RoboPlanCandidateBindingDTO &binding,
                         const RoboPlanCandidateRequestDTO &request,
                         const ResolvedDynamicsCaps &caps) {
  return detail::CartesianTrajectoryContractInput{
      binding.compiled_group_name,
      core::compiled_frame_name(request.base_frame),
      core::compiled_frame_name(request.tcp_frame), request.speed_profile,
      caps.portable_by_joint};
}

[[nodiscard]] CartesianPlanningControl
cartesian_control(const RoboPlanCandidateControl &control) {
  return CartesianPlanningControl{control.cancellation_requested,
                                  control.deadline_expired};
}

// Certify the final P/V/A curve once, then stretch all its coordinates
// together. TOPPRA remains the geometric/timing producer; this is not another
// planner.
[[nodiscard]] core::Result<core::TrajectoryRuntimeScaleBoundsDTO>
prepare_candidate_time_law(
    const detail::CartesianTrajectoryContractInput &contract,
    const core::CartesianPathDTO *path,
    const std::shared_ptr<roboplan::Scene> &scene,
    roboplan::JointTrajectory &trajectory,
    const Eigen::VectorXd &model_velocity_limits,
    const Eigen::VectorXd &model_acceleration_limits,
    const CartesianPlanningControl &control,
    std::vector<double>* cartesian_waypoint_times_s = nullptr) {
  const auto incomplete = [] {
    return fail<core::TrajectoryRuntimeScaleBoundsDTO>(
        ErrorCode::kValidationLimitsViolation,
        "Trajectory lacks a complete runtime dynamics preparation",
        "native_runtime_scale_preparation_invalid");
  };
  if (trajectory.positions.size() < 2U ||
      trajectory.times.size() != trajectory.positions.size() ||
      trajectory.velocities.size() != trajectory.positions.size() ||
      trajectory.accelerations.size() != trajectory.positions.size() ||
      model_velocity_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size()) ||
      model_acceleration_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size())) {
    return incomplete();
  }
  if (trajectory.times.front() != 0.0 ||
      !trajectory.velocities.front().isZero(0.0) ||
      !trajectory.velocities.back().isZero(0.0) ||
      !trajectory.accelerations.front().isZero(0.0) ||
      !trajectory.accelerations.back().isZero(0.0))
    return incomplete();
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
    return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Err(
        joint.error());
  }
  if (!joint.value().complete)
    return incomplete();
  auto derivatives = std::move(joint.value().joint_derivatives);
  std::vector<core::detail::TrajectoryDerivativeLimits> limits;
  limits.reserve(trajectory.joint_names.size() + 2U);
  for (std::size_t index = 0; index < trajectory.joint_names.size(); ++index) {
    double velocity =
        std::abs(model_velocity_limits[static_cast<Eigen::Index>(index)]);
    double acceleration =
        std::abs(model_acceleration_limits[static_cast<Eigen::Index>(index)]);
    std::optional<double> jerk;
    const auto found =
        contract.per_joint_dynamics_limits.find(trajectory.joint_names[index]);
    if (found != contract.per_joint_dynamics_limits.end()) {
      if (found->second.max_velocity_rad_s)
        velocity = std::min(velocity, *found->second.max_velocity_rad_s);
      if (found->second.max_acceleration_rad_s2)
        acceleration =
            std::min(acceleration, *found->second.max_acceleration_rad_s2);
      jerk = found->second.max_jerk_rad_s3;
    }
    limits.push_back({velocity * contract.speed_profile.velocity_scale,
                      acceleration * contract.speed_profile.acceleration_scale,
                      jerk});
  }
  double frame_velocity_ratio = 0.0;
  if (path || contract.speed_profile.max_tcp_linear_speed_mps) {
    auto frame = detail::measure_cartesian_frame_dynamics_envelope(
        contract, path, scene, trajectory, control);
    if (!frame.ok())
      return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Err(
          frame.error());
    if (!frame.value().complete)
      return incomplete();
    frame_velocity_ratio = std::max(frame.value().linear_speed_ratio,
                                    frame.value().angular_speed_ratio);
    if (path) {
      const double linear_velocity =
          contract.speed_profile.max_tcp_linear_speed_mps.value_or(0.0) *
          contract.speed_profile.velocity_scale;
      const double angular_velocity =
          path->max_angular_speed_rad_s * contract.speed_profile.velocity_scale;
      const double linear_acceleration =
          path->max_linear_acceleration_mps2 *
          contract.speed_profile.acceleration_scale;
      const double angular_acceleration =
          path->max_angular_acceleration_rad_s2 *
          contract.speed_profile.acceleration_scale;
      derivatives.push_back(
          {frame.value().linear_speed_ratio * linear_velocity,
           frame.value().linear_acceleration_ratio * linear_acceleration, 0.0});
      limits.push_back({linear_velocity, linear_acceleration, std::nullopt});
      derivatives.push_back(
          {frame.value().angular_speed_ratio * angular_velocity,
           frame.value().angular_acceleration_ratio * angular_acceleration,
           0.0});
      limits.push_back({angular_velocity, angular_acceleration, std::nullopt});
    }
  }
  // One implementation constant, not an extra operator setting. Reserving 20%
  // avoids spending the complete acceleration/jerk budget on nominal timing.
  // 4/s retains the prior rate ceiling; 16/s² removes its rate discontinuity.
  if (!std::isfinite(frame_velocity_ratio))
    return incomplete();
  const std::array envelopes{core::detail::TrajectoryDynamicsEnvelope{
      derivatives, limits, std::max(1.0, frame_velocity_ratio)}};
  const auto prepared =
      core::detail::prepare_runtime_scaling(envelopes, 0.20, {4.0, 16.0});
  if (!prepared)
    return incomplete();
  // Retain velocity authority from the exact clipped/scaled limits and the
  // continuous curve envelopes. R/B describe live scaling, not Stop headroom.
  double velocity_ratio = frame_velocity_ratio;
  for (std::size_t i = 0; i < derivatives.size(); ++i)
    velocity_ratio =
        std::max(velocity_ratio, derivatives[i].velocity / limits[i].velocity);
  const double multiplier = prepared->trajectory_time_multiplier;
  const double raw_stop_path_rate =
      velocity_ratio > 0.0 ? multiplier / velocity_ratio : 1.0;
  if (!std::isfinite(velocity_ratio) || !finite_positive(multiplier) ||
      !std::isfinite(raw_stop_path_rate))
    return incomplete();
  const double stop_path_rate =
      std::max(1.0, std::nextafter(raw_stop_path_rate, 0.0));
  scale_trajectory_time(trajectory, multiplier);
  if (cartesian_waypoint_times_s)
    for (auto& time : *cartesian_waypoint_times_s) time *= multiplier;
  if (!finite_positive(trajectory.times.back()))
    return incomplete();
  return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Ok(
      {prepared->scale_bounds.max_rate_per_s,
       prepared->scale_bounds.max_rate_change_per_s2, stop_path_rate, multiplier});
}

[[nodiscard]] core::Result<roboplan::JointTrajectory>
plan_cartesian_candidate(const std::shared_ptr<roboplan::Scene> &scene,
                         const RoboPlanCandidateBindingDTO &binding,
                         const RoboPlanCandidateRequestDTO &request,
                         const core::CartesianPathDTO &path,
                         const ResolvedDynamicsCaps &caps,
                         const RoboPlanCandidateComputerConfig &config,
                         const RoboPlanCandidateControl &control,
                         core::TrajectoryRuntimeScaleBoundsDTO &scale_bounds,
                         std::vector<double>& waypoint_times_s,
                         core::CartesianFidelityMetricsDTO& fidelity_metrics) {
  if (!finite_positive(request.tolerances.translation_tol_m) ||
      !finite_positive(request.tolerances.rotation_tol_rad)) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kInvalidRequest,
        "Cartesian planning requires positive translation and rotation "
        "tolerances",
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
  const Eigen::VectorXd full_start =
      scene->toFullJointPositions(binding.compiled_group_name, group_start);
  const Eigen::Matrix4d actual_start = scene->forwardKinematics(
      full_start, core::compiled_frame_name(request.tcp_frame),
      core::compiled_frame_name(request.base_frame));
  const double start_translation_error =
      (actual_start.block<3, 1>(0, 3) -
       dense.value().poses.front().block<3, 1>(0, 3))
          .norm();
  const double start_rotation_error =
      rotation_distance(actual_start.block<3, 3>(0, 0),
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
        "Cartesian planning requires a complete retained model dynamics "
        "projection",
        "cartesian_model_dynamics_limits_unavailable");
  }

  roboplan::CartesianPlannerOptions options;
  options.group_name = binding.compiled_group_name;
  options.dt = config.trajectory_sample_period_s;
  options.speed_mode = roboplan::CartesianSpeedMode::Bounded;
  if (!request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kInvalidRequest,
        "Cartesian planning requires the admitted TCP linear speed",
        "cartesian_tcp_linear_speed_missing");
  }
  options.max_linear_speed =
      *request.speed_profile.max_tcp_linear_speed_mps;
  options.max_angular_speed = path.max_angular_speed_rad_s;
  options.max_linear_acceleration = path.max_linear_acceleration_mps2;
  options.max_angular_acceleration = path.max_angular_acceleration_rad_s2;
  options.max_position_error = request.tolerances.translation_tol_m;
  options.max_orientation_error = request.tolerances.rotation_tol_rad;
  options.velocity_scale = request.speed_profile.velocity_scale;
  options.acceleration_scale = request.speed_profile.acceleration_scale;

  constexpr std::size_t kMaximumBlendAttempts = 5U;
  std::array<double, kMaximumBlendAttempts> blend_attempts{};
  blend_attempts.front() = path.max_blend_deviation_rad;
  std::size_t blend_attempt_count = 1U;
  if (path.max_blend_deviation_rad > 0.0) {
    for (std::size_t index = 1U; index + 1U < kMaximumBlendAttempts; ++index) {
      blend_attempts[index] = blend_attempts[index - 1U] * 0.5;
    }
    blend_attempts.back() = 0.0;
    blend_attempt_count = kMaximumBlendAttempts;
  }

  for (std::size_t blend_attempt = 0U;
       blend_attempt < blend_attempt_count; ++blend_attempt) {
    options.toppra_blend_deviation = blend_attempts[blend_attempt];

    if (auto error = control_error(control, "before_cartesian_tracking");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    auto planned = [&] {
      // RoboPlan's Cartesian tracker cannot parameterize a zero-length pose
      // path. Reuse its ordinary stationary joint timing instead. This is only
      // floating-point equality, not permission to omit small authored moves
      // merely because they fit within the goal tolerance.
      const bool stationary =
          std::all_of(dense.value().poses.begin(), dense.value().poses.end(),
                      [&](const auto &target) {
                        return target.isApprox(actual_start, 1e-12);
                      });
      if (stationary) {
        roboplan::PathParameterizerTOPPRA parameterizer(
            scene, binding.compiled_group_name);
        roboplan::TOPPRAOptions timing;
        timing.dt = config.trajectory_sample_period_s;
        timing.mode = roboplan::SplineFittingMode::Hermite;
        timing.velocity_scale = options.velocity_scale;
        timing.acceleration_scale = options.acceleration_scale;
        timing.max_output_samples = config.maximum_trajectory_samples;
        return parameterizer.generate(
            roboplan::JointPath{binding.compiled_joint_names,
                                {group_start, group_start}},
            timing);
      }
      roboplan::CartesianPathPlanner planner(scene, options);
      const roboplan::CartesianPath native_path(
          {core::compiled_frame_name(request.base_frame)},
          {core::compiled_frame_name(request.tcp_frame)},
          {dense.value().poses});
      roboplan::JointConfiguration native_start;
      native_start.positions = full_start;
      return planner.plan(native_path, native_start);
    }();
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
        dense.value().poses, interpolant.value().poses, certified_translation,
        certified_rotation, contract_control,
        dense.value().source_waypoint_dense_indices);
    if (!fidelity.ok()) {
      return core::Result<roboplan::JointTrajectory>::Err(fidelity.error());
    }
    if (!fidelity.value().complete) {
      if (blend_attempt + 1U < blend_attempt_count) continue;
      std::map<std::string, std::string> details{
        {"reason", "cartesian_path_tolerance_exceeded"},
        {"failure_stage", "cartesian_output_validation"},
        {"fraction_completed",
         std::to_string(fidelity.value().fraction_completed)},
        {"max_translation_error_m",
         std::to_string(fidelity.value().max_translation_error_m)},
        {"max_rotation_error_rad",
         std::to_string(fidelity.value().max_rotation_error_rad)},
        {"requested_max_blend_deviation_rad",
         std::to_string(path.max_blend_deviation_rad)},
        {"planning_attempt_count", std::to_string(blend_attempt + 1U)}};
      if (fidelity.value().first_failing_path_sample_index.has_value()) {
        details["first_failing_path_sample_index"] =
            std::to_string(*fidelity.value().first_failing_path_sample_index);
      }
      return core::Result<roboplan::JointTrajectory>::Err(
          make_error(ErrorCode::kCartesianIncomplete,
                     "Cartesian trajectory deviated from the requested path",
                     "cartesian_path_tolerance_exceeded", std::move(details)));
    }

    const auto &matches = fidelity.value().waypoint_actual_sample_indices;
    if (matches.size() != path.waypoints.size() ||
        interpolant.value().times_s.size() != interpolant.value().poses.size())
      return fail<roboplan::JointTrajectory>(
          ErrorCode::kCartesianIncomplete,
          "Cartesian source waypoint correspondence is incomplete",
          "cartesian_waypoint_correspondence_invalid");
    waypoint_times_s.clear();
    waypoint_times_s.reserve(matches.size());
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (matches[i] >= interpolant.value().poses.size())
        return fail<roboplan::JointTrajectory>(
            ErrorCode::kCartesianIncomplete,
            "Cartesian source waypoint has no reached trajectory sample",
            "cartesian_waypoint_correspondence_invalid");
      const auto &actual = interpolant.value().poses[matches[i]];
      const auto &requested =
          dense.value().poses[dense.value().source_waypoint_dense_indices[i]];
      if ((actual.block<3, 1>(0, 3) - requested.block<3, 1>(0, 3)).norm() >
              request.tolerances.translation_tol_m ||
          rotation_distance(actual.block<3, 3>(0, 0),
                            requested.block<3, 3>(0, 0)) >
              request.tolerances.rotation_tol_rad)
        return fail<roboplan::JointTrajectory>(
            ErrorCode::kCartesianIncomplete,
            "Cartesian waypoint milestone is outside its pose tolerance",
            "cartesian_waypoint_correspondence_invalid");
      waypoint_times_s.push_back(interpolant.value().times_s[matches[i]]);
    }
    auto prepared = prepare_candidate_time_law(
        contract, &path, scene, trajectory, model_velocity_limits->second,
        model_acceleration_limits->second, contract_control, &waypoint_times_s);
    if (!prepared.ok())
      return core::Result<roboplan::JointTrajectory>::Err(prepared.error());
    scale_bounds = prepared.value();
    fidelity_metrics = core::CartesianFidelityMetricsDTO{
        request.tolerances.translation_tol_m,
        request.tolerances.rotation_tol_rad,
        fidelity.value().max_translation_error_m,
        fidelity.value().max_rotation_error_rad,
        interpolant.value().unsampled_translation_bound_m,
        interpolant.value().unsampled_rotation_bound_rad,
        fidelity.value().max_translation_error_m +
            interpolant.value().unsampled_translation_bound_m,
        fidelity.value().max_rotation_error_rad +
            interpolant.value().unsampled_rotation_bound_rad,
        path.max_blend_deviation_rad,
        options.toppra_blend_deviation,
        static_cast<std::uint32_t>(blend_attempt + 1U)};
    return core::Result<roboplan::JointTrajectory>::Ok(std::move(trajectory));
  }
  return fail<roboplan::JointTrajectory>(
      ErrorCode::kCartesianIncomplete,
      "Cartesian blend search exhausted without a candidate",
      "cartesian_blend_search_exhausted");
}

[[nodiscard]] core::Result<roboplan::JointTrajectory> time_joint_candidate(
    const std::shared_ptr<roboplan::Scene> &scene,
    const RoboPlanCandidateBindingDTO &binding,
    const RoboPlanCandidateRequestDTO &request,
    const RoboPlanCandidateComputerConfig &config,
    const RoboPlanCandidateControl &control, const roboplan::JointPath &path,
    const Eigen::VectorXd &start, const Eigen::VectorXd &goal) {
  if (auto error = control_error(control, "joint_time_parameterization");
      error.has_value())
    return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));

  roboplan::PathParameterizerTOPPRA parameterizer(scene,
                                                  binding.compiled_group_name);
  roboplan::TOPPRAOptions options;
  options.dt = config.trajectory_sample_period_s;
  options.mode = roboplan::SplineFittingMode::Hermite;
  options.velocity_scale = request.speed_profile.velocity_scale;
  options.acceleration_scale = request.speed_profile.acceleration_scale;
  options.max_output_samples = config.maximum_trajectory_samples;
  auto generated = parameterizer.generate(path, options);
  if (auto error = control_error(control, "joint_time_parameterization");
      error.has_value())
    return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
  if (!generated.has_value())
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kPlanningFailed,
        "RoboPlan TOPP-RA could not time-parameterize the candidate",
        "native_time_parameterization_failed",
        {{"planner_error", generated.error()}});

  auto trajectory = std::move(generated.value());
  anchor_trajectory_endpoints(trajectory, start, goal);
  // The caller certifies the final interpolant and applies one common time
  // dilation for joint/TCP caps, optional jerk and live-scaling headroom.
  // Re-running TOPPRA against sampled peaks would duplicate that stronger gate.
  return core::Result<roboplan::JointTrajectory>::Ok(std::move(trajectory));
}

[[nodiscard]] core::Result<std::vector<double>> solve_ik_locked(
    const std::shared_ptr<roboplan::Scene> &scene,
    const RoboPlanCandidateBindingDTO &binding, const core::Pose3DTO &goal,
    const core::ResolvedFrameDTO &base_frame,
    const core::ResolvedFrameDTO &tcp_frame,
    const core::ControlledStateDTO &seed_state, double timeout_s,
    const RoboPlanCandidateControl &control, core::PlanningProfileDTO profile,
    const core::TolerancesDTO *tolerances = nullptr) {
  if (!valid_pose(goal) || !finite_positive(timeout_s) ||
      !contains_frame(binding.permitted_frames, base_frame) ||
      !contains_frame(binding.permitted_frames, tcp_frame)) {
    return fail<std::vector<double>>(ErrorCode::kInvalidRequest,
                                     "RoboPlan IK input is malformed",
                                     "native_ik_request_invalid");
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
      options.max_linear_error_norm = std::min(options.max_linear_error_norm,
                                               tolerances->translation_tol_m);
    }
    if (finite_positive(tolerances->rotation_tol_rad)) {
      options.max_angular_error_norm = std::min(options.max_angular_error_norm,
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
    return fail<std::vector<double>>(ErrorCode::kIkFailed,
                                     "RoboPlan could not resolve the pose goal",
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

} // namespace

RoboPlanCandidateComputer::RoboPlanCandidateComputer(
    std::shared_ptr<roboplan::Scene> scene, RoboPlanCandidateBindingDTO binding,
    RoboPlanCandidateComputerConfig config)
    : scene_(std::move(scene)), binding_(std::move(binding)),
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
    const core::Pose3DTO &goal, const core::ResolvedFrameDTO &base_frame,
    const core::ResolvedFrameDTO &tcp_frame,
    const core::ControlledStateDTO &seed_state, double timeout_s,
    const RoboPlanCandidateControl &control) {
  std::lock_guard lock(computation_mutex_);
  try {
    if (config_.rng_seed.has_value()) {
      scene_->setRngSeed(*config_.rng_seed);
    }
    return solve_ik_locked(scene_, binding_, goal, base_frame, tcp_frame,
                           seed_state, timeout_s, control,
                           core::PlanningProfileDTO::kFirstFeasible);
  } catch (const std::exception &error) {
    return fail<std::vector<double>>(ErrorCode::kPlanningFailed,
                                     "RoboPlan IK adapter failed: " +
                                         std::string(error.what()),
                                     "native_ik_adapter_failure");
  }
}

core::Result<core::TrajectoryDTO>
RoboPlanCandidateComputer::compute(const RoboPlanCandidateRequestDTO &request,
                                   const RoboPlanCandidateControl &control) {
  const auto request_validation = validate_request_binding(binding_, request);
  if (!request_validation.ok()) {
    return core::Result<core::TrajectoryDTO>::Err(request_validation.error());
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
    const auto &planning_scene = scene_;
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
      return core::Result<core::TrajectoryDTO>::Err(start_positions.error());
    }
    const Eigen::VectorXd native_start_positions =
        eigen_vector(start_positions.value());
    const roboplan::JointConfiguration native_start{
        binding_.compiled_joint_names, native_start_positions, {}, {}};
    const Eigen::VectorXd full_start = planning_scene->toFullJointPositions(
        binding_.compiled_group_name, native_start_positions);
    if (planning_scene->hasCollisions(full_start)) {
      auto details = collision_failure_details(*planning_scene, full_start,
                                               "start_state", "state_source",
                                               "observed");
      auto message = collision_failure_message(
          "Planning start state is in collision", details);
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed, std::move(message),
          "native_start_state_collision", std::move(details));
    }
    if (auto error = control_error(control, "start_collision_check");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }

    if (const auto *cartesian =
            std::get_if<core::CartesianPathDTO>(&request.motion)) {
      core::TrajectoryRuntimeScaleBoundsDTO scale_bounds;
      std::vector<double> waypoint_times_s;
      core::CartesianFidelityMetricsDTO fidelity_metrics;
      auto trajectory = plan_cartesian_candidate(
          planning_scene, binding_, request, *cartesian, caps.value(), config_,
          control, scale_bounds, waypoint_times_s, fidelity_metrics);
      if (!trajectory.ok()) {
        return core::Result<core::TrajectoryDTO>::Err(trajectory.error());
      }
      auto portable =
          portable_trajectory(trajectory.value(), binding_,
                              config_.maximum_trajectory_samples, scale_bounds);
      if (portable.ok())
        portable.value().cartesian_waypoint_times_s =
            std::move(waypoint_times_s);
      if (portable.ok())
        portable.value().cartesian_fidelity_metrics = fidelity_metrics;
      return portable;
    }

    roboplan::JointConfiguration native_goal;
    native_goal.joint_names = binding_.compiled_joint_names;
    bool resolved_from_pose = false;
    if (const auto *joint =
            std::get_if<core::ResolvedJointGoalDTO>(&request.motion)) {
      if (joint->resources.empty() ||
          joint->resources.size() != joint->joint_names.size() ||
          joint->resources.size() != joint->positions_rad.size() ||
          joint->resources.size() > binding_.resources.size()) {
        return fail<core::TrajectoryDTO>(
            ErrorCode::kInvalidRequest,
            "Resolved joint goal differs from the exact participant binding",
            "native_joint_goal_binding_mismatch");
      }
      native_goal.positions = native_start.positions;
      std::set<std::size_t> selected_indices;
      for (std::size_t index = 0U; index < joint->resources.size(); ++index) {
        const auto selected =
            std::find(binding_.resources.begin(), binding_.resources.end(),
                      joint->resources[index]);
        if (selected == binding_.resources.end() ||
            !finite(joint->positions_rad[index])) {
          return fail<core::TrajectoryDTO>(
              ErrorCode::kInvalidRequest,
              "Resolved joint goal differs from the exact participant binding",
              "native_joint_goal_binding_mismatch");
        }
        const auto selected_index = static_cast<std::size_t>(
            std::distance(binding_.resources.begin(), selected));
        if (binding_.compiled_joint_names[selected_index] !=
                joint->joint_names[index] ||
            !selected_indices.insert(selected_index).second) {
          return fail<core::TrajectoryDTO>(
              ErrorCode::kInvalidRequest,
              "Resolved joint goal differs from the exact participant binding",
              "native_joint_goal_binding_mismatch");
        }
        native_goal.positions[static_cast<Eigen::Index>(selected_index)] =
            joint->positions_rad[index];
      }
    } else if (const auto *pose =
                   std::get_if<core::PoseGoalDTO>(&request.motion)) {
      auto solution = solve_ik_locked(
          planning_scene, binding_, pose->goal, request.base_frame,
          request.tcp_frame, request.start_state, request.planning_timeout_s,
          control, request.planning_profile, &request.tolerances);
      if (!solution.ok()) {
        return core::Result<core::TrajectoryDTO>::Err(solution.error());
      }
      native_goal.positions = eigen_vector(solution.value());
      normalize_ik_goal_for_limits(planning_scene, binding_.compiled_group_name,
                                   native_start.positions,
                                   native_goal.positions);
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
      auto details = collision_failure_details(*planning_scene, full_goal,
                                               "goal_state", "goal_source",
                                               resolved_from_pose ? "pose" : "joint");
      auto message = collision_failure_message(
          "Planning goal state is in collision", details);
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed, std::move(message),
          "native_goal_state_collision", std::move(details));
    }
    if (auto error = control_error(control, "goal_collision_check");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }

    auto rrt_options = planning_profile_rrt_options(
        production_rrt_options(binding_.compiled_group_name,
                               request.planning_timeout_s,
                               request.planning_attempt_index),
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
          "native_rrt_failed", {{"planner_error", path.error()}});
    }

    auto candidate_path = std::move(path.value());
    if (shortcut_candidate_for_attempt(request.planning_attempt_index)) {
      const auto shortcut_options = production_shortcutting_options(
          binding_.compiled_group_name, request_seed,
          request.planning_attempt_index);
      roboplan::PathShortcutter shortcutter(planning_scene, shortcut_options);
      candidate_path = shortcutter.shortcut(candidate_path);
      if (auto error = control_error(control, "shortcut"); error.has_value()) {
        return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
      }
    }
    auto trajectory = time_joint_candidate(
        planning_scene, binding_, request, config_, control,
        candidate_path, native_start.positions, native_goal.positions);
    if (!trajectory.ok()) {
      return core::Result<core::TrajectoryDTO>::Err(trajectory.error());
    }
    if (auto error = control_error(control, "trajectory_conversion");
        error.has_value()) {
      return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
    }
    const auto model_velocity =
        planning_scene->getVelocityLimitVectors(binding_.compiled_group_name);
    const auto model_acceleration = planning_scene->getAccelerationLimitVectors(
        binding_.compiled_group_name);
    if (!model_velocity || !model_acceleration) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kUnsupportedCapability,
          "Runtime scaling requires retained model dynamics limits",
          "native_model_dynamics_limits_unavailable");
    }
    auto prepared = prepare_candidate_time_law(
        cartesian_contract_input(binding_, request, caps.value()), nullptr,
        planning_scene, trajectory.value(), model_velocity->second,
        model_acceleration->second, cartesian_control(control));
    if (!prepared.ok())
      return core::Result<core::TrajectoryDTO>::Err(prepared.error());
    return portable_trajectory(trajectory.value(), binding_,
                               config_.maximum_trajectory_samples,
                               prepared.value());
  } catch (const std::exception &error) {
    return fail<core::TrajectoryDTO>(ErrorCode::kPlanningFailed,
                                     "RoboPlan candidate adapter failed: " +
                                         std::string(error.what()),
                                     "native_candidate_adapter_failure");
  }
}

} // namespace manyforge::roboplan_backend
