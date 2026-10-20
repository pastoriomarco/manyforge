#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <sstream>
#include <utility>

namespace manyforge::core {
namespace {

ErrorDTO stop_error(std::string reason, std::string message,
                    std::map<std::string, std::string> details = {}) {
  details.insert_or_assign("reason", std::move(reason));
  return ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed), std::move(message),
                  std::move(details)};
}

std::string format(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

bool finite(double value) { return std::isfinite(value); }

/// Points closer than 1 ns are one point: the controller's wire time
/// resolution (`builtin_interfaces/Duration`), and no segment that short is
/// meaningful.
constexpr double kJoinTimeEpsilonS = 1e-9;

bool complete_waypoint(const TrajectoryWaypointDTO& waypoint, std::size_t joints) {
  const auto all_finite = [](const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(), [](double value) { return finite(value); });
  };
  return finite(waypoint.time_from_start_s) && waypoint.positions_rad.size() == joints &&
         waypoint.velocities_rad_s.size() == joints &&
         waypoint.accelerations_rad_s2.size() == joints && all_finite(waypoint.positions_rad) &&
         all_finite(waypoint.velocities_rad_s) && all_finite(waypoint.accelerations_rad_s2);
}

/// Complete derivatives and strictly increasing times.
bool complete_trajectory(const TrajectoryDTO& trajectory) {
  const auto joints = trajectory.joint_names.size();
  if (joints == 0U || trajectory.waypoints.empty()) return false;
  for (std::size_t index = 0U; index < trajectory.waypoints.size(); ++index) {
    if (!complete_waypoint(trajectory.waypoints[index], joints)) return false;
    if (index > 0U && !(trajectory.waypoints[index].time_from_start_s >
                        trajectory.waypoints[index - 1U].time_from_start_s)) {
      return false;
    }
  }
  return true;
}

/// Index map from `target` joint order into `source` joint order.
std::optional<std::vector<std::size_t>> joint_order(const std::vector<std::string>& source,
                                                    const std::vector<std::string>& target) {
  if (source.size() != target.size()) return std::nullopt;
  std::vector<std::size_t> order;
  order.reserve(target.size());
  for (const auto& name : target) {
    const auto found = std::find(source.begin(), source.end(), name);
    if (found == source.end() ||
        std::find(order.begin(), order.end(),
                  static_cast<std::size_t>(found - source.begin())) != order.end()) {
      return std::nullopt;
    }
    order.push_back(static_cast<std::size_t>(found - source.begin()));
  }
  return order;
}

std::vector<double> reorder(const std::vector<double>& values,
                            const std::vector<std::size_t>& order) {
  std::vector<double> result;
  result.reserve(order.size());
  for (const auto index : order) result.push_back(values[index]);
  return result;
}

}  // namespace

Result<double> compute_stop_join_elapsed_s(const StopJoinElapsedInputDTO& input) {
  if (!finite(input.sample_cursor_s) || !finite(input.frozen_factor) ||
      input.frozen_factor < 0.0 || input.frozen_factor > 1.0 ||
      !finite(input.wall_since_sample_s) || input.wall_since_sample_s < 0.0 ||
      !finite(input.start_allowance_s) || input.start_allowance_s < 0.0 ||
      (input.lead_in_end_path_s.has_value() && !finite(*input.lead_in_end_path_s))) {
    return Result<double>::Err(stop_error("stop_join_elapsed_input_invalid",
                                          "Stop join elapsed time has invalid inputs"));
  }
  const double elapsed =
      input.sample_cursor_s +
      input.frozen_factor * (input.wall_since_sample_s + input.start_allowance_s);
  if (input.lead_in_end_path_s.has_value() && !(elapsed < *input.lead_in_end_path_s)) {
    return Result<double>::Err(stop_error(
        "stop_delta_budget_exceeded",
        "The replacement would start at or after the end of the Stop lead-in",
        {{"recommended_disposition", "hard_stop"},
         {"elapsed_path_s", format(elapsed)},
         {"lead_in_end_path_s", format(*input.lead_in_end_path_s)},
         {"wall_since_sample_s", format(input.wall_since_sample_s)},
         {"start_allowance_s", format(input.start_allowance_s)}}));
  }
  return Result<double>::Ok(elapsed);
}

Result<StopReplacementStartDTO> compute_stop_replacement_start(
    const StopJoinElapsedInputDTO& input, const ExecutionControllerSampleDTO& sample,
    const ExecutionLaneFactsDTO& lane) {
  const auto legacy = compute_stop_join_elapsed_s(input);
  if (!legacy.ok()) return Result<StopReplacementStartDTO>::Err(legacy.error());
  if (!lane.supports_scheduled_replacement)
    return Result<StopReplacementStartDTO>::Ok({legacy.value(), std::nullopt});
  const auto invalid = [] {
    return Result<StopReplacementStartDTO>::Err(stop_error(
        "stop_replacement_start_invalid", "The associated controller start cannot be represented"));
  };
  if (!sample.binding_epoch.nonzero() || sample.source_stamp_ns <= 0 ||
      !finite(lane.controller_update_rate_hz) || lane.controller_update_rate_hz <= 0.0 ||
      (lane.supports_scale && (!sample.applied_factor ||
                              *sample.applied_factor != input.frozen_factor)))
    return invalid();
  // Match the controller's integer-nanosecond nominal period. Convert the
  // requested duration to wire nanoseconds BEFORE rounding to the grid, so
  // floating representation of an exact tick cannot add a spurious tick.
  const double period = 1e9 / lane.controller_update_rate_hz;
  const long double duration =
      (static_cast<long double>(input.wall_since_sample_s) + input.start_allowance_s) * 1e9L;
  const auto maximum = std::numeric_limits<TimePoint>::max();
  if (!finite(period) || period < 1.0 || static_cast<long double>(period) > maximum ||
      !std::isfinite(duration) || duration < 0.0L || duration > maximum - 0.5L)
    return invalid();
  const auto period_ns = static_cast<TimePoint>(period);
  const auto requested_ns = static_cast<TimePoint>(std::llround(duration));
  auto ticks = requested_ns / period_ns;
  if (requested_ns % period_ns != 0) ++ticks;
  if (ticks > (maximum - sample.source_stamp_ns) / period_ns) return invalid();
  const auto advance_ns = ticks * period_ns;
  auto aligned = input;
  aligned.wall_since_sample_s = static_cast<double>(advance_ns) / 1e9;
  aligned.start_allowance_s = 0.0;
  const auto elapsed = compute_stop_join_elapsed_s(aligned);
  if (!elapsed.ok()) return Result<StopReplacementStartDTO>::Err(elapsed.error());
  if (!finite(elapsed.value())) return invalid();
  return Result<StopReplacementStartDTO>::Ok(
      {elapsed.value(), ExecutionReplacementStartDTO{
                            sample.binding_epoch, sample.source_stamp_ns + advance_ns}});
}

Result<TrajectoryDTO> convert_stop_trajectory_to_path_time(const TrajectoryDTO& wall_time_trajectory,
                                                           const double frozen_factor) {
  if (!finite(frozen_factor) || frozen_factor <= 0.0 || frozen_factor > 1.0 ||
      !complete_trajectory(wall_time_trajectory)) {
    return Result<TrajectoryDTO>::Err(stop_error(
        "stop_path_time_conversion_invalid",
        "A Stop trajectory needs complete derivatives and a frozen factor in (0, 1]",
        {{"frozen_factor", format(frozen_factor)}}));
  }
  TrajectoryDTO path_time;
  path_time.resources = wall_time_trajectory.resources;
  path_time.joint_names = wall_time_trajectory.joint_names;
  path_time.waypoints.reserve(wall_time_trajectory.waypoints.size());
  const double f2 = frozen_factor * frozen_factor;
  for (const auto& waypoint : wall_time_trajectory.waypoints) {
    TrajectoryWaypointDTO converted{frozen_factor * waypoint.time_from_start_s,
                                    waypoint.positions_rad,
                                    waypoint.velocities_rad_s,
                                    waypoint.accelerations_rad_s2};
    for (auto& velocity : converted.velocities_rad_s) velocity /= frozen_factor;
    for (auto& acceleration : converted.accelerations_rad_s2) acceleration /= f2;
    path_time.waypoints.push_back(std::move(converted));
  }
  return Result<TrajectoryDTO>::Ok(std::move(path_time));
}

double original_path_cursor(const StopReplacementCursorMapDTO& map,
                            const double replacement_cursor_s) {
  const double active = replacement_cursor_s + map.elapsed_s;
  const auto& times = map.continuation_times_s;
  const auto& path = map.original_path_s;
  if (active < map.trip_path_s || times.empty() || times.size() != path.size()) return active;
  const double tau = active - map.continuation_origin_s;
  if (tau <= times.front()) return path.front();
  if (tau >= times.back()) return path.back();
  const auto upper = std::upper_bound(times.begin(), times.end(), tau);
  const auto index = static_cast<std::size_t>(upper - times.begin());
  const double span = times[index] - times[index - 1U];
  const double alpha = span > 0.0 ? (tau - times[index - 1U]) / span : 1.0;
  return path[index - 1U] + alpha * (path[index] - path[index - 1U]);
}

Result<TrajectoryDTO> build_stop_replacement_join(const TrajectoryDTO& active,
                                                  const double elapsed_s,
                                                  const TrajectoryDTO& continuation,
                                                  const double continuation_origin_s,
                                                  const double trip_path_s) {
  const auto order = joint_order(continuation.joint_names, active.joint_names);
  if (!finite(elapsed_s) || !finite(continuation_origin_s) || !finite(trip_path_s) ||
      trip_path_s < continuation_origin_s || !complete_trajectory(active) ||
      !complete_trajectory(continuation) || !order.has_value()) {
    return Result<TrajectoryDTO>::Err(stop_error(
        "stop_join_input_invalid",
        "The Stop join needs two complete trajectories over the same joints"));
  }
  // §4.2 steps 3-4: with the trip point still ahead, the original path itself
  // (its own waypoints and polynomials) runs up to the trip point.
  const bool trip_ahead = trip_path_s > elapsed_s;
  if (trip_ahead &&
      (trip_path_s < active.waypoints.front().time_from_start_s ||
       trip_path_s > active.waypoints.back().time_from_start_s + kJoinTimeEpsilonS)) {
    return Result<TrajectoryDTO>::Err(stop_error(
        "stop_join_input_invalid", "The Stop trip point lies outside the active trajectory",
        {{"trip_path_s", format(trip_path_s)},
         {"active_start_path_s", format(active.waypoints.front().time_from_start_s)},
         {"active_end_path_s", format(active.waypoints.back().time_from_start_s)}}));
  }
  TrajectoryDTO joined;
  joined.resources = active.resources;
  joined.joint_names = active.joint_names;
  // The active trajectory's own waypoints before the horizon, shifted so that
  // the reference time falls at 0 (negative times kept, never clamped).
  const double horizon = trip_ahead ? trip_path_s : elapsed_s;
  for (const auto& waypoint : active.waypoints) {
    if (!(waypoint.time_from_start_s < horizon)) break;
    auto kept = waypoint;
    kept.time_from_start_s -= elapsed_s;
    joined.waypoints.push_back(std::move(kept));
  }
  const double shift = continuation_origin_s - elapsed_s;
  // Continuation points kept: after the trip point when it is ahead, else at
  // or after the expected start.
  double first_kept_after = 0.0;
  bool inclusive = true;
  if (trip_ahead) {
    const double trip_time = trip_path_s - elapsed_s;
    if (joined.waypoints.empty() ||
        joined.waypoints.back().time_from_start_s < trip_time - kJoinTimeEpsilonS) {
      // The original's exact state at the trip point (an active waypoint just
      // after it is never kept, so none is duplicated).
      const auto state = sample_trajectory(active, trip_path_s);
      if (!state.ok()) {
        return Result<TrajectoryDTO>::Err(stop_error(
            "stop_join_input_invalid", "The Stop trip point cannot be sampled on the active path",
            {{"trip_path_s", format(trip_path_s)}, {"sampler_error", state.error().code}}));
      }
      joined.waypoints.push_back(TrajectoryWaypointDTO{trip_time, state.value().positions_rad,
                                                       state.value().velocities_rad_s,
                                                       state.value().accelerations_rad_s2});
    }
    // Otherwise the active waypoint within the tolerance is the trip point.
    first_kept_after = joined.waypoints.back().time_from_start_s + kJoinTimeEpsilonS;
    inclusive = false;
  }
  bool continued = false;
  for (const auto& waypoint : continuation.waypoints) {
    const double time = waypoint.time_from_start_s + shift;
    if (inclusive ? time < first_kept_after : !(time > first_kept_after)) continue;
    joined.waypoints.push_back(TrajectoryWaypointDTO{time, reorder(waypoint.positions_rad, *order),
                                                     reorder(waypoint.velocities_rad_s, *order),
                                                     reorder(waypoint.accelerations_rad_s2, *order)});
    continued = true;
  }
  if (!continued) {
    return Result<TrajectoryDTO>::Err(stop_error(
        "stop_join_continuation_elapsed",
        trip_ahead ? "The Stop continuation ends at or before the trip point"
                   : "The Stop continuation ends before the replacement's expected start",
        {{"recommended_disposition", "hard_stop"},
         {"elapsed_path_s", format(elapsed_s)},
         {"continuation_origin_path_s", format(continuation_origin_s)},
         {"trip_path_s", format(trip_path_s)}}));
  }
  return Result<TrajectoryDTO>::Ok(std::move(joined));
}

Result<ControlledHardStopBrakeDTO> plan_controlled_hard_stop_brake(
    const ControlledHardStopBrakeInputDTO& input) {
  const auto joints = input.joint_names.size();
  const auto invalid = [](std::string message, std::map<std::string, std::string> details = {}) {
    return Result<ControlledHardStopBrakeDTO>::Err(
        stop_error("controlled_hard_stop_brake_input_invalid", std::move(message),
                   std::move(details)));
  };
  if (joints == 0U || input.reference_positions_rad.size() != joints ||
      input.reference_path_velocities.size() != joints) {
    return invalid("Controlled hard stop needs one position and velocity per joint");
  }
  if (!finite(input.frozen_factor) || input.frozen_factor <= 0.0 || input.frozen_factor > 1.0) {
    return invalid("Controlled hard stop needs an applied factor in (0, 1]",
                   {{"frozen_factor", format(input.frozen_factor)}});
  }
  if (!finite(input.controller_period_s) || input.controller_period_s <= 0.0) {
    return invalid("Controlled hard stop needs a positive controller period");
  }
  const double f = input.frozen_factor;
  std::vector<double> speed(joints);  // physical start velocity
  double stop_time = 0.0;
  std::string limiting_joint;
  for (std::size_t joint = 0U; joint < joints; ++joint) {
    const auto& name = input.joint_names[joint];
    const auto limit = input.max_deceleration_rad_s2.find(name);
    if (limit == input.max_deceleration_rad_s2.end() || !finite(limit->second) ||
        limit->second <= 0.0) {
      return invalid("Controlled hard stop needs a positive deceleration limit per joint",
                     {{"joint", name}});
    }
    if (!finite(input.reference_positions_rad[joint]) ||
        !finite(input.reference_path_velocities[joint])) {
      return invalid("Controlled hard stop state must be finite", {{"joint", name}});
    }
    speed[joint] = f * input.reference_path_velocities[joint];
    const double joint_time = std::abs(speed[joint]) / limit->second;
    if (joint_time > stop_time) {
      stop_time = joint_time;
      limiting_joint = name;
    }
  }

  ControlledHardStopBrakeDTO brake;
  brake.trajectory.joint_names = input.joint_names;
  brake.stop_time_s = stop_time;
  brake.stop_path_time_s = f * stop_time;
  brake.limiting_joint = limiting_joint;
  if (stop_time == 0.0) {
    brake.trajectory.waypoints.push_back(TrajectoryWaypointDTO{
        0.0, input.reference_positions_rad, std::vector<double>(joints, 0.0),
        std::vector<double>(joints, 0.0)});
    return Result<ControlledHardStopBrakeDTO>::Ok(std::move(brake));
  }
  // Physical law per joint: q(t) = q0 + u (t - t^2 / (2T)), q'(t) = u (1 - t/T),
  // q''(t) = -u / T. Path time s = f t: dq/ds = q'/f, d2q/ds2 = q''/f^2.
  const auto point = [&](double t) {
    TrajectoryWaypointDTO waypoint{f * t, std::vector<double>(joints),
                                   std::vector<double>(joints), std::vector<double>(joints)};
    for (std::size_t joint = 0U; joint < joints; ++joint) {
      const double u = speed[joint];
      waypoint.positions_rad[joint] =
          input.reference_positions_rad[joint] + u * (t - t * t / (2.0 * stop_time));
      waypoint.velocities_rad_s[joint] = u * (1.0 - t / stop_time) / f;
      waypoint.accelerations_rad_s2[joint] = -u / stop_time / (f * f);
    }
    return waypoint;
  };
  const auto periods =
      static_cast<std::size_t>(std::ceil(stop_time / input.controller_period_s));
  for (std::size_t k = 0U; k < periods; ++k) {
    const double t = static_cast<double>(k) * input.controller_period_s;
    // Keep strictly increasing times: the stop point closes the brake.
    if (k > 0U && t >= stop_time * (1.0 - 1e-12)) break;
    brake.trajectory.waypoints.push_back(point(t));
  }
  auto stop = point(stop_time);
  std::fill(stop.velocities_rad_s.begin(), stop.velocities_rad_s.end(), 0.0);
  brake.trajectory.waypoints.push_back(std::move(stop));
  return Result<ControlledHardStopBrakeDTO>::Ok(std::move(brake));
}

}  // namespace manyforge::core
