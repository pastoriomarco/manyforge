#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include <Eigen/Core>

#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_roboplan/roboplan_candidate_computer.hpp>

namespace manyforge::roboplan_backend::detail {

/// Largest direction change between consecutive samples of a rounded corner
/// (well under RoboPlan's 0.175 rad Bounded `corner_tolerance`, so the tool
/// runs through the arc without a stop). RoboPlan's Bounded resolution walks
/// the arc at the same turn per step (N18: r/20, within the decided r/10).
inline constexpr double kCornerArcMaxTurnRad = 0.05;

struct DensifiedCartesianPath final {
  std::vector<Eigen::Matrix4d> poses;
  /// Dense index standing for each source waypoint: the waypoint itself, or
  /// the middle of the arc that rounds its corner.
  std::vector<std::size_t> source_waypoint_dense_indices;
  /// Radius of the tightest rounded corner (+inf when no corner is rounded).
  double smallest_corner_radius_m{std::numeric_limits<double>::infinity()};
};

struct CartesianResolutionTolerances final {
  double translation_m{};
  double rotation_rad{};
};

/// N18: the position and orientation tolerances handed to RoboPlan's Bounded
/// resolution. RoboPlan walks each run between stops in one even step of half
/// its tolerances, so the node's tolerances are scaled down by one common
/// factor until that translation step is at most r * kCornerArcMaxTurnRad
/// (r/20) of the tightest rounded corner (the rotation step shrinks to
/// match). Unchanged without a rounded
/// corner, or when the node's step is already fine enough.
[[nodiscard]] CartesianResolutionTolerances cartesian_resolution_tolerances(
    const DensifiedCartesianPath& dense, double translation_tol_m,
    double rotation_tol_rad);

/// Samples the requested Cartesian path for RoboPlan (RP-2). Straight
/// segments are split by `max_translation_step_m` / `max_rotation_step_rad`
/// (positions linear, orientation slerp). An interior waypoint with a blend
/// radius r > 0 between two segments of non-zero length is rounded by the
/// circular arc tangent to both segments at distance d = min(r, half of each
/// adjacent segment) from the waypoint. Across the arc the orientation follows
/// a cubic Hermite in rotation-vector space about the waypoint orientation
/// that matches both segments' orientation and per-metre rotation rate at the
/// arc ends, so neither the direction nor the rotation rate jumps there. A
/// reversal (turn ~pi), a collinear waypoint or r = 0 keeps the exact corner.
[[nodiscard]] core::Result<DensifiedCartesianPath>
densify_cartesian_path(const core::CartesianPathDTO& path,
                       const RoboPlanCandidateControl& control);

}  // namespace manyforge::roboplan_backend::detail
