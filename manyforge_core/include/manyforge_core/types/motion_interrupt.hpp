#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
/// feedback when no prepared per-resource settle threshold is available.
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
  bool jerk_limits_enabled = true;
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
  std::optional<MotionInterruptSoftOnPathPlannerConfigDTO> soft_on_path_planner_config;
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
/// the kernel rejects the execute request if the requested mode or qualified
/// recoverable substitution is outside the policy's `allowed_stop_modes` or
/// the backend's
/// `MotionInterruptStopCapabilityDTO`. At trip time the backend
/// fails closed when live controller behavior diverges.
struct MotionInterruptStopProfileDTO {
  /// Stable audit identifier for this authored inline profile. It is not a
  /// catalog selector or runtime lookup key.
  std::string profile_id;
  MotionInterruptStopMode stop_mode = MotionInterruptStopMode::kSpringBackHold;
  /// Controlled-Stop deceleration time, including the bounded command lead-in
  /// (`stop_traj_delta_s`); the soft ramp lasts the remainder (as main).
  /// Required for `kSoftOnPath`; an unset `kSpringBackHold` value takes
  /// `kDefaultDecelerationTimeS` once, when the profile is loaded. For on-path
  /// spring-back the value is split into equal forward-deceleration (lead-in
  /// included) and reverse-return halves.
  std::optional<double> deceleration_time_s;
  /// Q28/Q51 (`main`'s per-profile override): upper bound on this robot's
  /// controlled Stop, measured from the Stop request. Absent: the deceleration
  /// time (or its default) plus `complete_move_on_signal_timeout` (500 ms).
  std::optional<double> max_stop_time_s;
  /// Typed source-URDF/SRDF-joint acceleration limits. Required with exact
  /// target-joint coverage for soft_on_path and spring_back_hold; forbidden
  /// for hard_deceleration. Kernel Prepare resolves these source names to
  /// exact resource identities without changing their coordinate meaning.
  std::map<std::string, CoordinateAccelerationDTO> per_joint_max_deceleration;
  /// When a soft-on-path or spring-back profile is requested and the remaining accepted
  /// trajectory would finish before a useful wall-time stop ramp
  /// can be accepted, allow the motion to complete naturally
  /// instead of requiring a stop-mode fallback. Soft defaults on; Spring
  /// defaults off to preserve its precision return. With explicit opt-in, short moves finish
  /// normally while the kernel/backend report Stopping until terminal.
  /// Source decoding normalizes omission to true for soft_on_path and false
  /// for the other modes; this DTO stores that normalized value.
  bool allow_natural_completion_on_short_path = false;
  /// Sole qualified nominal-contract substitution. Legal only for
  /// soft_on_path and only as spring_back_hold; it is not a fallback list or
  /// a Kernel escalation choice.
  std::optional<MotionInterruptStopMode> recoverable_substitution_mode;
  /// v1 production profiles MUST use `true`; `false` is reserved for
  /// diagnostic simulators and MUST NOT produce
  /// `terminal_cause=signal_trip`.
  bool require_backend_confirmation = true;
  /// Optional typed source-joint measured-rest overrides. Kernel Prepare may
  /// only tighten matching retained deployment thresholds; it rejects looser,
  /// wrong-unit, unknown, duplicate, or incomplete meaning. A present map MUST
  /// be non-empty, so the normalized DTO preserves authored omission versus an
  /// invalid explicitly empty object.
  std::optional<std::map<std::string, CoordinateVelocityDTO>> per_joint_settle_velocity_threshold;
  /// Bounded JSON-safe profile metadata (controller family / version
  /// / qualification artifact id).
  InterruptStructuredValue diagnostics;
  /// Explicit controlled-attempt requirement, independent of ordinary motion.
  /// Hard containment makes no jerk guarantee. Source decoding requires an
  /// explicit choice; omission must neither activate nor waive a limit.
  /// Absent is an unreviewed declaration or pre-choice history; it cannot be
  /// prepared for a new controlled attempt.
  std::optional<bool> jerk_limits_enabled;
  /// Source-joint overrides before preparation; exact, frozen limits afterward.
  /// Missing enabled values are resolved by the cold profile mapper, never by
  /// changing limits to make a Stop-time candidate feasible.
  std::map<std::string, CoordinateJerkDTO> per_joint_max_jerk;
  friend bool operator==(const MotionInterruptStopProfileDTO&,
                         const MotionInterruptStopProfileDTO&) = default;
};

inline double motion_interrupt_deceleration_time_or_default(
    const MotionInterruptStopProfileDTO& profile) {
  return profile.deceleration_time_s.value_or(kDefaultDecelerationTimeS);
}

/// Existing ManyForge shaping preset, not a manufacturer/equipment limit.
/// The caller supplies validated positive inputs and freezes the result at
/// preparation. Ordinary profiles use a stable configured reference duration.
inline double motion_jerk_preset_limit(double acceleration_limit, double reference_time_s) {
  const double ramp_s = std::min(std::max(0.15 * reference_time_s, 0.02), reference_time_s / 3.0);
  return acceleration_limit / std::max(ramp_s, 1e-9);
}

inline bool motion_interrupt_stop_profile_id_valid(std::string_view profile_id) {
  const auto ascii_alphanumeric = [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (profile_id.empty() || profile_id.size() > 128U || !ascii_alphanumeric(profile_id.front())) {
    return false;
  }
  return std::all_of(profile_id.begin() + 1, profile_id.end(), [&](char character) {
    return ascii_alphanumeric(character) || character == '.' || character == '_' ||
           character == '-';
  });
}

inline std::string motion_interrupt_max_deceleration_time_label() {
  std::ostringstream out;
  out << std::setprecision(15) << kMaxControlledStopDecelerationTimeS;
  std::string label = out.str();
  const bool has_exponent =
      label.find('e') != std::string::npos || label.find('E') != std::string::npos;
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
  return "deceleration_time_s must be <= " + motion_interrupt_max_deceleration_time_label();
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
  if (!motion_interrupt_stop_profile_id_valid(profile.profile_id)) {
    return "profile_id must be a 1-128 byte portable token";
  }
  if (profile.max_stop_time_s.has_value() &&
      (!std::isfinite(*profile.max_stop_time_s) || *profile.max_stop_time_s <= 0.0 ||
       *profile.max_stop_time_s > 9.0e9)) {
    return "stop_profile.max_stop_time_s must be finite and > 0";
  }
  if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath ||
      profile.stop_mode == MotionInterruptStopMode::kSpringBackHold) {
    if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath && !profile.deceleration_time_s) {
      return "soft_on_path requires a finite positive deceleration_time_s";
    }
    if (profile.deceleration_time_s &&
        (!std::isfinite(*profile.deceleration_time_s) || *profile.deceleration_time_s <= 0.0)) {
      return "soft_on_path/spring_back_hold requires a finite positive "
             "deceleration_time_s";
    }
    if (profile.deceleration_time_s &&
        *profile.deceleration_time_s > kMaxControlledStopDecelerationTimeS) {
      return motion_interrupt_deceleration_time_max_error();
    }
    if (profile.per_joint_max_deceleration.empty()) {
      return "soft_on_path/spring_back_hold requires a non-empty "
             "per_joint_max_deceleration";
    }
    for (const auto& [joint, limit] : profile.per_joint_max_deceleration) {
      const double value = std::visit(
          [](const auto& typed) {
            if constexpr (requires { typed.value_rad_s2; }) {
              return typed.value_rad_s2;
            } else {
              return typed.value_m_s2;
            }
          },
          limit);
      if (joint.empty() || !std::isfinite(value) || value <= 0.0) {
        return "soft_on_path/spring_back_hold "
               "per_joint_max_deceleration entries must have non-empty "
               "joint names and finite positive values";
      }
    }
  }
  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration &&
      (!profile.per_joint_max_deceleration.empty() || profile.deceleration_time_s.has_value())) {
    return "hard_deceleration forbids deceleration_time_s and "
           "per_joint_max_deceleration";
  }
  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration &&
      (profile.jerk_limits_enabled == true || !profile.per_joint_max_jerk.empty()))
    return "hard_deceleration forbids jerk shaping";
  for (const auto& [joint, jerk] : profile.per_joint_max_jerk) {
    const double value = std::visit(
        [](const auto& typed) {
          if constexpr (requires { typed.value_rad_s3; })
            return typed.value_rad_s3;
          else
            return typed.value_m_s3;
        },
        jerk);
    const auto acceleration = profile.per_joint_max_deceleration.find(joint);
    if (acceleration == profile.per_joint_max_deceleration.end() ||
        coordinate_kind(jerk) != coordinate_kind(acceleration->second) || !std::isfinite(value) ||
        value <= 0.0) {
      return "per_joint_max_jerk must contain positive limits with matching joint units";
    }
  }
  if (profile.stop_mode == MotionInterruptStopMode::kHardDeceleration &&
      profile.allow_natural_completion_on_short_path) {
    return "natural completion is legal only for soft_on_path or spring_back_hold";
  }
  if (profile.recoverable_substitution_mode.has_value() &&
      (profile.stop_mode != MotionInterruptStopMode::kSoftOnPath ||
       *profile.recoverable_substitution_mode != MotionInterruptStopMode::kSpringBackHold)) {
    return "recoverable_substitution_mode is only soft_on_path to "
           "spring_back_hold";
  }
  if (profile.per_joint_settle_velocity_threshold.has_value()) {
    if (profile.per_joint_settle_velocity_threshold->empty()) {
      return "per_joint_settle_velocity_threshold must be omitted or non-empty";
    }
    for (const auto& [joint, threshold] : *profile.per_joint_settle_velocity_threshold) {
      const double value = std::visit(
          [](const auto& typed) {
            if constexpr (requires { typed.value_rad_s; }) {
              return typed.value_rad_s;
            } else {
              return typed.value_m_s;
            }
          },
          threshold);
      if (joint.empty() || !std::isfinite(value) || value <= 0.0) {
        return "per_joint_settle_velocity_threshold entries must have non-empty "
               "joint names and finite positive values";
      }
    }
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
/// `backend_stop_confirmed=false`, `confirmation_latency_ns` exceeding the
/// Kernel Stop envelope, an unqualified requested/executed mode mismatch, or a
/// precision-contact
/// node category accepting a fallback at all.
struct ControlledStopResultDTO {
  /// `true` only when the backend accepted the controlled-stop
  /// request. `false` plus a populated `error` MUST be used when the
  /// backend cannot execute the requested or any fallback mode.
  bool accepted = false;
  /// `true` only for the explicit short-path policy where
  /// `kSoftOnPath` or explicitly opted-in `kSpringBackHold` was requested,
  /// no useful wall-time replacement ramp
  /// can be accepted before the original trajectory endpoint, and the
  /// stop profile allows natural completion. No controlled stop is
  /// claimed in this case; the kernel records `terminal_cause =
  /// no_signal_trip` after the original motion reaches its endpoint.
  bool completed_by_natural_completion = false;
  /// Mode the backend actually executed. Set even when `accepted=false`
  /// so the audit trail records what the backend tried.
  MotionInterruptStopMode executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
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

/// Validate the bounded, self-consistent result returned by a controlled-stop
/// backend. This is deliberately separate from stop-profile validation: the
/// profile is immutable input, while this function closes the untrusted
/// backend-result boundary before the Kernel uses any result field as stop
/// evidence. It does not authorize a stronger Kernel stage; a mode mismatch is
/// only provenance for the Kernel's already-prepared escalation/substitution
/// rules.
[[nodiscard]] inline std::optional<std::string> controlled_stop_result_field_error(
    const MotionInterruptStopProfileDTO& requested_profile, const ControlledStopResultDTO& result) {
  const auto known_mode = [](MotionInterruptStopMode mode) {
    switch (mode) {
      case MotionInterruptStopMode::kSpringBackHold:
      case MotionInterruptStopMode::kSoftOnPath:
      case MotionInterruptStopMode::kHardDeceleration:
        return true;
    }
    return false;
  };
  const auto known_command_source = [](MotionInterruptStopCommandSource source) {
    switch (source) {
      case MotionInterruptStopCommandSource::kFjtPreemptHold:
      case MotionInterruptStopCommandSource::kFjtPreemptPathRamp:
      case MotionInterruptStopCommandSource::kJtcSpeedScalingRamp:
      case MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel:
      case MotionInterruptStopCommandSource::kJtcCancelUnprofiledHold:
      case MotionInterruptStopCommandSource::kNativeSimulatedStop:
      case MotionInterruptStopCommandSource::kNativeSimulatedPathRamp:
      case MotionInterruptStopCommandSource::kRobotNativeStop:
      case MotionInterruptStopCommandSource::kDiagnosticOther:
        return true;
    }
    return false;
  };
  const auto finite_nonnegative = [](const std::optional<double>& value) {
    return !value.has_value() || (std::isfinite(*value) && *value >= 0.0);
  };

  if (!known_mode(result.executed_stop_mode)) {
    return "executed_stop_mode_invalid";
  }
  if (!known_command_source(result.stop_command_source)) {
    return "stop_command_source_invalid";
  }
  if (result.confirmation_latency_ns < 0) {
    return "confirmation_latency_negative";
  }
  if (!finite_nonnegative(result.stop_distance_rad) ||
      !finite_nonnegative(result.path_deviation_max_rad) ||
      !finite_nonnegative(result.initial_path_deviation_rad)) {
    return "controlled_stop_numeric_evidence_invalid";
  }
  if (const auto diagnostics_error = interrupt_structured_value_error(result.diagnostics)) {
    return "controlled_stop_diagnostics_" + *diagnostics_error;
  }
  if (!result.accepted && !result.error.has_value()) {
    return "rejected_result_missing_error";
  }
  if (!result.accepted && result.backend_stop_confirmed) {
    return "rejected_result_cannot_be_confirmed";
  }
  if (result.error.has_value() && (result.error->code.empty() || result.error->message.empty())) {
    return "controlled_stop_error_invalid";
  }

  const bool mode_differs = result.executed_stop_mode != requested_profile.stop_mode;
  if (mode_differs) {
    if (result.accepted && result.fallback_reason.has_value() &&
        !motion_interrupt_stop_profile_id_valid(*result.fallback_reason)) {
      return "recoverable_substitution_reason_invalid";
    }
  } else if (result.accepted && result.fallback_reason.has_value()) {
    return "recoverable_substitution_reason_without_mode_change";
  }

  if (result.completed_by_natural_completion &&
      (requested_profile.stop_mode == MotionInterruptStopMode::kHardDeceleration ||
       !requested_profile.allow_natural_completion_on_short_path || !result.accepted ||
       !result.backend_stop_confirmed || result.error.has_value() || mode_differs)) {
    return "natural_completion_result_invalid";
  }

  if (result.accepted && result.backend_stop_confirmed && !mode_differs &&
      result.stop_command_source == MotionInterruptStopCommandSource::kDiagnosticOther) {
    const auto carries_stable_reason = [&](std::string_view key) {
      const auto found = result.diagnostics.find(std::string(key));
      if (found == result.diagnostics.end()) {
        return false;
      }
      const auto* text = std::get_if<std::string>(&found->second);
      return text != nullptr && motion_interrupt_stop_profile_id_valid(*text);
    };
    if (!carries_stable_reason("command_source") && !carries_stable_reason("reason")) {
      return "diagnostic_command_source_reason_missing";
    }
  }
  return std::nullopt;
}

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
  /// `kPrecisionContact` matches `ProbeSearchMove`-class nodes and rejects a
  /// recoverable substitution at arm time.
  MotionInterruptNodeCategory node_category = MotionInterruptNodeCategory::kGeneric;
  InterruptStructuredValue metadata;
  friend bool operator==(const MotionInterruptScopeDTO&, const MotionInterruptScopeDTO&) = default;
};

/// Source from which the kernel captured a controlled stop snapshot
/// for a motion-execution terminal outcome (spec/210 §3.11).
enum class ExecutionStopSnapshotSource {
  /// Preferred: backend-reported stopped joint state and time.
  kExecutionBackendFeedback,
  /// Fallback: kernel's most-recent execution snapshot.
  kKernelExecutionSnapshot,
  /// Last resort: observed-state provider sample. Precision-contact node
  /// families MUST reject this source.
  kObservedStateProvider,
};

/// Stop-state observation captured by the kernel after a
/// `complete_move_on_signal` latch (spec/260 §5.6). Distinct from
/// `ExecutionSnapshotDTO`: certifies a *final* stop for one motion
/// outcome, not an in-flight progress sample.
struct ExecutionStopSnapshotDTO {
  std::int64_t captured_at_ns = 0;
  ExecutionStopSnapshotSource source = ExecutionStopSnapshotSource::kExecutionBackendFeedback;
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
  /// within the prepared per-resource settle thresholds. Absent when velocities are
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
  /// `requested_stop_profile.stop_mode` unless this is the explicitly
  /// qualified `soft_on_path` to `spring_back_hold` recoverable substitution.
  /// Default `kSpringBackHold` is a schema placeholder;
  /// real values are populated by the trip-handling path.
  MotionInterruptStopMode executed_stop_mode = MotionInterruptStopMode::kSpringBackHold;
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
  MotionExecutionTerminalCause terminal_cause = MotionExecutionTerminalCause::kCompletedNormally;
  /// Present iff `terminal_cause == kSignalTrip`.
  std::optional<MotionTripOutcomeDTO> trip;
  /// Present iff `terminal_cause == kFailed`.
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core
