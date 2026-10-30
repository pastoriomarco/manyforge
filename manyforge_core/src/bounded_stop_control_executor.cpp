#include <algorithm>
#include <array>
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

  void WakeForClockAdvanceTest() noexcept { WakeWorker(); }

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
      if (token.generation >= slot.writer_generation) {
        slot.writer_generation = token.generation;
        PublishDeadline(slot, Deadline{token.generation, deadline, true});
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
      if (slot.writer_generation == token.generation) {
        PublishDeadline(slot, Deadline{token.generation, 0, false});
        cancelled = true;
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
  struct Deadline final {
    std::uint64_t generation{};
    TimePoint at{};
    bool armed{};
  };

  static constexpr unsigned kIndexMask = 3U;
  static constexpr unsigned kDirty = 4U;
  static constexpr unsigned kActive = 8U;
  static constexpr unsigned kWakePending = 1U;
  static constexpr unsigned kQuiescent = 2U;

  struct Slot final {
    std::atomic<std::uint64_t> requested_generation{0U};
    std::atomic<std::uint64_t> delivered_generation{0U};
    // One serialized producer and one worker exchange the latest scalar
    // deadline. Each owns a buffer; the third is the exchange slot. No reader
    // can see a partially replaced pair, and no publication counter can wrap.
    // This is a capacity-one handoff, not a queue of past deadlines.
    std::array<Deadline, 3U> deadlines{};
    unsigned writer_index{0U};
    unsigned reader_index{1U};
    std::uint64_t writer_generation{};
    std::atomic<unsigned> middle{2U};
  };

  static void PublishDeadline(Slot& slot, Deadline deadline) noexcept {
    slot.deadlines[slot.writer_index] = deadline;
    slot.writer_index =
        slot.middle.exchange(slot.writer_index | kDirty | kActive, std::memory_order_acq_rel) &
        kIndexMask;
  }

  [[nodiscard]] bool Valid(const StopControlWorkToken token) const noexcept {
    return token.locally_valid() && token.slot < capacity_;
  }

  void EnterApi() noexcept { api_calls_inflight_.fetch_add(1U, std::memory_order_acq_rel); }

  void LeaveApi() noexcept {
    api_calls_inflight_.fetch_sub(1U, std::memory_order_acq_rel);
    NotifyDrainState();
  }

  void WakeWorker() noexcept {
    // Publication invalidates the worker's idle acknowledgement in the same
    // atomic handoff that coalesces semaphore wakes.
    if ((wake_state_.exchange(kWakePending, std::memory_order_acq_rel) & kWakePending) == 0U) {
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
    return wake_state_.load(std::memory_order_acquire) == kQuiescent;
  }

  // Only the worker audits slots: a callback can transfer pending work from
  // a later slot to an earlier one, defeating a concurrent caller's scan.
  void AcknowledgeQuiescence() noexcept {
    for (std::size_t index = 0; index < capacity_; ++index) {
      const auto& slot = slots_[index];
      if (slot.requested_generation.load(std::memory_order_acquire) >
              slot.delivered_generation.load(std::memory_order_acquire) ||
          (slot.middle.load(std::memory_order_acquire) & kActive) != 0U) {
        return;
      }
    }
    unsigned expected = 0U;
    static_cast<void>(wake_state_.compare_exchange_strong(
        expected, kQuiescent, std::memory_order_acq_rel, std::memory_order_acquire));
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
      if ((slot.middle.load(std::memory_order_acquire) & kDirty) != 0U) {
        slot.reader_index =
            slot.middle.exchange(slot.reader_index | kActive, std::memory_order_acq_rel) &
            kIndexMask;
      }
      auto& deadline = slot.deadlines[slot.reader_index];
      const auto requested = slot.requested_generation.load(std::memory_order_acquire);
      const auto delivered = slot.delivered_generation.load(std::memory_order_acquire);
      bool due = false;
      if (deadline.armed && requested > deadline.generation) {
        deadline.armed = false;
      } else if (deadline.armed && requested == deadline.generation) {
        // Arm-before-Publish stays dormant until its generation is visible.
        // Claim only the current clean publication. A concurrent Cancel/rearm
        // marks middle dirty and prevents claiming this older deadline, even
        // when it uses the identical generation and timestamp.
        if (deadline.at <= clock_.now()) {
          auto publication = slot.middle.load(std::memory_order_acquire);
          if ((publication & kDirty) == 0U &&
              slot.middle.compare_exchange_strong(
                  publication, publication, std::memory_order_acq_rel, std::memory_order_acquire)) {
            due = true;
            deadline.armed = false;
          }
        } else if (!nearest_deadline.has_value() || deadline.at < *nearest_deadline) {
          nearest_deadline = deadline.at;
        }
      }

      // Initial progress covers only a deadline claimed BEFORE the callback.
      // A rearm during that callback stays dirty for the next scan; a future
      // deadline becoming due during it likewise remains armed.
      if (requested > delivered) {
        Invoke(StopControlWorkToken{static_cast<std::uint32_t>(index), requested});
        slot.delivered_generation.store(requested, std::memory_order_release);
      } else if (due) {
        Invoke(StopControlWorkToken{static_cast<std::uint32_t>(index), deadline.generation});
      }

      if (!deadline.armed) {
        // Clear drain accounting only after the claimed callback returned,
        // and only if no producer has published another pending deadline.
        auto publication = slot.middle.load(std::memory_order_acquire);
        if ((publication & kDirty) == 0U) {
          static_cast<void>(slot.middle.compare_exchange_strong(publication, publication & ~kActive,
                                                                std::memory_order_acq_rel,
                                                                std::memory_order_acquire));
        }
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
      AcknowledgeQuiescence();
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
        // to the next scan or observes clear state and posts the next semaphore wake.
        wake_state_.store(0U, std::memory_order_release);
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
  std::atomic<unsigned> wake_state_{0U};
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

void BoundedStopControlExecutor::WakeForClockAdvanceTest() noexcept {
  impl_->WakeForClockAdvanceTest();
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
