#include <manyforge_roboplan/roboplan_candidate_computer.hpp>

#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/trajectory_splice.hpp>
#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_path_densification.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>
#include <manyforge_roboplan/roboplan_planner_options.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
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

constexpr double kNominalScalingHeadroom = 0.20;

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

[[nodiscard]] bool valid_scale_evidence(const core::TrajectoryRuntimeScaleBoundsDTO &bounds) {
  return finite_positive(bounds.max_rate_per_s) &&
         finite_positive(bounds.max_rate_change_per_s2) &&
         finite(bounds.max_stop_path_rate) &&
         bounds.max_stop_path_rate >= 1.0 &&
         (!bounds.nominal_time_multiplier ||
          (finite(*bounds.nominal_time_multiplier) &&
           *bounds.nominal_time_multiplier >= 1.0));
}

[[nodiscard]] bool equivalent_span(double original, double delivered) {
  return finite_positive(original) && finite_positive(delivered) &&
         std::abs(original - delivered) <=
             std::min(1e-9, std::sqrt(std::numeric_limits<double>::epsilon()) *
                                original);
}

[[nodiscard]] std::map<std::string, std::string>
collision_failure_details(const roboplan::Scene &scene,
                          const Eigen::VectorXd &positions,
                          std::string failure_stage, std::string source_field,
                          std::string source_value) {
  const bool start_state = failure_stage == "start_state";
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
  // Pairs the request's contact policy deactivated keep stale results.
  for (std::size_t index = 0U; !start_state && index < pair_count; ++index) {
    if (!data.activeCollisionPairs[index] ||
        !data.collisionResults[index].isCollision()) {
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
  // A real contact first; otherwise a pair closer than its collision
  // clearance (the scene's per-pair security margin), reported as
  // `classification: clearance`.
  for (const bool clearance : {false, true}) {
    for (std::size_t index = 0U; index < distance_count; ++index) {
      if (!data.activeCollisionPairs[index]) continue;
      const auto distance = data.distanceResults[index].min_distance;
      const double margin = index < data.collisionRequests.size()
                                ? data.collisionRequests[index].security_margin
                                : 0.0;
      if (!std::isfinite(distance) ||
          (clearance ? !(distance > 0.0 && distance < margin) : distance > 0.0)) {
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
      if (clearance) {
        details.emplace("classification", "clearance");
        details.emplace("collision_clearance_m", std::to_string(margin));
      }
      return details;
    }
  }
  return details;
}

[[nodiscard]] std::string collision_failure_message(
    std::string message, const std::map<std::string, std::string> &details) {
  if (const auto classification = details.find("classification");
      classification != details.end() && classification->second == "clearance") {
    message += " (closer than the collision clearance)";
  }
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

void log_moving_phase(const RoboPlanCandidateRequestDTO &input,
                      const char *phase,
                      std::chrono::steady_clock::time_point started) noexcept {
  if (!input.moving_replacement)
    return;
  const auto &moving = *input.moving_replacement;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  const auto remaining = [&](double seconds) {
    return static_cast<long long>(seconds * 1e9) -
           static_cast<long long>(elapsed);
  };
  char line[768];
  const int size = std::snprintf(
      line, sizeof(line),
      "manyforge_moving_candidate_phase pid=%ld source_goal=%.256s "
      "generation=%llu phase=%s elapsed_ns=%lld suffix_remaining_ns=%lld "
      "preparation_remaining_ns=%lld\n",
      static_cast<long>(::getpid()),
      moving.source_goal.controller_goal_id.value().c_str(),
      static_cast<unsigned long long>(
          moving.source_goal.controller_goal_generation.value()),
      phase, static_cast<long long>(elapsed),
      moving.remaining_planning_budget_s
          ? remaining(*moving.remaining_planning_budget_s)
          : -1LL,
      remaining(moving.remaining_preparation_budget_s));
  if (size > 0) {
    // Best effort, one bounded write; diagnostics never retry or alter outcome.
    const auto written = ::write(STDERR_FILENO, line,
        std::min(static_cast<std::size_t>(size), sizeof(line) - 1U));
    static_cast<void>(written);
  }
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
      !finite_positive(config.trajectory_sample_period_s) ||
      !core::valid_planner_settings(config.planner_defaults())) {
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
      !core::valid_planner_override(request.planner)) {
    return core::Result<void>::Err(make_error(
        ErrorCode::kInvalidRequest,
        "RoboPlan request has invalid tolerances, limits, timeout, or planner",
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

} // namespace

namespace detail {
namespace {

// Rotation vector (world frame) taking `from` to `to` along the shorter way.
[[nodiscard]] Eigen::Vector3d rotation_vector(const Eigen::Quaterniond &from,
                                              const Eigen::Quaterniond &to) {
  Eigen::Quaterniond relative = to * from.conjugate();
  if (relative.w() < 0.0) {
    relative.coeffs() *= -1.0;
  }
  const Eigen::AngleAxisd angle_axis(relative.normalized());
  return angle_axis.angle() * angle_axis.axis();
}

[[nodiscard]] Eigen::Quaterniond rotated(const Eigen::Vector3d &rotation,
                                         const Eigen::Quaterniond &base) {
  const double angle = rotation.norm();
  if (angle == 0.0) {
    return base;
  }
  return (Eigen::Quaterniond(Eigen::AngleAxisd(angle, rotation / angle)) * base)
      .normalized();
}

[[nodiscard]] Eigen::Matrix4d pose_of(const Eigen::Vector3d &position,
                                      const Eigen::Quaterniond &orientation) {
  Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
  pose.block<3, 3>(0, 0) = orientation.toRotationMatrix();
  pose.block<3, 1>(0, 3) = position;
  return pose;
}

// One rounded corner: the arc tangent to both segments at `distance` from the
// waypoint. `rotation_in`/`rotation_out` are the adjacent segments' rotation
// per metre (world rotation vectors).
struct CornerArc final {
  double distance{};
  double turn{};
  double radius{};
  Eigen::Vector3d center;
  Eigen::Vector3d inward;
  Eigen::Vector3d direction_in;
  Eigen::Vector3d rotation_in;
  Eigen::Vector3d rotation_out;
  Eigen::Quaterniond orientation;
  std::size_t steps{};

  [[nodiscard]] Eigen::Matrix4d at(double fraction) const {
    const double angle = fraction * turn;
    const Eigen::Vector3d position =
        center + radius * (-inward * std::cos(angle) +
                           direction_in * std::sin(angle));
    // Cubic Hermite in rotation-vector space about the waypoint orientation:
    // ends match the segments' orientation, slopes their rotation per metre.
    const double length = radius * turn;
    const double u = fraction, u2 = u * u, u3 = u2 * u;
    const Eigen::Vector3d rotation =
        (2.0 * u3 - 3.0 * u2 + 1.0) * (-distance * rotation_in) +
        (u3 - 2.0 * u2 + u) * (length * rotation_in) +
        (-2.0 * u3 + 3.0 * u2) * (distance * rotation_out) +
        (u3 - u2) * (length * rotation_out);
    return pose_of(position, rotated(rotation, orientation));
  }
};

} // namespace

core::Result<DensifiedCartesianPath>
densify_cartesian_path(const core::CartesianPathDTO &path,
                       const RoboPlanCandidateControl &control) {
  constexpr std::size_t kMaximumInputWaypoints = 4096U;
  constexpr std::size_t kMaximumDensifiedWaypoints = 100'000U;
  constexpr std::size_t kControlStride = 64U;
  // Below this a waypoint is collinear (no corner); above pi minus it the
  // path reverses and no arc can round it.
  constexpr double kMinimumTurnRad = 1e-6;
  constexpr double kMinimumLengthM = 1e-9;
  const auto invalid_radius = [](double radius) {
    return !std::isfinite(radius) || radius < 0.0;
  };
  if (path.waypoints.size() < 2U ||
      path.waypoints.size() > kMaximumInputWaypoints ||
      !finite_positive(path.max_translation_step_m) ||
      !finite_positive(path.max_rotation_step_rad) ||
      path.fallback_policy != core::CartesianFallbackPolicyDTO::kFailClosed ||
      !finite_positive(path.max_angular_speed_rad_s) ||
      !finite_positive(path.max_linear_acceleration_mps2) ||
      !finite_positive(path.max_angular_acceleration_rad_s2) ||
      (!path.blend_radii_m.empty() &&
       path.blend_radii_m.size() != path.waypoints.size()) ||
      std::any_of(path.blend_radii_m.begin(), path.blend_radii_m.end(),
                  invalid_radius)) {
    return fail<DensifiedCartesianPath>(
        ErrorCode::kInvalidRequest,
        "Cartesian request shape, policy, steps, limits or blend radii are "
        "invalid",
        "cartesian_request_invalid");
  }

  const std::size_t count = path.waypoints.size();
  std::vector<Eigen::Vector3d> positions;
  std::vector<Eigen::Quaterniond> orientations;
  positions.reserve(count);
  orientations.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
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
    const Eigen::Matrix4d pose = native_pose(path.waypoints[index]);
    positions.push_back(pose.block<3, 1>(0, 3));
    orientations.emplace_back(Eigen::Matrix3d(pose.block<3, 3>(0, 0)));
  }

  // Segment k runs from waypoint k to k + 1.
  std::vector<double> lengths(count - 1U);
  std::vector<Eigen::Vector3d> rotations(count - 1U);
  for (std::size_t k = 0U; k + 1U < count; ++k) {
    lengths[k] = (positions[k + 1U] - positions[k]).norm();
    rotations[k] = rotation_vector(orientations[k], orientations[k + 1U]);
  }

  std::vector<std::optional<CornerArc>> corners(count);
  for (std::size_t i = 1U; i + 1U < count; ++i) {
    const double requested = path.blend_radii_m.empty() ? 0.0 : path.blend_radii_m[i];
    const double length_in = lengths[i - 1U];
    const double length_out = lengths[i];
    if (requested <= 0.0 || length_in <= kMinimumLengthM ||
        length_out <= kMinimumLengthM) {
      continue;
    }
    const Eigen::Vector3d direction_in =
        (positions[i] - positions[i - 1U]) / length_in;
    const Eigen::Vector3d direction_out =
        (positions[i + 1U] - positions[i]) / length_out;
    const double turn =
        std::acos(std::clamp(direction_in.dot(direction_out), -1.0, 1.0));
    if (turn <= kMinimumTurnRad || turn >= std::numbers::pi - kMinimumTurnRad) {
      continue;
    }
    CornerArc arc;
    arc.distance = std::min({requested, 0.5 * length_in, 0.5 * length_out});
    arc.turn = turn;
    arc.radius = arc.distance / std::tan(0.5 * turn);
    arc.direction_in = direction_in;
    arc.inward = (direction_out - direction_out.dot(direction_in) * direction_in)
                     .normalized();
    arc.center =
        positions[i] - arc.distance * direction_in + arc.radius * arc.inward;
    arc.rotation_in = rotations[i - 1U] / length_in;
    arc.rotation_out = rotations[i] / length_out;
    arc.orientation = orientations[i];
    const double arc_length = arc.radius * turn;
    const double arc_rotation =
        arc.distance * (arc.rotation_in.norm() + arc.rotation_out.norm());
    const double steps = std::max(
        {2.0, std::ceil(turn / kCornerArcMaxTurnRad),
         std::ceil(arc_length / path.max_translation_step_m),
         std::ceil(arc_rotation / path.max_rotation_step_rad)});
    // An even count puts the arc middle on a sample (the waypoint's stand-in).
    arc.steps = static_cast<std::size_t>(steps) + (static_cast<std::size_t>(steps) % 2U);
    corners[i] = arc;
  }

  DensifiedCartesianPath result;
  for (const auto &corner : corners) {
    if (corner) {
      result.smallest_corner_radius_m =
          std::min(result.smallest_corner_radius_m, corner->radius);
    }
  }
  result.poses.reserve(count);
  result.source_waypoint_dense_indices.reserve(count);
  result.poses.push_back(pose_of(positions.front(), orientations.front()));
  result.source_waypoint_dense_indices.push_back(0U);
  const auto budget_exceeded = [&](std::size_t segment, double required) {
    const std::size_t remaining =
        kMaximumDensifiedWaypoints - result.poses.size();
    if (finite(required) && required <= static_cast<double>(remaining)) {
      return std::optional<core::ErrorDTO>{};
    }
    return std::optional<core::ErrorDTO>{make_error(
        ErrorCode::kInvalidRequest,
        "Cartesian path exceeds its bounded densification budget",
        "cartesian_path_too_dense",
        {{"segment_index", std::to_string(segment)},
         {"maximum_waypoints", std::to_string(kMaximumDensifiedWaypoints)}})};
  };
  for (std::size_t segment = 0U; segment + 1U < count; ++segment) {
    if (auto error = control_error(control, "cartesian_path_densification");
        error.has_value()) {
      return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
    }
    // The straight part between this segment's corner arcs, as fractions.
    const double length = lengths[segment];
    const double first =
        corners[segment] ? corners[segment]->distance / length : 0.0;
    const double last =
        corners[segment + 1U] ? 1.0 - corners[segment + 1U]->distance / length
                              : 1.0;
    Eigen::Quaterniond to_orientation = orientations[segment + 1U];
    if (orientations[segment].dot(to_orientation) < 0.0) {
      to_orientation.coeffs() *= -1.0;
    }
    const double span = last - first;
    if (span * length > kMinimumLengthM || !corners[segment + 1U]) {
      const double required = std::max(
          1.0, std::ceil(std::max(span * length / path.max_translation_step_m,
                                  span * rotations[segment].norm() /
                                      path.max_rotation_step_rad)));
      if (auto error = budget_exceeded(segment, required); error.has_value()) {
        return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
      }
      const auto subdivisions = static_cast<std::size_t>(required);
      for (std::size_t step = 1U; step <= subdivisions; ++step) {
        if (step % kControlStride == 0U) {
          if (auto error =
                  control_error(control, "cartesian_path_densification");
              error.has_value()) {
            return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
          }
        }
        const double alpha =
            first + span * static_cast<double>(step) /
                        static_cast<double>(subdivisions);
        result.poses.push_back(pose_of(
            (1.0 - alpha) * positions[segment] + alpha * positions[segment + 1U],
            orientations[segment].slerp(alpha, to_orientation).normalized()));
      }
    }
    const auto &arc = corners[segment + 1U];
    if (!arc) {
      result.source_waypoint_dense_indices.push_back(result.poses.size() - 1U);
      continue;
    }
    if (auto error = budget_exceeded(segment, static_cast<double>(arc->steps));
        error.has_value()) {
      return core::Result<DensifiedCartesianPath>::Err(std::move(*error));
    }
    for (std::size_t step = 1U; step <= arc->steps; ++step) {
      result.poses.push_back(arc->at(static_cast<double>(step) /
                                     static_cast<double>(arc->steps)));
      if (2U * step == arc->steps) {
        result.source_waypoint_dense_indices.push_back(result.poses.size() - 1U);
      }
    }
  }
  return core::Result<DensifiedCartesianPath>::Ok(std::move(result));
}

CartesianResolutionTolerances
cartesian_resolution_tolerances(const DensifiedCartesianPath &dense,
                                double translation_tol_m,
                                double rotation_tol_rad) {
  // RoboPlan 0.8's resolution step is this share of its position and
  // orientation tolerances (`kSeedStepFraction`).
  constexpr double kRoboPlanResolutionStepFraction = 0.5;
  const double scale = std::min(
      1.0, kCornerArcMaxTurnRad * dense.smallest_corner_radius_m /
               (kRoboPlanResolutionStepFraction * translation_tol_m));
  return {scale * translation_tol_m, scale * rotation_tol_rad};
}

} // namespace detail

namespace {

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

[[nodiscard]] std::vector<core::detail::TrajectoryDerivativeLimits>
resolved_runtime_joint_limits(
    const detail::CartesianTrajectoryContractInput &contract,
    const std::vector<std::string> &joint_names,
    const Eigen::VectorXd &model_velocity_limits,
    const Eigen::VectorXd &model_acceleration_limits) {
  std::vector<core::detail::TrajectoryDerivativeLimits> limits;
  limits.reserve(joint_names.size() + 2U);
  for (std::size_t index = 0; index < joint_names.size(); ++index) {
    double velocity =
        std::abs(model_velocity_limits[static_cast<Eigen::Index>(index)]);
    double acceleration =
        std::abs(model_acceleration_limits[static_cast<Eigen::Index>(index)]);
    std::optional<double> jerk;
    const auto found =
        contract.per_joint_dynamics_limits.find(joint_names[index]);
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
  return limits;
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
    std::vector<double>* cartesian_waypoint_times_s = nullptr,
    bool retain_moving_evidence = false) {
  const auto incomplete = [] {
    return fail<core::TrajectoryRuntimeScaleBoundsDTO>(
        ErrorCode::kValidationLimitsViolation,
        "Trajectory lacks a complete runtime dynamics preparation",
        "execution_host_scale_preparation_invalid");
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
  auto limits = resolved_runtime_joint_limits(contract, trajectory.joint_names,
                                              model_velocity_limits,
                                              model_acceleration_limits);
  double frame_velocity_ratio = 0.0;
  double tcp_time_multiplier = 1.0;
  if (path &&
      path->speed_mode != core::CartesianSpeedModeDTO::kTimeOptimal) {
    auto frame = detail::measure_cartesian_frame_dynamics_envelope(
        contract, *path, scene, trajectory, control);
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
    tcp_time_multiplier = std::max(1.0, frame_velocity_ratio);
  }
  if (contract.speed_profile.max_tcp_linear_speed_mps) {
    // Retain the original Cartesian sample/envelope time law until the
    // independent validator's chord threshold would actually be exceeded.
    if (!path || path->speed_mode == core::CartesianSpeedModeDTO::kTimeOptimal) {
      const auto sample = detail::measure_tcp_sample_speed_ratio(
          contract, path, scene, trajectory);
      if (!sample) return incomplete();
      frame_velocity_ratio = *sample;
      if (!path || *sample > 1.0)
        tcp_time_multiplier = std::max(
            tcp_time_multiplier, *sample / detail::kTcpSpeedTargetFraction);
    }
    if (!path) {
      const auto chord = detail::measure_tcp_chord_speed_ratio(
          contract, scene, trajectory, control);
      if (!chord.ok())
        return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Err(chord.error());
      frame_velocity_ratio = std::max(frame_velocity_ratio, chord.value());
      tcp_time_multiplier = std::max(
          tcp_time_multiplier, chord.value() / detail::kTcpSpeedTargetFraction);
    }
  }
  // One implementation constant, not an extra operator setting. Reserving 20%
  // avoids spending the complete acceleration/jerk budget on nominal timing.
  // 4/s retains the prior rate ceiling; 16/s² removes its rate discontinuity.
  if (!std::isfinite(frame_velocity_ratio))
    return incomplete();
  const auto prepare = [&](double minimum_multiplier) {
    const std::array envelopes{core::detail::TrajectoryDynamicsEnvelope{
        derivatives, limits, minimum_multiplier}};
    return core::detail::prepare_runtime_scaling(
        envelopes, kNominalScalingHeadroom, {4.0, 16.0});
  };
  auto prepared = prepare(tcp_time_multiplier);
  if (!prepared)
    return incomplete();
  if (path && contract.speed_profile.max_tcp_linear_speed_mps) {
    const auto baseline_speed = detail::measure_tcp_chord_speed_mps(
        contract, scene, trajectory, control, prepared->trajectory_time_multiplier);
    if (!baseline_speed.ok())
      return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Err(baseline_speed.error());
    // Match final validation's unscaled limit and tolerance exactly.
    constexpr double kValidatorLimitTolerance = 1e-8;
    if (baseline_speed.value() > *contract.speed_profile.max_tcp_linear_speed_mps +
                                     kValidatorLimitTolerance) {
      const auto chord = detail::measure_tcp_chord_speed_ratio(
          contract, scene, trajectory, control);
      if (!chord.ok())
        return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Err(chord.error());
      frame_velocity_ratio = std::max(frame_velocity_ratio, chord.value());
      tcp_time_multiplier = std::max(
          tcp_time_multiplier, chord.value() / detail::kTcpSpeedTargetFraction);
      prepared = prepare(tcp_time_multiplier);
      if (!prepared) return incomplete();
    }
  }
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
  const auto original_times =
      retain_moving_evidence ? trajectory.times : std::vector<double>{};
  scale_trajectory_time(trajectory, multiplier);
  if (retain_moving_evidence)
    for (std::size_t index = 1U; index < original_times.size(); ++index)
      if (!equivalent_span((original_times[index] - original_times[index - 1U]) * multiplier,
                           trajectory.times[index] - trajectory.times[index - 1U]))
        return incomplete();
  if (cartesian_waypoint_times_s)
    for (auto& time : *cartesian_waypoint_times_s) time *= multiplier;
  if (!finite_positive(trajectory.times.back()))
    return incomplete();
  return core::Result<core::TrajectoryRuntimeScaleBoundsDTO>::Ok(
      {prepared->scale_bounds.max_rate_per_s,
       prepared->scale_bounds.max_rate_change_per_s2, stop_path_rate, multiplier});
}

// Read-only measurement of one exact piece. The source and changed future use
// their own resolved constraints; neither curve is normalized or dilated.
struct FixedPieceMeasurement final {
  std::vector<core::detail::TrajectoryDerivativeBounds> derivatives;
  std::vector<core::detail::TrajectoryDerivativeLimits> limits;
  double velocity_ratio{};
  double correction_ratio{1.0};
  double duration_ratio{1.0};
  bool chord_correction{};
};

// Diagnostic-only worker-local counters. No curve, qualification or deadline
// is reused or changed. A hard cap bounds stderr work even for long searches.
struct MovingFinalizationMeasurements final {
  const RoboPlanCandidateRequestDTO &input;
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  std::array<long long, 3> source_ns{}, future_ns{};
  long long splice_ns{};
  std::size_t entries{}, attempts{}, source_points{}, future_points{},
      future_points_total{};
  std::size_t records{};
  double entry_s{}, duration_s{}, ratio{};
  double endpoint_lower_bound_s{}, future_v_ratio{}, future_a_ratio{},
      future_j_ratio{}, future_tcp_ratio{}, next_duration_ratio{};
  bool source{true};
  bool trace_stages{true};

  void Record(const char *stage) noexcept {
    if (records++ >= 24U)
      return;
    char line[2048];
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const auto &goal = input.moving_replacement->source_goal;
    const int size = std::snprintf(
        line, sizeof(line),
        "manyforge_moving_connector_measure pid=%ld source_goal=%.256s "
        "generation=%llu stage=%s elapsed_ns=%lld entries=%zu attempts=%zu "
        "entry_s=%.17g duration_s=%.17g ratio=%.17g source_points=%zu "
        "future_points=%zu future_points_total=%zu splice_ns=%lld "
        "source_joint_ns=%lld "
        "source_tcp_ns=%lld source_position_ns=%lld future_joint_ns=%lld "
        "future_tcp_ns=%lld future_position_ns=%lld "
        "endpoint_lower_bound_s=%.17g "
        "future_v_ratio=%.17g future_a_ratio=%.17g future_j_ratio=%.17g "
        "future_tcp_ratio=%.17g next_duration_ratio=%.17g\n",
        static_cast<long>(::getpid()), goal.controller_goal_id.value().c_str(),
        static_cast<unsigned long long>(
            goal.controller_goal_generation.value()),
        stage, static_cast<long long>(elapsed), entries, attempts, entry_s,
        duration_s, ratio, source_points, future_points, future_points_total,
        splice_ns, source_ns[0], source_ns[1], source_ns[2], future_ns[0],
        future_ns[1], future_ns[2], endpoint_lower_bound_s, future_v_ratio,
        future_a_ratio, future_j_ratio, future_tcp_ratio, next_duration_ratio);
    if (size > 0) {
      const auto written =
          ::write(STDERR_FILENO, line,
                  std::min(static_cast<std::size_t>(size), sizeof(line) - 1U));
      static_cast<void>(written);
    }
  }
  ~MovingFinalizationMeasurements() { Record("finalizer_return"); }
};

struct MovingEnvelopeMeasurement final {
  MovingFinalizationMeasurements &measurements;
  std::size_t metric;
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
  MovingEnvelopeMeasurement(MovingFinalizationMeasurements &value,
                            std::size_t index, const char *stage)
      : measurements(value), metric(index) {
    if (measurements.trace_stages)
      measurements.Record(stage);
    started = std::chrono::steady_clock::now();
  }
  ~MovingEnvelopeMeasurement() {
    auto &totals =
        measurements.source ? measurements.source_ns : measurements.future_ns;
    totals[metric] += std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  }
};

[[nodiscard]] roboplan::JointTrajectory
native_trajectory(const core::TrajectoryDTO &trajectory) {
  roboplan::JointTrajectory native;
  native.joint_names = trajectory.joint_names;
  for (const auto &point : trajectory.waypoints) {
    native.times.push_back(point.time_from_start_s);
    native.positions.push_back(eigen_vector(point.positions_rad));
    native.velocities.push_back(eigen_vector(point.velocities_rad_s));
    native.accelerations.push_back(eigen_vector(point.accelerations_rad_s2));
  }
  return native;
}

[[nodiscard]] core::Result<void> check_fixed_piece_positions(
    const std::string &group, const std::shared_ptr<roboplan::Scene> &scene,
    const roboplan::JointTrajectory &trajectory, double position_tolerance,
    const CartesianPlanningControl &control) {
  const auto positions = scene->getPositionLimitVectors(group, false);
  const auto width = static_cast<Eigen::Index>(trajectory.joint_names.size());
  if (!positions || positions->first.size() != width ||
      positions->second.size() != width)
    return fail<void>(ErrorCode::kValidationLimitsViolation,
                      "Moving position limits are unavailable",
                      "execution_host_scale_preparation_invalid");
  const auto invalid = [] {
    return fail<void>(ErrorCode::kValidationLimitsViolation,
                      "Moving position bounds are not finite",
                      "execution_host_scale_preparation_invalid");
  };
  // Bernstein subdivision is bounded independently of numeric convergence.
  // Exhaustion is inconclusive and never an acceptance of a joint limit.
  constexpr std::size_t kMaximumPositionIntervalChecks = 4096U;
  for (std::size_t segment = 1U; segment < trajectory.times.size(); ++segment) {
    for (Eigen::Index joint_index = 0; joint_index < width; ++joint_index) {
      const double lower = positions->first[joint_index];
      const double upper = positions->second[joint_index];
      if (!finite(lower) || !finite(upper))
        return invalid();
      const auto qualified = detail::check_quintic_position_bounds(
          {trajectory.positions[segment - 1U][joint_index],
           trajectory.velocities[segment - 1U][joint_index],
           trajectory.accelerations[segment - 1U][joint_index]},
          {trajectory.positions[segment][joint_index],
           trajectory.velocities[segment][joint_index],
           trajectory.accelerations[segment][joint_index]},
          trajectory.times[segment] - trajectory.times[segment - 1U], lower,
          upper, position_tolerance, kMaximumPositionIntervalChecks, control);
      if (!qualified.ok())
        return core::Result<void>::Err(qualified.error());
      if (qualified.value() !=
          detail::QuinticPositionBoundResult::kProvenWithin)
        return fail<void>(ErrorCode::kValidationLimitsViolation,
                          "Moving trajectory position bounds were not proved",
                          "native_moving_position_bounds_unqualified");
    }
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] core::Result<FixedPieceMeasurement>
measure_fixed_piece(const detail::CartesianTrajectoryContractInput &contract,
                    const std::shared_ptr<roboplan::Scene> &scene,
                    const roboplan::JointTrajectory &trajectory,
                    double position_tolerance,
                    const CartesianPlanningControl &control,
                    MovingFinalizationMeasurements &measurements,
                    bool preserve_cartesian_timing) {
  if (!measurements.source)
    measurements.future_v_ratio = measurements.future_a_ratio =
        measurements.future_j_ratio = measurements.future_tcp_ratio = 0.0;
  const auto invalid = [] {
    return fail<FixedPieceMeasurement>(
        ErrorCode::kValidationLimitsViolation,
        "Moving trajectory lacks a complete exact-curve qualification",
        "execution_host_scale_preparation_invalid");
  };
  // Both callers supply complete P/V/A curves already checked by the input
  // contract or shared splice builder. Keep absolute output times here: a
  // zero-origin rebase could round a segment duration and certify another
  // interpolant instead of the actual delivered composite.
  const auto model_velocity =
      scene->getVelocityLimitVectors(contract.compiled_group_name);
  const auto model_acceleration =
      scene->getAccelerationLimitVectors(contract.compiled_group_name);
  const auto positions =
      scene->getPositionLimitVectors(contract.compiled_group_name, false);
  const auto width = static_cast<Eigen::Index>(trajectory.joint_names.size());
  if (!model_velocity || !model_acceleration || !positions ||
      model_velocity->second.size() != width ||
      model_acceleration->second.size() != width ||
      positions->first.size() != width || positions->second.size() != width) {
    return invalid();
  }
  auto joint = [&] {
    MovingEnvelopeMeasurement timer(measurements, 0U,
                                    measurements.source ? "source_joint_begin"
                                                        : "future_joint_begin");
    return detail::measure_cartesian_joint_dynamics_envelope(
        contract, trajectory,
        std::span<const double>(model_velocity->second.data(),
                                static_cast<std::size_t>(width)),
        std::span<const double>(model_acceleration->second.data(),
                                static_cast<std::size_t>(width)),
        control);
  }();
  if (!joint.ok())
    return core::Result<FixedPieceMeasurement>::Err(joint.error());
  if (!joint.value().complete)
    return invalid();
  FixedPieceMeasurement result;
  std::optional<double> deferred_cartesian_chord_ratio;
  double sampled_tcp_ratio = 0.0;
  result.derivatives = std::move(joint.value().joint_derivatives);
  result.limits = resolved_runtime_joint_limits(
      contract, trajectory.joint_names, model_velocity->second,
      model_acceleration->second);
  if (contract.speed_profile.max_tcp_linear_speed_mps) {
    MovingEnvelopeMeasurement timer(measurements, 1U,
                                    measurements.source ? "source_tcp_begin"
                                                        : "future_tcp_begin");
    // The retained source remains as delivered. The new connector also needs
    // the final-waypoint chord check used for ordinary candidates.
    const auto ratio =
        detail::measure_tcp_sample_speed_ratio(contract, nullptr, scene,
                                               trajectory);
    if (!ratio)
      return invalid();
    result.velocity_ratio = *ratio;
    sampled_tcp_ratio = *ratio;
    if (!measurements.source) {
      const auto speed = detail::measure_tcp_chord_speed_mps(
          contract, scene, trajectory, control, 1.0);
      if (!speed.ok()) return core::Result<FixedPieceMeasurement>::Err(speed.error());
      constexpr double kValidatorLimitTolerance = 1e-8;
      const double limit = *contract.speed_profile.max_tcp_linear_speed_mps;
      if (!preserve_cartesian_timing ||
          speed.value() > limit + kValidatorLimitTolerance) {
        const double scaled_limit = limit * contract.speed_profile.velocity_scale;
        if (!finite_positive(scaled_limit)) return invalid();
        const double chord_ratio = speed.value() / scaled_limit;
        if (preserve_cartesian_timing) {
          deferred_cartesian_chord_ratio = chord_ratio;
        } else {
          result.velocity_ratio = std::max(result.velocity_ratio, chord_ratio);
          const double target_ratio = result.velocity_ratio / detail::kTcpSpeedTargetFraction;
          result.correction_ratio = std::max(result.correction_ratio, target_ratio);
          result.duration_ratio = std::max(result.duration_ratio, target_ratio);
        }
      }
    }
    if (!measurements.source)
      measurements.future_tcp_ratio = result.velocity_ratio;
  }
  // Use the same nominal reserve as ordinary planning only for the new
  // connector/adjacent interval. The retained source is checked as delivered.
  const double nominal_fraction =
      measurements.source ? 1.0 : 1.0 - kNominalScalingHeadroom;
  for (std::size_t index = 0; index < result.derivatives.size(); ++index) {
    const auto &derivative = result.derivatives[index];
    const auto &limit = result.limits[index];
    if (!finite_positive(limit.velocity) ||
        !finite_positive(limit.acceleration) ||
        (limit.jerk && !finite_positive(*limit.jerk)))
      return invalid();
    result.velocity_ratio =
        std::max(result.velocity_ratio, derivative.velocity / limit.velocity);
    result.correction_ratio =
        std::max({result.correction_ratio, derivative.velocity / limit.velocity,
                  std::sqrt(derivative.acceleration /
                            (nominal_fraction * limit.acceleration))});
    if (limit.jerk)
      result.correction_ratio = std::max(
          result.correction_ratio,
          std::cbrt(derivative.jerk / (nominal_fraction * *limit.jerk)));
    // Fixed endpoint P/V/A includes 1/T terms: uniform-dilation roots are
    // acceptance measurements, not a decisive duration search step.
    result.duration_ratio = std::max(
        {result.duration_ratio, derivative.velocity / limit.velocity,
         derivative.acceleration / (nominal_fraction * limit.acceleration)});
    if (limit.jerk)
      result.duration_ratio =
          std::max(result.duration_ratio,
                   derivative.jerk / (nominal_fraction * *limit.jerk));
    if (!measurements.source) {
      measurements.future_v_ratio = std::max(
          measurements.future_v_ratio, derivative.velocity / limit.velocity);
      measurements.future_a_ratio =
          std::max(measurements.future_a_ratio,
                   derivative.acceleration / limit.acceleration);
      if (limit.jerk)
        measurements.future_j_ratio = std::max(measurements.future_j_ratio,
                                               derivative.jerk / *limit.jerk);
    }
  }
  result.correction_ratio =
      std::max(result.correction_ratio, result.velocity_ratio);
  result.duration_ratio =
      std::max(result.duration_ratio, result.velocity_ratio);
  // Let the existing connector search reach its original accepted candidate
  // before adding the chord correction, preserving every previously passing
  // Cartesian connector and its exact time law.
  if (deferred_cartesian_chord_ratio && result.correction_ratio <= 1.0) {
    result.chord_correction = true;
    const double tcp_ratio = std::max(sampled_tcp_ratio, *deferred_cartesian_chord_ratio);
    result.velocity_ratio = std::max(result.velocity_ratio, tcp_ratio);
    const double target_ratio = tcp_ratio / detail::kTcpSpeedTargetFraction;
    result.correction_ratio = std::max(result.correction_ratio, target_ratio);
    result.duration_ratio = std::max(result.duration_ratio, target_ratio);
    measurements.future_tcp_ratio = result.velocity_ratio;
  }
  if (!finite(result.correction_ratio) || !finite(result.velocity_ratio))
    return invalid();
  MovingEnvelopeMeasurement timer(
      measurements, 2U,
      measurements.source ? "source_position_begin" : "future_position_begin");
  const auto position_proof =
      check_fixed_piece_positions(contract.compiled_group_name, scene,
                                  trajectory, position_tolerance, control);
  if (!position_proof.ok())
    return core::Result<FixedPieceMeasurement>::Err(position_proof.error());
  return core::Result<FixedPieceMeasurement>::Ok(std::move(result));
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
  auto dense = detail::densify_cartesian_path(path, control);
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

  const auto contract = cartesian_contract_input(binding, request, caps);
  const auto contract_control = cartesian_control(control);
  // The Deployment's `cartesian_timing` applies when the move sets none.
  core::CartesianPathDTO timed = path;
  timed.speed_mode = path.speed_mode.value_or(config.cartesian_timing);
  const bool time_optimal =
      *timed.speed_mode == core::CartesianSpeedModeDTO::kTimeOptimal;

  roboplan::CartesianPlannerOptions options;
  options.group_name = binding.compiled_group_name;
  options.dt = config.trajectory_sample_period_s;
  options.speed_mode = time_optimal ? roboplan::CartesianSpeedMode::TimeOptimal
                                    : roboplan::CartesianSpeedMode::Bounded;
  if (!request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    return fail<roboplan::JointTrajectory>(
        ErrorCode::kInvalidRequest,
        "Cartesian planning requires the admitted TCP linear speed",
        "cartesian_tcp_linear_speed_missing");
  }
  options.position_tolerance = request.tolerances.translation_tol_m;
  options.orientation_tolerance = request.tolerances.rotation_tol_rad;
  // Bounded times the resolved samples themselves, so a rounded corner
  // is resolved at most every r/20 (the whole path takes the finer step; the
  // fidelity check below still holds the node's own tolerances). TimeOptimal
  // reduces the samples to its tolerance budget and keeps the node's values.
  if (!time_optimal) {
    const auto resolution = detail::cartesian_resolution_tolerances(
        dense.value(), options.position_tolerance,
        options.orientation_tolerance);
    options.position_tolerance = resolution.translation_m;
    options.orientation_tolerance = resolution.rotation_rad;
  }
  options.velocity_scale = request.speed_profile.velocity_scale;
  // As for fast joint moves: time within the nominal acceleration share
  // directly, so the live-scaling headroom needs no uniform stretch; the
  // final stretch covers only what is still exceeded.
  options.acceleration_scale = request.speed_profile.acceleration_scale *
                               (1.0 - kNominalScalingHeadroom);
  // Bounded applies the scales to the joint limits only, so its Cartesian
  // maxima get them here: speeds at the 0.98 TCP target, accelerations within
  // the nominal share.
  options.max_linear_speed = detail::kTcpSpeedTargetFraction *
                             *request.speed_profile.max_tcp_linear_speed_mps *
                             options.velocity_scale;
  options.max_angular_speed = detail::kTcpSpeedTargetFraction *
                              path.max_angular_speed_rad_s *
                              options.velocity_scale;
  options.max_linear_acceleration =
      path.max_linear_acceleration_mps2 * options.acceleration_scale;
  options.max_angular_acceleration =
      path.max_angular_acceleration_rad_s2 * options.acceleration_scale;
  // Corners are rounded by densify_cartesian_path. TimeOptimal keeps
  // RoboPlan's own small joint-space blend between the waypoints it keeps;
  // The TCP bound holds the active TCP at 0.98 of its linear and angular speed limits
  // on TOPP-RA's grid (patches 0016, 0018).
  if (time_optimal) {
    auto bound =
        detail::make_tcp_speed_path_velocity_bound(contract, &timed, scene);
    if (!bound)
      return fail<roboplan::JointTrajectory>(
          ErrorCode::kValidationLimitsViolation,
          "Trajectory lacks a complete runtime dynamics preparation",
          "execution_host_scale_preparation_invalid");
    options.toppra_max_path_velocity = std::move(*bound);
  }

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
    std::map<std::string, std::string> details{
        {"reason", "cartesian_path_tolerance_exceeded"},
        {"failure_stage", "cartesian_output_validation"},
        {"cartesian_timing",
         std::string(core::cartesian_speed_mode_name(*timed.speed_mode))},
        {"fraction_completed",
         std::to_string(fidelity.value().fraction_completed)},
        {"max_translation_error_m",
         std::to_string(fidelity.value().max_translation_error_m)},
        {"max_rotation_error_rad",
         std::to_string(fidelity.value().max_rotation_error_rad)}};
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
  // Cartesian moves follow `cartesian_timing`, never `motion_timing`:
  // time_optimal carries the TCP bound; constant_speed is certified above.
  auto prepared = prepare_candidate_time_law(
      contract, &timed, scene, trajectory, model_velocity_limits->second,
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
          interpolant.value().unsampled_rotation_bound_rad};
  return core::Result<roboplan::JointTrajectory>::Ok(std::move(trajectory));
}

[[nodiscard]] core::Result<roboplan::JointTrajectory> time_joint_candidate(
    const std::shared_ptr<roboplan::Scene> &scene,
    const RoboPlanCandidateBindingDTO &binding,
    const RoboPlanCandidateRequestDTO &request,
    const RoboPlanCandidateComputerConfig &config,
    const detail::CartesianTrajectoryContractInput &contract,
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
  // The robot's Deployment `motion_timing`.
  const bool fast = config.motion_timing == core::MotionTimingDTO::kFast;
  // Fast: time within the nominal acceleration share directly, so the
  // live-scaling headroom needs no uniform stretch that would also slow the
  // stretches held at the TCP target; the final stretch covers only what is
  // still exceeded. Smooth: the full acceleration scale; the final
  // uniform stretch adds the TCP target, the headroom and jerk limits.
  options.acceleration_scale =
      request.speed_profile.acceleration_scale *
      (fast ? 1.0 - kNominalScalingHeadroom : 1.0);
  options.max_output_samples = config.maximum_trajectory_samples;
  // Fast: hold the active TCP at 0.98 of its speed limit on TOPP-RA's
  // grid.
  if (fast && contract.speed_profile.max_tcp_linear_speed_mps) {
    auto bound =
        detail::make_tcp_speed_path_velocity_bound(contract, nullptr, scene);
    if (!bound)
      return fail<roboplan::JointTrajectory>(
          ErrorCode::kValidationLimitsViolation,
          "Trajectory lacks a complete runtime dynamics preparation",
          "execution_host_scale_preparation_invalid");
    options.max_path_velocity = std::move(*bound);
  }
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
  // The caller checks the samples and applies one common time dilation for
  // joint caps, the TCP (smooth: down to its target; fast: a sampled
  // exceedance), optional jerk and live-scaling headroom.
  return core::Result<roboplan::JointTrajectory>::Ok(std::move(trajectory));
}

// Internal IK stopping tolerance for pose goals (see solve_ik_locked).
constexpr double kPoseGoalRefineLinearM = 1e-5;
constexpr double kPoseGoalRefineAngularRad = 1e-4;

[[nodiscard]] core::Result<std::vector<double>> solve_ik_locked(
    const std::shared_ptr<roboplan::Scene> &scene,
    const RoboPlanCandidateBindingDTO &binding, const core::Pose3DTO &goal,
    const core::ResolvedFrameDTO &base_frame,
    const core::ResolvedFrameDTO &tcp_frame,
    const core::ControlledStateDTO &seed_state, double timeout_s,
    const RoboPlanCandidateControl &control,
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

  auto options = production_planning_ik_options(binding.compiled_group_name);
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
  const auto ik_started = std::chrono::steady_clock::now();
  if (!solver.solveIk(native_goal, native_seed, solution)) {
    return fail<std::vector<double>>(ErrorCode::kIkFailed,
                                     "RoboPlan could not resolve the pose goal",
                                     "native_ik_no_solution");
  }
  if (tolerances != nullptr) {
    // The request tolerances stay the acceptance check above. The first
    // accepted iterate of the damped solver lands just inside them on the start
    // side (about 1 mm short), and a goal closer than that accepts the seed
    // itself. Refine the accepted solution to a tight internal stop so pose
    // goals land on target; keep the accepted one if refinement cannot.
    // Cancellation and the deadline are rechecked first, and refinement only
    // spends what is left of the original IK budget (none left: skip it).
    if (auto error = control_error(control, "before_ik_refinement");
        error.has_value()) {
      return core::Result<std::vector<double>>::Err(std::move(*error));
    }
    auto refine = options;
    refine.max_time =
        options.max_time - std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - ik_started)
                               .count();
    refine.max_linear_error_norm =
        std::min(options.max_linear_error_norm, kPoseGoalRefineLinearM);
    refine.max_angular_error_norm =
        std::min(options.max_angular_error_norm, kPoseGoalRefineAngularRad);
    refine.max_restarts = 0;
    refine.fast_return = true;
    roboplan::JointConfiguration refined;
    if (refine.max_time > 0.0 &&
        roboplan::SimpleIk(scene, refine)
            .solveIk(native_goal, solution, refined) &&
        refined.positions.size() == solution.positions.size() &&
        refined.positions.allFinite()) {
      solution = std::move(refined);
    }
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

[[nodiscard]] core::Result<RoboPlanCandidateRequestDTO>
moving_suffix_request(const RoboPlanCandidateBindingDTO &binding,
                      const RoboPlanCandidateRequestDTO &input,
                      const CartesianPlanningControl &control,
                      std::size_t maximum_samples) {
  // Cartesian positional fidelity alone does not prove the authored joint-space
  // blend constraint for a new quintic connector. The existing caller owns
  // approved spring-back fallback for this unqualified constraint.
  if (std::holds_alternative<core::CartesianPathDTO>(input.motion))
    return fail<RoboPlanCandidateRequestDTO>(
        ErrorCode::kUnsupportedCapability,
        "Moving Cartesian connector joint-space blend bounds are not qualified",
        "native_moving_cartesian_blend_proof_unavailable");
  const auto invalid = [] {
    return fail<RoboPlanCandidateRequestDTO>(
        ErrorCode::kInvalidRequest,
        "Moving replacement source differs from its captured planning context",
        "native_moving_source_invalid");
  };
  const auto &moving = *input.moving_replacement;
  const auto &prefix = moving.source_prefix_trajectory;
  const double divergence = moving.divergence_cursor_s - moving.source_cursor_s;
  if (moving.source_goal.controller_goal_id.empty() ||
      !moving.source_goal.controller_goal_generation.nonzero() ||
      !moving.source_binding_epoch.nonzero() ||
      !finite_nonnegative(moving.source_cursor_s) ||
      !finite_positive(divergence) ||
      !finite_positive(moving.remaining_preparation_budget_s) ||
      prefix.resources != binding.resources ||
      prefix.joint_names != binding.compiled_joint_names ||
      prefix.waypoints.size() < 2U ||
      prefix.waypoints.size() > maximum_samples ||
      prefix.waypoints.front().time_from_start_s != 0.0 ||
      prefix.waypoints.back().time_from_start_s != divergence ||
      (prefix.runtime_scale_bounds && !valid_scale_evidence(*prefix.runtime_scale_bounds)) ||
      !prefix.cartesian_waypoint_times_s.empty() ||
      prefix.cartesian_fidelity_metrics ||
      !contains_frame(binding.permitted_frames, moving.source_base_frame) ||
      !contains_frame(binding.permitted_frames, moving.source_tcp_frame))
    return invalid();
  if (const auto error = detail::validate_cartesian_trajectory_output(
          native_trajectory(prefix), binding.compiled_joint_names, control))
    return core::Result<RoboPlanCandidateRequestDTO>::Err(*error);
  const auto &speed = moving.source_speed_profile;
  if (!finite_positive(speed.velocity_scale) || speed.velocity_scale > 1.0 ||
      !finite_positive(speed.acceleration_scale) ||
      speed.acceleration_scale > 1.0 ||
      (speed.max_tcp_linear_speed_mps &&
       !finite_positive(*speed.max_tcp_linear_speed_mps)))
    return invalid();
  if (!core::validate_controlled_state(moving.suffix_start_state).ok())
    return invalid();
  auto seed_identity = moving.suffix_start_state;
  seed_identity.positions = input.start_state.positions;
  seed_identity.velocities = input.start_state.velocities;
  if (seed_identity != input.start_state)
    return invalid();
  const auto captured = core::angular_position_projection(input.start_state);
  const auto seed =
      core::angular_position_projection(moving.suffix_start_state);
  if (!captured.ok() || !seed.ok() ||
      captured.value() != prefix.waypoints.front().positions_rad ||
      seed.value() != prefix.waypoints.back().positions_rad)
    return invalid();
  for (std::size_t index = 0; index < binding.resources.size(); ++index) {
    double captured_velocity = 0.0;
    if (input.start_state.velocities) {
      const auto *value = std::get_if<core::AngularVelocityDTO>(
          &(*input.start_state.velocities)[index]);
      if (!value)
        return invalid();
      captured_velocity = value->value_rad_s;
    }
    if (captured_velocity != prefix.waypoints.front().velocities_rad_s[index])
      return invalid();
    if (moving.suffix_start_state.velocities) {
      const auto *value = std::get_if<core::AngularVelocityDTO>(
          &(*moving.suffix_start_state.velocities)[index]);
      if (!value || value->value_rad_s != 0.0)
        return invalid();
    }
  }
  // A connector cannot repair an already excessive incoming derivative at
  // its first instant by choosing a longer duration.
  for (const auto &cap : input.dynamics_limits) {
    const auto found = std::find(binding.resources.begin(),
                                 binding.resources.end(), cap.resource_id);
    const auto index =
        static_cast<std::size_t>(found - binding.resources.begin());
    const auto &seam = prefix.waypoints.back();
    if ((cap.limit.max_velocity_rad_s &&
         std::abs(seam.velocities_rad_s[index]) >
             *cap.limit.max_velocity_rad_s *
                 input.speed_profile.velocity_scale) ||
        (cap.limit.max_acceleration_rad_s2 &&
         std::abs(seam.accelerations_rad_s2[index]) >
             *cap.limit.max_acceleration_rad_s2 *
                 input.speed_profile.acceleration_scale))
      return fail<RoboPlanCandidateRequestDTO>(
          ErrorCode::kValidationLimitsViolation,
          "Moving connector incoming derivatives exceed the successor limits",
          "native_moving_incoming_limits_violated");
  }
  auto suffix = input;
  suffix.moving_replacement.reset();
  suffix.start_state = moving.suffix_start_state;
  if (const auto *authored =
          std::get_if<core::ResolvedJointGoalDTO>(&input.motion)) {
    if (authored->resources.empty() ||
        authored->resources.size() != authored->joint_names.size() ||
        authored->resources.size() != authored->positions_rad.size())
      return invalid();
    core::ResolvedJointGoalDTO resolved{
        binding.resources, binding.compiled_joint_names, captured.value()};
    std::set<std::size_t> selected;
    for (std::size_t index = 0; index < authored->resources.size(); ++index) {
      const auto found =
          std::find(binding.resources.begin(), binding.resources.end(),
                    authored->resources[index]);
      if (found == binding.resources.end() ||
          !finite(authored->positions_rad[index]))
        return invalid();
      const auto offset =
          static_cast<std::size_t>(found - binding.resources.begin());
      if (!selected.insert(offset).second ||
          authored->joint_names[index] != binding.compiled_joint_names[offset])
        return invalid();
      resolved.positions_rad[offset] = authored->positions_rad[index];
    }
    suffix.motion = std::move(resolved);
  }
  return core::Result<RoboPlanCandidateRequestDTO>::Ok(std::move(suffix));
}

[[nodiscard]] core::Result<core::TrajectoryDTO> finalize_moving_candidate(
    const RoboPlanCandidateBindingDTO &binding,
    const RoboPlanCandidateRequestDTO &input,
    const ResolvedDynamicsCaps &future_caps,
    const std::shared_ptr<roboplan::Scene> &scene, core::TrajectoryDTO suffix,
    std::size_t maximum_samples, const RoboPlanCandidateControl &control) {
  const auto &moving = *input.moving_replacement;
  const auto &prefix = moving.source_prefix_trajectory;
  const double divergence = moving.divergence_cursor_s - moving.source_cursor_s;
  auto source_caps = resolve_dynamics_caps(
      binding, moving.source_per_resource_dynamics_limits);
  if (!source_caps.ok())
    return core::Result<core::TrajectoryDTO>::Err(source_caps.error());
  const detail::CartesianTrajectoryContractInput source_contract{
      binding.compiled_group_name,
      core::compiled_frame_name(moving.source_base_frame),
      core::compiled_frame_name(moving.source_tcp_frame),
      moving.source_speed_profile, source_caps.value().portable_by_joint};
  const auto future_contract =
      cartesian_contract_input(binding, input, future_caps);
  const auto contract_control = cartesian_control(control);
  MovingFinalizationMeasurements measurements{input};
  if (prefix.runtime_scale_bounds) {
    const auto positions = check_fixed_piece_positions(
        source_contract.compiled_group_name, scene, native_trajectory(prefix),
        input.tolerances.joint_position_tol_rad, contract_control);
    if (!positions.ok())
      return core::Result<core::TrajectoryDTO>::Err(positions.error());
  }
  auto source_piece = prefix;
  // The Kernel's existing derived bounds cover the retained source. Only the
  // last restricted boundary interval needs a fresh local qualification.
  if (prefix.runtime_scale_bounds && source_piece.waypoints.size() > 2U)
    source_piece.waypoints.erase(source_piece.waypoints.begin(),
                                 source_piece.waypoints.end() - 2);
  measurements.source_points = source_piece.waypoints.size();
  measurements.Record("source_begin");
  auto source = measure_fixed_piece(
      source_contract, scene, native_trajectory(source_piece),
      input.tolerances.joint_position_tol_rad, contract_control, measurements,
      false);
  measurements.Record("source_return");
  measurements.source = false;
  if (!source.ok())
    return core::Result<core::TrajectoryDTO>::Err(source.error());
  if (source.value().correction_ratio > 1.0)
    return fail<core::TrajectoryDTO>(
        ErrorCode::kValidationLimitsViolation,
        "The retained source prefix violates its resolved motion limits",
        "native_moving_source_limits_violated");
  // Final-time-law suffix evidence certifies retained intervals, never the
  // connector. Splice verifies literal PVA and the explicitly approved <=1ns
  // AND relative-span equivalence for every translated interval.
  if (!suffix.runtime_scale_bounds ||
      !valid_scale_evidence(*suffix.runtime_scale_bounds))
    return fail<core::TrajectoryDTO>(
        ErrorCode::kValidationLimitsViolation,
        "Moving suffix lacks final time-law evidence",
        "execution_host_scale_preparation_invalid");
  const auto suffix_bounds = *suffix.runtime_scale_bounds;
  suffix.runtime_scale_bounds.reset();
  const auto velocity_limits =
      scene->getVelocityLimitVectors(binding.compiled_group_name);
  const auto acceleration_limits =
      scene->getAccelerationLimitVectors(binding.compiled_group_name);
  if (!velocity_limits || !acceleration_limits)
    return fail<core::TrajectoryDTO>(
        ErrorCode::kUnsupportedCapability,
        "Moving duration limits are unavailable",
        "native_model_dynamics_limits_unavailable");
  const auto endpoint_limits = resolved_runtime_joint_limits(
      future_contract, suffix.joint_names, velocity_limits->second,
      acceleration_limits->second);
  const double target_time = suffix.waypoints.back().time_from_start_s;
  std::vector<double> entries;
  for (std::size_t index = 1U; index < suffix.waypoints.size(); ++index) {
    const double from = suffix.waypoints[index - 1U].time_from_start_s;
    const double to = suffix.waypoints[index].time_from_start_s;
    const double middle = from + (to - from) / 2.0;
    if (middle > from && middle < to && middle < target_time)
      entries.push_back(middle);
    if (to < target_time)
      entries.push_back(to);
    if (to >= target_time)
      break;
  }
  const auto nonzero_derivative = [](const auto &values) {
    return std::any_of(values.begin(), values.end(),
                       [](double value) { return value != 0.0; });
  };
  const bool moving_source =
      nonzero_derivative(prefix.waypoints.front().velocities_rad_s) ||
      nonzero_derivative(prefix.waypoints.back().velocities_rad_s) ||
      nonzero_derivative(prefix.waypoints.front().accelerations_rad_s2) ||
      nonzero_derivative(prefix.waypoints.back().accelerations_rad_s2);
  const auto sample_shape = core::validate_trajectory_for_sampling(suffix);
  if (!sample_shape.ok())
    return core::Result<core::TrajectoryDTO>::Err(sample_shape.error());
  core::TrajectorySampleResult entry_state;
  ErrorDTO last = make_error(
      ErrorCode::kPlanningFailed,
      "No qualified moving entry exists before the first successor target",
      "native_moving_connector_unqualified");
  for (const double entry : entries) {
    ++measurements.entries;
    measurements.entry_s = entry;
    if (auto error = control_error(control, "moving_entry_search"))
      return core::Result<core::TrajectoryDTO>::Err(*error);
    const auto state =
        core::sample_trajectory_prevalidated(suffix, entry, entry_state);
    if (!state.ok())
      return core::Result<core::TrajectoryDTO>::Err(state.error());
    if (moving_source && !nonzero_derivative(entry_state.velocities_rad_s))
      continue;
    core::TrajectoryDTO retained_suffix;
    retained_suffix.resources = suffix.resources;
    retained_suffix.joint_names = suffix.joint_names;
    retained_suffix.waypoints.push_back({entry, entry_state.positions_rad,
                                         entry_state.velocities_rad_s,
                                         entry_state.accelerations_rad_s2});
    for (const auto &point : suffix.waypoints)
      if (point.time_from_start_s > entry)
        retained_suffix.waypoints.push_back(point);
    const auto positions = check_fixed_piece_positions(
        future_contract.compiled_group_name, scene,
        native_trajectory(retained_suffix),
        input.tolerances.joint_position_tol_rad, contract_control);
    if (!positions.ok()) {
      last = positions.error();
      continue;
    }
    // The ordinary suffix's elapsed timing is already derived from its limits.
    // Ratios below suggest new durations only; fixed boundary P/V/A makes the
    // feasibility function non-monotonic, so every suggestion is rebuilt.
    double lower_bound = 0.0;
    const auto &inlet = prefix.waypoints.back();
    for (std::size_t joint = 0; joint < endpoint_limits.size(); ++joint) {
      const auto &limit = endpoint_limits[joint];
      lower_bound = std::max(
          {lower_bound,
           std::abs(entry_state.positions_rad[joint] -
                    inlet.positions_rad[joint]) /
               limit.velocity,
           std::abs(entry_state.velocities_rad_s[joint] -
                    inlet.velocities_rad_s[joint]) /
               ((1.0 - kNominalScalingHeadroom) * limit.acceleration)});
      if (limit.jerk)
        lower_bound = std::max(
            lower_bound, std::abs(entry_state.accelerations_rad_s2[joint] -
                                  inlet.accelerations_rad_s2[joint]) /
                             ((1.0 - kNominalScalingHeadroom) * *limit.jerk));
    }
    double duration = std::max(entry, lower_bound);
    measurements.endpoint_lower_bound_s = lower_bound;
    double previous_ratio = std::numeric_limits<double>::infinity();
    bool chord_search_started = false;
    for (;;) {
      if (auto error = control_error(control, "moving_duration_search"))
        return core::Result<core::TrajectoryDTO>::Err(*error);
      ++measurements.attempts;
      measurements.duration_s = duration;
      measurements.trace_stages = measurements.attempts == 1U;
      if (measurements.attempts <= 128U &&
          (measurements.attempts & (measurements.attempts - 1U)) == 0U)
        measurements.Record("attempt_begin");
      const auto splice_started = std::chrono::steady_clock::now();
      auto composite = core::build_quintic_trajectory_splice(
          prefix, suffix, 0.0, divergence, entry, duration);
      measurements.splice_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - splice_started)
              .count();
      if (!composite.ok()) {
        last = composite.error();
        break;
      }
      if (composite.value().waypoints.size() > maximum_samples) {
        last = make_error(
            ErrorCode::kPlanningFailed,
            "Moving candidate exceeds the existing trajectory sample bound",
            "native_trajectory_shape_invalid");
        break;
      }
      core::TrajectoryDTO future;
      future.resources = composite.value().resources;
      future.joint_names = composite.value().joint_names;
      for (const auto &point : composite.value().waypoints)
        if (point.time_from_start_s >= divergence)
          future.waypoints.push_back(point);
      // Check the connector and its adjacent restricted suffix interval only.
      // Later knots retain the already-qualified suffix PVA/time law.
      // Timestamp translation can move a boundary chord over limit + epsilon;
      // independent final validation then refuses the candidate without motion.
      if (future.waypoints.size() > 3U) future.waypoints.resize(3U);
      measurements.future_points = future.waypoints.size();
      measurements.future_points_total += future.waypoints.size();
      auto changed =
          measure_fixed_piece(future_contract, scene, native_trajectory(future),
                              input.tolerances.joint_position_tol_rad,
                              contract_control, measurements,
                              std::holds_alternative<core::CartesianPathDTO>(input.motion));
      if (measurements.trace_stages)
        measurements.Record("future_return");
      if (!changed.ok()) {
        last = changed.error();
        break;
      }
      const double ratio = changed.value().correction_ratio;
      measurements.ratio = ratio;
      measurements.next_duration_ratio = changed.value().duration_ratio;
      if (ratio > 1.0) {
        if (changed.value().chord_correction && !chord_search_started) {
          previous_ratio = std::numeric_limits<double>::infinity();
          chord_search_started = true;
        }
        if (!(ratio < previous_ratio))
          break;
        const double next = std::nextafter(
            duration * changed.value().duration_ratio, std::numeric_limits<double>::infinity());
        if (!finite_positive(next) || !(next > duration))
          break;
        previous_ratio = ratio;
        duration = next;
        continue;
      }
      const std::array envelopes{
          core::detail::TrajectoryDynamicsEnvelope{
              source.value().derivatives, source.value().limits,
              std::max(1.0, source.value().velocity_ratio)},
          core::detail::TrajectoryDynamicsEnvelope{
              changed.value().derivatives, changed.value().limits,
              std::max(1.0, changed.value().velocity_ratio)}};
      const auto prepared =
          core::detail::prepare_fixed_runtime_scaling(envelopes, {4.0, 16.0});
      if (!prepared)
        break;
      const double velocity_ratio = std::max(source.value().velocity_ratio,
                                             changed.value().velocity_ratio);
      const double raw_stop_path_rate =
          velocity_ratio > 0.0 ? 1.0 / velocity_ratio : 1.0;
      if (!finite(raw_stop_path_rate))
        break;
      const double stop_path_rate =
          std::max(1.0, std::nextafter(raw_stop_path_rate, 0.0));
      composite.value().runtime_scale_bounds =
          core::TrajectoryRuntimeScaleBoundsDTO{
              prepared->max_rate_per_s, prepared->max_rate_change_per_s2,
              stop_path_rate, std::nullopt};
      auto &combined = *composite.value().runtime_scale_bounds;
      const auto intersect = [&](const auto &retained) {
        combined.max_rate_per_s =
            std::min(combined.max_rate_per_s, retained.max_rate_per_s);
        combined.max_rate_change_per_s2 = std::min(
            combined.max_rate_change_per_s2, retained.max_rate_change_per_s2);
        combined.max_stop_path_rate =
            std::min(combined.max_stop_path_rate, retained.max_stop_path_rate);
      };
      intersect(suffix_bounds);
      if (prefix.runtime_scale_bounds)
        intersect(*prefix.runtime_scale_bounds);
      if (auto error = control_error(control, "moving_finalization"))
        return core::Result<core::TrajectoryDTO>::Err(*error);
      return composite;
    }
  }
  return core::Result<core::TrajectoryDTO>::Err(std::move(last));
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
    const RoboPlanCandidateControl &control,
    const core::TolerancesDTO *tolerances) {
  std::lock_guard lock(computation_mutex_);
  try {
    if (config_.rng_seed.has_value()) {
      scene_->setRngSeed(*config_.rng_seed);
    }
    auto result = solve_ik_locked(scene_, binding_, goal, base_frame, tcp_frame,
                                  seed_state, timeout_s, control, tolerances);
    if (!result.ok() || tolerances == nullptr) return result;
    const Eigen::VectorXd q = eigen_vector(result.value());
    const auto limits = scene_->getPositionLimitVectors(binding_.compiled_group_name, false);
    if (!limits || limits->first.size() != q.size() || limits->second.size() != q.size())
      return fail<std::vector<double>>(ErrorCode::kIkFailed,
          "Diagnostic IK limits unavailable", "native_diagnostic_ik_unqualified");
    for (Eigen::Index i = 0; i < q.size(); ++i) {
      if (std::isnan(limits->first[i]) || std::isnan(limits->second[i]) ||
          q[i] < limits->first[i] || q[i] > limits->second[i])
        return fail<std::vector<double>>(ErrorCode::kIkFailed,
            "Diagnostic IK violates model limits", "native_diagnostic_ik_unqualified");
    }
    const auto full = scene_->toFullJointPositions(binding_.compiled_group_name, q);
    const auto actual = scene_->forwardKinematics(full, core::compiled_frame_name(tcp_frame),
                                                   core::compiled_frame_name(base_frame));
    const auto expected = native_pose(goal);
    auto options = production_planning_ik_options(binding_.compiled_group_name);
    if (finite_positive(tolerances->translation_tol_m))
      options.max_linear_error_norm = std::min(options.max_linear_error_norm, tolerances->translation_tol_m);
    if (finite_positive(tolerances->rotation_tol_rad))
      options.max_angular_error_norm = std::min(options.max_angular_error_norm, tolerances->rotation_tol_rad);
    if (!actual.allFinite() ||
        (actual.block<3, 1>(0, 3) - expected.block<3, 1>(0, 3)).norm() > options.max_linear_error_norm ||
        rotation_distance(actual.block<3, 3>(0, 0), expected.block<3, 3>(0, 0)) > options.max_angular_error_norm)
      return fail<std::vector<double>>(ErrorCode::kIkFailed,
          "Diagnostic IK does not reach its target", "native_diagnostic_ik_unqualified");
    if (auto expired = control_error(control, "diagnostic_ik_qualification"))
      return core::Result<std::vector<double>>::Err(std::move(*expired));
    return result;
  } catch (const std::exception &error) {
    return fail<std::vector<double>>(ErrorCode::kPlanningFailed,
                                     "RoboPlan IK adapter failed: " +
                                         std::string(error.what()),
                                     "native_ik_adapter_failure");
  }
}

core::Result<core::TrajectoryDTO> RoboPlanCandidateComputer::compute(
    const RoboPlanCandidateRequestDTO &input,
    const RoboPlanCandidateControl &caller_control,
    std::optional<core::ControlledStateDTO> *qualified_target) {
  if (qualified_target) qualified_target->reset();
  const auto whole_started = input.moving_replacement
                                 ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  RoboPlanCandidateControl whole_control;
  if (input.moving_replacement) {
    whole_control = caller_control;
    const double remaining =
        input.moving_replacement->remaining_preparation_budget_s;
    if (!finite_positive(remaining) ||
        remaining >
            std::chrono::duration<double>(std::chrono::hours(24)).count())
      return fail<core::TrajectoryDTO>(ErrorCode::kInvalidRequest,
                                       "Moving preparation budget is invalid",
                                       "native_moving_source_invalid");
    const auto planning = input.moving_replacement->remaining_planning_budget_s;
    if (planning &&
        (!finite_positive(*planning) ||
         *planning > std::chrono::duration<double>(std::chrono::hours(24)).count()))
      return fail<core::TrajectoryDTO>(ErrorCode::kInvalidRequest,
                                       "Moving planning budget is invalid",
                                       "native_moving_source_invalid");
    const auto deadline =
        whole_started +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(remaining));
    whole_control.deadline_expired =
        [deadline, external = caller_control.deadline_expired] {
          return (external && external()) ||
                 std::chrono::steady_clock::now() >= deadline;
        };
  }
  const auto request_validation = validate_request_binding(binding_, input);
  if (!request_validation.ok()) {
    return core::Result<core::TrajectoryDTO>::Err(request_validation.error());
  }
  auto caps = resolve_dynamics_caps(binding_, input.dynamics_limits);
  if (!caps.ok()) {
    return core::Result<core::TrajectoryDTO>::Err(caps.error());
  }

  log_moving_phase(input, "compute_begin", whole_started);
  std::lock_guard lock(computation_mutex_);
  try {
    std::optional<RoboPlanCandidateRequestDTO> suffix_request;
    RoboPlanCandidateControl suffix_control;
    std::optional<std::chrono::steady_clock::time_point> suffix_deadline;
    if (input.moving_replacement) {
      auto suffix = moving_suffix_request(binding_, input,
                                          cartesian_control(whole_control),
                                          config_.maximum_trajectory_samples);
      if (!suffix.ok())
        return core::Result<core::TrajectoryDTO>::Err(suffix.error());
      suffix_request = std::move(suffix.value());
      const double remaining =
          std::min(input.moving_replacement->remaining_preparation_budget_s,
                   input.moving_replacement->remaining_planning_budget_s.value_or(
                       input.moving_replacement->remaining_preparation_budget_s)) -
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        whole_started)
              .count();
      if (!finite_positive(remaining))
        return fail<core::TrajectoryDTO>(ErrorCode::kPlanningTimeout,
                                         "Moving preparation budget expired",
                                         "planning_timeout");
      suffix_request->planning_timeout_s =
          std::min(input.planning_timeout_s, remaining);
      suffix_deadline =
          std::chrono::steady_clock::now() +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(
                  suffix_request->planning_timeout_s));
      if (const auto planning = input.moving_replacement->remaining_planning_budget_s)
        suffix_deadline = std::min(
            *suffix_deadline,
            whole_started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(*planning)));
      suffix_control.cancellation_requested =
          whole_control.cancellation_requested;
      suffix_control.deadline_expired = [deadline = *suffix_deadline,
                                         expired =
                                             whole_control.deadline_expired,
                                         planning_expired =
                                             caller_control.planning_deadline_expired] {
        return (expired && expired()) ||
               (planning_expired && planning_expired()) ||
               std::chrono::steady_clock::now() >= deadline;
      };
    }
    log_moving_phase(input, "suffix_begin", whole_started);
    const auto &request = suffix_request ? *suffix_request : input;
    const auto &control = suffix_request ? suffix_control : caller_control;
    const auto remaining_planning_timeout = [&] {
      return suffix_deadline ? std::min(request.planning_timeout_s,
                                        std::chrono::duration<double>(
                                            *suffix_deadline -
                                            std::chrono::steady_clock::now())
                                            .count())
                             : request.planning_timeout_s;
    };
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
    if (request.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled) {
      if (!request.diagnostic_qualified_target) {
        return fail<core::TrajectoryDTO>(ErrorCode::kInvalidRequest,
            "Diagnostic planning requires a qualified target", "native_diagnostic_target_missing");
      }
      auto expected = request.start_state;
      expected.positions = request.diagnostic_qualified_target->positions;
      expected.velocities = request.diagnostic_qualified_target->velocities;
      auto projected = core::angular_position_projection(*request.diagnostic_qualified_target);
      if (expected != *request.diagnostic_qualified_target || !projected.ok() ||
          request.diagnostic_qualified_target->velocities.has_value()) {
        return fail<core::TrajectoryDTO>(ErrorCode::kInvalidRequest,
            "Diagnostic target differs from the retained binding", "native_diagnostic_target_invalid");
      }
      native_goal.positions = eigen_vector(projected.value());
      resolved_from_pose = std::holds_alternative<core::PoseGoalDTO>(request.motion);
    } else if (const auto *joint =
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
      const double ik_timeout = remaining_planning_timeout();
      if (suffix_deadline && !finite_positive(ik_timeout))
        return fail<core::TrajectoryDTO>(
            ErrorCode::kPlanningTimeout,
            "Moving suffix planning timeout expired", "planning_timeout");
      auto solution = solve_ik_locked(
          planning_scene, binding_, pose->goal, request.base_frame,
          request.tcp_frame, request.start_state, ik_timeout, control,
          &request.tolerances);
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
    // This evidence does not change ordinary planning or its errors. Only a
    // finite target within the retained limits may qualify for preview replan.
    const auto limits = planning_scene->getPositionLimitVectors(binding_.compiled_group_name, false);
    bool within_limits = limits && limits->first.size() == native_goal.positions.size() &&
                         limits->second.size() == native_goal.positions.size();
    if (within_limits) {
      for (Eigen::Index i = 0; i < native_goal.positions.size(); ++i) {
        if (std::isnan(limits->first[i]) || std::isnan(limits->second[i]) ||
            native_goal.positions[i] < limits->first[i] ||
            native_goal.positions[i] > limits->second[i]) {
          within_limits = false;
          break;
        }
      }
    }
    if (!within_limits && request.collision_mode == core::PlannerCollisionMode::kDiagnosticDisabled)
      return fail<core::TrajectoryDTO>(ErrorCode::kInvalidRequest,
          "Diagnostic target is outside retained model limits", "native_diagnostic_target_invalid");
    if (within_limits && qualified_target) {
      auto target = request.start_state;
      target.positions.clear();
      for (Eigen::Index i = 0; i < native_goal.positions.size(); ++i)
        target.positions.emplace_back(core::AngularPositionDTO{native_goal.positions[i]});
      target.velocities.reset();
      *qualified_target = std::move(target);
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

    const double rrt_timeout = remaining_planning_timeout();
    if (suffix_deadline && !finite_positive(rrt_timeout))
      return fail<core::TrajectoryDTO>(ErrorCode::kPlanningTimeout,
                                       "Moving suffix planning timeout expired",
                                       "planning_timeout");
    const auto planner_settings = core::resolve_planner_settings(
        config_.planner_defaults(), request.planner);
    roboplan::RRT planner(
        planning_scene,
        production_rrt_options(binding_.compiled_group_name, rrt_timeout,
                               request.planning_attempt_index,
                               planner_settings));
    // best_of_n: n first-path plans with distinct seeds (seed + run) sharing
    // the RRT budget; the shortest shortcut path wins. Other modes plan once.
    const auto search_started = std::chrono::steady_clock::now();
    const std::uint32_t runs =
        planner_settings.optimize == core::PlannerOptimizeDTO::kBestOfN
            ? planner_settings.n
            : 1U;
    std::optional<roboplan::JointPath> candidate_path;
    double candidate_length = std::numeric_limits<double>::infinity();
    std::string planner_error;
    for (std::uint32_t run = 0U; run < runs; ++run) {
      if (run > 0U) {
        const double remaining =
            rrt_timeout - std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - search_started)
                              .count();
        if (!finite_positive(remaining)) break;
        planner.setOptions(production_rrt_options(
            binding_.compiled_group_name, remaining,
            request.planning_attempt_index, planner_settings));
      }
      const auto run_seed =
          request_seed.has_value()
              ? std::optional<std::uint32_t>(*request_seed + run)
              : std::nullopt;
      if (run_seed.has_value()) {
        planner.setRngSeed(*run_seed);
      }
      auto path = planner.plan(native_start, native_goal);
      if (auto error = control_error(control, "rrt"); error.has_value()) {
        return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
      }
      if (!path.has_value()) {
        planner_error = path.error();
        continue;
      }
      auto run_path = std::move(path.value());
      if (shortcut_candidate_for_attempt(request.planning_attempt_index)) {
        const auto shortcut_options = production_shortcutting_options(
            binding_.compiled_group_name, run_seed,
            request.planning_attempt_index,
            planner_settings.collision_check_step_rad);
        roboplan::PathShortcutter shortcutter(planning_scene, shortcut_options);
        run_path = shortcutter.shortcut(run_path);
        if (auto error = control_error(control, "shortcut"); error.has_value()) {
          return core::Result<core::TrajectoryDTO>::Err(std::move(*error));
        }
      }
      const double length =
          runs == 1U ? 0.0
                     : roboplan::computePathLength(*planning_scene,
                                                   binding_.compiled_group_name,
                                                   run_path)
                           .value_or(std::numeric_limits<double>::infinity());
      if (!candidate_path.has_value() || length < candidate_length) {
        candidate_path = std::move(run_path);
        candidate_length = length;
      }
    }
    if (!candidate_path.has_value()) {
      return fail<core::TrajectoryDTO>(
          ErrorCode::kPlanningFailed,
          "RoboPlan could not find a collision-free joint path",
          "native_rrt_failed", {{"planner_error", planner_error}});
    }
    const auto contract =
        cartesian_contract_input(binding_, request, caps.value());
    auto trajectory = time_joint_candidate(
        planning_scene, binding_, request, config_, contract, control,
        *candidate_path, native_start.positions, native_goal.positions);
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
        contract, nullptr,
        planning_scene, trajectory.value(), model_velocity->second,
        model_acceleration->second, cartesian_control(control),
        nullptr, input.moving_replacement.has_value());
    if (!prepared.ok())
      return core::Result<core::TrajectoryDTO>::Err(prepared.error());
    auto portable = portable_trajectory(trajectory.value(), binding_,
                                        config_.maximum_trajectory_samples,
                                        prepared.value());
    if (portable.ok() && input.moving_replacement) {
      log_moving_phase(input, "connector_begin", whole_started);
      auto finalized = finalize_moving_candidate(
          binding_, input, caps.value(), planning_scene,
          std::move(portable.value()), config_.maximum_trajectory_samples,
          whole_control);
      log_moving_phase(input, "connector_return", whole_started);
      return finalized;
    }
    return portable;
  } catch (const std::exception &error) {
    return fail<core::TrajectoryDTO>(ErrorCode::kPlanningFailed,
                                     "RoboPlan candidate adapter failed: " +
                                         std::string(error.what()),
                                     "native_candidate_adapter_failure");
  }
}

} // namespace manyforge::roboplan_backend
