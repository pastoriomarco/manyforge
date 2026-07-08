#include <manyforge_core/stop_policy.hpp>

namespace manyforge::core {

StopPolicy::StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
                       CacheInvalidationCallback on_invalidate,
                       TerminalTransitionCallback terminal_transition,
                       StopRequestedChangedCallback on_stop_requested_changed)
    : sm_(sm), scheduler_(scheduler), clock_(clock),
      on_invalidate_(std::move(on_invalidate)),
      terminal_transition_(std::move(terminal_transition)),
      on_stop_requested_changed_(std::move(on_stop_requested_changed)) {}

StopPolicy::~StopPolicy() {
    cancel_pending_callbacks();
}

Result<void> StopPolicy::transition_terminal(ExecutionState state) {
    if (terminal_transition_) {
        return terminal_transition_(state);
    }
    return sm_.try_transition(state);
}

Result<void> StopPolicy::invoke_stop_callback() {
    StopCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = stop_callback_;
    }
    if (!callback) {
        return Result<void>::Ok();
    }
    return callback();
}

Result<void> StopPolicy::transition_intervention_after_stop_failure(
    const Result<void>& stop_result) {
    (void)transition_terminal(ExecutionState::kInterventionRequired);
    return stop_result;
}

void StopPolicy::update_stop_requested(bool requested) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = stop_requested_ != requested;
        stop_requested_ = requested;
    }
    if (changed && on_stop_requested_changed_) {
        on_stop_requested_changed_(requested);
    }
}

Result<void> StopPolicy::request_soft_stop(Duration deceleration_ns) {
    auto current = sm_.status().state;

    // From Planning or Validating: direct to Stopped (cancel in-flight).
    if (current == ExecutionState::kPlanning || current == ExecutionState::kValidating) {
        update_stop_requested(true);
        if (on_invalidate_) on_invalidate_();
        auto stop_result = invoke_stop_callback();
        if (!stop_result.ok()) {
            return transition_intervention_after_stop_failure(stop_result);
        }
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
        return Result<void>::Ok();
    }

    // From Executing: decelerate via scheduler.
    if (current == ExecutionState::kExecuting) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        update_stop_requested(true);
        if (on_invalidate_) on_invalidate_();

        auto deadline = clock_.now() + deceleration_ns;
        ScheduleHandle handle = scheduler_.schedule(deadline, [this]() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_soft_stop_ = 0;
            }
            const auto stop_result = invoke_stop_callback();
            if (!stop_result.ok()) {
                (void)transition_intervention_after_stop_failure(stop_result);
                return;
            }
            // Stop authority asserting its own terminal; a failure means a
            // higher-priority terminal already latched — benign to discard.
            (void)transition_terminal(ExecutionState::kStopped);
        }, SchedulerPriority::StopControl);
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
        update_stop_requested(true);
        if (on_invalidate_) on_invalidate_();
        auto stop_result = invoke_stop_callback();
        if (!stop_result.ok()) {
            return transition_intervention_after_stop_failure(stop_result);
        }
        auto result2 = transition_terminal(ExecutionState::kStopped);
        if (!result2.ok()) return result2;
        return Result<void>::Ok();
    }

    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
}

Result<void> StopPolicy::request_hard_stop() {
    // Cancel any pending scheduled stop/pause before issuing the hard floor.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
        if (pending_pause_ != 0) {
            scheduler_.cancel(pending_pause_);
            pending_pause_ = 0;
        }
    }

    auto current = sm_.status().state;
    const bool backend_stop_required =
        current == ExecutionState::kPlanning ||
        current == ExecutionState::kValidating ||
        current == ExecutionState::kExecuting ||
        current == ExecutionState::kPaused ||
        current == ExecutionState::kStopping;

    bool terminal_stop_deferred_until_backend_ack = false;

    // From Planning or Validating: direct to Stopped after the backend stop
    // floor has had a chance to cancel a paused/resume goal that may still be
    // owned by the execution backend.
    if (current == ExecutionState::kPlanning || current == ExecutionState::kValidating) {
        terminal_stop_deferred_until_backend_ack = true;
    }
    // From Stopping: upgrade soft → hard, but only publish Stopped after the
    // backend acknowledges the hard floor.
    else if (current == ExecutionState::kStopping) {
        terminal_stop_deferred_until_backend_ack = true;
    }
    // From Executing: enter Stopping before the hard floor so active-motion
    // backend refusal cannot be reported as a clean stop.
    else if (current == ExecutionState::kExecuting) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        terminal_stop_deferred_until_backend_ack = true;
    }
    // From Paused: ask the backend before entering Stopping. Some backends
    // implement pause by terminating the action handle; in that case "no
    // execution in flight" is a valid no-op stop and must not be converted
    // into a false InterventionRequired.
    else if (current == ExecutionState::kPaused) {
        terminal_stop_deferred_until_backend_ack = true;
    }
    else {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }

    update_stop_requested(true);
    if (on_invalidate_) on_invalidate_();
    if (backend_stop_required) {
        auto stop_result = invoke_stop_callback();
        if (!stop_result.ok()) {
            return transition_intervention_after_stop_failure(stop_result);
        }
    }
    if (terminal_stop_deferred_until_backend_ack) {
        if (current == ExecutionState::kPaused &&
            sm_.status().state == ExecutionState::kPaused) {
            auto stopping = sm_.try_transition(ExecutionState::kStopping);
            if (!stopping.ok()) return stopping;
        }
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
    }
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
    ScheduleHandle handle = scheduler_.schedule(deadline, [this]() {
        PauseCompletedCallback completed_callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_pause_ = 0;
            completed_callback = pause_completed_callback_;
        }
        auto transition_result = sm_.try_transition(ExecutionState::kPaused);
        if (transition_result.ok() && completed_callback) {
            completed_callback();
        }
    }, SchedulerPriority::Lifecycle);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_pause_ = handle;
    }

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
    bool stop_requested_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
        if (pending_pause_ != 0) {
            scheduler_.cancel(pending_pause_);
            pending_pause_ = 0;
        }
        stop_requested_changed = stop_requested_;
        stop_requested_ = false;
    }
    if (stop_requested_changed && on_stop_requested_changed_) {
        on_stop_requested_changed_(false);
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

void StopPolicy::cancel_pending_callbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_soft_stop_ != 0) {
        scheduler_.cancel(pending_soft_stop_);
        pending_soft_stop_ = 0;
    }
    if (pending_pause_ != 0) {
        scheduler_.cancel(pending_pause_);
        pending_pause_ = 0;
    }
}

bool StopPolicy::stop_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

}  // namespace manyforge::core
