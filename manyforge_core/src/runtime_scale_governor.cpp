#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <stdexcept>

namespace manyforge::core::detail {
namespace {
constexpr double kRoundoff = 64.0 * std::numeric_limits<double>::epsilon();

bool valid_scale(double scale) noexcept {
  return std::isfinite(scale) && scale >= 0.0 && scale <= 1.0;
}

bool within_scale_roundoff(double scale) noexcept {
  return std::isfinite(scale) && scale >= -kRoundoff && scale <= 1.0 + kRoundoff;
}

bool positive(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}
}  // namespace

std::optional<RuntimeScalePreparation> prepare_runtime_scaling(
    std::span<const TrajectoryDerivativeBounds> derivatives,
    std::span<const TrajectoryDerivativeLimits> limits, double headroom_fraction,
    RuntimeScaleBounds software_ceiling) noexcept {
  const std::array participants{TrajectoryDynamicsEnvelope{derivatives, limits}};
  return prepare_runtime_scaling(participants, headroom_fraction, software_ceiling);
}

std::optional<RuntimeScalePreparation> prepare_runtime_scaling(
    std::span<const TrajectoryDynamicsEnvelope> participants, double headroom_fraction,
    RuntimeScaleBounds software_ceiling) noexcept {
  if (participants.empty() || !positive(headroom_fraction) || headroom_fraction >= 1.0 ||
      !positive(software_ceiling.max_rate_per_s) ||
      !positive(software_ceiling.max_rate_change_per_s2)) {
    return std::nullopt;
  }
  RuntimeScalePreparation prepared{1.0, software_ceiling};
  const double nominal_fraction = 1.0 - headroom_fraction;
  for (const auto& participant : participants) {
    const auto derivatives = participant.derivatives;
    const auto limits = participant.limits;
    if (derivatives.empty() || derivatives.size() != limits.size() ||
        !positive(participant.minimum_time_multiplier) || participant.minimum_time_multiplier < 1.0)
      return std::nullopt;
    prepared.trajectory_time_multiplier =
        std::max(prepared.trajectory_time_multiplier, participant.minimum_time_multiplier);
    for (std::size_t index = 0U; index < derivatives.size(); ++index) {
      const auto& d = derivatives[index];
      const auto& limit = limits[index];
      if (!std::isfinite(d.velocity) || d.velocity < 0.0 || !std::isfinite(d.acceleration) ||
          d.acceleration < 0.0 || !std::isfinite(d.jerk) || d.jerk < 0.0 ||
          !positive(limit.velocity) || !positive(limit.acceleration) ||
          !positive(nominal_fraction * limit.acceleration) ||
          (limit.jerk.has_value() &&
           (!positive(*limit.jerk) || !positive(nominal_fraction * *limit.jerk)))) {
        return std::nullopt;
      }
      prepared.trajectory_time_multiplier =
          std::max({prepared.trajectory_time_multiplier, d.velocity / limit.velocity,
                    std::sqrt(d.acceleration / (nominal_fraction * limit.acceleration))});
      if (limit.jerk.has_value()) {
        prepared.trajectory_time_multiplier =
            std::max(prepared.trajectory_time_multiplier,
                     std::cbrt(d.jerk / (nominal_fraction * *limit.jerk)));
      }
    }
  }
  const double dilation = prepared.trajectory_time_multiplier;
  if (!positive(dilation)) return std::nullopt;
  for (const auto& participant : participants) {
    const auto derivatives = participant.derivatives;
    const auto limits = participant.limits;
    for (std::size_t index = 0U; index < derivatives.size(); ++index) {
      const auto& source = derivatives[index];
      const auto& limit = limits[index];
      const double velocity = source.velocity / dilation;
      const double acceleration = source.acceleration / dilation / dilation;
      const double jerk = source.jerk / dilation / dilation / dilation;
      if (velocity > 0.0) {
        prepared.scale_bounds.max_rate_per_s = std::min(
            prepared.scale_bounds.max_rate_per_s, (limit.acceleration - acceleration) / velocity);
      }
      if (limit.jerk.has_value()) {
        const double margin = *limit.jerk - jerk;
        if (!(margin > 0.0)) return std::nullopt;
        if (acceleration > 0.0) {
          prepared.scale_bounds.max_rate_per_s =
              std::min(prepared.scale_bounds.max_rate_per_s, margin / acceleration / 6.0);
        }
        if (velocity > 0.0) {
          prepared.scale_bounds.max_rate_change_per_s2 =
              std::min(prepared.scale_bounds.max_rate_change_per_s2, margin / velocity / 2.0);
        }
      }
    }
  }
  // Keep arithmetic rounding away from a bound shared by two jerk terms.
  prepared.scale_bounds.max_rate_per_s *= 1.0 - 1e-12;
  prepared.scale_bounds.max_rate_change_per_s2 *= 1.0 - 1e-12;
  if (!positive(prepared.scale_bounds.max_rate_per_s) ||
      !positive(prepared.scale_bounds.max_rate_change_per_s2)) {
    return std::nullopt;
  }
  return prepared;
}

RuntimeScaleGovernor::RuntimeScaleGovernor(double initial_scale, RuntimeScaleBounds bounds)
    : bounds_(bounds), scale_(initial_scale), target_(initial_scale) {
  if (!valid_scale(initial_scale) || !std::isfinite(bounds.max_rate_per_s) ||
      bounds.max_rate_per_s <= 0.0 || !std::isfinite(bounds.max_rate_change_per_s2) ||
      bounds.max_rate_change_per_s2 <= 0.0) {
    throw std::invalid_argument(
        "runtime scale governor requires finite positive bounds and [0,1] scale");
  }
}

RuntimeScaleGovernor::RuntimeScaleGovernor(double initial_scale, RuntimeScaleBounds bounds,
                                           double initial_rate_per_s, double target_scale)
    : RuntimeScaleGovernor(initial_scale, bounds) {
  if (!std::isfinite(initial_rate_per_s) ||
      std::abs(initial_rate_per_s) > bounds.max_rate_per_s * (1.0 + kRoundoff)) {
    throw std::invalid_argument("runtime scale continuation has an invalid rate");
  }
  rate_ = initial_rate_per_s;
  target_ = -1.0;  // Force preparation, even when the new target equals the current scale.
  if (!retarget(target_scale)) {
    throw std::invalid_argument("runtime scale continuation cannot preserve its bounds");
  }
}

double RuntimeScaleGovernor::position(const Phase& phase, double time_s) noexcept {
  return phase.initial_scale + time_s * (phase.initial_rate + 0.5 * phase.rate_change * time_s);
}

bool RuntimeScaleGovernor::valid_phase(const Phase& phase, double max_rate) noexcept {
  if (!std::isfinite(phase.duration_s) || phase.duration_s < 0.0 ||
      !within_scale_roundoff(phase.initial_scale) ||
      !within_scale_roundoff(position(phase, phase.duration_s))) {
    return false;
  }
  const double final_rate = phase.initial_rate + phase.rate_change * phase.duration_s;
  if (!std::isfinite(final_rate) || std::abs(phase.initial_rate) > max_rate * (1.0 + kRoundoff) ||
      std::abs(final_rate) > max_rate * (1.0 + kRoundoff)) {
    return false;
  }
  if (phase.rate_change != 0.0) {
    const double extremum = -phase.initial_rate / phase.rate_change;
    if (extremum > 0.0 && extremum < phase.duration_s &&
        !within_scale_roundoff(position(phase, extremum))) {
      return false;
    }
  }
  return true;
}

bool RuntimeScaleGovernor::retarget(double target_scale) noexcept {
  if (!valid_scale(target_scale)) return false;
  if (target_scale == target_) return true;

  const double acceleration = bounds_.max_rate_change_per_s2;
  const double distance = target_scale - scale_;
  const double stop_distance = 0.5 * rate_ * (std::abs(rate_) / acceleration);
  // A captured terminal-braking phase already has its unique one-phase
  // solution. Re-solving the triangular peak subtracts equal terms and can
  // produce a negative peak or a negative, sub-roundoff acceleration duration.
  if (rate_ != 0.0 && std::abs(distance - stop_distance) <=
                          kRoundoff * (std::abs(distance) + std::abs(stop_distance))) {
    Phase brake{scale_, rate_, -std::copysign(acceleration, rate_), std::abs(rate_) / acceleration};
    if (!valid_phase(brake, bounds_.max_rate_per_s)) return false;
    phases_[0] = brake;
    phase_count_ = 1U;
    phase_index_ = 0U;
    phase_elapsed_s_ = 0.0;
    target_ = target_scale;
    return true;
  }
  // Choose direction relative to the stop point, not just the target. A new
  // target behind that point requires braking the existing rate and reversing.
  const double direction = distance >= stop_distance ? 1.0 : -1.0;
  const double initial_rate = direction * rate_;
  const double directed_distance = direction * distance;
  const double distance_term = acceleration * directed_distance;
  const double rate_term = 0.5 * initial_rate * initial_rate;
  double peak_squared = distance_term + rate_term;
  // Capturing a brake-only tail reconstructs the difference of equal terms.
  // Accept only cancellation-sized negative roundoff, not an infeasible rate.
  const double cancellation_bound = kRoundoff * (std::abs(distance_term) + rate_term);
  if (!std::isfinite(peak_squared) || peak_squared < -cancellation_bound) return false;
  peak_squared = std::max(0.0, peak_squared);
  const double peak = std::min(bounds_.max_rate_per_s, std::sqrt(peak_squared));
  const double accelerate_s = (peak - initial_rate) / acceleration;
  const double decelerate_s = peak / acceleration;
  const double ramp_distance =
      0.5 * (initial_rate + peak) * accelerate_s + 0.5 * peak * decelerate_s;
  const double cruise_s =
      peak > 0.0 ? std::max(0.0, (directed_distance - ramp_distance) / peak) : 0.0;
  const std::array<double, 3U> durations{accelerate_s, cruise_s, decelerate_s};
  const std::array<double, 3U> changes{direction * acceleration, 0.0, -direction * acceleration};
  std::array<Phase, 3U> prepared{};
  std::size_t count = 0U;
  double next_scale = scale_;
  double next_rate = rate_;
  double total_duration = 0.0;
  for (std::size_t index = 0U; index < durations.size(); ++index) {
    if (!std::isfinite(durations[index]) || durations[index] < 0.0) return false;
    if (durations[index] == 0.0) continue;
    Phase phase{next_scale, next_rate, changes[index], durations[index]};
    if (!valid_phase(phase, bounds_.max_rate_per_s)) return false;
    prepared[count++] = phase;
    next_scale = position(phase, phase.duration_s);
    next_rate += phase.rate_change * phase.duration_s;
    total_duration += phase.duration_s;
  }
  if (!std::isfinite(total_duration) || std::abs(next_scale - target_scale) > kRoundoff ||
      std::abs(next_rate) > kRoundoff * bounds_.max_rate_per_s) {
    return false;
  }
  // Commit only after all intermediate extrema and the terminal state pass.
  phases_ = prepared;
  phase_count_ = count;
  phase_index_ = 0U;
  phase_elapsed_s_ = 0.0;
  target_ = target_scale;
  return true;
}

std::optional<double> RuntimeScaleGovernor::advance(double dt_s) noexcept {
  if (!std::isfinite(dt_s) || dt_s < 0.0) return std::nullopt;
  auto next = *this;
  auto result = next.advance_in_place(dt_s);
  if (result.has_value()) *this = next;
  return result;
}

std::optional<double> RuntimeScaleGovernor::advance_in_place(double dt_s) noexcept {
  double integral = 0.0;
  // Each iteration consumes a phase or all remaining dt. There are at most
  // three phases even when a tick crosses the complete transition.
  while (phase_index_ < phase_count_ && dt_s > 0.0) {
    const auto& phase = phases_[phase_index_];
    const double remaining = phase.duration_s - phase_elapsed_s_;
    const bool completes = dt_s >= remaining;
    const double step = std::min(dt_s, remaining);
    integral += step * (scale_ + step * (0.5 * rate_ + step * phase.rate_change / 6.0));
    phase_elapsed_s_ = completes ? phase.duration_s : phase_elapsed_s_ + step;
    if (phase_index_ + 1U == phase_count_ && phase_elapsed_s_ > 0.5 * phase.duration_s) {
      // Near scale=0, evaluating from the start subtracts nearly equal terms
      // and can erase the small positive scale while leaving a negative rate.
      // The solved terminal state is exact; evaluate backward from it instead.
      const double tail = phase.duration_s - phase_elapsed_s_;
      scale_ = target_ + 0.5 * phase.rate_change * tail * tail;
      rate_ = -phase.rate_change * tail;
    } else {
      scale_ = position(phase, phase_elapsed_s_);
      rate_ = phase.initial_rate + phase.rate_change * phase_elapsed_s_;
    }
    dt_s -= step;
    if (completes) {
      ++phase_index_;
      phase_elapsed_s_ = 0.0;
      if (phase_index_ == phase_count_) {
        scale_ = target_;
        rate_ = 0.0;
      }
    }
  }
  integral += scale_ * dt_s;
  if (!std::isfinite(integral) || integral < 0.0 || !within_scale_roundoff(scale_) ||
      !std::isfinite(rate_)) {
    return std::nullopt;
  }
  scale_ = std::clamp(scale_, 0.0, 1.0);
  return integral;
}

double RuntimeScaleGovernor::remaining_transition_s() const noexcept {
  double remaining = -phase_elapsed_s_;
  for (std::size_t index = phase_index_; index < phase_count_; ++index) {
    remaining += phases_[index].duration_s;
  }
  return remaining;
}

std::optional<RuntimeScaleTimeAnchor> sample_runtime_scale_time(
    const RuntimeScaleTimeAnchor& anchor, std::int64_t timestamp_ns) noexcept {
  if (timestamp_ns < anchor.timestamp_ns || !std::isfinite(anchor.trajectory_time_s) ||
      anchor.trajectory_time_s < 0.0)
    return std::nullopt;
  auto result = anchor;
  // Unsigned subtraction avoids signed overflow even at an injected clock's
  // extreme range. Subtract before conversion to preserve short intervals at
  // large absolute timestamps.
  const auto elapsed_ns =
      static_cast<std::uint64_t>(timestamp_ns) - static_cast<std::uint64_t>(anchor.timestamp_ns);
  const auto advance = result.governor.advance(static_cast<double>(elapsed_ns) / 1e9);
  if (!advance || !std::isfinite(result.trajectory_time_s + *advance)) return std::nullopt;
  result.trajectory_time_s += *advance;
  result.timestamp_ns = timestamp_ns;
  return result;
}

}  // namespace manyforge::core::detail
