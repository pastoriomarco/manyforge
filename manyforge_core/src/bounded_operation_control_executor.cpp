#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <manyforge_core/bounded_operation_control_executor.hpp>
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO infrastructure_error() {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                  "subordinate operation-control effect threw",
                  {{"reason", "operation_control_effect_threw"}}};
}

}  // namespace

class BoundedOperationControlExecutor::Impl final {
 public:
  Impl(BoundedOperationControlExecutor& owner,
       std::vector<IResolvedParticipantExecutionEngine*> engines,
       IOperationControlEffectCompletionReceiver& receiver)
      : owner_(owner), receiver_(receiver) {
    if (engines.empty() || engines.size() > kMaximumOperationControlLanes) {
      throw std::invalid_argument("operation-control executor engine bound is invalid");
    }
    slots_.reserve(engines.size());
    for (auto* engine : engines) {
      if (engine == nullptr) {
        throw std::invalid_argument("operation-control executor has a null engine");
      }
      for (const auto& existing : slots_) {
        if (existing->engine == engine) {
          throw std::invalid_argument("operation-control executor engine is duplicated");
        }
      }
      auto slot = std::make_unique<Slot>();
      slot->engine = engine;
      slots_.push_back(std::move(slot));
    }
    active_slots_ = slots_.size();
    try {
      for (std::size_t index = 0U; index < slots_.size(); ++index) {
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
    while (ready != slots_.size()) {
      ready_workers_.wait(ready, std::memory_order_acquire);
      ready = ready_workers_.load(std::memory_order_acquire);
    }
  }

  ~Impl() {
    if (!drain_confirmed_) {
      // An owner that never drained (e.g. its constructor threw after this
      // executor was built) leaves the drain to the destructor.
      CloseIngress();
      WaitForDrain("operation_control_executor.unconfirmed_destruction", std::chrono::seconds(5),
                   [this](std::chrono::nanoseconds bound) { return DrainFor(bound); });
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

  [[nodiscard]] std::size_t capacity() const noexcept {
    std::shared_lock lock(slots_mutex_);
    return active_slots_;
  }
  [[nodiscard]] bool has_active_calls() const noexcept {
    return active_calls_.load(std::memory_order_acquire) != 0U;
  }

  [[nodiscard]] bool prequalified_ready() const noexcept {
    std::shared_lock lock(slots_mutex_);
    return accepting_.load(std::memory_order_acquire) &&
           ready_workers_.load(std::memory_order_acquire) == active_slots_;
  }

  [[nodiscard]] OperationControlLaneUpdateStatus ReconfigureEngines(
      const std::vector<IResolvedParticipantExecutionEngine*>& engines) noexcept {
    if (engines.empty() || engines.size() > kMaximumOperationControlLanes ||
        std::any_of(engines.begin(), engines.end(), [](auto* engine) { return engine == nullptr; })) {
      return OperationControlLaneUpdateStatus::kInvalid;
    }
    for (std::size_t index = 0; index < engines.size(); ++index) {
      if (std::find(engines.begin(), engines.begin() + index, engines[index]) !=
          engines.begin() + index) {
        return OperationControlLaneUpdateStatus::kInvalid;
      }
    }
    try {
      std::unique_lock slots_lock(slots_mutex_);
      if (!accepting_.load(std::memory_order_acquire)) {
        return OperationControlLaneUpdateStatus::kClosed;
      }
      if (has_active_calls()) {
        return OperationControlLaneUpdateStatus::kBusy;
      }
      for (const auto& slot : slots_) {
        std::lock_guard lock(slot->mutex);
        if (slot->state != State::kFree) {
          return OperationControlLaneUpdateStatus::kBusy;
        }
      }

      std::vector<std::size_t> retired;
      std::vector<std::size_t> vacant;
      for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto* engine = slots_[index]->engine;
        if (engine == nullptr) {
          vacant.push_back(index);
        } else if (std::find(engines.begin(), engines.end(), engine) == engines.end()) {
          retired.push_back(index);
          vacant.push_back(index);
        }
      }
      std::vector<std::pair<std::size_t, std::unique_ptr<Slot>>> additions;
      additions.reserve(engines.size());
      std::size_t vacant_index = 0;
      std::size_t next_index = slots_.size();
      for (auto* engine : engines) {
        const bool retained = std::any_of(slots_.begin(), slots_.end(),
                                          [&](const auto& slot) { return slot->engine == engine; });
        if (retained) continue;
        const auto index = vacant_index < vacant.size() ? vacant[vacant_index++] : next_index++;
        auto slot = std::make_unique<Slot>();
        slot->engine = engine;
        if (index < slots_.size()) slot->generation = slots_[index]->generation;
        additions.emplace_back(index, std::move(slot));
      }
      slots_.reserve(next_index);
      const auto ready_before = ready_workers_.load(std::memory_order_acquire);
      try {
        for (auto& [index, slot] : additions) {
          slot->worker = std::jthread([this, pointer = slot.get(), index](std::stop_token token) {
            Run(*pointer, static_cast<std::uint32_t>(index), token);
          });
        }
      } catch (...) {
        for (auto& [index, slot] : additions) {
          (void)index;
          if (slot->worker.joinable()) {
            { std::lock_guard lock(slot->mutex); slot->worker.request_stop(); }
            slot->changed.notify_all();
            slot->worker.join();
          }
        }
        return OperationControlLaneUpdateStatus::kCapacityUnavailable;
      }
      auto ready = ready_workers_.load(std::memory_order_acquire);
      while (ready < ready_before + additions.size()) {
        ready_workers_.wait(ready, std::memory_order_acquire);
        ready = ready_workers_.load(std::memory_order_acquire);
      }
      for (const auto index : retired) {
        auto& slot = slots_[index];
        { std::lock_guard lock(slot->mutex); slot->worker.request_stop(); }
        slot->changed.notify_all();
        if (slot->worker.joinable()) slot->worker.join();
        slot->engine = nullptr;
      }
      for (auto& [index, slot] : additions) {
        if (index == slots_.size()) {
          slots_.push_back(std::move(slot));
        } else {
          slots_[index] = std::move(slot);
        }
      }
      active_slots_ = engines.size();
      return OperationControlLaneUpdateStatus::kApplied;
    } catch (...) {
      return OperationControlLaneUpdateStatus::kCapacityUnavailable;
    }
  }

  [[nodiscard]] OperationControlEffectStartResult TryStart(
      OperationControlEffectRequest request) noexcept {
    std::shared_lock slots_lock(slots_mutex_);
    if (request.engine == nullptr || request.operation_id.empty() ||
        request.participant_id.empty() || !request.operation_revision.nonzero() ||
        (request.control != OperationControlKind::kReset &&
         request.control != OperationControlKind::kAcknowledgeCompleted) ||
        ((request.control == OperationControlKind::kReset ||
          request.control == OperationControlKind::kAcknowledgeCompleted) &&
         request.subordinate_id == 0U) ||
        (request.control == OperationControlKind::kReset &&
         request.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
         (request.command_target.empty() || request.subordinate_generation == 0U))) {
      return {OperationControlEffectStartStatus::kInvalidRequest, std::nullopt};
    }
    if (!accepting_.load(std::memory_order_acquire)) {
      return {OperationControlEffectStartStatus::kClosed, std::nullopt};
    }
    if (ready_workers_.load(std::memory_order_acquire) != active_slots_) {
      return {OperationControlEffectStartStatus::kNotPrequalified, std::nullopt};
    }
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
      auto& slot = *slots_[index];
      if (slot.engine != request.engine) {
        continue;
      }
      std::lock_guard lock(slot.mutex);
      if (slot.state != State::kFree) {
        return {OperationControlEffectStartStatus::kLaneBusy, std::nullopt};
      }
      if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
        accepting_.store(false, std::memory_order_release);
        return {OperationControlEffectStartStatus::kNotPrequalified, std::nullopt};
      }
      ++slot.generation;
      slot.request = std::move(request);
      slot.result.reset();
      slot.infrastructure_failure = false;
      slot.state = State::kQueued;
      const OperationControlEffectHandle handle{static_cast<std::uint32_t>(index), slot.generation};
      slot.changed.notify_one();
      return {OperationControlEffectStartStatus::kStarted, handle};
    }
    return {OperationControlEffectStartStatus::kNotPrequalified, std::nullopt};
  }

  [[nodiscard]] OperationControlEffectTakeResult TryTake(
      const OperationControlEffectHandle handle) noexcept {
    std::shared_lock slots_lock(slots_mutex_);
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return {OperationControlEffectTakeStatus::kStaleHandle, std::nullopt};
    }
    std::lock_guard lock(slot->mutex);
    if (slot->generation != handle.generation || slot->state == State::kFree) {
      return {OperationControlEffectTakeStatus::kStaleHandle, std::nullopt};
    }
    if (slot->state == State::kQueued || slot->state == State::kRunning) {
      return {OperationControlEffectTakeStatus::kPending, std::nullopt};
    }
    if (slot->state == State::kTaken) {
      return {OperationControlEffectTakeStatus::kAlreadyTaken, std::nullopt};
    }
    slot->state = State::kTaken;
    if (!slot->result.has_value()) {
      return {OperationControlEffectTakeStatus::kInfrastructureFailure, std::nullopt};
    }
    if (slot->infrastructure_failure) {
      return {OperationControlEffectTakeStatus::kInfrastructureFailure,
              std::optional<OperationControlEffectResult>(std::move(*slot->result))};
    }
    return {OperationControlEffectTakeStatus::kCompleted,
            std::optional<OperationControlEffectResult>(std::move(*slot->result))};
  }

  [[nodiscard]] OperationControlEffectReleaseStatus Release(
      const OperationControlEffectHandle handle) noexcept {
    std::shared_lock slots_lock(slots_mutex_);
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return OperationControlEffectReleaseStatus::kStaleHandle;
    }
    {
      std::lock_guard lock(slot->mutex);
      if (slot->generation != handle.generation || slot->state == State::kFree) {
        return OperationControlEffectReleaseStatus::kStaleHandle;
      }
      if (slot->state != State::kTaken) {
        return OperationControlEffectReleaseStatus::kNotTaken;
      }
      slot->request.reset();
      slot->result.reset();
      slot->infrastructure_failure = false;
      slot->state = State::kFree;
    }
    SignalStateChange();
    return OperationControlEffectReleaseStatus::kReleased;
  }

  void CloseIngress() noexcept {
    std::shared_lock slots_lock(slots_mutex_);
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
    IResolvedParticipantExecutionEngine* engine{};
    std::mutex mutex;
    std::condition_variable changed;
    State state{State::kFree};
    std::uint64_t generation{};
    std::optional<OperationControlEffectRequest> request;
    std::optional<OperationControlEffectResult> result;
    bool infrastructure_failure{};
    std::jthread worker;
  };

  [[nodiscard]] Slot* Resolve(const OperationControlEffectHandle handle) noexcept {
    return handle.locally_valid() && handle.slot < slots_.size() ? slots_[handle.slot].get()
                                                                 : nullptr;
  }

  [[nodiscard]] bool Drained() noexcept {
    if (active_calls_.load(std::memory_order_acquire) != 0U) {
      return false;
    }
    for (auto& slot : slots_) {
      std::lock_guard lock(slot->mutex);
      if (slot->state == State::kQueued || slot->state == State::kRunning) {
        return false;
      }
    }
    return true;
  }

  void SignalStateChange() noexcept {
    std::lock_guard lock(state_mutex_);
    state_changed_.notify_all();
  }

  void Run(Slot& slot, const std::uint32_t index, const std::stop_token worker_stop) noexcept {
    ready_workers_.fetch_add(1U, std::memory_order_acq_rel);
    ready_workers_.notify_all();
    SignalStateChange();
    while (!worker_stop.stop_requested()) {
      std::optional<OperationControlEffectRequest> request;
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
        slot.state = State::kRunning;
        active_calls_.fetch_add(1U, std::memory_order_acq_rel);
      }

      OperationControlEffectResult result{request->operation_id,
                                          request->participant_id,
                                          request->operation_revision,
                                          request->control,
                                          false,
                                          std::nullopt};
      bool infrastructure_failure = false;
      try {
        switch (request->control) {
          case OperationControlKind::kAcknowledgeCompleted: {
            auto value =
                request->engine->acknowledge_completed(request->subordinate_id, std::nullopt);
            result.accepted = value.accepted;
            result.error = std::move(value.error);
            break;
          }
          case OperationControlKind::kReset: {
            if (request->payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
              auto value = request->engine->reset_capability_command(
                  request->command_target, request->subordinate_id, request->subordinate_generation);
              result.accepted = value.ok();
              if (!value.ok()) {
                result.error = value.error();
              }
            } else {
              auto value = request->engine->reset_after_certified_containment(
                  request->subordinate_id, std::nullopt);
              result.accepted = value.accepted;
              result.error = std::move(value.error);
            }
            break;
          }
        }
      } catch (...) {
        infrastructure_failure = true;
        result.error = infrastructure_error();
      }
      OperationControlEffectHandle handle;
      {
        std::lock_guard lock(slot.mutex);
        slot.result = std::move(result);
        slot.infrastructure_failure = infrastructure_failure;
        slot.state = State::kComplete;
        handle = OperationControlEffectHandle{index, slot.generation};
      }
      SignalStateChange();
      receiver_.OperationControlEffectCompleted(owner_, handle);
      active_calls_.fetch_sub(1U, std::memory_order_acq_rel);
      SignalStateChange();
    }
    ready_workers_.fetch_sub(1U, std::memory_order_acq_rel);
    ready_workers_.notify_all();
    SignalStateChange();
  }

  BoundedOperationControlExecutor& owner_;
  IOperationControlEffectCompletionReceiver& receiver_;
  mutable std::shared_mutex slots_mutex_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::size_t active_slots_{};
  std::atomic<bool> accepting_{true};
  std::atomic<std::size_t> ready_workers_{0U};
  std::atomic<std::size_t> active_calls_{0U};
  std::mutex state_mutex_;
  std::condition_variable state_changed_;
  bool drain_confirmed_{};
};

BoundedOperationControlExecutor::BoundedOperationControlExecutor(
    std::vector<IResolvedParticipantExecutionEngine*> engines,
    IOperationControlEffectCompletionReceiver& receiver)
    : impl_(std::make_unique<Impl>(*this, std::move(engines), receiver)) {}

BoundedOperationControlExecutor::~BoundedOperationControlExecutor() = default;

std::size_t BoundedOperationControlExecutor::capacity() const noexcept {
  return impl_->capacity();
}

bool BoundedOperationControlExecutor::prequalified_ready() const noexcept {
  return impl_->prequalified_ready();
}

bool BoundedOperationControlExecutor::has_active_calls() const noexcept {
  return impl_->has_active_calls();
}

OperationControlLaneUpdateStatus BoundedOperationControlExecutor::ReconfigureEngines(
    std::vector<IResolvedParticipantExecutionEngine*> engines) noexcept {
  return impl_->ReconfigureEngines(engines);
}

OperationControlEffectStartResult BoundedOperationControlExecutor::TryStart(
    OperationControlEffectRequest request) noexcept {
  return impl_->TryStart(std::move(request));
}

OperationControlEffectTakeResult BoundedOperationControlExecutor::TryTake(
    const OperationControlEffectHandle handle) noexcept {
  return impl_->TryTake(handle);
}

OperationControlEffectReleaseStatus BoundedOperationControlExecutor::Release(
    const OperationControlEffectHandle handle) noexcept {
  return impl_->Release(handle);
}

void BoundedOperationControlExecutor::CloseIngress() noexcept {
  impl_->CloseIngress();
}

bool BoundedOperationControlExecutor::DrainFor(const std::chrono::nanoseconds timeout) noexcept {
  return impl_->DrainFor(timeout);
}

}  // namespace manyforge::core
