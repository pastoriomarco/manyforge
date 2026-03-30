#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/time/iclock.hpp>

#include <functional>
#include <mutex>

namespace manyforge::core {

/// Thread-safe execution state machine.
/// Encodes valid transitions between execution states.
/// ADR-0005: Kernel owns the state machine; BT/HMI observe via status.
class ExecutionStateMachine {
 public:
  /// Callback signature for state transitions: (from_state, to_state).
  using TransitionCallback = std::function<void(ExecutionState, ExecutionState)>;

  /// Constructs the state machine in kIdle state.
  /// @param clock The clock to use for timestamps.
  /// @param on_transition Optional callback fired on each transition.
  ExecutionStateMachine(IClock& clock, TransitionCallback on_transition = nullptr);

  /// Attempt to transition to the given state.
  /// @param new_state The target state.
  /// @return Result<void> - Ok() on success, Err(kInvalidStateTransition) if invalid.
  Result<void> try_transition(ExecutionState new_state);

  /// Reset the state machine to kIdle.
  /// Valid from ANY non-Idle state (spec 230 §6.3).
  /// @return Result<void> - Ok() on success, Err(kInvalidStateTransition) if already Idle.
  Result<void> reset();

  /// Get the current execution status.
  /// @return ExecutionStatus with current state, error, progress, and timestamp.
  ExecutionStatus status() const;

  /// Set the current progress (0.0 to 1.0).
  void set_progress(double progress);

  /// Set the last error code.
  void set_last_error(ErrorCode error);

  /// Set the execution ID.
  void set_execution_id(uint64_t execution_id);

 private:
  /// Check if a transition is valid.
  bool is_valid_transition(ExecutionState from, ExecutionState to) const;

  /// Perform the transition (must be called with lock held).
  void do_transition(ExecutionState new_state);

  IClock& clock_;
  mutable std::mutex mutex_;
  ExecutionState current_state_;
  ExecutionStatus status_;
  TransitionCallback on_transition_;
};

}  // namespace manyforge::core
