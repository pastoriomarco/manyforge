#include <atomic>
#include <condition_variable>
#include <limits>
#include <manyforge_core/bounded_operation_control_executor.hpp>
#include <mutex>
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
  Impl(std::vector<IResolvedParticipantExecutionEngine*> engines,
       IOperationControlEffectCompletionReceiver& receiver)
      : receiver_(receiver) {
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
      throw;
    }
    std::unique_lock lock(state_mutex_);
    state_changed_.wait(lock, [this] { return ready_workers_ == slots_.size(); });
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
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  [[nodiscard]] bool prequalified_ready() const noexcept {
    return accepting_.load(std::memory_order_acquire) &&
           ready_workers_.load(std::memory_order_acquire) == slots_.size();
  }

  [[nodiscard]] OperationControlEffectStartResult TryStart(
      OperationControlEffectRequest request) noexcept {
    if (request.engine == nullptr || request.operation_id.empty() ||
        request.participant_id.empty() || !request.operation_revision.nonzero() ||
        (request.control != OperationControlKind::kPause &&
         request.control != OperationControlKind::kResume &&
         request.control != OperationControlKind::kReset &&
         request.control != OperationControlKind::kAcknowledgeCompleted) ||
        (request.control == OperationControlKind::kResume &&
         (request.prepared_engine == nullptr || !request.prepared_resume ||
          !request.cancellation_requested)) ||
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
    if (!prequalified_ready()) {
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
    auto* slot = Resolve(handle);
    if (slot == nullptr) {
      return OperationControlEffectReleaseStatus::kStaleHandle;
    }
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
    SignalStateChange();
    return OperationControlEffectReleaseStatus::kReleased;
  }

  void CloseIngress() noexcept {
    accepting_.store(false, std::memory_order_release);
    for (auto& slot : slots_) {
      slot->changed.notify_all();
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

  void SignalStateChange() noexcept { state_changed_.notify_all(); }

  void Run(Slot& slot, const std::uint32_t index, const std::stop_token worker_stop) noexcept {
    ready_workers_.fetch_add(1U, std::memory_order_acq_rel);
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
          case OperationControlKind::kPause: {
            auto value = request->engine->pause();
            result.accepted = value.accepted;
            result.error = std::move(value.error);
            break;
          }
          case OperationControlKind::kResume: {
            if (request->cancellation_requested->load(std::memory_order_acquire)) {
              result.error =
                  ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                           "Prepared Resume was superseded before subordinate preparation",
                           {{"reason", "prepared_resume_superseded_before_prepare"}}};
              result.prepared_resume_cleanup_complete = true;
              break;
            }
            auto value = request->prepared_engine->PrepareResumeMotion(request->subordinate_id,
                                                                       *request->prepared_resume);
            result.prepared_resume_created = value.ok();
            if (!value.ok()) {
              // The exact preparation API must not retain a dispatchable
              // product when it reports failure.
              result.prepared_resume_cleanup_complete = true;
            }
            if (value.ok() && request->cancellation_requested->load(std::memory_order_acquire)) {
              auto cancelled =
                  request->prepared_engine->CancelPreparedResumeMotion(request->subordinate_id);
              if (!cancelled.ok()) {
                value = Result<void>::Err(cancelled.error());
              } else {
                result.prepared_resume_cleanup_complete = true;
                value = Result<void>::Err(
                    ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                             "Prepared Resume was superseded during subordinate preparation",
                             {{"reason", "prepared_resume_superseded_during_prepare"}}});
              }
            }
            result.accepted = value.ok();
            if (!value.ok()) {
              result.error = value.error();
            }
            break;
          }
          case OperationControlKind::kAcknowledgeCompleted: {
            auto value =
                request->engine->acknowledge_completed(request->subordinate_id, std::nullopt);
            result.accepted = value.accepted;
            result.error = std::move(value.error);
            break;
          }
          case OperationControlKind::kReset: {
            if (request->payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
              auto value =
                  request->engine->reset_gripper(request->command_target, request->subordinate_id,
                                                 request->subordinate_generation);
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
      receiver_.OperationControlEffectCompleted(handle);
      active_calls_.fetch_sub(1U, std::memory_order_acq_rel);
      SignalStateChange();
    }
    ready_workers_.fetch_sub(1U, std::memory_order_acq_rel);
    SignalStateChange();
  }

  IOperationControlEffectCompletionReceiver& receiver_;
  std::vector<std::unique_ptr<Slot>> slots_;
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
    : impl_(std::make_unique<Impl>(std::move(engines), receiver)) {}

BoundedOperationControlExecutor::~BoundedOperationControlExecutor() = default;

std::size_t BoundedOperationControlExecutor::capacity() const noexcept {
  return impl_->capacity();
}

bool BoundedOperationControlExecutor::prequalified_ready() const noexcept {
  return impl_->prequalified_ready();
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
