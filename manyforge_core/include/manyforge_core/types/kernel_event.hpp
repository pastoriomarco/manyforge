#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <map>
#include <string>

#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Kernel event types (spec §6.1)
enum class KernelEventType {
  kStateTransition,  // state machine transition
  kPlanStarted,      // planning phase started
  kPlanCompleted,    // planning phase completed
  kCacheHit,         // trajectory found in cache
  kCacheMiss,        // cache lookup returned empty
  kCacheRevalidationCandidate,
  kCacheRevalidated,
  kCacheRevalidationFailed,
  kCacheInvalidated, // cached trajectory invalidated
  kValidationStarted,  // validation phase started
  kValidationPassed,   // validation passed
  kValidationFailed,   // validation failed
  kParticipantPrepared,  // immutable participant prepared; no effect dispatched
  kExecuteStarted,     // execution phase started
  kExecuteCompleted,   // execution phase completed
  kStopRequested,      // stop requested
  kStopIssued,         // stop issued to backend
  kRuntimeVelocityScaleChanged,  // runtime velocity scale changed
  kCompletedAcknowledged,  // completed terminal outcome acknowledged
  kFailedValidationAcknowledged,  // retryable validation failure acknowledged
  // Intervention acknowledgement. Emitted when an explicit operator/runtime
  // call clears `kInterventionRequired`. `details` carries `principal_id`
  // and `reason`. `execute()` MUST NOT clear the state implicitly — the
  // boundary is load-bearing for the operator-intervention audit trail.
  kInterventionAcknowledged,
  kErrorRaised,        // error occurred
  kCollisionDetected,  // collision detected during execution
  // Kernel-native interrupt events (spec/260 §7). Details carry the required
  // event payload in string-normalized form.
  kInterruptSignalIngested,
  kInterruptSignalRejectedStale,
  kInterruptSignalRejectedClassViolation,
  kInterruptSignalRejected,
  kInterruptLatched,
  kInterruptSuppressedByActiveMask,
  kInterruptQueued,
  kInterruptDroppedQueueOverflow,
  kInterruptPreemptApplied,
  kInterruptDispositionApplied,
  kInterruptCleared,
  kInterruptIngestOverflow,
  kInterruptSourceClassViolation,
  kInterruptRecoveryHandlerStarted,
  kInterruptRecoveryHandlerCompleted,
  // Motion-scoped interrupt outcomes (spec/260 §7).
  // `motion_interrupt_scope_armed` / `motion_interrupt_scope_reaped`
  // surface the binding lifecycle. `motion_interrupt_queued_update_dropped`
  // and `motion_trip_secondary_signal` come from the manager. Arm-time
  // failures emit `kMotionInterruptArmFailed` (kernel-owned, not
  // surfaced by the manager because the kernel decides not to dispatch).
  // `kMotionExecutionOutcomeRecorded` is the terminal record event
  // referenced by spec/210 §3.11; details carry the outcome's
  // `terminal_cause`, plus `execution_id`, `move_id`, and any
  // trip-specific identifiers when present.
  kMotionInterruptScopeArmed,
  kMotionInterruptScopeReaped,
  kMotionInterruptArmFailed,
  kMotionInterruptQueuedUpdateDropped,
  kMotionTripSecondarySignal,
  kMotionTripFailed,
  kMotionTripCompleted,
  kMotionExecutionOutcomeRecorded,
  // RT-8B recovery authority evidence (spec/200 §recovery, spec/230 §3.7.1).
  kStopCertificatePublished,
  kRecoveryAssessmentChanged,
  kRecoveryActionAccepted,
  kRecoveryActionRejected,
  // Generation-bound effective runtime collision coverage summary. Emitted
  // once when a controlled-stop monitor window closes; details retain exact
  // attempts/queries/unavailable gaps without creating a new runtime fault.
  kRuntimeCollisionCoverage,
  kContactPolicyViolation,
};

/// Kernel event structure (spec §6.1)
struct KernelEvent {
  KernelEventType type;
  uint64_t execution_id;
  int64_t timestamp_ns;  // from IClock

  // For kStateTransition
  ExecutionState from_state;
  ExecutionState to_state;

  // For kErrorRaised
  std::optional<ErrorDTO> error;

  // General-purpose structured key-value data
  std::map<std::string, std::string> details;

  /// Monotonic, contiguous dispatch-order sequence assigned by the kernel at
  /// dispatch time (1-based; 0 = not yet dispatched). Consumers use gaps to
  /// detect event loss anywhere in the pipeline (spec 520 §2.2). Declared
  /// last so the ubiquitous positional aggregate initializers stay valid.
  uint64_t sequence = 0;
};

/// Event callback type. Must be thread-safe.
using EventCallback = std::function<void(const KernelEvent&)>;

/// Subscription handle for unsubscribing.
struct SubscriptionHandle {
  uint64_t id;
};

}  // namespace manyforge::core
