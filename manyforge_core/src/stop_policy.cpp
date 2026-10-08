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

    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
}

Result<void> StopPolicy::request_hard_stop(
    BeforeTerminalStopCallback before_terminal) {
    // Cancel any pending scheduled stop before issuing the hard floor.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
    }

    auto current = sm_.status().state;
    const bool backend_stop_required =
        current == ExecutionState::kPlanning ||
        current == ExecutionState::kValidating ||
        current == ExecutionState::kExecuting ||
        current == ExecutionState::kStopping;

    bool terminal_stop_deferred_until_backend_ack = false;

    // From Planning or Validating: direct to Stopped after the backend stop
    // floor has had a chance to cancel a goal that may still be owned by the
    // execution backend.
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
        if (before_terminal) {
            before_terminal();
        }
        auto result = transition_terminal(ExecutionState::kStopped);
        if (!result.ok()) return result;
    }
    return Result<void>::Ok();
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

void StopPolicy::cancel_pending_callbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_soft_stop_ != 0) {
        scheduler_.cancel(pending_soft_stop_);
        pending_soft_stop_ = 0;
    }
}

bool StopPolicy::stop_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

}  // namespace manyforge::core
