#pragma once

#include <cstdint>
#include <string>

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Execution state machine states (spec §3.10)
enum class ExecutionState {
  kIdle,
  kPlanning,
  kValidating,  // pre-execution validation phase
  kExecuting,
  kPaused,
  kStopping,
  kStopped,
  kCompleted,
  kFailed,
};

inline const char* to_string(ExecutionState state) {
  switch (state) {
    case ExecutionState::kIdle: return "Idle";
    case ExecutionState::kPlanning: return "Planning";
    case ExecutionState::kValidating: return "Validating";
    case ExecutionState::kExecuting: return "Executing";
    case ExecutionState::kPaused: return "Paused";
    case ExecutionState::kStopping: return "Stopping";
    case ExecutionState::kStopped: return "Stopped";
    case ExecutionState::kCompleted: return "Completed";
    case ExecutionState::kFailed: return "Failed";
  }
  return "Unknown";
}

/// Cache mode for trajectory caching (spec §3.1)
enum class CacheMode {
  kDefault,  // kernel MAY use and populate the trajectory cache
  kDisable,   // kernel MUST NOT read from or write to the trajectory cache
};

/// Structured execution status published by the Kernel (spec §3.10)
struct ExecutionStatus {
  ExecutionState state = ExecutionState::kIdle;

  /// Error code if state is kFailed or kStopped.
  ErrorCode last_error = ErrorCode::kSuccess;

  /// Progress through current trajectory [0.0, 1.0].
  double progress = 0.0;

  /// Timestamp of last state transition (nanoseconds, from IClock).
  int64_t last_transition_ns = 0;

  /// Execution ID for correlation (ADR-0011).
  uint64_t execution_id = 0;
};

}  // namespace manyforge::core
