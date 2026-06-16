#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Source used for the soft-on-path stop initial condition.
enum class SoftOnPathStateSource {
  /// Fresh backend/controller feedback such as FJT feedback,
  /// controller state, or `/joint_states`.
  kMeasuredFeedback,
  /// Accepted trajectory model plus the applied runtime velocity scale.
  kModelDerived,
};

/// Trip-time state estimate for a soft-on-path stop.
struct SoftOnPathInitialStateDTO {
  RobotStateDTO state;
  /// Optional active trajectory model time. Required when model-derived
  /// velocity is used; otherwise only a hint for projection.
  std::optional<double> trajectory_time_from_start_s;
  SoftOnPathStateSource position_source =
      SoftOnPathStateSource::kMeasuredFeedback;
  SoftOnPathStateSource velocity_source =
      SoftOnPathStateSource::kMeasuredFeedback;
  /// Runtime velocity scale already applied to the original trajectory at
  /// trip time. Used only for model-derived velocity and diagnostics.
  double runtime_velocity_scale_at_trip = 1.0;
  std::int64_t sample_age_ns = 0;
};

/// Planner options that are deployment/runtime facts rather than node policy.
struct SoftOnPathPlannerConfig {
  /// Handoff budget from kernel trip/latch time to the first useful
  /// controller waypoint.
  double stop_traj_delta_s = 0.05;
  /// Sampling period for the generated derivative-complete ramp.
  double sample_period_s = 0.01;
  /// Explicitly authorize model-derived velocity when measured velocity is
  /// missing or marked as model-derived.
  bool allow_model_velocity_fallback = false;
  /// Optional best-effort drift limit. When absent, drift is measured and
  /// reported but not a planner failure.
  std::optional<double> max_initial_path_deviation_rad;
  /// Optional measured-velocity coherence limit. When absent, off-path
  /// velocity is preserved in the anchor waypoint and measured in diagnostics
  /// but does not reject the plan.
  std::optional<double> max_initial_velocity_deviation_rad_s;
  /// Optional deployment jerk limits. Missing joints default from
  /// acceleration limit and effective ramp time.
  std::map<std::string, double> per_joint_max_jerk_rad_s3;
};

struct SoftOnPathStopPlanDTO {
  TrajectoryDTO replacement_trajectory;
  double projected_time_from_start_s = 0.0;
  double effective_ramp_s = 0.0;
  double stop_traj_delta_s = 0.0;
  double initial_path_deviation_rad = 0.0;
  double initial_velocity_deviation_rad_s = 0.0;
  double path_deviation_max_rad = 0.0;
  double runtime_velocity_scale_at_trip = 1.0;
  bool used_model_velocity = false;
  std::string position_source;
  std::string velocity_source;
  /// `deployment`, `default`, or `mixed`.
  std::string jerk_limit_source;
  InterruptStructuredValue diagnostics;
};

struct SoftOnPathFeasibilityEnvelopeDTO {
  std::size_t checked_samples = 0;
  std::size_t natural_completion_samples = 0;
  double first_sample_time_from_start_s = 0.0;
  double last_sample_time_from_start_s = 0.0;
  double max_path_deviation_rad = 0.0;
  double max_initial_velocity_deviation_rad_s = 0.0;
  std::string jerk_limit_source;
  InterruptStructuredValue diagnostics;
};

/// Build a wall-time, best-effort path-preserving replacement trajectory for
/// `MotionInterruptStopMode::kSoftOnPath`.
///
/// The planner is intentionally side-effect free. Backends use the returned
/// trajectory as the command shape; kernel/backend code remains responsible for
/// runtime-scale isolation, dispatch, controller acknowledgement, and outcome
/// persistence.
Result<SoftOnPathStopPlanDTO> plan_soft_on_path_stop(
    const TrajectoryDTO& accepted_trajectory,
    const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state,
    const SoftOnPathPlannerConfig& config = {});

/// Validate, before backend dispatch, that model-derived trip samples along the
/// accepted trajectory can produce a `soft_on_path` ramp under the same planner
/// limits. This is the plan-time feasibility envelope; it is
/// conservative because it assumes runtime scale 1.0 for the original motion.
Result<SoftOnPathFeasibilityEnvelopeDTO>
validate_soft_on_path_feasibility_envelope(
    const TrajectoryDTO& accepted_trajectory,
    const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathPlannerConfig& config = {});

/// True when a planner rejection means the accepted trajectory is already too
/// close to its natural endpoint for a useful wall-time `soft_on_path` ramp and
/// the caller may apply `allow_natural_completion_on_short_path` policy.
bool soft_on_path_error_recommends_natural_completion(
    const ErrorDTO& error);

/// Measure joint-space projection residual from an observed state to the
/// accepted trajectory path. Used by backends to turn controller feedback into
/// runtime path-deviation evidence for `soft_on_path`.
Result<double> measure_soft_on_path_path_deviation(
    const TrajectoryDTO& accepted_trajectory,
    const RobotStateDTO& observed_state);

/// Configuration for re-aligning a soft-stop replacement trajectory to the
/// compute latency between the trip instant and dispatch.
struct DispatchLatencyShiftConfig {
  /// Leading continuity-anchor time (negative; seconds before dispatch "now").
  double anchor_negative_time_s = -0.010;
  /// Reject (→ hard stop) once the compute latency reaches this fraction of the
  /// requested stop duration. Compute is a few ms, so this only trips on a
  /// too-small stop time or a compute hiccup.
  double max_latency_fraction = 0.2;
};

/// Re-align a planned soft-stop replacement trajectory for the compute latency
/// between the trip instant (when `planned` was computed) and dispatch, and
/// prepend a negative-time continuity anchor at the fresh dispatch-time observed
/// state so stock JTC's spline starts continuously (no backward-pull bump).
///
/// Subtracts `compute_latency_s` from every planned waypoint's `time_from_start`
/// and drops those that become negative (consumed during the gap), then prepends
/// the anchor. Returns `Err` — signalling the caller to escalate to a hard stop —
/// when `compute_latency_s >= config.max_latency_fraction * stop_duration_s`, or
/// when every future waypoint was consumed.
Result<TrajectoryDTO> shift_soft_stop_for_dispatch_latency(
    const TrajectoryDTO& planned,
    double compute_latency_s,
    double stop_duration_s,
    const RobotStateDTO& dispatch_anchor_state,
    const DispatchLatencyShiftConfig& config = {});

/// Result of the minimum-time on-path stop feasibility sweep.
struct SoftOnPathMinStopTimeDTO {
  /// Fastest budget-respecting time to bring the path-rate to zero from the
  /// trip rate, decelerating along the validated path within the per-joint
  /// limits. This is the lower bound on the soft-stop duration: a requested
  /// `deceleration_time_s` below it is infeasible (caller escalates to a hard
  /// stop); at or above it the stop can be re-timed to land exactly on time.
  double min_stop_time_s = 0.0;
  /// Path cursor (`time_from_start` along the accepted trajectory) where the
  /// path-rate reaches zero under the minimum-time profile.
  double stop_path_time_s = 0.0;
  /// Path-length the minimum-time stop consumes (`stop_path_time_s` minus the
  /// trip cursor), in accepted-trajectory time units.
  double stop_distance_path_s = 0.0;
  /// False when the minimum-time stop would run past the accepted trajectory's
  /// final waypoint (the path ends before the robot can stop on it).
  bool reaches_rest_on_path = true;
  /// Joint whose deceleration limit bound the sweep most often (diagnostic).
  std::string binding_joint;
};

/// Compute the minimum-time on-path stop: forward-integrate the path-rate phase
/// plane from `(trip_path_time_s, trip_path_rate)` applying, at each step, the
/// most negative path-rate acceleration that keeps every joint's executed
/// acceleration `q''(u)·u̇² + q'(u)·u̇_dot` within its per-joint deceleration
/// limit, until the path-rate reaches zero. Side-effect free; the planner uses
/// it to gate exact-time feasibility (and the redistribution target). `Err` on
/// malformed inputs (non-finite rate, missing per-joint limit, etc.).
Result<SoftOnPathMinStopTimeDTO> compute_soft_on_path_min_stop_time(
    const TrajectoryDTO& accepted_trajectory,
    double trip_path_time_s,
    double trip_path_rate,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    double integration_step_s = 0.001);

/// One sample of a jerk-limited soft-stop path-rate profile.
struct JerkLimitedStopSampleDTO {
  double time_from_ramp_start_s = 0.0;  // t within the ramp (0 at handoff end)
  double path_time_s = 0.0;             // path cursor s = u(t)
  double path_rate = 0.0;               // u̇(t)
  double path_rate_accel = 0.0;         // ü(t)
};

struct JerkLimitedStopProfileDTO {
  std::vector<JerkLimitedStopSampleDTO> samples;  // from (u_dot0, u_ddot0) to rest
  double achieved_stop_time_s = 0.0;
  bool reaches_rest_on_path = false;
  /// True iff the achieved stop time matches the requested time within the
  /// retimer's sample-resolution tolerance.
  bool hits_target_time = false;
  /// True iff the emitted terminal sample is a clean hold point:
  /// `path_rate == 0` and `path_rate_accel == 0`.
  bool terminal_state_zero = false;
  /// True iff the emitted trajectory, sampled through the same Hermite/quintic
  /// interpolation used for dispatch, stays within per-joint acceleration and
  /// finite-difference jerk limits.
  bool within_budget = false;
  /// Single caller-facing predicate: only true when the profile is suitable to
  /// turn into a replacement trajectory. False means fall back / hard-stop.
  bool dispatchable = false;
  /// First validation reason that made `within_budget`/`dispatchable` false.
  std::string validation_failure_reason;
  /// True when the generic retimer used the backward velocity envelope to
  /// pre-slow for a future path bottleneck.
  bool used_backward_envelope = false;
  double target_decel_path_rate = 0.0;  // the time-fit's converged target (diag)
};

/// Jerk-limited exact-time re-timing of the soft stop's ramp. Forward-integrates
/// the path-rate phase plane `(s, u̇, ü)` from the C2 seam `(u̇0, ü0)`, slewing ü
/// within a path-rate jerk budget derived from the per-joint jerk limits and
/// clamping it to the per-joint acceleration budget (riding corners at the
/// budget edge), with an S-curve endpoint that brings `(u̇, ü)` to `(0, 0)`. The
/// target deceleration is binary-searched so the ramp reaches rest at exactly
/// `target_stop_time_s`. A conservative backward velocity envelope is applied
/// as a future-corner viability/look-ahead gate; when it binds but the emitted
/// profile cannot be proven safe, `dispatchable` remains false. The first
/// emitted sample is the exact incoming seam state. The final emitted sample
/// must be a zero-rate, zero-acceleration hold before `dispatchable` can become
/// true. The emitted samples are also converted back into a derivative-complete
/// joint trajectory and oversampled through the dispatch interpolation path; any
/// acceleration/jerk overrun clears `within_budget`/`dispatchable`. When the
/// jerk limit or path shape makes the requested stop infeasible, the helper
/// returns the best profile it found with `dispatchable=false` so the caller
/// falls back. `Err` only on malformed inputs.
Result<JerkLimitedStopProfileDTO> compute_jerk_limited_stop_profile(
    const TrajectoryDTO& accepted_trajectory,
    double start_path_time_s,
    double start_path_rate,
    double start_path_rate_accel,
    double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3,
    double sample_period_s = 0.01);

}  // namespace manyforge::core
