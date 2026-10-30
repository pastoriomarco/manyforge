#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "soft_on_path_stop_planner_internal.hpp"

namespace manyforge::core {
namespace detail {

std::optional<double> spring_back_return_start_s(double forward_end_s, double phase_time_s,
                                                 double return_time_s, double deadline_s) {
  constexpr double kJoinEpsilon = 1e-9;
  if (!std::isfinite(forward_end_s) || !std::isfinite(phase_time_s) ||
      !std::isfinite(return_time_s) || !std::isfinite(deadline_s))
    return std::nullopt;
  // Early or on-time braking joins at the half boundary; late braking moves
  // the return after it, never overlapping the two halves.
  const double start_s = forward_end_s > phase_time_s + kJoinEpsilon ? forward_end_s : phase_time_s;
  if (start_s + return_time_s > deadline_s + kJoinEpsilon) return std::nullopt;
  return start_s;
}

// Jerk limits for one Stop phase (Q36, as main 1073-1086 and 1491-1499):
// an authored (deployment) jerk is used verbatim; a joint without one gets
// main's default from its acceleration limit and this phase's own duration
// (`reference_s`: soft ramp = deceleration time - lead-in, spring-back return
// = half the deceleration time).
StopPhaseJerkLimits stop_phase_jerk_limits(const TrajectoryDTO& trajectory,
                                           const std::map<std::string, double>& accel_limits,
                                           const SoftOnPathPlannerConfig& config,
                                           double reference_s) {
  StopPhaseJerkLimits out;
  if (!config.jerk_limits_enabled) {
    out.source = "disabled";
    return out;
  }
  bool any_default = false;
  bool any_deployment = false;
  for (const auto& joint_name : trajectory.joint_names) {
    const auto it = config.per_joint_max_jerk_rad_s3.find(joint_name);
    if (it != config.per_joint_max_jerk_rad_s3.end() && std::isfinite(it->second) &&
        it->second > 0.0) {
      out.limits[joint_name] = it->second;
      any_deployment = true;
    } else {
      out.limits[joint_name] = motion_jerk_preset_limit(accel_limits.at(joint_name), reference_s);
      any_default = true;
    }
  }
  out.source =
      any_default && any_deployment ? "mixed" : (any_deployment ? "deployment" : "default");
  return out;
}

}  // namespace detail

namespace {

constexpr double kEpsilon = 1e-9;

std::string format_double(double value) {
  // %.12g prints the same text as `ostringstream << setprecision(12)`
  // (defaultfloat) without the per-call stream construction.
  char buffer[32];
  const int written = std::snprintf(buffer, sizeof(buffer), "%.12g", value);
  return std::string(buffer, written > 0 ? static_cast<size_t>(written) : 0);
}

const char* source_string(SoftOnPathStateSource source) {
  switch (source) {
    case SoftOnPathStateSource::kMeasuredFeedback:
      return "measured_feedback";
    case SoftOnPathStateSource::kModelDerived:
      return "model_derived";
  }
  return "unknown";
}

template <typename T = SoftOnPathStopPlanDTO>
Result<T> err(std::string reason, std::string message,
              std::map<std::string, std::string> details = {}) {
  details["reason"] = reason;
  return Result<T>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
               std::move(message), std::move(details)});
}

Result<SoftOnPathStopPlanDTO> err_structured(std::string reason, std::string message,
                                             InterruptStructuredValue details) {
  details["reason"] = interrupt_structured_string(reason);
  return Result<SoftOnPathStopPlanDTO>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
               std::move(message), project_interrupt_structured_value_to_text(details)});
}

bool finite_positive(double value) {
  return std::isfinite(value) && value > 0.0;
}

std::optional<std::map<std::string, double>> angular_deceleration_limits(
    const MotionInterruptStopProfileDTO& profile) {
  std::map<std::string, double> limits;
  for (const auto& [joint, typed] : profile.per_joint_max_deceleration) {
    const auto* angular = std::get_if<AngularAccelerationDTO>(&typed);
    if (angular == nullptr || !finite_positive(angular->value_rad_s2)) {
      return std::nullopt;
    }
    limits.emplace(joint, angular->value_rad_s2);
  }
  return limits;
}

// Every trajectory a Stop is planned for carries its prepared time-law bounds
// (item 6): the spring-back return cap reads them.
std::optional<std::pair<std::string, std::string>> runtime_scale_bounds_error(
    const TrajectoryDTO& trajectory) {
  if (!trajectory.runtime_scale_bounds) {
    return std::pair<std::string, std::string>{
        "runtime_scale_bounds_required",
        "Stop planning requires the trajectory's prepared runtime_scale_bounds"};
  }
  const auto& bounds = *trajectory.runtime_scale_bounds;
  if (!finite_positive(bounds.max_rate_per_s) || !finite_positive(bounds.max_rate_change_per_s2) ||
      !std::isfinite(bounds.max_stop_path_rate) || bounds.max_stop_path_rate < 1.0) {
    return std::pair<std::string, std::string>{
        "runtime_scale_bounds_invalid",
        "Stop planning requires finite positive runtime_scale_bounds with max_stop_path_rate >= 1"};
  }
  return std::nullopt;
}

bool all_finite(const std::vector<double>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](double value) { return std::isfinite(value); });
}

double norm(const std::vector<double>& values) {
  double sum = 0.0;
  for (double value : values) {
    sum += value * value;
  }
  return std::sqrt(sum);
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double sum = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

std::vector<double> subtract(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<double> out(a.size(), 0.0);
  for (size_t i = 0; i < a.size(); ++i) {
    out[i] = a[i] - b[i];
  }
  return out;
}

std::optional<size_t> find_joint(const std::vector<std::string>& names, const std::string& target) {
  const auto it = std::find(names.begin(), names.end(), target);
  if (it == names.end()) {
    return std::nullopt;
  }
  return static_cast<size_t>(it - names.begin());
}

struct OrderedState {
  std::vector<double> positions;
  std::vector<double> velocities;
  bool has_velocity = false;
};

Result<OrderedState> order_state_for_trajectory(const RobotStateDTO& state,
                                                const std::vector<std::string>& joint_names) {
  if (joint_names.empty()) {
    return Result<OrderedState>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                              "soft_on_path trajectory has no joint names",
                                              {{"reason", "trajectory_joint_names_empty"}}});
  }
  OrderedState ordered;
  ordered.positions.resize(joint_names.size(), 0.0);
  ordered.velocities.resize(joint_names.size(), 0.0);

  if (state.joint_names.empty()) {
    if (state.positions_rad.size() != joint_names.size()) {
      return Result<OrderedState>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path observed position size does not match trajectory joints",
                   {{"reason", "observed_position_size_mismatch"},
                    {"joint_count", std::to_string(joint_names.size())},
                    {"position_count", std::to_string(state.positions_rad.size())}}});
    }
    ordered.positions = state.positions_rad;
    if (!all_finite(ordered.positions)) {
      return Result<OrderedState>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                                "soft_on_path observed positions must be finite",
                                                {{"reason", "observed_position_not_finite"}}});
    }
    if (state.velocities_rad_s.size() == joint_names.size()) {
      ordered.velocities = state.velocities_rad_s;
      if (!all_finite(ordered.velocities)) {
        return Result<OrderedState>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "soft_on_path observed velocities must be finite",
                     {{"reason", "observed_velocity_not_finite"}}});
      }
      ordered.has_velocity = true;
    }
    return Result<OrderedState>::Ok(std::move(ordered));
  }

  if (state.positions_rad.size() != state.joint_names.size()) {
    return Result<OrderedState>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "soft_on_path observed state joint/position sizes differ",
                 {{"reason", "observed_state_size_mismatch"},
                  {"state_joint_count", std::to_string(state.joint_names.size())},
                  {"position_count", std::to_string(state.positions_rad.size())}}});
  }
  const bool state_has_velocity = state.velocities_rad_s.size() == state.joint_names.size();
  for (size_t i = 0; i < joint_names.size(); ++i) {
    const auto idx = find_joint(state.joint_names, joint_names[i]);
    if (!idx.has_value()) {
      return Result<OrderedState>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path observed state is missing a trajectory joint",
                   {{"reason", "observed_state_missing_joint"}, {"joint", joint_names[i]}}});
    }
    ordered.positions[i] = state.positions_rad[*idx];
    if (!std::isfinite(ordered.positions[i])) {
      return Result<OrderedState>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path observed positions must be finite",
                   {{"reason", "observed_position_not_finite"}, {"joint", joint_names[i]}}});
    }
    if (state_has_velocity) {
      ordered.velocities[i] = state.velocities_rad_s[*idx];
      if (!std::isfinite(ordered.velocities[i])) {
        return Result<OrderedState>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "soft_on_path observed velocities must be finite",
                     {{"reason", "observed_velocity_not_finite"}, {"joint", joint_names[i]}}});
      }
    }
  }
  ordered.has_velocity = state_has_velocity;
  return Result<OrderedState>::Ok(std::move(ordered));
}

Result<void> validate_trajectory_shape(const TrajectoryDTO& trajectory) {
  if (trajectory.joint_names.empty()) {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                      "soft_on_path requires trajectory joint names",
                                      {{"reason", "trajectory_joint_names_empty"}}});
  }
  if (trajectory.waypoints.size() < 2) {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                      "soft_on_path requires at least two trajectory waypoints",
                                      {{"reason", "trajectory_too_short"}}});
  }
  double previous_time = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    const auto& wp = trajectory.waypoints[i];
    if (!std::isfinite(wp.time_from_start_s) || wp.time_from_start_s <= previous_time) {
      return Result<void>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path trajectory waypoint times must be finite and increasing",
                   {{"reason", "trajectory_time_not_strictly_increasing"},
                    {"waypoint_index", std::to_string(i)}}});
    }
    if (wp.positions_rad.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path waypoint position size does not match joint names",
                   {{"reason", "trajectory_position_size_mismatch"},
                    {"waypoint_index", std::to_string(i)}}});
    }
    if (!wp.velocities_rad_s.empty() &&
        wp.velocities_rad_s.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path waypoint velocity size does not match joint names",
                   {{"reason", "trajectory_velocity_size_mismatch"},
                    {"waypoint_index", std::to_string(i)}}});
    }
    if (!wp.accelerations_rad_s2.empty() &&
        wp.accelerations_rad_s2.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "soft_on_path waypoint acceleration size does not match joint names",
                   {{"reason", "trajectory_acceleration_size_mismatch"},
                    {"waypoint_index", std::to_string(i)}}});
    }
    // Non-finite values defeat every downstream limit check (NaN compares
    // false against any threshold), so a poisoned trajectory would pass the
    // accel/jerk gates and could reach dispatch. Reject it here instead.
    if (!all_finite(wp.positions_rad) || !all_finite(wp.velocities_rad_s) ||
        !all_finite(wp.accelerations_rad_s2)) {
      return Result<void>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path waypoint values must be finite",
          {{"reason", "trajectory_value_not_finite"}, {"waypoint_index", std::to_string(i)}}});
    }
    previous_time = wp.time_from_start_s;
  }
  return Result<void>::Ok();
}

struct PathSample {
  std::vector<double> position;
  std::vector<double> d_position_dt;
  std::vector<double> d2_position_dt2;
  std::vector<double> d3_position_dt3;
};

void path_waypoint(const PathSample& path, double time, double rate, double acceleration,
                   TrajectoryWaypointDTO& out) {
  out.time_from_start_s = time;
  out.positions_rad = path.position;
  out.velocities_rad_s.resize(path.position.size());
  out.accelerations_rad_s2.resize(path.position.size());
  for (size_t j = 0; j < path.position.size(); ++j) {
    out.velocities_rad_s[j] = path.d_position_dt[j] * rate;
    out.accelerations_rad_s2[j] =
        path.d2_position_dt2[j] * rate * rate + path.d_position_dt[j] * acceleration;
  }
}

// One-sided jerk of the shared Hermite interpolation, expressed in normalized
// segment time r. Its quadratic extrema also give a cheap continuous check of
// the emitted quintic, independent of the planner's integration/sample period.
struct SegmentJerk {
  double c0 = 0.0;
  double c1 = 0.0;
  double c2 = 0.0;
  double at(double r) const { return c0 + r * (c1 + r * c2); }
};

SegmentJerk segment_jerk(const TrajectoryWaypointDTO& left, const TrajectoryWaypointDTO& right,
                         size_t joint, unsigned degree) {
  const double span = right.time_from_start_s - left.time_from_start_s;
  const double delta = right.positions_rad[joint] - left.positions_rad[joint];
  const double v0 = span * left.velocities_rad_s[joint];
  const double v1 = span * right.velocities_rad_s[joint];
  const double span3 = span * span * span;
  if (degree == 3) return {(-12.0 * delta + 6.0 * (v0 + v1)) / span3, 0.0, 0.0};
  const double a0 = span * span * left.accelerations_rad_s2[joint];
  const double a1 = span * span * right.accelerations_rad_s2[joint];
  return {(60.0 * delta - 36.0 * v0 - 24.0 * v1 - 9.0 * a0 + 3.0 * a1) / span3,
          (-360.0 * delta + 192.0 * v0 + 168.0 * v1 + 36.0 * a0 - 24.0 * a1) / span3,
          (360.0 * delta - 180.0 * v0 - 180.0 * v1 - 30.0 * a0 + 30.0 * a1) / span3};
}

struct SegmentLimitFailure {
  const char* reason;
  size_t joint;
  double peak;
  double limit;

  bool is_jerk() const { return std::string_view(reason) == "interpolated_jerk_limit_exceeded"; }
  bool is_acceleration() const {
    return std::string_view(reason) == "interpolated_acceleration_limit_exceeded";
  }
};

std::optional<SegmentLimitFailure> segment_limit_failure(const TrajectoryWaypointDTO& left,
                                                         const TrajectoryWaypointDTO& right,
                                                         const std::vector<double>& max_accel,
                                                         const std::vector<double>& max_jerk) {
  const double span = right.time_from_start_s - left.time_from_start_s;
  constexpr double tolerance = 1e-6;
  const auto complete = [&](const auto& point) {
    return point.positions_rad.size() == max_accel.size() &&
           point.velocities_rad_s.size() == max_accel.size() &&
           point.accelerations_rad_s2.size() == max_accel.size();
  };
  if (!finite_positive(span) || !complete(left) || !complete(right) ||
      (!max_jerk.empty() && max_jerk.size() != max_accel.size()))
    return SegmentLimitFailure{"emitted_trajectory_sample_invalid", 0, 0.0, 0.0};
  for (size_t j = 0; j < max_accel.size(); ++j) {
    const auto jerk = segment_jerk(left, right, j, 5);
    if (!std::isfinite(jerk.c0) || !std::isfinite(jerk.c1) || !std::isfinite(jerk.c2))
      return SegmentLimitFailure{"emitted_trajectory_sample_invalid", j, 0.0, 0.0};
    // An absent table is genuine jerk-off, not an invented large ceiling.
    if (!max_jerk.empty()) {
      double peak_jerk = std::max(std::abs(jerk.at(0.0)), std::abs(jerk.at(1.0)));
      if (jerk.c2 != 0.0) {
        const double r = -0.5 * jerk.c1 / jerk.c2;
        if (r > 0.0 && r < 1.0) peak_jerk = std::max(peak_jerk, std::abs(jerk.at(r)));
      }
      if (!std::isfinite(peak_jerk) || peak_jerk > max_jerk[j] + tolerance)
        return SegmentLimitFailure{"interpolated_jerk_limit_exceeded", j, peak_jerk, max_jerk[j]};
    }

    double peak_accel =
        std::max(std::abs(left.accelerations_rad_s2[j]), std::abs(right.accelerations_rad_s2[j]));
    const auto check_accel = [&](double r) {
      if (r > 0.0 && r < 1.0) {
        const double a = left.accelerations_rad_s2[j] +
                         span * r * (jerk.c0 + r * (jerk.c1 / 2.0 + r * jerk.c2 / 3.0));
        peak_accel = std::isfinite(a) ? std::max(peak_accel, std::abs(a))
                                      : std::numeric_limits<double>::infinity();
      }
    };
    // Acceleration extrema are the real roots of jerk. Normalize before the
    // discriminant and use the stable quadratic form to avoid cancellation.
    const double scale = std::max({std::abs(jerk.c0), std::abs(jerk.c1), std::abs(jerk.c2)});
    if (scale > 0.0) {
      const double a = jerk.c2 / scale, b = jerk.c1 / scale, c = jerk.c0 / scale;
      if (a == 0.0) {
        if (b != 0.0) check_accel(-c / b);
      } else {
        const double disc = b * b - 4.0 * a * c;
        if (disc >= 0.0) {
          const double q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
          check_accel(q / a);
          if (q != 0.0) check_accel(c / q);
        }
      }
    }
    if (!std::isfinite(peak_accel) || peak_accel > max_accel[j] + tolerance)
      return SegmentLimitFailure{"interpolated_acceleration_limit_exceeded", j, peak_accel,
                                 max_accel[j]};
  }
  return std::nullopt;
}

PathSample sample_path(const TrajectoryDTO& trajectory, double time_s) {
  // Transitional shim: forward to the shared trajectory sampler and
  // adapt the result into the planner's local PathSample shape. The
  // shared sampler reproduces the per-segment polynomial math
  // previously implemented here (parity tests cover linear / cubic
  // Hermite / quintic equivalence on shared fixtures).
  //
  // Boundary semantics shift here in two narrow ways relative to the
  // previous implementation:
  //   - `time_s < t_first` and `time_s > t_last` now hold the boundary
  //     waypoint position with EXACTLY zero velocity and acceleration,
  //     rather than carrying the boundary waypoint's stored derivative
  //     values out into the clamp. Callers that sample only inside
  //     `[t_first, t_last]` (the planner's normal path-time loop) are
  //     unaffected.
  //   - `time_s == t_first` and `time_s == t_last` sample the adjacent
  //     segment polynomial at alpha=0 or alpha=1, which evaluates to
  //     the endpoint's stored velocity and acceleration via the
  //     polynomial basis. Behavior at the exact endpoints is therefore
  //     equivalent to the previous implementation.
  //
  // Structurally malformed trajectories trigger Result::Err on the
  // sampler. Planner callers feed only validated trajectories, so the
  // Err path is a defensive default that yields an empty sample.
  const size_t joint_count = trajectory.joint_names.size();
  PathSample sample;
  sample.position.assign(joint_count, 0.0);
  sample.d_position_dt.assign(joint_count, 0.0);
  sample.d2_position_dt2.assign(joint_count, 0.0);

  auto result = sample_trajectory(trajectory, time_s);
  if (!result.ok()) {
    return sample;
  }
  auto& value = result.value();
  if (value.positions_rad.size() == joint_count) {
    sample.position = std::move(value.positions_rad);
  }
  if (value.velocities_rad_s.size() == joint_count) {
    sample.d_position_dt = std::move(value.velocities_rad_s);
  }
  if (value.accelerations_rad_s2.size() == joint_count) {
    sample.d2_position_dt2 = std::move(value.accelerations_rad_s2);
  }
  return sample;
}

struct Projection {
  double time_from_start_s = 0.0;
  double distance_rad = 0.0;
};

Projection project_to_path(const TrajectoryDTO& trajectory, const std::vector<double>& q) {
  // Index-based scalar accumulation: same math and accumulation order as
  // the vector-temporary formulation, with zero heap traffic per segment.
  Projection best;
  best.distance_rad = std::numeric_limits<double>::infinity();
  const size_t n = q.size();
  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const auto& prev = trajectory.waypoints[i - 1];
    const auto& next = trajectory.waypoints[i];
    const size_t joint_count = std::min({n, prev.positions_rad.size(), next.positions_rad.size()});
    double denom = 0.0;
    double along = 0.0;
    for (size_t j = 0; j < joint_count; ++j) {
      const double seg = next.positions_rad[j] - prev.positions_rad[j];
      denom += seg * seg;
      along += (q[j] - prev.positions_rad[j]) * seg;
    }
    const double alpha = denom <= kEpsilon ? 0.0 : std::clamp(along / denom, 0.0, 1.0);
    double dist2 = 0.0;
    for (size_t j = 0; j < joint_count; ++j) {
      const double seg = next.positions_rad[j] - prev.positions_rad[j];
      const double d = q[j] - (prev.positions_rad[j] + alpha * seg);
      dist2 += d * d;
    }
    const double distance = std::sqrt(dist2);
    if (distance < best.distance_rad) {
      best.distance_rad = distance;
      best.time_from_start_s =
          prev.time_from_start_s + alpha * (next.time_from_start_s - prev.time_from_start_s);
    }
  }
  return best;
}

// Repeated-sampling cursor over an immutable trajectory: hoists the shared
// sampler's per-call structural validation to construction and reuses the
// sample buffers across calls (zero allocation in steady state). Sampling
// output is identical to sample_path(), including the zero-filled defensive
// sample for structurally invalid trajectories and non-finite times. A
// one-entry memo collapses consecutive identical-time samples (the ALI hold
// phase re-samples one frozen path time per hold waypoint).
class PathCursor {
 public:
  explicit PathCursor(const TrajectoryDTO& trajectory)
      : trajectory_(&trajectory),
        joint_count_(trajectory.joint_names.size()),
        prevalidated_(validate_trajectory_for_sampling(trajectory).ok()) {}

  void sample(double time_s, PathSample& out) {
    if (has_memo_ && memo_time_ == time_s) {
      out = memo_;
      return;
    }
    out.position.assign(joint_count_, 0.0);
    out.d_position_dt.assign(joint_count_, 0.0);
    out.d2_position_dt2.assign(joint_count_, 0.0);
    out.d3_position_dt3.assign(joint_count_, 0.0);
    bool sampled = false;
    if (prevalidated_) {
      sampled = sample_trajectory_prevalidated(*trajectory_, time_s, scratch_).ok();
    } else {
      auto result = sample_trajectory(*trajectory_, time_s);
      if (result.ok()) {
        scratch_ = std::move(result.value());
        sampled = true;
      }
    }
    if (sampled) {
      if (scratch_.positions_rad.size() == joint_count_) {
        out.position = scratch_.positions_rad;
      }
      if (scratch_.velocities_rad_s.size() == joint_count_) {
        out.d_position_dt = scratch_.velocities_rad_s;
      }
      if (scratch_.accelerations_rad_s2.size() == joint_count_) {
        out.d2_position_dt2 = scratch_.accelerations_rad_s2;
      }
      // Differentiate the same one-sided Hermite segment as the shared
      // sampler. Differencing acceleration across neighboring segments would
      // smear a knot and give the retimer the wrong instantaneous jerk budget.
      if (scratch_.segment_index && scratch_.degree >= 3) {
        const auto& left = trajectory_->waypoints[*scratch_.segment_index];
        const auto& right = trajectory_->waypoints[*scratch_.segment_index + 1];
        const double span = right.time_from_start_s - left.time_from_start_s;
        const double r = (time_s - left.time_from_start_s) / span;
        for (size_t j = 0; j < joint_count_; ++j) {
          out.d3_position_dt3[j] = segment_jerk(left, right, j, scratch_.degree).at(r);
        }
      }
    }
    memo_ = out;
    memo_time_ = time_s;
    has_memo_ = true;
  }

  PathSample sample(double time_s) {
    PathSample out;
    sample(time_s, out);
    return out;
  }

 private:
  const TrajectoryDTO* trajectory_;
  size_t joint_count_;
  bool prevalidated_;
  TrajectorySampleResult scratch_;
  PathSample memo_;
  double memo_time_ = std::numeric_limits<double>::quiet_NaN();
  bool has_memo_ = false;
};

// Time-scaling ramp factors for the stop, on the normalized ramp parameter
// r in [0, 1]. The ramp slows the path-time rate u̇ from path_time_rate
// (steady traversal) to 0:
//
//   u̇(r)  = path_time_rate · ramp_velocity_factor(r)
//   ü(r)  = (path_time_rate / ramp_s) · ramp_acceleration_factor(r)
//   u(r)  = handoff + path_time_rate · ramp_s · ramp_position_factor(r)
//
// where ramp_acceleration_factor = d/dr(ramp_velocity_factor) and
// ramp_position_factor = ∫ ramp_velocity_factor.
//
// The velocity factor is the minimum-jerk quintic v(r) = 1 − 10r³ + 15r⁴ − 6r⁵
// (= 1 − smootherstep(r)). Its boundary conditions give an
// acceleration-AND-jerk-continuous stop at both the seam and the terminus:
//
//   v(0) = 1, v(1) = 0                       full incoming rate → rest
//   v'(0) = v'(1) = 0   → accel = 0 at both ends (continuous with the steady
//                          ü = 0 before the seam; zero at rest)
//   v''(0) = v''(1) = 0 → jerk  = 0 at both ends (continuous with the steady
//                          u⃛ = 0 before the seam; zero at rest)
//
// The earlier cubic factor (1 − 3r² + 2r³) was acceleration-continuous but
// left a nonzero path-time jerk (v'' = −6 + 12r ≠ 0) at the endpoints — a jerk
// step at the preemption seam that propagated to every joint through the
// chain rule. The quintic removes it. ramp_position_factor(1) = 0.5 is
// unchanged and correct: a deceleration from full rate to rest covers half
// the constant-rate path distance.
double ramp_position_factor(double r) {
  // ∫₀ʳ (1 − 10s³ + 15s⁴ − 6s⁵) ds = r − 2.5r⁴ + 3r⁵ − r⁶
  const double r2 = r * r;
  const double r4 = r2 * r2;
  return r - 2.5 * r4 + 3.0 * r4 * r - r4 * r2;
}

double ramp_velocity_factor(double r) {
  // 1 − 10r³ + 15r⁴ − 6r⁵
  const double r2 = r * r;
  const double r3 = r2 * r;
  return 1.0 - 10.0 * r3 + 15.0 * r3 * r - 6.0 * r3 * r2;
}

double ramp_acceleration_factor(double r) {
  // d/dr(velocity factor) = −30r² + 60r³ − 30r⁴ = −30 r² (r − 1)²
  const double r2 = r * r;
  return -30.0 * r2 + 60.0 * r2 * r - 30.0 * r2 * r2;
}

// Rest-to-rest spring-back return over `duration_s` covering `distance`
// path seconds. `quintic` is main's minimum-jerk return (peak rate
// 1.875 * distance / duration). `capped` keeps the same target and duration
// but limits the path rate to `cap_rate`: a ramp up built from the Stop ramp
// factors, a cruise at the cap, and the mirrored ramp down.
struct ReturnProgress {
  double distance = 0.0;
  double rate = 0.0;
  double rate_accel = 0.0;
};

struct ReturnShape {
  double duration_s = 0.0;
  double distance = 0.0;
  double cap_rate = 0.0;
  double ramp_s = 0.0;
  double cruise_s = 0.0;

  ReturnProgress quintic(double tau) const {
    const double r = std::clamp(tau / duration_s, 0.0, 1.0);
    const double r2 = r * r;
    const double r3 = r2 * r;
    const double r4 = r3 * r;
    const double r5 = r4 * r;
    return {distance * (10.0 * r3 - 15.0 * r4 + 6.0 * r5),
            distance / duration_s * (30.0 * r2 - 60.0 * r3 + 30.0 * r4),
            distance / (duration_s * duration_s) * (60.0 * r - 180.0 * r2 + 120.0 * r3)};
  }

  ReturnProgress capped(double tau) const {
    if (tau <= ramp_s) {
      const double r = std::clamp(tau / ramp_s, 0.0, 1.0);
      return {cap_rate * ramp_s * (r - ramp_position_factor(r)),
              cap_rate * (1.0 - ramp_velocity_factor(r)),
              -cap_rate / ramp_s * ramp_acceleration_factor(r)};
    }
    if (tau <= ramp_s + cruise_s) {
      return {cap_rate * (0.5 * ramp_s + (tau - ramp_s)), cap_rate, 0.0};
    }
    const double down_s = duration_s - ramp_s - cruise_s;
    const double r = std::clamp((tau - ramp_s - cruise_s) / down_s, 0.0, 1.0);
    return {cap_rate * (0.5 * ramp_s + cruise_s + down_s * ramp_position_factor(r)),
            cap_rate * ramp_velocity_factor(r), cap_rate / down_s * ramp_acceleration_factor(r)};
  }
};

bool constant_jerk_rate_within_unit_range(double rate, double accel, double jerk, double duration) {
  const auto valid = [&](double time) {
    const double value = rate + time * (accel + 0.5 * jerk * time);
    return std::isfinite(value) && value >= -1e-12 && value <= 1.0 + 1e-12;
  };
  if (!std::isfinite(duration) || duration < 0.0 || !std::isfinite(accel) || !std::isfinite(jerk) ||
      !valid(0.0) || !valid(duration))
    return false;
  if (jerk == 0.0) return true;
  const double extremum = -accel / jerk;
  return extremum <= 0.0 || extremum >= duration || valid(extremum);
}

struct AliStopProfileDTO {
  std::vector<JerkLimitedStopSampleDTO> samples;
  double achieved_stop_time_s = 0.0;
  bool reaches_rest_on_path = false;
  bool hits_target_time = false;
  bool terminal_state_zero = false;
  bool within_budget = false;
  bool dispatchable = false;
  std::string validation_failure_reason;
  size_t backtracking_iterations = 0;
  double motion_stop_time_s = 0.0;
  // Derivative-complete joint trajectory built from `samples` during
  // validation (ramp-relative times, 1:1 with `samples`); reused by the
  // caller instead of resampling the path per sample.
  TrajectoryDTO emitted;
};

TrajectoryDTO build_path_rate_trajectory(PathCursor& cursor,
                                         const std::vector<std::string>& joint_names,
                                         const std::vector<JerkLimitedStopSampleDTO>& samples) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = joint_names;
  const size_t joint_count = joint_names.size();
  trajectory.waypoints.reserve(samples.size());
  PathSample path_sample;
  for (const auto& sample : samples) {
    cursor.sample(sample.path_time_s, path_sample);
    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = sample.time_from_ramp_start_s;
    wp.positions_rad = path_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = path_sample.d_position_dt[j] * sample.path_rate;
      wp.accelerations_rad_s2[j] =
          path_sample.d2_position_dt2[j] * sample.path_rate * sample.path_rate +
          path_sample.d_position_dt[j] * sample.path_rate_accel;
    }
    trajectory.waypoints.push_back(std::move(wp));
  }
  return trajectory;
}

void validate_path_rate_stop_profile(
    const TrajectoryDTO& accepted_trajectory, PathCursor& path_cursor,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3, double start_path_time_s,
    double start_path_rate, double start_path_rate_accel, double target_stop_time_s,
    double sample_period_s, AliStopProfileDTO& profile) {
  constexpr double kTerminalTolerance = 1e-3;
  const double target_time_tolerance_s = std::max(2.0 * sample_period_s, 1e-3);
  const size_t joint_count = accepted_trajectory.joint_names.size();
  auto mark_invalid = [&](const std::string& reason) {
    if (profile.validation_failure_reason.empty()) {
      profile.validation_failure_reason = reason;
    }
    profile.within_budget = false;
  };

  profile.hits_target_time =
      std::abs(profile.achieved_stop_time_s - target_stop_time_s) <= target_time_tolerance_s;
  profile.terminal_state_zero = false;
  profile.within_budget = profile.reaches_rest_on_path && profile.samples.size() >= 2;
  if (profile.samples.empty()) {
    profile.dispatchable = false;
    return;
  }

  const auto& first = profile.samples.front();
  if (std::abs(first.time_from_ramp_start_s) > kEpsilon ||
      std::abs(first.path_time_s - start_path_time_s) > 1e-7 ||
      std::abs(first.path_rate - start_path_rate) > 1e-7 ||
      std::abs(first.path_rate_accel - start_path_rate_accel) > 1e-7) {
    mark_invalid("seam_sample_mismatch");
  }
  for (size_t i = 1; i < profile.samples.size(); ++i) {
    const auto& prev = profile.samples[i - 1];
    const auto& curr = profile.samples[i];
    if (!std::isfinite(curr.time_from_ramp_start_s) || !std::isfinite(curr.path_time_s) ||
        !std::isfinite(curr.path_rate) || !std::isfinite(curr.path_rate_accel) ||
        curr.time_from_ramp_start_s <= prev.time_from_ramp_start_s + kEpsilon ||
        curr.path_time_s + 1e-8 < prev.path_time_s) {
      mark_invalid("sample_time_or_value_invalid");
    }
    if (accepted_trajectory.runtime_scale_bounds) {
      const double dt = curr.time_from_ramp_start_s - prev.time_from_ramp_start_s;
      if (curr.path_rate < -1e-12 || curr.path_rate > 1.0 + 1e-12 ||
          !constant_jerk_rate_within_unit_range(prev.path_rate, prev.path_rate_accel,
                                                (curr.path_rate_accel - prev.path_rate_accel) / dt,
                                                dt)) {
        mark_invalid("path_rate_limit_exceeded");
      }
    }
  }

  const auto& terminal = profile.samples.back();
  profile.terminal_state_zero = std::abs(terminal.path_rate) <= kTerminalTolerance &&
                                std::abs(terminal.path_rate_accel) <= kTerminalTolerance;
  if (!profile.terminal_state_zero) {
    mark_invalid("terminal_state_not_zero");
  }

  profile.emitted =
      build_path_rate_trajectory(path_cursor, accepted_trajectory.joint_names, profile.samples);
  const auto shape = validate_trajectory_shape(profile.emitted);
  if (!shape.ok()) {
    mark_invalid("emitted_trajectory_shape_invalid");
    profile.dispatchable = false;
    return;
  }

  // The same continuous per-segment check applies to every Stop candidate;
  // a sampling floor or averaged acceleration difference cannot prove jerk.
  std::vector<double> accel_limits(joint_count, 0.0);
  std::vector<double> jerk_limits_by_index(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    accel_limits[j] = per_joint_max_deceleration_rad_s2.at(accepted_trajectory.joint_names[j]);
    jerk_limits_by_index[j] = per_joint_max_jerk_rad_s3.at(accepted_trajectory.joint_names[j]);
  }

  const auto& points = profile.emitted.waypoints;
  for (size_t i = 1; i < points.size(); ++i)
    if (const auto failure =
            segment_limit_failure(points[i - 1], points[i], accel_limits, jerk_limits_by_index))
      mark_invalid(failure->reason);

  profile.dispatchable = profile.reaches_rest_on_path && profile.hits_target_time &&
                         profile.terminal_state_zero && profile.within_budget;
}

Result<AliStopProfileDTO> compute_ali_backtracking_stop_profile(
    const TrajectoryDTO& accepted_trajectory, double start_path_time_s, double start_path_rate,
    double start_path_rate_accel, double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3, double sample_period_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<AliStopProfileDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<AliStopProfileDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 std::move(message),
                 {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(start_path_rate) || start_path_rate <= 0.0) {
    return invalid("invalid_start_path_rate", "ALI retimer requires a positive start_path_rate");
  }
  if (!std::isfinite(start_path_rate_accel)) {
    return invalid("invalid_start_path_rate_accel", "start_path_rate_accel must be finite");
  }
  if (!finite_positive(target_stop_time_s) || !finite_positive(sample_period_s)) {
    return invalid("invalid_ali_timing",
                   "ALI retimer requires positive target_stop_time_s and "
                   "sample_period_s");
  }
  const double path_start_s = accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s = accepted_trajectory.waypoints.back().time_from_start_s;
  if (!std::isfinite(start_path_time_s) || start_path_time_s < path_start_s - kEpsilon ||
      start_path_time_s > path_end_s + kEpsilon) {
    return invalid("invalid_start_path_time",
                   "start_path_time_s must be finite and inside the accepted "
                   "trajectory time range");
  }
  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const auto d_it = per_joint_max_deceleration_rad_s2.find(joint_name);
    const auto j_it = per_joint_max_jerk_rad_s3.find(joint_name);
    if (d_it == per_joint_max_deceleration_rad_s2.end() || !finite_positive(d_it->second) ||
        j_it == per_joint_max_jerk_rad_s3.end() || !finite_positive(j_it->second)) {
      return invalid("missing_per_joint_limit",
                     "per-joint deceleration and jerk limits are required for " + joint_name);
    }
  }

  auto failure_profile = [&](std::string reason) {
    AliStopProfileDTO profile;
    profile.validation_failure_reason = std::move(reason);
    profile.achieved_stop_time_s = target_stop_time_s;
    profile.motion_stop_time_s = target_stop_time_s;
    return profile;
  };

  if (std::abs(start_path_rate_accel) > 1e-6) {
    return Result<AliStopProfileDTO>::Ok(failure_profile("ali_nonzero_start_accel_unsupported"));
  }

  PathCursor path_cursor(accepted_trajectory);

  // ALI is the third-line candidate after the exact-time quintic and generic
  // jerk-limited retimer fail. It is allowed to stop the moving portion before
  // a future bottleneck, then hold until the requested stop horizon, but only
  // if the emitted derivative-complete trajectory passes the same oversampled
  // accel/jerk validation used by the other candidates.
  auto build_triangular_scurve_candidate = [&](double brake_time_s,
                                               size_t backtracking_iterations) {
    AliStopProfileDTO profile;
    profile.achieved_stop_time_s = target_stop_time_s;
    profile.motion_stop_time_s = brake_time_s;
    profile.backtracking_iterations = backtracking_iterations;
    profile.samples.reserve(static_cast<size_t>(std::ceil(target_stop_time_s / sample_period_s)) +
                            2);

    const double path_jerk = 4.0 * start_path_rate / (brake_time_s * brake_time_s);
    const double peak_path_accel = 0.5 * path_jerk * brake_time_s;
    const double half_t = 0.5 * brake_time_s;
    const double v_mid = 0.5 * start_path_rate;
    const double s_mid = start_path_rate * half_t - (path_jerk * half_t * half_t * half_t) / 6.0;
    const double stop_distance = 0.5 * start_path_rate * brake_time_s;

    const size_t brake_steps =
        std::max<size_t>(2, static_cast<size_t>(std::ceil(brake_time_s / sample_period_s)));
    for (size_t i = 0; i <= brake_steps; ++i) {
      const double t = brake_time_s * static_cast<double>(i) / static_cast<double>(brake_steps);
      double v = 0.0;
      double a = 0.0;
      double s_rel = 0.0;
      if (t <= half_t) {
        a = -path_jerk * t;
        v = start_path_rate - 0.5 * path_jerk * t * t;
        s_rel = start_path_rate * t - (path_jerk * t * t * t) / 6.0;
      } else {
        const double tau = t - half_t;
        a = -peak_path_accel + path_jerk * tau;
        v = v_mid - peak_path_accel * tau + 0.5 * path_jerk * tau * tau;
        s_rel = s_mid + v_mid * tau - 0.5 * peak_path_accel * tau * tau +
                (path_jerk * tau * tau * tau) / 6.0;
      }
      if (i == brake_steps) {
        v = 0.0;
        a = 0.0;
        s_rel = stop_distance;
      }
      profile.samples.push_back({t, start_path_time_s + s_rel, std::max(0.0, v), a});
    }

    const double stop_path_time = start_path_time_s + stop_distance;
    if (stop_path_time > path_end_s + kEpsilon) {
      profile.validation_failure_reason = "ali_stop_runs_past_path_end";
      return profile;
    }

    double next_hold_time = profile.samples.back().time_from_ramp_start_s + sample_period_s;
    while (next_hold_time < target_stop_time_s - kEpsilon) {
      profile.samples.push_back({next_hold_time, stop_path_time, 0.0, 0.0});
      next_hold_time += sample_period_s;
    }
    if (target_stop_time_s > profile.samples.back().time_from_ramp_start_s + kEpsilon) {
      profile.samples.push_back({target_stop_time_s, stop_path_time, 0.0, 0.0});
    }

    profile.reaches_rest_on_path = true;
    validate_path_rate_stop_profile(accepted_trajectory, path_cursor,
                                    per_joint_max_deceleration_rad_s2, per_joint_max_jerk_rad_s3,
                                    start_path_time_s, start_path_rate, start_path_rate_accel,
                                    target_stop_time_s, sample_period_s, profile);
    return profile;
  };

  const size_t kCandidateCount = 32;
  AliStopProfileDTO best_failure = failure_profile("ali_no_candidate");
  for (size_t i = 0; i <= kCandidateCount; ++i) {
    const double fraction =
        1.0 - 0.9 * static_cast<double>(i) / static_cast<double>(kCandidateCount);
    const double brake_time_s = target_stop_time_s * fraction;
    if (brake_time_s <= 2.0 * sample_period_s) {
      continue;
    }
    auto candidate = build_triangular_scurve_candidate(brake_time_s, i);
    if (candidate.dispatchable) {
      return Result<AliStopProfileDTO>::Ok(std::move(candidate));
    }
    if (best_failure.validation_failure_reason == "ali_no_candidate" ||
        candidate.samples.size() > best_failure.samples.size()) {
      best_failure = std::move(candidate);
    }
  }
  if (best_failure.validation_failure_reason.empty()) {
    best_failure.validation_failure_reason = "ali_no_dispatchable_candidate";
  }
  return Result<AliStopProfileDTO>::Ok(std::move(best_failure));
}

struct ResolvedStopInitialState {
  PathCursor cursor;
  Projection projection;
  std::map<std::string, double> per_joint_max_deceleration_rad_s2;
  double effective_ramp_s;
  double command_path_time_s;
  double path_time_rate;
  double initial_velocity_deviation_rad_s;
  bool used_model_velocity;
};

Result<ResolvedStopInitialState> resolve_stop_initial_state(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config) {
  if (stop_profile.stop_mode != MotionInterruptStopMode::kSoftOnPath) {
    return err<ResolvedStopInitialState>(
        "stop_mode_not_soft_on_path", "soft_on_path planner called for a non-soft_on_path profile");
  }
  if (!stop_profile.deceleration_time_s || !finite_positive(*stop_profile.deceleration_time_s)) {
    return err<ResolvedStopInitialState>("soft_on_path_deceleration_time_required",
                                         "soft_on_path requires a positive deceleration_time_s");
  }
  if (!std::isfinite(config.stop_traj_delta_s) || config.stop_traj_delta_s < 0.0) {
    return err<ResolvedStopInitialState>("invalid_stop_traj_delta_s",
                                         "soft_on_path stop_traj_delta_s must be finite and >= 0");
  }
  if (!finite_positive(config.sample_period_s)) {
    return err<ResolvedStopInitialState>(
        "invalid_sample_period_s", "soft_on_path sample_period_s must be finite and positive");
  }
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<ResolvedStopInitialState>::Err(valid_shape.error());
  }
  if (const auto bounds_error = runtime_scale_bounds_error(accepted_trajectory)) {
    return err<ResolvedStopInitialState>(bounds_error->first, bounds_error->second,
                                         {{"recommended_disposition", "hard_stop"}});
  }
  const auto ordered_state_result =
      order_state_for_trajectory(initial_state.state, accepted_trajectory.joint_names);
  if (!ordered_state_result.ok()) {
    return Result<ResolvedStopInitialState>::Err(ordered_state_result.error());
  }
  const OrderedState& ordered_state = ordered_state_result.value();
  const size_t joint_count = accepted_trajectory.joint_names.size();
  const double path_start_s = accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s = accepted_trajectory.waypoints.back().time_from_start_s;

  // As main (item 24): the command-frame lead-in sits inside the deceleration
  // time, so the ramp itself lasts deceleration_time_s - stop_traj_delta_s.
  const double effective_ramp_s = *stop_profile.deceleration_time_s - config.stop_traj_delta_s;
  if (effective_ramp_s <= kEpsilon) {
    return err<ResolvedStopInitialState>(
        "deceleration_time_not_larger_than_handoff",
        "soft_on_path deceleration_time_s must exceed stop_traj_delta_s",
        {{"deceleration_time_s", format_double(*stop_profile.deceleration_time_s)},
         {"stop_traj_delta_s", format_double(config.stop_traj_delta_s)},
         {"recommended_disposition", "hard_stop"}});
  }

  auto typed_acceleration_limits = angular_deceleration_limits(stop_profile);
  if (!typed_acceleration_limits.has_value()) {
    return err<ResolvedStopInitialState>(
        "soft_on_path_coordinate_kind_unsupported",
        "legacy angular planner cannot consume a linear coordinate");
  }
  const auto& per_joint_max_deceleration_rad_s2 = *typed_acceleration_limits;
  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const auto it = per_joint_max_deceleration_rad_s2.find(joint_name);
    if (it == per_joint_max_deceleration_rad_s2.end() || !finite_positive(it->second)) {
      return err<ResolvedStopInitialState>(
          "missing_joint_deceleration_limit",
          "soft_on_path requires positive per-joint acceleration limits", {{"joint", joint_name}});
    }
  }

  const Projection projection = project_to_path(accepted_trajectory, ordered_state.positions);
  if (!std::isfinite(projection.distance_rad) || !std::isfinite(projection.time_from_start_s)) {
    return err<ResolvedStopInitialState>(
        "initial_path_projection_failed",
        "soft_on_path could not project observed feedback onto the "
        "accepted path; escalate to hard stop",
        {{"recommended_disposition", "hard_stop"}});
  }
  if (config.max_initial_path_deviation_rad.has_value() &&
      projection.distance_rad > *config.max_initial_path_deviation_rad) {
    return err<ResolvedStopInitialState>(
        "initial_path_deviation_exceeds_limit",
        "soft_on_path observed state is outside the configured path drift limit",
        {{"initial_path_deviation_rad", format_double(projection.distance_rad)},
         {"max_initial_path_deviation_rad", format_double(*config.max_initial_path_deviation_rad)},
         {"recommended_disposition", "hard_stop"}});
  }

  PathCursor cursor(accepted_trajectory);
  const PathSample projected_sample = cursor.sample(projection.time_from_start_s);
  const bool use_model_velocity =
      !ordered_state.has_velocity ||
      initial_state.velocity_source == SoftOnPathStateSource::kModelDerived;
  if (use_model_velocity && !config.allow_model_velocity_fallback) {
    return err<ResolvedStopInitialState>(
        "model_velocity_not_authorized",
        "soft_on_path requires measured velocity unless model fallback is authorized");
  }

  // Seam cursor (TARGET §4.8 authority): the lead-in and ramp anchor here, so it
  // MUST be the command frame, not the lagging measured-position projection —
  // anchoring on the projection re-issues points the controller's command has
  // already passed. Model/command authority uses the active command cursor
  // (trajectory_time_from_start_s, required here). A measured trip state uses
  // it too when its lane's lead-in anchor is kCommanded and the cursor is
  // known; kMeasured keeps the measured projection. projection.* stays the
  // drift/velocity guard input only.
  const bool command_anchor =
      use_model_velocity || (config.lead_in_anchor == StopLeadInAnchor::kCommanded &&
                             initial_state.trajectory_time_from_start_s.has_value());
  double seam_cursor_s = projection.time_from_start_s;
  if (command_anchor) {
    if (!initial_state.trajectory_time_from_start_s.has_value()) {
      return err<ResolvedStopInitialState>(
          "missing_command_cursor",
          "soft_on_path model/command authority requires "
          "trajectory_time_from_start_s (the active command cursor)");
    }
    // A non-finite cursor would defeat every downstream guard (NaN compares
    // false), letting the planner emit a defensive zero-filled replacement
    // trajectory that commands all joints to zero. Reject it here.
    if (!std::isfinite(*initial_state.trajectory_time_from_start_s)) {
      return err<ResolvedStopInitialState>(
          "non_finite_command_cursor",
          "soft_on_path command cursor (trajectory_time_from_start_s) "
          "must be finite; escalate to hard stop",
          {{"recommended_disposition", "hard_stop"}});
    }
    if (*initial_state.trajectory_time_from_start_s < path_start_s - kEpsilon ||
        *initial_state.trajectory_time_from_start_s > path_end_s + kEpsilon) {
      return err<ResolvedStopInitialState>(
          "command_cursor_out_of_range",
          "soft_on_path command cursor (trajectory_time_from_start_s) "
          "must be inside the accepted trajectory time range; escalate "
          "to hard stop",
          {{"trajectory_time_from_start_s",
            format_double(*initial_state.trajectory_time_from_start_s)},
           {"trajectory_start_time_s", format_double(path_start_s)},
           {"trajectory_end_time_s", format_double(path_end_s)},
           {"recommended_disposition", "hard_stop"}});
    }
    seam_cursor_s = *initial_state.trajectory_time_from_start_s;
  }

  double path_time_rate = 0.0;
  std::vector<double> projected_velocity(joint_count, 0.0);
  if (use_model_velocity) {
    if (!std::isfinite(initial_state.runtime_velocity_scale_at_trip) ||
        initial_state.runtime_velocity_scale_at_trip < 0.0) {
      return err<ResolvedStopInitialState>(
          "invalid_runtime_velocity_scale_at_trip",
          "soft_on_path runtime_velocity_scale_at_trip must be finite and >= 0");
    }
    path_time_rate = std::min(1.0, initial_state.runtime_velocity_scale_at_trip);
  } else {
    const double tangent_norm2 =
        dot(projected_sample.d_position_dt, projected_sample.d_position_dt);
    if (tangent_norm2 <= kEpsilon) {
      path_time_rate = 0.0;
    } else {
      path_time_rate =
          dot(ordered_state.velocities, projected_sample.d_position_dt) / tangent_norm2;
      if (!std::isfinite(path_time_rate)) {
        return err<ResolvedStopInitialState>(
            "invalid_projected_velocity",
            "soft_on_path could not project observed velocity onto the path");
      }
      path_time_rate = std::max(0.0, path_time_rate);
    }
  }
  for (size_t j = 0; j < joint_count; ++j) {
    projected_velocity[j] = projected_sample.d_position_dt[j] * path_time_rate;
  }
  const double initial_velocity_deviation =
      (!use_model_velocity && ordered_state.has_velocity)
          ? norm(subtract(ordered_state.velocities, projected_velocity))
          : 0.0;
  if (config.max_initial_velocity_deviation_rad_s.has_value() &&
      initial_velocity_deviation > *config.max_initial_velocity_deviation_rad_s) {
    return err<ResolvedStopInitialState>(
        "initial_velocity_deviation_exceeds_limit",
        "soft_on_path observed velocity is not coherent with the accepted path",
        {{"initial_velocity_deviation_rad_s", format_double(initial_velocity_deviation)},
         {"max_initial_velocity_deviation_rad_s",
          format_double(*config.max_initial_velocity_deviation_rad_s)},
         {"recommended_disposition", "hard_stop"}});
  }
  // A command-anchored lead-in on a measured trip state continues the command
  // at its own rate, the frozen runtime factor (as model authority); the
  // measured velocity only fed the coherence guard above.
  if (command_anchor && !use_model_velocity) {
    if (!std::isfinite(initial_state.runtime_velocity_scale_at_trip) ||
        initial_state.runtime_velocity_scale_at_trip < 0.0) {
      return err<ResolvedStopInitialState>(
          "invalid_runtime_velocity_scale_at_trip",
          "soft_on_path runtime_velocity_scale_at_trip must be finite and >= 0");
    }
    path_time_rate = std::min(1.0, initial_state.runtime_velocity_scale_at_trip);
  }

  return Result<ResolvedStopInitialState>::Ok(
      {std::move(cursor), projection, std::move(*typed_acceleration_limits), effective_ramp_s,
       seam_cursor_s, path_time_rate, initial_velocity_deviation, use_model_velocity});
}

}  // namespace

Result<SoftOnPathResolvedInitialStateDTO> resolve_soft_on_path_stop_initial_state(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config) {
  auto resolved =
      resolve_stop_initial_state(accepted_trajectory, stop_profile, initial_state, config);
  if (!resolved.ok()) return Result<SoftOnPathResolvedInitialStateDTO>::Err(resolved.error());
  auto& value = resolved.value();
  const double return_cursor =
      config.spring_back_return_path_time_s.value_or(value.command_path_time_s);
  if (!std::isfinite(return_cursor))
    return Result<SoftOnPathResolvedInitialStateDTO>::Err(ErrorDTO{
        "spring_back_return_cursor_invalid", "Spring-back return cursor must be finite", {}});
  const auto at_return = value.cursor.sample(return_cursor);
  const auto joints = accepted_trajectory.joint_names.size();
  return Result<SoftOnPathResolvedInitialStateDTO>::Ok(
      {value.command_path_time_s, value.path_time_rate,
       initial_state.runtime_velocity_scale_at_trip, accepted_trajectory.joint_names,
       TrajectoryWaypointDTO{0.0, at_return.position, std::vector<double>(joints, 0.0),
                             std::vector<double>(joints, 0.0)},
       value.projection.time_from_start_s, value.projection.distance_rad,
       value.initial_velocity_deviation_rad_s, value.used_model_velocity,
       config.spring_back_return_path_time_s});
}

Result<SoftOnPathStopPlanDTO> plan_soft_on_path_stop(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config) {
  auto resolved =
      resolve_stop_initial_state(accepted_trajectory, stop_profile, initial_state, config);
  if (!resolved.ok()) return Result<SoftOnPathStopPlanDTO>::Err(resolved.error());
  auto& cursor = resolved.value().cursor;
  const auto& projection = resolved.value().projection;
  const auto& per_joint_max_deceleration_rad_s2 =
      resolved.value().per_joint_max_deceleration_rad_s2;
  const double effective_ramp_s = resolved.value().effective_ramp_s;
  const double seam_cursor_s = resolved.value().command_path_time_s;
  const double path_time_rate = resolved.value().path_time_rate;
  const double initial_velocity_deviation = resolved.value().initial_velocity_deviation_rad_s;
  const bool use_model_velocity = resolved.value().used_model_velocity;
  const size_t joint_count = accepted_trajectory.joint_names.size();
  const double path_end_s = accepted_trajectory.waypoints.back().time_from_start_s;

  // Exact-time feasibility gate: refuse (escalate to hard stop) a soft stop
  // that cannot bring the robot to rest on this path, in the requested time,
  // within the per-joint deceleration budget. Both escalations below use the
  // fastest budget-respecting on-path stop (T_min) from the trip rate.
  {
    const auto min_stop = compute_soft_on_path_min_stop_time(
        accepted_trajectory, seam_cursor_s, path_time_rate, per_joint_max_deceleration_rad_s2);
    if (min_stop.ok()) {
      // Cannot stop on-path within the budget at all: even the fastest
      // budget-respecting stop overruns the remaining path. This is at least as
      // serious as "cannot stop fast enough" — escalate to a hard stop unless
      // the policy permits letting the original motion run to its natural
      // completion (handled by the short-path check below).
      if (!min_stop.value().reaches_rest_on_path &&
          !stop_profile.allow_natural_completion_on_short_path) {
        return err(
            "soft_on_path_cannot_stop_on_path_within_budget",
            "soft_on_path cannot bring the robot to rest on the remaining path "
            "within the per-joint deceleration budget; escalate to hard stop",
            {{"min_stop_distance_path_s", format_double(min_stop.value().stop_distance_path_s)},
             {"recommended_disposition", "hard_stop"}});
      }
      // Requested time shorter than the fastest feasible on-path stop (T_min):
      // physically infeasible under these limits, so escalate to a hard stop
      // rather than silently running long. (T_dec >= T_min is achievable: the
      // quintic ramp below when nothing binds, or a future redistributed
      // profile.)
      if (min_stop.value().reaches_rest_on_path &&
          *stop_profile.deceleration_time_s + kEpsilon < min_stop.value().min_stop_time_s) {
        return err("soft_on_path_deceleration_time_infeasible",
                   "soft_on_path deceleration_time_s is shorter than the minimum "
                   "budget-respecting on-path stop time; escalate to hard stop",
                   {{"deceleration_time_s", format_double(*stop_profile.deceleration_time_s)},
                    {"min_stop_time_s", format_double(min_stop.value().min_stop_time_s)},
                    {"recommended_disposition", "hard_stop"}});
      }
    }
  }

  // As main (Q38): the speed factor is frozen at the Stop request, so the
  // lead-in continues at that constant rate and the ramp starts from it with
  // zero path acceleration.
  const double handoff_path_time = seam_cursor_s + path_time_rate * config.stop_traj_delta_s;
  const double stop_path_time = handoff_path_time + 0.5 * path_time_rate * effective_ramp_s;
  if (stop_path_time > path_end_s + kEpsilon) {
    InterruptStructuredValue details{
        {"projected_time_from_start_s", format_double(projection.time_from_start_s)},
        {"stop_path_time_s", format_double(stop_path_time)},
        {"trajectory_end_time_s", format_double(path_end_s)}};
    // main's natural-completion predicate (item 2): the ramp overruns the
    // accepted path end. Decided here, once; spring-back does not re-check it.
    if (stop_profile.allow_natural_completion_on_short_path) {
      details["recommended_disposition"] = interrupt_structured_string("natural_completion");
    } else {
      details["recommended_fallback"] = interrupt_structured_string("spring_back_hold");
    }
    return err_structured("soft_on_path_remaining_time_short",
                          "soft_on_path stop ramp would pass the accepted trajectory end",
                          std::move(details));
  }

  // Q36: authored jerk verbatim, else main's default for this ramp, derived
  // from the deceleration time minus this lane's lead-in.
  const auto phase_jerk = detail::stop_phase_jerk_limits(
      accepted_trajectory, per_joint_max_deceleration_rad_s2, config, effective_ramp_s);
  const auto& jerk_limits = phase_jerk.limits;

  SoftOnPathStopPlanDTO plan;
  plan.command_path_time_s = seam_cursor_s;
  plan.forward_stop_path_time_s = stop_path_time;
  plan.projected_time_from_start_s = projection.time_from_start_s;
  plan.effective_ramp_s = effective_ramp_s;
  plan.stop_traj_delta_s = config.stop_traj_delta_s;
  plan.initial_path_deviation_rad = projection.distance_rad;
  plan.initial_velocity_deviation_rad_s = initial_velocity_deviation;
  plan.path_deviation_max_rad = projection.distance_rad;
  plan.runtime_velocity_scale_at_trip =
      std::clamp(initial_state.runtime_velocity_scale_at_trip, 0.0, 1.0);
  plan.used_model_velocity = use_model_velocity;
  plan.position_source = source_string(initial_state.position_source);
  plan.velocity_source =
      use_model_velocity ? "model_derived" : source_string(initial_state.velocity_source);
  plan.jerk_limit_source = phase_jerk.source;
  plan.diagnostics["jerk_reference_s"] = format_double(effective_ramp_s);
  plan.diagnostics["position_source"] = plan.position_source;
  plan.diagnostics["velocity_source"] = plan.velocity_source;
  plan.diagnostics["used_model_velocity"] = interrupt_structured_bool(plan.used_model_velocity);
  plan.diagnostics["sample_age_ns"] = std::to_string(initial_state.sample_age_ns);
  plan.diagnostics["runtime_velocity_scale_at_trip"] =
      format_double(plan.runtime_velocity_scale_at_trip);
  plan.diagnostics["stop_traj_delta_s"] = format_double(plan.stop_traj_delta_s);
  plan.diagnostics["effective_ramp_s"] = format_double(plan.effective_ramp_s);
  // Seam cursor (where the lead-in/ramp anchor) vs the measured-position
  // projection: their difference is the command-vs-measured tracking lag the
  // command-frame seam corrects for.
  plan.diagnostics["seam_cursor_s"] = format_double(seam_cursor_s);
  plan.diagnostics["measured_projection_s"] = format_double(projection.time_from_start_s);
  plan.diagnostics["initial_path_deviation_rad"] = format_double(plan.initial_path_deviation_rad);
  plan.diagnostics["initial_velocity_deviation_rad_s"] =
      format_double(plan.initial_velocity_deviation_rad_s);
  plan.diagnostics["jerk_limit_source"] = plan.jerk_limit_source;

  plan.replacement_trajectory.joint_names = accepted_trajectory.joint_names;
  // Command-frame lead-in seam (TARGET §5.4): open the replacement with the
  // in-flight accepted trajectory itself, resampled at the current command rate
  // from the trip cursor for the lead-in budget D_lead (config.stop_traj_delta_s),
  // then the decel ramp. The lead-in IS the backend command, so the seam
  // continues identical motion — continuity holds by construction, with no
  // synthetic bridge reconciling a measured anchor against the path (the source
  // of the handoff-jerk escalation that forced spring-back). The first sample
  // (tau = 0) carries the live command (q, qdot, qddot) as the seam pin; the ramp
  // continues from the on-path point at cursor + path_time_rate * D_lead,
  // derivative-coherent for free. The lead-in is the already-validated original
  // command, so it is NOT re-checked against the stop's deceleration budget (the
  // move may legitimately run faster than the stop decel). Measured feedback is
  // retained only as a monitor (initial path/velocity deviation, sample age, stop
  // confirmation), computed above and unaffected by this seam.
  plan.diagnostics["seam"] = interrupt_structured_string("command_frame_lead_in");
  // The lead-in anchor actually used (StopLeadInAnchor): a measured trip state
  // starts at its measured projection only on a kMeasured lane or without a
  // known command cursor.
  plan.diagnostics["lead_in_anchor"] = interrupt_structured_string(
      !use_model_velocity && (config.lead_in_anchor == StopLeadInAnchor::kMeasured ||
                              !initial_state.trajectory_time_from_start_s.has_value())
          ? "measured"
          : "commanded");
  const double lead_in_s = config.stop_traj_delta_s;
  const size_t lead_in_samples =
      std::max<size_t>(1, static_cast<size_t>(std::ceil(lead_in_s / config.sample_period_s)));
  PathSample lead_sample;
  for (size_t i = 0; i < lead_in_samples; ++i) {
    const double tau = lead_in_s * static_cast<double>(i) / static_cast<double>(lead_in_samples);
    const double path_time = seam_cursor_s + path_time_rate * tau;
    cursor.sample(path_time, lead_sample);
    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = tau;
    wp.positions_rad = lead_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = lead_sample.d_position_dt[j] * path_time_rate;
      wp.accelerations_rad_s2[j] = lead_sample.d2_position_dt2[j] * path_time_rate * path_time_rate;
    }
    plan.replacement_trajectory.waypoints.push_back(std::move(wp));
    plan.replacement_path_time_s.push_back(path_time);
  }

  // The ramp loop only appends waypoints to `plan` (its scalar fields and
  // diagnostics stay untouched until after the loop), so the retimer/ALI
  // fallback snapshot is reconstructed lazily by truncating back to the
  // lead-in prefix instead of deep-copying the plan on the happy path.
  const size_t lead_in_waypoint_count = plan.replacement_trajectory.waypoints.size();
  const auto base_plan_before_ramp = [&]() {
    SoftOnPathStopPlanDTO base = plan;
    base.replacement_trajectory.waypoints.resize(lead_in_waypoint_count);
    base.replacement_path_time_s.resize(lead_in_waypoint_count);
    return base;
  };
  auto try_jerk_limited_retimer =
      [&](std::string trigger_reason,
          InterruptStructuredValue trigger_details) -> Result<SoftOnPathStopPlanDTO> {
    trigger_details["handoff_path_time_s"] = format_double(handoff_path_time);
    if (!config.jerk_limits_enabled) {
      trigger_details["recommended_disposition"] = interrupt_structured_string("hard_stop");
      return err_structured(std::move(trigger_reason),
                            "Controlled Stop ramp cannot meet its path/acceleration constraints; "
                            "advance the declared fallback chain",
                            std::move(trigger_details));
    }
    auto retimed = compute_jerk_limited_stop_profile(
        accepted_trajectory, handoff_path_time, path_time_rate, 0.0, effective_ramp_s,
        per_joint_max_deceleration_rad_s2, jerk_limits, config.sample_period_s);
    if (!retimed.ok()) {
      return Result<SoftOnPathStopPlanDTO>::Err(retimed.error());
    }
    const auto& profile = retimed.value();
    if (!profile.dispatchable) {
      auto ali = compute_ali_backtracking_stop_profile(
          accepted_trajectory, handoff_path_time, path_time_rate, 0.0, effective_ramp_s,
          per_joint_max_deceleration_rad_s2, jerk_limits, config.sample_period_s);
      if (!ali.ok()) {
        return Result<SoftOnPathStopPlanDTO>::Err(ali.error());
      }
      const auto& ali_profile = ali.value();
      if (ali_profile.dispatchable) {
        SoftOnPathStopPlanDTO ali_plan = base_plan_before_ramp();
        ali_plan.forward_stop_path_time_s = ali_profile.samples.back().path_time_s;
        ali_plan.diagnostics["soft_on_path_retimer"] =
            interrupt_structured_string("ali_backtracking");
        ali_plan.diagnostics["retimer_trigger_reason"] = trigger_reason;
        ali_plan.diagnostics["prior_retimer"] = interrupt_structured_string("jerk_limited");
        ali_plan.diagnostics["prior_retimer_validation_failure_reason"] =
            profile.validation_failure_reason;
        ali_plan.diagnostics["ali_achieved_stop_time_s"] =
            format_double(ali_profile.achieved_stop_time_s);
        ali_plan.diagnostics["ali_motion_stop_time_s"] =
            format_double(ali_profile.motion_stop_time_s);
        ali_plan.diagnostics["ali_hold_time_s"] = format_double(
            std::max(0.0, ali_profile.achieved_stop_time_s - ali_profile.motion_stop_time_s));
        ali_plan.diagnostics["ali_stopped_before_requested_time"] = interrupt_structured_bool(
            ali_profile.motion_stop_time_s + kEpsilon < ali_profile.achieved_stop_time_s);
        ali_plan.diagnostics["ali_backtracking_iterations"] =
            std::to_string(ali_profile.backtracking_iterations);
        // The validated profile already carries the emitted joint waypoints
        // (1:1 with samples, same path-sampling math); reuse them with the
        // lead-in time shift instead of resampling the path per sample.
        const size_t first_ali_sample = config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
        for (size_t i = first_ali_sample; i < ali_profile.samples.size(); ++i) {
          TrajectoryWaypointDTO wp = ali_profile.emitted.waypoints[i];
          wp.time_from_start_s =
              config.stop_traj_delta_s + ali_profile.samples[i].time_from_ramp_start_s;
          ali_plan.replacement_trajectory.waypoints.push_back(std::move(wp));
          ali_plan.replacement_path_time_s.push_back(ali_profile.samples[i].path_time_s);
        }
        ali_plan.diagnostics["handoff_path_deviation_max_rad"] =
            format_double(ali_plan.path_deviation_max_rad);
        ali_plan.diagnostics["path_deviation_max_rad"] =
            format_double(ali_plan.path_deviation_max_rad);
        ali_plan.diagnostics["projected_time_from_start_s"] =
            format_double(ali_plan.projected_time_from_start_s);
        ali_plan.diagnostics["waypoint_count"] =
            std::to_string(ali_plan.replacement_trajectory.waypoints.size());
        return Result<SoftOnPathStopPlanDTO>::Ok(std::move(ali_plan));
      }
      trigger_details["reason"] =
          interrupt_structured_string("soft_on_path_retimer_not_dispatchable");
      trigger_details["trigger_reason"] = trigger_reason;
      trigger_details["retimer_validation_failure_reason"] = profile.validation_failure_reason;
      trigger_details["retimer_achieved_stop_time_s"] = format_double(profile.achieved_stop_time_s);
      trigger_details["retimer_used_backward_envelope"] =
          interrupt_structured_bool(profile.used_backward_envelope);
      trigger_details["retimer_reaches_rest_on_path"] =
          interrupt_structured_bool(profile.reaches_rest_on_path);
      trigger_details["retimer_hits_target_time"] =
          interrupt_structured_bool(profile.hits_target_time);
      trigger_details["ali_validation_failure_reason"] = ali_profile.validation_failure_reason;
      trigger_details["ali_achieved_stop_time_s"] = format_double(ali_profile.achieved_stop_time_s);
      trigger_details["ali_motion_stop_time_s"] = format_double(ali_profile.motion_stop_time_s);
      trigger_details["ali_hold_time_s"] = format_double(
          std::max(0.0, ali_profile.achieved_stop_time_s - ali_profile.motion_stop_time_s));
      trigger_details["ali_backtracking_iterations"] =
          std::to_string(ali_profile.backtracking_iterations);
      trigger_details["ali_reaches_rest_on_path"] =
          interrupt_structured_bool(ali_profile.reaches_rest_on_path);
      trigger_details["ali_hits_target_time"] =
          interrupt_structured_bool(ali_profile.hits_target_time);
      trigger_details["recommended_disposition"] = interrupt_structured_string("hard_stop");
      return err_structured("soft_on_path_retimer_not_dispatchable",
                            "soft_on_path fixed ramp exceeded a limit and the jerk-limited "
                            "retimer plus ALI backtracking could not prove a dispatchable "
                            "replacement; escalate to hard stop",
                            std::move(trigger_details));
    }

    SoftOnPathStopPlanDTO retimed_plan = base_plan_before_ramp();
    retimed_plan.forward_stop_path_time_s = profile.samples.back().path_time_s;
    retimed_plan.diagnostics["soft_on_path_retimer"] = interrupt_structured_string("jerk_limited");
    retimed_plan.diagnostics["retimer_trigger_reason"] = trigger_reason;
    retimed_plan.diagnostics["retimer_achieved_stop_time_s"] =
        format_double(profile.achieved_stop_time_s);
    retimed_plan.diagnostics["retimer_target_decel_path_rate"] =
        format_double(profile.target_decel_path_rate);
    retimed_plan.diagnostics["retimer_used_backward_envelope"] =
        interrupt_structured_bool(profile.used_backward_envelope);
    // The validated profile already carries the emitted joint waypoints
    // (1:1 with samples, same path-sampling math); reuse them with the
    // lead-in time shift instead of resampling the path per sample.
    const size_t first_retimer_sample = config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
    for (size_t i = first_retimer_sample; i < profile.samples.size(); ++i) {
      TrajectoryWaypointDTO wp = profile.emitted_trajectory.waypoints[i];
      wp.time_from_start_s = config.stop_traj_delta_s + profile.samples[i].time_from_ramp_start_s;
      retimed_plan.replacement_trajectory.waypoints.push_back(std::move(wp));
      retimed_plan.replacement_path_time_s.push_back(profile.samples[i].path_time_s);
    }
    retimed_plan.diagnostics["handoff_path_deviation_max_rad"] =
        format_double(retimed_plan.path_deviation_max_rad);
    retimed_plan.diagnostics["path_deviation_max_rad"] =
        format_double(retimed_plan.path_deviation_max_rad);
    retimed_plan.diagnostics["projected_time_from_start_s"] =
        format_double(retimed_plan.projected_time_from_start_s);
    retimed_plan.diagnostics["waypoint_count"] =
        std::to_string(retimed_plan.replacement_trajectory.waypoints.size());
    return Result<SoftOnPathStopPlanDTO>::Ok(std::move(retimed_plan));
  };

  // Per-joint limits hoisted into index-aligned vectors: the ramp loop
  // below reads them per joint per waypoint.
  std::vector<double> accel_limits(joint_count, 0.0);
  std::vector<double> jerk_limits_by_index(config.jerk_limits_enabled ? joint_count : 0, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    accel_limits[j] = per_joint_max_deceleration_rad_s2.at(accepted_trajectory.joint_names[j]);
    if (config.jerk_limits_enabled)
      jerk_limits_by_index[j] = jerk_limits.at(accepted_trajectory.joint_names[j]);
  }

  const size_t ramp_samples = std::max<size_t>(
      1, static_cast<size_t>(std::ceil(effective_ramp_s / config.sample_period_s)));
  PathSample path_sample;
  const size_t first_ramp_sample = config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
  for (size_t i = first_ramp_sample; i <= ramp_samples; ++i) {
    const double r = static_cast<double>(i) / static_cast<double>(ramp_samples);
    const double tau = r * effective_ramp_s;
    const double path_time =
        handoff_path_time + path_time_rate * effective_ramp_s * ramp_position_factor(r);
    const double u_dot = path_time_rate * ramp_velocity_factor(r);
    const double u_ddot = path_time_rate / effective_ramp_s * ramp_acceleration_factor(r);
    cursor.sample(path_time, path_sample);

    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = config.stop_traj_delta_s + tau;
    wp.positions_rad = path_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = path_sample.d_position_dt[j] * u_dot;
      wp.accelerations_rad_s2[j] =
          path_sample.d2_position_dt2[j] * u_dot * u_dot + path_sample.d_position_dt[j] * u_ddot;
    }
    // Exclude the already-accepted lead-in, but include the first complete
    // ramp segment and its P/V/A seam even when the lead-in is zero.
    if (i > 0) {
      const auto& previous = plan.replacement_trajectory.waypoints.back();
      if (const auto failure =
              segment_limit_failure(previous, wp, accel_limits, jerk_limits_by_index)) {
        return try_jerk_limited_retimer(
            failure->is_jerk()           ? "soft_on_path_jerk_limit_exceeded"
            : failure->is_acceleration() ? "soft_on_path_acceleration_limit_exceeded"
                                         : "soft_on_path_interpolation_invalid",
            {{"joint", accepted_trajectory.joint_names[failure->joint]},
             {"interpolation_check", failure->reason},
             {"segment_start_s", format_double(previous.time_from_start_s)},
             {"segment_end_s", format_double(wp.time_from_start_s)},
             {"peak_value", format_double(failure->peak)},
             {"limit", format_double(failure->limit)}});
      }
    }
    plan.path_deviation_max_rad = std::max(plan.path_deviation_max_rad, projection.distance_rad);
    plan.replacement_trajectory.waypoints.push_back(std::move(wp));
    plan.replacement_path_time_s.push_back(path_time);
  }

  plan.diagnostics["handoff_path_deviation_max_rad"] = format_double(plan.path_deviation_max_rad);
  plan.diagnostics["path_deviation_max_rad"] = format_double(plan.path_deviation_max_rad);
  plan.diagnostics["soft_on_path_retimer"] = interrupt_structured_string("quintic");
  plan.diagnostics["projected_time_from_start_s"] = format_double(plan.projected_time_from_start_s);
  plan.diagnostics["waypoint_count"] = std::to_string(plan.replacement_trajectory.waypoints.size());
  return Result<SoftOnPathStopPlanDTO>::Ok(std::move(plan));
}

Result<SoftOnPathStopPlanDTO> plan_spring_back_on_path_stop(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config) {
  if (stop_profile.stop_mode != MotionInterruptStopMode::kSpringBackHold) {
    return err("stop_mode_not_spring_back_hold",
               "spring_back_hold planner called for a non-spring_back_hold profile",
               {{"recommended_disposition", "hard_stop"}});
  }
  // As main: the deceleration time is required here. Its default is filled
  // in once, when the profile is loaded (item 24), not by the planner.
  if (!stop_profile.deceleration_time_s || !finite_positive(*stop_profile.deceleration_time_s)) {
    return err("spring_back_deceleration_time_required",
               "spring_back_hold requires a positive deceleration_time_s",
               {{"recommended_disposition", "hard_stop"}});
  }
  if (!std::isfinite(config.stop_traj_delta_s) || config.stop_traj_delta_s < 0.0) {
    return err("invalid_stop_traj_delta_s",
               "spring_back_hold stop_traj_delta_s must be finite and >= 0",
               {{"recommended_disposition", "hard_stop"}});
  }
  if (!finite_positive(config.sample_period_s)) {
    return err("invalid_sample_period_s",
               "spring_back_hold sample_period_s must be finite and positive",
               {{"recommended_disposition", "hard_stop"}});
  }
  if (config.stop_deadline_s.has_value() && !finite_positive(*config.stop_deadline_s)) {
    return err("invalid_stop_deadline_s",
               "spring_back_hold stop_deadline_s must be finite and positive when present",
               {{"recommended_disposition", "hard_stop"}});
  }

  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) return Result<SoftOnPathStopPlanDTO>::Err(valid_shape.error());
  if (const auto bounds_error = runtime_scale_bounds_error(accepted_trajectory)) {
    return err(bounds_error->first, bounds_error->second,
               {{"recommended_disposition", "hard_stop"}});
  }
  const double total_time_s = *stop_profile.deceleration_time_s;
  const double phase_time_s = 0.5 * total_time_s;
  if (phase_time_s <= config.stop_traj_delta_s + kEpsilon) {
    return err("spring_back_phase_time_not_larger_than_handoff",
               "spring_back_hold deceleration_time_s must leave room for the "
               "command-frame lead-in inside the forward half",
               {{"recommended_disposition", "hard_stop"},
                {"deceleration_time_s", format_double(total_time_s)},
                {"forward_phase_time_s", format_double(phase_time_s)},
                {"stop_traj_delta_s", format_double(config.stop_traj_delta_s)}});
  }
  const double deadline_s = config.stop_deadline_s.value_or(total_time_s);

  MotionInterruptStopProfileDTO forward_profile = stop_profile;
  forward_profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  // As main: the forward half is a soft Stop over half the deceleration time,
  // its lead-in inside that half.
  forward_profile.deceleration_time_s = phase_time_s;
  // The spring-back planner is itself a fallback/stop mode. If the forward
  // half cannot be generated, fail closed and let the kernel/backend hard-stop
  // floor decide; do not recursively fall back to spring_back_hold. Natural
  // completion was decided once, before spring-back (item 2): any forward
  // failure below, including a short path, ends in the hard stop.
  forward_profile.fallback_stop_modes.clear();
  auto forward =
      plan_soft_on_path_stop(accepted_trajectory, forward_profile, initial_state, config);
  if (!forward.ok()) {
    ErrorDTO error = forward.error();
    error.message = "spring_back_hold forward on-path deceleration failed: " + error.message;
    error.details["spring_back_phase"] = "forward_deceleration";
    error.details["requested_stop_mode"] = "spring_back_hold";
    error.details["recommended_disposition"] = "hard_stop";
    error.details.erase("recommended_fallback");
    return Result<SoftOnPathStopPlanDTO>::Err(std::move(error));
  }

  SoftOnPathStopPlanDTO plan = std::move(forward.value());
  if (plan.replacement_trajectory.waypoints.empty()) {
    return err("spring_back_forward_plan_empty",
               "spring_back_hold forward phase produced no waypoints",
               {{"recommended_disposition", "hard_stop"}});
  }

  // At-rest join (item 6, fixes main's overlap of the two halves): braking
  // must end at rest. A retimer that settles early holds that rest until the
  // half boundary; one that ends late starts the return after it, as long as
  // the whole Stop still fits its deadline.
  std::optional<size_t> adjusted_forward_end;
  double return_start_s = phase_time_s;
  auto& forward_end = plan.replacement_trajectory.waypoints.back();
  if (forward_end.time_from_start_s != phase_time_s) {
    const auto is_rest = [&](const auto& values) {
      return values.size() == accepted_trajectory.joint_names.size() &&
             std::all_of(values.begin(), values.end(), [](double value) { return value == 0.0; });
    };
    if (!is_rest(forward_end.velocities_rad_s) || !is_rest(forward_end.accelerations_rad_s2)) {
      return err("spring_back_forward_phase_not_at_rest",
                 "spring_back_hold cannot join its return to an unsettled forward profile",
                 {{"recommended_disposition", "hard_stop"}});
    }
    if (forward_end.time_from_start_s < phase_time_s - kEpsilon) {
      auto hold = forward_end;
      hold.time_from_start_s = phase_time_s;
      plan.replacement_trajectory.waypoints.push_back(std::move(hold));
      plan.replacement_path_time_s.push_back(plan.replacement_path_time_s.back());
    } else if (forward_end.time_from_start_s <= phase_time_s + kEpsilon) {
      forward_end.time_from_start_s = phase_time_s;
      adjusted_forward_end = plan.replacement_trajectory.waypoints.size() - 1;
    } else {
      return_start_s = forward_end.time_from_start_s;
    }
  }
  const auto checked_return_start =
      detail::spring_back_return_start_s(return_start_s, phase_time_s, phase_time_s, deadline_s);
  if (!checked_return_start) {
    return err("spring_back_stop_deadline_exceeded",
               "spring_back_hold forward braking ended too late for the return to finish "
               "within the Stop deadline",
               {{"recommended_disposition", "hard_stop"},
                {"spring_back_phase", "forward_deceleration"},
                {"forward_end_s", format_double(return_start_s)},
                {"return_time_s", format_double(phase_time_s)},
                {"stop_deadline_s", format_double(deadline_s)}});
  }
  return_start_s = *checked_return_start;

  const size_t joint_count = accepted_trajectory.joint_names.size();
  const double trip_path_time_s =
      config.spring_back_return_path_time_s.value_or(plan.command_path_time_s);
  const double stop_path_time_s = plan.forward_stop_path_time_s;
  double return_distance_s = stop_path_time_s - trip_path_time_s;
  if (!std::isfinite(return_distance_s) || return_distance_s < -kEpsilon) {
    return err("spring_back_invalid_return_distance",
               "spring_back_hold forward stop point is behind the trip point",
               {{"recommended_disposition", "hard_stop"},
                {"trip_path_time_s", format_double(trip_path_time_s)},
                {"stop_path_time_s", format_double(stop_path_time_s)}});
  }
  return_distance_s = std::max(0.0, return_distance_s);

  const auto typed_acceleration_limits = angular_deceleration_limits(stop_profile);
  if (!typed_acceleration_limits.has_value()) {
    return err("spring_back_coordinate_kind_unsupported",
               "legacy angular planner cannot consume a linear coordinate",
               {{"recommended_disposition", "hard_stop"}});
  }
  const auto& per_joint_max_deceleration_rad_s2 = *typed_acceleration_limits;
  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const auto accel_it = per_joint_max_deceleration_rad_s2.find(joint_name);
    if (accel_it == per_joint_max_deceleration_rad_s2.end() || !finite_positive(accel_it->second)) {
      return err("missing_joint_deceleration_limit",
                 "spring_back_hold requires positive per-joint acceleration limits",
                 {{"recommended_disposition", "hard_stop"}, {"joint", joint_name}});
    }
  }
  // Q36: the return half uses main's own default reference, the half time.
  const auto return_jerk = detail::stop_phase_jerk_limits(
      accepted_trajectory, per_joint_max_deceleration_rad_s2, config, phase_time_s);
  const auto& jerk_limits = return_jerk.limits;

  // Per-joint limits hoisted into index-aligned vectors for the reverse loop.
  std::vector<double> accel_limits(joint_count, 0.0);
  std::vector<double> jerk_limits_by_index(config.jerk_limits_enabled ? joint_count : 0, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    accel_limits[j] = per_joint_max_deceleration_rad_s2.at(accepted_trajectory.joint_names[j]);
    if (config.jerk_limits_enabled)
      jerk_limits_by_index[j] = jerk_limits.at(accepted_trajectory.joint_names[j]);
  }

  // Even a roundoff-sized timestamp adjustment changes the polynomial.
  // Recheck only that segment, against the forward half's own limits.
  if (adjusted_forward_end && *adjusted_forward_end > 0) {
    const auto forward_jerk =
        detail::stop_phase_jerk_limits(accepted_trajectory, per_joint_max_deceleration_rad_s2,
                                       config, phase_time_s - config.stop_traj_delta_s);
    std::vector<double> forward_jerk_by_index(config.jerk_limits_enabled ? joint_count : 0, 0.0);
    for (size_t j = 0; j < forward_jerk_by_index.size(); ++j)
      forward_jerk_by_index[j] = forward_jerk.limits.at(accepted_trajectory.joint_names[j]);
    const auto& points = plan.replacement_trajectory.waypoints;
    if (const auto failure =
            segment_limit_failure(points[*adjusted_forward_end - 1], points[*adjusted_forward_end],
                                  accel_limits, forward_jerk_by_index))
      return err("spring_back_forward_interpolation_invalid",
                 "spring_back_hold forward phase is invalid at its exact return boundary",
                 {{"recommended_disposition", "hard_stop"},
                  {"spring_back_phase", "forward_deceleration"},
                  {"interpolation_check", failure->reason}});
  }

  // Return to the trip point in the same half-time. main's quintic peaks at
  // 1.875 d/T; when that exceeds the trajectory's certified Stop path rate,
  // the return ramps up to that rate, cruises, and ramps down instead (item 6).
  const double max_stop_path_rate = accepted_trajectory.runtime_scale_bounds->max_stop_path_rate;
  const double quintic_peak_rate = 1.875 * return_distance_s / phase_time_s;
  const bool capped_return = quintic_peak_rate > max_stop_path_rate + kEpsilon;
  ReturnShape shape{phase_time_s, return_distance_s, max_stop_path_rate};
  if (capped_return) {
    shape.ramp_s = phase_time_s - return_distance_s / max_stop_path_rate;
    shape.cruise_s = phase_time_s - 2.0 * shape.ramp_s;
    if (!std::isfinite(shape.ramp_s) || shape.ramp_s <= kEpsilon || shape.cruise_s < -kEpsilon) {
      return err("spring_back_return_rate_cap_infeasible",
                 "spring_back_hold cannot return to the trip point within its half-time at the "
                 "certified Stop path rate",
                 {{"recommended_disposition", "hard_stop"},
                  {"spring_back_phase", "reverse_return"},
                  {"return_distance_path_s", format_double(return_distance_s)},
                  {"max_stop_path_rate", format_double(max_stop_path_rate)}});
    }
    shape.cruise_s = std::max(0.0, shape.cruise_s);
  }

  // Knots at every piece boundary, so no emitted segment straddles a switch.
  std::vector<double> return_times;
  const auto append_piece = [&](double begin_s, double length_s) {
    if (length_s <= kEpsilon) return;
    const size_t samples =
        std::max<size_t>(1, static_cast<size_t>(std::ceil(length_s / config.sample_period_s)));
    for (size_t i = 1; i <= samples; ++i)
      return_times.push_back(begin_s +
                             length_s * static_cast<double>(i) / static_cast<double>(samples));
  };
  if (capped_return) {
    append_piece(0.0, shape.ramp_s);
    append_piece(shape.ramp_s, shape.cruise_s);
    append_piece(shape.ramp_s + shape.cruise_s, phase_time_s - shape.ramp_s - shape.cruise_s);
  } else {
    append_piece(0.0, phase_time_s);
  }
  if (return_times.empty() || return_times.back() != phase_time_s) {
    if (!return_times.empty())
      return_times.back() = phase_time_s;
    else
      return_times.push_back(phase_time_s);
  }

  PathCursor reverse_cursor(accepted_trajectory);
  PathSample path_sample;
  for (const double tau : return_times) {
    const auto progress = capped_return ? shape.capped(tau) : shape.quintic(tau);
    const double path_time =
        tau == phase_time_s ? trip_path_time_s : stop_path_time_s - progress.distance;
    const double path_rate = -progress.rate;
    const double path_accel = -progress.rate_accel;
    reverse_cursor.sample(path_time, path_sample);

    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = return_start_s + tau;
    wp.positions_rad = path_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = path_sample.d_position_dt[j] * path_rate;
      wp.accelerations_rad_s2[j] = path_sample.d2_position_dt2[j] * path_rate * path_rate +
                                   path_sample.d_position_dt[j] * path_accel;
    }
    const auto& previous = plan.replacement_trajectory.waypoints.back();
    if (const auto failure =
            segment_limit_failure(previous, wp, accel_limits, jerk_limits_by_index)) {
      return err(failure->is_jerk()           ? "spring_back_return_jerk_limit_exceeded"
                 : failure->is_acceleration() ? "spring_back_return_acceleration_limit_exceeded"
                                              : "spring_back_return_interpolation_invalid",
                 "spring_back_hold return interpolation exceeds a per-joint limit",
                 {{"recommended_disposition", "hard_stop"},
                  {"spring_back_phase", "reverse_return"},
                  {"joint", accepted_trajectory.joint_names[failure->joint]},
                  {"interpolation_check", failure->reason},
                  {"segment_start_s", format_double(previous.time_from_start_s)},
                  {"segment_end_s", format_double(wp.time_from_start_s)},
                  {"peak_value", format_double(failure->peak)},
                  {"limit", format_double(failure->limit)}});
    }
    plan.replacement_trajectory.waypoints.push_back(std::move(wp));
    plan.replacement_path_time_s.push_back(path_time);
  }

  const double stop_end_s = return_start_s + phase_time_s;
  plan.effective_ramp_s = stop_end_s;
  plan.jerk_limit_source = return_jerk.source;
  plan.diagnostics["spring_back_return_jerk_reference_s"] = format_double(phase_time_s);
  plan.diagnostics["jerk_limit_source"] = plan.jerk_limit_source;
  plan.diagnostics["stop_mode"] = interrupt_structured_string("spring_back_hold");
  plan.diagnostics["spring_back_mode"] = interrupt_structured_string("on_path_two_phase");
  plan.diagnostics["spring_back_forward_time_s"] = format_double(phase_time_s);
  plan.diagnostics["spring_back_return_start_s"] = format_double(return_start_s);
  plan.diagnostics["spring_back_reverse_time_s"] = format_double(phase_time_s);
  plan.diagnostics["spring_back_total_time_s"] = format_double(stop_end_s);
  plan.diagnostics["spring_back_trip_path_time_s"] = format_double(trip_path_time_s);
  plan.diagnostics["spring_back_stop_path_time_s"] = format_double(stop_path_time_s);
  plan.diagnostics["spring_back_return_distance_path_s"] = format_double(return_distance_s);
  plan.diagnostics["spring_back_return_profile"] = interrupt_structured_string(
      capped_return ? "ramp_cruise_ramp_rate_capped" : "quintic_rest_to_rest");
  plan.diagnostics["spring_back_return_peak_path_rate"] =
      format_double(capped_return ? max_stop_path_rate : quintic_peak_rate);
  plan.diagnostics["max_stop_path_rate"] = format_double(max_stop_path_rate);
  plan.diagnostics["effective_ramp_s"] = format_double(stop_end_s);
  plan.diagnostics["waypoint_count"] = std::to_string(plan.replacement_trajectory.waypoints.size());
  return Result<SoftOnPathStopPlanDTO>::Ok(std::move(plan));
}

bool soft_on_path_error_recommends_natural_completion(const ErrorDTO& error) {
  const auto reason_it = error.details.find("reason");
  const auto disposition_it = error.details.find("recommended_disposition");
  return reason_it != error.details.end() &&
         reason_it->second == "soft_on_path_remaining_time_short" &&
         disposition_it != error.details.end() && disposition_it->second == "natural_completion";
}

bool soft_on_path_error_recommends_hard_stop(const ErrorDTO& error) {
  const auto disposition_it = error.details.find("recommended_disposition");
  return disposition_it != error.details.end() && disposition_it->second == "hard_stop";
}

Result<double> measure_soft_on_path_path_deviation(const TrajectoryDTO& accepted_trajectory,
                                                   const RobotStateDTO& observed_state) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<double>::Err(valid_shape.error());
  }
  const auto ordered_state_result =
      order_state_for_trajectory(observed_state, accepted_trajectory.joint_names);
  if (!ordered_state_result.ok()) {
    return Result<double>::Err(ordered_state_result.error());
  }
  const Projection projection =
      project_to_path(accepted_trajectory, ordered_state_result.value().positions);
  if (!std::isfinite(projection.distance_rad)) {
    return Result<double>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMotionInterruptStopStateInvalid)),
                 "soft_on_path could not project observed feedback onto the accepted path",
                 {{"reason", "path_deviation_projection_failed"}}});
  }
  return Result<double>::Ok(projection.distance_rad);
}

Result<TrajectoryDTO> shift_soft_stop_for_dispatch_latency(
    const TrajectoryDTO& planned, double compute_latency_s, double lead_in_budget_s,
    const TrajectoryDTO& accepted_trajectory, double command_cursor_s, double path_time_rate,
    const DispatchLatencyShiftConfig& config) {
  const auto valid_shape = validate_trajectory_shape(planned);
  if (!valid_shape.ok()) {
    return Result<TrajectoryDTO>::Err(valid_shape.error());
  }
  const auto valid_accepted = validate_trajectory_shape(accepted_trajectory);
  if (!valid_accepted.ok()) {
    return Result<TrajectoryDTO>::Err(valid_accepted.error());
  }
  if (!std::isfinite(compute_latency_s) || compute_latency_s < 0.0) {
    return Result<TrajectoryDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "dispatch-latency shift requires a finite, non-negative compute latency",
                 {{"reason", "invalid_compute_latency_s"},
                  {"compute_latency_s", format_double(compute_latency_s)}}});
  }
  if (!finite_positive(lead_in_budget_s)) {
    return Result<TrajectoryDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "dispatch-latency shift requires a finite, positive lead-in budget",
                 {{"reason", "invalid_lead_in_budget_s"},
                  {"lead_in_budget_s", format_double(lead_in_budget_s)}}});
  }
  if (!std::isfinite(path_time_rate) || path_time_rate < 0.0) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "dispatch-latency shift requires a finite, non-negative path_time_rate",
        {{"reason", "invalid_path_time_rate"}, {"path_time_rate", format_double(path_time_rate)}}});
  }
  if (!std::isfinite(config.anchor_negative_time_s) || config.anchor_negative_time_s >= 0.0) {
    return Result<TrajectoryDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "dispatch-latency anchor time must be finite and negative",
                 {{"reason", "invalid_anchor_negative_time_s"},
                  {"anchor_negative_time_s", format_double(config.anchor_negative_time_s)}}});
  }

  // Reliability guard: if the compute+dispatch latency consumed the command-frame
  // lead-in budget there is no command-continuation left to seam onto, so escalate
  // to a hard stop rather than emit a degraded seam. This is the worst-case path
  // (a slow planner — e.g. the ALI/backtracking tail — or a too-small lead-in),
  // not the common few-ms case.
  if (compute_latency_s >= lead_in_budget_s) {
    return Result<TrajectoryDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
                 "dispatch latency consumed the lead-in budget; escalate to hard stop",
                 {{"reason", "dispatch_latency_exceeds_lead_in"},
                  {"recommended_disposition", "hard_stop"},
                  {"compute_latency_s", format_double(compute_latency_s)},
                  {"lead_in_budget_s", format_double(lead_in_budget_s)}}});
  }

  const size_t joint_count = planned.joint_names.size();

  TrajectoryDTO shifted;
  shifted.joint_names = planned.joint_names;

  // Leading negative-time continuity pin at the COMMAND state at the advanced
  // cursor (where the controller's command has reached by dispatch), carrying the
  // full q, qdot, qddot sampled from the accepted trajectory — command-frame, not
  // a measured or partial (qddot=0) state — so stock JTC's spline starts through
  // the live command frame.
  const double advanced_cursor_s = command_cursor_s + path_time_rate * compute_latency_s;
  const PathSample pin_sample = sample_path(accepted_trajectory, advanced_cursor_s);
  TrajectoryWaypointDTO anchor_wp;
  anchor_wp.time_from_start_s = config.anchor_negative_time_s;
  anchor_wp.positions_rad = pin_sample.position;
  anchor_wp.velocities_rad_s.assign(joint_count, 0.0);
  anchor_wp.accelerations_rad_s2.assign(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    anchor_wp.velocities_rad_s[j] = pin_sample.d_position_dt[j] * path_time_rate;
    anchor_wp.accelerations_rad_s2[j] =
        pin_sample.d2_position_dt2[j] * path_time_rate * path_time_rate;
  }
  shifted.waypoints.push_back(std::move(anchor_wp));

  // Shift every planned waypoint earlier by the elapsed compute latency and drop
  // those already consumed during the gap (would otherwise pull the robot back).
  std::size_t kept = 0;
  for (const auto& wp : planned.waypoints) {
    const double new_time = wp.time_from_start_s - compute_latency_s;
    if (new_time < 0.0) {
      continue;
    }
    TrajectoryWaypointDTO out_wp = wp;
    out_wp.time_from_start_s = new_time;
    shifted.waypoints.push_back(std::move(out_wp));
    ++kept;
  }

  if (kept == 0) {
    return Result<TrajectoryDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
                 "dispatch latency consumed every replacement waypoint; escalate to hard stop",
                 {{"reason", "dispatch_latency_consumed_all_waypoints"},
                  {"recommended_disposition", "hard_stop"},
                  {"compute_latency_s", format_double(compute_latency_s)}}});
  }

  return Result<TrajectoryDTO>::Ok(std::move(shifted));
}

Result<SoftOnPathMinStopTimeDTO> compute_soft_on_path_min_stop_time(
    const TrajectoryDTO& accepted_trajectory, double trip_path_time_s, double trip_path_rate,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    double integration_step_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<SoftOnPathMinStopTimeDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<SoftOnPathMinStopTimeDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 std::move(message),
                 {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(trip_path_rate) || trip_path_rate < 0.0) {
    return invalid("invalid_trip_path_rate", "trip_path_rate must be finite and non-negative");
  }
  if (!finite_positive(integration_step_s)) {
    return invalid("invalid_integration_step_s", "integration_step_s must be finite and positive");
  }
  const size_t joint_count = accepted_trajectory.joint_names.size();
  std::vector<double> max_decel(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    const auto it = per_joint_max_deceleration_rad_s2.find(accepted_trajectory.joint_names[j]);
    if (it == per_joint_max_deceleration_rad_s2.end() || !finite_positive(it->second)) {
      return invalid("missing_per_joint_max_deceleration",
                     "per-joint max deceleration missing or non-positive for " +
                         accepted_trajectory.joint_names[j]);
    }
    max_decel[j] = it->second;
  }

  const double path_start_s = accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s = accepted_trajectory.waypoints.back().time_from_start_s;
  if (!std::isfinite(trip_path_time_s) || trip_path_time_s < path_start_s - kEpsilon ||
      trip_path_time_s > path_end_s + kEpsilon) {
    return invalid("invalid_trip_path_time",
                   "trip_path_time_s must be finite and inside the accepted "
                   "trajectory time range");
  }
  SoftOnPathMinStopTimeDTO out;
  out.stop_path_time_s = trip_path_time_s;
  if (trip_path_rate <= kEpsilon) {  // already at rest: zero-time stop in place
    out.reaches_rest_on_path = trip_path_time_s <= path_end_s + kEpsilon;
    return Result<SoftOnPathMinStopTimeDTO>::Ok(std::move(out));
  }

  double s = trip_path_time_s;    // path cursor (accepted-trajectory time)
  double u_dot = trip_path_rate;  // path-rate ds/dt (> 0)
  double t = 0.0;
  std::vector<int> binding_counts(joint_count, 0);
  const double dt = integration_step_s;
  const size_t kMaxSteps = 2'000'000;  // defensive: rate must reach zero first
  bool reached_rest = false;
  PathCursor path_cursor(accepted_trajectory);
  PathSample sample;
  for (size_t steps = 0; steps < kMaxSteps; ++steps) {
    path_cursor.sample(s, sample);
    // Most-negative path-rate acceleration ü keeping every joint's executed
    // acceleration a_j = q''_j·u̇² + q'_j·ü within [-D_j, D_j]. Per joint that
    // is an interval [lo_j, hi_j] on ü; the feasible band is the intersection,
    // and the hardest deceleration is its lower edge (the binding joint).
    double u_ddot_lo = 0.0;
    double u_ddot_hi = 0.0;
    bool found = false;
    int binding_j = -1;
    for (size_t j = 0; j < joint_count; ++j) {
      const double qp = sample.d_position_dt[j];
      if (std::abs(qp) <= kEpsilon) {
        continue;  // joint not advancing along the path here: no ü constraint
      }
      const double curvature_accel = sample.d2_position_dt2[j] * u_dot * u_dot;
      const double bound_a = (-max_decel[j] - curvature_accel) / qp;
      const double bound_b = (max_decel[j] - curvature_accel) / qp;
      const double lo_j = std::min(bound_a, bound_b);  // qp sign folded in
      const double hi_j = std::max(bound_a, bound_b);
      if (!found || lo_j > u_ddot_lo) {
        u_ddot_lo = lo_j;
        binding_j = static_cast<int>(j);
      }
      if (!found || hi_j < u_ddot_hi) {
        u_ddot_hi = hi_j;
      }
      found = true;
    }
    if (!found) {  // degenerate: no joint advances along the path → stop now
      reached_rest = true;
      break;
    }
    if (u_ddot_lo > u_ddot_hi + 1e-9 || u_ddot_lo >= -kEpsilon) {
      // Path curvature alone saturates a joint's budget at this rate, or no
      // net deceleration is available: cannot stop on-path within budget.
      out.reaches_rest_on_path = false;
      break;
    }
    if (binding_j >= 0) {
      binding_counts[static_cast<size_t>(binding_j)] += 1;
    }
    const double next_u_dot = u_dot + u_ddot_lo * dt;
    if (next_u_dot <= 0.0) {  // sub-step to the exact zero-rate crossing
      const double dt_zero = u_dot / (-u_ddot_lo);
      s += u_dot * dt_zero + 0.5 * u_ddot_lo * dt_zero * dt_zero;
      t += dt_zero;
      reached_rest = true;
      break;
    }
    s += u_dot * dt + 0.5 * u_ddot_lo * dt * dt;
    t += dt;
    u_dot = next_u_dot;
    if (s > path_end_s + kEpsilon) {  // ran off the path end before stopping
      break;
    }
  }

  out.min_stop_time_s = t;
  out.stop_path_time_s = s;
  out.stop_distance_path_s = s - trip_path_time_s;
  out.reaches_rest_on_path = reached_rest && s <= path_end_s + kEpsilon;
  // Alphabetical iteration with a strict `>` preserves the previous
  // map-keyed tie-break: among equal counts the lexicographically
  // smallest joint name wins.
  std::vector<size_t> by_name(joint_count);
  for (size_t j = 0; j < joint_count; ++j) {
    by_name[j] = j;
  }
  std::sort(by_name.begin(), by_name.end(), [&](size_t a, size_t b) {
    return accepted_trajectory.joint_names[a] < accepted_trajectory.joint_names[b];
  });
  int best = 0;
  for (const size_t j : by_name) {
    if (binding_counts[j] > best) {
      best = binding_counts[j];
      out.binding_joint = accepted_trajectory.joint_names[j];
    }
  }
  return Result<SoftOnPathMinStopTimeDTO>::Ok(std::move(out));
}

Result<JerkLimitedStopProfileDTO> compute_jerk_limited_stop_profile(
    const TrajectoryDTO& accepted_trajectory, double start_path_time_s, double start_path_rate,
    double start_path_rate_accel, double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3, double sample_period_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<JerkLimitedStopProfileDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<JerkLimitedStopProfileDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 std::move(message),
                 {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(start_path_rate) || start_path_rate <= 0.0) {
    return invalid("invalid_start_path_rate",
                   "jerk-limited retimer requires a positive start_path_rate");
  }
  if (!std::isfinite(start_path_rate_accel)) {
    return invalid("invalid_start_path_rate_accel", "start_path_rate_accel must be finite");
  }
  const double path_start_s = accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s = accepted_trajectory.waypoints.back().time_from_start_s;
  if (!std::isfinite(start_path_time_s) || start_path_time_s < path_start_s - kEpsilon ||
      start_path_time_s > path_end_s + kEpsilon) {
    return invalid("invalid_start_path_time",
                   "start_path_time_s must be finite and inside the accepted "
                   "trajectory time range");
  }
  if (!finite_positive(target_stop_time_s) || !finite_positive(sample_period_s)) {
    return invalid("invalid_retimer_timing",
                   "jerk-limited retimer requires positive target_stop_time_s "
                   "and sample_period_s");
  }
  const size_t joint_count = accepted_trajectory.joint_names.size();
  std::vector<double> max_decel(joint_count, 0.0);
  std::vector<double> max_jerk(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    const auto& name = accepted_trajectory.joint_names[j];
    const auto d_it = per_joint_max_deceleration_rad_s2.find(name);
    const auto j_it = per_joint_max_jerk_rad_s3.find(name);
    if (d_it == per_joint_max_deceleration_rad_s2.end() || !finite_positive(d_it->second) ||
        j_it == per_joint_max_jerk_rad_s3.end() || !finite_positive(j_it->second)) {
      return invalid("missing_per_joint_limit",
                     "per-joint deceleration and jerk limits are required for " + name);
    }
    max_decel[j] = d_it->second;
    max_jerk[j] = j_it->second;
  }
  constexpr double kBig = 1e18;
  const double target_time_tolerance_s = std::max(2.0 * sample_period_s, 1e-3);
  constexpr double kTerminalTolerance = 1e-3;
  constexpr double kAccelTolerance = 1e-6;
  constexpr double kJerkTolerance = 1e-6;
  constexpr double kEnvelopeSafetyMargin = 0.8;

  PathCursor path_cursor(accepted_trajectory);

  // Intersect signed path-acceleration and path-jerk intervals for every joint:
  // q_ddot = q'' v² + q' a; q_jerk = q''' v³ + 3 q'' v a + q' jerk.
  // The residual terms can add to OR cancel the controllable term. These
  // instantaneous budgets guide the search; emitted-curve validation below
  // still checks the whole replacement, including segment boundaries.
  auto budgets = [&](const PathSample& smp, double v, double a, double* lo, double* hi,
                     double* jerk_lo, double* jerk_hi) -> bool {
    *lo = -kBig;
    *hi = kBig;
    *jerk_lo = -kBig;
    *jerk_hi = kBig;
    bool found = false;
    for (size_t j = 0; j < joint_count; ++j) {
      const double qp = smp.d_position_dt[j];
      const double curvature = smp.d2_position_dt2[j] * v * v;
      const double accel_budget = kEnvelopeSafetyMargin * max_decel[j];
      const double residual_jerk =
          smp.d3_position_dt3[j] * v * v * v + 3.0 * smp.d2_position_dt2[j] * v * a;
      const double jerk_budget = kEnvelopeSafetyMargin * max_jerk[j];
      if (!std::isfinite(qp) || !std::isfinite(curvature) || !std::isfinite(residual_jerk)) {
        *lo = 1.0;
        *hi = 0.0;
        return true;
      }
      if (std::abs(qp) <= kEpsilon) {
        if (std::abs(curvature) > accel_budget + kAccelTolerance ||
            std::abs(residual_jerk) > jerk_budget + kJerkTolerance) {
          // A zero tangent cannot compensate a residual derivative violation.
          *lo = 1.0;
          *hi = 0.0;
          return true;
        }
        continue;
      }
      const double bound_a = (-accel_budget - curvature) / qp;
      const double bound_b = (accel_budget - curvature) / qp;
      if (!found || std::min(bound_a, bound_b) > *lo) {
        *lo = std::min(bound_a, bound_b);
      }
      if (!found || std::max(bound_a, bound_b) < *hi) {
        *hi = std::max(bound_a, bound_b);
      }
      const double jerk_a = (-jerk_budget - residual_jerk) / qp;
      const double jerk_b = (jerk_budget - residual_jerk) / qp;
      *jerk_lo = std::max(*jerk_lo, std::min(jerk_a, jerk_b));
      *jerk_hi = std::min(*jerk_hi, std::max(jerk_a, jerk_b));
      found = true;
    }
    return found;
  };

  auto solve_velocity_zero = [](double v, double a, double j, double fallback_dt) {
    if (std::abs(j) <= kEpsilon) {
      return a < -kEpsilon ? std::max(0.0, -v / a) : fallback_dt;
    }
    const double disc = a * a - 2.0 * j * v;
    if (disc < 0.0) {
      return fallback_dt;
    }
    const double root = std::sqrt(disc);
    const double r1 = (-a - root) / j;
    const double r2 = (-a + root) / j;
    double best = fallback_dt;
    if (r1 >= -kEpsilon) {
      best = std::min(best, std::max(0.0, r1));
    }
    if (r2 >= -kEpsilon) {
      best = std::min(best, std::max(0.0, r2));
    }
    return best;
  };

  struct BackwardEnvelopePoint {
    double path_time_s = 0.0;
    double max_path_rate = 0.0;
  };

  // The backward envelope is an acceleration search hint, not a jerk proof.
  // A q'''-only rate cap would exclude valid cancellation by the other two
  // chain-rule terms. Jerk is budgeted together at the actual forward state.
  PathSample cap_sample;

  auto local_rate_cap = [&](double s) {
    path_cursor.sample(s, cap_sample);
    double cap = accepted_trajectory.runtime_scale_bounds ? 1.0 : kBig;
    for (size_t j = 0; j < joint_count; ++j) {
      const double curvature = std::abs(cap_sample.d2_position_dt2[j]);
      if (curvature > kEpsilon) {
        cap = std::min(cap, std::sqrt((kEnvelopeSafetyMargin * max_decel[j]) / curvature));
      }
    }
    return cap;
  };

  PathSample decel_cap_sample;
  auto local_path_decel_cap = [&](double s) {
    path_cursor.sample(s, decel_cap_sample);
    double cap = kBig;
    for (size_t j = 0; j < joint_count; ++j) {
      const double qp = std::abs(decel_cap_sample.d_position_dt[j]);
      if (qp > kEpsilon) {
        cap = std::min(cap, (kEnvelopeSafetyMargin * max_decel[j]) / qp);
      }
    }
    return cap;
  };

  std::vector<BackwardEnvelopePoint> backward_envelope;
  {
    const double remaining = path_end_s - start_path_time_s;
    if (remaining > kEpsilon) {
      const double nominal_step = std::max(0.002, sample_period_s * std::max(start_path_rate, 0.1));
      const size_t steps = std::max<size_t>(
          1, std::min<size_t>(4000, static_cast<size_t>(std::ceil(remaining / nominal_step))));
      backward_envelope.resize(steps + 1);
      for (size_t i = 0; i <= steps; ++i) {
        const double alpha = static_cast<double>(i) / static_cast<double>(steps);
        const double s = start_path_time_s + remaining * alpha;
        backward_envelope[i].path_time_s = s;
        backward_envelope[i].max_path_rate = local_rate_cap(s);
      }
      backward_envelope.back().max_path_rate = 0.0;
      for (size_t i = steps; i-- > 0;) {
        const double ds =
            std::max(0.0, backward_envelope[i + 1].path_time_s - backward_envelope[i].path_time_s);
        const double decel_cap = local_path_decel_cap(backward_envelope[i].path_time_s);
        const double reachable =
            decel_cap >= kBig / 2.0
                ? backward_envelope[i + 1].max_path_rate
                : std::sqrt(std::max(0.0, backward_envelope[i + 1].max_path_rate *
                                                  backward_envelope[i + 1].max_path_rate +
                                              2.0 * decel_cap * ds));
        backward_envelope[i].max_path_rate =
            std::min(backward_envelope[i].max_path_rate, reachable);
      }
    }
  }

  auto envelope_rate_at = [&](double s) {
    if (backward_envelope.empty()) {
      return kBig;
    }
    if (s <= backward_envelope.front().path_time_s) {
      return backward_envelope.front().max_path_rate;
    }
    if (s >= backward_envelope.back().path_time_s) {
      return backward_envelope.back().max_path_rate;
    }
    auto it = std::lower_bound(
        backward_envelope.begin(), backward_envelope.end(), s,
        [](const BackwardEnvelopePoint& point, double value) { return point.path_time_s < value; });
    if (it == backward_envelope.begin()) {
      return it->max_path_rate;
    }
    const auto& right = *it;
    const auto& left = *(it - 1);
    const double span = std::max(right.path_time_s - left.path_time_s, kEpsilon);
    const double alpha = std::clamp((s - left.path_time_s) / span, 0.0, 1.0);
    return left.max_path_rate + alpha * (right.max_path_rate - left.max_path_rate);
  };

  // Forward-integrate with target path-rate deceleration `a_decel`: slew ü
  // toward the budget-clamped target within the jerk bound, and ramp ü back to
  // zero on the S-curve approach to rest (start when ramping ü->0 lands exactly
  // at u̇=0). Records (t, s, u̇, ü) samples; returns (stop_time, reaches_rest).
  PathSample integrate_sample;
  auto integrate = [&](double a_decel, std::vector<JerkLimitedStopSampleDTO>* out,
                       bool* used_envelope) -> std::pair<double, bool> {
    double s = start_path_time_s;
    double v = start_path_rate;
    double a = start_path_rate_accel;
    double t = 0.0;
    if (out) {
      out->clear();
      out->push_back({t, s, v, a});
    }
    const double dt = sample_period_s;
    const size_t kMaxSteps = 2'000'000;
    for (size_t steps = 0; steps < kMaxSteps; ++steps) {
      path_cursor.sample(s, integrate_sample);
      const PathSample& smp = integrate_sample;
      double lo = 0.0;
      double hi = 0.0;
      double jerk_lo = -kBig;
      double jerk_hi = kBig;
      if (!budgets(smp, v, a, &lo, &hi, &jerk_lo, &jerk_hi)) {
        if (out) {
          if (out->empty() || std::abs(out->back().time_from_ramp_start_s - t) > kEpsilon) {
            out->push_back({t, s, 0.0, 0.0});
          } else {
            out->back() = {t, s, 0.0, 0.0};
          }
        }
        return {t, true};
      }
      const double hi_eff = std::min(hi, 0.0);
      if (lo > hi_eff + kAccelTolerance || jerk_lo > jerk_hi) {
        return {t, false};
      }
      if (!std::isfinite(jerk_lo) || !std::isfinite(jerk_hi)) {
        return {t, false};
      }
      double jerk = 0.0;
      double dt_step = dt;
      bool target_terminal_branch = false;
      const double remaining_to_target = target_stop_time_s - t;
      if (a < -kEpsilon && remaining_to_target > kEpsilon) {
        const double required_accel_now = -2.0 * v / remaining_to_target;
        const double jerk_to_zero_at_target = -a / remaining_to_target;
        const double accel_margin =
            std::abs(required_accel_now) * 0.05 + std::max(0.0, jerk_hi) * dt;
        if (a <= required_accel_now + accel_margin && jerk_to_zero_at_target >= jerk_lo &&
            jerk_to_zero_at_target <= jerk_hi) {
          // Target-aware terminal branch: ramp path acceleration to zero at the
          // requested stop time. Bisection tunes the preceding decel so velocity
          // reaches zero at the same instant.
          jerk = jerk_to_zero_at_target;
          target_terminal_branch = true;
          if (remaining_to_target <= dt + kEpsilon) {
            dt_step = std::max(0.0, remaining_to_target);
          }
        }
      }
      if (a < -kEpsilon && v > kEpsilon && jerk_hi > kEpsilon) {
        const double terminal_velocity_at_max_jerk = (a * a) / (2.0 * jerk_hi);
        const double one_step_velocity_margin = std::abs(a) * dt + 0.5 * jerk_hi * dt * dt;
        if (!target_terminal_branch &&
            v <= terminal_velocity_at_max_jerk + one_step_velocity_margin + kEpsilon) {
          // Terminal S-curve branch. Wait until the max-jerk terminal envelope
          // is close, then use the exact jerk when it is within the bound. This
          // avoids starting an extremely gentle endpoint ramp while the robot is
          // still far from rest.
          const double endpoint_jerk = (a * a) / (2.0 * v);
          jerk = std::clamp(endpoint_jerk, jerk_lo, jerk_hi);
        }
      }
      const double envelope_rate =
          std::min(envelope_rate_at(s), envelope_rate_at(std::min(path_end_s, s + v * dt_step)));
      double desired = -a_decel;
      const bool envelope_binding =
          std::isfinite(envelope_rate) &&
          (v > envelope_rate + std::max(1e-6, 0.01 * envelope_rate) ||
           v + desired * dt_step > envelope_rate + std::max(1e-6, 0.01 * envelope_rate));
      if (envelope_binding) {
        if (used_envelope != nullptr) {
          *used_envelope = true;
        }
        // Search and emitted integration must use identical dynamics;
        // requesting a diagnostic must never change the candidate.
        desired = std::min(desired, (envelope_rate - v) / std::max(dt_step, kEpsilon));
      }
      if (std::abs(jerk) <= kEpsilon) {
        const double target = std::clamp(desired, std::min(lo, hi_eff), hi_eff);
        jerk = std::clamp((target - a) / std::max(dt_step, kEpsilon), jerk_lo, jerk_hi);
      }

      const double a_next = a + jerk * dt_step;
      const double v_next = v + a * dt_step + 0.5 * jerk * dt_step * dt_step;
      if (accepted_trajectory.runtime_scale_bounds) {
        const double rate_interval =
            v_next <= 0.0 ? std::clamp(solve_velocity_zero(v, a, jerk, dt_step), 0.0, dt_step)
                          : dt_step;
        if (!constant_jerk_rate_within_unit_range(v, a, jerk, rate_interval)) return {t, false};
      }
      if (target_terminal_branch && dt_step < dt + kEpsilon &&
          std::abs(remaining_to_target - dt_step) <= kEpsilon &&
          std::abs(v_next) <= kTerminalTolerance && std::abs(a_next) <= kTerminalTolerance) {
        s += v * dt_step + 0.5 * a * dt_step * dt_step + (jerk * dt_step * dt_step * dt_step) / 6.0;
        t += dt_step;
        if (out) {
          if (out->empty() || t > out->back().time_from_ramp_start_s + kEpsilon) {
            out->push_back({t, s, 0.0, 0.0});
          } else {
            out->back() = {t, s, 0.0, 0.0};
          }
        }
        return {t, s <= path_end_s + kEpsilon};
      }
      if (v_next <= 0.0) {
        const double dt_zero = std::clamp(solve_velocity_zero(v, a, jerk, dt_step), 0.0, dt_step);
        s += v * dt_zero + 0.5 * a * dt_zero * dt_zero + (jerk * dt_zero * dt_zero * dt_zero) / 6.0;
        t += dt_zero;
        const double terminal_accel = a + jerk * dt_zero;
        const bool stop_time_matches_target =
            std::abs(t - target_stop_time_s) <= target_time_tolerance_s;
        const double emitted_accel =
            (stop_time_matches_target || std::abs(terminal_accel) <= kTerminalTolerance)
                ? 0.0
                : terminal_accel;
        if (out) {
          if (out->empty() || t > out->back().time_from_ramp_start_s + kEpsilon) {
            out->push_back({t, s, 0.0, emitted_accel});
          } else {
            out->back() = {t, s, 0.0, emitted_accel};
          }
        }
        return {t, s <= path_end_s + kEpsilon};
      }

      s += v * dt_step + 0.5 * a * dt_step * dt_step + (jerk * dt_step * dt_step * dt_step) / 6.0;
      t += dt_step;
      v = v_next;
      a = a_next;
      if (out) {
        out->push_back({t, s, v, a});
      }
      if (s > path_end_s + kEpsilon) {
        return {t, false};
      }
    }
    return {t, false};
  };

  PathSample build_sample;
  auto build_joint_trajectory = [&](const std::vector<JerkLimitedStopSampleDTO>& samples) {
    TrajectoryDTO trajectory;
    trajectory.joint_names = accepted_trajectory.joint_names;
    trajectory.waypoints.reserve(samples.size());
    for (const auto& smp : samples) {
      path_cursor.sample(smp.path_time_s, build_sample);
      const PathSample& ps = build_sample;
      TrajectoryWaypointDTO wp;
      path_waypoint(ps, smp.time_from_ramp_start_s, smp.path_rate, smp.path_rate_accel, wp);
      trajectory.waypoints.push_back(std::move(wp));
    }
    return trajectory;
  };

  auto validate_dispatch_trajectory = [&](JerkLimitedStopProfileDTO& profile) {
    auto mark_invalid = [&](const std::string& reason) {
      if (profile.validation_failure_reason.empty()) {
        profile.validation_failure_reason = reason;
      }
      profile.within_budget = false;
    };
    profile.hits_target_time =
        profile.reaches_rest_on_path &&
        std::abs(profile.achieved_stop_time_s - target_stop_time_s) <= target_time_tolerance_s;
    profile.terminal_state_zero = false;
    profile.within_budget = profile.reaches_rest_on_path && profile.samples.size() >= 2;
    if (profile.samples.empty()) {
      profile.dispatchable = false;
      return;
    }

    const auto& first = profile.samples.front();
    if (std::abs(first.time_from_ramp_start_s) > kEpsilon ||
        std::abs(first.path_time_s - start_path_time_s) > 1e-7 ||
        std::abs(first.path_rate - start_path_rate) > 1e-7 ||
        std::abs(first.path_rate_accel - start_path_rate_accel) > 1e-7) {
      mark_invalid("seam_sample_mismatch");
    }
    for (size_t i = 1; i < profile.samples.size(); ++i) {
      const auto& prev = profile.samples[i - 1];
      const auto& curr = profile.samples[i];
      if (!std::isfinite(curr.time_from_ramp_start_s) || !std::isfinite(curr.path_time_s) ||
          !std::isfinite(curr.path_rate) || !std::isfinite(curr.path_rate_accel) ||
          curr.time_from_ramp_start_s <= prev.time_from_ramp_start_s + kEpsilon) {
        mark_invalid("sample_time_or_value_invalid");
      }
      if (accepted_trajectory.runtime_scale_bounds) {
        const double dt = curr.time_from_ramp_start_s - prev.time_from_ramp_start_s;
        if (curr.path_rate < -1e-12 || curr.path_rate > 1.0 + 1e-12 ||
            !constant_jerk_rate_within_unit_range(
                prev.path_rate, prev.path_rate_accel,
                (curr.path_rate_accel - prev.path_rate_accel) / dt, dt)) {
          mark_invalid("path_rate_limit_exceeded");
        }
      }
    }

    const auto& terminal = profile.samples.back();
    profile.terminal_state_zero = std::abs(terminal.path_rate) <= kTerminalTolerance &&
                                  std::abs(terminal.path_rate_accel) <= kTerminalTolerance;
    if (!profile.terminal_state_zero) {
      mark_invalid("terminal_state_not_zero");
    }

    profile.emitted_trajectory = build_joint_trajectory(profile.samples);
    const auto shape = validate_trajectory_shape(profile.emitted_trajectory);
    if (!shape.ok()) {
      mark_invalid("emitted_trajectory_shape_invalid");
      profile.dispatchable = false;
      return;
    }

    const auto& points = profile.emitted_trajectory.waypoints;
    for (size_t i = 1; i < points.size(); ++i) {
      if (const auto failure = segment_limit_failure(points[i - 1], points[i], max_decel, max_jerk))
        mark_invalid(failure->reason);
    }

    profile.dispatchable = profile.reaches_rest_on_path && profile.hits_target_time &&
                           profile.terminal_state_zero && profile.within_budget;
  };

  // main's closed-form straight-path S-curve, tried before the generic search
  // (restored with the removal of the affine shortcut, decision Q23).
  auto straight_scurve_candidate = [&]() -> std::optional<JerkLimitedStopProfileDTO> {
    if (std::abs(start_path_rate_accel) > kTerminalTolerance) {
      return std::nullopt;
    }
    const PathSample start_sample = path_cursor.sample(start_path_time_s);
    const double peak_path_accel = 2.0 * start_path_rate / target_stop_time_s;
    const double path_jerk = 4.0 * start_path_rate / (target_stop_time_s * target_stop_time_s);
    double path_accel_limit = kBig;
    double path_jerk_limit = kBig;
    bool found_tangent = false;
    for (size_t j = 0; j < joint_count; ++j) {
      if (std::abs(start_sample.d2_position_dt2[j]) > kTerminalTolerance) {
        return std::nullopt;
      }
      const double qp = start_sample.d_position_dt[j];
      if (std::abs(qp) <= kEpsilon) {
        continue;
      }
      path_accel_limit = std::min(path_accel_limit, max_decel[j] / std::abs(qp));
      path_jerk_limit = std::min(path_jerk_limit, max_jerk[j] / std::abs(qp));
      found_tangent = true;
    }
    if (!found_tangent || peak_path_accel > path_accel_limit + kAccelTolerance ||
        path_jerk > path_jerk_limit + kJerkTolerance) {
      return std::nullopt;
    }

    const double stop_distance = 0.5 * start_path_rate * target_stop_time_s;
    const double stop_path_time = start_path_time_s + stop_distance;
    if (stop_path_time > path_end_s + kEpsilon) {
      return std::nullopt;
    }
    const PathSample mid_sample = path_cursor.sample(start_path_time_s + 0.5 * stop_distance);
    const PathSample stop_sample = path_cursor.sample(stop_path_time);
    for (size_t j = 0; j < joint_count; ++j) {
      if (std::abs(mid_sample.d2_position_dt2[j]) > kTerminalTolerance ||
          std::abs(stop_sample.d2_position_dt2[j]) > kTerminalTolerance ||
          std::abs(mid_sample.d_position_dt[j] - start_sample.d_position_dt[j]) > 1e-6 ||
          std::abs(stop_sample.d_position_dt[j] - start_sample.d_position_dt[j]) > 1e-6) {
        return std::nullopt;
      }
    }

    JerkLimitedStopProfileDTO candidate;
    candidate.target_decel_path_rate = peak_path_accel;
    candidate.achieved_stop_time_s = target_stop_time_s;
    candidate.reaches_rest_on_path = true;
    const size_t steps =
        std::max<size_t>(2, static_cast<size_t>(std::ceil(target_stop_time_s / sample_period_s)));
    const double half_t = 0.5 * target_stop_time_s;
    const double v_mid = 0.5 * start_path_rate;
    const double s_mid = start_path_rate * half_t - (path_jerk * half_t * half_t * half_t) / 6.0;
    for (size_t i = 0; i <= steps; ++i) {
      const double t = target_stop_time_s * static_cast<double>(i) / static_cast<double>(steps);
      double v = 0.0;
      double a = 0.0;
      double s_rel = 0.0;
      if (t <= half_t) {
        a = -path_jerk * t;
        v = start_path_rate - 0.5 * path_jerk * t * t;
        s_rel = start_path_rate * t - (path_jerk * t * t * t) / 6.0;
      } else {
        const double tau = t - half_t;
        a = -peak_path_accel + path_jerk * tau;
        v = v_mid - peak_path_accel * tau + 0.5 * path_jerk * tau * tau;
        s_rel = s_mid + v_mid * tau - 0.5 * peak_path_accel * tau * tau +
                (path_jerk * tau * tau * tau) / 6.0;
      }
      if (i == steps) {
        v = 0.0;
        a = 0.0;
        s_rel = stop_distance;
      }
      candidate.samples.push_back({t, start_path_time_s + s_rel, std::max(0.0, v), a});
    }
    validate_dispatch_trajectory(candidate);
    return candidate;
  };

  if (auto candidate = straight_scurve_candidate();
      candidate.has_value() && candidate->dispatchable) {
    return Result<JerkLimitedStopProfileDTO>::Ok(std::move(*candidate));
  }

  // Bisection on the target deceleration so the ramp lands rest at
  // target_stop_time_s (stop time decreases as the target grows; an unreachable
  // run reads as "too gentle"). Jerk-limiting bounds the fastest stop, so the
  // achieved time may exceed the target — reported for the caller to judge.
  auto stop_time = [&](double a_decel) {
    const auto [t, reached] = integrate(a_decel, nullptr, nullptr);
    return reached ? t : kBig;
  };
  // The integrate() sweep is deterministic in a_decel, so each bracket
  // endpoint's stop time is evaluated exactly once and carried through
  // the search instead of being re-integrated.
  double a_max = 1.0;
  double t_at_a_max = stop_time(a_max);
  for (int grow = 0; grow < 64 && t_at_a_max > target_stop_time_s; ++grow) {
    a_max *= 2.0;
    t_at_a_max = stop_time(a_max);
  }
  double a_min = a_max;
  double t_at_a_min = t_at_a_max;
  for (int shrink = 0; shrink < 80 && t_at_a_min < target_stop_time_s; ++shrink) {
    a_min *= 0.5;
    t_at_a_min = stop_time(a_min);
  }
  for (int i = 0; i < 64; ++i) {
    const double a_mid = 0.5 * (a_min + a_max);
    if (a_mid == a_min || a_mid == a_max) {
      break;  // bracket collapsed below double resolution; no-op iterations
    }
    const double t_mid = stop_time(a_mid);
    if (t_mid >= target_stop_time_s) {
      a_min = a_mid;
      t_at_a_min = t_mid;
    } else {
      a_max = a_mid;
      t_at_a_max = t_mid;
    }
  }

  const double t_min_candidate = t_at_a_min;
  const double t_max_candidate = t_at_a_max;
  const double selected_decel = std::abs(t_min_candidate - target_stop_time_s) <
                                        std::abs(t_max_candidate - target_stop_time_s)
                                    ? a_min
                                    : a_max;

  JerkLimitedStopProfileDTO out;
  out.target_decel_path_rate = selected_decel;
  bool used_backward_envelope = false;
  const auto [achieved, reached] = integrate(selected_decel, &out.samples, &used_backward_envelope);
  out.achieved_stop_time_s = achieved;
  out.reaches_rest_on_path = reached;
  out.used_backward_envelope = used_backward_envelope;

  validate_dispatch_trajectory(out);
  return Result<JerkLimitedStopProfileDTO>::Ok(std::move(out));
}

}  // namespace manyforge::core
