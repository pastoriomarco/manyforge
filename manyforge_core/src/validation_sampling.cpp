#include <manyforge_core/trajectory/validation_sampling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <manyforge_core/trajectory/trajectory_sampler.hpp>

namespace manyforge::core {

namespace {

Result<CollisionValidationSamples> refuse(std::string reason, std::string message,
                                          std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  return Result<CollisionValidationSamples>::Err(
      ErrorDTO{kErrorValidationCollision, std::move(message), std::move(details)});
}

template <std::size_t N>
double max_step(const std::array<double, N>& control) {
  double step = 0.0;
  for (std::size_t k = 0; k + 1 < N; ++k) {
    step = std::max(step, std::abs(control[k + 1] - control[k]));
  }
  return step;
}

// Bound on |dp/ds| over s in [0, 1] for one segment: every joint's bound,
// combined as a Euclidean norm (RoboPlan's configuration distance). The
// degree selection mirrors trajectory_sampler's endpoint classification. A
// Bezier curve's derivative is bounded by degree x the largest difference of
// adjacent control points (hodograph convex hull).
double segment_speed_bound(const TrajectoryWaypointDTO& a, const TrajectoryWaypointDTO& b,
                           std::size_t joints) {
  const double T = b.time_from_start_s - a.time_from_start_s;
  const bool velocity = a.velocities_rad_s.size() == joints && b.velocities_rad_s.size() == joints;
  const bool acceleration = velocity && a.accelerations_rad_s2.size() == joints &&
                            b.accelerations_rad_s2.size() == joints;
  double bound = 0.0;
  for (std::size_t j = 0; j < joints; ++j) {
    const double pa = a.positions_rad[j];
    const double pb = b.positions_rad[j];
    double joint = 0.0;
    if (acceleration) {
      const double da = T * a.velocities_rad_s[j];
      const double db = T * b.velocities_rad_s[j];
      const double aa = T * T * a.accelerations_rad_s2[j];
      const double ab = T * T * b.accelerations_rad_s2[j];
      joint = 5.0 * max_step(std::array<double, 6>{
                        pa, pa + da / 5.0, pa + 2.0 * da / 5.0 + aa / 20.0,
                        pb - 2.0 * db / 5.0 + ab / 20.0, pb - db / 5.0, pb});
    } else if (velocity) {
      joint = 3.0 * max_step(std::array<double, 4>{
                        pa, pa + T * a.velocities_rad_s[j] / 3.0,
                        pb - T * b.velocities_rad_s[j] / 3.0, pb});
    } else {
      joint = std::abs(pb - pa);
    }
    bound += joint * joint;
  }
  return std::sqrt(bound);
}

}  // namespace

Result<CollisionValidationSamples> sample_for_collision_validation(
    const TrajectoryDTO& trajectory, const double max_step_rad, const std::size_t max_states,
    const ValidationSamplingStopCheck& stop) {
  if (auto valid = validate_trajectory_for_sampling(trajectory); !valid.ok()) {
    return refuse("collision_validation_trajectory_unsampleable",
                  "Trajectory cannot be sampled for collision validation",
                  {{"sampler_error", valid.error().code}});
  }
  const auto& waypoints = trajectory.waypoints;
  CollisionValidationSamples out;
  out.trajectory.resources = trajectory.resources;
  out.trajectory.joint_names = trajectory.joint_names;
  if (waypoints.size() > max_states) {
    return refuse("collision_validation_sample_budget_exceeded",
                  "Trajectory exceeds the collision validation sample budget",
                  {{"max_states", std::to_string(max_states)},
                   {"stored_waypoints", std::to_string(waypoints.size())}});
  }
  const auto add = [&out](double time_s, std::vector<double> positions, double fraction) {
    TrajectoryWaypointDTO sample;
    sample.time_from_start_s = time_s;
    sample.positions_rad = std::move(positions);
    out.trajectory.waypoints.push_back(std::move(sample));
    out.original_fractions.push_back(fraction);
  };
  if (waypoints.empty()) return Result<CollisionValidationSamples>::Ok(std::move(out));
  const double last = static_cast<double>(std::max<std::size_t>(waypoints.size() - 1, 1));
  out.trajectory.waypoints.reserve(waypoints.size());
  out.original_fractions.reserve(waypoints.size());
  add(waypoints.front().time_from_start_s, waypoints.front().positions_rad,
      waypoints.size() == 1 ? 1.0 : 0.0);
  TrajectorySampleResult scratch;
  std::size_t since_poll = 0;
  const auto stopped = [&](std::size_t segment) -> std::optional<Result<CollisionValidationSamples>> {
    since_poll = 0;
    const char* reason = stop ? stop() : nullptr;
    if (reason == nullptr) return std::nullopt;
    return refuse(reason, "Collision validation stopped while sampling the trajectory",
                  {{"stopped_at_segment", std::to_string(segment)},
                   {"inserted_samples", std::to_string(out.inserted)}});
  };
  for (std::size_t index = 0; index + 1 < waypoints.size(); ++index) {
    if (auto refused = stopped(index)) return std::move(*refused);
    const auto& from = waypoints[index];
    const auto& to = waypoints[index + 1];
    const double wanted =
        segment_speed_bound(from, to, trajectory.joint_names.size()) / max_step_rad;
    const std::size_t remaining = max_states - out.trajectory.waypoints.size();
    if (!std::isfinite(wanted) || wanted > static_cast<double>(remaining)) {
      return refuse("collision_validation_sample_budget_exceeded",
                    "Trajectory exceeds the collision validation sample budget",
                    {{"max_states", std::to_string(max_states)},
                     {"stored_waypoints", std::to_string(waypoints.size())},
                     {"segment", std::to_string(index)}});
    }
    const auto steps = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(wanted)));
    if (steps > remaining) {
      return refuse("collision_validation_sample_budget_exceeded",
                    "Trajectory exceeds the collision validation sample budget",
                    {{"max_states", std::to_string(max_states)},
                     {"stored_waypoints", std::to_string(waypoints.size())},
                     {"segment", std::to_string(index)}});
    }
    const double duration = to.time_from_start_s - from.time_from_start_s;
    for (std::size_t k = 1; k < steps; ++k) {
      if (++since_poll >= kValidationSamplingPollInterval) {
        if (auto refused = stopped(index)) return std::move(*refused);
      }
      const double s = static_cast<double>(k) / static_cast<double>(steps);
      const double time_s = from.time_from_start_s + s * duration;
      if (auto sampled = sample_trajectory_prevalidated(trajectory, time_s, scratch);
          !sampled.ok()) {
        return refuse("collision_validation_trajectory_unsampleable",
                      "Trajectory cannot be sampled for collision validation",
                      {{"sampler_error", sampled.error().code}});
      }
      add(time_s, scratch.positions_rad, (static_cast<double>(index) + s) / last);
      ++out.inserted;
    }
    add(to.time_from_start_s, to.positions_rad, static_cast<double>(index + 1) / last);
  }
  return Result<CollisionValidationSamples>::Ok(std::move(out));
}

}  // namespace manyforge::core
