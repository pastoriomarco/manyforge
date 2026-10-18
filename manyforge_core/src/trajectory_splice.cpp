#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/trajectory_splice.hpp>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

Result<TrajectoryDTO> rejected(std::string reason) {
  return Result<TrajectoryDTO>::Err(
      ErrorDTO{kErrorInvalidRequest,
               "Quintic trajectory splice cannot preserve its exact boundaries",
               {{"reason", std::move(reason)}}});
}

bool finite_values(const std::vector<double>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](double value) { return std::isfinite(value); });
}

bool complete_point(const TrajectoryWaypointDTO& point, std::size_t width) {
  return std::isfinite(point.time_from_start_s) && point.positions_rad.size() == width &&
         point.velocities_rad_s.size() == width && point.accelerations_rad_s2.size() == width &&
         finite_values(point.positions_rad) && finite_values(point.velocities_rad_s) &&
         finite_values(point.accelerations_rad_s2);
}

bool complete_curve(const TrajectoryDTO& curve) {
  const auto width = curve.joint_names.size();
  if (width == 0U || curve.resources.size() != width || curve.waypoints.size() < 2U) return false;
  for (std::size_t index = 0U; index < width; ++index) {
    if (curve.joint_names[index].empty() || curve.resources[index].empty() ||
        std::find(curve.joint_names.begin(), curve.joint_names.begin() + index,
                  curve.joint_names[index]) != curve.joint_names.begin() + index ||
        std::find(curve.resources.begin(), curve.resources.begin() + index,
                  curve.resources[index]) != curve.resources.begin() + index)
      return false;
  }
  for (std::size_t index = 0U; index < curve.waypoints.size(); ++index) {
    if (!complete_point(curve.waypoints[index], width)) return false;
    if (index != 0U) {
      const auto span =
          curve.waypoints[index].time_from_start_s - curve.waypoints[index - 1U].time_from_start_s;
      if (!std::isfinite(span) || !(span > 0.0)) return false;
    }
  }
  return true;
}

bool finite_sample(const TrajectorySampleResult& sample, std::size_t width) {
  return sample.positions_rad.size() == width && sample.velocities_rad_s.size() == width &&
         sample.accelerations_rad_s2.size() == width && finite_values(sample.positions_rad) &&
         finite_values(sample.velocities_rad_s) && finite_values(sample.accelerations_rad_s2);
}

std::vector<double> reordered(const std::vector<double>& values,
                              const std::vector<std::size_t>& order) {
  std::vector<double> result;
  result.reserve(order.size());
  for (const auto index : order) result.push_back(values[index]);
  return result;
}

// Translation must not pair unchanged P/V/A with a materially different
// interval. Cap permitted rounding by the existing Stop/wire 1 ns resolution
// AND a local-duration conditioning threshold. Scaling tolerance by absolute
// clock magnitude would wrongly admit large-offset duration distortion.
bool preserved_span(double source_span, double translated_span) {
  if (!std::isfinite(source_span) || !std::isfinite(translated_span) || !(source_span > 0.0) ||
      !(translated_span > 0.0))
    return false;
  const auto relative_roundoff = std::sqrt(std::numeric_limits<double>::epsilon());
  const auto tolerance = std::min(1e-9, relative_roundoff * source_span);
  return std::abs(translated_span - source_span) <= tolerance;
}

bool same_pva(const TrajectoryWaypointDTO& point, const TrajectoryWaypointDTO& expected) {
  return point.positions_rad == expected.positions_rad &&
         point.velocities_rad_s == expected.velocities_rad_s &&
         point.accelerations_rad_s2 == expected.accelerations_rad_s2;
}

const char* rebase_milestones(const TrajectoryDTO& source, double cut, double shift,
                              double translated_cut, double translated_end,
                              std::vector<double>& destination) {
  double previous = source.waypoints.front().time_from_start_s;
  for (const auto milestone : source.cartesian_waypoint_times_s) {
    if (!std::isfinite(milestone) || milestone < previous ||
        milestone > source.waypoints.back().time_from_start_s)
      return "trajectory_splice_milestones_invalid";
    if (milestone < cut) return "trajectory_splice_milestone_skipped";
    const auto translated = milestone == cut ? translated_cut : milestone + shift;
    if (!std::isfinite(translated) || translated < translated_cut || translated > translated_end ||
        (!destination.empty() && milestone > previous && translated <= destination.back()))
      return "trajectory_splice_arithmetic_unrepresentable";
    destination.push_back(translated);
    previous = milestone;
  }
  return nullptr;
}

}  // namespace

Result<TrajectoryDTO> build_quintic_trajectory_splice(const TrajectoryDTO& active,
                                                      const TrajectoryDTO& successor,
                                                      const double elapsed_path_s,
                                                      const double divergence_path_s,
                                                      const double successor_entry_path_s,
                                                      const double connector_duration_path_s) {
  if (!complete_curve(active) || !complete_curve(successor))
    return rejected("trajectory_splice_curve_invalid");
  if (!std::isfinite(elapsed_path_s) || !std::isfinite(divergence_path_s) ||
      !std::isfinite(successor_entry_path_s) || !std::isfinite(connector_duration_path_s) ||
      !(divergence_path_s > elapsed_path_s) || !(connector_duration_path_s > 0.0) ||
      divergence_path_s < active.waypoints.front().time_from_start_s ||
      divergence_path_s > active.waypoints.back().time_from_start_s ||
      successor_entry_path_s < successor.waypoints.front().time_from_start_s ||
      successor_entry_path_s > successor.waypoints.back().time_from_start_s)
    return rejected("trajectory_splice_time_invalid");

  const auto width = active.joint_names.size();
  if (successor.joint_names.size() != width) return rejected("trajectory_splice_binding_mismatch");
  std::vector<std::size_t> order;
  order.reserve(width);
  for (std::size_t index = 0U; index < width; ++index) {
    const auto found = std::find(successor.joint_names.begin(), successor.joint_names.end(),
                                 active.joint_names[index]);
    if (found == successor.joint_names.end()) return rejected("trajectory_splice_binding_mismatch");
    const auto source = static_cast<std::size_t>(found - successor.joint_names.begin());
    if (successor.resources[source] != active.resources[index])
      return rejected("trajectory_splice_binding_mismatch");
    order.push_back(source);
  }

  const auto connector_start = divergence_path_s - elapsed_path_s;
  const auto connector_end = connector_start + connector_duration_path_s;
  const auto suffix_shift = connector_end - successor_entry_path_s;
  const auto duration_squared = connector_duration_path_s * connector_duration_path_s;
  if (!std::isfinite(connector_start) || !std::isfinite(connector_end) ||
      !(connector_end > connector_start) || !std::isfinite(suffix_shift) ||
      !std::isfinite(duration_squared) || !(duration_squared > 0.0) ||
      !std::isfinite(1.0 / duration_squared))
    return rejected("trajectory_splice_arithmetic_unrepresentable");

  const auto inlet = sample_trajectory(active, divergence_path_s);
  const auto outlet = sample_trajectory(successor, successor_entry_path_s);
  if (!inlet.ok() || !outlet.ok() || !finite_sample(inlet.value(), width) ||
      !finite_sample(outlet.value(), width))
    return rejected("trajectory_splice_sample_unrepresentable");
  const TrajectoryWaypointDTO from{0.0, inlet.value().positions_rad, inlet.value().velocities_rad_s,
                                   inlet.value().accelerations_rad_s2};
  const TrajectoryWaypointDTO to{connector_duration_path_s,
                                 reordered(outlet.value().positions_rad, order),
                                 reordered(outlet.value().velocities_rad_s, order),
                                 reordered(outlet.value().accelerations_rad_s2, order)};
  // These are the scaled endpoint quantities used by the shared sampler.
  // Interior extrema/limits remain the final composite validator's job.
  for (std::size_t index = 0U; index < width; ++index) {
    if (!std::isfinite(to.positions_rad[index] - from.positions_rad[index]) ||
        !std::isfinite(connector_duration_path_s * from.velocities_rad_s[index]) ||
        !std::isfinite(connector_duration_path_s * to.velocities_rad_s[index]) ||
        !std::isfinite(duration_squared * from.accelerations_rad_s2[index]) ||
        !std::isfinite(duration_squared * to.accelerations_rad_s2[index]))
      return rejected("trajectory_splice_arithmetic_unrepresentable");
  }

  TrajectoryDTO continuation;
  continuation.resources = active.resources;
  continuation.joint_names = active.joint_names;
  continuation.waypoints = {from, to};
  std::size_t suffix_count = 0U;
  for (const auto& point : successor.waypoints) {
    if (point.time_from_start_s <= successor_entry_path_s) continue;
    const auto time =
        connector_duration_path_s + (point.time_from_start_s - successor_entry_path_s);
    continuation.waypoints.push_back({time, reordered(point.positions_rad, order),
                                      reordered(point.velocities_rad_s, order),
                                      reordered(point.accelerations_rad_s2, order)});
    ++suffix_count;
  }
  if (!complete_curve(continuation))
    return rejected("trajectory_splice_arithmetic_unrepresentable");

  auto joined = build_stop_replacement_join(active, elapsed_path_s, continuation, divergence_path_s,
                                            divergence_path_s);
  if (!joined.ok()) return rejected("trajectory_splice_join_failed");
  auto& result = joined.value();
  const auto prefix_count = static_cast<std::size_t>(std::count_if(
      active.waypoints.begin(), active.waypoints.end(),
      [&](const auto& point) { return point.time_from_start_s < divergence_path_s; }));
  // The Stop join intentionally coalesces sub-nanosecond cuts. This API must
  // refuse that case rather than silently move a caller's exact P/V/A seam.
  if (result.waypoints.size() != prefix_count + 2U + suffix_count ||
      result.waypoints[prefix_count].time_from_start_s != connector_start ||
      result.waypoints[prefix_count + 1U].time_from_start_s != connector_end ||
      !same_pva(result.waypoints[prefix_count], from) ||
      !same_pva(result.waypoints[prefix_count + 1U], to))
    return rejected("trajectory_splice_boundary_coalesced");

  // Express each retained suffix knot with the same single translation as
  // its milestones; never accumulate a clock shift or retime derivatives.
  auto destination = prefix_count + 2U;
  for (const auto& point : successor.waypoints)
    if (point.time_from_start_s > successor_entry_path_s)
      result.waypoints[destination++].time_from_start_s = point.time_from_start_s + suffix_shift;
  if (!complete_curve(result)) return rejected("trajectory_splice_arithmetic_unrepresentable");

  // Check every segment against its source interval, including both sampled
  // cuts. Comparing rounded endpoint expressions alone cannot detect this.
  for (std::size_t index = 1U; index < prefix_count; ++index)
    if (!preserved_span(active.waypoints[index].time_from_start_s -
                            active.waypoints[index - 1U].time_from_start_s,
                        result.waypoints[index].time_from_start_s -
                            result.waypoints[index - 1U].time_from_start_s))
      return rejected("trajectory_splice_time_precision_lost");
  if (prefix_count != 0U &&
      !preserved_span(divergence_path_s - active.waypoints[prefix_count - 1U].time_from_start_s,
                      connector_start - result.waypoints[prefix_count - 1U].time_from_start_s))
    return rejected("trajectory_splice_time_precision_lost");
  if (!preserved_span(connector_duration_path_s, connector_end - connector_start))
    return rejected("trajectory_splice_time_precision_lost");
  auto previous_source_time = successor_entry_path_s;
  auto previous_destination_time = connector_end;
  destination = prefix_count + 2U;
  for (const auto& point : successor.waypoints) {
    if (point.time_from_start_s <= successor_entry_path_s) continue;
    const auto translated_time = result.waypoints[destination++].time_from_start_s;
    if (!preserved_span(point.time_from_start_s - previous_source_time,
                        translated_time - previous_destination_time))
      return rejected("trajectory_splice_time_precision_lost");
    previous_source_time = point.time_from_start_s;
    previous_destination_time = translated_time;
  }

  if (const auto failure = rebase_milestones(
          successor, successor_entry_path_s, suffix_shift, connector_end,
          result.waypoints.back().time_from_start_s, result.cartesian_waypoint_times_s))
    return rejected(failure);
  return joined;
}

Result<TrajectoryDTO> slice_future_trajectory(const TrajectoryDTO& trajectory,
                                              const double start_path_s) {
  if (!complete_curve(trajectory)) return rejected("trajectory_splice_curve_invalid");
  if (!std::isfinite(start_path_s) ||
      start_path_s < trajectory.waypoints.front().time_from_start_s ||
      !(start_path_s < trajectory.waypoints.back().time_from_start_s))
    return rejected("trajectory_splice_time_invalid");
  const auto sample = sample_trajectory(trajectory, start_path_s);
  if (!sample.ok() || !finite_sample(sample.value(), trajectory.joint_names.size()))
    return rejected("trajectory_splice_sample_unrepresentable");

  TrajectoryDTO result;
  result.resources = trajectory.resources;
  result.joint_names = trajectory.joint_names;
  result.waypoints.push_back({0.0, sample.value().positions_rad, sample.value().velocities_rad_s,
                              sample.value().accelerations_rad_s2});
  auto previous_source_time = start_path_s;
  for (const auto& point : trajectory.waypoints) {
    if (point.time_from_start_s <= start_path_s) continue;
    auto retained = point;
    retained.time_from_start_s -= start_path_s;
    if (!preserved_span(point.time_from_start_s - previous_source_time,
                        retained.time_from_start_s - result.waypoints.back().time_from_start_s))
      return rejected("trajectory_splice_time_precision_lost");
    previous_source_time = point.time_from_start_s;
    result.waypoints.push_back(std::move(retained));
  }
  if (!complete_curve(result)) return rejected("trajectory_splice_arithmetic_unrepresentable");
  if (const auto failure = rebase_milestones(trajectory, start_path_s, -start_path_s, 0.0,
                                             result.waypoints.back().time_from_start_s,
                                             result.cartesian_waypoint_times_s))
    return rejected(failure);
  return Result<TrajectoryDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
