#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <manyforge_core/bounded_stop_control_executor.hpp>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace manyforge::core {
namespace {

[[nodiscard]] std::chrono::nanoseconds remaining_wait(TimePoint now, TimePoint deadline) noexcept {
  if (deadline <= now) {
    return std::chrono::nanoseconds::zero();
  }
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if (now < 0 && deadline > maximum + now) {
    return std::chrono::nanoseconds::max();
  }
  return std::chrono::nanoseconds(deadline - now);
}

}  // namespace

class BoundedStopControlExecutor::Impl final {
 public:
  Impl(const std::size_t capacity, IClock& clock, IStopControlWorkReceiver& receiver)
      : capacity_(capacity), clock_(clock), receiver_(receiver) {
    if (capacity < 2U || capacity > kMaximumStopControlWorkSlots) {
      throw std::invalid_argument(
          "StopControl executor capacity must contain correlation and Full-Stop slots");
    }
    slots_ = std::make_unique<Slot[]>(capacity_);
    worker_ = std::jthread([this](std::stop_token stop_token) noexcept { Run(stop_token); });
    while (!worker_ready_.load(std::memory_order_acquire)) {
      worker_ready_.wait(false, std::memory_order_acquire);
    }
  }

  ~Impl() {
    CloseIngress();
    worker_.request_stop();
    WakeWorker();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] bool prequalified_ready() const noexcept {
    return accepting_.load(std::memory_order_acquire) &&
           worker_ready_.load(std::memory_order_acquire) &&
           worker_running_.load(std::memory_order_acquire);
  }

  void Publish(const StopControlWorkToken token) noexcept {
    EnterApi();
    bool published = false;
    if (accepting_.load(std::memory_order_acquire) && Valid(token)) {
      auto& slot = slots_[token.slot];
      auto retained = slot.requested_generation.load(std::memory_order_relaxed);
      while (token.generation > retained) {
        if (slot.requested_generation.compare_exchange_weak(
                retained, token.generation, std::memory_order_release, std::memory_order_relaxed)) {
          published = true;
          break;
        }
      }
    }
    if (published) {
      WakeWorker();
    }
    LeaveApi();
  }

  void ArmDeadline(const StopControlWorkToken token, const TimePoint deadline) noexcept {
    EnterApi();
    bool armed = false;
    if (accepting_.load(std::memory_order_acquire) && Valid(token) && deadline >= 0) {
      auto& slot = slots_[token.slot];
      const auto retained = slot.deadline_generation.load(std::memory_order_acquire);
      if (token.generation >= retained) {
        // Disarm before replacing the scalar fields. The one Stop owner may
        // rearm a generation after its prior deadline callback has dequeued;
        // a concurrent already-dequeued callback is intentionally not recalled.
        slot.deadline_armed.store(false, std::memory_order_release);
        slot.deadline_ns.store(deadline, std::memory_order_relaxed);
        slot.deadline_generation.store(token.generation, std::memory_order_relaxed);
        slot.deadline_armed.store(true, std::memory_order_release);
        armed = true;
      }
    }
    if (armed) {
      WakeWorker();
    }
    LeaveApi();
  }

  void CancelDeadline(const StopControlWorkToken token) noexcept {
    EnterApi();
    bool cancelled = false;
    if (Valid(token)) {
      auto& slot = slots_[token.slot];
      if (slot.deadline_generation.load(std::memory_order_acquire) == token.generation) {
        cancelled = slot.deadline_armed.exchange(false, std::memory_order_acq_rel);
      }
    }
    if (cancelled) {
      WakeWorker();
    }
    LeaveApi();
  }

  void CloseIngress() noexcept {
    accepting_.store(false, std::memory_order_release);
    WakeWorker();
    NotifyDrainState();
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    if (timeout < std::chrono::nanoseconds::zero()) {
      return false;
    }
    try {
      std::unique_lock lock(drain_mutex_);
      const auto started = std::chrono::steady_clock::now();
      constexpr auto maximum_poll =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(1));
      while (!Drained()) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed >= timeout) {
          return false;
        }
        const auto remaining = timeout - elapsed;
        static_cast<void>(drain_changed_.wait_for(lock, std::min(remaining, maximum_poll)));
      }
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  struct Slot final {
    std::atomic<std::uint64_t> requested_generation{0U};
    std::atomic<std::uint64_t> delivered_generation{0U};
    std::atomic<std::uint64_t> deadline_generation{0U};
    std::atomic<TimePoint> deadline_ns{0};
    std::atomic<bool> deadline_armed{false};
  };

  [[nodiscard]] bool Valid(const StopControlWorkToken token) const noexcept {
    return token.locally_valid() && token.slot < capacity_;
  }

  void EnterApi() noexcept { api_calls_inflight_.fetch_add(1U, std::memory_order_acq_rel); }

  void LeaveApi() noexcept {
    api_calls_inflight_.fetch_sub(1U, std::memory_order_acq_rel);
    NotifyDrainState();
  }

  void WakeWorker() noexcept {
    if (!wake_posted_.exchange(true, std::memory_order_acq_rel)) {
      wake_.release();
    }
  }

  void NotifyDrainState() noexcept { drain_changed_.notify_all(); }

  [[nodiscard]] bool Drained() const noexcept {
    if (api_calls_inflight_.load(std::memory_order_acquire) != 0U ||
        callback_inflight_.load(std::memory_order_acquire) != 0U ||
        worker_scanning_.load(std::memory_order_acquire)) {
      return false;
    }
    for (std::size_t index = 0; index < capacity_; ++index) {
      const auto& slot = slots_[index];
      if (slot.requested_generation.load(std::memory_order_acquire) >
              slot.delivered_generation.load(std::memory_order_acquire) ||
          slot.deadline_armed.load(std::memory_order_acquire)) {
        return false;
      }
    }
    return true;
  }

  void Invoke(const StopControlWorkToken token) noexcept {
    callback_inflight_.fetch_add(1U, std::memory_order_acq_rel);
    receiver_.ProgressStop(token);
    callback_inflight_.fetch_sub(1U, std::memory_order_acq_rel);
    NotifyDrainState();
  }

  [[nodiscard]] std::optional<TimePoint> Scan() noexcept {
    std::optional<TimePoint> nearest_deadline;
    for (std::size_t index = 0; index < capacity_; ++index) {
      auto& slot = slots_[index];
      const auto requested = slot.requested_generation.load(std::memory_order_acquire);
      const auto delivered = slot.delivered_generation.load(std::memory_order_acquire);
      bool invoked_initial = false;
      if (requested > delivered) {
        Invoke(StopControlWorkToken{static_cast<std::uint32_t>(index), requested});
        slot.delivered_generation.store(requested, std::memory_order_release);
        invoked_initial = true;
      }

      if (!slot.deadline_armed.load(std::memory_order_acquire)) {
        continue;
      }
      const auto deadline_generation = slot.deadline_generation.load(std::memory_order_acquire);
      const auto current_generation = slot.requested_generation.load(std::memory_order_acquire);
      if (current_generation > deadline_generation) {
        bool expected = true;
        static_cast<void>(slot.deadline_armed.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel, std::memory_order_acquire));
        continue;
      }
      // Arm-before-Publish is the required safe ordering: the deadline stays
      // dormant until Publish makes the same generation visible.
      if (current_generation < deadline_generation) {
        continue;
      }
      const auto deadline = slot.deadline_ns.load(std::memory_order_acquire);
      if (deadline <= clock_.now()) {
        bool expected = true;
        if (slot.deadline_armed.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
          // The initial progress call already observes an elapsed outer
          // deadline. Do not manufacture a redundant callback in the same
          // scan merely because ArmDeadline preceded Publish.
          if (!invoked_initial) {
            Invoke(StopControlWorkToken{static_cast<std::uint32_t>(index), deadline_generation});
          }
        }
      } else if (!nearest_deadline.has_value() || deadline < *nearest_deadline) {
        nearest_deadline = deadline;
      }
    }
    return nearest_deadline;
  }

  void Run(const std::stop_token stop_token) noexcept {
    worker_running_.store(true, std::memory_order_release);
    worker_ready_.store(true, std::memory_order_release);
    worker_ready_.notify_all();

    while (!stop_token.stop_requested()) {
      worker_scanning_.store(true, std::memory_order_release);
      const auto nearest_deadline = Scan();
      worker_scanning_.store(false, std::memory_order_release);
      NotifyDrainState();
      if (stop_token.stop_requested()) {
        break;
      }

      bool awakened = false;
      if (nearest_deadline.has_value()) {
        const auto wait = remaining_wait(clock_.now(), *nearest_deadline);
        if (wait > std::chrono::nanoseconds::zero()) {
          awakened = wake_.try_acquire_for(wait);
        }
      } else {
        wake_.acquire();
        awakened = true;
      }
      if (awakened) {
        // A producer that races this store either left all of its state visible
        // to the next scan or observes false and posts the next semaphore wake.
        wake_posted_.store(false, std::memory_order_release);
      }
    }

    worker_scanning_.store(false, std::memory_order_release);
    worker_running_.store(false, std::memory_order_release);
    NotifyDrainState();
  }

  const std::size_t capacity_;
  IClock& clock_;
  IStopControlWorkReceiver& receiver_;
  std::unique_ptr<Slot[]> slots_;
  std::atomic<bool> accepting_{true};
  std::atomic<bool> worker_ready_{false};
  std::atomic<bool> worker_running_{false};
  std::atomic<bool> worker_scanning_{false};
  std::atomic<std::size_t> api_calls_inflight_{0U};
  std::atomic<std::size_t> callback_inflight_{0U};
  std::binary_semaphore wake_{0};
  std::atomic<bool> wake_posted_{false};
  std::mutex drain_mutex_;
  std::condition_variable drain_changed_;
  std::jthread worker_;
};

BoundedStopControlExecutor::BoundedStopControlExecutor(const std::size_t capacity, IClock& clock,
                                                       IStopControlWorkReceiver& receiver)
    : impl_(std::make_unique<Impl>(capacity, clock, receiver)) {}

BoundedStopControlExecutor::~BoundedStopControlExecutor() = default;

std::size_t BoundedStopControlExecutor::capacity() const noexcept {
  return impl_->capacity();
}

bool BoundedStopControlExecutor::prequalified_ready() const noexcept {
  return impl_->prequalified_ready();
}

void BoundedStopControlExecutor::Publish(const StopControlWorkToken token) noexcept {
  impl_->Publish(token);
}

void BoundedStopControlExecutor::ArmDeadline(const StopControlWorkToken token,
                                             const TimePoint deadline) noexcept {
  impl_->ArmDeadline(token, deadline);
}

void BoundedStopControlExecutor::CancelDeadline(const StopControlWorkToken token) noexcept {
  impl_->CancelDeadline(token);
}

void BoundedStopControlExecutor::CloseIngress() noexcept {
  impl_->CloseIngress();
}

bool BoundedStopControlExecutor::DrainFor(const std::chrono::nanoseconds timeout) noexcept {
  return impl_->DrainFor(timeout);
}

}  // namespace manyforge::core
