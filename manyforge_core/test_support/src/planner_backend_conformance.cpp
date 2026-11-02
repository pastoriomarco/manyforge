#include <manyforge_core/testing/planner_backend_conformance.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace manyforge::core::testing {
namespace {

constexpr double kNumericEpsilon = 1e-9;

const char* motion_name(PlannerMotionCapability motion) noexcept {
  switch (motion) {
    case PlannerMotionCapability::kJoint:
      return "joint";
    case PlannerMotionCapability::kPose:
      return "pose";
    case PlannerMotionCapability::kCartesian:
      return "cartesian";
  }
  return "unknown";
}

const char* motion_capability_key(PlannerMotionCapability motion) noexcept {
  switch (motion) {
    case PlannerMotionCapability::kJoint:
      return "planning.motion.joint_goal";
    case PlannerMotionCapability::kPose:
      return "planning.motion.pose_goal";
    case PlannerMotionCapability::kCartesian:
      return "planning.motion.cartesian_path";
  }
  return "planning.motion.unknown";
}

std::optional<PlannerMotionCapability> motion_capability(
    const ResolvedMotionSpecDTO& motion) noexcept {
  if (std::holds_alternative<ResolvedJointGoalDTO>(motion)) {
    return PlannerMotionCapability::kJoint;
  }
  if (std::holds_alternative<PoseGoalDTO>(motion)) {
    return PlannerMotionCapability::kPose;
  }
  if (std::holds_alternative<CartesianPathDTO>(motion)) {
    return PlannerMotionCapability::kCartesian;
  }
  return std::nullopt;
}

bool supports_motion(const PlannerCapabilityProfile& capabilities,
                     PlannerMotionCapability motion) noexcept {
  switch (motion) {
    case PlannerMotionCapability::kJoint:
      return capabilities.joint_goal;
    case PlannerMotionCapability::kPose:
      return capabilities.pose_goal;
    case PlannerMotionCapability::kCartesian:
      return capabilities.cartesian_path;
  }
  return false;
}

void add_violation(PlannerConformanceReport& report,
                   const std::string& case_id,
                   std::string invariant,
                   std::string detail) {
  report.violations.push_back(PlannerConformanceViolation{
      case_id, std::move(invariant), std::move(detail)});
}

bool finite_pose(const Pose3DTO& pose) noexcept {
  return std::isfinite(pose.position_m.x_m) &&
         std::isfinite(pose.position_m.y_m) &&
         std::isfinite(pose.position_m.z_m) &&
         std::isfinite(pose.orientation.x) &&
         std::isfinite(pose.orientation.y) &&
         std::isfinite(pose.orientation.z) &&
         std::isfinite(pose.orientation.w);
}

double translation_distance(const Pose3DTO& lhs,
                            const Pose3DTO& rhs) noexcept {
  return std::hypot(
      std::hypot(lhs.position_m.x_m - rhs.position_m.x_m,
                 lhs.position_m.y_m - rhs.position_m.y_m),
      lhs.position_m.z_m - rhs.position_m.z_m);
}

double rotation_distance(const Pose3DTO& lhs, const Pose3DTO& rhs) noexcept {
  const double lhs_norm = std::hypot(
      std::hypot(lhs.orientation.x, lhs.orientation.y),
      std::hypot(lhs.orientation.z, lhs.orientation.w));
  const double rhs_norm = std::hypot(
      std::hypot(rhs.orientation.x, rhs.orientation.y),
      std::hypot(rhs.orientation.z, rhs.orientation.w));
  if (!std::isfinite(lhs_norm) || !std::isfinite(rhs_norm) ||
      lhs_norm <= std::numeric_limits<double>::min() ||
      rhs_norm <= std::numeric_limits<double>::min()) {
    return std::numeric_limits<double>::infinity();
  }
  const double dot =
      (lhs.orientation.x * rhs.orientation.x +
       lhs.orientation.y * rhs.orientation.y +
       lhs.orientation.z * rhs.orientation.z +
       lhs.orientation.w * rhs.orientation.w) /
      (lhs_norm * rhs_norm);
  return 2.0 * std::acos(std::clamp(std::abs(dot), 0.0, 1.0));
}

std::optional<std::map<std::string, std::size_t>> unique_name_index(
    const std::vector<std::string>& names) {
  std::map<std::string, std::size_t> index;
  for (std::size_t position = 0; position < names.size(); ++position) {
    if (names[position].empty() ||
        !index.emplace(names[position], position).second) {
      return std::nullopt;
    }
  }
  return index;
}

void validate_structured_error(const std::string& case_id,
                               const ErrorDTO& error,
                               PlannerConformanceReport& report) {
  if (error.code.empty()) {
    add_violation(report, case_id, "error.code", "error code is empty");
  }
  if (error.message.empty()) {
    add_violation(report, case_id, "error.message", "error message is empty");
  }
  const auto reason = error.details.find("reason");
  if (reason == error.details.end() || reason->second.empty()) {
    add_violation(report, case_id, "error.reason",
                  "structured error has no non-empty reason detail");
  }
}

void validate_unsupported_error(const std::string& case_id,
                                const PlanResultDTO& result,
                                const std::string& capability,
                                PlannerConformanceReport& report) {
  if (result.trajectory.has_value() || !result.error.has_value()) {
    add_violation(report, case_id, "capability.rejection",
                  "unsupported capability did not return error-only result");
    return;
  }
  validate_structured_error(case_id, *result.error, report);
  if (result.error->code != kErrorUnsupportedCapability) {
    add_violation(report, case_id, "capability.error_code",
                  "expected unsupported_capability, got " +
                      result.error->code);
  }
  const auto found = result.error->details.find("capability");
  if (found == result.error->details.end() || found->second != capability) {
    add_violation(report, case_id, "capability.detail",
                  "rejection does not identify " + capability);
  }
}

void validate_pose_goal(const std::string& case_id,
                        const PlanRequestDTO& request,
                        const TrajectoryDTO& trajectory,
                        const Pose3DTO& expected,
                        const PlannerForwardKinematics& fk,
                        PlannerConformanceReport& report) {
  if (!fk || trajectory.waypoints.empty()) {
    add_violation(report, case_id, "goal.pose_verification",
                  "forward kinematics or trajectory is unavailable");
    return;
  }
  RobotStateDTO final_state;
  final_state.joint_names = trajectory.joint_names;
  final_state.positions_rad = trajectory.waypoints.back().positions_rad;
  const auto actual = fk(
      FkRequestDTO{final_state, compiled_frame_name(request.tcp_frame),
                   compiled_frame_name(request.base_frame)});
  if (!actual.ok()) {
    add_violation(report, case_id, "goal.pose_fk",
                  "forward kinematics failed: " + actual.error().message);
    return;
  }
  const double translation_error = translation_distance(actual.value(), expected);
  const double rotation_error = rotation_distance(actual.value(), expected);
  if (translation_error > request.tolerances.translation_tol_m +
                              kNumericEpsilon) {
    add_violation(report, case_id, "goal.translation_tolerance",
                  "translation error " + std::to_string(translation_error));
  }
  if (rotation_error > request.tolerances.rotation_tol_rad + kNumericEpsilon) {
    add_violation(report, case_id, "goal.rotation_tolerance",
                  "rotation error " + std::to_string(rotation_error));
  }
}

void validate_cartesian_samples(const std::string& case_id,
                                const PlanRequestDTO& request,
                                const TrajectoryDTO& trajectory,
                                const CartesianPathDTO& path,
                                const PlannerForwardKinematics& fk,
                                PlannerConformanceReport& report) {
  if (path.waypoints.empty()) {
    add_violation(report, case_id, "cartesian.request_path",
                  "Cartesian path is empty");
    return;
  }
  validate_pose_goal(case_id, request, trajectory, path.waypoints.back(), fk,
                     report);
  if (!fk || trajectory.waypoints.empty()) {
    return;
  }

  std::vector<Pose3DTO> actual_poses;
  actual_poses.reserve(trajectory.waypoints.size());
  for (const auto& waypoint : trajectory.waypoints) {
    RobotStateDTO state;
    state.joint_names = trajectory.joint_names;
    state.positions_rad = waypoint.positions_rad;
    const auto pose =
        fk(FkRequestDTO{state, compiled_frame_name(request.tcp_frame),
                        compiled_frame_name(request.base_frame)});
    if (!pose.ok() || !finite_pose(pose.value())) {
      add_violation(report, case_id, "cartesian.sample_fk",
                    pose.ok() ? "sample FK returned non-finite pose"
                              : "sample FK failed: " + pose.error().message);
      return;
    }
    actual_poses.push_back(pose.value());
  }

  // Every authored waypoint must be represented in chronological order. This
  // is the planner-neutral sampled-path check; adapters additionally qualify
  // their own between-sample/continuous fidelity certificate.
  std::size_t search_from = 0;
  for (std::size_t requested_index = 0;
       requested_index < path.waypoints.size(); ++requested_index) {
    bool matched = false;
    for (std::size_t actual_index = search_from;
         actual_index < actual_poses.size(); ++actual_index) {
      if (translation_distance(actual_poses[actual_index],
                               path.waypoints[requested_index]) <=
              request.tolerances.translation_tol_m + kNumericEpsilon &&
          rotation_distance(actual_poses[actual_index],
                            path.waypoints[requested_index]) <=
              request.tolerances.rotation_tol_rad + kNumericEpsilon) {
        search_from = actual_index;
        matched = true;
        break;
      }
    }
    if (!matched) {
      add_violation(report, case_id, "cartesian.ordered_waypoint_fidelity",
                    "authored waypoint " +
                        std::to_string(requested_index) +
                        " was not reached in order");
    }
  }
}

void validate_trajectory(const PlannerConformanceCase& test_case,
                         const TrajectoryDTO& trajectory,
                         const PlannerBackendConformanceInput& input,
                         PlannerConformanceReport& report) {
  const auto& request = test_case.request;
  if (trajectory.joint_names.empty()) {
    add_violation(report, test_case.id, "trajectory.joint_names",
                  "trajectory has no joints");
    return;
  }
  const auto trajectory_index = unique_name_index(trajectory.joint_names);
  const auto start_index = unique_name_index(request.start_state.joint_names);
  const auto start_positions =
      angular_position_projection(request.start_state);
  if (!trajectory_index.has_value()) {
    add_violation(report, test_case.id, "trajectory.joint_names",
                  "trajectory joint names are empty or duplicated");
    return;
  }
  if (!start_index.has_value() || !start_positions.ok() ||
      start_positions.value().size() !=
          request.start_state.joint_names.size()) {
    add_violation(report, test_case.id, "request.start_state",
                  "request start state is malformed");
    return;
  }
  if (trajectory.resources != request.resources) {
    add_violation(report, test_case.id, "trajectory.resource_projection",
                  "trajectory resources differ from the resolved request");
  }
  if (trajectory_index->size() != start_index->size()) {
    add_violation(report, test_case.id, "model.canonical_joint_set",
                  "trajectory joint count differs from canonical start state");
  }
  for (const auto& [name, unused] : *start_index) {
    (void)unused;
    if (!trajectory_index->contains(name)) {
      add_violation(report, test_case.id, "model.canonical_joint_set",
                    "trajectory leaked or omitted canonical joint " + name);
    }
  }
  if (trajectory.waypoints.size() < 2U ||
      trajectory.waypoints.size() > input.max_waypoint_count) {
    add_violation(report, test_case.id, "trajectory.output_size",
                  "waypoint count is outside [2, " +
                      std::to_string(input.max_waypoint_count) + "]");
    return;
  }

  double previous_time = -1.0;
  for (std::size_t index = 0; index < trajectory.waypoints.size(); ++index) {
    const auto& waypoint = trajectory.waypoints[index];
    if (!std::isfinite(waypoint.time_from_start_s) ||
        waypoint.time_from_start_s < 0.0 ||
        waypoint.time_from_start_s <= previous_time) {
      add_violation(report, test_case.id, "trajectory.monotonic_time",
                    "invalid time at waypoint " + std::to_string(index));
    }
    if (index == 0U && std::abs(waypoint.time_from_start_s) > kNumericEpsilon) {
      add_violation(report, test_case.id, "trajectory.start_time",
                    "first waypoint does not start at zero");
    }
    if (waypoint.positions_rad.size() != trajectory.joint_names.size() ||
        !std::all_of(waypoint.positions_rad.begin(),
                     waypoint.positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      add_violation(report, test_case.id, "trajectory.positions",
                    "position shape/value failure at waypoint " +
                        std::to_string(index));
    }
    const auto check_optional_dimension = [&](const std::vector<double>& values,
                                              const char* dimension) {
      if ((!values.empty() && values.size() != trajectory.joint_names.size()) ||
          !std::all_of(values.begin(), values.end(), [](double value) {
            return std::isfinite(value);
          })) {
        add_violation(report, test_case.id,
                      std::string("trajectory.") + dimension,
                      std::string(dimension) +
                          " shape/value failure at waypoint " +
                          std::to_string(index));
      }
    };
    check_optional_dimension(waypoint.velocities_rad_s, "velocities");
    check_optional_dimension(waypoint.accelerations_rad_s2, "accelerations");
    previous_time = waypoint.time_from_start_s;
  }

  const auto& first = trajectory.waypoints.front();
  if (first.positions_rad.size() == trajectory.joint_names.size()) {
    for (const auto& [name, start_position] : *start_index) {
      const auto found = trajectory_index->find(name);
      if (found == trajectory_index->end()) {
        continue;
      }
      if (std::abs(first.positions_rad[found->second] -
                   start_positions.value()[start_position]) >
          request.tolerances.joint_position_tol_rad + kNumericEpsilon) {
        add_violation(report, test_case.id, "trajectory.start_state",
                      "trajectory start differs for joint " + name);
      }
    }
  }

  if (const auto* goal =
          std::get_if<ResolvedJointGoalDTO>(&request.motion)) {
    const auto goal_index = unique_name_index(goal->joint_names);
    if (!goal_index.has_value() ||
        goal->positions_rad.size() != goal->joint_names.size()) {
      add_violation(report, test_case.id, "goal.joint_shape",
                    "joint goal is malformed");
      return;
    }
    const auto& final_positions = trajectory.waypoints.back().positions_rad;
    for (const auto& [name, goal_position] : *goal_index) {
      const auto found = trajectory_index->find(name);
      if (found == trajectory_index->end() ||
          found->second >= final_positions.size()) {
        add_violation(report, test_case.id, "goal.joint_mapping",
                      "goal joint is absent from trajectory: " + name);
        continue;
      }
      if (std::abs(final_positions[found->second] -
                   goal->positions_rad[goal_position]) >
          request.tolerances.joint_position_tol_rad + kNumericEpsilon) {
        add_violation(report, test_case.id, "goal.joint_tolerance",
                      "final position is outside tolerance for " + name);
      }
    }
  } else if (const auto* goal = std::get_if<PoseGoalDTO>(&request.motion)) {
    validate_pose_goal(test_case.id, request, trajectory, goal->goal,
                       input.forward_kinematics, report);
  } else if (const auto* path =
                 std::get_if<CartesianPathDTO>(&request.motion)) {
    validate_cartesian_samples(test_case.id, request, trajectory, *path,
                               input.forward_kinematics, report);
  }
}

void validate_plan_case(const PlannerConformanceCase& test_case,
                        const PlannerBackendConformanceInput& input,
                        PlannerConformanceReport& report) {
  const auto capability = motion_capability(test_case.request.motion);
  if (!capability.has_value()) {
    add_violation(report, test_case.id, "request.motion_type",
                  "case is not joint, pose, or Cartesian");
    return;
  }
  const bool motion_supported =
      supports_motion(input.capabilities, *capability);
  const auto started = std::chrono::steady_clock::now();
  const PlanResultDTO result = input.planner->plan(test_case.request);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  if (elapsed > input.max_plan_wall_time) {
    add_violation(report, test_case.id, "planning.wall_time_bound",
                  "plan() exceeded harness bound: " +
                      std::to_string(elapsed.count()) + " ms");
  }

  if (!motion_supported) {
    validate_unsupported_error(test_case.id, result,
                               motion_capability_key(*capability), report);
    return;
  }
  if (!result.trajectory.has_value() || result.error.has_value()) {
    if (result.error.has_value()) {
      validate_structured_error(test_case.id, *result.error, report);
      add_violation(report, test_case.id, "planning.required_success",
                    "supported case failed with " + result.error->code);
    } else {
      add_violation(report, test_case.id, "planning.result_shape",
                    "supported case returned neither trajectory nor error");
    }
    return;
  }
  if (result.projection_revision !=
      test_case.request.projection_binding.projection_revision) {
    add_violation(
        report, test_case.id, "projection.exact_revision_acknowledgement",
        "planner result did not acknowledge the retained composite revision");
  }
  validate_trajectory(test_case, *result.trajectory, input, report);
}

void validate_matrix_completeness(const PlannerBackendConformanceInput& input,
                                  PlannerConformanceReport& report) {
  std::set<std::string> seen;
  for (const auto& test_case : input.plan_cases) {
    const auto motion = motion_capability(test_case.request.motion);
    if (!motion.has_value()) {
      continue;
    }
    const std::string key = motion_name(*motion);
    if (!seen.insert(key).second) {
      add_violation(report, test_case.id, "matrix.unique_case",
                    "duplicate portable matrix cell " + key);
    }
  }
  for (const auto motion : {PlannerMotionCapability::kJoint,
                            PlannerMotionCapability::kPose,
                            PlannerMotionCapability::kCartesian}) {
    const std::string key = motion_name(motion);
    if (!seen.contains(key)) {
      add_violation(report, "matrix", "matrix.required_case",
                    "missing portable matrix cell " + key);
    }
  }
}

void validate_expected_failure(const PlannerExpectedFailureCase& test_case,
                               IPlannerBackend& planner,
                               PlannerConformanceReport& report) {
  const auto result = planner.plan(test_case.request);
  if (result.trajectory.has_value() || !result.error.has_value()) {
    add_violation(report, test_case.id, "failure.result_shape",
                  "expected failure did not return error-only result");
    return;
  }
  validate_structured_error(test_case.id, *result.error, report);
  if (result.error->code != test_case.expected_code) {
    add_violation(report, test_case.id, "failure.error_code",
                  "expected " + test_case.expected_code + ", got " +
                      result.error->code);
  }
  const auto reason = result.error->details.find("reason");
  if (reason == result.error->details.end() ||
      reason->second != test_case.expected_reason) {
    add_violation(report, test_case.id, "failure.error_reason",
                  "expected reason " + test_case.expected_reason);
  }
}

void validate_ik(const PlannerIkConformanceCase& test_case,
                 const PlannerBackendConformanceInput& input,
                 PlannerConformanceReport& report) {
  const auto result = input.planner->solve_ik(test_case.request);
  if (!input.capabilities.single_shot_ik) {
    if (result.ok || !result.error.has_value()) {
      add_violation(report, test_case.id, "ik.capability_rejection",
                    "unsupported IK did not return an error");
      return;
    }
    validate_structured_error(test_case.id, *result.error, report);
    if (result.error->code != kErrorUnsupportedCapability ||
        !result.error->details.contains("capability") ||
        result.error->details.at("capability") !=
            "planning.ik.single_shot") {
      add_violation(report, test_case.id, "ik.capability_rejection",
                    "IK rejection lacks portable capability identity");
    }
    return;
  }
  if (!result.ok || result.error.has_value()) {
    if (result.error.has_value()) {
      validate_structured_error(test_case.id, *result.error, report);
    }
    add_violation(report, test_case.id, "ik.required_success",
                  "advertised single-shot IK failed");
    return;
  }
  if (result.joint_names.empty() ||
      result.resources != test_case.request.resources ||
      result.positions_rad.size() != result.joint_names.size() ||
      !unique_name_index(result.joint_names).has_value() ||
      !std::all_of(result.positions_rad.begin(), result.positions_rad.end(),
                   [](double value) { return std::isfinite(value); })) {
    add_violation(report, test_case.id, "ik.result_shape",
                  "IK result is malformed");
    return;
  }
  if (!test_case.request.seed_state.joint_names.empty()) {
    const auto canonical_seed_names =
        unique_name_index(test_case.request.seed_state.joint_names);
    const auto result_names = unique_name_index(result.joint_names);
    if (!canonical_seed_names.has_value() || !result_names.has_value() ||
        canonical_seed_names->size() != result_names->size()) {
      add_violation(report, test_case.id, "ik.canonical_joint_set",
                    "IK result differs from the canonical seed joint set");
    } else {
      for (const auto& [name, unused] : *canonical_seed_names) {
        (void)unused;
        if (!result_names->contains(name)) {
          add_violation(report, test_case.id, "ik.canonical_joint_set",
                        "IK result leaked or omitted canonical joint " + name);
        }
      }
    }
  }
  if (input.forward_kinematics) {
    RobotStateDTO state{result.joint_names, result.positions_rad, {}};
    const auto pose = input.forward_kinematics(FkRequestDTO{
        state, compiled_frame_name(test_case.request.tcp_frame),
        compiled_frame_name(test_case.request.base_frame)});
    if (!pose.ok()) {
      add_violation(report, test_case.id, "ik.fk_verification",
                    "FK verification failed: " + pose.error().message);
    } else if (translation_distance(pose.value(),
                                    test_case.request.goal_pose) > 1e-3 ||
               rotation_distance(pose.value(),
                                 test_case.request.goal_pose) > 1e-2) {
      add_violation(report, test_case.id, "ik.goal_tolerance",
                    "IK result does not reproduce the requested pose");
    }
  }
}

void validate_cancellation(
    const PlannerCancellationConformanceCase& test_case,
    IPlannerBackend& default_planner,
    PlannerConformanceReport& report) {
  IPlannerBackend& planner =
      test_case.planner != nullptr ? *test_case.planner : default_planner;
  auto future = std::async(std::launch::async,
                           [&planner, &test_case] {
                             return planner.plan(test_case.request);
                           });
  if (test_case.wait_until_active) {
    if (!test_case.wait_until_active(test_case.completion_timeout)) {
      planner.request_cancel();
      add_violation(report, test_case.id, "cancellation.active_gate",
                    "planner did not reach its active-work gate");
    }
  } else if (test_case.settle_before_cancel.count() > 0) {
    std::this_thread::sleep_for(test_case.settle_before_cancel);
  }
  if (future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::timeout) {
    add_violation(report, test_case.id, "cancellation.in_flight",
                  "plan() completed before cancellation could be exercised");
    (void)future.get();
    return;
  }

  const auto cancelled_at = std::chrono::steady_clock::now();
  planner.request_cancel();
  if (future.wait_for(test_case.completion_timeout) !=
      std::future_status::ready) {
    // C++ cannot safely abandon a thread that still references the adapter
    // fixture. Fail this isolated test process immediately instead of hanging
    // until the outer CTest timeout or detaching into destroyed state.
    planner.request_cancel();
    std::cerr << "[" << test_case.id
              << "] cancellation.bounded_completion: plan() did not "
                 "terminate within "
              << test_case.completion_timeout.count() << " ms\n"
              << std::flush;
    std::_Exit(EXIT_FAILURE);
  }
  const auto result = future.get();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - cancelled_at);
  if (elapsed > test_case.completion_timeout) {
    add_violation(report, test_case.id, "cancellation.latency",
                  "cancellation took " + std::to_string(elapsed.count()) +
                      " ms");
  }
  if (result.trajectory.has_value() || !result.error.has_value()) {
    add_violation(report, test_case.id, "cancellation.result_shape",
                  "cancelled plan did not return error-only result");
    return;
  }
  validate_structured_error(test_case.id, *result.error, report);
  if (result.error->details.find("reason") == result.error->details.end() ||
      result.error->details.at("reason") !=
          "planning_cancelled_by_interrupt") {
    add_violation(report, test_case.id, "cancellation.error_reason",
                  "cancelled plan lacks stable cancellation reason");
  }
}

}  // namespace

std::vector<PlannerConformanceCase> make_portable_planner_matrix(
    const PlanRequestDTO& joint_request,
    const PlanRequestDTO& pose_request,
    const PlanRequestDTO& cartesian_request) {
  const std::array<std::pair<PlannerMotionCapability, const PlanRequestDTO*>, 3>
      requests{{{PlannerMotionCapability::kJoint, &joint_request},
                {PlannerMotionCapability::kPose, &pose_request},
                {PlannerMotionCapability::kCartesian, &cartesian_request}}};
  std::vector<PlannerConformanceCase> cases;
  cases.reserve(requests.size());
  for (const auto& [motion, request] : requests) {
    const std::string case_id = motion_name(motion);
    PlannerConformanceCase test_case{case_id, *request};
    test_case.request.move_id =
        MoveId(request->move_id.value() + "_" + case_id);
    cases.push_back(std::move(test_case));
  }
  return cases;
}

std::string PlannerConformanceReport::summary() const {
  std::ostringstream out;
  out << completed_cases.size() << " cases completed, " << violations.size()
      << " violations";
  for (const auto& violation : violations) {
    out << "\n[" << violation.case_id << "] " << violation.invariant << ": "
        << violation.detail;
  }
  return out.str();
}

PlannerConformanceReport run_planner_backend_conformance(
    const PlannerBackendConformanceInput& input) {
  PlannerConformanceReport report;
  if (input.adapter_id.empty()) {
    add_violation(report, "harness", "adapter.identity",
                  "adapter id is empty");
  }
  if (input.planner == nullptr) {
    add_violation(report, "harness", "adapter.instance",
                  "planner instance is null");
    return report;
  }
  validate_matrix_completeness(input, report);
  for (const auto& test_case : input.plan_cases) {
    validate_plan_case(test_case, input, report);
    report.completed_cases.push_back(test_case.id);
  }
  for (const auto& test_case : input.expected_failures) {
    validate_expected_failure(test_case, *input.planner, report);
    report.completed_cases.push_back(test_case.id);
  }
  if (input.ik_case.has_value()) {
    validate_ik(*input.ik_case, input, report);
    report.completed_cases.push_back(input.ik_case->id);
  }
  if (input.cancellation_case.has_value()) {
    validate_cancellation(*input.cancellation_case, *input.planner, report);
    report.completed_cases.push_back(input.cancellation_case->id);
  }
  return report;
}

}  // namespace manyforge::core::testing
