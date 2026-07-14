#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <Eigen/Core>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>

#include <manyforge_core/types/plan_request.hpp>

namespace manyforge::roboplan_backend {

struct CartesianPlanningControl {
  std::function<bool()> cancellation_requested;
  std::function<bool()> deadline_expired;
};

/// Geometric conformance of a returned TCP path to the requested Cartesian
/// path. Both inputs are compared under a monotonic, normalized SE(3) path
/// parameter so a retimer may change speed but not path shape.
struct CartesianPathFidelityReport {
  bool complete = false;
  double fraction_completed = 0.0;
  std::optional<std::size_t> first_failing_path_sample_index;
  double max_translation_error_m = 0.0;
  double max_rotation_error_rad = 0.0;
  /// Number of adaptive interval certifications attempted. Exposed so tests
  /// and diagnostics can prove the verifier's bounded-work contract.
  std::size_t interval_checks = 0;
  /// Present only when bounded adaptive subdivision could not certify a
  /// rotational interval. This is an upper bound, not a sampled error.
  std::optional<double> unresolved_rotation_error_upper_bound_rad;
};

/// Verifies the complete path, including intermediate poses. This helper is
/// independent of RoboPlan's own success result so adapter conformance does
/// not trust the candidate producer to validate itself.
CartesianPathFidelityReport verify_cartesian_path_fidelity(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad);

/// Converts RoboPlan's current textual Cartesian failure surface into the
/// stable, backend-neutral ManyForge taxonomy. Kept as a named adapter seam so
/// upstream message changes fail focused contract tests instead of drifting
/// application-visible errors silently.
core::ErrorDTO map_cartesian_backend_failure(
    std::string_view planner_error, double fraction_completed,
    std::optional<std::size_t> first_failing_waypoint_index);

/// Maps ManyForge's planner-neutral Cartesian-path contract onto one
/// request-local RoboPlan 0.5 planner and verifies the returned trajectory.
/// This is NRT planning work; it never runs on a controller or runtime-safety
/// thread and never mutates the retained/live scene generation.
core::Result<roboplan::JointTrajectory> plan_cartesian_path(
    const core::PlanRequestDTO& request,
    const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& planning_scene,
    const CartesianPlanningControl& control);

}  // namespace manyforge::roboplan_backend
