#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <manyforge_core/execution/motion_step.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <optional>
#include <span>

// Real-time path: every function here is noexcept and performs no allocation,
// blocking, logging, clock read or exception. Prepared data is read in place;
// all scratch is the caller's fixed-size output.

namespace manyforge::core {
namespace {

using detail::RuntimeScaleBounds;
using detail::RuntimeScaleGovernor;

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

bool positive_finite(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

bool valid_bounds(const RuntimeScaleBounds& bounds) noexcept {
  return positive_finite(bounds.max_rate_per_s) && positive_finite(bounds.max_rate_change_per_s2);
}

std::uint8_t interpolation_degree(MotionInterpolation interpolation) noexcept {
  switch (interpolation) {
    case MotionInterpolation::kLinear:
    case MotionInterpolation::kCubicHermite:
    case MotionInterpolation::kQuintic:
      return static_cast<std::uint8_t>(interpolation);
  }
  return 0U;
}

// Constant-time checks of the scalar prepared data the step relies on. The
// per-segment derivative data is checked when the segment is evaluated.
bool valid_prepared(const PreparedMotion& prepared) noexcept {
  const auto& waypoints = prepared.trajectory.waypoints;
  const auto& envelope = prepared.tick_envelope;
  const double stop_path_rate = prepared.runtime_scale_bounds.max_stop_path_rate;
  return interpolation_degree(prepared.interpolation) != 0U && prepared.joint_count > 0U &&
         prepared.joint_count <= kMaxMotionJoints &&
         prepared.trajectory.joint_names.size() == prepared.joint_count &&
         waypoints.size() >= 2U && std::isfinite(waypoints.front().time_from_start_s) &&
         std::isfinite(waypoints.back().time_from_start_s) &&
         waypoints.back().time_from_start_s > waypoints.front().time_from_start_s &&
         valid_bounds(prepared.governor_bounds) && valid_bounds(prepared.stop_braking_bounds) &&
         std::isfinite(stop_path_rate) && stop_path_rate >= 1.0 &&
         envelope.nominal_period_ns > 0 && envelope.max_delay_ns >= 0 &&
         envelope.max_delay_ns <= kInt64Max - envelope.nominal_period_ns;
}

bool valid_state(const PreparedMotion& prepared, const MotionStepState& state) noexcept {
  if (!state.governor.has_value() || !state.goal.nonzero() ||
      !(state.goal == prepared.key.fence()) || state.mode == MotionStepStatus::kCannotProceed) {
    return false;
  }
  const auto& governor = *state.governor;
  const auto& waypoints = prepared.trajectory.waypoints;
  const std::size_t hint = state.segment_hint;
  if (!std::isfinite(governor.scale()) || !std::isfinite(governor.rate_per_s()) ||
      !std::isfinite(state.path_time_s) || hint + 1U >= waypoints.size()) {
    return false;
  }
  const double end = waypoints.back().time_from_start_s;
  const double segment_end = waypoints[hint + 1U].time_from_start_s;
  return state.path_time_s >= waypoints[hint].time_from_start_s && state.path_time_s <= end &&
         (state.path_time_s < segment_end || hint + 2U == waypoints.size());
}

MotionInputValidation validate_stop(const PreparedMotion& prepared, const MotionStepState& state,
                                    const StopTransition* stop) noexcept {
  if (stop == nullptr) return MotionInputValidation::kAbsent;
  const StopGeneration consumed =
      state.active_stop ? state.active_stop->stop_generation : StopGeneration{0U};
  if (!(stop->goal == state.goal) || !(stop->stop_generation > consumed)) {
    return MotionInputValidation::kStale;
  }
  const auto& bounds = stop->braking_bounds;
  const auto& ceiling = prepared.stop_braking_bounds;
  if (!valid_bounds(bounds) || bounds.max_rate_per_s > ceiling.max_rate_per_s ||
      bounds.max_rate_change_per_s2 > ceiling.max_rate_change_per_s2) {
    return MotionInputValidation::kInvalid;
  }
  return MotionInputValidation::kAccepted;
}

MotionInputValidation validate_scale(const MotionStepState& state, const ScaleTarget* scale,
                                     bool stop_governs) noexcept {
  if (scale == nullptr) return MotionInputValidation::kAbsent;
  if (!(scale->goal == state.goal) || !(scale->generation > state.scale_generation)) {
    return MotionInputValidation::kStale;
  }
  if (!std::isfinite(scale->target_scale) || scale->target_scale < 0.0 ||
      scale->target_scale > 1.0) {
    return MotionInputValidation::kInvalid;
  }
  return stop_governs ? MotionInputValidation::kIgnoredForStop : MotionInputValidation::kAccepted;
}

// Device facts are validated and reported only. Their composition with the
// time law is not qualified yet, so the step never multiplies the law by the
// factor (no double slowdown, no fabricated slow clock) and never reports it
// as applied.
MotionInputValidation validate_device(const DeviceScalingFacts& device, std::int64_t now_ns,
                                      bool stop_governs) noexcept {
  switch (device.source) {
    case DeviceScalingSource::kNone:
      return device.factor == 1.0 ? MotionInputValidation::kAbsent
                                  : MotionInputValidation::kInvalid;
    case DeviceScalingSource::kDeviceReported:
      if (!device.valid || !std::isfinite(device.factor) || device.factor < 0.0 ||
          device.factor > 1.0 || device.received_local_ns > now_ns) {
        return MotionInputValidation::kInvalid;
      }
      return stop_governs ? MotionInputValidation::kIgnoredForStop
                          : MotionInputValidation::kAccepted;
  }
  return MotionInputValidation::kInvalid;
}

MotionStepStatus cannot_proceed(MotionStepOutput& output) noexcept {
  output.command = MotionJointCommand{};
  output.status = MotionStepStatus::kCannotProceed;
  return MotionStepStatus::kCannotProceed;
}

}  // namespace

bool InstallMotionStep(const PreparedMotion& prepared, std::int64_t now_ns, double initial_scale,
                       RuntimeScaleGeneration scale_generation,
                       MotionStepState& state) noexcept {
  if (!valid_prepared(prepared) || !prepared.key.fence().nonzero()) return false;
  const auto governor = RuntimeScaleGovernor::create(initial_scale, prepared.governor_bounds);
  if (!governor) return false;
  MotionStepState installed{};
  installed.governor = governor;
  installed.path_time_s = prepared.trajectory.waypoints.front().time_from_start_s;
  installed.segment_hint = 0U;
  installed.last_tick_ns = now_ns;
  installed.goal = prepared.key.fence();
  installed.scale_generation = scale_generation;
  installed.mode = initial_scale == 0.0 ? MotionStepStatus::kHolding : MotionStepStatus::kRunning;
  state = installed;
  return true;
}

MotionStepStatus StepMotion(const PreparedMotion& prepared, MotionStepState& state,
                            const MotionStepInputs& inputs, MotionStepOutput& output) noexcept {
  // Report the unchanged state unless the tick commits.
  output = MotionStepOutput{};
  output.goal = state.goal;
  output.path_time_s = state.path_time_s;
  output.consumed_scale_generation = state.scale_generation;
  output.consumed_stop_generation =
      state.active_stop ? state.active_stop->stop_generation : StopGeneration{0U};
  if (state.governor) {
    output.applied_scale = state.governor->scale();
    output.scale_rate_per_s = state.governor->rate_per_s();
  }

  // Rule 1: each input is validated on its own, Stop first; a rejected scale
  // or device fact never blocks a valid Stop.
  output.stop_validation = validate_stop(prepared, state, inputs.stop);
  const bool stop_accepted = output.stop_validation == MotionInputValidation::kAccepted;
  const bool stop_governs = stop_accepted || state.active_stop.has_value();
  output.scale_validation = validate_scale(state, inputs.scale, stop_governs);
  output.device_validation = validate_device(inputs.device, inputs.now_ns, stop_governs);

  // Rule 2: reversed clock, non-finite/inconsistent state or invalid prepared
  // data leave `state` unchanged. A goal-fenced newer Stop that is invalid is
  // never ignored: the adapter's authorized containment primitive applies.
  if (!valid_prepared(prepared) || !valid_state(prepared, state) ||
      inputs.now_ns < state.last_tick_ns ||
      output.stop_validation == MotionInputValidation::kInvalid) {
    return cannot_proceed(output);
  }

  // Rule 3: unscaled loop time. Unsigned subtraction cannot overflow.
  const auto& envelope = prepared.tick_envelope;
  const std::uint64_t elapsed_ns =
      static_cast<std::uint64_t>(inputs.now_ns) - static_cast<std::uint64_t>(state.last_tick_ns);
  const auto bound_ns =
      static_cast<std::uint64_t>(envelope.nominal_period_ns + envelope.max_delay_ns);
  output.observed_tick_delay_ns =
      elapsed_ns > static_cast<std::uint64_t>(kInt64Max)
          ? kInt64Max
          : static_cast<std::int64_t>(elapsed_ns) - envelope.nominal_period_ns;
  output.tick_delay_within_envelope = elapsed_ns <= bound_ns;
  // PROVISIONAL late-tick response (maintainer decision 2026-10-08: the
  // response is qualified in the experiment). Outside the envelope the step
  // advances by at most the envelope bound and drops the remainder: no
  // catch-up jump now or later, because `last_tick_ns` takes the actual time.
  // Stop deadlines are absolute local monotonic times enforced by the Stop
  // realizer, so this cap never stretches them.
  const std::uint64_t advance_ns = output.tick_delay_within_envelope ? elapsed_ns : bound_ns;
  const double dt_s = static_cast<double>(advance_ns) / 1e9;

  // Rule 4: the elapsed interval runs under the previous target; inputs
  // consumed now govern only the next interval (never retroactive).
  RuntimeScaleGovernor governor = *state.governor;
  const auto advance = governor.advance(dt_s);
  if (!advance) return cannot_proceed(output);
  const auto& waypoints = prepared.trajectory.waypoints;
  const double end_s = waypoints.back().time_from_start_s;
  double path_time_s = state.path_time_s + *advance;
  if (!std::isfinite(path_time_s)) return cannot_proceed(output);
  // Rule 5 follows from the exact integral: zero scale adds zero path time.
  if (path_time_s > end_s) path_time_s = end_s;

  std::optional<StopTransition> active_stop = state.active_stop;
  RuntimeScaleGeneration scale_generation = state.scale_generation;
  if (stop_accepted) {
    // Rule 6: brake on-path from this tick's actual scale and rate, under the
    // stage's bounds. A newer stage continues from the current braking state.
    // Only a certified same-path Stop interpolant may exceed scale one, up to
    // the sealed `max_stop_path_rate`.
    const auto braking = RuntimeScaleGovernor::create_continuation(
        governor.scale(), inputs.stop->braking_bounds, governor.rate_per_s(), 0.0,
        prepared.runtime_scale_bounds.max_stop_path_rate);
    if (!braking) {
      output.stop_validation = MotionInputValidation::kInvalid;
      return cannot_proceed(output);
    }
    governor = *braking;
    active_stop = *inputs.stop;
  } else if (output.scale_validation == MotionInputValidation::kAccepted) {
    if (governor.retarget(inputs.scale->target_scale)) {
      scale_generation = inputs.scale->generation;
    } else {
      output.scale_validation = MotionInputValidation::kInvalid;
    }
  }

  // Advance the segment hint monotonically (path time never decreases). Work
  // per tick is bounded independently of knot density: a few linear steps
  // cover the ordinary case, then a binary search over the remaining knots
  // (logarithmic in the codec-bounded waypoint count).
  std::uint32_t hint = state.segment_hint;
  const std::size_t last_segment = waypoints.size() - 2U;
  constexpr std::uint32_t kLinearHintSteps = 4U;
  for (std::uint32_t step = 0U; step < kLinearHintSteps && hint < last_segment &&
                                path_time_s >= waypoints[hint + 1U].time_from_start_s;
       ++step) {
    ++hint;
  }
  if (hint < last_segment && path_time_s >= waypoints[hint + 1U].time_from_start_s) {
    const auto first = waypoints.begin() + static_cast<std::ptrdiff_t>(hint) + 1;
    const auto last = waypoints.begin() + static_cast<std::ptrdiff_t>(last_segment) + 1;
    const auto after = std::upper_bound(
        first, last, path_time_s,
        [](const double t, const TrajectoryWaypointDTO& waypoint) noexcept {
          return t < waypoint.time_from_start_s;
        });
    hint = static_cast<std::uint32_t>(after - waypoints.begin() - 1);
  }

  // Sample q, q', q'', q''' with the exact prepared interpolation into the
  // output arrays, then compose the time law in place.
  const std::size_t joints = prepared.joint_count;
  auto& command = output.command;
  const std::uint8_t degree = interpolation_degree(prepared.interpolation);
  if (!evaluate_trajectory_segment(
          prepared.trajectory, hint, path_time_s, degree,
          std::span<double>(command.position_rad.data(), joints),
          std::span<double>(command.velocity_rad_s.data(), joints),
          std::span<double>(command.acceleration_rad_s2.data(), joints),
          std::span<double>(command.jerk_rad_s3.data(), joints))) {
    return cannot_proceed(output);
  }
  // At the path end the cursor is held, so the effective path rate is zero
  // whatever the governor still does.
  const bool at_end = path_time_s >= end_s;
  const double s = at_end ? 0.0 : governor.scale();
  const double s_dot = at_end ? 0.0 : governor.rate_per_s();
  const double s_ddot = at_end ? 0.0 : governor.scale_acceleration_per_s2();
  // q''' is continuous across knots only for quintic segments; lower degrees
  // have acceleration steps at knots, so their jerk is never qualified.
  const bool jerk_qualified = degree == 5U;
  for (std::size_t j = 0U; j < joints; ++j) {
    const double q1 = command.velocity_rad_s[j];
    const double q2 = command.acceleration_rad_s2[j];
    const double q3 = command.jerk_rad_s3[j];
    command.velocity_rad_s[j] = q1 * s;
    command.acceleration_rad_s2[j] = q2 * s * s + q1 * s_dot;
    command.jerk_rad_s3[j] =
        jerk_qualified ? q3 * s * s * s + 3.0 * q2 * s * s_dot + q1 * s_ddot : 0.0;
    if (!std::isfinite(command.velocity_rad_s[j]) ||
        !std::isfinite(command.acceleration_rad_s2[j]) || !std::isfinite(command.jerk_rad_s3[j])) {
      return cannot_proceed(output);
    }
  }
  command.joint_count = static_cast<std::uint8_t>(joints);
  command.jerk_qualified = jerk_qualified;

  MotionStepStatus status = MotionStepStatus::kRunning;
  const bool scale_at_rest = governor.scale() == 0.0 && governor.rate_per_s() == 0.0;
  if (active_stop && scale_at_rest && !governor.transitioning()) {
    status = MotionStepStatus::kStopped;  // Computed fact, not rest evidence.
  } else if (at_end) {
    status = MotionStepStatus::kComputedEnd;  // Rule 7: not completion.
  } else if (active_stop) {
    status = MotionStepStatus::kBraking;  // Never returns to nominal.
  } else if (scale_at_rest) {
    status = MotionStepStatus::kHolding;
  }

  state.governor = governor;
  state.path_time_s = path_time_s;
  state.segment_hint = hint;
  state.last_tick_ns = inputs.now_ns;
  state.scale_generation = scale_generation;
  state.mode = status;
  state.active_stop = active_stop;

  output.path_time_s = path_time_s;
  output.applied_scale = governor.scale();
  output.scale_rate_per_s = governor.rate_per_s();
  output.status = status;
  output.consumed_scale_generation = scale_generation;
  output.consumed_stop_generation =
      active_stop ? active_stop->stop_generation : StopGeneration{0U};
  return status;
}

}  // namespace manyforge::core
