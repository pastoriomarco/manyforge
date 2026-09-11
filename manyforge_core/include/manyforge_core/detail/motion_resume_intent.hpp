#pragma once

#include <algorithm>
#include <cmath>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <optional>
#include <stdexcept>

namespace manyforge::core::detail {

// Semantic continuation only: no old start state, scene lease or dispatch
// authority. The caller retains the exact owner/attempt/trajectory provenance.
struct MotionResumeIntent {
  ResolvedMotionSpecDTO motion;
  std::size_t next_cartesian_waypoint{};
};

inline Result<MotionResumeIntent> capture_motion_resume_intent(
    const ResolvedMotionSpecDTO& motion, const TrajectoryDTO& trajectory,
    std::optional<double> original_cursor_s) {
  const auto* path = std::get_if<CartesianPathDTO>(&motion);
  if (!path) return Result<MotionResumeIntent>::Ok({motion, 0U});
  const auto& milestones = trajectory.cartesian_waypoint_times_s;
  if (!original_cursor_s || !std::isfinite(*original_cursor_s) || *original_cursor_s < 0.0 ||
      milestones.size() < 2U || milestones.size() > 4096U ||
      milestones.size() != path->waypoints.size() || trajectory.waypoints.empty() ||
      trajectory.waypoints.front().time_from_start_s != 0.0 || milestones.front() != 0.0 ||
      milestones.back() != trajectory.waypoints.back().time_from_start_s ||
      *original_cursor_s > milestones.back() ||
      !std::is_sorted(milestones.begin(), milestones.end()) ||
      !std::all_of(milestones.begin(), milestones.end(),
                   [](double time) { return std::isfinite(time) && time >= 0.0; })) {
    return Result<MotionResumeIntent>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Cartesian continuation lacks exact original-path progress",
                 {{"reason", "cartesian_resume_progress_unavailable"}}});
  }
  // Consume reached intermediate visits, but an interrupted node retains its
  // final target until normal execution reports success. Repositioning after
  // Stop therefore plans back to that target, not to consumed intermediate ones.
  const auto next = std::upper_bound(milestones.begin() + 1, milestones.end(), *original_cursor_s);
  return Result<MotionResumeIntent>::Ok(
      {motion,
       std::min(static_cast<std::size_t>(next - milestones.begin()), path->waypoints.size() - 1U)});
}

inline ResolvedMotionSpecDTO resume_motion_from_current_tcp(const MotionResumeIntent& intent,
                                                            const Pose3DTO& current_tcp) {
  auto motion = intent.motion;
  if (auto* path = std::get_if<CartesianPathDTO>(&motion)) {
    if (intent.next_cartesian_waypoint == 0U ||
        intent.next_cartesian_waypoint >= path->waypoints.size())
      throw std::invalid_argument("Cartesian continuation was not captured from a valid source");
    path->waypoints.erase(path->waypoints.begin(),
                          path->waypoints.begin() + intent.next_cartesian_waypoint);
    path->waypoints.insert(path->waypoints.begin(), current_tcp);
  }
  return motion;
}

}  // namespace manyforge::core::detail
