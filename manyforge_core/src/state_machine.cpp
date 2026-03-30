#include <manyforge_core/state_machine.hpp>

namespace manyforge::core {

ExecutionStateMachine::ExecutionStateMachine(IClock& clock, TransitionCallback on_transition)
    : clock_(clock), current_state_(ExecutionState::kIdle), on_transition_(std::move(on_transition)) {
    status_.state = ExecutionState::kIdle;
    status_.last_transition_ns = clock_.now();
}

bool ExecutionStateMachine::is_valid_transition(ExecutionState from, ExecutionState to) const {
    switch (from) {
        case ExecutionState::kIdle: return to == ExecutionState::kPlanning;
        case ExecutionState::kPlanning: return to == ExecutionState::kExecuting || to == ExecutionState::kFailed;
        case ExecutionState::kExecuting:
            return to == ExecutionState::kPaused || to == ExecutionState::kStopping ||
                   to == ExecutionState::kCompleted || to == ExecutionState::kFailed;
        case ExecutionState::kPaused: return to == ExecutionState::kExecuting || to == ExecutionState::kStopping;
        case ExecutionState::kStopping: return to == ExecutionState::kStopped;
        default: return false;
    }
}

void ExecutionStateMachine::do_transition(ExecutionState new_state) {
    ExecutionState old_state = current_state_;
    current_state_ = new_state;
    status_.state = new_state;
    status_.last_transition_ns = clock_.now();
    if (on_transition_) {
        on_transition_(old_state, new_state);
    }
}

Result<void> ExecutionStateMachine::try_transition(ExecutionState new_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_transition(current_state_, new_state)) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }
    do_transition(new_state);
    return Result<void>::Ok();
}

Result<void> ExecutionStateMachine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_state_ != ExecutionState::kStopped &&
        current_state_ != ExecutionState::kCompleted &&
        current_state_ != ExecutionState::kFailed) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }
    do_transition(ExecutionState::kIdle);
    status_.last_error = ErrorCode::kSuccess;
    status_.progress = 0.0;
    return Result<void>::Ok();
}

ExecutionStatus ExecutionStateMachine::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void ExecutionStateMachine::set_progress(double progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.progress = progress;
}

void ExecutionStateMachine::set_last_error(ErrorCode error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.last_error = error;
}

void ExecutionStateMachine::set_execution_id(uint64_t execution_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.execution_id = execution_id;
}

}  // namespace manyforge::core
