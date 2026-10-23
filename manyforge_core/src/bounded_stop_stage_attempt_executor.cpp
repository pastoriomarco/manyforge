#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <manyforge_core/bounded_stop_stage_attempt_executor.hpp>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {

class BoundedStopStageAttemptExecutor::Impl final {
 public:
  Impl(const std::size_t capacity, const std::size_t prequalified_max_live_participants,
       IStopStageAttemptCompletionReceiver& receiver)
      : capacity_(capacity),
        prequalified_max_live_participants_(prequalified_max_live_participants),
        receiver_(receiver) {
    if (prequalified_max_live_participants == 0U ||
        prequalified_max_live_participants >
            kMaximumPrequalifiedStopStageAttemptLanes / kMaxStopEscalationStages ||
        capacity != prequalified_max_live_participants * kMaxStopEscalationStages ||
        capacity > kMaximumPrequalifiedStopStageAttemptLanes) {
      throw std::invalid_argument(
          "Stop-stage attempt capacity must equal the prequalified live "
          "participant bound times eight stages");
    }
    slots_.reserve(capacity_);
    for (std::size_t index = 0U; index < capacity_; ++index) {
      slots_.push_back(std::make_unique<Slot>());
    }
    try {
      for (std::size_t index = 0U; index < capacity_; ++index) {
        auto& slot = slots_[index];
        slot->worker = std::jthread([this, pointer = slot.get(), index](std::stop_token token) {
          Run(*pointer, static_cast<std::uint32_t>(index), token);
        });
      }
    } catch (...) {
      accepting_.store(false, std::memory_order_release);
      for (auto& slot : slots_) {
        {
          std::lock_guard lock(slot->mutex);
          slot->worker.request_stop();
        }
        slot->changed.notify_all();
      }
      for (auto& slot : slots_) {
        if (slot->worker.joinable()) {
          slot->worker.join();
        }
      }
      throw;
    }
    auto ready = ready_workers_.load(std::memory_order_acquire);
    while (ready != capacity_) {
      ready_workers_.wait(ready, std::memory_order_acquire);
      ready = ready_workers_.load(std::memory_order_acquire);
    }
  }

  ~Impl() {
    if (!drain_confirmed_) {
      std::terminate();
    }
    for (auto& slot : slots_) {
      // Synchronize stop publication with the mutex protecting the wait
      // predicate so shutdown cannot lose its only notification.
      {
        std::lock_guard lock(slot->mutex);
        slot->worker.request_stop();
      }
      slot->changed.notify_all();
    }
    for (auto& slot : slots_) {
      if (slot->worker.joinable()) {
        slot->worker.join();
      }
    }
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] std::size_t prequalified_max_live_participants() const noexcept {
    return prequalified_max_live_participants_;
  }

  [[nodiscard]] bool prequalified_ready() const noexcept {
    return accepting_.load(std::memory_order_acquire) &&
           replenishment_healthy_.load(std::memory_order_acquire) &&
           ready_workers_.load(std::memory_order_acquire) == capacity_;
  }

  [[nodiscard]] StopStageAttemptStartResult TryStart(StopStageAttemptRequest request) noexcept {
    if (request.port == nullptr || !request.stage || !request.budget) {
      return {StopStageAttemptStartStatus::kInvalidRequest, std::nullopt};
    }
    if (!accepting_.load(std::memory_order_acquire)) {
      return {StopStageAttemptStartStatus::kClosed, std::nullopt};
    }
    if (!prequalified_ready()) {
      return {StopStageAttemptStartStatus::kNotPrequalified, std::nullopt};
    }
    for (std::size_t index = 0U; index < capacity_; ++index) {
      auto& slot = *slots_[index];
      std::lock_guard lock(slot.mutex);
      if (slot.state != State::kFree) {
        continue;
      }
      if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
        replenishment_healthy_.store(false, std::memory_order_release);
        return {StopStageAttemptStartStatus::kNotPrequalified, std::nullopt};
      }
      ++slot.generation;
      slot.request = std::move(request);
      slot.infrastructure_failure = false;
      slot.result.reset();
      slot.spring_return_point.reset();
      slot.state = State::kQueued;
      const StopStageAttemptHandle handle{static_cast<std::uint32_t>(index), slot.generation};
      slot.changed.notify_one();
      return {StopStageAttemptStartStatus::kStarted, handle};
    }
    return {StopStageAttemptStartStatus::kCapacityExhausted, std::nullopt};
  }

  [[nodiscard]] bool RequestCancel(const StopStageAttemptHandle handle) noexcept {
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return false;
    }
    std::lock_guard lock(slot->mutex);
    if (slot->generation != handle.generation || slot->state == State::kFree) {
      return false;
    }
    slot->cancellation.request_stop();
    return true;
  }

  [[nodiscard]] StopStageAttemptTakeResult TryTake(const StopStageAttemptHandle handle) noexcept {
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return {StopStageAttemptTakeStatus::kStaleHandle, std::nullopt};
    }
    std::lock_guard lock(slot->mutex);
    if (slot->generation != handle.generation || slot->state == State::kFree) {
      return {StopStageAttemptTakeStatus::kStaleHandle, std::nullopt};
    }
    if (slot->state == State::kQueued || slot->state == State::kRunning) {
      return {StopStageAttemptTakeStatus::kPending, std::nullopt};
    }
    if (slot->state == State::kTaken) {
      return {StopStageAttemptTakeStatus::kAlreadyTaken, std::nullopt};
    }
    slot->state = State::kTaken;
    if (slot->infrastructure_failure) {
      return {StopStageAttemptTakeStatus::kInfrastructureFailure, std::nullopt};
    }
    return {StopStageAttemptTakeStatus::kCompleted,
            std::optional<ParticipantStopStageResultDTO>(std::move(*slot->result))};
  }

  [[nodiscard]] std::shared_ptr<const SpringBackReturnPointDTO> ReadSpringReturnPoint(
      const StopStageAttemptHandle handle) noexcept {
    auto* slot = Resolve(handle);
    if (slot == nullptr) return {};
    std::lock_guard lock(slot->mutex);
    if (slot->generation != handle.generation || slot->state == State::kFree ||
        slot->cancellation.stop_requested()) return {};
    return slot->spring_return_point;
  }

  [[nodiscard]] StopStageAttemptReleaseStatus Release(
      const StopStageAttemptHandle handle) noexcept {
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return StopStageAttemptReleaseStatus::kStaleHandle;
    }
    try {
      std::stop_source fresh_cancellation;
      {
        std::lock_guard lock(slot->mutex);
        if (slot->generation != handle.generation || slot->state == State::kFree) {
          return StopStageAttemptReleaseStatus::kStaleHandle;
        }
        if (slot->state != State::kTaken) {
          return StopStageAttemptReleaseStatus::kNotTaken;
        }
        slot->request = {};
        slot->result.reset();
        slot->spring_return_point.reset();
        slot->cancellation = std::move(fresh_cancellation);
        slot->infrastructure_failure = false;
        slot->state = State::kFree;
      }
      SignalStateChange();
      return StopStageAttemptReleaseStatus::kReleased;
    } catch (...) {
      replenishment_healthy_.store(false, std::memory_order_release);
      return StopStageAttemptReleaseStatus::kReplenishmentFailed;
    }
  }

  void CloseIngress() noexcept {
    accepting_.store(false, std::memory_order_release);
    for (auto& slot : slots_) {
      // Handshake with the mutex protecting the worker wait predicate so a
      // close cannot land between the predicate check and sleeping.
      {
        std::lock_guard lock(slot->mutex);
        slot->changed.notify_all();
      }
    }
    SignalStateChange();
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    if (timeout < std::chrono::nanoseconds::zero() || accepting_.load(std::memory_order_acquire)) {
      return false;
    }
    try {
      std::unique_lock lock(state_mutex_);
      const bool drained = state_changed_.wait_for(lock, timeout, [this] { return Drained(); });
      if (drained) {
        drain_confirmed_ = true;
      }
      return drained;
    } catch (...) {
      return false;
    }
  }

 private:
  enum class State : std::uint8_t { kFree = 0, kQueued, kRunning, kComplete, kTaken };

  struct Slot final {
    std::mutex mutex;
    std::condition_variable changed;
    State state{State::kFree};
    std::uint64_t generation{};
    StopStageAttemptRequest request;
    std::stop_source cancellation;
    std::optional<ParticipantStopStageResultDTO> result;
    std::shared_ptr<const SpringBackReturnPointDTO> spring_return_point;
    bool infrastructure_failure{};
    std::jthread worker;
  };

  [[nodiscard]] Slot* Resolve(const StopStageAttemptHandle handle) noexcept {
    return handle.locally_valid() && handle.slot < capacity_ ? slots_[handle.slot].get() : nullptr;
  }

  [[nodiscard]] bool PublishSpringReturnPoint(
      Slot& slot, const StopStageAttemptHandle handle,
      const SpringBackReturnPointDTO& point) noexcept {
    try {
      auto immutable = std::make_shared<const SpringBackReturnPointDTO>(point);
      {
        std::lock_guard lock(slot.mutex);
        if (slot.generation != handle.generation || slot.state != State::kRunning ||
            !slot.request.force_spring_return || slot.cancellation.stop_requested() ||
            slot.spring_return_point) return false;
        slot.spring_return_point = std::move(immutable);
      }
      receiver_.StageAttemptCompleted(handle);
      return true;
    } catch (...) {
      // Progress is optional. Containment continues on the same stage lane.
      return false;
    }
  }

  class SpringObserver final : public IStopStageSpringReturnObserver {
   public:
    SpringObserver(Impl& owner, Slot& slot, StopStageAttemptHandle handle)
        : owner_(owner), slot_(slot), handle_(handle) {}
    [[nodiscard]] bool SpringReturnPointReady(
        const SpringBackReturnPointDTO& point) noexcept override {
      return owner_.PublishSpringReturnPoint(slot_, handle_, point);
    }
   private:
    Impl& owner_;
    Slot& slot_;
    StopStageAttemptHandle handle_;
  };

  [[nodiscard]] bool Drained() noexcept {
    if (active_calls_.load(std::memory_order_acquire) != 0U) {
      return false;
    }
    for (auto& candidate : slots_) {
      std::lock_guard lock(candidate->mutex);
      if (candidate->state == State::kQueued || candidate->state == State::kRunning) {
        return false;
      }
    }
    return true;
  }

  void SignalStateChange() noexcept {
    std::lock_guard lock(state_mutex_);
    state_changed_.notify_all();
  }

  void Run(Slot& slot, const std::uint32_t slot_index, const std::stop_token worker_stop) noexcept {
    ready_workers_.fetch_add(1U, std::memory_order_acq_rel);
    ready_workers_.notify_all();
    SignalStateChange();
    while (!worker_stop.stop_requested()) {
      StopStageAttemptRequest request;
      std::stop_token cancellation;
      {
        std::unique_lock lock(slot.mutex);
        slot.changed.wait(lock, [&] {
          return worker_stop.stop_requested() || slot.state == State::kQueued ||
                 (!accepting_.load(std::memory_order_acquire) && slot.state == State::kFree);
        });
        if (worker_stop.stop_requested() ||
            (!accepting_.load(std::memory_order_acquire) && slot.state == State::kFree)) {
          break;
        }
        request = slot.request;
        cancellation = slot.cancellation.get_token();
        slot.state = State::kRunning;
        active_calls_.fetch_add(1U, std::memory_order_acq_rel);
      }

      std::optional<ParticipantStopStageResultDTO> result;
      bool infrastructure_failure = false;
      try {
        auto budget = *request.budget;
        if (request.stop_ingress_clock) {
          budget.stop_ingress_age_ns = std::max<Duration>(
              0, request.stop_ingress_clock->now() - request.stop_ingress_at_ns);
        }
        if (request.force_spring_return) {
          SpringObserver observer(*this, slot, {slot_index, slot.generation});
          result.emplace(request.port->TranslateStopStageForTakeover(
              *request.stage, budget, cancellation, observer));
        } else {
          result.emplace(
              request.port->TranslateStopStage(*request.stage, budget, cancellation));
        }
      } catch (...) {
        infrastructure_failure = true;
      }
      StopStageAttemptHandle completed_handle;
      {
        std::lock_guard lock(slot.mutex);
        slot.result = std::move(result);
        slot.infrastructure_failure = infrastructure_failure;
        slot.state = State::kComplete;
        completed_handle = StopStageAttemptHandle{slot_index, slot.generation};
      }
      SignalStateChange();
      receiver_.StageAttemptCompleted(completed_handle);
      active_calls_.fetch_sub(1U, std::memory_order_acq_rel);
      SignalStateChange();
    }
    ready_workers_.fetch_sub(1U, std::memory_order_acq_rel);
    ready_workers_.notify_all();
    SignalStateChange();
  }

  const std::size_t capacity_;
  const std::size_t prequalified_max_live_participants_;
  IStopStageAttemptCompletionReceiver& receiver_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::atomic<bool> accepting_{true};
  std::atomic<bool> replenishment_healthy_{true};
  std::atomic<std::size_t> ready_workers_{0U};
  std::atomic<std::size_t> active_calls_{0U};
  std::mutex state_mutex_;
  std::condition_variable state_changed_;
  bool drain_confirmed_{};
};

BoundedStopStageAttemptExecutor::BoundedStopStageAttemptExecutor(
    const std::size_t capacity, const std::size_t prequalified_max_live_participants,
    IStopStageAttemptCompletionReceiver& receiver)
    : impl_(std::make_unique<Impl>(capacity, prequalified_max_live_participants, receiver)) {}

BoundedStopStageAttemptExecutor::~BoundedStopStageAttemptExecutor() = default;

std::size_t BoundedStopStageAttemptExecutor::capacity() const noexcept {
  return impl_->capacity();
}

std::size_t BoundedStopStageAttemptExecutor::prequalified_max_live_participants() const noexcept {
  return impl_->prequalified_max_live_participants();
}

bool BoundedStopStageAttemptExecutor::prequalified_ready() const noexcept {
  return impl_->prequalified_ready();
}

StopStageAttemptStartResult BoundedStopStageAttemptExecutor::TryStart(
    StopStageAttemptRequest request) noexcept {
  return impl_->TryStart(std::move(request));
}

bool BoundedStopStageAttemptExecutor::RequestCancel(const StopStageAttemptHandle handle) noexcept {
  return impl_->RequestCancel(handle);
}

StopStageAttemptTakeResult BoundedStopStageAttemptExecutor::TryTake(
    const StopStageAttemptHandle handle) noexcept {
  return impl_->TryTake(handle);
}

std::shared_ptr<const SpringBackReturnPointDTO>
BoundedStopStageAttemptExecutor::ReadSpringReturnPoint(
    const StopStageAttemptHandle handle) noexcept {
  return impl_->ReadSpringReturnPoint(handle);
}

StopStageAttemptReleaseStatus BoundedStopStageAttemptExecutor::Release(
    const StopStageAttemptHandle handle) noexcept {
  return impl_->Release(handle);
}

void BoundedStopStageAttemptExecutor::CloseIngress() noexcept {
  impl_->CloseIngress();
}

bool BoundedStopStageAttemptExecutor::DrainFor(const std::chrono::nanoseconds timeout) noexcept {
  return impl_->DrainFor(timeout);
}

}  // namespace manyforge::core
