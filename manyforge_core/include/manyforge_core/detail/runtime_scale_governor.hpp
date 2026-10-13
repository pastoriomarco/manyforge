#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace manyforge::core::detail {

// Subordinate numerical mechanism, not an admission/lifecycle API. The caller
// derives these bounds from the accepted trajectory and effective constraints.
struct RuntimeScaleBounds {
  double max_rate_per_s;
  double max_rate_change_per_s2;
};

struct TrajectoryDerivativeBounds {
  double velocity{};
  double acceleration{};
  double jerk{};
};

struct TrajectoryDerivativeLimits {
  double velocity;
  double acceleration;
  std::optional<double> jerk;
};

struct RuntimeScalePreparation {
  double trajectory_time_multiplier{1.0};
  RuntimeScaleBounds scale_bounds;
};

// Borrowed cold-path dynamics facts. All envelopes must describe
// the same pre-dilation schedule; this numerical helper owns no barrier or
// dispatch authority and retains none of these spans.
struct TrajectoryDynamicsEnvelope {
  std::span<const TrajectoryDerivativeBounds> derivatives;
  std::span<const TrajectoryDerivativeLimits> limits;
  // Additional proved timing floor, e.g. a TCP velocity-only constraint.
  // Include it before deriving governor margins, not as a later retiming pass.
  double minimum_time_multiplier{1.0};
};

// Choose one dilation and governor budget across the supplied dimensions.
// Ordinary candidate preparation also uses the envelope's TCP timing floor.
// This numerical helper owns no execution clock or participant coordination.
[[nodiscard]] std::optional<RuntimeScalePreparation> prepare_runtime_scaling(
    std::span<const TrajectoryDynamicsEnvelope> participants, double headroom_fraction,
    RuntimeScaleBounds software_ceiling) noexcept;

// Cold-path calculation over matching resolved dimensions. TCP derivative
// bounds, when applicable, join the same minimum as joint bounds. No identity
// resolution happens here. Missing jerk means off, never an invented cap.
[[nodiscard]] std::optional<RuntimeScalePreparation> prepare_runtime_scaling(
    std::span<const TrajectoryDerivativeBounds> derivatives,
    std::span<const TrajectoryDerivativeLimits> limits, double headroom_fraction,
    RuntimeScaleBounds software_ceiling) noexcept;

// A second-order time-law governor: scale and scale rate are continuous even
// when the target changes during a ramp. Three analytic phases suffice; no
// queue, allocation, clock read, owner call or integration substep is needed.
class RuntimeScaleGovernor final {
 public:
  RuntimeScaleGovernor(double initial_scale, RuntimeScaleBounds bounds);
  // Reconstruct the same bounded continuation at a captured Stop seam.
  RuntimeScaleGovernor(double initial_scale, RuntimeScaleBounds bounds, double initial_rate_per_s,
                       double target_scale);

  [[nodiscard]] bool retarget(double target_scale) noexcept;
  // Returns the exact integral of scale over dt, i.e. trajectory-time advance.
  // Invalid/unrepresentable input leaves the complete current state unchanged.
  [[nodiscard]] std::optional<double> advance(double dt_s) noexcept;

  [[nodiscard]] double scale() const noexcept { return scale_; }
  [[nodiscard]] double rate_per_s() const noexcept { return rate_; }
  [[nodiscard]] double target_scale() const noexcept { return target_; }
  [[nodiscard]] bool transitioning() const noexcept { return phase_index_ < phase_count_; }
  [[nodiscard]] double remaining_transition_s() const noexcept;

 private:
  struct Phase {
    double initial_scale;
    double initial_rate;
    double rate_change;
    double duration_s;
  };

  [[nodiscard]] std::optional<double> advance_in_place(double dt_s) noexcept;
  [[nodiscard]] static double position(const Phase& phase, double time_s) noexcept;
  [[nodiscard]] static bool valid_phase(const Phase& phase, double max_rate) noexcept;

  RuntimeScaleBounds bounds_;
  double scale_;
  double rate_{};
  double target_;
  std::array<Phase, 3U> phases_{};
  std::size_t phase_count_{};
  std::size_t phase_index_{};
  double phase_elapsed_s_{};
};

// Immutable input for evaluating an execution's law at a monotonic clock instant.
// Sampling a copy leaves its source anchor unchanged and grants no dispatch authority.
struct RuntimeScaleTimeAnchor {
  double trajectory_time_s;
  RuntimeScaleGovernor governor;
  std::int64_t timestamp_ns;
};

[[nodiscard]] std::optional<RuntimeScaleTimeAnchor> sample_runtime_scale_time(
    const RuntimeScaleTimeAnchor& anchor, std::int64_t timestamp_ns) noexcept;

}  // namespace manyforge::core::detail
