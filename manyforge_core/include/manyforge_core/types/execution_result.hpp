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
  std::optional<ErrorDTO> error;  // present when accepted is false
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
/// event with `principal_id` and `reason` for traceability. Callers (cycle
/// manager Run-side auto-ack, MCP, operator UI) hold the policy decision
/// about whether an ack is appropriate; the kernel only enforces
/// "explicit, traceable, fail-closed on other states".
struct AcknowledgeInterventionResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core
