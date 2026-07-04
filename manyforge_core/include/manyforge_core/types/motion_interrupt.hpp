#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

/// Default controlled-stop deceleration ramp (seconds) when a stop profile
/// leaves `deceleration_time_s` unset. 1.0 s is the current operator-visible
/// default for live stop-profile validation; the prior 50 ms default was a
/// near-instant stop. Backends MUST resolve an unset `deceleration_time_s`
/// to this value rather than an ad-hoc local literal.
inline constexpr double kDefaultDecelerationTimeS = 1.0;
/// Maximum configured wall-time deceleration ramp for operator/fault soft-stop
/// profiles. Longer stops are an explicit deployment-policy change and must not
/// slip through profile parsing as a "soft stop".
inline constexpr double kMaxControlledStopDecelerationTimeS = 2.0;
/// Default measured-velocity threshold (rad/s) below which a joint is treated
/// as at rest for the settle signal. Used to derive `at_rest` from controller
/// feedback when a profile leaves `settle_velocity_threshold_rad_s` unset.
inline constexpr double kDefaultSettleVelocityThresholdRadS = 0.01;

/// Backend-declared planner parameters for production `soft_on_path`.
/// These are deployment/runtime facts, not behavior-node policy: the
/// kernel uses them for pre-dispatch feasibility envelopes and the
/// backend uses the same values at trip time when generating the
/// replacement trajectory.
struct MotionInterruptSoftOnPathPlannerConfigDTO {
  double stop_traj_delta_s = 0.05;
  double sample_period_s = 0.01;
  std::optional<double> max_initial_path_deviation_rad;
  std::optional<double> max_initial_velocity_deviation_rad_s;
  std::map<std::string, double> per_joint_max_jerk_rad_s3;
  bool allow_model_velocity_fallback = false;
};

/// Controlled-stop capability surface declared by an execution backend
/// (spec/240 backend capability profile; spec/260 §5.6 dual-boundary
/// capability validation). The kernel reads this once at execution
/// time and gates pre-dispatch arming against it. Default empty means
/// the backend has not declared any controlled-stop modes and any
/// `complete_move_on_signal` execute request fails closed at arm time.
struct MotionInterruptStopCapabilityDTO {
  std::set<MotionInterruptStopMode> supported_stop_modes;
  /// Optional opaque profile id / build identifier. Useful for audit:
  /// the outcome record can correlate the supported set with a
  /// specific qualified backend build.
  std::optional<std::string> capability_profile_id;
  /// Present when the backend declares `kSoftOnPath`. If absent, the
  /// kernel uses conservative planner defaults for compatibility with
  /// older test backends; production backends SHOULD populate this so
  /// arm-time feasibility and trip-time execution use the same timing
  /// and drift/jerk limits.
  std::optional<MotionInterruptSoftOnPathPlannerConfigDTO>
      soft_on_path_planner_config;
  /// Hard-stop floor the backend can prove for critical fallback logic. This
  /// is intentionally a string capability rather than a stop-mode enum because
  /// "immediate" / controller-native brake semantics are backend-family facts,
  /// not user-selectable controlled-stop profiles in the current API.
  std::optional<std::string> hard_stop_level;
  /// Level of derivative-continuity proof available for `kSoftOnPath`.
  /// Examples: `none`, `manyforge_core_sampled_path_ramp`,
  /// `manyforge_core_native_path_ramp`.
  std::optional<std::string> derivative_proof_level;
  /// What speed-scaling source the backend can observe/control when executing
  /// a controlled stop, and whether that visibility has been live-probed.
  std::optional<std::string> scaling_source_visibility;
  std::optional<std::string> scaling_source_probe_status;
  /// Stable backend-family reasons for capability omissions. Composer adds the
  /// deployment-profile dimension when it turns these into UI/log payloads.
  std::map<std::string, std::string> unsupported_reasons;
};

/// Controlled-stop profile requested by a motion-scoped interrupt
/// binding (spec/210 §3.11 `MotionInterruptStopProfileDTO`).
///
/// Selected by the motion node (e.g. `MoveUntilSignal` /
/// `ProbeSearchMove`) and executed only by the kernel/backend path.
/// Behavior runtime MUST NOT call backend stop directly. Pre-dispatch
/// the kernel rejects the execute request if the requested mode or
/// any element of `allowed_fallback_modes` is outside the policy's
/// `allowed_stop_modes` or the backend's
/// `MotionInterruptStopCapabilityDTO`. At trip time the backend
/// fails closed when live controller behavior diverges.
struct MotionInterruptStopProfileDTO {
  /// Stable profile identifier for trace/audit. May reference a
  /// deployment-defined profile or an inline motion-node profile.
  std::string profile_id;
  MotionInterruptStopMode stop_mode = MotionInterruptStopMode::kSpringBackHold;
  /// Requested ramp duration for `kSpringBackHold` and `kSoftOnPath`.
  /// Required for `kSoftOnPath`; deployments MAY provide defaults for
  /// `kSpringBackHold`. For on-path spring-back the value is split into equal
  /// forward-deceleration and reverse-return halves.
  std::optional<double> deceleration_time_s;
  /// Upper bound on backend stop confirmation. If absent, the effective
  /// deadline is `deceleration_time_s` (or its profile default) plus the
  /// deployment stop-confirmation tolerance.
  std::optional<double> max_stop_time_s;
  /// Deployment-defined joint-limit / controller profile used by
  /// `kHardDeceleration`.
  std::optional<std::string> joint_deceleration_profile_id;
  /// Inline per-axis deceleration limit override. Industrial
  /// deployments SHOULD prefer immutable deployment profiles; inline
  /// values are for test and explicitly versioned recipes.
  std::map<std::string, double> per_joint_max_deceleration_rad_s2;
  /// Stop modes that MAY be used if the requested mode is unsupported
  /// at trip time. Empty (default) means fail closed. Any fallback
  /// MUST be recorded in the terminal outcome. The conjunctive rule
  /// (spec/210 §3.11) gates trip-time acceptance.
  std::vector<MotionInterruptStopMode> allowed_fallback_modes;
  /// When `kSoftOnPath` is requested and the remaining accepted
  /// trajectory would finish before a useful wall-time stop ramp
  /// can be accepted, allow the motion to complete naturally
  /// instead of requiring a stop-mode fallback. Default true keeps
  /// operator-stop semantics resumable: short remaining moves finish
  /// normally while the kernel/backend report Stopping until terminal.
  /// Precision-contact lanes that require fallback instead of natural
  /// completion must opt out explicitly.
  bool allow_natural_completion_on_short_path = true;
  /// v1 production profiles MUST use `true`; `false` is reserved for
  /// diagnostic simulators and MUST NOT produce
  /// `terminal_cause=signal_trip`.
  bool require_backend_confirmation = true;
  /// Measured-velocity threshold (rad/s) for the at-rest settle verdict. When
  /// unset the kernel uses `kDefaultSettleVelocityThresholdRadS`. The monitor
  /// uses this to confirm a stop reached measured rest within the deadline.
  std::optional<double> settle_velocity_threshold_rad_s;
  /// Bounded JSON-safe profile metadata (controller family / version
  /// / qualification artifact id).
  InterruptStructuredValue diagnostics;
};

inline double motion_interrupt_deceleration_time_or_default(
    const MotionInterruptStopProfileDTO& profile) {
  return profile.deceleration_time_s.value_or(kDefaultDecelerationTimeS);
}

inline std::string motion_interrupt_max_deceleration_time_label() {
  std::ostringstream out;
  out << std::setprecision(15) << kMaxControlledStopDecelerationTimeS;
  std::string label = out.str();
  const bool has_exponent =
      label.find('e') != std::string::npos ||
      label.find('E') != std::string::npos;
  const auto dot = label.find('.');
  if (dot == std::string::npos && !has_exponent) {
    label += ".0";
  } else if (dot != std::string::npos) {
    while (label.size() > dot + 2 && label.back() == '0') {
      label.pop_back();
    }
  }
  return label;
}

inline std::string motion_interrupt_deceleration_time_max_error() {
  return "deceleration_time_s must be <= " +
         motion_interrupt_max_deceleration_time_label();
}

/// Validate the mode-specific REQUIRED fields of a stop profile at the
/// config-parse / load boundary, so a malformed profile fails to load rather
/// than only failing the moment an operator presses Stop. Returns an empty
/// string when valid, else a human-readable reason. `kSoftOnPath` requires a
/// finite positive `deceleration_time_s` and a non-empty per-joint
/// deceleration map whose entries are named and finite-positive. (Coverage of
/// every bound robot joint is checked by the load gate / planner, which hold
/// the joint list; the planner re-checks these same fields at trip time as
/// defense in depth.)
inline std::string motion_interrupt_stop_profile_field_error(
    const MotionInterruptStopProfileDTO& profile) {
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath ||
      profile.stop_mode == MotionInterruptStopMode::kSpringBackHold) {
    if (!profile.deceleration_time_s ||
        !std::isfinite(*profile.deceleration_time_s) ||
        *profile.deceleration_time_s <= 0.0) {
      return "soft_on_path/spring_back_hold requires a finite positive "
             "deceleration_time_s";
    }
    if (*profile.deceleration_time_s >
        kMaxControlledStopDecelerationTimeS) {
      return motion_interrupt_deceleration_time_max_error();
    }
    if (profile.per_joint_max_deceleration_rad_s2.empty()) {
      return "soft_on_path/spring_back_hold requires a non-empty "
             "per_joint_max_deceleration_rad_s2";
    }
    for (const auto& [joint, limit] :
         profile.per_joint_max_deceleration_rad_s2) {
      if (joint.empty() || !std::isfinite(limit) || limit <= 0.0) {
        return "soft_on_path/spring_back_hold "
               "per_joint_max_deceleration_rad_s2 entries must have non-empty "
               "joint names and finite positive values";
      }
    }
  }
  if (profile.stop_mode != MotionInterruptStopMode::kSoftOnPath &&
      profile.stop_mode != MotionInterruptStopMode::kSpringBackHold &&
      profile.deceleration_time_s.has_value()) {
    if (!std::isfinite(*profile.deceleration_time_s) ||
        *profile.deceleration_time_s <= 0.0) {
      return "deceleration_time_s must be finite and positive";
    }
    if (*profile.deceleration_time_s >
        kMaxControlledStopDecelerationTimeS) {
      return motion_interrupt_deceleration_time_max_error();
    }
  }
  if (profile.settle_velocity_threshold_rad_s.has_value() &&
      (!std::isfinite(*profile.settle_velocity_threshold_rad_s) ||
       *profile.settle_velocity_threshold_rad_s <= 0.0)) {
    return "settle_velocity_threshold_rad_s must be finite and positive";
  }
  return "";
}

/// Result of a controlled-stop request issued to the execution backend
/// at trip time (spec/260 §5.6). The backend MUST set
/// `accepted=false` and a populated `error` when it cannot execute any
/// controlled stop (including the trivial native-simulated path);
/// otherwise it reports the mode it actually executed, which surface
/// issued the stop, whether the controller confirmed, and the timing
/// fields the kernel later embeds in `MotionTripOutcomeDTO`.
///
/// The kernel applies the conjunctive fallback rule (spec/210 §3.11)
/// to this result before declaring `signal_trip` success. Any of the
/// following degrades the outcome to `failed`: `accepted=false`,
/// `backend_stop_confirmed=false`, `confirmation_latency_ns` exceeding
/// `max_stop_time_s`, `executed_stop_mode` outside `allowed_fallback_modes`
/// when it differs from the requested mode, or a precision-contact
/// node category accepting a fallback at all.
struct ControlledStopResultDTO {
  /// `true` only when the backend accepted the controlled-stop
  /// request. `false` plus a populated `error` MUST be used when the
  /// backend cannot execute the requested or any fallback mode.
  bool accepted = false;
  /// `true` only for the explicit short-path policy where
  /// `kSoftOnPath` was requested, no useful wall-time replacement ramp
  /// can be accepted before the original trajectory endpoint, and the
  /// stop profile allows natural completion. No controlled stop is
  /// claimed in this case; the kernel records `terminal_cause =
  /// no_signal_trip` after the original motion reaches its endpoint.
  bool completed_by_natural_completion = false;
  /// Mode the backend actually executed. Set even when `accepted=false`
  /// so the audit trail records what the backend tried.
  MotionInterruptStopMode executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  /// Surface that issued the stop. `kDiagnosticOther` MUST carry a
  /// stable backend reason in `diagnostics`.
  MotionInterruptStopCommandSource stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  /// `true` only when the backend's controller acknowledged the
  /// controlled stop within the deadline. Required for a successful
  /// `signal_trip` outcome (spec/210 §3.11).
  bool backend_stop_confirmed = false;
  /// Time from `execute_controlled_stop` call to backend
  /// acknowledgement. Echoed into `MotionTripOutcomeDTO.latch_to_stop_confirmed_ns`
  /// by the kernel for end-to-end timing audit.
  std::int64_t confirmation_latency_ns = 0;
  /// Optional joint-space stop distance from the trip sample to the
  /// confirmed stop state. REQUIRED for `kHardDeceleration` runtime
  /// evidence; OPTIONAL for `kSpringBackHold` / `kSoftOnPath`.
  std::optional<double> stop_distance_rad;
  /// Optional maximum deviation from the accepted path during the
  /// stop. REQUIRED for `kSoftOnPath` production qualification.
  std::optional<double> path_deviation_max_rad;
  /// Optional deviation at the trip/handoff anchor before the
  /// path-ramp projection. Recorded separately so
  /// best-effort path preservation does not hide initial tracking
  /// error.
  std::optional<double> initial_path_deviation_rad;
  /// Optional stable reason explaining why the executed mode differs
  /// from the requested mode. REQUIRED when `executed_stop_mode !=
  /// requested.stop_mode`.
  std::optional<std::string> fallback_reason;
  /// Populated when `accepted=false`. `error.code` SHOULD be one of
  /// `motion_interrupt_backend_stop_failed` or
  /// `motion_interrupt_stop_state_invalid`; the kernel relays this
  /// directly into the terminal outcome `error`.
  std::optional<ErrorDTO> error;
  /// Bounded JSON-safe diagnostics. MUST carry a stable
  /// `command_source` reason when `stop_command_source ==
  /// kDiagnosticOther` (spec/210 §3.11).
  InterruptStructuredValue diagnostics;
};

/// Per-move binding that arms a `kMove`-scoped interrupt policy for
/// exactly one `ExecuteRequestDTO` (spec/260 §2.6).
///
/// Carried on `ExecuteRequestDTO.motion_interrupt_scopes`, so arming is
/// atomic with dispatch — the behavior runtime never owns a separate
/// enable/disable lease. Arming happens after planning and validation
/// pass and immediately before backend dispatch; reaping happens on
/// any terminal execution state.
struct MotionInterruptScopeDTO {
  /// Unique within the active execute request.
  std::string scope_id;
  /// References a registered `InterruptPolicyDefinition` whose
  /// `scope_kind == kMove`.
  std::string policy_id;
  MotionInterruptActivationMode activation_mode =
      MotionInterruptActivationMode::kRequireInactiveAtArm;
  /// Maximum time the kernel may wait for an initial source baseline.
  /// Falls back to the deployment default
  /// (`interrupts.config.motion_scope_arm_timeout_ns`) when unset.
  std::optional<std::int64_t> arm_timeout_ns;
  /// Opaque BT blackboard key the behavior runtime associates with the
  /// resulting `MotionTripOutcomeDTO`. The kernel does not interpret
  /// the key but persists it; format rule (spec/260 §2.6): UTF-8,
  /// 1–128 bytes, no NUL, matches `[A-Za-z_][A-Za-z0-9_.:-]*`.
  std::optional<std::string> result_binding_key;
  /// v1 MUST be `true`; present so later advisory scopes can downgrade
  /// without a schema break.
  bool required = true;
  /// Controlled-stop profile (spec/260 §5.6 + spec/210 §3.11)
  /// selected by the motion node. REQUIRED when the bound
  /// policy's `preemption_action == kCompleteMoveOnSignal`; missing
  /// here is an arm-time rejection. Free-move / non-trip scopes (none
  /// in v1) MAY omit.
  std::optional<MotionInterruptStopProfileDTO> stop_profile;
  /// Motion-node category metadata. Drives default fallback rules
  /// at arm time (spec/210 §3.11). Default
  /// `kGeneric` matches `MoveUntilSignal`-class nodes;
  /// `kPrecisionContact` matches `ProbeSearchMove`-class nodes and
  /// rejects any non-empty `allowed_fallback_modes` at arm time.
  MotionInterruptNodeCategory node_category =
      MotionInterruptNodeCategory::kGeneric;
  InterruptStructuredValue metadata;
};

/// Source from which the kernel captured a controlled stop snapshot
/// for a motion-execution terminal outcome (spec/210 §3.11).
enum class ExecutionStopSnapshotSource {
  /// Preferred: backend-reported stopped joint state and time.
  kExecutionBackendFeedback,
  /// Fallback: kernel's most-recent execution snapshot.
  kKernelExecutionSnapshot,
  /// Last resort: planning-context state. Precision-contact node
  /// families MUST reject this source.
  kPlanningContextState,
};

/// Stop-state observation captured by the kernel after a
/// `complete_move_on_signal` latch (spec/260 §5.6). Distinct from
/// `ExecutionSnapshotDTO`: certifies a *final* stop for one motion
/// outcome, not an in-flight progress sample.
struct ExecutionStopSnapshotDTO {
  std::int64_t captured_at_ns = 0;
  ExecutionStopSnapshotSource source =
      ExecutionStopSnapshotSource::kExecutionBackendFeedback;
  RobotStateDTO robot_state;
  std::optional<Pose3DTO> tcp_pose;
  std::optional<double> trajectory_time_from_start_s;
  std::optional<double> trajectory_progress;
  /// MUST be `true` for a successful `signal_trip` outcome.
  bool backend_stop_confirmed = false;
  /// Largest measured |joint velocity| (rad/s) at the captured stop state.
  /// Absent when the backend exposes no measured velocities (no settle
  /// evidence); present values are the audit/monitor settle signal.
  std::optional<double> max_abs_velocity_rad_s;
  /// Measured settle verdict: `true` when every measured joint velocity is
  /// within `settle_velocity_threshold_rad_s`. Absent when velocities are
  /// unavailable (settle UNKNOWN — consumers MUST NOT treat as at-rest).
  std::optional<bool> at_rest;
  InterruptStructuredValue diagnostics;
};

/// Records exactly one winning desired-trip latch under a move-scoped
/// interrupt binding (spec/210 §3.11). Embedded inside
/// `MotionExecutionOutcomeDTO` only when
/// `terminal_cause == kSignalTrip`. The DTO requires a validated stop
/// snapshot, so it is never present on failure outcomes.
struct MotionTripOutcomeDTO {
  std::string latch_id;
  std::string source_id;
  std::string policy_id;
  std::string scope_id;
  InterruptSignalUpdate signal_snapshot;
  ExecutionStopSnapshotDTO stop_snapshot;
  /// Exact profile carried by the winning scope. The backend MUST
  /// report both requested and executed stop modes; a
  /// successful `signal_trip` outcome is invalid if the executed mode
  /// is unknown (spec/210 §3.11).
  MotionInterruptStopProfileDTO requested_stop_profile;
  /// Mode the backend actually executed. MUST match
  /// `requested_stop_profile.stop_mode` unless the profile explicitly
  /// listed this mode in `allowed_fallback_modes` AND every other
  /// condition of the conjunctive fallback rule (spec/210 §3.11) is
  /// satisfied. Default `kSpringBackHold` is a schema placeholder;
  /// real values are populated by the trip-handling path.
  MotionInterruptStopMode executed_stop_mode =
      MotionInterruptStopMode::kSpringBackHold;
  /// Which backend/controller surface issued the stop.
  /// `kDiagnosticOther` MUST include a stable backend reason in
  /// `diagnostics`.
  MotionInterruptStopCommandSource stop_command_source =
      MotionInterruptStopCommandSource::kDiagnosticOther;
  std::int64_t signal_to_latch_ns = 0;
  std::int64_t latch_to_backend_stop_call_ns = 0;
  std::int64_t latch_to_stop_confirmed_ns = 0;
  std::optional<CollisionWorldRevision> collision_world_revision;
  std::optional<std::string> collision_policy_revision;
  /// Joint-space stop distance from trip sample to confirmed stop
  /// state. REQUIRED for `kHardDeceleration` runtime evidence;
  /// OPTIONAL for `kSpringBackHold` / `kSoftOnPath`.
  std::optional<double> stop_distance_rad;
  /// Maximum deviation from the accepted path during the stop.
  /// REQUIRED for `kSoftOnPath` production qualification;
  /// MAY be absent at runtime only when the backend records an
  /// explicit diagnostic reason.
  std::optional<double> path_deviation_max_rad;
  /// Deviation at the stop-ramp anchor before projection onto the
  /// accepted path. Required for release-ready `kSoftOnPath`
  /// evidence and optional for other modes.
  std::optional<double> initial_path_deviation_rad;
  /// Stable reason explaining why the executed mode differs from
  /// the requested mode. REQUIRED when
  /// `executed_stop_mode != requested_stop_profile.stop_mode`.
  std::optional<std::string> fallback_reason;
  /// The originating scope's
  /// `MotionInterruptScopeDTO.result_binding_key` (spec/260 §2.6),
  /// echoed into the trip outcome so BT/runtime consumers can
  /// correlate the outcome with the blackboard slot that owns its
  /// downstream result. Absent when the scope did not set the key.
  std::optional<std::string> result_binding_key;
  InterruptStructuredValue diagnostics;
};

/// Terminal-cause taxonomy for a motion execution (spec/210 §3.11).
enum class MotionExecutionTerminalCause {
  /// Trajectory reached planned endpoint; move carried no scopes.
  kCompletedNormally,
  /// `complete_move_on_signal` latched, backend confirmed stop, and a
  /// valid stop snapshot was captured. Requires `trip` and forbids
  /// `error`.
  kSignalTrip,
  /// Trajectory reached planned endpoint without any scoped policy
  /// latching. Valid only when the originating execute request carried
  /// at least one `MotionInterruptScopeDTO`.
  kNoSignalTrip,
  /// Operator or BT-driven `Stop()` reached the kernel before any trip
  /// latched; later scoped signal updates are ignored because the
  /// scope is reaped.
  kStopped,
  /// Any failure mode (backend stop failed, stop-state invalid, source
  /// stale mid-move). Requires `error` and forbids `trip`.
  kFailed,
  /// A higher-priority safety / fault session policy latched over the
  /// move; the desired-trip scope was reaped. Forbids both `trip` and
  /// `error`.
  kInterruptedFault,
};

/// Immutable terminal record of one motion execution (spec/210 §3.11).
/// Stored in the kernel's terminal-outcome surface; queryable via
/// `get_execution_outcome(execution_id)`. Emitted exactly
/// once per terminal execution; never mutated after publication.
struct MotionExecutionOutcomeDTO {
  std::uint64_t execution_id = 0;
  std::string move_id;
  std::optional<ActionSpanDTO> action_span;
  std::string contact_policy_id;
  MotionExecutionTerminalCause terminal_cause =
      MotionExecutionTerminalCause::kCompletedNormally;
  /// Present iff `terminal_cause == kSignalTrip`.
  std::optional<MotionTripOutcomeDTO> trip;
  /// Present iff `terminal_cause == kFailed`.
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core
