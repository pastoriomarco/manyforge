#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>

namespace manyforge::core {

/// Observer-safe snapshot of backend-owned execution state.
struct ExecutionSnapshot {
  int64_t timestamp_ns = 0;
  uint64_t sequence_number = 0;
  uint64_t execution_id = 0;
  std::string move_id;
  ExecutionState state = ExecutionState::kIdle;
  double progress = 0.0;
  std::vector<double> joint_positions_rad;
  /// Measured joint velocities (rad/s) when the backend has controller
  /// feedback available. Empty means unknown/not-available; consumers MUST
  /// treat empty as "no measured settle evidence" rather than at-rest.
  std::vector<double> joint_velocities_rad_s;
  std::string state_source;
  int64_t source_timestamp_ns = 0;
  int64_t source_age_ns = 0;
  std::optional<ErrorDTO> error;
};

/// Execution handle for tracking trajectory execution (spec 220 §4.2)
/// Provides phase-event feedback; exact delivery mechanism is implementation-defined.
struct ExecutionHandle {
  std::function<bool(int64_t timeout_ns)> wait_for_completion;
  std::function<ExecutionSnapshot()> get_snapshot;
};

/// Stop result (spec 220 §4.2)
struct StopResultDTO {
  bool accepted = false;
  // Present when the stop command failed or completed through a degraded
  // fallback path that the operator/audit trail must see.
  std::optional<ErrorDTO> error;
};

/// Pause result (spec 220 §4.2)
struct PauseResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Resume result (spec 230 §1.1)
struct ResumeResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Reset result (spec 220 §4.2)
struct ResetResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Successful terminal acknowledgement result.
///
/// This is intentionally narrower than Reset: it is valid only for the exact
/// Completed execution_id observed by the runtime, and must not clear Failed or
/// Stopped terminal states.
struct AcknowledgeCompletedResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Explicit intervention acknowledgement result.
///
/// Valid only from `kInterventionRequired`. Clears the intervention flag
/// and transitions to `kIdle`, emitting a `kInterventionAcknowledged`
/// event with `principal_id` and `reason` for traceability. The reset/resync
/// and retry-without-reset intervention APIs share this result shape.
struct AcknowledgeInterventionResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Explicit acknowledgement of an interrupted execution for operator retry.
///
/// Unlike Reset, this must not command or reset the execution backend. It is
/// used when a fault interrupt stopped a motion, the operator fixed the
/// condition, and Run means "retry from the current observed robot state".
struct AcknowledgeInterruptedResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core
