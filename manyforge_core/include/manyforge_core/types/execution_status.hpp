#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Execution state machine states (spec §3.10)
enum class ExecutionState {
  kIdle,
  kPlanning,
  kValidating,  // pre-execution validation phase
  kExecuting,
  /// Kernel-native interrupt latch is active. The prior execution phase is
  /// recorded on the InterruptLatch; the state itself is the observable
  /// interrupted annotation from spec/260.
  kInterrupted,
  kStopping,
  kStopped,
  kCompleted,
  kFailed,
  /// Terminal software fault: ManyForge could not prove stop/reset completion.
  /// This is an operator/external-safety intervention request, not a claim
  /// that ManyForge itself provides a certified safety function.
  kInterventionRequired,
};

inline const char* to_string(ExecutionState state) {
  switch (state) {
    using enum ExecutionState;
    case kIdle: return "Idle";
    case kPlanning: return "Planning";
    case kValidating: return "Validating";
    case kExecuting: return "Executing";
    case kInterrupted: return "Interrupted";
    case kStopping: return "Stopping";
    case kStopped: return "Stopped";
    case kCompleted: return "Completed";
    case kFailed: return "Failed";
    case kInterventionRequired: return "InterventionRequired";
  }
  return "Unknown";
}

/// Cache mode for trajectory caching (spec §3.1)
enum class CacheMode {
  kDefault,  // kernel MAY use and populate the trajectory cache
  kDisable,   // kernel MUST NOT read from or write to the trajectory cache
};

/// How one participant execution ended, as known by the participant. A fact,
/// never an operation outcome: the addressed Kernel derives Completed,
/// Stopped, Failed or InterventionRequired from it together with its own
/// Stop and escalation state.
enum class ParticipantExecutionFactKind : std::uint8_t {
  /// The execution reached the computed end of its command and the adapter
  /// reported natural completion; evidence is the published final snapshot.
  kComputedCompletion,
  /// Planning (including an expired planning deadline, `planning_timeout`),
  /// validation, preparation, dispatch rejection or an adapter fault ended
  /// the execution; the fact carries the exact error.
  kLocalFailure,
  /// Cancelled before dispatch: nothing crossed the dispatch barrier.
  kCancelledBeforeDispatch,
  /// The addressed owner's Stop ended the dispatched execution; the owner
  /// holds the containment evidence and certificate.
  kPreemptedByAddressedStop,
  /// Exact physical handover transferred this command to its recognized successor.
  kPreemptedByReplacement,
};

/// What the participant knows about physical effects of its ended execution.
/// A participant-level subset of the CCLC domain-effect vocabulary.
enum class ParticipantEffectKnowledge : std::uint8_t {
  /// Nothing was dispatched, or the effect was prevented before dispatch.
  kNoEffect,
  /// Dispatched; the adapter reported the command settled (completed,
  /// failed-and-stopped, or contained).
  kEffectSettled,
  /// Dispatched; settlement of the command could not be established.
  kEffectUncertain,
};

struct ParticipantExecutionFact {
  ParticipantExecutionFactKind kind = ParticipantExecutionFactKind::kLocalFailure;
  ParticipantEffectKnowledge effect = ParticipantEffectKnowledge::kNoEffect;
  std::optional<ErrorDTO> error;

  friend bool operator==(const ParticipantExecutionFact&, const ParticipantExecutionFact&) =
      default;
};

inline const char* to_string(ParticipantExecutionFactKind kind) {
  switch (kind) {
    using enum ParticipantExecutionFactKind;
    case kComputedCompletion: return "computed_completion";
    case kLocalFailure: return "local_failure";
    case kCancelledBeforeDispatch: return "cancelled_before_dispatch";
    case kPreemptedByAddressedStop: return "preempted_by_addressed_stop";
    case kPreemptedByReplacement: return "preempted_by_replacement";
  }
  return "unknown";
}

inline const char* to_string(ParticipantEffectKnowledge effect) {
  switch (effect) {
    using enum ParticipantEffectKnowledge;
    case kNoEffect: return "no_effect";
    case kEffectSettled: return "effect_settled";
    case kEffectUncertain: return "effect_uncertain";
  }
  return "unknown";
}

/// Structured execution status published by a participant execution engine.
/// `state` is a working phase (Idle, Planning, Validating, Executing). Once the
/// execution has ended, `fact` says how; `state` then keeps the phase in which
/// it ended until retirement returns the participant to Idle.
struct ExecutionStatus {
  ExecutionState state = ExecutionState::kIdle;

  /// Error code of the ended execution's fact, if any.
  ErrorCode last_error = ErrorCode::kSuccess;

  /// Progress through current trajectory [0.0, 1.0].
  double progress = 0.0;

  /// Timestamp of last state transition (nanoseconds, from IClock).
  int64_t last_transition_ns = 0;

  /// Execution ID for correlation (ADR-0011).
  uint64_t execution_id = 0;

  /// The move_id of the in-flight request (empty in Idle). Spec 230 §1.
  std::string active_move_id;

  /// Present from the end of the current execution until its retirement.
  std::optional<ParticipantExecutionFact> fact;
};

}  // namespace manyforge::core
