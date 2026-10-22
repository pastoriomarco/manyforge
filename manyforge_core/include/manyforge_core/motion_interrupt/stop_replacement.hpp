#pragma once

#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

// ---------------------------------------------------------------------------
// Stop replacement building blocks (shared execution design §4.2).
//
// Side-effect free functions used by the Core Stop-stage helper and by the
// test-support exporter for Track 2's pinned-JTC characterization. Three time
// bases stay explicit: the active trajectory's path time (the controller's
// cursor), wall time (the Stop planner's braking law, Q17/Q38) and the
// replacement's own path time, whose zero is where the mechanism is expected to
// start it.
// ---------------------------------------------------------------------------

/// Q26: the active trajectory's path time at which the mechanism is expected to
/// start a replacement. Not wall-clock time since dispatch (wrong under speed
/// scaling): the controller's own cursor from its latest associated sample,
/// advanced at the frozen factor for the local time already spent since that
/// sample was received plus the lane's measured allowance until the mechanism
/// starts the replacement.
struct StopJoinElapsedInputDTO final {
  /// Cursor of the latest sample associated with the active trajectory.
  double sample_cursor_s{0.0};
  /// Frozen speed factor in [0, 1] (Q17/Q35/Q38).
  double frozen_factor{0.0};
  /// Local monotonic time from the sample's receipt to the dispatch decision.
  double wall_since_sample_s{0.0};
  /// Lane-measured wall time from dispatch until the mechanism starts the
  /// replacement (characterization fact; no default exists).
  double start_allowance_s{0.0};
  /// Q22 (`main`'s replacement-acceptance-within-lead-in rule): when present,
  /// the elapsed time must stay strictly before this active-path time (the
  /// end of the lead-in, where the checked ramp starts).
  std::optional<double> lead_in_end_path_s{};
};

/// Returns `sample_cursor_s + frozen_factor * (wall_since_sample_s +
/// start_allowance_s)`. Errors: invalid inputs (`stop_join_elapsed_input_invalid`)
/// or an expected start at or beyond the lead-in end
/// (`stop_delta_budget_exceeded`, recommended disposition hard stop).
[[nodiscard]] Result<double> compute_stop_join_elapsed_s(const StopJoinElapsedInputDTO& input);

/// The curve's shifted origin and optional controller-clock start are chosen
/// together from one associated observation. No clock offset is estimated:
/// local receipt age advances only the duration; the stamp stays on the
/// controller clock. Tick alignment uses the nominal period, not an RT
/// installation guarantee. Unqualified lanes keep the existing elapsed rule.
struct StopReplacementStartDTO final {
  double elapsed_path_s{0.0};
  std::optional<ExecutionReplacementStartDTO> scheduled{};
};

[[nodiscard]] Result<StopReplacementStartDTO> compute_stop_replacement_start(
    const StopJoinElapsedInputDTO& input, const ExecutionControllerSampleDTO& sample,
    const ExecutionLaneFactsDTO& lane);

/// Q17/Q38: express a wall-time braking trajectory (the Stop planner's output,
/// planned at the frozen factor) in path time at that factor, so that a
/// controller applying the factor once reproduces the planned wall-time
/// motion: t -> f t, velocity -> v / f, acceleration -> a / f^2. Every waypoint
/// must carry positions, velocities and accelerations. `frozen_factor` must be
/// in (0, 1].
[[nodiscard]] Result<TrajectoryDTO> convert_stop_trajectory_to_path_time(
    const TrajectoryDTO& wall_time_trajectory, double frozen_factor);

/// §4.2 step 2: the trip point, where the Stop ramp leaves the original path,
/// on the active trajectory's path clock: the continuation's origin advanced
/// at the frozen factor through the wall-time lead-in (`stop_traj_delta_s`).
[[nodiscard]] inline double stop_trip_path_s(double continuation_origin_s, double frozen_factor,
                                             double lead_in_s) {
  return continuation_origin_s + frozen_factor * lead_in_s;
}

/// Q20 (ManyMove's past-point join, as clarified) and §4.2 steps 3-4. All
/// times are shifted by `-elapsed_s`, so the expected start falls at 0 and
/// past points keep negative times (never clamped). `continuation` is in path
/// time with its time 0 at the active trajectory's path time
/// `continuation_origin_s`; `trip_path_s` (>= `continuation_origin_s`) is the
/// active path time where the continuation's ramp leaves the original path.
///
/// - Trip point ahead (`trip_path_s > elapsed_s`, the soft Stop and
///   spring-back): the replacement carries every active waypoint strictly
///   before `trip_path_s` verbatim (past and lead-in alike), then the trip
///   point with the active trajectory's own P/V/A there (the shared sampler's
///   per-segment Hermite interpolation; an active waypoint within 1 ns of the
///   trip point is that point and is not duplicated), then only the
///   continuation's points after the trip point. The continuation's own
///   lead-in resampling is not used, so the controller executes the original
///   path's polynomials through the lead-in. The trip point must not lie past
///   the active trajectory's end.
/// - Trip point already passed (`trip_path_s <= elapsed_s`, the controlled hard
///   stop whose brake starts at the sample): the active waypoints strictly
///   before `elapsed_s`, then the continuation's points at or after time 0
///   (already-passed ones are covered by the past points and dropped).
///
/// Joint sets must match; the continuation is reordered to the active
/// trajectory's joint order. Every waypoint must carry positions, velocities
/// and accelerations. The result is strictly monotonic in time and keeps the
/// active trajectory's joint names and resources. Errors: invalid shape or
/// trip point (`stop_join_input_invalid`), or no continuation point left
/// after the start or the trip point (`stop_join_continuation_elapsed`).
[[nodiscard]] Result<TrajectoryDTO> build_stop_replacement_join(const TrajectoryDTO& active,
                                                                double elapsed_s,
                                                                const TrajectoryDTO& continuation,
                                                                double continuation_origin_s,
                                                                double trip_path_s);

/// Experimental resident-timeline realization of the unchanged checked join,
/// for a controller that replaces its whole timeline at once on the resident
/// clock origin (JTC `replacement_time_reference: active_trajectory`).
/// `resident_trajectory` is the full logical curve on the resident clock:
/// `joined` (time 0 at `elapsed_s` on the active path) shifted by
/// `elapsed_s - source_clock_offset_s`, where source_clock_offset_s maps the
/// native resident cursor to the active trajectory's canonical path time.
/// `delivery_trajectory` is the complete remaining curve sent to the
/// controller: `resident_trajectory` from its last knot at or before
/// `resident_cursor_s` (the actual sample cursor on the resident clock; the
/// first knot when none is), through the connector, into the new motion.
struct ResidentTimelineReplacementDTO final {
  TrajectoryDTO resident_trajectory;
  TrajectoryDTO delivery_trajectory;
};
[[nodiscard]] Result<ResidentTimelineReplacementDTO> build_resident_timeline_replacement(
    const TrajectoryDTO& joined, double elapsed_s, double source_clock_offset_s,
    double resident_cursor_s);

/// The delivery view alone: `resident` (already on the resident clock) from
/// its last knot at or before `cursor_s`, or from its first knot.
[[nodiscard]] TrajectoryDTO resident_timeline_delivery(const TrajectoryDTO& resident,
                                                       double cursor_s);

/// §3.3 cursor mapping of one on-path Stop replacement built by
/// `build_stop_replacement_join`: the controller's cursor on the replacement
/// back to the active (original) trajectory's path time, with the Stop
/// planner's own mapping of each ramp sample. No second integrator.
struct StopReplacementCursorMapDTO final {
  /// The join's `elapsed_s`: the replacement's time 0 on the active path.
  double elapsed_s{0.0};
  /// Below this active-path time the replacement carries the original points.
  double trip_path_s{0.0};
  /// The continuation's time 0 on the active path (`continuation_origin_s`).
  double continuation_origin_s{0.0};
  /// The continuation's waypoint times (path time at the frozen factor) and
  /// the active path time the planner sampled for each (1:1, times rising).
  std::vector<double> continuation_times_s;
  std::vector<double> original_path_s;
};

/// The active path time for the replacement cursor `replacement_cursor_s`:
/// the original's own time before the trip point, then the planner's mapping,
/// linearly interpolated between its samples and held after its last one.
[[nodiscard]] double original_path_cursor(const StopReplacementCursorMapDTO& map,
                                          double replacement_cursor_s);

/// Q42 level 3 (controlled hard stop) input: the controller's latest commanded
/// state and the lane's hard-stop limits. Vectors follow `joint_names`.
struct ControlledHardStopBrakeInputDTO final {
  std::vector<std::string> joint_names;
  /// Latest associated sample's reference position.
  std::vector<double> reference_positions_rad;
  /// Latest associated sample's reference velocity in path-time units
  /// (unscaled); the physical speed is `frozen_factor` times this value.
  std::vector<double> reference_path_velocities;
  /// The factor the controller applies (Q35: its actual applied value), in
  /// (0, 1]. At 0 nothing advances and level 4 applies directly.
  double frozen_factor{0.0};
  /// Physical per-joint deceleration limits: the lane's JTC
  /// `max_deceleration_on_cancel` values (Q2: equal to the Stop profile's
  /// limits). Must cover every joint with a finite positive value.
  std::map<std::string, double> max_deceleration_rad_s2;
  /// Controller update period; the brake is sampled at this period.
  double controller_period_s{0.0};
};

struct ControlledHardStopBrakeDTO final {
  /// Path-time trajectory (time 0 = the sample's state) with exact closed-form
  /// positions, path-unit velocities and accelerations at every controller
  /// period and at the stop time. The final point has zero velocity and keeps
  /// the constant braking acceleration, so the controller's interpolation of
  /// the last segment stays on the same braking law.
  TrajectoryDTO trajectory;
  /// Common physical stop time, set by the slowest-braking joint.
  double stop_time_s{0.0};
  /// `frozen_factor * stop_time_s`: the brake's duration on the path clock.
  double stop_path_time_s{0.0};
  /// The slowest-braking joint; empty when every joint is already at rest.
  std::string limiting_joint;
};

/// Q42 closed-form brake: one common stop time T = max_j(f |v_j| / d_j); every
/// joint decelerates linearly to rest at T along its current direction
/// (physical velocity f v_j (1 - t / T)), so the physical start speed equals
/// the actual speed (no drop) and each joint's physical deceleration
/// f |v_j| / T stays within d_j. Expressed in path time s = f t, so a
/// controller applying f once realizes exactly this motion. All joints at rest
/// yield a single hold point. Errors: `controlled_hard_stop_brake_input_invalid`.
[[nodiscard]] Result<ControlledHardStopBrakeDTO> plan_controlled_hard_stop_brake(
    const ControlledHardStopBrakeInputDTO& input);

}  // namespace manyforge::core
