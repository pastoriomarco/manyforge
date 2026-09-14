#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include "affine_path_stop_internal.hpp"

namespace manyforge::core::detail {
namespace {

using Controls = std::array<double, 6>;

bool at_least(double lower, double required) {
  // Only floating-point roundoff, not a configurable motion allowance. Even
  // a constant-speed Hermite curve can have derivative controls one ulp apart.
  const double roundoff =
      64.0 * std::numeric_limits<double>::epsilon() * std::max(std::abs(lower), std::abs(required));
  return std::isfinite(lower) && lower >= required - roundoff;
}

bool same_derivative(double value, double expected, double segment_scale) {
  // Near a zero crossing, use the same derivative's two endpoint magnitudes.
  // No position, unrelated joint, or unit-size floor supplies an allowance.
  return std::isfinite(value) && std::isfinite(expected) &&
         std::abs(value - expected) <=
             512.0 * std::numeric_limits<double>::epsilon() *
                 std::max({std::abs(value), std::abs(expected), segment_scale});
}

// Bernstein control points bound the complete polynomial by their convex hull.
// Subdivision tightens a bound on a requested interval without sampling it.
std::pair<Controls, Controls> split(Controls values, unsigned degree, double r) {
  Controls left{}, right{};
  left[0] = values[0];
  right[degree] = values[degree];
  for (unsigned level = 1; level <= degree; ++level) {
    for (unsigned i = 0; i <= degree - level; ++i)
      values[i] = (1.0 - r) * values[i] + r * values[i + 1];
    left[level] = values[0];
    right[degree - level] = values[degree - level];
  }
  return {left, right};
}

Controls position_controls(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                           size_t joint, double origin) {
  const double h = b.time_from_start_s - a.time_from_start_s;
  const double p = a.positions_rad[joint] - origin, q = b.positions_rad[joint] - origin;
  const double v = h * a.velocities_rad_s[joint], w = h * b.velocities_rad_s[joint];
  const double aa = h * h * a.accelerations_rad_s2[joint];
  const double ab = h * h * b.accelerations_rad_s2[joint];
  return {p, p + v / 5.0, p + 2.0 * v / 5.0 + aa / 20.0, q - 2.0 * w / 5.0 + ab / 20.0, q - w / 5.0,
          q};
}

struct Segment {
  double start, duration;
  Controls position;

  double value(double r) const { return split(position, 5, r).first[5]; }

  double lower_derivative(unsigned order, double begin, double end) const {
    Controls values = position;
    unsigned degree = 5;
    for (unsigned derivative = 0; derivative < order; ++derivative, --degree)
      for (unsigned i = 0; i < degree; ++i)
        values[i] = degree * (values[i + 1] - values[i]) / duration;
    if (end < 1.0) values = split(values, degree, end).first;
    if (begin > 0.0) values = split(values, degree, begin / end).second;
    for (unsigned i = 0; i <= degree; ++i)
      if (!std::isfinite(values[i])) return std::numeric_limits<double>::quiet_NaN();
    return *std::min_element(values.begin(), values.begin() + degree + 1);
  }
};

struct State {
  double position, velocity, acceleration;
  State advance(double jerk, double t) const {
    return {position + velocity * t + acceleration * t * t / 2.0 + jerk * t * t * t / 6.0,
            velocity + acceleration * t + jerk * t * t / 2.0, acceleration + jerk * t};
  }
};

}  // namespace

std::optional<JerkLimitedStopProfileDTO> affine_path_stop_candidate(
    const TrajectoryDTO& accepted, double cursor, double rate, double rate_accel, double duration,
    const std::vector<double>& max_accel, const std::vector<double>& max_jerk) {
  const double max_path_rate = accepted.runtime_scale_bounds
                                   ? accepted.runtime_scale_bounds->max_stop_path_rate
                                   : 1.0;
  if (!std::isfinite(max_path_rate) || max_path_rate < 1.0) return std::nullopt;
  const auto initial = sample_trajectory(accepted, cursor);
  if (!initial.ok() || !initial.value().segment_index) return std::nullopt;
  const auto& observed = initial.value();
  const size_t count = accepted.joint_names.size();
  const auto driver = static_cast<size_t>(
      std::max_element(observed.velocities_rad_s.begin(), observed.velocities_rad_s.end(),
                       [](double a, double b) { return std::abs(a) < std::abs(b); }) -
      observed.velocities_rad_s.begin());
  const double tangent = std::abs(observed.velocities_rad_s[driver]);
  if (!std::isfinite(tangent) || tangent <= 1e-12) return std::nullopt;
  const double sign = std::copysign(1.0, observed.velocities_rad_s[driver]);
  const double source_accel = sign * observed.accelerations_rad_s2[driver];
  const State start{0.0, tangent * rate, source_accel * rate * rate + tangent * rate_accel};
  if (!std::isfinite(start.acceleration) || start.velocity <= 0.0 ||
      start.velocity / max_path_rate > tangent ||
      (start.acceleration >= 0.0 &&
       start.acceleration / max_path_rate / max_path_rate > source_accel))
    return std::nullopt;

  std::vector<double> direction(count);
  const auto& first = accepted.waypoints[*observed.segment_index];
  const auto& next = accepted.waypoints[*observed.segment_index + 1];
  if (first.velocities_rad_s.size() != count || next.velocities_rad_s.size() != count)
    return std::nullopt;
  const auto& reference =
      std::abs(first.velocities_rad_s[driver]) > std::abs(next.velocities_rad_s[driver]) ? first
                                                                                         : next;
  double accel_limit = std::numeric_limits<double>::infinity();
  double jerk_limit = std::numeric_limits<double>::infinity();
  for (size_t j = 0; j < count; ++j) {
    direction[j] = reference.velocities_rad_s[driver] != 0.0
                       ? reference.velocities_rad_s[j] / (sign * reference.velocities_rad_s[driver])
                       : observed.velocities_rad_s[j] / tangent;
    if (direction[j] != 0.0) {
      accel_limit = std::min(accel_limit, max_accel[j] / std::abs(direction[j]));
      jerk_limit = std::min(jerk_limit, max_jerk[j] / std::abs(direction[j]));
    }
  }
  const double neutral_time = std::abs(start.acceleration) / jerk_limit;
  const double brake_time = duration - neutral_time;
  if (!std::isfinite(jerk_limit) || !std::isfinite(brake_time) || brake_time <= 1e-9 ||
      std::abs(start.acceleration) > accel_limit || (neutral_time > 0.0 && neutral_time <= 1e-9))
    return std::nullopt;
  auto neutral = start.advance(-std::copysign(jerk_limit, start.acceleration), neutral_time);
  neutral.acceleration = 0.0;
  if (!(neutral.velocity > 0.0)) return std::nullopt;
  const double brake_jerk = 4.0 * neutral.velocity / (brake_time * brake_time);
  const double brake_accel = 2.0 * neutral.velocity / brake_time;
  if (!std::isfinite(brake_jerk) || brake_jerk > jerk_limit || brake_accel > accel_limit)
    return std::nullopt;
  const auto middle = neutral.advance(-brake_jerk, brake_time / 2.0);
  auto terminal = middle.advance(brake_jerk, brake_time / 2.0);
  terminal.velocity = terminal.acceleration = 0.0;
  if (!std::isfinite(terminal.position)) return std::nullopt;

  // Qualify the actual Hermite curve, not merely collinear waypoint positions.
  // Relative controls avoid cancellation from an unrelated world/joint origin.
  std::vector<Segment> segments;
  for (size_t i = *observed.segment_index; i + 1 < accepted.waypoints.size(); ++i) {
    const auto& a = accepted.waypoints[i];
    const auto& b = accepted.waypoints[i + 1];
    if (a.velocities_rad_s.size() != count || b.velocities_rad_s.size() != count ||
        a.accelerations_rad_s2.size() != count || b.accelerations_rad_s2.size() != count)
      return std::nullopt;
    Segment segment{a.time_from_start_s, b.time_from_start_s - a.time_from_start_s,
                    position_controls(a, b, driver, observed.positions_rad[driver])};
    for (auto& value : segment.position) value *= sign;
    for (size_t j = 0; j < count; ++j) {
      // A small position residual is not enough: a tiny acceleration bump on
      // an otherwise stationary joint must not be stretched into a long move.
      const double velocity_scale =
          std::max(std::abs(a.velocities_rad_s[j]), std::abs(b.velocities_rad_s[j]));
      const double acceleration_scale =
          std::max(std::abs(a.accelerations_rad_s2[j]), std::abs(b.accelerations_rad_s2[j]));
      if (!same_derivative(a.velocities_rad_s[j], direction[j] * sign * a.velocities_rad_s[driver],
                           velocity_scale) ||
          !same_derivative(b.velocities_rad_s[j], direction[j] * sign * b.velocities_rad_s[driver],
                           velocity_scale) ||
          !same_derivative(a.accelerations_rad_s2[j],
                           direction[j] * sign * a.accelerations_rad_s2[driver],
                           acceleration_scale) ||
          !same_derivative(b.accelerations_rad_s2[j],
                           direction[j] * sign * b.accelerations_rad_s2[driver],
                           acceleration_scale))
        return std::nullopt;
      if (direction[j] == 0.0 && (a.positions_rad[j] != first.positions_rad[j] ||
                                  b.positions_rad[j] != first.positions_rad[j]))
        return std::nullopt;
      const auto controls = position_controls(a, b, j, observed.positions_rad[j]);
      const double position_roundoff =
          64.0 * std::numeric_limits<double>::epsilon() *
          std::max({std::abs(a.positions_rad[j]), std::abs(b.positions_rad[j]),
                    std::abs(observed.positions_rad[j]),
                    std::abs(direction[j]) * std::max({std::abs(a.positions_rad[driver]),
                                                       std::abs(b.positions_rad[driver]),
                                                       std::abs(observed.positions_rad[driver])})});
      for (size_t k = 0; k < controls.size(); ++k)
        if (!std::isfinite(controls[k]) || !std::isfinite(segment.position[k]) ||
            std::abs(controls[k] - direction[j] * segment.position[k]) > position_roundoff)
          return std::nullopt;
    }
    const double begin = std::clamp((cursor - segment.start) / segment.duration, 0.0, 1.0);
    // Nonnegative derivative controls, with positive net displacement, prove
    // monotonicity for inversion; isolated boundary zeros are harmless here.
    if (!(segment.lower_derivative(1, begin, 1.0) >= 0.0) ||
        segment.position[5] <= segment.value(begin))
      return std::nullopt;
    segments.push_back(segment);
    if (segment.position[5] >= terminal.position) break;
  }
  if (segments.empty() || segments.back().position[5] < terminal.position) return std::nullopt;
  const auto inverse = [&](double position) -> std::optional<std::pair<double, double>> {
    if (position == 0.0) return std::pair{cursor, cursor};
    if (!std::isfinite(position)) return std::nullopt;
    const auto it = std::lower_bound(segments.begin(), segments.end(), position,
                                     [](const Segment& s, double x) { return s.position[5] < x; });
    if (it == segments.end()) return std::nullopt;
    const auto& segment = *it;
    double lo = std::clamp((cursor - segment.start) / segment.duration, 0.0, 1.0), hi = 1.0;
    for (unsigned i = 0; i < 48; ++i) {
      const double mid = (lo + hi) / 2.0;
      if (segment.value(mid) < position)
        lo = mid;
      else
        hi = mid;
    }
    return std::pair{segment.start + segment.duration * lo, segment.start + segment.duration * hi};
  };
  const auto neutral_range = inverse(neutral.position), stop_range = inverse(terminal.position);
  if (!neutral_range || !stop_range) return std::nullopt;
  const double stop_cursor = (stop_range->first + stop_range->second) / 2.0;

  // The simple speed bound can take over before neutralization finishes.
  // This avoids following an inherited nominal jerk spike once the accepted
  // path is already faster than the maximum speed of the entire Stop.
  double speed_suffix = neutral_range->first;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    const double begin = std::clamp((cursor - it->start) / it->duration, 0.0, 1.0);
    const double end = std::clamp((stop_range->second - it->start) / it->duration, 0.0, 1.0);
    if (end <= begin) continue;
    if (!at_least(it->lower_derivative(1, begin, end), neutral.velocity / max_path_rate)) break;
    speed_suffix = std::min(speed_suffix, std::max(cursor, it->start));
  }

  // Continuous same-path speed certificate. During -J neutralization, the
  // inequalities M*f'(u)>=y' and M²*f''(u)>=y'' are invariant when
  // f'''(u)>=-J/M³: on their boundaries eV'=eA and
  // eA'=M²*f'''*u'+J>=0 for 0<=u'<=M. M is the prepared joint/TCP velocity
  // certificate (legacy curves grant only 1), not the live velocity slider.
  // Thereafter y'<=v_peak; a lower bound f'>=v_peak/M suffices. This covers the
  // initial equality without an inverse-polynomial or sampled-rate shortcut.
  for (const auto& segment : segments) {
    const auto normalized = [&](double u) {
      return std::clamp((u - segment.start) / segment.duration, 0.0, 1.0);
    };
    const double begin = normalized(cursor), end = normalized(stop_range->second);
    if (end <= begin) continue;
    if (!(segment.lower_derivative(1, begin, end) > 0.0)) return std::nullopt;
    // Negative incoming acceleration is neutralized with +J, then braking
    // continues from the lower speed. The trip velocity bounds all phases;
    // prove it against the whole accepted path, without the +acceleration
    // prefix argument below (which applies only to -J neutralization).
    if (start.acceleration < 0.0) {
      if (!at_least(segment.lower_derivative(1, begin, end), start.velocity / max_path_rate))
        return std::nullopt;
      continue;
    }
    // The inverse bracket deliberately overlaps the two proofs: no sliver
    // near neutralization or the terminal position is certified by midpoint.
    const double prefix_end =
        normalized(speed_suffix < neutral_range->first ? speed_suffix : neutral_range->second);
    const double brake_begin = normalized(speed_suffix);
    if (prefix_end > begin &&
        !at_least(segment.lower_derivative(3, begin, std::min(prefix_end, end)),
                  -jerk_limit / max_path_rate / max_path_rate / max_path_rate))
      return std::nullopt;
    if (end > brake_begin &&
        !at_least(segment.lower_derivative(1, std::max(begin, brake_begin), end),
                  neutral.velocity / max_path_rate))
      return std::nullopt;
  }

  JerkLimitedStopProfileDTO candidate;
  candidate.achieved_stop_time_s = duration;
  candidate.reaches_rest_on_path = candidate.hits_target_time = candidate.terminal_state_zero =
      true;
  candidate.emitted_trajectory.joint_names = accepted.joint_names;
  const auto append = [&](double time, const State& state) {
    const auto range = inverse(state.position);
    if (!range) return false;
    const double u = (range->first + range->second) / 2.0;
    const auto sampled = sample_trajectory(accepted, u);
    if (!sampled.ok()) return false;
    const auto& path = sampled.value();
    const double residual =
        sign * (path.positions_rad[driver] - observed.positions_rad[driver]) - state.position;
    const double inverse_roundoff =
        64.0 * std::numeric_limits<double>::epsilon() *
        std::max({std::abs(observed.positions_rad[driver]), std::abs(segments.back().position[5]),
                  std::abs(state.position)});
    if (!std::isfinite(residual) || std::abs(residual) > inverse_roundoff) return false;
    const double qp = sign * path.velocities_rad_s[driver];
    if (!(qp > 0.0)) return false;
    const double path_rate = state.velocity / qp;
    const double path_accel =
        (state.acceleration - sign * path.accelerations_rad_s2[driver] * path_rate * path_rate) /
        qp;
    if (!std::isfinite(u) || u < cursor || u > stop_range->second || !std::isfinite(path_rate) ||
        path_rate < 0.0 || !at_least(max_path_rate, path_rate) || !std::isfinite(path_accel))
      return false;
    candidate.samples.push_back({time, u, path_rate, path_accel});
    TrajectoryWaypointDTO point{time, {}, {}, {}};
    for (size_t j = 0; j < count; ++j) {
      point.positions_rad.push_back(observed.positions_rad[j] + direction[j] * state.position);
      point.velocities_rad_s.push_back(direction[j] * state.velocity);
      point.accelerations_rad_s2.push_back(direction[j] * state.acceleration);
      if (time == 0.0) {
        point.velocities_rad_s.back() = observed.velocities_rad_s[j] * rate;
        point.accelerations_rad_s2.back() = observed.accelerations_rad_s2[j] * rate * rate +
                                            observed.velocities_rad_s[j] * rate_accel;
      }
    }
    candidate.emitted_trajectory.waypoints.push_back(std::move(point));
    return true;
  };
  // Constant-jerk phases need only their endpoints: the controller's P/V/A
  // quintic exactly reproduces each cubic. Never omit a jerk-switch knot.
  if (!append(0.0, start) || (neutral_time > 0.0 && !append(neutral_time, neutral)) ||
      !append(neutral_time + brake_time / 2.0, middle) || !append(duration, terminal))
    return std::nullopt;
  candidate.samples.front() = {0.0, cursor, rate, rate_accel};
  candidate.samples.back() = {duration, stop_cursor, 0.0, 0.0};
  return candidate;
}

}  // namespace manyforge::core::detail
