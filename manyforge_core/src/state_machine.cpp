#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>

namespace manyforge::core {
namespace {

bool working_phase(ExecutionState state) {
  return state == ExecutionState::kPlanning || state == ExecutionState::kValidating ||
         state == ExecutionState::kExecuting;
}

bool valid_edge(ExecutionState from, ExecutionState to) {
  switch (from) {
    case ExecutionState::kIdle:
      return to == ExecutionState::kPlanning || to == ExecutionState::kValidating;  // cache hit
    case ExecutionState::kPlanning:
      return to == ExecutionState::kValidating;
    case ExecutionState::kValidating:
      // Replan after an authoritative validation rejection, or dispatch.
      return to == ExecutionState::kPlanning || to == ExecutionState::kExecuting;
    default:
      return false;
  }
}

}  // namespace

ExecutionStateMachine::ExecutionStateMachine(IClock& clock) : clock_(clock) {
  status_.last_transition_ns = clock_.now();
}

Result<void> ExecutionStateMachine::try_transition(ExecutionState phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.fact.has_value() || !working_phase(phase)) {
    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
  }
  if (status_.state == phase) {
    return Result<void>::Ok();
  }
  if (!valid_edge(status_.state, phase)) {
    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
  }
  status_.state = phase;
  status_.last_transition_ns = clock_.now();
  return Result<void>::Ok();
}

Result<void> ExecutionStateMachine::settle(ParticipantExecutionFact fact) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.fact.has_value() || !working_phase(status_.state)) {
    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
  }
  status_.last_error = fact.error.has_value() ? error_code_from_dto(*fact.error)
                                              : ErrorCode::kSuccess;
  if (fact.kind == ParticipantExecutionFactKind::kComputedCompletion) {
    status_.progress = 1.0;
  }
  status_.fact = std::move(fact);
  status_.last_transition_ns = clock_.now();
  return Result<void>::Ok();
}

void ExecutionStateMachine::retire() {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.state = ExecutionState::kIdle;
  status_.fact.reset();
  status_.last_error = ErrorCode::kSuccess;
  status_.progress = 0.0;
  status_.last_transition_ns = clock_.now();
}

ExecutionStatus ExecutionStateMachine::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

bool ExecutionStateMachine::settled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_.fact.has_value();
}

void ExecutionStateMachine::set_progress(double progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.progress = progress;
}

void ExecutionStateMachine::set_execution_id(uint64_t execution_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.execution_id = execution_id;
}

}  // namespace manyforge::core
