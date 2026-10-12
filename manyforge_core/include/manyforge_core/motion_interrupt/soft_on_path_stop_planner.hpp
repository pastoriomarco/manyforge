#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
  SoftOnPathStateSource position_source = SoftOnPathStateSource::kMeasuredFeedback;
  SoftOnPathStateSource velocity_source = SoftOnPathStateSource::kMeasuredFeedback;
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
  /// Authored (deployment) jerk limits, used verbatim. While
  /// `jerk_limits_enabled`, a joint without one gets main's default from its
  /// acceleration limit and the phase duration: deceleration time minus
  /// `stop_traj_delta_s` for a soft ramp (and a spring-back forward half),
  /// half the deceleration time for a spring-back return.
  std::map<std::string, double> per_joint_max_jerk_rad_s3;
  /// Internal legacy callers preserve their prior shaping until the owning
  /// Deployment is migrated. Runtime wiring must pass the saved activation.
  /// Off uses the same smooth ramp and acceleration/path checks, but neither
  /// jerk rejection nor the jerk-only alternative solver.
  bool jerk_limits_enabled = true;
  /// Stop deadline on the replacement's own clock (time 0 is the trip sample
  /// the plan starts from). Spring-back may start its return after a late
  /// forward braking end only while the whole Stop still ends by this time.
  /// Absent: the requested deceleration time is the deadline.
  std::optional<double> stop_deadline_s;
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
  /// `deployment`, `default`, `mixed`, or `disabled`.
  std::string jerk_limit_source;
  InterruptStructuredValue diagnostics;
  /// Exact scalar interval of the forward plan, retained only between these
  /// subordinate planners. Spring-back must not recover it by nearest pose;
  /// an accepted path can visit the same pose more than once.
  double command_path_time_s = 0.0;
  double forward_stop_path_time_s = 0.0;
  /// §3.3 cursor mapping: the accepted trajectory's path time of each
  /// `replacement_trajectory` waypoint (1:1), as the planner sampled it. It
  /// rises through braking, holds at rest and falls on a spring-back return.
  std::vector<double> replacement_path_time_s;
};

/// Build a wall-time, best-effort path-preserving replacement trajectory for
/// `MotionInterruptStopMode::kSoftOnPath`. The command-frame lead-in
/// (`stop_traj_delta_s`) sits inside `deceleration_time_s`; the ramp lasts the
/// remainder. The trajectory must carry its `runtime_scale_bounds`. When the
/// ramp would overrun the accepted path end the planner returns
/// `soft_on_path_remaining_time_short`, recommending natural completion when
/// the profile allows it, else the spring-back fallback. Initial path or
/// velocity deviation beyond its configured limit recommends the hard stop.
///
/// The planner is intentionally side-effect free. Backends use the returned
/// trajectory as the command shape; kernel/backend code remains responsible for
/// runtime-scale isolation, dispatch, controller acknowledgement, and outcome
/// persistence.
Result<SoftOnPathStopPlanDTO> plan_soft_on_path_stop(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config = {});

/// Build the non-critical `spring_back_hold` replacement as a two-phase,
/// path-preserving trajectory:
///   1. a normal soft-on-path deceleration over half of `deceleration_time_s`;
///   2. a rest-to-rest return on the same accepted path back to the trip cursor
///      over the remaining half: main's quintic when its peak path rate
///      (1.875 * distance / half) is within the trajectory's
///      `max_stop_path_rate`, otherwise a ramp-cruise-ramp return capped at it.
///
/// Forward braking must end at rest; if it ends after the half boundary, the
/// return starts after it while the whole Stop still fits
/// `SoftOnPathPlannerConfig::stop_deadline_s`, otherwise the hard stop is
/// recommended. Natural completion is not re-checked here (it is decided once,
/// before spring-back).
///
/// This is intentionally a replacement/preemption trajectory, not a JTC cancel
/// path. The first phase reuses `plan_soft_on_path_stop`, so it inherits the
/// command-frame lead-in, dispatch-latency compatibility, and limit checks used
/// by `soft_on_path`.
Result<SoftOnPathStopPlanDTO> plan_spring_back_on_path_stop(
    const TrajectoryDTO& accepted_trajectory, const MotionInterruptStopProfileDTO& stop_profile,
    const SoftOnPathInitialStateDTO& initial_state, const SoftOnPathPlannerConfig& config = {});

/// True when a planner rejection means the accepted trajectory is already too
/// close to its natural endpoint for a useful wall-time `soft_on_path` ramp and
/// the caller may apply `allow_natural_completion_on_short_path` policy.
bool soft_on_path_error_recommends_natural_completion(const ErrorDTO& error);

/// True when a planner rejection means no feasible on-path soft stop exists and
/// the caller MUST escalate to a hard stop (e.g. the requested time is below the
/// minimum budget-respecting on-path stop, or neither the retimer nor the ALI
/// backtracking candidate could prove a dispatchable replacement). Backends
/// MUST honor this with a real hard deceleration, not route it through
/// `spring_back_hold`.
bool soft_on_path_error_recommends_hard_stop(const ErrorDTO& error);

/// Measure joint-space projection residual from an observed state to the
/// accepted trajectory path. Used by backends to turn controller feedback into
/// runtime path-deviation evidence for `soft_on_path`.
Result<double> measure_soft_on_path_path_deviation(const TrajectoryDTO& accepted_trajectory,
                                                   const RobotStateDTO& observed_state);

/// Configuration for re-aligning a soft-stop replacement trajectory to the
/// compute+dispatch latency between the trip cursor snapshot and dispatch.
struct DispatchLatencyShiftConfig {
  /// Leading continuity-pin time (negative; seconds before dispatch "now").
  double anchor_negative_time_s = -0.010;
};

/// Re-align a planned soft-stop replacement for the compute+dispatch latency
/// between the trip cursor snapshot (when `planned` was computed) and dispatch.
/// During that latency the controller's command advances along the path, so the
/// lead-in's earliest points fall behind the live command frame. Subtracts
/// `compute_latency_s` from every planned waypoint's `time_from_start`, drops
/// those that become negative (consumed during the gap), and prepends a
/// negative-time continuity pin sampled from `accepted_trajectory` at the
/// advanced command cursor (`command_cursor_s + path_time_rate*compute_latency_s`)
/// carrying the full command-frame `(q, qdot, qddot)` — so stock JTC's spline
/// starts through the live command (no backward-pull bump). Returns `Err`
/// (caller escalates to a hard stop) when `compute_latency_s >= lead_in_budget_s`
/// (the latency consumed the command-continuation lead-in) or when every future
/// waypoint was consumed.
Result<TrajectoryDTO> shift_soft_stop_for_dispatch_latency(
    const TrajectoryDTO& planned, double compute_latency_s, double lead_in_budget_s,
    const TrajectoryDTO& accepted_trajectory, double command_cursor_s, double path_time_rate,
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
    const TrajectoryDTO& accepted_trajectory, double trip_path_time_s, double trip_path_rate,
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
  /// True iff the emitted trajectory's Hermite/quintic segment extrema stay
  /// within per-joint acceleration and jerk limits.
  bool within_budget = false;
  /// Single caller-facing predicate: only true when the profile is suitable to
  /// turn into a replacement trajectory. False means fall back / hard-stop.
  bool dispatchable = false;
  /// First validation reason that made `within_budget`/`dispatchable` false.
  std::string validation_failure_reason;
  /// True when the generic retimer used the backward velocity envelope to
  /// pre-slow for a future path bottleneck.
  bool used_backward_envelope = false;
  double target_decel_path_rate = 0.0;  // time-fit target; zero for analytic braking
  /// The derivative-complete joint trajectory built from `samples` during
  /// dispatch validation (times are ramp-relative, 1:1 with `samples`).
  /// Consumers that turn a dispatchable profile into a replacement
  /// trajectory can reuse these waypoints instead of resampling the path.
  TrajectoryDTO emitted_trajectory;
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
/// joint trajectory and checked at its acceleration/jerk polynomial extrema; any
/// acceleration/jerk overrun clears `within_budget`/`dispatchable`. When the
/// jerk limit or path shape makes the requested stop infeasible, the helper
/// returns the best profile it found with `dispatchable=false` so the caller
/// falls back. `Err` only on malformed inputs.
Result<JerkLimitedStopProfileDTO> compute_jerk_limited_stop_profile(
    const TrajectoryDTO& accepted_trajectory, double start_path_time_s, double start_path_rate,
    double start_path_rate_accel, double target_stop_time_s,
    const std::map<std::string, double>& per_joint_max_deceleration_rad_s2,
    const std::map<std::string, double>& per_joint_max_jerk_rad_s3, double sample_period_s = 0.01);

}  // namespace manyforge::core
