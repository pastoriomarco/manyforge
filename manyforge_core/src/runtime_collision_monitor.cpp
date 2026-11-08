#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace manyforge::core {
namespace {

thread_local const RuntimeCollisionMonitor* t_current_runtime_collision_monitor = nullptr;

constexpr auto kDrainCallbacksTimeout = std::chrono::seconds(1);

void update_atomic_max(std::atomic<std::uint64_t>& target,
                       std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
}

TimePoint saturating_add(TimePoint base, Duration positive_delta) noexcept {
  if (positive_delta > 0 &&
      base > std::numeric_limits<TimePoint>::max() - positive_delta) {
    return std::numeric_limits<TimePoint>::max();
  }
  return base + positive_delta;
}

}  // namespace

RuntimeCollisionMonitor::RuntimeCollisionMonitor(
    IClock& clock,
    IScheduler& scheduler,
    Config config,
    TickCallback tick_callback,
    ShouldContinueCallback should_continue_callback)
    : clock_(clock),
      scheduler_(scheduler),
      config_(config),
      tick_callback_(std::move(tick_callback)),
      should_continue_callback_(std::move(should_continue_callback)) {}

bool RuntimeCollisionMonitor::in_callback() const noexcept {
  return t_current_runtime_collision_monitor == this;
}

RuntimeCollisionMonitor::~RuntimeCollisionMonitor() {
  stop(/*drain_callbacks=*/true);
}

void RuntimeCollisionMonitor::set_fail_closed_dump_hook(
    std::function<void()> hook) {
  fail_closed_dump_hook_ = std::move(hook);
}

void RuntimeCollisionMonitor::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (detached_) return;
  active_ = true;
  ++generation_;
  if (handle_ != 0) {
    scheduler_.cancel(handle_);
    handle_ = 0;
  }
  const auto period_ns = std::max<Duration>(1, config_.period_ns);
  schedule_next_locked(generation_, saturating_add(clock_.now(), period_ns));
}

void RuntimeCollisionMonitor::stop(bool drain_callbacks) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detach_completed_) return;
  }
  if (!in_callback()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      ++generation_;
      if (handle_ != 0) {
        scheduler_.cancel(handle_);
        handle_ = 0;
      }
    }
    // Monitor callbacks can own borrowed kernel state: a missed drain bound
    // must not continue into teardown or a new session. It keeps waiting.
    if (drain_callbacks) {
      WaitForDrain(
          "runtime_collision_monitor.scheduler_runtime_safety_lane",
          kDrainCallbacksTimeout,
          [this](std::chrono::nanoseconds bound) {
            return scheduler_.drain_for(SchedulerPriority::RuntimeSafety, bound);
          },
          [this] {
            if (fail_closed_dump_hook_) fail_closed_dump_hook_();
          });
    }
    std::unique_lock<std::mutex> lock(mutex_);
    auto no_callbacks_in_flight = [this]() {
      return callbacks_in_flight_ == 0;
    };
    if (drain_callbacks) {
      WaitForDrain(
          "runtime_collision_monitor.monitor_callbacks", kDrainCallbacksTimeout,
          [&](std::chrono::nanoseconds bound) {
            return cv_.wait_for(lock, bound, no_callbacks_in_flight);
          },
          [this] {
            if (fail_closed_dump_hook_) fail_closed_dump_hook_();
          });
    } else {
      (void)cv_.wait_for(lock, std::chrono::milliseconds(250),
                         no_callbacks_in_flight);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    ++generation_;
    if (handle_ != 0) {
      scheduler_.cancel(handle_);
      handle_ = 0;
    }
  }
}

Result<void> RuntimeCollisionMonitor::detach() {
  if (in_callback())
    return Result<void>::Err(ErrorDTO{kErrorExecutionRejected,
        "Cannot detach a monitor from its own callback", {{"reason", "scheduler_not_ready"}}});
  std::lock_guard detach_lock(detach_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detach_completed_) return Result<void>::Ok();
    detached_ = true;
  }
  // Owner admission is closed; callbacks still in flight may finish and must
  // be drained before the scheduler borrow can be released.
  stop(/*drain_callbacks=*/true);
  std::lock_guard<std::mutex> lock(mutex_);
  detach_completed_ = true;
  return Result<void>::Ok();
}

void RuntimeCollisionMonitor::set_active_contact_policy_id(
    std::string contact_policy_id) {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  active_contact_policy_id_ = std::move(contact_policy_id);
}

void RuntimeCollisionMonitor::clear_active_contact_policy_id() {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  active_contact_policy_id_.clear();
}

std::string RuntimeCollisionMonitor::active_contact_policy_id() const {
  std::lock_guard<std::mutex> lock(contact_policy_mutex_);
  return active_contact_policy_id_;
}

RuntimeCollisionMonitorTimingMetrics RuntimeCollisionMonitor::timing_metrics()
    const noexcept {
  return RuntimeCollisionMonitorTimingMetrics{
      timing_callbacks_started_.load(std::memory_order_relaxed),
      timing_start_jitter_max_ns_.load(std::memory_order_relaxed),
      timing_callback_duration_max_ns_.load(std::memory_order_relaxed),
      timing_late_periods_.load(std::memory_order_relaxed),
      timing_missed_periods_.load(std::memory_order_relaxed),
      timing_max_consecutive_late_periods_.load(std::memory_order_relaxed),
  };
}

void RuntimeCollisionMonitor::schedule_next_locked(std::uint64_t generation,
                                                   TimePoint deadline) {
  handle_ = scheduler_.schedule(
      deadline, [this, generation, deadline]() {
        const auto callback_started_ns = clock_.now();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!active_ || generation_ != generation) {
            return;
          }
          handle_ = 0;
          ++callbacks_in_flight_;
        }

        // Canceled callbacks from an older generation are not monitor work and
        // must not contaminate deadline evidence for the active generation.
        timing_callbacks_started_.fetch_add(1, std::memory_order_relaxed);
        const auto jitter_ns = callback_started_ns > deadline
                                   ? static_cast<std::uint64_t>(
                                         callback_started_ns - deadline)
                                   : 0;
        update_atomic_max(timing_start_jitter_max_ns_, jitter_ns);
        struct CallbackGuard {
          RuntimeCollisionMonitor* self;
          const RuntimeCollisionMonitor* previous = t_current_runtime_collision_monitor;

          explicit CallbackGuard(RuntimeCollisionMonitor* monitor)
              : self(monitor) {
            t_current_runtime_collision_monitor = monitor;
          }

          ~CallbackGuard() {
            t_current_runtime_collision_monitor = previous;
            {
              std::lock_guard<std::mutex> lock(self->mutex_);
              if (self->callbacks_in_flight_ > 0) {
                --self->callbacks_in_flight_;
              }
            }
            self->cv_.notify_all();
          }
        } callback_guard{this};

        if (tick_callback_) {
          (void)tick_callback_();
        }
        const auto callback_finished_ns = clock_.now();
        const auto callback_duration_ns =
            callback_finished_ns > callback_started_ns
                ? static_cast<std::uint64_t>(callback_finished_ns -
                                             callback_started_ns)
                : 0;
        update_atomic_max(timing_callback_duration_max_ns_,
                          callback_duration_ns);

        const auto period_ns = std::max<Duration>(1, config_.period_ns);
        const auto elapsed_after_deadline_ns =
            callback_finished_ns > deadline
                ? static_cast<std::uint64_t>(callback_finished_ns - deadline)
                : 0;
        const auto missed =
            elapsed_after_deadline_ns /
            static_cast<std::uint64_t>(period_ns);
        if (missed > 0) {
          timing_late_periods_.fetch_add(1, std::memory_order_relaxed);
          timing_missed_periods_.fetch_add(missed,
                                           std::memory_order_relaxed);
          const auto consecutive =
              timing_consecutive_late_periods_.fetch_add(
                  1, std::memory_order_relaxed) +
              1;
          update_atomic_max(timing_max_consecutive_late_periods_, consecutive);
        } else {
          timing_consecutive_late_periods_.store(0,
                                                 std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || generation_ != generation ||
            (should_continue_callback_ && !should_continue_callback_())) {
          return;
        }
        // Keep a fixed-rate cadence without queuing a catch-up burst. Every
        // elapsed deadline is counted above; the next callback is scheduled at
        // the first cadence boundary strictly after this callback completed.
        TimePoint next_deadline = saturating_add(deadline, period_ns);
        if (callback_finished_ns > deadline) {
          const auto remainder =
              elapsed_after_deadline_ns %
              static_cast<std::uint64_t>(period_ns);
          const auto advance =
              remainder == 0
                  ? period_ns
                  : period_ns - static_cast<Duration>(remainder);
          next_deadline = saturating_add(callback_finished_ns, advance);
        }
        schedule_next_locked(generation, next_deadline);
      },
      SchedulerPriority::RuntimeSafety);
}

}  // namespace manyforge::core
