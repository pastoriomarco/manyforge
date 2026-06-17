#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>

#include <manyforge_core/trajectory/trajectory_sampler.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr double kEpsilon = 1e-9;

std::string format_double(double value) {
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
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

Result<SoftOnPathStopPlanDTO> err(
    std::string reason,
    std::string message,
    InterruptStructuredValue details = {}) {
  details["reason"] = reason;
  if (!details.contains("recommended_fallback") &&
      !details.contains("recommended_disposition")) {
    details["recommended_fallback"] = "spring_back_hold";
  }
  return Result<SoftOnPathStopPlanDTO>::Err(ErrorDTO{
      std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
      std::move(message),
      std::move(details)});
}

bool finite_positive(double value) {
  return std::isfinite(value) && value > 0.0;
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

std::vector<double> subtract(const std::vector<double>& a,
                             const std::vector<double>& b) {
  std::vector<double> out(a.size(), 0.0);
  for (size_t i = 0; i < a.size(); ++i) {
    out[i] = a[i] - b[i];
  }
  return out;
}

std::vector<double> add_scaled(const std::vector<double>& a,
                               const std::vector<double>& b,
                               double scale) {
  std::vector<double> out(a.size(), 0.0);
  for (size_t i = 0; i < a.size(); ++i) {
    out[i] = a[i] + scale * b[i];
  }
  return out;
}

std::optional<size_t> find_joint(const std::vector<std::string>& names,
                                 const std::string& target) {
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

Result<OrderedState> order_state_for_trajectory(
    const RobotStateDTO& state,
    const std::vector<std::string>& joint_names) {
  if (joint_names.empty()) {
    return Result<OrderedState>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "soft_on_path trajectory has no joint names",
        {{"reason", "trajectory_joint_names_empty"}}});
  }
  OrderedState ordered;
  ordered.positions.resize(joint_names.size(), 0.0);
  ordered.velocities.resize(joint_names.size(), 0.0);

  if (state.joint_names.empty()) {
    if (state.positions_rad.size() != joint_names.size()) {
      return Result<OrderedState>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path observed position size does not match trajectory joints",
          {{"reason", "observed_position_size_mismatch"},
           {"joint_count", std::to_string(joint_names.size())},
           {"position_count", std::to_string(state.positions_rad.size())}}});
    }
    ordered.positions = state.positions_rad;
    if (state.velocities_rad_s.size() == joint_names.size()) {
      ordered.velocities = state.velocities_rad_s;
      ordered.has_velocity = true;
    }
    return Result<OrderedState>::Ok(std::move(ordered));
  }

  if (state.positions_rad.size() != state.joint_names.size()) {
    return Result<OrderedState>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "soft_on_path observed state joint/position sizes differ",
        {{"reason", "observed_state_size_mismatch"},
         {"state_joint_count", std::to_string(state.joint_names.size())},
         {"position_count", std::to_string(state.positions_rad.size())}}});
  }
  const bool state_has_velocity =
      state.velocities_rad_s.size() == state.joint_names.size();
  for (size_t i = 0; i < joint_names.size(); ++i) {
    const auto idx = find_joint(state.joint_names, joint_names[i]);
    if (!idx.has_value()) {
      return Result<OrderedState>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path observed state is missing a trajectory joint",
          {{"reason", "observed_state_missing_joint"},
           {"joint", joint_names[i]}}});
    }
    ordered.positions[i] = state.positions_rad[*idx];
    if (state_has_velocity) {
      ordered.velocities[i] = state.velocities_rad_s[*idx];
    }
  }
  ordered.has_velocity = state_has_velocity;
  return Result<OrderedState>::Ok(std::move(ordered));
}

Result<void> validate_trajectory_shape(const TrajectoryDTO& trajectory) {
  if (trajectory.joint_names.empty()) {
    return Result<void>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "soft_on_path requires trajectory joint names",
        {{"reason", "trajectory_joint_names_empty"}}});
  }
  if (trajectory.waypoints.size() < 2) {
    return Result<void>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "soft_on_path requires at least two trajectory waypoints",
        {{"reason", "trajectory_too_short"}}});
  }
  double previous_time = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    const auto& wp = trajectory.waypoints[i];
    if (!std::isfinite(wp.time_from_start_s) ||
        wp.time_from_start_s <= previous_time) {
      return Result<void>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path trajectory waypoint times must be finite and increasing",
          {{"reason", "trajectory_time_not_strictly_increasing"},
           {"waypoint_index", std::to_string(i)}}});
    }
    if (wp.positions_rad.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path waypoint position size does not match joint names",
          {{"reason", "trajectory_position_size_mismatch"},
           {"waypoint_index", std::to_string(i)}}});
    }
    if (!wp.velocities_rad_s.empty() &&
        wp.velocities_rad_s.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path waypoint velocity size does not match joint names",
          {{"reason", "trajectory_velocity_size_mismatch"},
           {"waypoint_index", std::to_string(i)}}});
    }
    if (!wp.accelerations_rad_s2.empty() &&
        wp.accelerations_rad_s2.size() != trajectory.joint_names.size()) {
      return Result<void>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kInvalidRequest)),
          "soft_on_path waypoint acceleration size does not match joint names",
          {{"reason", "trajectory_acceleration_size_mismatch"},
           {"waypoint_index", std::to_string(i)}}});
    }
    previous_time = wp.time_from_start_s;
  }
  return Result<void>::Ok();
}

struct PathSample {
  std::vector<double> position;
  std::vector<double> d_position_dt;
  std::vector<double> d2_position_dt2;
};

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

Projection project_to_path(const TrajectoryDTO& trajectory,
                           const std::vector<double>& q) {
  Projection best;
  best.distance_rad = std::numeric_limits<double>::infinity();
  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const auto& prev = trajectory.waypoints[i - 1];
    const auto& next = trajectory.waypoints[i];
    const auto segment = subtract(next.positions_rad, prev.positions_rad);
    const auto from_prev = subtract(q, prev.positions_rad);
    const double denom = dot(segment, segment);
    const double alpha =
        denom <= kEpsilon ? 0.0 : std::clamp(dot(from_prev, segment) / denom, 0.0, 1.0);
    const auto projected = add_scaled(prev.positions_rad, segment, alpha);
    const double distance = norm(subtract(q, projected));
    if (distance < best.distance_rad) {
      best.distance_rad = distance;
      best.time_from_start_s =
          prev.time_from_start_s +
          alpha * (next.time_from_start_s - prev.time_from_start_s);
    }
  }
  return best;
}

double default_jerk_limit(double accel_limit, double effective_ramp_s) {
  const double jerk_ramp_s =
      std::min(std::max(0.15 * effective_ramp_s, 0.02),
               effective_ramp_s / 3.0);
  return accel_limit / std::max(jerk_ramp_s, kEpsilon);
}

bool planner_reason_allows_spring_back_fallback(const std::string& reason) {
  return reason == "soft_on_path_remaining_time_short" ||
         reason == "soft_on_path_acceleration_limit_exceeded" ||
         reason == "soft_on_path_jerk_limit_exceeded" ||
         reason == "soft_on_path_handoff_acceleration_limit_exceeded" ||
         reason == "soft_on_path_handoff_jerk_limit_exceeded" ||
         reason == "initial_path_deviation_exceeds_limit" ||
         reason == "initial_velocity_deviation_exceeds_limit";
}

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
};

TrajectoryDTO build_path_rate_trajectory(
    const TrajectoryDTO& accepted_trajectory,
    const std::vector<JerkLimitedStopSampleDTO>& samples) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = accepted_trajectory.joint_names;
  const size_t joint_count = accepted_trajectory.joint_names.size();
  trajectory.waypoints.reserve(samples.size());
  for (const auto& sample : samples) {
    const PathSample path_sample =
        sample_path(accepted_trajectory, sample.path_time_s);
    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = sample.time_from_ramp_start_s;
    wp.positions_rad = path_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] =
          path_sample.d_position_dt[j] * sample.path_rate;
      wp.accelerations_rad_s2[j] =
          path_sample.d2_position_dt2[j] * sample.path_rate *
              sample.path_rate +
          path_sample.d_position_dt[j] * sample.path_rate_accel;
    }
    trajectory.waypoints.push_back(std::move(wp));
  }
  return trajectory;
}

void validate_path_rate_stop_profile(
    const TrajectoryDTO& accepted_trajectory,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3,
    double start_path_time_s,
    double start_path_rate,
    double start_path_rate_accel,
    double target_stop_time_s,
    double sample_period_s,
    AliStopProfileDTO& profile) {
  constexpr double kTerminalTolerance = 1e-3;
  constexpr double kAccelTolerance = 1e-6;
  constexpr double kJerkTolerance = 1e-6;
  const double target_time_tolerance_s =
      std::max(2.0 * sample_period_s, 1e-3);
  const size_t joint_count = accepted_trajectory.joint_names.size();
  auto mark_invalid = [&](const std::string& reason) {
    if (profile.validation_failure_reason.empty()) {
      profile.validation_failure_reason = reason;
    }
    profile.within_budget = false;
  };

  profile.hits_target_time =
      std::abs(profile.achieved_stop_time_s - target_stop_time_s) <=
      target_time_tolerance_s;
  profile.terminal_state_zero = false;
  profile.within_budget =
      profile.reaches_rest_on_path && profile.samples.size() >= 2;
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
    if (!std::isfinite(curr.time_from_ramp_start_s) ||
        !std::isfinite(curr.path_time_s) ||
        !std::isfinite(curr.path_rate) ||
        !std::isfinite(curr.path_rate_accel) ||
        curr.time_from_ramp_start_s <=
            prev.time_from_ramp_start_s + kEpsilon ||
        curr.path_time_s + 1e-8 < prev.path_time_s) {
      mark_invalid("sample_time_or_value_invalid");
    }
  }

  const auto& terminal = profile.samples.back();
  profile.terminal_state_zero =
      std::abs(terminal.path_rate) <= kTerminalTolerance &&
      std::abs(terminal.path_rate_accel) <= kTerminalTolerance;
  if (!profile.terminal_state_zero) {
    mark_invalid("terminal_state_not_zero");
  }

  const TrajectoryDTO emitted =
      build_path_rate_trajectory(accepted_trajectory, profile.samples);
  const auto shape = validate_trajectory_shape(emitted);
  if (!shape.ok()) {
    mark_invalid("emitted_trajectory_shape_invalid");
    profile.dispatchable = false;
    return;
  }

  const double validate_dt =
      std::max(sample_period_s / 4.0, target_stop_time_s / 2000.0);
  const size_t steps = std::max<size_t>(
      1, static_cast<size_t>(std::ceil(profile.achieved_stop_time_s /
                                       std::max(validate_dt, kEpsilon))));
  std::vector<double> prev_accel;
  double prev_t = 0.0;
  for (size_t i = 0; i <= steps; ++i) {
    const double t_sample =
        profile.achieved_stop_time_s * static_cast<double>(i) /
        static_cast<double>(steps);
    auto sampled = sample_trajectory(emitted, t_sample);
    if (!sampled.ok() ||
        sampled.value().accelerations_rad_s2.size() != joint_count) {
      mark_invalid("emitted_trajectory_sample_invalid");
      continue;
    }
    const auto accel = sampled.value().accelerations_rad_s2;
    for (size_t j = 0; j < joint_count; ++j) {
      const auto& joint_name = accepted_trajectory.joint_names[j];
      const double accel_limit =
          per_joint_max_deceleration_rad_s2.at(joint_name);
      if (!std::isfinite(accel[j]) ||
          std::abs(accel[j]) > accel_limit + kAccelTolerance) {
        mark_invalid("interpolated_acceleration_limit_exceeded");
      }
    }
    if (!prev_accel.empty()) {
      const double dt_check = t_sample - prev_t;
      if (dt_check > kEpsilon) {
        for (size_t j = 0; j < joint_count; ++j) {
          const auto& joint_name = accepted_trajectory.joint_names[j];
          const double jerk = (accel[j] - prev_accel[j]) / dt_check;
          const double jerk_limit = per_joint_max_jerk_rad_s3.at(joint_name);
          if (!std::isfinite(jerk) ||
              std::abs(jerk) > jerk_limit + kJerkTolerance) {
            mark_invalid("interpolated_jerk_limit_exceeded");
          }
        }
      }
    }
    prev_accel = accel;
    prev_t = t_sample;
  }

  profile.dispatchable = profile.reaches_rest_on_path &&
                         profile.hits_target_time &&
                         profile.terminal_state_zero &&
                         profile.within_budget;
}

Result<AliStopProfileDTO> compute_ali_backtracking_stop_profile(
    const TrajectoryDTO& accepted_trajectory,
    double start_path_time_s,
    double start_path_rate,
    double start_path_rate_accel,
    double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3,
    double sample_period_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<AliStopProfileDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<AliStopProfileDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)), std::move(message),
        {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(start_path_rate) || start_path_rate <= 0.0) {
    return invalid("invalid_start_path_rate",
                   "ALI retimer requires a positive start_path_rate");
  }
  if (!std::isfinite(start_path_rate_accel)) {
    return invalid("invalid_start_path_rate_accel",
                   "start_path_rate_accel must be finite");
  }
  if (!finite_positive(target_stop_time_s) ||
      !finite_positive(sample_period_s)) {
    return invalid("invalid_ali_timing",
                   "ALI retimer requires positive target_stop_time_s and "
                   "sample_period_s");
  }
  const double path_start_s =
      accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s =
      accepted_trajectory.waypoints.back().time_from_start_s;
  if (!std::isfinite(start_path_time_s) ||
      start_path_time_s < path_start_s - kEpsilon ||
      start_path_time_s > path_end_s + kEpsilon) {
    return invalid("invalid_start_path_time",
                   "start_path_time_s must be finite and inside the accepted "
                   "trajectory time range");
  }
  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const auto d_it = per_joint_max_deceleration_rad_s2.find(joint_name);
    const auto j_it = per_joint_max_jerk_rad_s3.find(joint_name);
    if (d_it == per_joint_max_deceleration_rad_s2.end() ||
        !finite_positive(d_it->second) ||
        j_it == per_joint_max_jerk_rad_s3.end() ||
        !finite_positive(j_it->second)) {
      return invalid(
          "missing_per_joint_limit",
          "per-joint deceleration and jerk limits are required for " +
              joint_name);
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
    return Result<AliStopProfileDTO>::Ok(
        failure_profile("ali_nonzero_start_accel_unsupported"));
  }

  // ALI is the third-line candidate after the exact-time quintic and generic
  // jerk-limited retimer fail. It is allowed to stop the moving portion before
  // a future bottleneck, then hold until the requested stop horizon, but only
  // if the emitted derivative-complete trajectory passes the same oversampled
  // accel/jerk validation used by the other candidates.
  auto build_triangular_scurve_candidate =
      [&](double brake_time_s, size_t backtracking_iterations) {
        AliStopProfileDTO profile;
        profile.achieved_stop_time_s = target_stop_time_s;
        profile.motion_stop_time_s = brake_time_s;
        profile.backtracking_iterations = backtracking_iterations;
        profile.samples.reserve(
            static_cast<size_t>(std::ceil(target_stop_time_s /
                                          sample_period_s)) +
            2);

        const double path_jerk =
            4.0 * start_path_rate / (brake_time_s * brake_time_s);
        const double peak_path_accel = 0.5 * path_jerk * brake_time_s;
        const double half_t = 0.5 * brake_time_s;
        const double v_mid = 0.5 * start_path_rate;
        const double s_mid =
            start_path_rate * half_t -
            (path_jerk * half_t * half_t * half_t) / 6.0;
        const double stop_distance = 0.5 * start_path_rate * brake_time_s;

        const size_t brake_steps = std::max<size_t>(
            2, static_cast<size_t>(std::ceil(brake_time_s /
                                             sample_period_s)));
        for (size_t i = 0; i <= brake_steps; ++i) {
          const double t =
              brake_time_s * static_cast<double>(i) /
              static_cast<double>(brake_steps);
          double v = 0.0;
          double a = 0.0;
          double s_rel = 0.0;
          if (t <= half_t) {
            a = -path_jerk * t;
            v = start_path_rate - 0.5 * path_jerk * t * t;
            s_rel =
                start_path_rate * t -
                (path_jerk * t * t * t) / 6.0;
          } else {
            const double tau = t - half_t;
            a = -peak_path_accel + path_jerk * tau;
            v = v_mid - peak_path_accel * tau +
                0.5 * path_jerk * tau * tau;
            s_rel = s_mid + v_mid * tau -
                    0.5 * peak_path_accel * tau * tau +
                    (path_jerk * tau * tau * tau) / 6.0;
          }
          if (i == brake_steps) {
            v = 0.0;
            a = 0.0;
            s_rel = stop_distance;
          }
          profile.samples.push_back(
              {t, start_path_time_s + s_rel, std::max(0.0, v), a});
        }

        const double stop_path_time = start_path_time_s + stop_distance;
        if (stop_path_time > path_end_s + kEpsilon) {
          profile.validation_failure_reason = "ali_stop_runs_past_path_end";
          return profile;
        }

        double next_hold_time =
            profile.samples.back().time_from_ramp_start_s + sample_period_s;
        while (next_hold_time < target_stop_time_s - kEpsilon) {
          profile.samples.push_back({next_hold_time, stop_path_time, 0.0, 0.0});
          next_hold_time += sample_period_s;
        }
        if (target_stop_time_s >
            profile.samples.back().time_from_ramp_start_s + kEpsilon) {
          profile.samples.push_back(
              {target_stop_time_s, stop_path_time, 0.0, 0.0});
        }

        profile.reaches_rest_on_path = true;
        validate_path_rate_stop_profile(
            accepted_trajectory, per_joint_max_deceleration_rad_s2,
            per_joint_max_jerk_rad_s3, start_path_time_s, start_path_rate,
            start_path_rate_accel, target_stop_time_s, sample_period_s,
            profile);
        return profile;
      };

  const size_t kCandidateCount = 32;
  AliStopProfileDTO best_failure = failure_profile("ali_no_candidate");
  for (size_t i = 0; i <= kCandidateCount; ++i) {
    const double fraction =
        1.0 - 0.9 * static_cast<double>(i) /
                  static_cast<double>(kCandidateCount);
    const double brake_time_s = target_stop_time_s * fraction;
    if (brake_time_s <= 2.0 * sample_period_s) {
      continue;
    }
    auto candidate =
        build_triangular_scurve_candidate(brake_time_s, i);
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

}  // namespace

Result<SoftOnPathStopPlanDTO> plan_soft_on_path_stop(
    const TrajectoryDTO& accepted_trajectory,
    const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state,
    const SoftOnPathPlannerConfig& config) {
  if (stop_profile.stop_mode != MotionInterruptStopMode::kSoftOnPath) {
    return err("stop_mode_not_soft_on_path",
               "soft_on_path planner called for a non-soft_on_path profile");
  }
  if (!stop_profile.deceleration_time_s ||
      !finite_positive(*stop_profile.deceleration_time_s)) {
    return err("soft_on_path_deceleration_time_required",
               "soft_on_path requires a positive deceleration_time_s");
  }
  if (!std::isfinite(config.stop_traj_delta_s) ||
      config.stop_traj_delta_s < 0.0) {
    return err("invalid_stop_traj_delta_s",
               "soft_on_path stop_traj_delta_s must be finite and >= 0");
  }
  if (!finite_positive(config.sample_period_s)) {
    return err("invalid_sample_period_s",
               "soft_on_path sample_period_s must be finite and positive");
  }
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<SoftOnPathStopPlanDTO>::Err(valid_shape.error());
  }
  const auto ordered_state_result = order_state_for_trajectory(
      initial_state.state, accepted_trajectory.joint_names);
  if (!ordered_state_result.ok()) {
    return Result<SoftOnPathStopPlanDTO>::Err(ordered_state_result.error());
  }
  const OrderedState& ordered_state = ordered_state_result.value();
  const size_t joint_count = accepted_trajectory.joint_names.size();

  const double effective_ramp_s =
      *stop_profile.deceleration_time_s - config.stop_traj_delta_s;
  if (effective_ramp_s <= kEpsilon) {
    return err("deceleration_time_not_larger_than_handoff",
               "soft_on_path deceleration_time_s must exceed stop_traj_delta_s",
               {{"deceleration_time_s", format_double(*stop_profile.deceleration_time_s)},
                {"stop_traj_delta_s", format_double(config.stop_traj_delta_s)}});
  }

  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const auto it = stop_profile.per_joint_max_deceleration_rad_s2.find(joint_name);
    if (it == stop_profile.per_joint_max_deceleration_rad_s2.end() ||
        !finite_positive(it->second)) {
      return err("missing_joint_deceleration_limit",
                 "soft_on_path requires positive per-joint acceleration limits",
                 {{"joint", joint_name}});
    }
  }

  const Projection projection = project_to_path(
      accepted_trajectory, ordered_state.positions);
  if (config.max_initial_path_deviation_rad.has_value() &&
      projection.distance_rad > *config.max_initial_path_deviation_rad) {
    return err("initial_path_deviation_exceeds_limit",
               "soft_on_path observed state is outside the configured path drift limit",
               {{"initial_path_deviation_rad", format_double(projection.distance_rad)},
                {"max_initial_path_deviation_rad",
                 format_double(*config.max_initial_path_deviation_rad)}});
  }

  const PathSample projected_sample =
      sample_path(accepted_trajectory, projection.time_from_start_s);
  const bool use_model_velocity =
      !ordered_state.has_velocity ||
      initial_state.velocity_source == SoftOnPathStateSource::kModelDerived;
  if (use_model_velocity && !config.allow_model_velocity_fallback) {
    return err("model_velocity_not_authorized",
               "soft_on_path requires measured velocity unless model fallback is authorized");
  }

  // Seam cursor (TARGET §4.8 authority): the lead-in and ramp anchor here, so it
  // MUST be the command frame, not the lagging measured-position projection —
  // anchoring on the projection re-issues points the controller's command has
  // already passed. Model/command authority uses the active command cursor
  // (trajectory_time_from_start_s, required here); the feedback lane keeps the
  // measured projection. projection.* stays the drift/velocity guard input only.
  double seam_cursor_s = projection.time_from_start_s;
  if (use_model_velocity) {
    if (!initial_state.trajectory_time_from_start_s.has_value()) {
      return err("missing_command_cursor",
                 "soft_on_path model/command authority requires "
                 "trajectory_time_from_start_s (the active command cursor)");
    }
    seam_cursor_s = *initial_state.trajectory_time_from_start_s;
  }

  double path_time_rate = 0.0;
  std::vector<double> projected_velocity(joint_count, 0.0);
  if (use_model_velocity) {
    if (!std::isfinite(initial_state.runtime_velocity_scale_at_trip) ||
        initial_state.runtime_velocity_scale_at_trip < 0.0) {
      return err("invalid_runtime_velocity_scale_at_trip",
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
          dot(ordered_state.velocities, projected_sample.d_position_dt) /
          tangent_norm2;
      if (!std::isfinite(path_time_rate)) {
        return err("invalid_projected_velocity",
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
      initial_velocity_deviation >
          *config.max_initial_velocity_deviation_rad_s) {
    return err("initial_velocity_deviation_exceeds_limit",
               "soft_on_path observed velocity is not coherent with the accepted path",
               {{"initial_velocity_deviation_rad_s",
                 format_double(initial_velocity_deviation)},
                {"max_initial_velocity_deviation_rad_s",
                 format_double(*config.max_initial_velocity_deviation_rad_s)}});
  }

  // Exact-time feasibility gate: refuse (escalate to hard stop) a soft stop
  // that cannot bring the robot to rest on this path, in the requested time,
  // within the per-joint deceleration budget. Both escalations below use the
  // fastest budget-respecting on-path stop (T_min) from the trip rate.
  {
    const auto min_stop = compute_soft_on_path_min_stop_time(
        accepted_trajectory, seam_cursor_s, path_time_rate,
        stop_profile.per_joint_max_deceleration_rad_s2);
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
            {{"min_stop_distance_path_s",
              format_double(min_stop.value().stop_distance_path_s)},
             {"recommended_disposition", "hard_stop"}});
      }
      // Requested time shorter than the fastest feasible on-path stop (T_min):
      // physically infeasible under these limits, so escalate to a hard stop
      // rather than silently running long. (T_dec >= T_min is achievable: the
      // quintic ramp below when nothing binds, or a future redistributed
      // profile.)
      if (min_stop.value().reaches_rest_on_path &&
          *stop_profile.deceleration_time_s + kEpsilon <
              min_stop.value().min_stop_time_s) {
        return err(
            "soft_on_path_deceleration_time_infeasible",
            "soft_on_path deceleration_time_s is shorter than the minimum "
            "budget-respecting on-path stop time; escalate to hard stop",
            {{"deceleration_time_s",
              format_double(*stop_profile.deceleration_time_s)},
             {"min_stop_time_s",
              format_double(min_stop.value().min_stop_time_s)},
             {"recommended_disposition", "hard_stop"}});
      }
    }
  }

  const double handoff_path_time =
      seam_cursor_s + path_time_rate * config.stop_traj_delta_s;
  const double stop_path_time =
      handoff_path_time + 0.5 * path_time_rate * effective_ramp_s;
  const double final_path_time =
      accepted_trajectory.waypoints.back().time_from_start_s;
  if (stop_path_time > final_path_time + kEpsilon) {
    InterruptStructuredValue details{
        {"projected_time_from_start_s", format_double(projection.time_from_start_s)},
        {"stop_path_time_s", format_double(stop_path_time)},
        {"trajectory_end_time_s", format_double(final_path_time)}};
    if (stop_profile.allow_natural_completion_on_short_path) {
      details["recommended_disposition"] = "natural_completion";
    }
    return err("soft_on_path_remaining_time_short",
               "soft_on_path stop ramp would pass the accepted trajectory end",
               std::move(details));
  }

  std::map<std::string, double> jerk_limits;
  bool any_default_jerk = false;
  bool any_deployment_jerk = false;
  for (const auto& joint_name : accepted_trajectory.joint_names) {
    const double accel_limit =
        stop_profile.per_joint_max_deceleration_rad_s2.at(joint_name);
    const auto jerk_it = config.per_joint_max_jerk_rad_s3.find(joint_name);
    if (jerk_it != config.per_joint_max_jerk_rad_s3.end() &&
        finite_positive(jerk_it->second)) {
      jerk_limits[joint_name] = jerk_it->second;
      any_deployment_jerk = true;
    } else {
      jerk_limits[joint_name] = default_jerk_limit(accel_limit, effective_ramp_s);
      any_default_jerk = true;
    }
  }

  SoftOnPathStopPlanDTO plan;
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
  plan.jerk_limit_source =
      any_default_jerk && any_deployment_jerk
          ? "mixed"
          : (any_deployment_jerk ? "deployment" : "default");
  plan.diagnostics["position_source"] = plan.position_source;
  plan.diagnostics["velocity_source"] = plan.velocity_source;
  plan.diagnostics["used_model_velocity"] = plan.used_model_velocity ? "true" : "false";
  plan.diagnostics["sample_age_ns"] = std::to_string(initial_state.sample_age_ns);
  plan.diagnostics["runtime_velocity_scale_at_trip"] =
      format_double(plan.runtime_velocity_scale_at_trip);
  plan.diagnostics["stop_traj_delta_s"] = format_double(plan.stop_traj_delta_s);
  plan.diagnostics["effective_ramp_s"] = format_double(plan.effective_ramp_s);
  // Seam cursor (where the lead-in/ramp anchor) vs the measured-position
  // projection: their difference is the command-vs-measured tracking lag the
  // command-frame seam corrects for.
  plan.diagnostics["seam_cursor_s"] = format_double(seam_cursor_s);
  plan.diagnostics["measured_projection_s"] =
      format_double(projection.time_from_start_s);
  plan.diagnostics["initial_path_deviation_rad"] =
      format_double(plan.initial_path_deviation_rad);
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
  plan.diagnostics["seam"] = "command_frame_lead_in";
  const double lead_in_s = config.stop_traj_delta_s;
  const size_t lead_in_samples = std::max<size_t>(
      1, static_cast<size_t>(std::ceil(lead_in_s / config.sample_period_s)));
  for (size_t i = 0; i < lead_in_samples; ++i) {
    const double tau =
        lead_in_s * static_cast<double>(i) / static_cast<double>(lead_in_samples);
    const double path_time = seam_cursor_s + path_time_rate * tau;
    const PathSample lead_sample = sample_path(accepted_trajectory, path_time);
    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = tau;
    wp.positions_rad = lead_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = lead_sample.d_position_dt[j] * path_time_rate;
      wp.accelerations_rad_s2[j] =
          lead_sample.d2_position_dt2[j] * path_time_rate * path_time_rate;
    }
    plan.replacement_trajectory.waypoints.push_back(std::move(wp));
  }

  const SoftOnPathStopPlanDTO base_plan_before_ramp = plan;
  auto try_jerk_limited_retimer =
      [&](std::string trigger_reason,
          InterruptStructuredValue trigger_details)
      -> Result<SoftOnPathStopPlanDTO> {
    auto retimed = compute_jerk_limited_stop_profile(
        accepted_trajectory, handoff_path_time, path_time_rate,
        /*start_path_rate_accel=*/0.0, effective_ramp_s,
        stop_profile.per_joint_max_deceleration_rad_s2, jerk_limits,
        config.sample_period_s);
    if (!retimed.ok()) {
      return Result<SoftOnPathStopPlanDTO>::Err(retimed.error());
    }
    const auto& profile = retimed.value();
    if (!profile.dispatchable) {
      auto ali = compute_ali_backtracking_stop_profile(
          accepted_trajectory, handoff_path_time, path_time_rate,
          /*start_path_rate_accel=*/0.0, effective_ramp_s,
          stop_profile.per_joint_max_deceleration_rad_s2, jerk_limits,
          config.sample_period_s);
      if (!ali.ok()) {
        return Result<SoftOnPathStopPlanDTO>::Err(ali.error());
      }
      const auto& ali_profile = ali.value();
      if (ali_profile.dispatchable) {
        SoftOnPathStopPlanDTO ali_plan = base_plan_before_ramp;
        ali_plan.diagnostics["soft_on_path_retimer"] = "ali_backtracking";
        ali_plan.diagnostics["retimer_trigger_reason"] = trigger_reason;
        ali_plan.diagnostics["prior_retimer"] = "jerk_limited";
        ali_plan.diagnostics["prior_retimer_validation_failure_reason"] =
            profile.validation_failure_reason;
        ali_plan.diagnostics["ali_achieved_stop_time_s"] =
            format_double(ali_profile.achieved_stop_time_s);
        ali_plan.diagnostics["ali_motion_stop_time_s"] =
            format_double(ali_profile.motion_stop_time_s);
        ali_plan.diagnostics["ali_hold_time_s"] =
            format_double(std::max(0.0, ali_profile.achieved_stop_time_s -
                                            ali_profile.motion_stop_time_s));
        ali_plan.diagnostics["ali_stopped_before_requested_time"] =
            ali_profile.motion_stop_time_s + kEpsilon <
                    ali_profile.achieved_stop_time_s
                ? "true"
                : "false";
        ali_plan.diagnostics["ali_backtracking_iterations"] =
            std::to_string(ali_profile.backtracking_iterations);
        const size_t first_ali_sample =
            config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
        for (size_t i = first_ali_sample; i < ali_profile.samples.size(); ++i) {
          const auto& sample = ali_profile.samples[i];
          const PathSample path_sample =
              sample_path(accepted_trajectory, sample.path_time_s);
          TrajectoryWaypointDTO wp;
          wp.time_from_start_s =
              config.stop_traj_delta_s + sample.time_from_ramp_start_s;
          wp.positions_rad = path_sample.position;
          wp.velocities_rad_s.resize(joint_count, 0.0);
          wp.accelerations_rad_s2.resize(joint_count, 0.0);
          for (size_t j = 0; j < joint_count; ++j) {
            wp.velocities_rad_s[j] =
                path_sample.d_position_dt[j] * sample.path_rate;
            wp.accelerations_rad_s2[j] =
                path_sample.d2_position_dt2[j] * sample.path_rate *
                    sample.path_rate +
                path_sample.d_position_dt[j] * sample.path_rate_accel;
          }
          ali_plan.replacement_trajectory.waypoints.push_back(std::move(wp));
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
      trigger_details["reason"] = "soft_on_path_retimer_not_dispatchable";
      trigger_details["trigger_reason"] = trigger_reason;
      trigger_details["retimer_validation_failure_reason"] =
          profile.validation_failure_reason;
      trigger_details["retimer_achieved_stop_time_s"] =
          format_double(profile.achieved_stop_time_s);
      trigger_details["retimer_used_backward_envelope"] =
          profile.used_backward_envelope ? "true" : "false";
      trigger_details["retimer_reaches_rest_on_path"] =
          profile.reaches_rest_on_path ? "true" : "false";
      trigger_details["retimer_hits_target_time"] =
          profile.hits_target_time ? "true" : "false";
      trigger_details["ali_validation_failure_reason"] =
          ali_profile.validation_failure_reason;
      trigger_details["ali_achieved_stop_time_s"] =
          format_double(ali_profile.achieved_stop_time_s);
      trigger_details["ali_motion_stop_time_s"] =
          format_double(ali_profile.motion_stop_time_s);
      trigger_details["ali_hold_time_s"] =
          format_double(std::max(0.0, ali_profile.achieved_stop_time_s -
                                          ali_profile.motion_stop_time_s));
      trigger_details["ali_backtracking_iterations"] =
          std::to_string(ali_profile.backtracking_iterations);
      trigger_details["ali_reaches_rest_on_path"] =
          ali_profile.reaches_rest_on_path ? "true" : "false";
      trigger_details["ali_hits_target_time"] =
          ali_profile.hits_target_time ? "true" : "false";
      trigger_details["recommended_disposition"] = "hard_stop";
      return err(
          "soft_on_path_retimer_not_dispatchable",
          "soft_on_path fixed ramp exceeded a limit and the jerk-limited "
          "retimer plus ALI backtracking could not prove a dispatchable "
          "replacement; escalate to hard stop",
          std::move(trigger_details));
    }

    SoftOnPathStopPlanDTO retimed_plan = base_plan_before_ramp;
    retimed_plan.diagnostics["soft_on_path_retimer"] = "jerk_limited";
    retimed_plan.diagnostics["retimer_trigger_reason"] = trigger_reason;
    retimed_plan.diagnostics["retimer_achieved_stop_time_s"] =
        format_double(profile.achieved_stop_time_s);
    retimed_plan.diagnostics["retimer_target_decel_path_rate"] =
        format_double(profile.target_decel_path_rate);
    retimed_plan.diagnostics["retimer_used_backward_envelope"] =
        profile.used_backward_envelope ? "true" : "false";
    const size_t first_retimer_sample =
        config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
    for (size_t i = first_retimer_sample; i < profile.samples.size(); ++i) {
      const auto& sample = profile.samples[i];
      const PathSample path_sample =
          sample_path(accepted_trajectory, sample.path_time_s);
      TrajectoryWaypointDTO wp;
      wp.time_from_start_s =
          config.stop_traj_delta_s + sample.time_from_ramp_start_s;
      wp.positions_rad = path_sample.position;
      wp.velocities_rad_s.resize(joint_count, 0.0);
      wp.accelerations_rad_s2.resize(joint_count, 0.0);
      for (size_t j = 0; j < joint_count; ++j) {
        wp.velocities_rad_s[j] =
            path_sample.d_position_dt[j] * sample.path_rate;
        wp.accelerations_rad_s2[j] =
            path_sample.d2_position_dt2[j] * sample.path_rate *
                sample.path_rate +
            path_sample.d_position_dt[j] * sample.path_rate_accel;
      }
      retimed_plan.replacement_trajectory.waypoints.push_back(std::move(wp));
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

  const size_t ramp_samples = std::max<size_t>(
      1, static_cast<size_t>(std::ceil(effective_ramp_s / config.sample_period_s)));
  std::vector<double> previous_accel;
  double previous_time = 0.0;
  const size_t first_ramp_sample = config.stop_traj_delta_s <= kEpsilon ? 1 : 0;
  for (size_t i = first_ramp_sample; i <= ramp_samples; ++i) {
    const double r = static_cast<double>(i) / static_cast<double>(ramp_samples);
    const double tau = r * effective_ramp_s;
    const double path_time =
        handoff_path_time +
        path_time_rate * effective_ramp_s * ramp_position_factor(r);
    const double u_dot = path_time_rate * ramp_velocity_factor(r);
    const double u_ddot =
        path_time_rate / effective_ramp_s * ramp_acceleration_factor(r);
    const PathSample path_sample = sample_path(accepted_trajectory, path_time);

    TrajectoryWaypointDTO wp;
    wp.time_from_start_s = config.stop_traj_delta_s + tau;
    wp.positions_rad = path_sample.position;
    wp.velocities_rad_s.resize(joint_count, 0.0);
    wp.accelerations_rad_s2.resize(joint_count, 0.0);
    for (size_t j = 0; j < joint_count; ++j) {
      wp.velocities_rad_s[j] = path_sample.d_position_dt[j] * u_dot;
      wp.accelerations_rad_s2[j] =
          path_sample.d2_position_dt2[j] * u_dot * u_dot +
          path_sample.d_position_dt[j] * u_ddot;
      const auto& joint_name = accepted_trajectory.joint_names[j];
      const double accel_limit =
          stop_profile.per_joint_max_deceleration_rad_s2.at(joint_name);
      if (std::abs(wp.accelerations_rad_s2[j]) > accel_limit + 1e-8) {
        return try_jerk_limited_retimer(
            "soft_on_path_acceleration_limit_exceeded",
            {{"joint", joint_name},
             {"acceleration_rad_s2",
              format_double(wp.accelerations_rad_s2[j])},
             {"limit_rad_s2", format_double(accel_limit)}});
      }
      if (!previous_accel.empty()) {
        const double dt = wp.time_from_start_s - previous_time;
        if (dt > kEpsilon) {
          const double jerk =
              (wp.accelerations_rad_s2[j] - previous_accel[j]) / dt;
          const double jerk_limit = jerk_limits.at(joint_name);
          if (std::abs(jerk) > jerk_limit + 1e-8) {
            return try_jerk_limited_retimer(
                "soft_on_path_jerk_limit_exceeded",
                {{"joint", joint_name},
                 {"jerk_rad_s3", format_double(jerk)},
                 {"limit_rad_s3", format_double(jerk_limit)}});
          }
        }
      }
    }
    plan.path_deviation_max_rad =
        std::max(plan.path_deviation_max_rad, projection.distance_rad);
    previous_accel = wp.accelerations_rad_s2;
    previous_time = wp.time_from_start_s;
    plan.replacement_trajectory.waypoints.push_back(std::move(wp));
  }

  plan.diagnostics["handoff_path_deviation_max_rad"] =
      format_double(plan.path_deviation_max_rad);
  plan.diagnostics["path_deviation_max_rad"] =
      format_double(plan.path_deviation_max_rad);
  plan.diagnostics["soft_on_path_retimer"] = "quintic";
  plan.diagnostics["projected_time_from_start_s"] =
      format_double(plan.projected_time_from_start_s);
  plan.diagnostics["waypoint_count"] =
      std::to_string(plan.replacement_trajectory.waypoints.size());
  return Result<SoftOnPathStopPlanDTO>::Ok(std::move(plan));
}

Result<SoftOnPathFeasibilityEnvelopeDTO>
validate_soft_on_path_feasibility_envelope(
    const TrajectoryDTO& accepted_trajectory,
    const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathPlannerConfig& config) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<SoftOnPathFeasibilityEnvelopeDTO>::Err(valid_shape.error());
  }
  if (!finite_positive(config.sample_period_s)) {
    return Result<SoftOnPathFeasibilityEnvelopeDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "soft_on_path feasibility envelope requires a finite positive sample_period_s",
        {{"reason", "invalid_sample_period_s"}}});
  }

  SoftOnPathPlannerConfig envelope_config = config;
  envelope_config.allow_model_velocity_fallback = true;

  const double start_time =
      accepted_trajectory.waypoints.front().time_from_start_s;
  const double end_time =
      accepted_trajectory.waypoints.back().time_from_start_s;
  const size_t sample_count = std::max<size_t>(
      1, static_cast<size_t>(
             std::ceil((end_time - start_time) / config.sample_period_s)));

  SoftOnPathFeasibilityEnvelopeDTO envelope;
  envelope.first_sample_time_from_start_s = start_time;
  envelope.last_sample_time_from_start_s = start_time;

  for (size_t i = 0; i <= sample_count; ++i) {
    const double t =
        i == sample_count
            ? end_time
            : std::min(end_time,
                       start_time + static_cast<double>(i) * config.sample_period_s);
    const auto sample = sample_path(accepted_trajectory, t);

    SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = accepted_trajectory.joint_names;
    initial.state.positions_rad = sample.position;
    initial.trajectory_time_from_start_s = t;
    initial.position_source = SoftOnPathStateSource::kModelDerived;
    initial.velocity_source = SoftOnPathStateSource::kModelDerived;
    initial.runtime_velocity_scale_at_trip = 1.0;

    auto planned = plan_soft_on_path_stop(
        accepted_trajectory, stop_profile, initial, envelope_config);
    if (!planned.ok()) {
      ErrorDTO error = planned.error();
      const auto reason_it = error.details.find("reason");
      const std::string planner_reason =
          reason_it == error.details.end() ? "" : reason_it->second;
      const bool natural_completion_allowed =
          stop_profile.allow_natural_completion_on_short_path &&
          soft_on_path_error_recommends_natural_completion(error);
      if (natural_completion_allowed) {
        envelope.checked_samples += 1;
        envelope.natural_completion_samples += 1;
        envelope.last_sample_time_from_start_s = t;
        continue;
      }
      error.code = kErrorMotionInterruptArmFailed;
      error.message =
          "soft_on_path feasibility envelope rejected a qualified trip point";
      error.details["reason"] = "soft_on_path_feasibility_envelope_infeasible";
      error.details["planner_reason"] = planner_reason;
      error.details["first_infeasible_time_from_start_s"] = format_double(t);
      error.details["checked_samples"] = std::to_string(envelope.checked_samples);
      if (planner_reason_allows_spring_back_fallback(planner_reason) &&
          !error.details.contains("recommended_fallback")) {
        error.details["recommended_fallback"] = "spring_back_hold";
      } else if (!planner_reason_allows_spring_back_fallback(planner_reason)) {
        error.details.erase("recommended_fallback");
      }
      return Result<SoftOnPathFeasibilityEnvelopeDTO>::Err(std::move(error));
    }

    envelope.checked_samples += 1;
    envelope.last_sample_time_from_start_s = t;
    envelope.max_path_deviation_rad = std::max(
        envelope.max_path_deviation_rad, planned.value().path_deviation_max_rad);
    envelope.max_initial_velocity_deviation_rad_s = std::max(
        envelope.max_initial_velocity_deviation_rad_s,
        planned.value().initial_velocity_deviation_rad_s);
    if (envelope.jerk_limit_source.empty()) {
      envelope.jerk_limit_source = planned.value().jerk_limit_source;
    } else if (envelope.jerk_limit_source != planned.value().jerk_limit_source) {
      envelope.jerk_limit_source = "mixed";
    }
  }

  envelope.diagnostics["checked_samples"] =
      std::to_string(envelope.checked_samples);
  envelope.diagnostics["natural_completion_samples"] =
      std::to_string(envelope.natural_completion_samples);
  envelope.diagnostics["first_sample_time_from_start_s"] =
      format_double(envelope.first_sample_time_from_start_s);
  envelope.diagnostics["last_sample_time_from_start_s"] =
      format_double(envelope.last_sample_time_from_start_s);
  envelope.diagnostics["max_path_deviation_rad"] =
      format_double(envelope.max_path_deviation_rad);
  envelope.diagnostics["max_initial_velocity_deviation_rad_s"] =
      format_double(envelope.max_initial_velocity_deviation_rad_s);
  envelope.diagnostics["jerk_limit_source"] = envelope.jerk_limit_source;
  return Result<SoftOnPathFeasibilityEnvelopeDTO>::Ok(std::move(envelope));
}

bool soft_on_path_error_recommends_natural_completion(
    const ErrorDTO& error) {
  const auto reason_it = error.details.find("reason");
  const auto disposition_it = error.details.find("recommended_disposition");
  return reason_it != error.details.end() &&
         reason_it->second == "soft_on_path_remaining_time_short" &&
         disposition_it != error.details.end() &&
         disposition_it->second == "natural_completion";
}

bool soft_on_path_error_recommends_hard_stop(const ErrorDTO& error) {
  const auto disposition_it = error.details.find("recommended_disposition");
  return disposition_it != error.details.end() &&
         disposition_it->second == "hard_stop";
}

Result<double> measure_soft_on_path_path_deviation(
    const TrajectoryDTO& accepted_trajectory,
    const RobotStateDTO& observed_state) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<double>::Err(valid_shape.error());
  }
  const auto ordered_state_result =
      order_state_for_trajectory(observed_state,
                                 accepted_trajectory.joint_names);
  if (!ordered_state_result.ok()) {
    return Result<double>::Err(ordered_state_result.error());
  }
  const Projection projection = project_to_path(
      accepted_trajectory, ordered_state_result.value().positions);
  if (!std::isfinite(projection.distance_rad)) {
    return Result<double>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kMotionInterruptStopStateInvalid)),
        "soft_on_path could not project observed feedback onto the accepted path",
        {{"reason", "path_deviation_projection_failed"}}});
  }
  return Result<double>::Ok(projection.distance_rad);
}

Result<TrajectoryDTO> shift_soft_stop_for_dispatch_latency(
    const TrajectoryDTO& planned,
    double compute_latency_s,
    double lead_in_budget_s,
    const TrajectoryDTO& accepted_trajectory,
    double command_cursor_s,
    double path_time_rate,
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
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "dispatch-latency shift requires a finite, non-negative compute latency",
        {{"reason", "invalid_compute_latency_s"},
         {"compute_latency_s", format_double(compute_latency_s)}}});
  }
  if (!finite_positive(lead_in_budget_s)) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "dispatch-latency shift requires a finite, positive lead-in budget",
        {{"reason", "invalid_lead_in_budget_s"},
         {"lead_in_budget_s", format_double(lead_in_budget_s)}}});
  }
  if (!std::isfinite(path_time_rate) || path_time_rate < 0.0) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "dispatch-latency shift requires a finite, non-negative path_time_rate",
        {{"reason", "invalid_path_time_rate"},
         {"path_time_rate", format_double(path_time_rate)}}});
  }
  if (!std::isfinite(config.anchor_negative_time_s) ||
      config.anchor_negative_time_s >= 0.0) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "dispatch-latency anchor time must be finite and negative",
        {{"reason", "invalid_anchor_negative_time_s"},
         {"anchor_negative_time_s",
          format_double(config.anchor_negative_time_s)}}});
  }

  // Reliability guard: if the compute+dispatch latency consumed the command-frame
  // lead-in budget there is no command-continuation left to seam onto, so escalate
  // to a hard stop rather than emit a degraded seam. This is the worst-case path
  // (a slow planner — e.g. the ALI/backtracking tail — or a too-small lead-in),
  // not the common few-ms case.
  if (compute_latency_s >= lead_in_budget_s) {
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
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
  // the live command frame (mirrors send_fjt_anchored_goal's past-anchor).
  const double advanced_cursor_s =
      command_cursor_s + path_time_rate * compute_latency_s;
  const PathSample pin_sample =
      sample_path(accepted_trajectory, advanced_cursor_s);
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
    return Result<TrajectoryDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kMotionInterruptBackendStopFailed)),
        "dispatch latency consumed every replacement waypoint; escalate to hard stop",
        {{"reason", "dispatch_latency_consumed_all_waypoints"},
         {"recommended_disposition", "hard_stop"},
         {"compute_latency_s", format_double(compute_latency_s)}}});
  }

  return Result<TrajectoryDTO>::Ok(std::move(shifted));
}

Result<SoftOnPathMinStopTimeDTO> compute_soft_on_path_min_stop_time(
    const TrajectoryDTO& accepted_trajectory,
    double trip_path_time_s,
    double trip_path_rate,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    double integration_step_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<SoftOnPathMinStopTimeDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<SoftOnPathMinStopTimeDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)), std::move(message),
        {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(trip_path_rate) || trip_path_rate < 0.0) {
    return invalid("invalid_trip_path_rate",
                   "trip_path_rate must be finite and non-negative");
  }
  if (!finite_positive(integration_step_s)) {
    return invalid("invalid_integration_step_s",
                   "integration_step_s must be finite and positive");
  }
  const size_t joint_count = accepted_trajectory.joint_names.size();
  std::vector<double> max_decel(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    const auto it = per_joint_max_deceleration_rad_s2.find(
        accepted_trajectory.joint_names[j]);
    if (it == per_joint_max_deceleration_rad_s2.end() ||
        !finite_positive(it->second)) {
      return invalid(
          "missing_per_joint_max_deceleration",
          "per-joint max deceleration missing or non-positive for " +
              accepted_trajectory.joint_names[j]);
    }
    max_decel[j] = it->second;
  }

  const double path_end_s =
      accepted_trajectory.waypoints.back().time_from_start_s;
  SoftOnPathMinStopTimeDTO out;
  out.stop_path_time_s = trip_path_time_s;
  if (trip_path_rate <= kEpsilon) {  // already at rest: zero-time stop in place
    out.reaches_rest_on_path = trip_path_time_s <= path_end_s + kEpsilon;
    return Result<SoftOnPathMinStopTimeDTO>::Ok(std::move(out));
  }

  double s = trip_path_time_s;    // path cursor (accepted-trajectory time)
  double u_dot = trip_path_rate;  // path-rate ds/dt (> 0)
  double t = 0.0;
  std::map<std::string, int> binding_counts;
  const double dt = integration_step_s;
  const size_t kMaxSteps = 2'000'000;  // defensive: rate must reach zero first
  bool reached_rest = false;
  for (size_t steps = 0; steps < kMaxSteps; ++steps) {
    const PathSample sample = sample_path(accepted_trajectory, s);
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
      binding_counts[accepted_trajectory.joint_names[binding_j]] += 1;
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
  int best = 0;
  for (const auto& [name, count] : binding_counts) {
    if (count > best) {
      best = count;
      out.binding_joint = name;
    }
  }
  return Result<SoftOnPathMinStopTimeDTO>::Ok(std::move(out));
}

Result<JerkLimitedStopProfileDTO> compute_jerk_limited_stop_profile(
    const TrajectoryDTO& accepted_trajectory,
    double start_path_time_s,
    double start_path_rate,
    double start_path_rate_accel,
    double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3,
    double sample_period_s) {
  const auto valid_shape = validate_trajectory_shape(accepted_trajectory);
  if (!valid_shape.ok()) {
    return Result<JerkLimitedStopProfileDTO>::Err(valid_shape.error());
  }
  auto invalid = [](std::string reason, std::string message) {
    return Result<JerkLimitedStopProfileDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)), std::move(message),
        {{"reason", std::move(reason)}}});
  };
  if (!std::isfinite(start_path_rate) || start_path_rate <= 0.0) {
    return invalid("invalid_start_path_rate",
                   "jerk-limited retimer requires a positive start_path_rate");
  }
  if (!std::isfinite(start_path_rate_accel)) {
    return invalid("invalid_start_path_rate_accel",
                   "start_path_rate_accel must be finite");
  }
  const double path_start_s =
      accepted_trajectory.waypoints.front().time_from_start_s;
  const double path_end_s =
      accepted_trajectory.waypoints.back().time_from_start_s;
  if (!std::isfinite(start_path_time_s) ||
      start_path_time_s < path_start_s - kEpsilon ||
      start_path_time_s > path_end_s + kEpsilon) {
    return invalid("invalid_start_path_time",
                   "start_path_time_s must be finite and inside the accepted "
                   "trajectory time range");
  }
  if (!finite_positive(target_stop_time_s) ||
      !finite_positive(sample_period_s)) {
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
    if (d_it == per_joint_max_deceleration_rad_s2.end() ||
        !finite_positive(d_it->second) ||
        j_it == per_joint_max_jerk_rad_s3.end() ||
        !finite_positive(j_it->second)) {
      return invalid(
          "missing_per_joint_limit",
          "per-joint deceleration and jerk limits are required for " + name);
    }
    max_decel[j] = d_it->second;
    max_jerk[j] = j_it->second;
  }
  constexpr double kBig = 1e18;
  const double target_time_tolerance_s =
      std::max(2.0 * sample_period_s, 1e-3);
  constexpr double kTerminalTolerance = 1e-3;
  constexpr double kAccelTolerance = 1e-6;
  constexpr double kJerkTolerance = 1e-6;
  constexpr double kEnvelopeSafetyMargin = 0.8;

  // Path-rate accel budget [lo, hi] and a conservative path-rate jerk bound at
  // (sample, v). The jerk bound limits |q'·d(ü)/dt| <= J_j (the controllable
  // term); residual analytic jerk terms are caught by the output check below.
  auto budgets = [&](const PathSample& smp, double v, double* lo, double* hi,
                     double* jpath) -> bool {
    *lo = 0.0;
    *hi = 0.0;
    *jpath = kBig;
    bool found = false;
    for (size_t j = 0; j < joint_count; ++j) {
      const double qp = smp.d_position_dt[j];
      const double curvature = smp.d2_position_dt2[j] * v * v;
      const double accel_budget = kEnvelopeSafetyMargin * max_decel[j];
      if (std::abs(qp) <= kEpsilon) {
        if (std::abs(curvature) > accel_budget + kAccelTolerance) {
          // No path-rate acceleration can help this joint: q' is zero, while
          // q'' * u_dot^2 already violates the joint acceleration budget.
          *lo = 1.0;
          *hi = 0.0;
          *jpath = 0.0;
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
      *jpath = std::min(*jpath, max_jerk[j] / std::abs(qp));
      found = true;
    }
    return found;
  };

  auto solve_velocity_zero = [](double v, double a, double j,
                                double fallback_dt) {
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

  auto path_third_derivative = [&](double s, size_t joint_index) {
    const double span = std::max(path_end_s - path_start_s, kEpsilon);
    const double h = std::min(std::max(sample_period_s, 1e-3), 0.01 * span);
    const double left = std::max(path_start_s, s - h);
    const double right = std::min(path_end_s, s + h);
    if (right <= left + kEpsilon) {
      return 0.0;
    }
    const PathSample l = sample_path(accepted_trajectory, left);
    const PathSample r = sample_path(accepted_trajectory, right);
    if (joint_index >= l.d2_position_dt2.size() ||
        joint_index >= r.d2_position_dt2.size()) {
      return 0.0;
    }
    return (r.d2_position_dt2[joint_index] -
            l.d2_position_dt2[joint_index]) /
           (right - left);
  };

  auto local_rate_cap = [&](double s) {
    const PathSample smp = sample_path(accepted_trajectory, s);
    double cap = kBig;
    for (size_t j = 0; j < joint_count; ++j) {
      const double curvature = std::abs(smp.d2_position_dt2[j]);
      if (curvature > kEpsilon) {
        cap = std::min(
            cap, std::sqrt((kEnvelopeSafetyMargin * max_decel[j]) / curvature));
      }
      const double q3 = std::abs(path_third_derivative(s, j));
      if (q3 > kEpsilon) {
        cap = std::min(
            cap, std::cbrt((kEnvelopeSafetyMargin * max_jerk[j]) / q3));
      }
    }
    return cap;
  };

  auto local_path_decel_cap = [&](double s) {
    const PathSample smp = sample_path(accepted_trajectory, s);
    double cap = kBig;
    for (size_t j = 0; j < joint_count; ++j) {
      const double qp = std::abs(smp.d_position_dt[j]);
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
      const double nominal_step =
          std::max(0.002, sample_period_s * std::max(start_path_rate, 0.1));
      const size_t steps = std::max<size_t>(
          1, std::min<size_t>(
                 4000, static_cast<size_t>(std::ceil(remaining / nominal_step))));
      backward_envelope.resize(steps + 1);
      for (size_t i = 0; i <= steps; ++i) {
        const double alpha = static_cast<double>(i) / static_cast<double>(steps);
        const double s = start_path_time_s + remaining * alpha;
        backward_envelope[i].path_time_s = s;
        backward_envelope[i].max_path_rate = local_rate_cap(s);
      }
      backward_envelope.back().max_path_rate = 0.0;
      for (size_t i = steps; i-- > 0;) {
        const double ds = std::max(
            0.0, backward_envelope[i + 1].path_time_s -
                     backward_envelope[i].path_time_s);
        const double decel_cap = local_path_decel_cap(
            backward_envelope[i].path_time_s);
        const double reachable =
            decel_cap >= kBig / 2.0
                ? backward_envelope[i + 1].max_path_rate
                : std::sqrt(std::max(
                      0.0, backward_envelope[i + 1].max_path_rate *
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
        [](const BackwardEnvelopePoint& point, double value) {
          return point.path_time_s < value;
        });
    if (it == backward_envelope.begin()) {
      return it->max_path_rate;
    }
    const auto& right = *it;
    const auto& left = *(it - 1);
    const double span = std::max(right.path_time_s - left.path_time_s, kEpsilon);
    const double alpha = std::clamp((s - left.path_time_s) / span, 0.0, 1.0);
    return left.max_path_rate +
           alpha * (right.max_path_rate - left.max_path_rate);
  };

  // Forward-integrate with target path-rate deceleration `a_decel`: slew ü
  // toward the budget-clamped target within the jerk bound, and ramp ü back to
  // zero on the S-curve approach to rest (start when ramping ü->0 lands exactly
  // at u̇=0). Records (t, s, u̇, ü) samples; returns (stop_time, reaches_rest).
  auto integrate = [&](double a_decel,
                       std::vector<JerkLimitedStopSampleDTO>* out,
                       bool* used_envelope)
      -> std::pair<double, bool> {
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
      const PathSample smp = sample_path(accepted_trajectory, s);
      double lo = 0.0;
      double hi = 0.0;
      double jpath = kBig;
      if (!budgets(smp, v, &lo, &hi, &jpath)) {
        if (out) {
          if (out->empty() ||
              std::abs(out->back().time_from_ramp_start_s - t) > kEpsilon) {
            out->push_back({t, s, 0.0, 0.0});
          } else {
            out->back() = {t, s, 0.0, 0.0};
          }
        }
        return {t, true};
      }
      const double hi_eff = std::min(hi, 0.0);
      if (lo > hi_eff + kAccelTolerance) {
        return {t, false};
      }
      // Use a fraction of the path-rate jerk bound so the finite-difference
      // jerk of the executed acceleration (which also picks up q''/q''' terms)
      // keeps margin under the per-joint limit.
      const double jpath_eff = 0.5 * jpath;
      if (!finite_positive(jpath_eff)) {
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
            std::abs(required_accel_now) * 0.05 + jpath_eff * dt;
        if (a <= required_accel_now + accel_margin &&
            jerk_to_zero_at_target >= -kEpsilon &&
            jerk_to_zero_at_target <= jpath_eff + kEpsilon) {
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
      if (a < -kEpsilon && v > kEpsilon) {
        const double terminal_velocity_at_max_jerk =
            (a * a) / (2.0 * jpath_eff);
        const double one_step_velocity_margin =
            std::abs(a) * dt + 0.5 * jpath_eff * dt * dt;
        if (!target_terminal_branch &&
            v <= terminal_velocity_at_max_jerk +
                     one_step_velocity_margin + kEpsilon) {
          // Terminal S-curve branch. Wait until the max-jerk terminal envelope
          // is close, then use the exact jerk when it is within the bound. This
          // avoids starting an extremely gentle endpoint ramp while the robot is
          // still far from rest.
          const double endpoint_jerk = (a * a) / (2.0 * v);
          jerk = std::min(endpoint_jerk, jpath_eff);
        }
      }
      const double envelope_rate =
          std::min(envelope_rate_at(s),
                   envelope_rate_at(std::min(path_end_s, s + v * dt_step)));
      double desired = -a_decel;
      const bool envelope_binding =
          std::isfinite(envelope_rate) &&
          (v > envelope_rate + std::max(1e-6, 0.01 * envelope_rate) ||
           v + desired * dt_step >
               envelope_rate + std::max(1e-6, 0.01 * envelope_rate));
      if (envelope_binding && used_envelope != nullptr) {
        *used_envelope = true;
        desired = std::min(desired, (envelope_rate - v) /
                                        std::max(dt_step, kEpsilon));
      }
      if (std::abs(jerk) <= kEpsilon) {
        const double slew = jpath_eff * dt_step;
        const double target = std::clamp(desired, std::min(lo, hi_eff), hi_eff);
        jerk = std::clamp(target - a, -slew, slew) /
               std::max(dt_step, kEpsilon);
      }

      const double a_next = a + jerk * dt_step;
      const double v_next =
          v + a * dt_step + 0.5 * jerk * dt_step * dt_step;
      if (target_terminal_branch && dt_step < dt + kEpsilon &&
          std::abs(remaining_to_target - dt_step) <= kEpsilon &&
          std::abs(v_next) <= kTerminalTolerance &&
          std::abs(a_next) <= kTerminalTolerance) {
        s += v * dt_step + 0.5 * a * dt_step * dt_step +
             (jerk * dt_step * dt_step * dt_step) / 6.0;
        t += dt_step;
        if (out) {
          if (out->empty() ||
              t > out->back().time_from_ramp_start_s + kEpsilon) {
            out->push_back({t, s, 0.0, 0.0});
          } else {
            out->back() = {t, s, 0.0, 0.0};
          }
        }
        return {t, s <= path_end_s + kEpsilon};
      }
      if (v_next <= 0.0) {
        const double dt_zero = std::clamp(
            solve_velocity_zero(v, a, jerk, dt_step), 0.0, dt_step);
        s += v * dt_zero + 0.5 * a * dt_zero * dt_zero +
             (jerk * dt_zero * dt_zero * dt_zero) / 6.0;
        t += dt_zero;
        const double terminal_accel = a + jerk * dt_zero;
        const bool stop_time_matches_target =
            std::abs(t - target_stop_time_s) <= target_time_tolerance_s;
        const double emitted_accel =
            (stop_time_matches_target ||
             std::abs(terminal_accel) <= kTerminalTolerance)
                ? 0.0
                : terminal_accel;
        if (out) {
          if (out->empty() ||
              t > out->back().time_from_ramp_start_s + kEpsilon) {
            out->push_back({t, s, 0.0, emitted_accel});
          } else {
            out->back() = {t, s, 0.0, emitted_accel};
          }
        }
        return {t, s <= path_end_s + kEpsilon};
      }

      s += v * dt_step + 0.5 * a * dt_step * dt_step +
           (jerk * dt_step * dt_step * dt_step) / 6.0;
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

  auto build_joint_trajectory =
      [&](const std::vector<JerkLimitedStopSampleDTO>& samples) {
        TrajectoryDTO trajectory;
        trajectory.joint_names = accepted_trajectory.joint_names;
        for (const auto& smp : samples) {
          const PathSample ps = sample_path(accepted_trajectory, smp.path_time_s);
          TrajectoryWaypointDTO wp;
          wp.time_from_start_s = smp.time_from_ramp_start_s;
          wp.positions_rad = ps.position;
          wp.velocities_rad_s.resize(joint_count, 0.0);
          wp.accelerations_rad_s2.resize(joint_count, 0.0);
          for (size_t j = 0; j < joint_count; ++j) {
            wp.velocities_rad_s[j] = ps.d_position_dt[j] * smp.path_rate;
            wp.accelerations_rad_s2[j] =
                ps.d2_position_dt2[j] * smp.path_rate * smp.path_rate +
                ps.d_position_dt[j] * smp.path_rate_accel;
          }
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
        std::abs(profile.achieved_stop_time_s - target_stop_time_s) <=
            target_time_tolerance_s;
    profile.terminal_state_zero = false;
    profile.within_budget =
        profile.reaches_rest_on_path && profile.samples.size() >= 2;
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
      if (!std::isfinite(curr.time_from_ramp_start_s) ||
          !std::isfinite(curr.path_time_s) ||
          !std::isfinite(curr.path_rate) ||
          !std::isfinite(curr.path_rate_accel) ||
          curr.time_from_ramp_start_s <=
              prev.time_from_ramp_start_s + kEpsilon) {
        mark_invalid("sample_time_or_value_invalid");
      }
    }

    const auto& terminal = profile.samples.back();
    profile.terminal_state_zero =
        std::abs(terminal.path_rate) <= kTerminalTolerance &&
        std::abs(terminal.path_rate_accel) <= kTerminalTolerance;
    if (!profile.terminal_state_zero) {
      mark_invalid("terminal_state_not_zero");
    }

    const TrajectoryDTO emitted = build_joint_trajectory(profile.samples);
    const auto shape = validate_trajectory_shape(emitted);
    if (!shape.ok()) {
      mark_invalid("emitted_trajectory_shape_invalid");
      profile.dispatchable = false;
      return;
    }

    const double validate_dt =
        std::max(sample_period_s / 4.0, target_stop_time_s / 2000.0);
    const size_t steps = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(profile.achieved_stop_time_s /
                                         std::max(validate_dt, kEpsilon))));
    std::vector<double> prev_accel;
    double prev_t = 0.0;
    for (size_t i = 0; i <= steps; ++i) {
      const double t_sample =
          profile.achieved_stop_time_s * static_cast<double>(i) /
          static_cast<double>(steps);
      auto sampled = sample_trajectory(emitted, t_sample);
      if (!sampled.ok() ||
          sampled.value().accelerations_rad_s2.size() != joint_count) {
        mark_invalid("emitted_trajectory_sample_invalid");
        continue;
      }
      const auto accel = sampled.value().accelerations_rad_s2;
      for (size_t j = 0; j < joint_count; ++j) {
        if (!std::isfinite(accel[j]) ||
            std::abs(accel[j]) > max_decel[j] + kAccelTolerance) {
          mark_invalid("interpolated_acceleration_limit_exceeded");
        }
      }
      if (!prev_accel.empty()) {
        const double dt_check = t_sample - prev_t;
        if (dt_check > kEpsilon) {
          for (size_t j = 0; j < joint_count; ++j) {
            const double jerk = (accel[j] - prev_accel[j]) / dt_check;
            if (!std::isfinite(jerk) ||
                std::abs(jerk) > max_jerk[j] + kJerkTolerance) {
              mark_invalid("interpolated_jerk_limit_exceeded");
            }
          }
        }
      }
      prev_accel = accel;
      prev_t = t_sample;
    }

    profile.dispatchable = profile.reaches_rest_on_path &&
                           profile.hits_target_time &&
                           profile.terminal_state_zero &&
                           profile.within_budget;
  };

  auto straight_scurve_candidate = [&]() -> std::optional<JerkLimitedStopProfileDTO> {
    if (std::abs(start_path_rate_accel) > kTerminalTolerance) {
      return std::nullopt;
    }
    const PathSample start_sample =
        sample_path(accepted_trajectory, start_path_time_s);
    const double peak_path_accel =
        2.0 * start_path_rate / target_stop_time_s;
    const double path_jerk =
        4.0 * start_path_rate /
        (target_stop_time_s * target_stop_time_s);
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
      path_accel_limit =
          std::min(path_accel_limit, max_decel[j] / std::abs(qp));
      path_jerk_limit =
          std::min(path_jerk_limit, max_jerk[j] / std::abs(qp));
      found_tangent = true;
    }
    if (!found_tangent ||
        peak_path_accel > path_accel_limit + kAccelTolerance ||
        path_jerk > path_jerk_limit + kJerkTolerance) {
      return std::nullopt;
    }

    const double stop_distance = 0.5 * start_path_rate * target_stop_time_s;
    const double stop_path_time = start_path_time_s + stop_distance;
    if (stop_path_time > path_end_s + kEpsilon) {
      return std::nullopt;
    }
    const PathSample mid_sample =
        sample_path(accepted_trajectory,
                    start_path_time_s + 0.5 * stop_distance);
    const PathSample stop_sample =
        sample_path(accepted_trajectory, stop_path_time);
    for (size_t j = 0; j < joint_count; ++j) {
      if (std::abs(mid_sample.d2_position_dt2[j]) > kTerminalTolerance ||
          std::abs(stop_sample.d2_position_dt2[j]) > kTerminalTolerance ||
          std::abs(mid_sample.d_position_dt[j] -
                   start_sample.d_position_dt[j]) > 1e-6 ||
          std::abs(stop_sample.d_position_dt[j] -
                   start_sample.d_position_dt[j]) > 1e-6) {
        return std::nullopt;
      }
    }

    JerkLimitedStopProfileDTO candidate;
    candidate.target_decel_path_rate = peak_path_accel;
    candidate.achieved_stop_time_s = target_stop_time_s;
    candidate.reaches_rest_on_path = true;
    const size_t steps = std::max<size_t>(
        2, static_cast<size_t>(std::ceil(target_stop_time_s /
                                         sample_period_s)));
    const double half_t = 0.5 * target_stop_time_s;
    const double v_mid = 0.5 * start_path_rate;
    const double s_mid =
        start_path_rate * half_t - (path_jerk * half_t * half_t * half_t) / 6.0;
    for (size_t i = 0; i <= steps; ++i) {
      const double t =
          target_stop_time_s * static_cast<double>(i) /
          static_cast<double>(steps);
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
        s_rel = s_mid + v_mid * tau -
                0.5 * peak_path_accel * tau * tau +
                (path_jerk * tau * tau * tau) / 6.0;
      }
      if (i == steps) {
        v = 0.0;
        a = 0.0;
        s_rel = stop_distance;
      }
      candidate.samples.push_back(
          {t, start_path_time_s + s_rel, std::max(0.0, v), a});
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
  double a_max = 1.0;
  for (int grow = 0; grow < 64 && stop_time(a_max) > target_stop_time_s;
       ++grow) {
    a_max *= 2.0;
  }
  double a_min = a_max;
  for (int shrink = 0; shrink < 80 && stop_time(a_min) < target_stop_time_s;
       ++shrink) {
    a_min *= 0.5;
  }
  for (int i = 0; i < 64; ++i) {
    const double a_mid = 0.5 * (a_min + a_max);
    if (stop_time(a_mid) >= target_stop_time_s) {
      a_min = a_mid;
    } else {
      a_max = a_mid;
    }
  }

  const double t_min_candidate = stop_time(a_min);
  const double t_max_candidate = stop_time(a_max);
  const double selected_decel =
      std::abs(t_min_candidate - target_stop_time_s) <
              std::abs(t_max_candidate - target_stop_time_s)
          ? a_min
          : a_max;

  JerkLimitedStopProfileDTO out;
  out.target_decel_path_rate = selected_decel;
  bool used_backward_envelope = false;
  const auto [achieved, reached] =
      integrate(selected_decel, &out.samples, &used_backward_envelope);
  out.achieved_stop_time_s = achieved;
  out.reaches_rest_on_path = reached;
  out.used_backward_envelope = used_backward_envelope;

  validate_dispatch_trajectory(out);
  return Result<JerkLimitedStopProfileDTO>::Ok(std::move(out));
}

}  // namespace manyforge::core
