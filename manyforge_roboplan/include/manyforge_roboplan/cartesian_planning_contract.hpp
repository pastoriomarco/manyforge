#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <Eigen/Core>

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::roboplan_backend {

// Pure/bounded contract recovered from roboplan_cartesian_path_adapter.* and
// roboplan_cartesian_trajectory_dynamics.cpp at parent
// c4b075fa6dfb69e463c61f3efa8cfce8de681e8a (deleted by 0822218). The future
// I6 planning adapter must integrate these helpers. This header deliberately
// excludes the former request-planning body and therefore restores no scene,
// planner, or execution owner.
struct CartesianPlanningControl {
  std::function<bool()> cancellation_requested;
  std::function<bool()> deadline_expired;
};

struct CartesianPathFidelityReport {
  bool complete = false;
  double fraction_completed = 0.0;
  std::optional<std::size_t> first_failing_path_sample_index;
  double max_translation_error_m = 0.0;
  double max_rotation_error_rad = 0.0;
  std::size_t interval_checks = 0;
  std::optional<double> unresolved_rotation_error_upper_bound_rad;
};

CartesianPathFidelityReport verify_cartesian_path_fidelity(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad);

/// Cancellation/deadline-aware production form of the same bounded fidelity
/// proof.
core::Result<CartesianPathFidelityReport>
verify_cartesian_path_fidelity_controlled(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad,
    const CartesianPlanningControl& control);

core::ErrorDTO map_cartesian_backend_failure(
    std::string_view planner_error, double fraction_completed,
    std::optional<std::size_t> first_failing_waypoint_index);

}  // namespace manyforge::roboplan_backend
