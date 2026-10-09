#pragma once

#include <array>
#include <cstdint>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>

// Real-time motion step. Every type here is trivially copyable and fixed-size
// so it can cross bounded RT handoffs. The step is noexcept and performs no
// allocation, blocking, logging, clock read, journal access, admission or
// scene work. It is called from the actual native/controller update loop.

namespace manyforge::core {

enum class MotionStepStatus : std::uint8_t {
  kRunning = 0,
  /// Scale zero: path time is held; communication, Stop and liveness checks
  /// continue outside the step.
  kHolding,
  /// On-path braking under an active Stop transition; never returns to
  /// nominal motion.
  kBraking,
  /// The numerical cursor reached the path end. Not "commanded", not
  /// completion and never proof of rest.
  kComputedEnd,
  /// Braking under a Stop transition reached zero scale and zero rate. A
  /// computed fact, not stationary evidence.
  kStopped,
  /// Reversed clock, non-finite state or invalid prepared data. `state` is
  /// unchanged; the adapter applies the containment primitive already
  /// authorized for the binding (hold or fence). Never disables containment.
  kCannotProceed,
};

/// Per-input validation result, reported for every tick. Each input is
/// validated independently; a rejected scale or device fact never blocks a
/// valid Stop.
enum class MotionInputValidation : std::uint8_t {
  kAbsent = 0,
  kAccepted,
  /// Wrong goal fence or a generation not newer than the consumed one.
  kStale,
  /// Non-finite or out-of-range value.
  kInvalid,
  /// Valid but ignored because a Stop transition governs this tick.
  kIgnoredForStop,
};

/// Kernel-selected Stop stage as realized by the shared stage realizer for
/// on-path braking in the step. The step never selects escalation, changes
/// scope or makes an operation terminal. A stronger stage arrives as a newer
/// transition with tighter bounds; it continues from the current braking
/// state. Hold and replacement plans are adapter primitives / continuity-
/// checked installations, not step inputs.
struct StopTransition final {
  ExecutionGoalFence goal;
  StopGeneration stop_generation{0U};
  /// Retained stage identity (index into the prepared participant Stop plan).
  std::uint32_t stage_index{};
  /// Braking bounds for this stage; never looser than the prepared
  /// `stop_braking_bounds`.
  detail::RuntimeScaleBounds braking_bounds{};
  /// Local monotonic deadline computed by the receiver from the retained
  /// remaining budget; never another machine's timestamp.
  std::int64_t braking_deadline_ns{};
};

/// Latest Kernel-accepted runtime-scale target. Coalesced outside RT; the
/// loop sees only the latest one. Applied from the next interval, never
/// retroactively.
struct ScaleTarget final {
  ExecutionGoalFence goal;
  RuntimeScaleGeneration generation{0U};
  double target_scale{1.0};
};

enum class DeviceScalingSource : std::uint8_t {
  /// The mechanism applies no device override; factor must be 1.
  kNone = 0,
  /// The device reported its currently applied override.
  kDeviceReported,
};

/// Qualified device speed-override fact. Composed explicitly with the time
/// law, never multiplied twice and never a fabricated slow clock.
/// Unsupported or uncertain scaling is invalid and never reported as applied.
struct DeviceScalingFacts final {
  DeviceScalingSource source{DeviceScalingSource::kNone};
  double factor{1.0};
  std::uint64_t source_generation{};
  /// Local monotonic receipt time of the source sample.
  std::int64_t received_local_ns{};
  bool valid{};
};

struct MotionStepInputs final {
  /// Unscaled time from the loop's qualified monotonic clock.
  std::int64_t now_ns{};
  /// Kernel-selected stage, or nullptr. Consumed first.
  const StopTransition* stop{};
  /// Latest accepted target, or nullptr.
  const ScaleTarget* scale{};
  DeviceScalingFacts device{};
};

/// Fixed-size step state owned by the loop. Initialized at installation from
/// the prepared motion by `InstallMotionStep`; `std::nullopt` governor means
/// not installed.
struct MotionStepState final {
  std::optional<detail::RuntimeScaleGovernor> governor;
  double path_time_s{};
  std::uint32_t segment_hint{};
  std::int64_t last_tick_ns{};
  ExecutionGoalFence goal;
  RuntimeScaleGeneration scale_generation{0U};
  MotionStepStatus mode{MotionStepStatus::kRunning};
  std::optional<StopTransition> active_stop;
};

/// Fixed-capacity joint command (index < joint_count).
struct MotionJointCommand final {
  std::uint8_t joint_count{};
  /// Jerk is populated only when the interpolation defines q''' and every
  /// term of the composed chain is available.
  bool jerk_qualified{};
  std::array<double, kMaxMotionJoints> position_rad{};
  std::array<double, kMaxMotionJoints> velocity_rad_s{};
  std::array<double, kMaxMotionJoints> acceleration_rad_s2{};
  std::array<double, kMaxMotionJoints> jerk_rad_s3{};
};

/// Generation-bound step result. Not an operation acceptance receipt and not
/// evidence of realization: the adapter writes it and reports realization
/// under its own contract.
struct MotionStepOutput final {
  MotionJointCommand command;
  double path_time_s{};
  double applied_scale{};
  double scale_rate_per_s{};
  MotionStepStatus status{MotionStepStatus::kCannotProceed};
  ExecutionGoalFence goal;
  RuntimeScaleGeneration consumed_scale_generation{0U};
  StopGeneration consumed_stop_generation{0U};
  MotionInputValidation stop_validation{MotionInputValidation::kAbsent};
  MotionInputValidation scale_validation{MotionInputValidation::kAbsent};
  MotionInputValidation device_validation{MotionInputValidation::kAbsent};
  /// now_ns - last_tick_ns - nominal period, measured on the loop clock.
  std::int64_t observed_tick_delay_ns{};
  bool tick_delay_within_envelope{};
};

/// One deterministic motion step.
///
/// Rules (Track 1 contract):
/// 1. Inputs are validated independently, Stop first. A valid Stop takes
///    effect even when scale/device facts are stale or invalid; those are
///    ignored for this tick and reported.
/// 2. Reversed clock, non-finite state or invalid prepared data leaves `state`
///    unchanged and returns kCannotProceed.
/// 3. Elapsed time is unscaled loop time. Within the tick envelope the step
///    advances normally; outside it reports the delay. PROVISIONAL until the
///    late-tick response is qualified: a late tick advances by at most the
///    envelope bound (nominal period + max delay) and the remainder is
///    dropped, never caught up. A capped advance never stretches a Stop
///    deadline: `braking_deadline_ns` stays absolute local monotonic time,
///    enforced by the Stop realizer, not by the step.
/// 4. Order: consume Stop, otherwise the latest valid scale target for the
///    next interval; advance the governor over the elapsed interval with the
///    previous target; advance path time; sample; compose derivatives
///    v=q'*s, a=q''*s^2+q'*s_dot, j=q'''*s^3+3*q''*s*s_dot+q'*s_ddot.
/// 5. Scale zero holds path time.
/// 6. On-path braking retargets the governor toward zero under the Stop bounds
///    from the same tick's actual path time, scale and scale rate. A newer
///    stage continues from the current braking state. A goal-fenced, newer
///    Stop whose values are invalid or that cannot be realized from the
///    current state (rate above its bound, overshoot above the
///    `max_stop_path_rate` ceiling) is reported kInvalid and yields
///    kCannotProceed; it is never silently ignored.
/// 7. kComputedEnd is not completion and never proof of rest.
///
/// On kCannotProceed the output command has `joint_count == 0`.
[[nodiscard]] MotionStepStatus StepMotion(const PreparedMotion& prepared, MotionStepState& state,
                                          const MotionStepInputs& inputs,
                                          MotionStepOutput& output) noexcept;

/// Installs `prepared` into `state` at local monotonic `now_ns`: cursor at
/// the trajectory start, governor at `initial_scale` (the latest accepted
/// target, generation `scale_generation`) under `governor_bounds`, no Stop.
/// Returns false and leaves `state` unchanged for invalid prepared data or
/// scale. Non-throwing and allocation-free.
[[nodiscard]] bool InstallMotionStep(const PreparedMotion& prepared, std::int64_t now_ns,
                                     double initial_scale,
                                     RuntimeScaleGeneration scale_generation,
                                     MotionStepState& state) noexcept;

}  // namespace manyforge::core
