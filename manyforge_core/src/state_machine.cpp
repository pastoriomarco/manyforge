#include <manyforge_core/state_machine.hpp>

namespace manyforge::core {

ExecutionStateMachine::ExecutionStateMachine(IClock& clock, TransitionCallback on_transition)
    : clock_(clock), current_state_(ExecutionState::kIdle), on_transition_(std::move(on_transition)) {
    status_.state = ExecutionState::kIdle;
    status_.last_transition_ns = clock_.now();
}

// Spec 230 §6.3 — complete state transition table.
bool ExecutionStateMachine::is_valid_transition(ExecutionState from, ExecutionState to) const {
    switch (from) {
        case ExecutionState::kIdle:
            return to == ExecutionState::kPlanning ||
                   to == ExecutionState::kValidating ||  // cache hit
                   to == ExecutionState::kStreaming;     // begin_streaming_session (ADR-0016)
        case ExecutionState::kPlanning:
            return to == ExecutionState::kValidating ||
                   to == ExecutionState::kInterrupted ||
                   to == ExecutionState::kFailed ||
                   to == ExecutionState::kStopped ||  // stop cancels in-flight
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kValidating:
            return to == ExecutionState::kExecuting ||
                   to == ExecutionState::kInterrupted ||
                   to == ExecutionState::kFailed ||
                   to == ExecutionState::kStopped ||  // stop cancels in-flight
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kExecuting:
            return to == ExecutionState::kStopping ||
                   to == ExecutionState::kInterrupted ||
                   to == ExecutionState::kCompleted ||
                   to == ExecutionState::kFailed ||
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kStreaming:
            // ADR-0016: streaming exits via soft-stop (stop()) -> Stopping ->
            // Stopped, or via hard cancel (reset()) -> Stopped directly, or
            // via fault -> Failed/InterventionRequired. Per-target updates do
            // not transition out of kStreaming.
            return to == ExecutionState::kStopping ||
                   to == ExecutionState::kInterrupted ||
                   to == ExecutionState::kStopped ||
                   to == ExecutionState::kFailed ||
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kPaused:
            return to == ExecutionState::kValidating ||  // resume re-validates
                   to == ExecutionState::kInterrupted ||
                   to == ExecutionState::kStopping ||
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kStopping:
            return to == ExecutionState::kStopped ||
                   to == ExecutionState::kPaused ||  // deceleration complete from pause
                   // Phase 4.5D-2 (spec/230 §3.4): a successful
                   // complete_move_on_signal trip transitions
                   // Stopping -> Completed once the controlled stop
                   // is confirmed and the validated stop snapshot
                   // is captured. The corresponding failure paths
                   // (backend stop failed, stop snapshot invalid)
                   // transition Stopping -> Failed.
                   to == ExecutionState::kCompleted ||
                   to == ExecutionState::kFailed ||
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kStopped:
            // ADR-0016: streaming sessions auto-return to kIdle after a
            // clean end_streaming_session (the operator's mouse release
            // IS the acknowledgement; no separate ack step needed). For
            // discrete trajectories, this transition is also acceptable
            // since the existing kernel reset() path uses sm_.reset()
            // which bypasses transition rules entirely.
            return to == ExecutionState::kIdle ||
                   to == ExecutionState::kInterventionRequired;
        case ExecutionState::kCompleted:
        case ExecutionState::kFailed:
            return to == ExecutionState::kInterventionRequired;
        case ExecutionState::kInterrupted:
            return to == ExecutionState::kInterventionRequired ||
                   to == ExecutionState::kFailed ||
                   // Phase 4.5D-2: when a move-scoped
                   // complete_move_on_signal latch fires, the kernel
                   // transitions Interrupted -> Stopping while the
                   // controlled stop is in flight (spec/230 §3.4).
                   // This is the non-fault terminal path; the latch
                   // visible during Stopping is the trip itself.
                   to == ExecutionState::kStopping ||
                   to == ExecutionState::kIdle;
        case ExecutionState::kInterventionRequired:
            // Idempotent safety sinks are allowed: repeated fault reports from
            // independent monitors must not turn an already-held kernel into a
            // transition error.
            return to == ExecutionState::kInterventionRequired;
        default:
            return false;
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
    if (current_state_ == new_state) {
        return Result<void>::Ok();
    }
    if (!is_valid_transition(current_state_, new_state)) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }
    do_transition(new_state);
    return Result<void>::Ok();
}

Result<void> ExecutionStateMachine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_state_ == ExecutionState::kIdle) {
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
