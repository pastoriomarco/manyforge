#pragma once

#include <functional>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
  std::optional<ActionSpanDTO> action_span;
  std::string contact_policy_id;
};

/// Execution handle for tracking trajectory execution
/// Provides phase-event feedback; exact delivery mechanism is implementation-defined.
struct ExecutionHandle {
  ExecutionHandle() = default;
  ExecutionHandle(std::function<bool(int64_t)> wait, std::function<ExecutionSnapshot()> snapshot,
                  std::uint64_t generation = 0,
                  std::optional<ControllerGoalId> goal_id = std::nullopt,
                  std::optional<ControllerGoalGeneration> goal_generation = std::nullopt,
                  std::optional<ContentDigest> contract_digest = std::nullopt)
      : wait_for_completion(std::move(wait)),
        get_snapshot(std::move(snapshot)),
        execution_generation(generation),
        controller_goal_id(std::move(goal_id)),
        controller_goal_generation(std::move(goal_generation)),
        prepared_contract_digest(std::move(contract_digest)) {}

  std::function<bool(int64_t timeout_ns)> wait_for_completion;
  std::function<ExecutionSnapshot()> get_snapshot;
  /// Backend generation opened by this exact dispatch. Zero means the backend
  /// cannot support generation-fenced runtime lookahead and admission must
  /// fail closed when lookahead is enabled.
  std::uint64_t execution_generation = 0;
  /// Exact Kernel goal/prepared membership for the direct dispatch path.
  /// All three fields are jointly present there and jointly absent on the
  /// historical trajectory-only path while that path is being removed.
  std::optional<ControllerGoalId> controller_goal_id;
  std::optional<ControllerGoalGeneration> controller_goal_generation;
  std::optional<ContentDigest> prepared_contract_digest;
};

/// Stop result
struct StopResultDTO {
  bool accepted = false;
  // Present when the stop command failed or completed through a degraded
  // fallback path that the operator/audit trail must see.
  std::optional<ErrorDTO> error;
};

/// Reset result
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

}  // namespace manyforge::core
