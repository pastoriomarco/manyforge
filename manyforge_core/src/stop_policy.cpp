#include <manyforge_core/stop_policy.hpp>

namespace manyforge::core {

StopPolicy::StopPolicy(ExecutionStateMachine& sm, IScheduler& scheduler, IClock& clock,
                       CacheInvalidationCallback on_invalidate)
    : sm_(sm), scheduler_(scheduler), clock_(clock),
      on_invalidate_(std::move(on_invalidate)) {}

Result<void> StopPolicy::request_soft_stop(Duration deceleration_ns) {
    auto current = sm_.status().state;

    if (current == ExecutionState::kExecuting) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }

        if (on_invalidate_) {
            on_invalidate_();
        }

        auto deadline = clock_.now() + deceleration_ns;
        ScheduleHandle handle = scheduler_.schedule(deadline, [this]() {
            sm_.try_transition(ExecutionState::kStopped);
            if (stop_callback_) {
                stop_callback_();
            }
        });

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_soft_stop_ = handle;
        }

        return Result<void>::Ok();
    }

    if (current == ExecutionState::kPaused) {
        // Already stationary — no deceleration needed, stop immediately.
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }

        if (on_invalidate_) {
            on_invalidate_();
        }

        auto result2 = sm_.try_transition(ExecutionState::kStopped);
        if (!result2.ok()) return result2;

        if (stop_callback_) {
            stop_callback_();
        }
        return Result<void>::Ok();
    }

    return Result<void>::Err(ErrorCode::kInvalidStateTransition);
}

Result<void> StopPolicy::request_hard_stop() {
    auto current = sm_.status().state;

    // Cancel any pending soft stop deceleration.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_soft_stop_ != 0) {
            scheduler_.cancel(pending_soft_stop_);
            pending_soft_stop_ = 0;
        }
    }

    if (current == ExecutionState::kStopping) {
        // Already stopping (from soft stop) — complete immediately.
        auto result = sm_.try_transition(ExecutionState::kStopped);
        if (!result.ok()) return result;
    } else if (current == ExecutionState::kExecuting ||
               current == ExecutionState::kPaused) {
        auto result = sm_.try_transition(ExecutionState::kStopping);
        if (!result.ok()) return result;
        auto result2 = sm_.try_transition(ExecutionState::kStopped);
        if (!result2.ok()) return result2;
    } else {
        return Result<void>::Err(ErrorCode::kInvalidStateTransition);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }

    if (on_invalidate_) {
        on_invalidate_();
    }
    if (stop_callback_) {
        stop_callback_();
    }
    return Result<void>::Ok();
}

Result<void> StopPolicy::request_pause() {
    return sm_.try_transition(ExecutionState::kPaused);
}

Result<void> StopPolicy::request_resume() {
    return sm_.try_transition(ExecutionState::kExecuting);
}

void StopPolicy::set_stop_callback(StopCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_callback_ = std::move(callback);
}

bool StopPolicy::stop_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

}  // namespace manyforge::core
