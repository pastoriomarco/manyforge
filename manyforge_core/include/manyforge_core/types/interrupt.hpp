#pragma once

#include <cstdint>
#include <map>
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
};

enum class InterruptPostLatchDisposition {
  kAwaitOperatorAck,
  kAwaitRecoveryHandler,
  kAutoClearOnSignalRelease,
  kFaultTerminal,
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
