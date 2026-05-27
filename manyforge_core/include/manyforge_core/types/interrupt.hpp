#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/execution_status.hpp>

namespace manyforge::core {

enum class InterruptAdapter {
  kKernel,
  kRos,
  kPlc,
  kComposerUi,
  kPerception,
  kSimulator,
  kAssistant,
  kInternalTest,
};

enum class InterruptSourceClass {
  kFastExecutionRelevant,
  kLowRateSupervisory,
};

enum class InterruptSourceKind {
  kEdge,
  kLevel,
};

enum class InterruptPreemptionAction {
  kCancelPlanning,
  kCancelValidation,
  kPreemptTrajectory,
  kPreemptStreaming,
  kLatchHeld,
  kLatchFaulted,
  /// Move-scoped non-fault completion (spec/260 §5.6). Reserved for
  /// `InterruptPolicyDefinition::scope_kind == kMove` policies. Stops
  /// trajectory execution on a desired signal trip and produces a
  /// `MotionExecutionOutcomeDTO` with `terminal_cause = signal_trip`
  /// when (and only when) the backend confirms stop and a valid
  /// `ExecutionStopSnapshotDTO` is captured.
  kCompleteMoveOnSignal,
};

enum class InterruptPostLatchDisposition {
  kAwaitOperatorAck,
  kAwaitRecoveryHandler,
  kAutoClearOnSignalRelease,
  /// Latch is cleared only after the matching `MotionExecutionOutcomeDTO`
  /// has been durably emitted to the operational event stream and
  /// stored in the kernel's terminal-outcome surface. Only valid for
  /// `scope_kind=move` policies whose `preemption_action ==
  /// kCompleteMoveOnSignal`.
  kAutoClearAfterMotionOutcome,
  kFaultTerminal,
};

/// Activation modes for move-scoped policy bindings (spec/260 §2.6).
enum class MotionInterruptActivationMode {
  /// Default for touch probes: kernel samples the source at arm time;
  /// if active or unknown, the move is rejected before dispatch.
  kRequireInactiveAtArm,
  /// Records baseline at arm time; only a post-arm transition latches.
  kFreshEdgeAfterArm,
  /// Latches immediately when already active; must be explicitly allowed
  /// in the policy's `allowed_activation_modes`.
  kTripIfAlreadyActive,
  /// Reserved for surface following and compliant maintenance; v1
  /// adapters return `diagnostic_incomplete` if they cannot support it.
  kMaintainLevelWithHysteresis,
};

/// Controlled-stop mode requested by a move-scoped interrupt scope
/// (spec/260 §5.6 Stop-profile semantics; spec/210 §3.11
/// `MotionInterruptStopProfileDTO`).
///
/// `kSpringBackHold` is the simplest controlled halt: observed-state
/// anchor + zero-velocity hold target. `kSoftOnPath` is a
/// path-preserving ramp along the accepted trajectory.
/// `kHardDeceleration` is an acceleration-limited stop that may leave
/// the path. None of these is a safety e-stop; e-stop / protective
/// stops remain higher-priority session/fault policies and produce
/// `interrupted_fault` or `failed`, never `signal_trip`.
enum class MotionInterruptStopMode {
  kSpringBackHold,
  kSoftOnPath,
  kHardDeceleration,
};

/// Which backend/controller surface actually issued the controlled
/// stop (spec/210 §3.11 `MotionTripOutcomeDTO.stop_command_source`).
/// `kDiagnosticOther` is reserved for backends whose stop path does
/// not match a named taxonomy and MUST carry a stable reason in the
/// outcome's `diagnostics`.
enum class MotionInterruptStopCommandSource {
  kFjtPreemptHold,
  kFjtPreemptPathRamp,
  kJtcSpeedScalingRamp,
  kJtcCancelDecelerateOnCancel,
  kNativeSimulatedStop,
  kNativeSimulatedPathRamp,
  kRobotNativeStop,
  kDiagnosticOther,
};

/// Motion-node category metadata carried on a `MotionInterruptScopeDTO`.
/// The kernel uses the category to enforce default fallback rules at
/// arm time (spec/210 §3.11 conjunctive fallback rule; motion-scoped
/// design doc §B.4 node-category defaults).
enum class MotionInterruptNodeCategory {
  /// `MoveUntilSignal` and other generic search nodes. May accept any
  /// fallback the backend capability profile and policy allow.
  kGeneric,
  /// `ProbeSearchMove` and other precision-contact node families.
  /// Defaults to no fallback — the precision contract cannot tolerate
  /// silent substitution of a different stop mode at trip time.
  kPrecisionContact,
};

/// Whether a policy is always active (`kSession`) or only when armed by
/// a `MotionInterruptScopeDTO` on the active `ExecuteRequestDTO`
/// (`kMove`). Spec/260 §2.2.
enum class InterruptPolicyScopeKind {
  kSession,
  kMove,
};

/// When the kernel auto-reaps a move-scoped policy binding.
/// Spec/260 §2.2.
enum class InterruptPolicyScopeExpiry {
  kExecutionTerminal,
  kStreamingSessionTerminal,
};

enum class InterruptExecutionPhase {
  kPlanning,
  kValidation,
  kTrajectoryExecution,
  kStreaming,
  kPaused,
  kHeld,
  kRecovery,
};

enum class InterruptFaultKind {
  kStale,
  kDisconnected,
  kIngestOverflow,
  kSourceClassViolation,
};

enum class InterruptPreemptionResult {
  kPending,
  kSucceeded,
  kPartial,
  kBackendPreemptFailed,
  kNoneRequired,
};

enum class InterruptTieBreak {
  kLexicographic,
};

enum class InterruptDispositionAction {
  kAcknowledge,
  kResume,
  kRestartSubtree,
  kRestartRun,
  kReset,
  kHold,
  kFaultTerminal,
  kDelegateToRecovery,
};

using InterruptStructuredValue = std::map<std::string, std::string>;
using InterruptSignalValue =
    std::variant<bool, double, InterruptStructuredValue>;

struct InterruptSourceDefinition {
  std::string source_id;
  InterruptAdapter adapter = InterruptAdapter::kKernel;
  std::string signal;
  InterruptSourceClass source_class =
      InterruptSourceClass::kFastExecutionRelevant;
  InterruptSourceKind kind = InterruptSourceKind::kEdge;
  int priority = 0;
  std::optional<bool> maskable;
  std::optional<std::int64_t> freshness_budget_ns;
  std::optional<std::int64_t> debounce_ns;
  std::optional<InterruptSignalValue> hysteresis_value;
  std::optional<std::string> description;
};

// Forward declaration of `MotionInterruptStopProfileDTO` so
// `InterruptPolicyDefinition` (below) can carry an optional
// `stop_profile` field without creating a circular include with
// `motion_interrupt.hpp`. The shared_ptr storage in the field below
// is what makes the incomplete type usable.
struct MotionInterruptStopProfileDTO;

struct InterruptPolicyDefinition {
  std::string policy_id;
  std::string source_id;
  std::optional<int> priority;
  std::optional<bool> maskable;
  std::set<InterruptExecutionPhase> affected_phases;
  InterruptPreemptionAction preemption_action =
      InterruptPreemptionAction::kLatchHeld;
  std::vector<std::string> maskable_by;
  InterruptPostLatchDisposition post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  /// Spec/260 §2.2. `kSession` (default) keeps legacy behavior.
  /// `kMove` policies are inactive until armed by a
  /// `MotionInterruptScopeDTO` on the active `ExecuteRequestDTO`.
  InterruptPolicyScopeKind scope_kind = InterruptPolicyScopeKind::kSession;
  /// Applies only when `scope_kind == kMove`. Default
  /// `kExecutionTerminal` reaps the binding on terminal execution
  /// state.
  InterruptPolicyScopeExpiry scope_expiry =
      InterruptPolicyScopeExpiry::kExecutionTerminal;
  /// Applies only when `scope_kind == kMove`. Empty set means the
  /// kernel default (only `kRequireInactiveAtArm` accepted).
  std::set<MotionInterruptActivationMode> allowed_activation_modes;
  /// Spec/260 §2.2: deployment policy gate listing the
  /// stop modes a move-scoped scope MAY request via its
  /// `MotionInterruptStopProfileDTO.stop_mode`. REQUIRED (non-empty)
  /// when `preemption_action == kCompleteMoveOnSignal`; rejected at
  /// registration otherwise. Distinct from the backend's
  /// `MotionInterruptStopCapabilityDTO.supported_stop_modes`:
  /// arm-time validation enforces both (the requested mode and every
  /// element of `allowed_fallback_modes` MUST be in BOTH sets).
  std::set<MotionInterruptStopMode> allowed_stop_modes;
  /// Fault-stop profile policy (spec/260 §2.2). Applies only when
  /// `preemption_action == kPreemptTrajectory`; the policy
  /// registration is rejected if this field is set for any other
  /// action. When present, a latch on this policy routes the preempt
  /// through `execution_backend_.execute_controlled_stop(*stop_profile)`
  /// instead of the legacy `execution_backend_.stop()`. This extends
  /// the controlled-stop machinery (originally defined only for
  /// `kCompleteMoveOnSignal` desired-trip policies) to fault-session
  /// preempts so the same `ControlledStopResultDTO` surface validates
  /// end-to-end on the fault path. Unset means legacy preempt.
  ///
  /// Stored via `std::shared_ptr` because the full
  /// `MotionInterruptStopProfileDTO` definition lives in
  /// `motion_interrupt.hpp` which itself includes this header — using
  /// `std::optional<MotionInterruptStopProfileDTO>` would force a
  /// circular include. The shared_ptr works with the forward
  /// declaration immediately above; the kernel constructs the
  /// concrete value via `std::make_shared` at registration.
  std::shared_ptr<MotionInterruptStopProfileDTO> stop_profile;
  std::optional<std::string> description;
};

struct InterruptSignalUpdate {
  std::string source_id;
  std::int64_t sequence_id = 0;
  std::int64_t timestamp_ns = 0;
  InterruptSignalValue value = false;
  std::optional<std::int64_t> freshness_ns;
  std::optional<double> confidence;
  std::optional<InterruptStructuredValue> covariance;
  std::optional<InterruptFaultKind> fault_kind;
  InterruptStructuredValue metadata;
};

struct InterruptDisposition {
  std::string latch_id;
  InterruptDispositionAction action = InterruptDispositionAction::kAcknowledge;
  std::string principal_id;
  std::optional<std::string> reason;
  std::optional<std::string> recovery_handler_id;
};

struct InterruptLatch {
  std::string latch_id;
  std::string source_id;
  std::string policy_id;
  std::int64_t latched_at_ns = 0;
  InterruptExecutionPhase execution_phase_at_latch =
      InterruptExecutionPhase::kPlanning;
  std::optional<std::uint64_t> active_execution_id;
  std::optional<std::string> active_move_id;
  std::optional<std::uint64_t> active_streaming_session_id;
  InterruptSignalUpdate signal_snapshot;
  InterruptPreemptionAction preemption_action_requested =
      InterruptPreemptionAction::kLatchHeld;
  std::optional<CollisionWorldRevision> borrowed_collision_world_revision;
  std::optional<InterruptTieBreak> tie_break;
  InterruptPreemptionResult preemption_result =
      InterruptPreemptionResult::kPending;
  std::optional<std::int64_t> preemption_latency_ns;
  std::optional<InterruptDisposition> disposition;
  std::optional<std::int64_t> cleared_at_ns;
  std::optional<std::string> clear_reason;
  /// Populated when this latch matched a `kMove`-scoped policy binding
  /// (spec/260 §2.4 / §2.6). Carries the `scope_id` from the active
  /// `ExecuteRequestDTO.motion_interrupt_scopes` entry that armed the
  /// binding.
  std::optional<std::string> motion_interrupt_scope_id;
  /// Populated after the kernel has captured a valid stop snapshot and
  /// emitted the matching `MotionExecutionOutcomeDTO`. The latch never
  /// carries `RobotStateDTO` in v1; this id links the latch to the
  /// outcome record stored in the kernel terminal-outcome surface.
  std::optional<std::string> terminal_outcome_id;
};

inline const char* to_string(InterruptExecutionPhase phase) {
  switch (phase) {
    using enum InterruptExecutionPhase;
    case kPlanning: return "planning";
    case kValidation: return "validation";
    case kTrajectoryExecution: return "trajectory_execution";
    case kStreaming: return "streaming";
    case kPaused: return "paused";
    case kHeld: return "held";
    case kRecovery: return "recovery";
  }
  return "unknown";
}

inline std::optional<InterruptExecutionPhase> interrupt_phase_from_state(
    ExecutionState state) {
  switch (state) {
    using enum ExecutionState;
    case kPlanning: return InterruptExecutionPhase::kPlanning;
    case kValidating: return InterruptExecutionPhase::kValidation;
    case kExecuting: return InterruptExecutionPhase::kTrajectoryExecution;
    case kStreaming: return InterruptExecutionPhase::kStreaming;
    case kPaused: return InterruptExecutionPhase::kPaused;
    case kInterrupted: return InterruptExecutionPhase::kHeld;
    case kInterventionRequired: return InterruptExecutionPhase::kHeld;
    default: return std::nullopt;
  }
}

}  // namespace manyforge::core
