#include <manyforge_core/stop_policy.hpp>

namespace manyforge::core {

StopPolicy::StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
                       CacheInvalidationCallback on_invalidate,
                       TerminalTransitionCallback terminal_transition)
    : sm_(sm), scheduler_(scheduler), clock_(clock),
      on_invalidate_(std::move(on_invalidate)),
      terminal_transition_(std::move(terminal_transition)) {}

Result<void> StopPolicy::transition_terminal(ExecutionState state) {
    if (terminal_transition_) {
        return terminal_transition_(state);
    }
    return sm_.try_transition(state);
}

Result<void> StopPolicy::request_soft_stop(Duration deceleration_ns) {
    auto current = sm_.status().state;

    // From Planning or Validating: direct to Stopped (cancel in-flight).
    if (current == ExecutionState::kPlanning || current == ExecutionState::kValidating) {
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        if (on_invalidate_) on_invalidate_();
        if (stop_callback_) stop_callback_();
        return Result<void>::Ok();
    }

    // From Executing: decelerate via scheduler.
    if (current == ExecutionState::kExecuting) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        if (on_invalidate_) on_invalidate_();

        auto deadline = clock_.now() + deceleration_ns;
        ScheduleHandle handle = scheduler_.schedule(deadline, [this]() {
            // Stop authority asserting its own terminal; a failure means a
            // higher-priority terminal already latched — benign to discard.
            (void)transition_terminal(ExecutionState::kStopped);
            if (stop_callback_) stop_callback_();
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_soft_stop_ = handle;
        }
        return Result<void>::Ok();
    }

    // From Paused: no deceleration needed, stop immediately.
    if (current == ExecutionState::kPaused) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        if (on_invalidate_) on_invalidate_();
        auto result2 = transition_terminal(ExecutionState::kStopped);
        if (!result2.ok()) return result2;
        if (stop_callback_) stop_callback_();
        return Result<void>::Ok();
    }

    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
}

Result<void> StopPolicy::request_hard_stop() {
    // Cancel any pending soft stop.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
    }

    auto current = sm_.status().state;

    // From Planning or Validating: direct to Stopped.
    if (current == ExecutionState::kPlanning || current == ExecutionState::kValidating) {
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
    }
    // From Stopping: upgrade soft → hard (immediate Stopped).
    else if (current == ExecutionState::kStopping) {
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
    }
    // From Executing or Paused: → Stopping → Stopped.
    else if (current == ExecutionState::kExecuting || current == ExecutionState::kPaused) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        auto result2 = transition_terminal(ExecutionState::kStopped);
        if (!result2.ok()) return result2;
    }
    else {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    if (on_invalidate_) on_invalidate_();
    if (stop_callback_) stop_callback_();
    return Result<void>::Ok();
}

Result<void> StopPolicy::request_pause() {
    auto current = sm_.status().state;
    if (current != ExecutionState::kExecuting) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }

    // Executing → Stopping (deceleration), then scheduler fires → Paused.
    auto result = sm_.try_transition(ExecutionState::kStopping);
    if (!result.ok()) return result;

    auto deadline = clock_.now();
    scheduler_.schedule(deadline, [this]() {
        auto transition_result = sm_.try_transition(ExecutionState::kPaused);
        if (transition_result.ok() && pause_completed_callback_) {
            pause_completed_callback_();
        }
    });

    return Result<void>::Ok();
}

Result<void> StopPolicy::request_resume() {
    auto current = sm_.status().state;
    if (current != ExecutionState::kPaused) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }
    // Paused → Validating (re-validates before resuming).
    return sm_.try_transition(ExecutionState::kValidating);
}

Result<void> StopPolicy::request_reset() {
    auto current = sm_.status().state;
    if (current == ExecutionState::kIdle) {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }

    // Cancel any pending operations.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
        stop_requested_ = false;
    }

    return sm_.reset();
}

void StopPolicy::set_stop_callback(StopCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_callback_ = std::move(callback);
}

void StopPolicy::set_pause_completed_callback(PauseCompletedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    pause_completed_callback_ = std::move(callback);
}

bool StopPolicy::stop_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

}  // namespace manyforge::core
