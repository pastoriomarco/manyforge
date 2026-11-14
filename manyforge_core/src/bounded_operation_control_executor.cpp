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
      slot->index = static_cast<std::uint32_t>(slots_.size());
      slots_.push_back(std::move(slot));
    }
    active_slots_ = slots_.size();
    try {
      for (auto& slot : slots_) {
        StartWorker(*slot);
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
    for (const auto& slot : slots_) {
      WaitStarted(*slot);
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
    ReleaseLaneUpdate();
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
           std::all_of(slots_.begin(), slots_.end(), [](const auto& slot) {
             return slot->engine == nullptr ||
                    slot->phase.load(std::memory_order_acquire) == WorkerPhase::kRunning;
           });
  }

  void StageEngines(std::vector<IResolvedParticipantExecutionEngine*> engines) noexcept {
    auto status = OperationControlLaneUpdateStatus::kApplied;
    if (engines.empty() || engines.size() > kMaximumOperationControlLanes ||
        std::any_of(engines.begin(), engines.end(), [](auto* engine) { return engine == nullptr; })) {
      status = OperationControlLaneUpdateStatus::kInvalid;
    }
    for (std::size_t index = 0; status == OperationControlLaneUpdateStatus::kApplied &&
                                index < engines.size();
         ++index) {
      if (std::find(engines.begin(), engines.begin() + index, engines[index]) !=
          engines.begin() + index) {
        status = OperationControlLaneUpdateStatus::kInvalid;
      }
    }
    std::vector<std::unique_ptr<Slot>> staged;
    if (status == OperationControlLaneUpdateStatus::kApplied) {
      try {
        {
          std::shared_lock slots_lock(slots_mutex_);
          for (auto* engine : engines) {
            const bool retained = std::any_of(slots_.begin(), slots_.end(),
                                              [&](const auto& slot) { return slot->engine == engine; });
            if (retained) continue;
            auto slot = std::make_unique<Slot>();
            slot->engine = engine;
            staged.push_back(std::move(slot));
          }
        }
        for (auto& slot : staged) {
          StartWorker(*slot);
        }
      } catch (...) {
        // Reported by the commit, after the same checks as before (OD-221).
        for (auto& slot : staged) {
          StopAndJoin(*slot);
        }
        staged.clear();
        status = OperationControlLaneUpdateStatus::kCapacityUnavailable;
      }
    }
    for (const auto& slot : staged) {
      WaitStarted(*slot);
    }
    std::unique_lock slots_lock(slots_mutex_);
    stage_status_ = status;
    pending_engines_ = std::move(engines);
    staged_ = std::move(staged);
  }

  [[nodiscard]] OperationControlLaneUpdateStatus CommitStagedEngines() noexcept {
    try {
      std::unique_lock slots_lock(slots_mutex_);
      if (stage_status_ == OperationControlLaneUpdateStatus::kInvalid) {
        return OperationControlLaneUpdateStatus::kInvalid;
      }
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
      if (stage_status_ != OperationControlLaneUpdateStatus::kApplied) {
        return stage_status_;
      }

      const auto& engines = pending_engines_;
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
      // Plan every change first (lane index, staged position); the swap below
      // cannot fail.
      std::vector<std::pair<std::size_t, std::size_t>> additions;
      additions.reserve(engines.size());
      std::size_t vacant_index = 0;
      std::size_t next_index = slots_.size();
      for (auto* engine : engines) {
        const bool retained = std::any_of(slots_.begin(), slots_.end(),
                                          [&](const auto& slot) { return slot->engine == engine; });
        if (retained) continue;
        const auto staged = std::find_if(staged_.begin(), staged_.end(), [&](const auto& slot) {
          return slot != nullptr && slot->engine == engine;
        });
        if (staged == staged_.end()) {
          return OperationControlLaneUpdateStatus::kInvalid;
        }
        const auto index = vacant_index < vacant.size() ? vacant[vacant_index++] : next_index++;
        additions.emplace_back(index, static_cast<std::size_t>(staged - staged_.begin()));
      }
      // A retired lane no addition takes stays as an empty lane that keeps its
      // generation, so its old handles stay stale.
      std::vector<std::unique_ptr<Slot>> empty_lanes;
      empty_lanes.reserve(retired.size());
      for (const auto index : retired) {
        if (std::any_of(additions.begin(), additions.end(),
                        [&](const auto& addition) { return addition.first == index; })) {
          continue;
        }
        auto lane = std::make_unique<Slot>();
        lane->index = static_cast<std::uint32_t>(index);
        lane->generation = slots_[index]->generation;
        empty_lanes.push_back(std::move(lane));
      }
      slots_.reserve(next_index);
      leaving_.reserve(leaving_.size() + retired.size());

      for (const auto& [index, position] : additions) {
        auto& slot = *staged_[position];
        slot.index = static_cast<std::uint32_t>(index);
        if (index < slots_.size()) slot.generation = slots_[index]->generation;
      }
      for (const auto index : retired) {
        leaving_.push_back(std::move(slots_[index]));
      }
      for (auto& lane : empty_lanes) {
        const auto index = lane->index;
        slots_[index] = std::move(lane);
      }
      for (const auto& [index, position] : additions) {
        if (index == slots_.size()) {
          slots_.push_back(std::move(staged_[position]));
        } else {
          slots_[index] = std::move(staged_[position]);
        }
      }
      active_slots_ = engines.size();
      return OperationControlLaneUpdateStatus::kApplied;
    } catch (...) {
      return OperationControlLaneUpdateStatus::kCapacityUnavailable;
    }
  }

  void ReleaseLaneUpdate() noexcept {
    std::vector<std::unique_ptr<Slot>> leaving;
    std::vector<std::unique_ptr<Slot>> staged;
    {
      std::unique_lock slots_lock(slots_mutex_);
      leaving.swap(leaving_);
      staged.swap(staged_);
      pending_engines_.clear();
      stage_status_ = OperationControlLaneUpdateStatus::kInvalid;
    }
    for (auto* lanes : {&leaving, &staged}) {
      for (auto& slot : *lanes) {
        if (slot != nullptr) StopAndJoin(*slot);
      }
    }
  }

  [[nodiscard]] OperationControlEffectStartResult TryStart(
      OperationControlEffectRequest request) noexcept {
    std::shared_lock slots_lock(slots_mutex_);
    if (request.engine == nullptr || !request.operation_id.nonzero() ||
        !request.participant_id.nonzero() || !request.operation_revision.nonzero() ||
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
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
      auto& slot = *slots_[index];
      if (slot.engine != request.engine) {
        continue;
      }
      // The target lane's own worker (OD-221): lanes staged or leaving in a
      // lane update are not in `slots_` and do not affect other lanes.
      if (slot.phase.load(std::memory_order_acquire) != WorkerPhase::kRunning) {
        return {OperationControlEffectStartStatus::kNotPrequalified, std::nullopt};
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
  enum class WorkerPhase : std::uint8_t { kNotStarted = 0, kRunning, kExited };

  struct Slot final {
    IResolvedParticipantExecutionEngine* engine{};
    /// Position in `slots_`; set before the lane is published there.
    std::uint32_t index{};
    std::atomic<WorkerPhase> phase{WorkerPhase::kNotStarted};
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

  /// May throw std::system_error when the thread cannot be created.
  void StartWorker(Slot& slot) {
    slot.worker = std::jthread([this, pointer = &slot](std::stop_token token) { Run(*pointer, token); });
  }

  static void WaitStarted(const Slot& slot) noexcept {
    slot.phase.wait(WorkerPhase::kNotStarted, std::memory_order_acquire);
  }

  static void StopAndJoin(Slot& slot) noexcept {
    if (!slot.worker.joinable()) return;
    // Synchronize stop publication with the mutex protecting the wait
    // predicate so the stop cannot lose its only notification.
    {
      std::lock_guard lock(slot.mutex);
      slot.worker.request_stop();
    }
    slot.changed.notify_all();
    slot.worker.join();
  }

  void Run(Slot& slot, const std::stop_token worker_stop) noexcept {
    slot.phase.store(WorkerPhase::kRunning, std::memory_order_release);
    slot.phase.notify_all();
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
        handle = OperationControlEffectHandle{slot.index, slot.generation};
      }
      SignalStateChange();
      receiver_.OperationControlEffectCompleted(owner_, handle);
      active_calls_.fetch_sub(1U, std::memory_order_acq_rel);
      SignalStateChange();
    }
    slot.phase.store(WorkerPhase::kExited, std::memory_order_release);
    slot.phase.notify_all();
    SignalStateChange();
  }

  BoundedOperationControlExecutor& owner_;
  IOperationControlEffectCompletionReceiver& receiver_;
  mutable std::shared_mutex slots_mutex_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::size_t active_slots_{};
  std::atomic<bool> accepting_{true};
  std::atomic<std::size_t> active_calls_{0U};
  // The one lane update in progress (see the header), guarded by slots_mutex_:
  // kApplied once staged, kInvalid when nothing is staged.
  OperationControlLaneUpdateStatus stage_status_{OperationControlLaneUpdateStatus::kInvalid};
  std::vector<IResolvedParticipantExecutionEngine*> pending_engines_;
  std::vector<std::unique_ptr<Slot>> staged_;
  std::vector<std::unique_ptr<Slot>> leaving_;
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

void BoundedOperationControlExecutor::StageEngines(
    std::vector<IResolvedParticipantExecutionEngine*> engines) noexcept {
  impl_->StageEngines(std::move(engines));
}

OperationControlLaneUpdateStatus BoundedOperationControlExecutor::CommitStagedEngines() noexcept {
  return impl_->CommitStagedEngines();
}

void BoundedOperationControlExecutor::ReleaseLaneUpdate() noexcept {
  impl_->ReleaseLaneUpdate();
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
