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

TrajectoryDTO make_two_point_trajectory(
    const std::vector<std::string>& joint_names,
    const std::vector<double>& q0,
    const std::vector<double>& v0,
    const std::vector<double>& a0,
    const std::vector<double>& q1,
    const std::vector<double>& v1,
    const std::vector<double>& a1,
    double duration_s) {
  TrajectoryDTO trajectory;
  trajectory.joint_names = joint_names;
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{0.0, q0, v0, a0});
  trajectory.waypoints.push_back(
      TrajectoryWaypointDTO{duration_s, q1, v1, a1});
  return trajectory;
}

double ramp_position_factor(double r) {
  return r - r * r * r + 0.5 * r * r * r * r;
}

double ramp_velocity_factor(double r) {
  return 1.0 - 3.0 * r * r + 2.0 * r * r * r;
}

double ramp_acceleration_factor(double r) {
  return -6.0 * r + 6.0 * r * r;
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

  const double handoff_path_time =
      projection.time_from_start_s + path_time_rate * config.stop_traj_delta_s;
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
  plan.diagnostics["initial_path_deviation_rad"] =
      format_double(plan.initial_path_deviation_rad);
  plan.diagnostics["initial_velocity_deviation_rad_s"] =
      format_double(plan.initial_velocity_deviation_rad_s);
  plan.diagnostics["jerk_limit_source"] = plan.jerk_limit_source;

  plan.replacement_trajectory.joint_names = accepted_trajectory.joint_names;
  std::vector<double> anchor_velocity =
      (!use_model_velocity && ordered_state.has_velocity)
          ? ordered_state.velocities
          : projected_velocity;
  std::vector<double> anchor_acceleration(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    anchor_acceleration[j] =
        projected_sample.d2_position_dt2[j] * path_time_rate * path_time_rate;
  }

  TrajectoryWaypointDTO anchor;
  anchor.time_from_start_s = 0.0;
  anchor.positions_rad = ordered_state.positions;
  anchor.velocities_rad_s = anchor_velocity;
  anchor.accelerations_rad_s2 = anchor_acceleration;
  plan.replacement_trajectory.waypoints.push_back(std::move(anchor));

  const PathSample handoff_sample =
      sample_path(accepted_trajectory, handoff_path_time);
  std::vector<double> handoff_velocity(joint_count, 0.0);
  std::vector<double> handoff_acceleration(joint_count, 0.0);
  for (size_t j = 0; j < joint_count; ++j) {
    handoff_velocity[j] = handoff_sample.d_position_dt[j] * path_time_rate;
    handoff_acceleration[j] =
        handoff_sample.d2_position_dt2[j] * path_time_rate * path_time_rate;
  }
  if (config.stop_traj_delta_s > kEpsilon) {
    const auto handoff_trajectory = make_two_point_trajectory(
        accepted_trajectory.joint_names,
        ordered_state.positions,
        anchor_velocity,
        anchor_acceleration,
        handoff_sample.position,
        handoff_velocity,
        handoff_acceleration,
        config.stop_traj_delta_s);
    const size_t handoff_samples = std::max<size_t>(
        1, static_cast<size_t>(
               std::ceil(config.stop_traj_delta_s / config.sample_period_s)));
    std::vector<double> previous_handoff_accel;
    double previous_handoff_time = 0.0;
    for (size_t i = 0; i <= handoff_samples; ++i) {
      const double t = config.stop_traj_delta_s *
                       static_cast<double>(i) /
                       static_cast<double>(handoff_samples);
      const auto handoff_state = sample_path(handoff_trajectory, t);
      const auto handoff_projection =
          project_to_path(accepted_trajectory, handoff_state.position);
      plan.path_deviation_max_rad = std::max(
          plan.path_deviation_max_rad, handoff_projection.distance_rad);
      for (size_t j = 0; j < joint_count; ++j) {
        const auto& joint_name = accepted_trajectory.joint_names[j];
        const double accel_limit =
            stop_profile.per_joint_max_deceleration_rad_s2.at(joint_name);
        if (std::abs(handoff_state.d2_position_dt2[j]) >
            accel_limit + 1e-8) {
          return err("soft_on_path_handoff_acceleration_limit_exceeded",
                     "soft_on_path handoff segment exceeds a per-joint acceleration limit",
                     {{"joint", joint_name},
                      {"acceleration_rad_s2",
                       format_double(handoff_state.d2_position_dt2[j])},
                      {"limit_rad_s2", format_double(accel_limit)}});
        }
        if (!previous_handoff_accel.empty()) {
          const double dt = t - previous_handoff_time;
          if (dt > kEpsilon) {
            const double jerk =
                (handoff_state.d2_position_dt2[j] -
                 previous_handoff_accel[j]) /
                dt;
            const double jerk_limit = jerk_limits.at(joint_name);
            if (std::abs(jerk) > jerk_limit + 1e-8) {
              return err("soft_on_path_handoff_jerk_limit_exceeded",
                         "soft_on_path handoff segment exceeds a per-joint jerk limit",
                         {{"joint", joint_name},
                          {"jerk_rad_s3", format_double(jerk)},
                          {"limit_rad_s3", format_double(jerk_limit)}});
            }
          }
        }
      }
      previous_handoff_accel = handoff_state.d2_position_dt2;
      previous_handoff_time = t;
    }
  }

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
        return err("soft_on_path_acceleration_limit_exceeded",
                   "soft_on_path ramp exceeds a per-joint acceleration limit",
                   {{"joint", joint_name},
                    {"acceleration_rad_s2", format_double(wp.accelerations_rad_s2[j])},
                    {"limit_rad_s2", format_double(accel_limit)}});
      }
      if (!previous_accel.empty()) {
        const double dt = wp.time_from_start_s - previous_time;
        if (dt > kEpsilon) {
          const double jerk =
              (wp.accelerations_rad_s2[j] - previous_accel[j]) / dt;
          const double jerk_limit = jerk_limits.at(joint_name);
          if (std::abs(jerk) > jerk_limit + 1e-8) {
            return err("soft_on_path_jerk_limit_exceeded",
                       "soft_on_path ramp exceeds a per-joint jerk limit",
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

}  // namespace manyforge::core
