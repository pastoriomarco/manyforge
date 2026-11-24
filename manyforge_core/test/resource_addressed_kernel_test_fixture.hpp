// Shared fixture for the resource-addressed Kernel tests: fakes, bindings,
// intents and the allocation counter. Included by exactly one translation
// unit per test executable (it defines the counting global operator new).
#pragma once

#include <manyforge_core/testing/request_key_fixture.hpp>
#include <manyforge_core/testing/heap_resource_addressed_kernel.hpp>
#include "../src/prepared_command_session_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/io_rule_stop_response.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#ifdef MANYFORGE_TEST_SQLITE_OPERATION_JOURNAL
#include <sqlite3.h>

#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_timing.hpp>
#endif
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <iostream>
#include <new>

// Counts this thread's allocations while enabled (rule 4: a time-critical
// path's allocation count may not grow).
namespace {
thread_local bool count_allocations = false;
thread_local std::size_t counted_allocations = 0U;
void* CountedAllocate(const std::size_t size) {
  if (count_allocations) ++counted_allocations;
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) return memory;
  throw std::bad_alloc();
}
// Out of line: see test_bounded_stop_control_executor.cpp.
[[gnu::noinline]] void CountedDeallocate(void* memory) noexcept { std::free(memory); }
template <typename Call>
std::size_t AllocationsDuring(Call&& call) {
  counted_allocations = 0U;
  count_allocations = true;
  call();
  count_allocations = false;
  return counted_allocations;
}
}  // namespace
void* operator new(const std::size_t size) { return CountedAllocate(size); }
void* operator new[](const std::size_t size) { return CountedAllocate(size); }
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (count_allocations) ++counted_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (count_allocations) ++counted_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void operator delete(void* memory) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory) noexcept { CountedDeallocate(memory); }
void operator delete(void* memory, std::size_t) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory, std::size_t) noexcept { CountedDeallocate(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { CountedDeallocate(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { CountedDeallocate(memory); }
#include "../src/resource_addressed_execution_kernel_internal.hpp"

namespace manyforge::core {

/// The lane's hard-stop completion timeout the test readiness carries (F4).
inline constexpr Duration kTestHardStopCompletionTimeoutNs = 500'000'000;

class StopControlExecutorTestAccess final {
 public:
  static void WakeAfterClockAdvance(ResourceAddressedExecutionKernel& kernel) noexcept {
    kernel.stop_control_executor_->WakeForClockAdvanceTest();
  }
  static bool SourceOwnsTerminalSlots(ResourceAddressedExecutionKernel& kernel, const OperationNumber& id) {
    std::lock_guard lock(kernel.state_mutex_);
    const auto* source = kernel.find_active_operation_locked(id);
    if (!source || source->participants.size() != 1U || source->state.participants.size() != 1U)
      return false;
    const auto& participant = source->participants.front();
    if (participant.slot_state != ResourceSlotState::kTerminal || participant.resources.empty())
      return false;
    const auto snapshot = kernel.resource_slots_->coherent_snapshot();
    return std::all_of(participant.resources.begin(), participant.resources.end(),
        [&](const auto& resource) {
          const auto slot = std::find_if(snapshot.slots.begin(), snapshot.slots.end(),
              [&](const auto& row) { return row.resource_id == resource; });
          return slot != snapshot.slots.end() && slot->state == ResourceSlotState::kTerminal &&
                 slot->owner && *slot->owner == participant.slot_owner;
        });
  }
  static std::pair<std::uint64_t, std::uint64_t> SeedRetiredUnknownTakeover(
      ResourceAddressedExecutionKernel& kernel, const OperationNumber& id) {
    std::lock_guard lock(kernel.state_mutex_);
    auto* source = kernel.find_active_operation_locked(id);
    if (!source || source->pending_motion_takeover.empty() || source->state.participants.size() != 1U ||
        source->state.state != OperationStateKind::kInterventionRequired ||
        !source->state.participants.front().terminal_fact ||
        source->state.participants.front().terminal_fact->effect != ParticipantEffectKnowledge::kEffectUncertain)
      return {};
    auto& candidate = *source->pending_motion_takeover.mapped();
    auto& row = candidate.state.participants.front();
    const ErrorDTO error{"candidate_unknown", "owner contact lost", {}};
    candidate.state.state = row.state = OperationStateKind::kInterventionRequired;
    row.terminal_error = error;
    row.terminal_fact = ParticipantExecutionFact{ParticipantExecutionFactKind::kLocalFailure,
                                                 ParticipantEffectKnowledge::kEffectUncertain, error};
    return {source->participants.front().subordinate_id,
            candidate.participants.front().preparation_id};
  }
  static void SetTakeoverBorrow(ResourceAddressedExecutionKernel& kernel,
                                const OperationNumber& id, bool active) {
    std::lock_guard lock(kernel.state_mutex_);
    auto* source = kernel.find_active_operation_locked(id);
    if (source) source->takeover_advance_in_flight = active;
  }
  /// Waits until the journal writer wrote every posted record.
  static bool DrainJournal(ResourceAddressedExecutionKernel& kernel) {
    return kernel.journal_writer_->DrainFor(std::chrono::seconds(10));
  }
  /// The journal writer queue's length (records posted, not yet taken).
  static std::size_t JournalQueueSize(ResourceAddressedExecutionKernel& kernel) {
    std::lock_guard lock(kernel.state_mutex_);
    return kernel.journal_queue_.size();
  }
  /// Holds one operation's ordinary commit gate like any ordinary writer.
  static auto HoldCommit(ResourceAddressedExecutionKernel& kernel, const OperationNumber& id) {
    return std::make_unique<ResourceAddressedExecutionKernel::OperationCommitGuard>(kernel, id);
  }
  // ---- Stop core (stop state model) access for test_stop_state_core.cpp.
  /// Runs exactly `request_stop_locked` under the Kernel lock with
  /// `count_allocations` raised around it (the test's counting allocator).
  /// `request_stop_locked` under the lock; `allocations` receives this
  /// thread's allocation count of exactly that call (the fixture's counter).
  static std::uint32_t RequestStopLocked(ResourceAddressedExecutionKernel& kernel,
                                         const StopRequest& request, StopHandoff& handoff,
                                         std::size_t& allocations) {
    std::lock_guard lock(kernel.state_mutex_);
    std::uint32_t index = 0U;
    allocations = AllocationsDuring([&] { index = kernel.request_stop_locked(request, handoff); });
    return index;
  }
  static StopHandoff& ThreadHandoff(ResourceAddressedExecutionKernel& kernel) {
    return kernel.thread_stop_handoff();
  }
  static void CompleteHandoff(ResourceAddressedExecutionKernel& kernel, StopHandoff& handoff) {
    kernel.complete_stop_handoff(handoff);
  }
  static StopEventRecord Record(ResourceAddressedExecutionKernel& kernel, std::uint32_t index) {
    std::lock_guard lock(kernel.state_mutex_);
    return kernel.stop_events_.records[index];
  }
  /// The participant's stop state (phase, attempt, lane index and generation).
  static ParticipantStopState ParticipantStop(ResourceAddressedExecutionKernel& kernel,
                                              const OperationNumber& id, std::size_t ordinal) {
    std::lock_guard lock(kernel.state_mutex_);
    const auto* operation = kernel.find_active_operation_locked(id);
    return operation ? operation->participants.at(ordinal).stop : ParticipantStopState{};
  }
  /// Empties the Stop lane free list (the bound normally excludes this).
  static std::uint16_t ExhaustStopLanes(ResourceAddressedExecutionKernel& kernel) {
    std::lock_guard lock(kernel.state_mutex_);
    const auto free = kernel.stop_lanes_.free_count;
    kernel.stop_lanes_.free_count = 0U;
    return free;
  }
  static void RestoreStopLanes(ResourceAddressedExecutionKernel& kernel, std::uint16_t free) {
    std::lock_guard lock(kernel.state_mutex_);
    kernel.stop_lanes_.free_count = free;
  }
  static std::uint32_t StoppingCount(ResourceAddressedExecutionKernel& kernel) {
    std::lock_guard lock(kernel.state_mutex_);
    return kernel.stopping_count_;
  }
  static bool RuntimeScalePending(ResourceAddressedExecutionKernel& kernel, const OperationNumber& id) {
    std::lock_guard lock(kernel.state_mutex_);
    const auto* operation = kernel.find_active_operation_locked(id);
    return operation != nullptr && operation->pending_runtime_scale.has_value();
  }
};

}  // namespace manyforge::core

// Not every test file uses every helper.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

namespace {

using namespace manyforge::core;

template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/// Speed scaling on its own lane: the value reaches the participants at once
/// and each live operation's speed step runs on the caller (OD-149; a busy
/// commit guard leaves it to the next `reconcile_subordinate_state()`); its
/// `runtime_scale_accepted` row is written behind on the journal writer.
bool drain_runtime_scale(ResourceAddressedExecutionKernel& kernel) {
  return StopControlExecutorTestAccess::DrainJournal(kernel);
}

/// Every record posted so far is written (the journal writer is idle).
bool drain_journal(ResourceAddressedExecutionKernel& kernel) {
  return StopControlExecutorTestAccess::DrainJournal(kernel);
}

/// One component's state as an adapter publishes it.
[[nodiscard]] ComponentState CellComponentState(std::string component_id,
                                                const ComponentLifecycle lifecycle,
                                                const ComponentActivity activity) {
  ComponentState state;
  state.component_id = std::move(component_id);
  state.lifecycle = lifecycle;
  state.activity = activity;
  if (lifecycle == ComponentLifecycle::kFault) {
    state.fault = Fault{FaultCode::kProtectiveStop, "protective stop", true,
                        FaultOperatorAction::kRecover, std::nullopt, std::nullopt, {}, 0};
  }
  return state;
}

class HeldSettlementCall final {
 public:
  ~HeldSettlementCall() { Release(); }
  // Wall-clock watchdogs only bound test orchestration under instrumentation.
  // Product deadlines remain controlled by FakeClock.
  auto Blocker() const {
    return [state = state_] {
      if (!state->entered_once.exchange(true)) state->entry.set_value();
      EXPECT_EQ(state->released.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    };
  }
  void Hold() { Blocker()(); }
  bool Entered() {
    return state_->entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  }
  void Release() {
    if (!state_->released_once.exchange(true)) state_->release.set_value();
  }

 private:
  struct State {
    std::promise<void> entry, release;
    std::future<void> entered{entry.get_future()};
    std::shared_future<void> released{release.get_future().share()};
    std::atomic_bool entered_once{}, released_once{};
  };
  // Worker callbacks can finish after this scope releases its gate. Own the
  // futures in the callback too, including failure-unwind and late-entry paths.
  std::shared_ptr<State> state_{std::make_shared<State>()};
};

// Test scripts name a participant's ended execution with the former
// ExecutionState terminal names. The engine reports them as the real
// participant does: its working phase plus a fact; the addressed Kernel
// derives the operation outcome.
[[nodiscard]] bool scripted_ending(ExecutionState state) noexcept {
  return state == ExecutionState::kCompleted || state == ExecutionState::kFailed ||
         state == ExecutionState::kStopped || state == ExecutionState::kInterventionRequired ||
         state == ExecutionState::kInterrupted;
}

[[nodiscard]] ParticipantExecutionFact scripted_fact(ExecutionState ending, ExecutionState phase,
                                                     std::optional<ErrorDTO> error) {
  const bool dispatched =
      phase == ExecutionState::kExecuting || phase == ExecutionState::kStopping;
  switch (ending) {
    case ExecutionState::kCompleted:
      return {ParticipantExecutionFactKind::kComputedCompletion,
              ParticipantEffectKnowledge::kEffectSettled, std::move(error)};
    case ExecutionState::kStopped:
      return dispatched ? ParticipantExecutionFact{ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                                                   ParticipantEffectKnowledge::kEffectSettled,
                                                   std::move(error)}
                        : ParticipantExecutionFact{ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                                                   ParticipantEffectKnowledge::kNoEffect,
                                                   std::move(error)};
    case ExecutionState::kInterventionRequired:
    case ExecutionState::kInterrupted:
      return {ParticipantExecutionFactKind::kLocalFailure,
              ParticipantEffectKnowledge::kEffectUncertain, std::move(error)};
    default:
      return {ParticipantExecutionFactKind::kLocalFailure,
              dispatched ? ParticipantEffectKnowledge::kEffectSettled
                         : ParticipantEffectKnowledge::kNoEffect,
              std::move(error)};
  }
}

[[nodiscard]] ExecutionStatus participant_status(ExecutionStatus scripted, ExecutionState phase,
                                                 std::optional<ErrorDTO> error) {
  if (!scripted_ending(scripted.state)) return scripted;
  scripted.fact = scripted_fact(scripted.state, phase, std::move(error));
  scripted.state = phase == ExecutionState::kStopping ? ExecutionState::kExecuting : phase;
  if (scripted.state == ExecutionState::kIdle || scripted_ending(scripted.state))
    scripted.state = ExecutionState::kValidating;
  return scripted;
}

class FakeParticipantEngine final : public IResolvedParticipantExecutionEngine,
                                    public IResolvedPreparedParticipantExecutionEngine,
                                    public IMotionPreview,
                                    public IControlDomainExecutionPort {
 public:
  FakeParticipantEngine() {
    gripper_status.state = GripperCommandState::kExecuting;
    gripper_status.gripper_id = "gripper.target";
    gripper_status.command_id = 73U;
    gripper_status.generation = 9U;
    gripper_status.revision = 1U;
    gripper_status.progress = 0.25;
  }

  Result<std::uint64_t> execute(const ExecuteRequestDTO& request) {
    ++execute_calls;
    Trace("engine:execute");
    {
      std::lock_guard lock(last_execute_mutex_);
      last_execute = request;
    }
    if (request.initial_runtime_scale.has_value()) {
      const auto initialized = set_runtime_scale(*request.initial_runtime_scale);
      if (!initialized.ok()) {
        return Result<std::uint64_t>::Err(initialized.error());
      }
    }
    {
      std::lock_guard lock(status_mutex_);
      status.state = ExecutionState::kPlanning;
      status.execution_id = next_execution_id;
    }
    if (event_during_execute.has_value()) {
      const auto snapshot = CallbackSnapshot();
      for (const auto& [id, callback] : snapshot) {
        (void)id;
        callback(*event_during_execute);
      }
      event_during_execute.reset();
    }
    if (execute_return_hook) execute_return_hook();
    return next_execute_result;
  }

  // Call (1) under the Kernel lock: the linearising part (fence id store,
  // dispatch-gate close). Call (2) after the unlock only records it.
  AddressedParticipantStopFenceResult fence_addressed_participant_stop(
      const std::uint64_t execution_id) noexcept override {
    addressed_stop_fence_execution_id.store(execution_id, std::memory_order_release);
    ++addressed_stop_fence_calls;
    Trace("engine:fence_addressed_stop");
    if (status_at_stop_fence.has_value()) {
      std::lock_guard lock(status_mutex_);
      if (!scripted_ending(status.state)) ended_phase = status.state;
      status = *status_at_stop_fence;
    }
    if (defer_prepared_dispatch_effect) {
      std::lock_guard lock(prepared_motion_mutex_);
      if (prepared_snapshot && prepared_snapshot->execution_id == execution_id &&
          prepared_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested) {
        prepared_snapshot->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
        return AddressedParticipantStopFenceResult{
            AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch,
            static_cast<std::uint8_t>(AddressedParticipantStopFenceGate::kActiveSession)};
      }
    }
    // A committed send that has not returned yet (the session worker's
    // window): the outcome is reported later (`emit_dispatch_outcome`).
    return AddressedParticipantStopFenceResult{
        AddressedParticipantStopFenceDisposition::kDispatchCommitted, 0U,
        fence_send_outcome_pending.load()};
  }

  void complete_addressed_participant_stop_fence(const std::uint64_t execution_id,
                                                 const std::uint8_t closed_gates) noexcept override {
    ++addressed_stop_fence_completions;
    stage_calls_at_fence_completion = translate_stop_stage_calls.load();
    last_fence_completion_execution_id = execution_id;
    last_fence_completion_gates = closed_gates;
    Trace("engine:complete_addressed_stop_fence");
  }

  Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) override {
    ++participant_stop_settlement_calls;
    last_settlement_prevented_before_dispatch = request.effect_prevented_before_dispatch;
    if (participant_stop_settlement_hook) {
      participant_stop_settlement_hook();
    }
    if (participant_stop_settlement_error.has_value()) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(*participant_stop_settlement_error);
    }
    const auto stop_event_id = request.stop_event_id;
    const auto& stage = request.containment.request;
    const bool expected_disposition = request.effect_prevented_before_dispatch
                                          ? request.containment.attempt_disposition ==
                                                StopStageAttemptDispositionDTO::kNotDispatched
                                          : request.containment.attempt_disposition !=
                                                StopStageAttemptDispositionDTO::kNotDispatched;
    if (request.execution_id != next_execution_id || !stop_event_id.nonzero() ||
        request.containment.outcome != ParticipantStopStageOutcomeDTO::kSucceeded ||
        !expected_disposition || stage.stop_event_id != stop_event_id) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(ErrorDTO{
          "addressed_motion_stop_execution_mismatch", "fake addressed Stop identity is stale", {}});
    }
    std::optional<ParticipantExecutionFact> terminal_fact;
    {
      std::lock_guard lock(status_mutex_);
      terminal_fact = participant_status(status, ended_phase, ended_error).fact;
      if (settle_contained_failure_as_ack) {
        if (!terminal_fact || terminal_fact->kind != ParticipantExecutionFactKind::kLocalFailure ||
            terminal_fact->effect != ParticipantEffectKnowledge::kEffectSettled ||
            !terminal_fact->error) {
          return Result<AddressedParticipantStopSettlementDTO>::Err(ErrorDTO{
              "addressed_motion_stop_state_invalid",
              "fake contained-failure ACK requires the original settled failure", {}});
        }
        status.execution_id = request.execution_id;
      } else {
        if (!scripted_ending(status.state)) ended_phase = status.state;
        status.state = ExecutionState::kStopped;
        status.execution_id = request.execution_id;
        terminal_fact.reset();
      }
    }
    recovery_assessment.assessment_revision += 1U;
    recovery_assessment.control_command_epoch += 1U;
    recovery_assessment.execution_id = request.execution_id;
    recovery_assessment.execution_state = settle_contained_failure_as_ack
                                              ? ExecutionState::kFailed
                                              : ExecutionState::kStopped;
    recovery_assessment.recovery_class = settle_contained_failure_as_ack
                                             ? RecoveryClass::kUnknownFault
                                             : RecoveryClass::kOrdinaryOperatorStop;
    recovery_assessment.status = settle_contained_failure_as_ack
                                     ? RecoveryAssessmentStatus::kActionRequired
                                     : RecoveryAssessmentStatus::kRetryReady;
    recovery_assessment.retry_admissible = !settle_contained_failure_as_ack;
    if (settle_contained_failure_as_ack) recovery_assessment.eligible_close = true;
    recovery_assessment.stop_event_id = stop_event_id.value();
    recovery_assessment.backend_stop_confirmed = true;
    recovery_assessment.allowed_actions = settle_contained_failure_as_ack
                                              ? std::vector<RecoveryAction>{RecoveryAction::kResetCycle,
                                                                           RecoveryAction::kCloseOperation}
                                              : std::vector<RecoveryAction>{
                                                    RecoveryAction::kReplanFromStoppedState,
                                                    RecoveryAction::kResetCycle};
    auto settlement = AddressedParticipantStopSettlementDTO{
        request.execution_id,     stop_event_id,
        stage.operation_id,       stage.participant_id,
        stage.controller_goal_id,
        recovery_assessment,
        settle_contained_failure_as_ack && addressed_stop_terminal_fact_override
            ? addressed_stop_terminal_fact_override
            : std::move(terminal_fact)};
    if (return_mismatched_participant_stop_settlement) {
      settlement.controller_goal_id.generation = stage.controller_goal_id.generation + 1U;
    }
    if (supports_motion_replacement && !return_mismatched_participant_stop_settlement) {
      std::lock_guard lock(prepared_motion_mutex_);
      settled_stop_event_id = stop_event_id;
      settled_stop_execution_id = request.execution_id;
    }
    return Result<AddressedParticipantStopSettlementDTO>::Ok(std::move(settlement));
  }

  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO request) override {
    ++prepare_motion_calls;
    Trace("engine:prepare_motion");
    if (prepare_motion_hook) prepare_motion_hook();
    if (prepare_call_failure.has_value()) return Result<std::uint64_t>::Err(*prepare_call_failure);
    if (request.replacement) {
      std::lock_guard lock(prepared_motion_mutex_);
      const bool attach = request.replacement->spring_return &&
                          request.replacement->spring_return->abandoned_preparation_id != 0U;
      if (!supports_motion_replacement ||
          (attach && (!replacement_snapshot ||
                      replacement_snapshot->execution_id !=
                          request.replacement->spring_return->abandoned_preparation_id ||
                      replacement_snapshot->state !=
                          ResolvedPreparedMotionExecutionStateDTO::kDispatched)) ||
          (!attach && replacement_snapshot &&
           replacement_snapshot->state !=
               ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch)) {
        return Result<std::uint64_t>::Err(ErrorCode::kUnsupportedCapability);
      }
      auto participant = MakePreparedMotionParticipant(request);
      if (!participant.ok()) return Result<std::uint64_t>::Err(participant.error());
      auto retained =
          std::make_shared<const PreparedOperationParticipantDTO>(std::move(participant.value()));
      std::optional<PrepareParticipantResultDTO> prepared;
      const bool cold = request.replacement->spring_return.has_value();
      if (!cold) {
        auto result =
            MakePrepareParticipantResult(*retained, PrepareParticipantResultStateDTO::kPrepared);
        if (!result.ok()) return Result<std::uint64_t>::Err(result.error());
        prepared = std::move(result.value());
      } else {
        const auto& spring = *request.replacement->spring_return;
        if (!spring.stage || !spring.point || !prepared_snapshot || !last_dispatched_participant) {
          return Result<std::uint64_t>::Err(ErrorCode::kInvalidRequest);
        }
        const ControllerGoalId goal{spring.stage->controller_goal_id};
        if (spring.point->source_goal != goal ||
            spring.point->binding_epoch != spring.point->trip.binding_epoch ||
            !spring.point->canonical_return_path_time_s) {
          return Result<std::uint64_t>::Err(ErrorCode::kInvalidRequest);
        }
        auto& source = request.replacement->source;
        source.execution_id = prepared_snapshot->execution_id;
        source.execution_generation = replacement_execution_generation;
        source.goal = spring.point->source_goal;
        source.binding_epoch = spring.point->binding_epoch;
        source.prepared_contract_digest = last_dispatched_participant->prepared_contract_digest;
        source.controller_sample = spring.point->trip;
      }
      if (attach) {
        // As the engine's attachment: the sent replacement keeps its session
        // and reports the cold successor's reserved identity beside it.
        const auto abandoned = request.replacement->spring_return->abandoned_preparation_id;
        attached_cold_request = std::move(request);
        attached_cold_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
            next_replacement_id++, ResolvedPreparedMotionExecutionStateDTO::kColdPrepared,
            retained, std::nullopt, std::nullopt};
        return Result<std::uint64_t>::Ok(abandoned);
      }
      replacement_request = std::move(request);
      replacement_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
          next_replacement_id++,
          defer_prepare_completion ? ResolvedPreparedMotionExecutionStateDTO::kPreparing
          : cold                   ? ResolvedPreparedMotionExecutionStateDTO::kColdPrepared
                                   : ResolvedPreparedMotionExecutionStateDTO::kPrepared,
          retained, std::move(prepared), std::nullopt};
      replacement_phase.store(defer_prepare_completion
                                  ? PreparedMotionReplacementPhaseDTO::kPreparing
                                  : PreparedMotionReplacementPhaseDTO::kPrepared);
      return Result<std::uint64_t>::Ok(replacement_snapshot->execution_id);
    }
    {
      std::lock_guard lock(prepared_motion_mutex_);
      last_prepared_request = request;
      last_dispatched_participant.reset();
      settled_stop_event_id.reset();
      cold_backend_prepared = false;
      if (defer_prepare_completion) {
        prepared_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
            next_execution_id,
            ResolvedPreparedMotionExecutionStateDTO::kPreparing,
            {},
            std::nullopt,
            std::nullopt};
        return Result<std::uint64_t>::Ok(next_execution_id);
      }
    }
    auto completed = CompletePreparation();
    if (!completed.ok()) {
      return Result<std::uint64_t>::Err(completed.error());
    }
    return Result<std::uint64_t>::Ok(next_execution_id);
  }

  Result<void> complete_preparation() {
    {
      std::lock_guard lock(prepared_motion_mutex_);
      defer_prepare_completion = false;
      if (replacement_snapshot &&
          replacement_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kPreparing) {
        replacement_snapshot->state = replacement_request->replacement->spring_return
                                          ? ResolvedPreparedMotionExecutionStateDTO::kColdPrepared
                                          : ResolvedPreparedMotionExecutionStateDTO::kPrepared;
        replacement_phase.store(PreparedMotionReplacementPhaseDTO::kPrepared);
        return Result<void>::Ok();
      }
    }
    return CompletePreparation();
  }

  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t execution_id) const override {
    ++observe_prepared_motion_calls;
    auto result = [&] {
      std::lock_guard lock(prepared_motion_mutex_);
      if (replacement_snapshot && replacement_snapshot->execution_id == execution_id) {
        auto snapshot = *replacement_snapshot;
        snapshot.replacement = PreparedMotionReplacementObservationDTO{replacement_phase.load()};
        if (attached_cold_snapshot)
          snapshot.spring_cold_result = ResolvedPreparedMotionSpringColdResultDTO{
              attached_cold_snapshot->execution_id, attached_cold_snapshot->state,
              attached_cold_snapshot->participant, std::nullopt};
        return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(std::move(snapshot));
      }
      if (!prepared_snapshot || prepared_snapshot->execution_id != execution_id) {
        return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                     "fake prepared execution is unavailable",
                     {{"reason", "prepared_motion_missing"}}});
      }
      auto snapshot = *prepared_snapshot;
      snapshot.last_dispatched_participant = last_dispatched_participant;
      return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(std::move(snapshot));
    }();
    if (observe_prepared_motion_hook) observe_prepared_motion_hook();
    return result;
  }

  Result<void> DispatchPreparedMotion(std::uint64_t execution_id,
                                      const ContentDigest& prepared_contract_digest) override {
    ++dispatch_prepared_motion_calls;
    Trace("engine:dispatch_prepared_motion");
    std::optional<ExecuteRequestDTO> execution_request;
    {
      std::lock_guard lock(prepared_motion_mutex_);
      if (replacement_snapshot && replacement_snapshot->execution_id == execution_id) {
        if (!replacement_snapshot->participant ||
            replacement_snapshot->participant->prepared_contract_digest !=
                prepared_contract_digest ||
            replacement_snapshot->state != ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
          return Result<void>::Err(ErrorCode::kInvalidRequest);
        }
        replacement_snapshot->state = ResolvedPreparedMotionExecutionStateDTO::kDispatched;
        // As the engine records a replacement's send (its handle returned);
        // its stopped cursor stays unknown until adoption.
        if (!replacement_send_receipt_missing)
          replacement_snapshot->last_dispatched_participant = replacement_snapshot->participant;
        replacement_phase.store(replacement_dispatch_phase);
        return Result<void>::Ok();
      }
      if (!prepared_snapshot || !prepared_snapshot->participant ||
          prepared_snapshot->execution_id != execution_id ||
          prepared_snapshot->participant->prepared_contract_digest != prepared_contract_digest ||
          !last_prepared_request) {
        return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                          "fake prepared dispatch identity mismatch",
                                          {{"reason", "prepared_dispatch_mismatch"}}});
      }
      prepared_snapshot->state = defer_prepared_dispatch_effect
                                     ? ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested
                                     : ResolvedPreparedMotionExecutionStateDTO::kDispatched;
      if (!defer_prepared_dispatch_effect)
        last_dispatched_participant = prepared_snapshot->participant;
      execution_request = last_prepared_request->execution_request;
    }
    if (defer_prepared_dispatch_effect) {
      return Result<void>::Ok();
    }
    if (execution_backend != nullptr) {
      std::shared_ptr<const PreparedOperationParticipantDTO> participant;
      {
        std::lock_guard lock(prepared_motion_mutex_);
        participant = prepared_snapshot->participant;
      }
      if (!cold_backend_prepared) {
        const auto prepared = execution_backend->Prepare(*participant, execution_id);
        if (prepared.state != PrepareParticipantResultStateDTO::kPrepared) {
          return Result<void>::Err(prepared.error.value_or(ErrorDTO{"backend_prepare_rejected", "", {}}));
        }
      }
      const auto& binding = execution_request->binding;
      const auto handle = execution_backend->Dispatch(*participant, binding.controller_goal_id);
      if (const auto invalid = prepared_execution_handle_field_error(
              handle, *participant, binding.controller_goal_id)) {
        return Result<void>::Err(
            ErrorDTO{"backend_dispatch_rejected", "", {{"reason", std::string(*invalid)}}});
      }
    }
    auto dispatched = execute(*execution_request);
    if (prepared_dispatch_event_error.has_value()) {
      emit_error(*prepared_dispatch_event_error);
    }
    return dispatched.ok() ? Result<void>::Ok() : Result<void>::Err(dispatched.error());
  }

  Result<void> CancelPreparedMotion(std::uint64_t execution_id) override {
    ++cancel_prepared_motion_calls;
    Trace("engine:cancel_prepared_motion");
    if (cancel_prepared_motion_hook) cancel_prepared_motion_hook();
    const auto prior = get_status();
    {
      std::lock_guard lock(prepared_motion_mutex_);
      if (replacement_snapshot && replacement_snapshot->execution_id == execution_id) {
        if (replacement_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched &&
            replacement_phase.load() != PreparedMotionReplacementPhaseDTO::kNoEffect) {
          return Result<void>::Err(ErrorCode::kInvalidStateTransition);
        }
        replacement_snapshot->state =
            ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
        replacement_phase.store(PreparedMotionReplacementPhaseDTO::kNoEffect);
        return Result<void>::Ok();
      }
      if (!prepared_snapshot || prepared_snapshot->execution_id != execution_id) {
        return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                                          "fake prepared execution is unavailable",
                                          {{"reason", "prepared_motion_missing"}}});
      }
      if (prepared_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched) {
        return Result<void>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                     "fake prepared execution already dispatched",
                     {{"reason", "prepared_motion_already_dispatched"}}});
      }
      // Like ExecutionKernel::CancelPreparedMotion, a preparation that has
      // already failed is left unchanged and publishes no further terminal.
      if (prepared_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed) {
        return Result<void>::Ok();
      }
      prepared_snapshot->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    }
    // Like the real engine, cancelling an already terminal preparation does
    // not publish its terminal a second time.
    if (!prior.fact.has_value()) {
      emit_transition(prior.state, prepared_cancel_terminal_state);
    }
    return Result<void>::Ok();
  }

  Result<PreparedMotionReplacementSourceDTO> CaptureMotionReplacementSource(
      std::uint64_t execution_id) const override {
    ++capture_replacement_source_calls;
    std::lock_guard lock(prepared_motion_mutex_);
    if (!supports_motion_replacement) {
      return Result<PreparedMotionReplacementSourceDTO>::Err(ErrorCode::kUnsupportedCapability);
    }
    const auto current = get_status();
    if (!prepared_snapshot || !last_prepared_request || !last_dispatched_participant ||
        prepared_snapshot->execution_id != execution_id || current.execution_id != execution_id ||
        current.state != ExecutionState::kExecuting || current.fact) {
      return Result<PreparedMotionReplacementSourceDTO>::Err(ErrorCode::kInvalidStateTransition);
    }
    const auto& binding = last_prepared_request->execution_request.binding;
    PreparedMotionReplacementSourceDTO source;
    source.execution_id = execution_id;
    source.execution_generation = replacement_execution_generation;
    source.goal = {binding.controller_goal_id};
    source.binding_epoch = ExecutionBindingEpoch(1U);
    source.prepared_contract_digest = last_dispatched_participant->prepared_contract_digest;
    source.controller_sample.binding_epoch = source.binding_epoch;
    source.controller_sample.local_receipt_count = ExecutionReceiptCount(1U);
    source.controller_sample.cursor_s = 0.25;
    source.controller_sample.applied_factor = 1.0;
    const auto dimensions = binding.resources.size();
    source.controller_sample.reference_positions_rad.assign(dimensions, 0.0);
    source.controller_sample.reference_velocities_rad_s.assign(dimensions, 0.1);
    source.controller_sample.reference_accelerations_rad_s2.assign(dimensions, 0.0);
    source.progress.sequence = 1U;
    source.progress.execution_generation = source.execution_generation;
    source.progress.trajectory_time_ns = 250'000'000;
    source.progress.valid = true;
    return Result<PreparedMotionReplacementSourceDTO>::Ok(std::move(source));
  }

  Result<bool> RetireCancelledPreparedMotion(std::uint64_t candidate_id) override {
    ++retire_cancelled_replacement_calls;
    std::lock_guard lock(prepared_motion_mutex_);
    if (!replacement_snapshot || replacement_snapshot->execution_id != candidate_id) {
      return Result<bool>::Err(ErrorCode::kMissingReference);
    }
    const bool no_effect = replacement_snapshot->state ==
                               ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch &&
                           replacement_phase.load() == PreparedMotionReplacementPhaseDTO::kNoEffect;
    bool stopped = false;
    const auto last_stage = LastStopStageSnapshot();
    if (replacement_request && replacement_request->replacement && settled_stop_event_id &&
        last_stage && prepared_snapshot && last_prepared_request &&
        last_dispatched_participant) {
      const auto& source = replacement_request->replacement->source;
      const auto& binding = last_prepared_request->execution_request.binding;
      const auto current = get_status();
      stopped = prepared_snapshot->execution_id == source.execution_id &&
                settled_stop_execution_id == source.execution_id &&
                source.execution_generation == replacement_execution_generation &&
                source.goal == ControllerGoalId{binding.controller_goal_id} &&
                source.prepared_contract_digest ==
                    last_dispatched_participant->prepared_contract_digest &&
                *settled_stop_event_id == last_stage->stop_event_id &&
                source.goal == ControllerGoalId{last_stage->controller_goal_id} &&
                current.execution_id == source.execution_id && current.fact &&
                current.fact->kind == ParticipantExecutionFactKind::kPreemptedByAddressedStop &&
                current.fact->effect == ParticipantEffectKnowledge::kEffectSettled;
    }
    if (!no_effect && !stopped) {
      return Result<bool>::Err(ErrorCode::kInvalidStateTransition);
    }
    replacement_snapshot.reset();
    replacement_request.reset();
    if (stopped && attached_cold_snapshot) {
      replacement_snapshot = std::move(attached_cold_snapshot);
      replacement_request = std::move(attached_cold_request);
      replacement_phase.store(PreparedMotionReplacementPhaseDTO::kPrepared);
    }
    attached_cold_snapshot.reset();
    attached_cold_request.reset();
    return Result<bool>::Ok(true);
  }

  Result<bool> RetireRebuiltUnknownPreparedMotion(std::uint64_t source_id,
                                                   std::uint64_t candidate_id) override {
    ++retire_rebuilt_unknown_calls;
    if (!rebuilt_unknown_retirement_allowed || source_id != rebuilt_unknown_source_id ||
        candidate_id != rebuilt_unknown_candidate_id)
      return Result<bool>::Err(ErrorCode::kExecutionRejected);
    std::lock_guard lock(prepared_motion_mutex_);
    if (!replacement_snapshot || replacement_snapshot->execution_id != candidate_id)
      return Result<bool>::Err(ErrorCode::kMissingReference);
    replacement_snapshot.reset();
    replacement_request.reset();
    return Result<bool>::Ok(true);
  }

  Result<bool> PrepareColdMotionAfterStop(std::uint64_t candidate_id,
                                          const ParticipantStopOutcomeDTO& incumbent) override {
    ++prepare_cold_motion_calls;
    if (prepare_cold_refusal) return Result<bool>::Err(*prepare_cold_refusal);
    std::unique_lock lock(prepared_motion_mutex_);
    if (!replacement_snapshot || !replacement_request || !replacement_request->replacement ||
        !replacement_request->replacement->spring_return ||
        replacement_snapshot->execution_id != candidate_id) {
      return Result<bool>::Err(ErrorCode::kInvalidRequest);
    }
    if (replacement_snapshot->state == ResolvedPreparedMotionExecutionStateDTO::kPreparing) {
      return Result<bool>::Ok(false);
    }
    const auto& context = *replacement_request->replacement;
    const auto& spring = *context.spring_return;
    const auto current = get_status();
    if (replacement_snapshot->state != ResolvedPreparedMotionExecutionStateDTO::kColdPrepared ||
        replacement_snapshot->backend_prepare_result || !replacement_snapshot->participant ||
        !spring.stage || !spring.point || !spring.point->canonical_return_path_time_s ||
        !settled_stop_event_id || settled_stop_execution_id != context.source.execution_id ||
        incumbent.stop_event_id != spring.stage->stop_event_id ||
        incumbent.stop_event_id != *settled_stop_event_id ||
        incumbent.phase != StopPhase::kStopped || incumbent.escalated ||
        current.execution_id != context.source.execution_id || !current.fact ||
        current.fact->kind != ParticipantExecutionFactKind::kPreemptedByAddressedStop ||
        current.fact->effect != ParticipantEffectKnowledge::kEffectSettled ||
        !last_dispatched_participant ||
        last_dispatched_participant->prepared_contract_digest !=
            context.source.prepared_contract_digest) {
      return Result<bool>::Err(ErrorCode::kInvalidStateTransition);
    }
    // The incumbent's attempt was contained by the spring-return stage.
    const bool returned = incumbent.containing_stage != kNoStopStage &&
                          incumbent.containing_stage == spring.stage->stage_index;
    if (!returned) return Result<bool>::Err(ErrorCode::kInvalidStateTransition);
    auto prepared = MakePrepareParticipantResult(*replacement_snapshot->participant,
                                                 PrepareParticipantResultStateDTO::kPrepared);
    if (!prepared.ok()) return Result<bool>::Err(prepared.error());
    if (execution_backend) {
      const auto retained = replacement_snapshot->participant;
      const auto expected_event = *settled_stop_event_id;
      lock.unlock();
      const auto backend = execution_backend->Prepare(*retained, candidate_id);
      lock.lock();
      if (backend.state != PrepareParticipantResultStateDTO::kPrepared) {
        return Result<bool>::Err(
            backend.error.value_or(ErrorDTO{"backend_prepare_rejected", "", {}}));
      }
      if (!replacement_snapshot || replacement_snapshot->execution_id != candidate_id ||
          replacement_snapshot->state != ResolvedPreparedMotionExecutionStateDTO::kColdPrepared ||
          !settled_stop_event_id || *settled_stop_event_id != expected_event) {
        return Result<bool>::Err(ErrorCode::kInvalidStateTransition);
      }
    }
    replacement_snapshot->backend_prepare_result = std::move(prepared.value());
    replacement_snapshot->state = ResolvedPreparedMotionExecutionStateDTO::kPrepared;
    prepared_snapshot = std::move(replacement_snapshot);
    replacement_snapshot.reset();
    last_prepared_request = std::move(replacement_request);
    replacement_request.reset();
    last_prepared_request->replacement.reset();
    last_dispatched_participant.reset();
    next_execution_id = candidate_id;
    next_execute_result = Result<std::uint64_t>::Ok(candidate_id);
    cold_backend_prepared = true;
    ++cold_participant_preparations;
    Trace("engine:prepare_cold_after_stop");
    {
      std::lock_guard status_lock(status_mutex_);
      status.state = ExecutionState::kIdle;
      status.execution_id = candidate_id;
      status.fact.reset();
    }
    return Result<bool>::Ok(true);
  }

  // OD-175 D1: "prove" (every check that can fail) and "apply" (the swaps,
  // no refusal), split around the Kernel's "decide" step.
  Result<bool> ProvePreparedMotionReplacement(
      std::uint64_t candidate_id, const PreparedMotionReplacementSourceDTO& source) override {
    ++commit_replacement_calls;
    const auto proven = ProveLocked(candidate_id, source);
    // Test point between "prove" and "decide" (no lock of the fake held).
    if (proven.ok() && proven.value() && after_prove_hook) after_prove_hook();
    return proven;
  }

  bool CloseMotionReplacementHazard(
      const PreparedMotionReplacementSourceDTO& source) noexcept override {
    ++close_replacement_hazard_calls;
    last_closed_hazard_generation = source.execution_generation;
    // A lookahead claim that won before "decide" (OD-175 D2).
    return !replacement_hazard_claimed.load();
  }

  void ApplyPreparedMotionReplacement(std::uint64_t candidate_id) override {
    // Test point in the gap's first half, after "decide" (Dc).
    if (before_apply_hook) before_apply_hook();
    {
      std::lock_guard lock(prepared_motion_mutex_);
      ++replacement_owner_commits;
      Trace("engine:commit_replacement");
      prepared_snapshot = std::move(replacement_snapshot);
      replacement_snapshot.reset();
      last_prepared_request = std::move(replacement_request);
      replacement_request.reset();
      last_dispatched_participant = prepared_snapshot->participant;
      {
        std::lock_guard execute_lock(last_execute_mutex_);
        last_execute = last_prepared_request->execution_request;
      }
      next_execution_id = candidate_id;
      next_execute_result = Result<std::uint64_t>::Ok(candidate_id);
      ++replacement_execution_generation;
      replacement_phase.store(PreparedMotionReplacementPhaseDTO::kCommitted);
      {
        std::lock_guard status_lock(status_mutex_);
        status.execution_id = candidate_id;
        status.state = ExecutionState::kExecuting;
        status.fact.reset();
      }
    }
    // Test point in the gap's second half, after "apply" (Ap).
    if (after_apply_hook) after_apply_hook();
  }

  Result<bool> ProveLocked(std::uint64_t candidate_id,
                           const PreparedMotionReplacementSourceDTO& source) {
    std::lock_guard lock(prepared_motion_mutex_);
    if (!replacement_snapshot || !replacement_request || !replacement_request->replacement ||
        replacement_snapshot->execution_id != candidate_id || !prepared_snapshot ||
        prepared_snapshot->execution_id != source.execution_id || !last_dispatched_participant ||
        !last_prepared_request) {
      return Result<bool>::Err(ErrorCode::kInvalidRequest);
    }
    const auto& captured = replacement_request->replacement->source;
    const auto current = get_status();
    const auto& binding = last_prepared_request->execution_request.binding;
    const ControllerGoalId current_goal{binding.controller_goal_id};
    const auto same_progress = [](const auto& left, const auto& right) {
      return left.sequence == right.sequence &&
             left.execution_generation == right.execution_generation &&
             left.trajectory_time_ns == right.trajectory_time_ns &&
             left.source_kind == right.source_kind &&
             left.authority_profile == right.authority_profile &&
             left.source_timestamp_ns == right.source_timestamp_ns &&
             left.received_timestamp_ns == right.received_timestamp_ns &&
             left.freshness_budget_ns == right.freshness_budget_ns && left.valid == right.valid &&
             left.terminal == right.terminal;
    };
    if (source.execution_id != captured.execution_id ||
        source.execution_generation != captured.execution_generation ||
        source.execution_generation != replacement_execution_generation ||
        current_goal != source.goal || current.execution_id != source.execution_id ||
        current.state != ExecutionState::kExecuting || current.fact ||
        addressed_stop_fence_execution_id.load() == source.execution_id ||
        source.goal != captured.goal || source.binding_epoch != captured.binding_epoch ||
        source.prepared_contract_digest != captured.prepared_contract_digest ||
        source.controller_sample != captured.controller_sample ||
        !same_progress(source.progress, captured.progress) ||
        last_dispatched_participant->prepared_contract_digest != source.prepared_contract_digest) {
      return Result<bool>::Err(ErrorCode::kInvalidRequest);
    }
    if (replacement_phase.load() != PreparedMotionReplacementPhaseDTO::kReadyToCommit) {
      return Result<bool>::Ok(false);
    }
    if (prove_refusal) return Result<bool>::Err(*prove_refusal);
    return Result<bool>::Ok(true);
  }

  Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO request) override {
    ++prepare_command_calls;
    Trace("engine:prepare_command");
    if (!request.participant) {
      return Result<std::uint64_t>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "fake command preparation has no participant",
                   {{"reason", "prepared_command_participant_missing"}}});
    }
    auto private_validation = validate_gripper_command(request.gripper().command_request);
    if (!private_validation.ok()) {
      return Result<std::uint64_t>::Err(private_validation.error());
    }
    if (prepare_command_hook) {
      prepare_command_hook();
    }
    if (prepare_command_failure.has_value()) {
      return Result<std::uint64_t>::Err(*prepare_command_failure);
    }
    auto prepared = MakePrepareParticipantResult(*request.participant,
                                                 PrepareParticipantResultStateDTO::kPrepared);
    if (!prepared.ok()) {
      return Result<std::uint64_t>::Err(prepared.error());
    }
    const auto preparation_id = next_execution_id;
    auto snapshot = ResolvedPreparedCapabilityCommandSnapshotDTO{
        next_execution_id,   ResolvedPreparedCapabilityCommandStateDTO::kPrepared,
        request.participant, std::move(prepared.value()),
        std::nullopt,        std::nullopt,
        std::nullopt};
    {
      std::lock_guard lock(prepared_command_mutex_);
      last_prepared_command_request = std::move(request);
      prepared_command_snapshot = std::move(snapshot);
    }
    return Result<std::uint64_t>::Ok(preparation_id);
  }

  Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t preparation_id) const override {
    ++observe_prepared_command_calls;
    if (observe_prepared_command_hook) observe_prepared_command_hook();
    if (observe_prepared_command_failure.has_value()) {
      return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
          *observe_prepared_command_failure);
    }
    std::lock_guard lock(prepared_command_mutex_);
    if (!prepared_command_snapshot.has_value() ||
        prepared_command_snapshot->preparation_id != preparation_id) {
      return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                   "fake prepared command is unavailable",
                   {{"reason", "prepared_command_missing"}}});
    }
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Ok(*prepared_command_snapshot);
  }

  Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t preparation_id, const ContentDigest& prepared_contract_digest,
      const ControllerGoalId& controller_goal_id) override {
    ++dispatch_prepared_command_calls;
    Trace("engine:dispatch_prepared_command");
    if (dispatch_prepared_command_hook) {
      dispatch_prepared_command_hook();
    }
    std::lock_guard lock(prepared_command_mutex_);
    if (!prepared_command_snapshot.has_value() || !prepared_command_snapshot->participant ||
        prepared_command_snapshot->preparation_id != preparation_id ||
        prepared_command_snapshot->participant->prepared_contract_digest !=
            prepared_contract_digest ||
        !last_prepared_command_request.has_value()) {
      return Result<GripperCommandResultDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                   "fake prepared command dispatch identity mismatch",
                   {{"reason", "prepared_command_dispatch_mismatch"}}});
    }
    if (prepared_command_snapshot->dispatch_result.has_value()) {
      if (prepared_command_snapshot->controller_goal_id != controller_goal_id) {
        return Result<GripperCommandResultDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "fake prepared command goal identity conflict",
                     {{"reason", "prepared_command_goal_conflict"}}});
      }
      return Result<GripperCommandResultDTO>::Ok(*prepared_command_snapshot->dispatch_result);
    }
    prepared_command_snapshot->controller_goal_id = controller_goal_id;
    auto dispatched = command_gripper(last_prepared_command_request->gripper().command_request);
    prepared_command_snapshot->dispatch_result = dispatched;
    prepared_command_snapshot->error = dispatched.error;
    prepared_command_snapshot->state =
        dispatched.accepted && !dispatched.error.has_value()
            ? ResolvedPreparedCapabilityCommandStateDTO::kDispatched
            : ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed;
    return Result<GripperCommandResultDTO>::Ok(std::move(dispatched));
  }

  Result<void> CancelPreparedCapabilityCommand(std::uint64_t preparation_id) override {
    ++cancel_prepared_command_calls;
    Trace("engine:cancel_prepared_command");
    if (cancel_prepared_command_hook) cancel_prepared_command_hook();
    std::lock_guard lock(prepared_command_mutex_);
    if (!prepared_command_snapshot.has_value() ||
        prepared_command_snapshot->preparation_id != preparation_id) {
      return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                                        "fake prepared command is unavailable",
                                        {{"reason", "prepared_command_missing"}}});
    }
    if (prepared_command_snapshot->state ==
        ResolvedPreparedCapabilityCommandStateDTO::kDispatched) {
      return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                                        "fake prepared command already dispatched",
                                        {{"reason", "prepared_command_already_dispatched"}}});
    }
    prepared_command_snapshot->state =
        ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
    return Result<void>::Ok();
  }

  Result<RobotStateDTO> current_preview_state(std::int64_t) const override {
    return Result<RobotStateDTO>::Ok(RobotStateDTO{{"mf2_joint_1"}, {0.0}, {}});
  }

  Result<MotionPreviewResultDTO> preview_motion(ExecuteRequestDTO request,
                                                CollisionWorldLeaseInventory projection_inventory,
                                                RobotStateDTO start_state, std::uint32_t,
                                                std::stop_token,
                                                std::optional<MotionPathPreview>* path_preview) override {
    if (path_preview != nullptr) path_preview->reset();
    ++preview_calls;
    last_preview = request;
    const RobotStateDTO final_state{start_state.joint_names, start_state.positions_rad, {}};
    return Result<MotionPreviewResultDTO>::Ok(MotionPreviewResultDTO{
        TrajectoryDTO{request.binding.resources,
                      start_state.joint_names,
                      {{0.0, start_state.positions_rad, {}, {}}}},
        start_state, final_state,
        ControlledStateDTO{request.binding.controllable_component_id,
                           request.binding.command_target_id,
                           request.binding.control_domain_id,
                           request.binding.control_domain_revision,
                           request.binding.control_binding_id,
                           request.binding.control_binding_revision,
                           request.binding.controller_topology_id,
                           request.binding.topology_revision,
                           request.binding.control_route_id,
                           request.binding.active_control_configuration_id,
                           request.binding.active_control_configuration_generation,
                           request.binding.resources,
                           start_state.joint_names,
                           {AngularPositionDTO{0.0}},
                           std::nullopt},
        Pose3DTO{}, projection_inventory.projection_binding,
        projection_inventory.bundle.world->revision, 0U});
  }

  ParticipantStopStageResultDTO TranslateStopStageForTakeover(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver& observer) override {
    ++takeover_stop_stage_calls;
    const auto& profile = stage.participant_stop_plan.participant_stop_profile;
    const auto is_spring = [](const MotionInterruptStopMode mode) {
      return mode == MotionInterruptStopMode::kSpringBackHold ||
             mode == MotionInterruptStopMode::kSpringBackFast;
    };
    std::optional<MotionInterruptStopMode> selected;
    if (profile) {
      if (is_spring(profile->stop_mode))
        selected = profile->stop_mode;
      else {
        const auto found = std::find_if(profile->fallback_stop_modes.begin(),
                                        profile->fallback_stop_modes.end(), is_spring);
        if (found != profile->fallback_stop_modes.end()) selected = *found;
      }
    }
    const bool spring = selected.has_value();
    if (spring) {
      std::optional<SpringBackReturnPointDTO> point;
      {
        std::lock_guard lock(prepared_motion_mutex_);
        if (last_prepared_request) {
          point.emplace();
          point->source_goal = {stage.controller_goal_id};
          point->binding_epoch = ExecutionBindingEpoch(1U);
          point->trip.binding_epoch = point->binding_epoch;
          point->trip.local_receipt_count = ExecutionReceiptCount(1U);
          point->trip.cursor_s = 0.25;
          point->trip.applied_factor = 1.0;
          point->initial.joint_names = last_prepared_request->execution_request.binding
                                           .model_group_binding.compiled_joint_names;
          const auto dimensions = point->initial.joint_names.size();
          point->trip.reference_positions_rad.assign(dimensions, 0.0);
          point->trip.reference_velocities_rad_s.assign(dimensions, 0.1);
          point->trip.reference_accelerations_rad_s2.assign(dimensions, 0.0);
          point->initial.command_path_time_s = 0.25;
          point->initial.path_time_rate = 1.0;
          point->initial.spring_back_return_point.positions_rad.assign(dimensions, 0.0);
          point->initial.spring_back_return_point.velocities_rad_s.assign(dimensions, 0.0);
          point->initial.spring_back_return_point.accelerations_rad_s2.assign(dimensions, 0.0);
          point->canonical_return_path_time_s = 0.25;
        }
      }
      if (point) static_cast<void>(observer.SpringReturnPointReady(*point));
    }
    // Existing hook runs after publishing the actual point, allowing a test
    // to hold the physical return while successor preparation proceeds.
    auto result = TranslateStopStage(stage, budget, cancellation);
    if (spring && result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
      result.actual_provider_contract_mode = *selected == MotionInterruptStopMode::kSpringBackHold
                                                 ? MotionControlledStopMode::kSpringBackHold
                                                 : MotionControlledStopMode::kSpringBackFast;
      if (profile->stop_mode != *selected) {
        result.recoverable_substitution_reason = "takeover_spring_return";
      }
    }
    return result;
  }

  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& stage,
                                                   const RemainingStopBudgetDTO& budget,
                                                   std::stop_token) override {
    Trace("engine:translate_stop_stage");
    {
      std::lock_guard lock(last_stop_mutex_);
      last_stop_stage = stage;
      last_stop_budget = budget;
    }
    const auto call = ++translate_stop_stage_calls;
    if (translate_stop_stage_hook) {
      translate_stop_stage_hook();
    }
    const auto stop_result = stop_result_for_call ? stop_result_for_call(call) : next_stop_result;
    std::vector<StopEvidenceItemDTO> evidence;
    const auto& requirements = stage.participant_stop_plan.stage_commands.at(stage.stage_index)
                                   .resolved_stage.evidence_requirements;
    evidence.reserve(requirements.size());
    for (const auto& requirement : requirements) {
      evidence.push_back(StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
          stop_result.accepted ? StopEvidenceItemStateDTO::kSatisfied
                                    : StopEvidenceItemStateDTO::kFailed,
          requirement.resolved_source,
          requirement.resolved_source.runtime_target.provider_runtime_epoch, 1U, 0,
          stop_result.accepted
              ? std::optional<JsonValueDTO>(next_stop_evidence.value_or(JsonValueDTO{true}))
              : std::nullopt,
          stop_result.accepted ? std::nullopt
                                    : std::optional(stop_result.error.value_or(
                                          ErrorDTO{"controller_stop_rejected",
                                                   "controller rejected Stop",
                                                   {{"reason", "controller_stop_rejected"}}}))});
    }
    ++stop_calls;
    std::optional<MotionControlledStopMode> actual_stop_mode;
    if (stage.participant_stop_plan.stage_commands.at(stage.stage_index)
            .resolved_stage.containment_command.command_kind ==
        StopContainmentCommandKind::kMotionControlledStop) {
      actual_stop_mode = stage.participant_stop_plan.participant_stop_profile.has_value() &&
                                 stage.participant_stop_plan.participant_stop_profile->stop_mode ==
                                     MotionInterruptStopMode::kSpringBackHold
                             ? MotionControlledStopMode::kSpringBackHold
                             : MotionControlledStopMode::kSoftOnPath;
    }
    auto result = ParticipantStopStageResultDTO{stage,
                                                StopStageAttemptDispositionDTO::kDispatched,
                                                stop_result.accepted
                                                    ? ParticipantStopStageOutcomeDTO::kSucceeded
                                                    : ParticipantStopStageOutcomeDTO::kFailed,
                                                actual_stop_mode,
                                                std::nullopt,
                                                1U,
                                                stage.provider_runtime_epoch,
                                                1U,
                                                0,
                                                std::move(evidence),
                                                stop_result.error};
    if (!stop_result.accepted) {
      const auto& first_error = result.error.value();
      for (auto& item : result.evidence_items) {
        item.state = StopEvidenceItemStateDTO::kFailed;
        item.evidence.reset();
        item.error = first_error;
      }
    }
    return result;
  }
  ResetResultDTO reset_after_certified_containment(std::uint64_t execution_id,
                                                   std::optional<std::uint64_t>) override {
    ++certified_reset_calls;
    if (reset_effect_hook) reset_effect_hook();
    Trace("engine:reset_after_certified_containment");
    if (execution_id == 0U || execution_id != next_execution_id) {
      return ResetResultDTO{false, ErrorDTO{"certified_containment_execution_mismatch",
                                            "fake certified reset execution is stale",
                                            {}}};
    }
    if (next_reset_result.accepted) {
      {
        std::lock_guard lock(prepared_motion_mutex_);
        prepared_snapshot.reset();
      }
      emit_transition(get_status().state, ExecutionState::kIdle);
    }
    return next_reset_result;
  }
  AcknowledgeCompletedResultDTO acknowledge_completed(std::uint64_t,
                                                      std::optional<std::uint64_t>) override {
    ++acknowledge_calls;
    Trace("engine:acknowledge_completed");
    if (acknowledge_effect_hook) {
      acknowledge_effect_hook();
    }
    if (next_acknowledge_result.accepted) {
      emit_transition(get_status().state, ExecutionState::kIdle);
    }
    return next_acknowledge_result;
  }

  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) override {
    ++runtime_scale_calls;
    Trace("engine:set_runtime_scale");
    std::function<void()> effect_hook;
    {
      std::lock_guard lock(runtime_scale_mutex_);
      effect_hook = runtime_scale_effect_hook;
    }
    if (effect_hook) effect_hook();
    std::lock_guard lock(runtime_scale_mutex_);
    if (!runtime_scale.supported) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kUnsupportedCapability)),
                   "fake participant has no runtime-scale mechanism",
                   {{"reason", "runtime_scale_mechanism_unsupported"}}});
    }
    if (!next_runtime_scale_result.ok()) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(next_runtime_scale_result.error());
    }
    last_runtime_scale_command = command;
    runtime_scale.commanded_scale = command.requested_scale;
    runtime_scale.applied_scale = command.requested_scale;
    ++runtime_scale.acknowledgement_sequence;
    runtime_scale.acknowledgement_timestamp_ns = 1;
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Ok(
        ResolvedRuntimeScaleAcceptedDTO{command.operation_id,
                                        command.operation_revision,
                                        command.participant_id,
                                        command.controllable_component_id,
                                        command.command_target_id,
                                        command.capability_id,
                                        command.control_domain_id,
                                        command.control_domain_revision,
                                        command.control_binding_id,
                                        command.control_binding_revision,
                                        command.controller_goal_id,
                                        command.controller_topology_id,
                                        command.topology_revision,
                                        command.control_route_id,
                                        command.scaling_domain_id,
                                        command.coordinated_barrier_id,
                                        command.resources,
                                        command.active_control_configuration_id,
                                        command.active_control_configuration_generation,
                                        command.requested_runtime_scale_generation,
                                        command.requested_scale,
                                        runtime_scale.acknowledgement_sequence,
                                        runtime_scale.acknowledgement_timestamp_ns});
  }

  void store_runtime_scale_setting(const double scale, const std::uint64_t sequence) override {
    std::lock_guard lock(runtime_scale_mutex_);
    stored_runtime_scale_settings.emplace_back(scale, sequence);
  }
  [[nodiscard]] std::vector<std::pair<double, std::uint64_t>> StoredRuntimeScaleSettings() const {
    std::lock_guard lock(runtime_scale_mutex_);
    return stored_runtime_scale_settings;
  }

  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationNumber& operation_id, const ControlBindingId& control_binding_id) const override {
    const auto dispatched = LastExecuteSnapshot();
    std::lock_guard lock(runtime_scale_mutex_);
    if (!last_runtime_scale_command.has_value() ||
        last_runtime_scale_command->operation_id != operation_id ||
        last_runtime_scale_command->control_binding_id != control_binding_id ||
        !dispatched.has_value()) {
      return Result<ResolvedRuntimeScaleStatusDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                   "fake runtime-scale status is not initialized",
                   {{"reason", "runtime_scale_status_not_initialized"}}});
    }
    const auto& command = *last_runtime_scale_command;
    return Result<ResolvedRuntimeScaleStatusDTO>::Ok(ResolvedRuntimeScaleStatusDTO{
        command.operation_id,
        command.operation_revision,
        command.participant_id,
        command.controllable_component_id,
        command.command_target_id,
        command.capability_id,
        command.control_domain_id,
        command.control_domain_revision,
        command.control_binding_id,
        command.control_binding_revision,
        command.controller_goal_id,
        command.controller_topology_id,
        command.topology_revision,
        command.control_route_id,
        command.scaling_domain_id,
        command.coordinated_barrier_id,
        command.resources,
        command.active_control_configuration_id,
        command.active_control_configuration_generation,
        command.requested_runtime_scale_generation,
        runtime_scale.commanded_scale,
        runtime_scale.applied_scale,
        runtime_scale.virtual_trajectory_time_s,
        dispatched->binding.provider_runtime_epoch,
        runtime_scale.acknowledgement_sequence,
        runtime_scale.acknowledgement_timestamp_ns,
        runtime_scale.error.has_value() ? RuntimeScaleApplicationState::kFailed
        : runtime_scale.active          ? RuntimeScaleApplicationState::kApplied
                                        : RuntimeScaleApplicationState::kPending,
        runtime_scale.error});
  }

  bool notify_exact_gripper_owner_loss_recorded(const std::string& gripper_id, std::uint64_t,
                                                std::uint64_t) override {
    return exact_owner_notification_accept &&
           (expected_owner_notification_gripper.empty() ||
            gripper_id == expected_owner_notification_gripper);
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override {
    ++command_calls;
    Trace("engine:command_gripper");
    last_command = request;
    return next_command_result;
  }

  Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const override {
    ++validate_command_calls;
    if (validate_command_hook) validate_command_hook();
    return next_command_validation;
  }

  GripperActuationKind gripper_actuation_kind(const std::string&) const override {
    return configured_gripper_actuation;
  }
  GripperOutputMode gripper_output_mode(const std::string&) const override {
    return configured_gripper_output;
  }
  GripperCommandStatusDTO get_gripper_status(const std::string&) const override {
    ++get_gripper_status_calls;
    return gripper_status;
  }

  Result<std::uint64_t> stop_gripper(const std::string& gripper_id,
                                     std::uint64_t expected_command_id,
                                     std::uint64_t expected_generation) override {
    ++stop_gripper_calls;
    Trace("engine:stop_gripper");
    if (gripper_status.gripper_id != gripper_id ||
        gripper_status.command_id != expected_command_id ||
        gripper_status.generation != expected_generation) {
      return Result<std::uint64_t>::Err(
          ErrorDTO{"stale_gripper_state", "fake gripper generation is stale", {}});
    }
    gripper_status.state = GripperCommandState::kIdle;
    ++gripper_status.generation;
    return Result<std::uint64_t>::Ok(gripper_status.generation);
  }

  Result<void> reset_gripper(const std::string& gripper_id, std::uint64_t expected_command_id,
                             std::uint64_t expected_generation) override {
    ++reset_gripper_calls;
    Trace("engine:reset_gripper");
    if (gripper_status.gripper_id != gripper_id ||
        gripper_status.command_id != expected_command_id ||
        gripper_status.generation != expected_generation) {
      return Result<void>::Err(
          ErrorDTO{"stale_gripper_state", "fake gripper generation is stale", {}});
    }
    gripper_status.state = GripperCommandState::kIdle;
    ++gripper_status.generation;
    return Result<void>::Ok();
  }

  ExecutionStatus get_status() const override {
    ++status_read_calls;
    if (status_read_hook) status_read_hook();
    std::lock_guard lock(status_mutex_);
    return participant_status(status, ended_phase, ended_error);
  }


  KernelRecoveryAssessmentDTO get_recovery_assessment() const override {
    ++recovery_assessment_calls;
    if (recovery_assessment_hook) {
      recovery_assessment_hook();
    }
    if (throw_during_recovery_assessment) {
      throw std::runtime_error("injected recovery assessment failure");
    }
    return recovery_assessment;
  }

  KernelRecoveryActionResultDTO apply_recovery_action(
      const KernelRecoveryActionRequestDTO& request) override {
    ++recovery_action_calls;
    last_recovery_request = request;
    if (recovery_action_hook) {
      recovery_action_hook();
    }
    if (throw_during_recovery) {
      throw std::runtime_error("injected recovery failure");
    }
    if (next_recovery_result.has_value()) {
      return *next_recovery_result;
    }
    return IResolvedParticipantExecutionEngine::apply_recovery_action(request);
  }

  SubscriptionHandle subscribe_events(EventCallback callback) override {
    if (subscribe_hook) subscribe_hook();
    std::lock_guard lock(callbacks_mutex_);
    const auto id = next_subscription_id++;
    callbacks.emplace(id, std::move(callback));
    return SubscriptionHandle{id};
  }

  void unsubscribe_events(SubscriptionHandle handle) override {
    {
      std::lock_guard lock(callbacks_mutex_);
      callbacks.erase(handle.id);
    }
    if (unsubscribe_hook) unsubscribe_hook();
  }

  [[nodiscard]] std::size_t subscription_count() const noexcept {
    std::lock_guard lock(callbacks_mutex_);
    return callbacks.size();
  }

  void emit_transition(ExecutionState from, ExecutionState to,
                       std::optional<ErrorDTO> error = std::nullopt) {
    deliver_event(record_transition(from, to, std::move(error)));
  }

  KernelEvent record_transition(ExecutionState from, ExecutionState to,
                                std::optional<ErrorDTO> error) {
    {
      std::lock_guard lock(status_mutex_);
      if (scripted_ending(to) && !scripted_ending(from)) ended_phase = from;
      if (scripted_ending(to)) ended_error = error;
      status.state = to;
      status.progress = to == ExecutionState::kCompleted ? 1.0 : 0.4;
      if (error.has_value()) {
        status.last_error = ErrorCode::kExecutionAborted;
      }
    }
    {
      std::lock_guard lock(runtime_scale_mutex_);
      runtime_scale.active = to == ExecutionState::kExecuting;
    }
    if (!scripted_ending(to)) {
      return KernelEvent{
          KernelEventType::kStateTransition, next_execution_id, 0, from, to, std::move(error), {}};
    }
    const auto reported = get_status();
    KernelEvent event{KernelEventType::kParticipantSettled,
                      next_execution_id,
                      0,
                      reported.state,
                      reported.state,
                      std::move(error),
                      {}};
    event.participant_fact = reported.fact;
    return event;
  }

  // Models KernelEventPublisher dispatch exactly: copy the callback set first,
  // expose that capture to the test, and invoke only after an explicit release.
  // Unsubscribe therefore cannot revoke the already-copied callback object.
  void emit_transition_after_callback_capture(
      ExecutionState from, ExecutionState to,
      const std::function<void()>& captured_boundary,
      std::optional<ErrorDTO> error = std::nullopt) {
    const auto event = record_transition(from, to, std::move(error));
    const auto snapshot = CallbackSnapshot();
    captured_boundary();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  void deliver_event(const KernelEvent& event) {
    const auto snapshot = CallbackSnapshot();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  /// The session worker's report of its committed send, as
  /// `ExecutionKernel::report_dispatch_outcome` emits it to a Stop whose
  /// fence found the send in flight. Not sent: `fact` is the fact the worker
  /// recorded (cancelled before dispatch when the Stop came first).
  void emit_dispatch_outcome(const bool dispatched,
                             std::optional<ParticipantExecutionFact> fact = std::nullopt) {
    const auto current = get_status().state;
    KernelEvent event{KernelEventType::kParticipantDispatchOutcome,
                      next_execution_id,
                      0,
                      current,
                      current,
                      std::nullopt,
                      {{"dispatched", dispatched ? "true" : "false"}}};
    event.participant_fact = std::move(fact);
    deliver_event(event);
  }

  /// The worker's own ending of a send the Stop's stage refused: the
  /// cancelled-before-dispatch fact (`kParticipantSettled`), before its report.
  void emit_cancelled_before_dispatch() {
    const auto current = get_status().state;
    KernelEvent event{KernelEventType::kParticipantSettled,
                      next_execution_id,
                      0,
                      current,
                      current,
                      std::nullopt,
                      {}};
    event.participant_fact =
        ParticipantExecutionFact{ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                                 ParticipantEffectKnowledge::kNoEffect, std::nullopt};
    deliver_event(event);
  }

  void emit_error(ErrorDTO error) {
    ExecutionState current;
    {
      std::lock_guard lock(status_mutex_);
      status.last_error = ErrorCode::kExecutionAborted;
    }
    current = get_status().state;
    const KernelEvent event{KernelEventType::kErrorRaised,
                            next_execution_id,
                            0,
                            current,
                            current,
                            std::move(error),
                            {}};
    const auto snapshot = CallbackSnapshot();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  void emit_collision(std::map<std::string, std::string> details = {}) {
    details.try_emplace("reason", "runtime_collision_detected");
    details.try_emplace("in_collision", "true");
    const auto current = get_status().state;
    const KernelEvent event{KernelEventType::kCollisionDetected,
                            next_execution_id,
                            0,
                            current,
                            current,
                            std::nullopt,
                            std::move(details)};
    const auto snapshot = CallbackSnapshot();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  void notify_prepared_motion() {
    const KernelEvent event{
        KernelEventType::kParticipantPrepared, next_execution_id, 0, {}, {}, std::nullopt, {}};
    const auto snapshot = CallbackSnapshot();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  std::atomic<int> execute_calls{};
  std::function<void()> subscribe_hook;
  std::function<void()> unsubscribe_hook;
  std::atomic<int> prepare_motion_calls{};
  mutable std::atomic<int> observe_prepared_motion_calls{};
  mutable std::atomic<int> status_read_calls{};
  std::atomic<int> dispatch_prepared_motion_calls{};
  std::atomic<int> cancel_prepared_motion_calls{};
  mutable std::atomic<int> capture_replacement_source_calls{};
  std::atomic<int> commit_replacement_calls{};
  std::atomic<int> replacement_owner_commits{};
  /// OD-175 test points and knobs of the take-over hand-over: after "prove"
  /// (before "decide"), before "apply" (Dc) and after "apply" (Ap); a refusal
  /// of "prove"; a lookahead claim that makes the "decide" step's close fail.
  std::function<void()> after_prove_hook;
  std::function<void()> before_apply_hook;
  std::function<void()> after_apply_hook;
  std::optional<ErrorDTO> prove_refusal;
  /// Refusal of PrepareColdMotionAfterStop (cold activation).
  std::optional<ErrorDTO> prepare_cold_refusal;
  std::atomic<bool> replacement_hazard_claimed{false};
  std::atomic<int> close_replacement_hazard_calls{};
  std::uint64_t last_closed_hazard_generation{};
  std::atomic<int> retire_cancelled_replacement_calls{};
  std::atomic<int> prepare_cold_motion_calls{};
  std::atomic<int> cold_participant_preparations{};
  std::atomic<int> takeover_stop_stage_calls{};
  std::atomic<int> prepare_command_calls{};
  mutable std::atomic<int> observe_prepared_command_calls{};
  std::atomic<int> dispatch_prepared_command_calls{};
  std::atomic<int> cancel_prepared_command_calls{};
  std::atomic<int> preview_calls{};
  std::atomic<int> command_calls{};
  mutable std::atomic<int> validate_command_calls{};
  std::function<void()> validate_command_hook;
  GripperActuationKind configured_gripper_actuation{GripperActuationKind::kPositionCommanded};
  GripperOutputMode configured_gripper_output{GripperOutputMode::kCommanded};
  mutable std::atomic<int> get_gripper_status_calls{};
  std::atomic<int> stop_gripper_calls{};
  std::atomic<int> reset_gripper_calls{};
  std::atomic<int> stop_calls{};
  std::atomic<int> translate_stop_stage_calls{};
  std::atomic<int> certified_reset_calls{};
  std::atomic<int> addressed_stop_fence_calls{};
  std::atomic<std::uint64_t> addressed_stop_fence_execution_id{};
  std::atomic<int> addressed_stop_fence_completions{};
  /// Stop stages already issued when call (2) ran (0: before the lane publish).
  int stage_calls_at_fence_completion{-1};
  std::uint64_t last_fence_completion_execution_id{};
  std::uint8_t last_fence_completion_gates{};
  std::atomic<int> participant_stop_settlement_calls{};
  /// The fence reports a committed send still in flight (`send_outcome_pending`).
  std::atomic<bool> fence_send_outcome_pending{false};
  std::atomic<bool> last_settlement_prevented_before_dispatch{false};
  std::atomic<int> acknowledge_calls{};
  std::atomic<int> runtime_scale_calls{};
  mutable std::atomic<int> recovery_assessment_calls{};
  std::atomic<int> recovery_action_calls{};
  std::atomic<int> retire_rebuilt_unknown_calls{};
  bool rebuilt_unknown_retirement_allowed{};
  std::uint64_t rebuilt_unknown_source_id{};
  std::uint64_t rebuilt_unknown_candidate_id{};
  std::uint64_t next_execution_id{41U};
  Result<std::uint64_t> next_execute_result = Result<std::uint64_t>::Ok(41U);
  bool exact_owner_notification_accept{true};
  std::string expected_owner_notification_gripper;
  GripperCommandResultDTO next_command_result{true, 73U, 9U, std::nullopt};
  Result<void> next_command_validation = Result<void>::Ok();
  StopResultDTO next_stop_result{true, std::nullopt};
  // Immutable per-call script for concurrent stage tests.
  std::function<StopResultDTO(int)> stop_result_for_call;
  std::optional<JsonValueDTO> next_stop_evidence;
  ResetResultDTO next_reset_result{true, std::nullopt};
  AcknowledgeCompletedResultDTO next_acknowledge_result{true, std::nullopt};
  template <typename Mutator>
  void UpdateRuntimeScale(Mutator&& mutate) {
    std::lock_guard lock(runtime_scale_mutex_);
    std::forward<Mutator>(mutate)(runtime_scale);
  }
  [[nodiscard]] RuntimeScaleMechanismStatusDTO RuntimeScaleSnapshot() const {
    std::lock_guard lock(runtime_scale_mutex_);
    return runtime_scale;
  }
  [[nodiscard]] std::optional<ResolvedRuntimeScaleCommandDTO> RuntimeScaleCommandSnapshot() const {
    std::lock_guard lock(runtime_scale_mutex_);
    return last_runtime_scale_command;
  }
  void SetRuntimeScaleCommand(std::optional<ResolvedRuntimeScaleCommandDTO> command) {
    std::lock_guard lock(runtime_scale_mutex_);
    last_runtime_scale_command = std::move(command);
  }
  void SetRuntimeScaleResult(Result<void> result) {
    std::lock_guard lock(runtime_scale_mutex_);
    next_runtime_scale_result = std::move(result);
  }
  void SetRuntimeScaleEffectHook(std::function<void()> hook) {
    std::lock_guard lock(runtime_scale_mutex_);
    runtime_scale_effect_hook = std::move(hook);
  }
  [[nodiscard]] RuntimeScaleMechanismAvailability runtime_scale_availability()
      const noexcept override {
    return scale_availability.load();
  }
  std::atomic<RuntimeScaleMechanismAvailability> scale_availability{
      RuntimeScaleMechanismAvailability::kAvailable};
  mutable std::mutex runtime_scale_mutex_;
  Result<void> next_runtime_scale_result = Result<void>::Ok();
  RuntimeScaleMechanismStatusDTO runtime_scale{true, false, 1.0, 1.0, 0.0, 1U, 1, std::nullopt};
  ExecutionStatus status{};
  ExecutionState ended_phase{ExecutionState::kIdle};
  std::optional<ErrorDTO> ended_error;
  std::optional<ExecutionStatus> status_at_stop_fence;
  mutable std::mutex status_mutex_;
  mutable std::mutex prepared_motion_mutex_;
  mutable std::mutex prepared_command_mutex_;
  std::optional<KernelEvent> event_during_execute;
  bool defer_prepare_completion{};
  bool supports_motion_replacement{};
  bool cold_backend_prepared{};
  std::optional<StopEventId> settled_stop_event_id;
  std::uint64_t settled_stop_execution_id{};
  std::uint64_t next_replacement_id{1001U};
  std::uint64_t replacement_execution_generation{1U};
  std::atomic<PreparedMotionReplacementPhaseDTO> replacement_phase{
      PreparedMotionReplacementPhaseDTO::kPreparing};
  PreparedMotionReplacementPhaseDTO replacement_dispatch_phase{
      PreparedMotionReplacementPhaseDTO::kPossiblySent};
  /// A replacement send whose handle never returned: no verified source.
  bool replacement_send_receipt_missing{};
  bool prepare_bounded_time_law{};
  /// Optional prepared trajectory waypoints and bounds (the binding's joints).
  std::optional<TrajectoryDTO> prepared_trajectory;
  /// Optional real execution backend: the prepared participant is prepared on
  /// and dispatched through it, as the participant Kernel does.
  IExecutionBackend* execution_backend{};
  bool defer_prepared_dispatch_effect{};
  ExecutionState prepared_cancel_terminal_state{ExecutionState::kStopped};
  std::optional<ErrorDTO> prepare_failure;
  std::optional<ErrorDTO> prepare_call_failure;
  std::optional<ErrorDTO> prepare_command_failure;
  std::optional<ErrorDTO> prepared_dispatch_event_error;
  std::optional<ErrorDTO> participant_stop_settlement_error;
  bool return_mismatched_participant_stop_settlement{};
  bool settle_contained_failure_as_ack{};
  std::optional<ParticipantExecutionFact> addressed_stop_terminal_fact_override;
  std::function<void()> acknowledge_effect_hook;
  std::function<void()> execute_return_hook;
  std::function<void()> reset_effect_hook;
  std::function<void()> prepare_command_hook;
  std::function<void()> prepare_motion_hook;
  std::function<void()> cancel_prepared_motion_hook;
  std::function<void()> cancel_prepared_command_hook;
  mutable std::function<void()> observe_prepared_motion_hook;
  mutable std::function<void()> observe_prepared_command_hook;
  /// Runs on every status read, before the status is sampled.
  mutable std::function<void()> status_read_hook;
  std::optional<ErrorDTO> observe_prepared_command_failure;
  std::function<void()> dispatch_prepared_command_hook;
  std::function<void()> translate_stop_stage_hook;
  std::function<void()> participant_stop_settlement_hook;
  std::function<void()> runtime_scale_effect_hook;
  mutable std::function<void()> recovery_assessment_hook;
  std::function<void()> recovery_action_hook;
  bool throw_during_recovery_assessment{};
  bool throw_during_recovery{};
  KernelRecoveryAssessmentDTO recovery_assessment{};
  std::optional<KernelRecoveryActionResultDTO> next_recovery_result;
  GripperCommandStatusDTO gripper_status{};
  [[nodiscard]] std::optional<ExecuteRequestDTO> LastExecuteSnapshot() const {
    std::lock_guard lock(last_execute_mutex_);
    return last_execute;
  }
  mutable std::mutex last_execute_mutex_;
  std::optional<ExecuteRequestDTO> last_execute;
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> last_prepared_request;
  mutable std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> prepared_snapshot;
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> replacement_request;
  std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> replacement_snapshot;
  /// A spring successor attached to the sent replacement (OD-285 test).
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> attached_cold_request;
  std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> attached_cold_snapshot;
  std::shared_ptr<const PreparedOperationParticipantDTO> last_dispatched_participant;
  std::optional<ResolvedPreparedCapabilityCommandExecutionRequestDTO> last_prepared_command_request;
  mutable std::optional<ResolvedPreparedCapabilityCommandSnapshotDTO> prepared_command_snapshot;
  std::optional<ExecuteRequestDTO> last_preview;
  std::optional<ResolvedRuntimeScaleCommandDTO> last_runtime_scale_command;
  /// OD-72: each speed setting the Kernel stored as this participant's ramp
  /// target (value, sequence).
  std::vector<std::pair<double, std::uint64_t>> stored_runtime_scale_settings;
  std::optional<GripperCommandRequestDTO> last_command;
  [[nodiscard]] std::optional<ResolvedParticipantStopStageDTO> LastStopStageSnapshot() const {
    std::lock_guard lock(last_stop_mutex_);
    return last_stop_stage;
  }
  [[nodiscard]] std::optional<RemainingStopBudgetDTO> LastStopBudgetSnapshot() const {
    std::lock_guard lock(last_stop_mutex_);
    return last_stop_budget;
  }
  mutable std::mutex last_stop_mutex_;
  std::optional<ResolvedParticipantStopStageDTO> last_stop_stage;
  std::optional<RemainingStopBudgetDTO> last_stop_budget;
  std::optional<KernelRecoveryActionRequestDTO> last_recovery_request;
  std::vector<std::string>* timeline{};
  std::mutex* timeline_mutex{};

 private:
  static std::string Digest(char digit) { return "sha256:" + std::string(64U, digit); }

  Result<PreparedOperationParticipantDTO> MakePreparedMotionParticipant(
      const ResolvedPreparedMotionExecutionRequestDTO& authority) {
    const auto& request = authority.execution_request;
    const auto& binding = authority.binding.value();
    const auto& control = *binding.control_path;
    std::vector<CoordinatePositionDTO> positions;
    positions.reserve(binding.resources.size());
    for (const auto& resource : binding.resources) {
      positions.emplace_back(resource.coordinate_kind == CoordinateKind::kAngular
                                 ? CoordinatePositionDTO{AngularPositionDTO{0.0}}
                                 : CoordinatePositionDTO{LinearPositionDTO{0.0}});
    }
    ControlledStateDTO start{binding.controllable_component_id,
                             binding.command_target_id,
                             control.control_domain_id,
                             control.control_domain_revision,
                             control.control_binding_id,
                             control.control_binding_revision,
                             control.controller_topology_id,
                             control.topology_revision,
                             control.control_route_id,
                             control.active_control_configuration_id,
                             control.active_control_configuration_generation,
                             request.binding.resources,
                             request.binding.model_group_binding.compiled_joint_names,
                             std::move(positions),
                             std::nullopt};
    ProjectionRevisionDTO projection{
        binding.retained_bundle_id, binding.graph_revision,
        SceneRevision{SceneId(Digest('a')), 1U, ContentDigest(Digest('b'))}};
    PlanRequestDTO plan_request{
        request.binding.operation_id,
        request.binding.participant_id,
        request.binding.program_revision,
        request.binding.program_binding_id,
        request.binding.program_run_id,
        MoveId(request.move_id),
        request.binding.controllable_component_id,
        request.binding.component_type_id,
        request.binding.component_type_revision,
        request.binding.command_target_id,
        request.binding.capability_id,
        request.binding.capability_definition_revision,
        request.binding.planner_capability_binding,
        request.binding.model_bindings,
        request.binding.model_group_binding,
        request.binding.control_domain_id,
        request.binding.control_domain_revision,
        request.binding.control_binding_id,
        request.binding.control_binding_revision,
        request.binding.control_interface_contract_id,
        request.binding.control_interface_revision,
        request.binding.controller_topology_id,
        request.binding.topology_revision,
        request.binding.control_route_id,
        request.binding.activation_set_id,
        request.binding.active_control_configuration_id,
        request.binding.active_control_configuration_generation,
        request.binding.provider_instance_id,
        request.binding.provider_manifest_id,
        request.binding.provider_manifest_revision,
        request.binding.provider_runtime_epoch,
        request.binding.runtime_unit_bindings,
        request.binding.resources,
        request.action_span,
        request.binding.base_frame,
        request.binding.tcp_frame,
        start,
        request.motion,
        request.tolerances,
        request.speed_profile,
        request.planner,
        request.per_resource_dynamics_limits,
        request.timeouts.planning_timeout_s,
        0U,
        ProjectionLeaseBindingDTO{projection, ProjectionLeaseId("lease.test")},
        request.binding.component_attachment_revisions,
        request.binding.carried_object_attachment_revisions,
        {},
        {}};
    std::vector<double> zeros(request.binding.resources.size(), 0.0);
    TrajectoryDTO trajectory{request.binding.resources,
                             request.binding.model_group_binding.compiled_joint_names,
                             {{0.0, zeros, {}, {}}, {1.0, zeros, {}, {}}}};
    if (prepared_trajectory.has_value()) {
      trajectory.waypoints = prepared_trajectory->waypoints;
      trajectory.runtime_scale_bounds = prepared_trajectory->runtime_scale_bounds;
    } else if (prepare_bounded_time_law) {
      trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0, 1.0, 1.0};
      for (auto& waypoint : trajectory.waypoints) {
        waypoint.velocities_rad_s = zeros;
        waypoint.accelerations_rad_s2 = zeros;
      }
    }
    PlanResultDTO planning_result{trajectory, std::nullopt, projection};
    CollisionWorldRevision world{1U, Digest('c')};
    CollisionDecision decision;
    decision.accepted = true;
    decision.severity = CollisionDecisionSeverity::kNone;
    decision.action = CollisionDecisionAction::kDispatch;
    decision.collision_world_revision = world;

    std::vector<PreparedMotionInterruptScopeDTO> scopes;
    for (const auto& scope : request.motion_interrupt_scopes) {
      const auto action = std::find_if(
          authority.interrupt_scope_actions.begin(), authority.interrupt_scope_actions.end(),
          [&](const auto& value) { return value.scope_id.value() == scope.scope_id; });
      const bool desired =
          action != authority.interrupt_scope_actions.end() &&
          action->preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal;
      const bool fault = action != authority.interrupt_scope_actions.end() &&
                         action->preemption_action == InterruptPreemptionAction::kPreemptTrajectory;
      if ((!desired && !fault) || desired != scope.stop_profile.has_value() ||
          (fault && !authority.ordinary_stop_plan)) {
        return Result<PreparedOperationParticipantDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "fake prepared scope has no Stop profile",
                     {{"reason", "stop_profile_missing"}}});
      }
      PreparedParticipantStopPlanContextDTO context{
          request.binding.operation_id,
          authority.operation_revision,
          request.binding.participant_id,
          request.binding.program_revision,
          request.binding.program_binding_id,
          request.binding.program_run_id,
          scope.stop_profile,
          ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
          scope.scope_id,
          scope.node_category};
      auto stop =
          fault ? Result<PreparedParticipantStopPlanDTO>::Ok(*authority.ordinary_stop_plan)
                : MakePreparedParticipantStopPlan(
                      authority.stop_readiness.value().resolved_stop_base_plan, std::move(context),
                      binding.resources, authority.retained_default_stop_limits);
      if (!stop.ok()) {
        return Result<PreparedOperationParticipantDTO>::Err(stop.error());
      }
      const auto entry = ResolvePreparedIoRuleStopEntry(stop.value(), action->stop_entry_stage_id);
      if (!entry.ok()) return Result<PreparedOperationParticipantDTO>::Err(entry.error());
      const auto success_through = ResolvePreparedIoRuleSuccessThrough(
          stop.value(), entry.value(), action->success_through_stage_id);
      if (!success_through.ok())
        return Result<PreparedOperationParticipantDTO>::Err(success_through.error());
      scopes.push_back({MotionInterruptScopeId(scope.scope_id), InterruptPolicyId(scope.policy_id),
                        scope.activation_mode, scope.arm_timeout_ns, scope.required,
                        scope.stop_profile, scope.node_category, scope.metadata,
                        std::move(stop.value()), action->preemption_action, entry.value(),
                        success_through.value()});
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const auto& left, const auto& right) { return left.scope_id < right.scope_id; });

    PreparedMotionExecutionDTO payload{
        MoveId(request.move_id),
        plan_request,
        trajectory,
        start,
        {std::move(planning_result), 0U, request.binding.planner_capability_binding},
        {ValidationRevision("validation.test"),
         ValidationResultDTO{true, std::nullopt,
                             TrajectoryValidationMetricsDTO{
                                 trajectory.waypoints.back().time_from_start_s,
                                 std::vector<double>(trajectory.joint_names.size(), 0.5),
                                 std::vector<double>(trajectory.joint_names.size(), 0.0), 0.1}},
         world, CollisionPolicyRevision{1U, Digest('d')}, decision},
        request.collision_rules,
        authority.requested_tcp_linear_speed_mps,
        authority.deployment_maximum_tcp_linear_speed_mps,
        std::move(scopes),
        request.timeouts.execution_timeout_s};

    std::vector<ResourceId> resources = request.binding.resources;
    std::sort(resources.begin(), resources.end());
    std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider;
    if (binding.capability_provider_path) {
      const auto& provider = *binding.capability_provider_path;
      capability_provider = PreparedCapabilityProviderRuntimeOccurrenceDTO{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          {provider.provider_instance_id, provider.provider_manifest_id,
           provider.provider_manifest_revision, provider.provider_runtime_unit_id,
           provider.provider_runtime_epoch}};
    }
    PreparedOperationParticipantDTO candidate{
        ContentDigest("pending"),
        request.binding.operation_id,
        authority.operation_revision,
        request.binding.participant_id,
        binding.program_revision,
        binding.program_binding_id,
        request.binding.program_run_id,
        binding.retained_bundle_id,
        binding.graph_revision,
        binding.controllable_component_id,
        authority.stop_readiness.value().component_lifecycle_generation,
        binding.component_type_id,
        binding.component_type_revision,
        binding.command_target_id,
        binding.capability_id,
        binding.capability_definition_revision,
        binding.model_bindings,
        binding.frame_bindings,
        control.control_domain_id,
        control.control_domain_revision,
        control.control_binding_id,
        control.control_binding_revision,
        control.control_interface_contract_id,
        control.control_interface_revision,
        control.controller_topology_id,
        control.topology_revision,
        control.control_route_id,
        control.activation_set_id,
        {control.provider_instance_id, control.provider_manifest_id,
         control.provider_manifest_revision, control.provider_runtime_unit_id,
         control.provider_runtime_epoch},
        std::move(capability_provider),
        std::move(resources),
        PreparedParticipantFocusedPortDTO{control.provider_runtime_unit_id},
        {},
        {},
        control.active_control_configuration_id,
        control.active_control_configuration_generation,
        binding.runtime_unit_epochs,
        control.stop_evidence_policy_id,
        control.stop_evidence_policy_revision,
        *authority.ordinary_stop_plan,
        std::move(payload),
        authority.fault_stop_plan
            ? std::optional<PreparedParticipantStopPlanDTO>{*authority.fault_stop_plan}
            : std::nullopt};
    return MakePreparedOperationParticipant(std::move(candidate), authority.binding,
                                            authority.stop_readiness,
                                            authority.retained_default_stop_limits,
                                            authority.stop_node_category);
  }

  Result<void> CompletePreparation() {
    std::optional<KernelEvent> event;
    {
      std::lock_guard lock(prepared_motion_mutex_);
      if (!last_prepared_request.has_value()) {
        return Result<void>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                     "fake preparation has no retained request",
                     {{"reason", "prepared_motion_request_missing"}}});
      }
      if (prepared_snapshot.has_value() &&
          prepared_snapshot->state ==
              ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
        return Result<void>::Ok();
      }
      if (prepare_failure.has_value()) {
        prepared_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
            next_execution_id,
            ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed,
            {},
            std::nullopt,
            prepare_failure};
        const auto current = get_status().state;
        event.emplace(KernelEvent{KernelEventType::kErrorRaised,
                                  next_execution_id,
                                  0,
                                  current,
                                  current,
                                  prepare_failure,
                                  {{"reason", "prepared_motion_prepare_failed"}}});
      } else {
        auto participant = MakePreparedMotionParticipant(*last_prepared_request);
        if (!participant.ok()) {
          prepared_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
              next_execution_id,
              ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed,
              {},
              std::nullopt,
              participant.error()};
          return Result<void>::Err(participant.error());
        }
        auto retained =
            std::make_shared<const PreparedOperationParticipantDTO>(std::move(participant.value()));
        auto backend_prepare =
            MakePrepareParticipantResult(*retained, PrepareParticipantResultStateDTO::kPrepared);
        if (!backend_prepare.ok()) {
          return Result<void>::Err(backend_prepare.error());
        }
        prepared_snapshot = ResolvedPreparedMotionExecutionSnapshotDTO{
            next_execution_id, ResolvedPreparedMotionExecutionStateDTO::kPrepared, retained,
            std::move(backend_prepare.value()), std::nullopt};
        event.emplace(KernelEvent{
            KernelEventType::kParticipantPrepared,
            next_execution_id,
            0,
            {},
            {},
            std::nullopt,
            {{"prepared_contract_digest", retained->prepared_contract_digest.value()}}});
      }
    }
    const auto snapshot = CallbackSnapshot();
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(*event);
    }
    return Result<void>::Ok();
  }

  // Takes the literal, not a std::string: building the string only when a
  // timeline records it keeps Stop fence calls (1) and (2) allocation-free
  // (design §9.2 test).
  void Trace(const char* value) {
    if (timeline != nullptr) {
      std::unique_lock lock(timeline_mutex != nullptr ? *timeline_mutex : fallback_timeline_mutex_);
      timeline->emplace_back(value);
    }
  }

  [[nodiscard]] std::map<std::uint64_t, EventCallback> CallbackSnapshot() const {
    std::lock_guard lock(callbacks_mutex_);
    return callbacks;
  }

  std::mutex fallback_timeline_mutex_;
  mutable std::mutex callbacks_mutex_;
  std::uint64_t next_subscription_id{1U};
  std::map<std::uint64_t, EventCallback> callbacks;
};

class FakeOperationJournal final : public IOperationJournal {
 public:
  Result<void> BeginStart(ApplicationStartId, std::int64_t, std::size_t) override {
    if (begin_start_error) return Result<void>::Err(*begin_start_error);
    calls = {"begin"};
    states.clear();
    return Result<void>::Ok();
  }

  /// The writer thread's batch: each record goes to the per-kind recorder
  /// below, in order. `Block()` holds the writer here (a stalled disk).
  JournalBatchResult WriteBatch(std::span<const JournalRecord> records,
                                const std::uint64_t records_dropped) override {
    {
      std::unique_lock lock(block_mutex_);
      ++batches_entered;
      block_changed_.notify_all();
      block_changed_.wait(lock, [this] { return !blocked_; });
    }
    JournalBatchResult result;
    std::vector<std::string> kinds;
    for (std::size_t index = 0U; index < records.size(); ++index) {
      const auto& record = records[index];
      if (record.skipped) continue;
      Result<std::uint64_t> written = Result<std::uint64_t>::Ok(0U);
      if (const auto* accepted = std::get_if<JournalOperationAccepted>(&record.payload)) {
        kinds.emplace_back("operation_accepted");
        written = RecordOperationAccepted(accepted->intent, accepted->accepted, accepted->state);
      } else if (const auto* control = std::get_if<JournalControlAccepted>(&record.payload)) {
        kinds.emplace_back("control_accepted");
        written = RecordControlAccepted(control->request, control->accepted, control->state);
      } else if (const auto* state = std::get_if<JournalOperationState>(&record.payload)) {
        kinds.emplace_back(std::string("state:") + state->transition_reason);
        written = RecordOperationState(state->state,
                                       state->scale_status ? &*state->scale_status : nullptr,
                                       state->transition_reason, state->prepared_motion_diagnostics);
      } else if (const auto* scale = std::get_if<JournalRuntimeScaleAccepted>(&record.payload)) {
        kinds.emplace_back("runtime_scale_accepted");
        written = RecordRuntimeScaleAccepted(scale->request, scale->accepted, scale->state,
                                             scale->scale_status);
      } else if (const auto* row = std::get_if<JournalStopEventRow>(&record.payload)) {
        kinds.emplace_back(row->completed ? "stop_event_completed" : "stop_event_accepted");
        written = row->completed ? RecordStopEventCompleted(*row->record)
                                 : RecordStopEventAccepted(*row->record);
      } else if (const auto* rule = std::get_if<IoRuleStateChangeDTO>(&record.payload)) {
        kinds.emplace_back("interrupt_rule_state_changed");
        written = RecordInterruptRuleStateChanged(*rule);
      } else if (const auto* claim = std::get_if<JournalCellClaimAcquired>(&record.payload)) {
        kinds.emplace_back("cell_admission_acquired");
        written = RecordCellAdmissionClaimAcquired(claim->claim);
      } else if (const auto* released = std::get_if<JournalCellClaimReleased>(&record.payload)) {
        kinds.emplace_back("cell_admission_released");
        written = RecordCellAdmissionClaimReleased(released->claim, released->release);
      } else if (const auto* tcp = std::get_if<ActiveTcpChangeDTO>(&record.payload)) {
        kinds.emplace_back("active_tcp_changed");
        written = RecordActiveTcpChanged(*tcp);
      }
      if (!written.ok()) result.failures.push_back({index, written.error()});
    }
    std::lock_guard lock(mutex);
    batches.push_back(std::move(kinds));
    dropped_records += records_dropped;
    return result;
  }

  /// Stalls the writer at its next batch until `Unblock()`.
  void Block() {
    std::lock_guard lock(block_mutex_);
    blocked_ = true;
  }
  void Unblock() {
    {
      std::lock_guard lock(block_mutex_);
      blocked_ = false;
    }
    block_changed_.notify_all();
  }
  /// With `hold_after_failure` set, the next injected failure also stalls
  /// the writer at its next batch, so no later clean batch ends the loss
  /// episode before the test reads the fault; `Unblock()` lets it go on.
  void HoldIfAsked() {
    if (hold_after_failure.exchange(false)) Block();
  }
  /// Waits until the writer entered `count` batches (it may be held there).
  bool WaitBatchesEntered(const std::size_t count) {
    std::unique_lock lock(block_mutex_);
    return block_changed_.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return batches_entered >= count; });
  }

  Result<std::uint64_t> RecordOperationAccepted(const OperationIntentDTO&,
                                                const OperationAcceptedDTO&,
                                                const OperationStateDTO& state) {
    if (operation_acceptance_hook) {
      operation_acceptance_hook();
    }
    return Record("operation_accepted", state);
  }

  Result<std::uint64_t> RecordControlAccepted(const OperationControlRequestDTO&,
                                              const OperationControlAcceptedDTO&,
                                              const OperationStateDTO& state) {
    if (control_acceptance_hook) {
      control_acceptance_hook();
    }
    return Record("control_accepted", state);
  }

  Result<std::uint64_t> RecordStopEventAccepted(const StopEventRecord& record) noexcept {
    if (stop_acceptance_hook) {
      stop_acceptance_hook();
    }
    std::lock_guard lock(mutex);
    calls.push_back("stop_event_accepted");
    if (timeline != nullptr) {
      std::unique_lock timeline_lock(timeline_mutex != nullptr ? *timeline_mutex
                                                               : fallback_timeline_mutex_);
      timeline->push_back("journal:stop_event_accepted");
    }
    last_stop_event_accepted = record;
    projected_stop_acceptances.push_back(record.event_id);
    ++stop_acceptance_calls;
    if (fail_next_kind == "stop_event_accepted") {
      fail_next_kind.reset();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordStopEventCompleted(const StopEventRecord& record) noexcept {
    std::lock_guard lock(mutex);
    calls.push_back("stop_event_completed");
    last_stop_event_completed = record;
    projected_stop_terminals.push_back(record.event_id);
    ++stop_terminal_calls;
    if (fail_next_kind == "stop_event_completed") {
      fail_next_kind.reset();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordActiveTcpChanged(const ActiveTcpChangeDTO& change) {
    std::lock_guard lock(mutex);
    calls.push_back("active_tcp_changed");
    if (fail_next_kind == "active_tcp_changed") {
      fail_next_kind.reset();
      HoldIfAsked();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    active_tcp_changes.push_back(change);
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordInterruptRuleStateChanged(const IoRuleStateChangeDTO& change) {
    RunRecordHook("interrupt_rule_state_changed");
    std::lock_guard lock(mutex);
    interrupt_rule_state_changes.push_back(change);
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordRuntimeScaleAccepted(const RuntimeScaleRequestDTO&,
                                                   const RuntimeScaleAcceptedDTO&,
                                                   const OperationStateDTO& state,
                                                   const RuntimeScaleStatusDTO&) {
    return Record("runtime_scale_accepted", state);
  }

  Result<std::uint64_t> RecordOperationState(
      const OperationStateDTO& state, const RuntimeScaleStatusDTO*,
      const char* transition_reason, std::span<const PreparedMotionDiagnosticsDTO>) {
    return Record(std::string("state:") + transition_reason, state);
  }

  Result<std::uint64_t> RecordCellAdmissionClaimAcquired(const CellAdmissionClaimDTO&) {
    if (cell_admission_acquire_hook) {
      cell_admission_acquire_hook();
    }
    std::lock_guard lock(mutex);
    calls.push_back("cell_admission_acquired");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordCellAdmissionClaimReleased(const CellAdmissionClaimDTO&,
                                                         const CellAdmissionClaimReleaseDTO&) {
    std::lock_guard lock(mutex);
    if (std::exchange(fail_next_cell_release, false)) {
      HoldIfAsked();
      return Result<std::uint64_t>::Err(
          ErrorDTO{"operation_journal_test_failure", "injected claim release failure", {}});
    }
    calls.push_back("cell_admission_released");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  // Tests set the hook while the Kernel's journal writer runs: it is set and
  // read under `mutex`, and called outside it so that a blocked callback does
  // not block unrelated journal projection.
  void SetRecordHook(std::function<void(std::string_view)> hook) {
    std::lock_guard lock(mutex);
    record_hook_ = std::move(hook);
  }

  // The next record of this kind fails; set while the writer runs, so it is
  // set and read under `mutex`, like the record hook.
  void SetFailNextKind(std::optional<std::string> kind) {
    std::lock_guard lock(mutex);
    fail_next_kind = std::move(kind);
  }

  void RunRecordHook(std::string_view kind) {
    std::function<void(std::string_view)> hook;
    {
      std::lock_guard lock(mutex);
      hook = record_hook_;
    }
    if (hook) hook(kind);
  }

  Result<std::uint64_t> Record(std::string kind, const OperationStateDTO& state) {
    RunRecordHook(kind);
    std::lock_guard lock(mutex);
    calls.push_back(kind);
    if (timeline != nullptr) {
      std::unique_lock timeline_lock(timeline_mutex != nullptr ? *timeline_mutex
                                                               : fallback_timeline_mutex_);
      timeline->push_back("journal:" + kind);
    }
    if (fail_next_kind == kind) {
      fail_next_kind.reset();
      HoldIfAsked();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    states.push_back(state);
    if (throw_after_capture_kind == kind) {
      throw_after_capture_kind.reset();
      throw std::runtime_error("journal reply lost after capture");
    }
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  std::optional<ErrorDTO> begin_start_error;
  std::atomic_bool hold_after_failure{};
  std::optional<std::string> throw_after_capture_kind;
  bool fail_next_cell_release{};
  std::vector<std::string> calls;
  std::atomic<int> stop_acceptance_calls{};
  std::atomic<int> stop_terminal_calls{};
  std::vector<OperationStateDTO> states;
  /// The written kinds of each batch, in writer order.
  std::vector<std::vector<std::string>> batches;
  std::uint64_t dropped_records{};
  std::function<void()> operation_acceptance_hook;
  std::function<void()> control_acceptance_hook;
  std::function<void()> stop_acceptance_hook;
  std::function<void()> cell_admission_acquire_hook;
  std::vector<IoRuleStateChangeDTO> interrupt_rule_state_changes;
  std::vector<ActiveTcpChangeDTO> active_tcp_changes;
  std::vector<StopEventId> projected_stop_acceptances;
  std::vector<StopEventId> projected_stop_terminals;
  std::optional<StopEventRecord> last_stop_event_accepted;
  std::optional<StopEventRecord> last_stop_event_completed;
  std::uint64_t next_sequence{1U};
  std::vector<std::string>* timeline{};
  std::mutex* timeline_mutex{};
  std::mutex fallback_timeline_mutex_;
  std::mutex mutex;

 private:
  std::function<void(std::string_view)> record_hook_;
  std::optional<std::string> fail_next_kind;
  std::mutex block_mutex_;
  std::condition_variable block_changed_;
  bool blocked_{};
  std::size_t batches_entered{};
};

class FakeResourceAdmissionGate final : public IResourceOperationAdmissionGate {
 public:
  FakeResourceAdmissionGate() : admission_mutex_(std::make_shared<std::recursive_mutex>()) {}

  [[nodiscard]] std::shared_ptr<std::recursive_mutex> shared_admission_mutex() const override {
    return admission_mutex_;
  }

  [[nodiscard]] Result<void> reserve(ResourceSlotTable& table, const ResourceSlotOwnerDTO& owner,
                                     const std::vector<ResourceId>& resources) override {
    std::lock_guard lock(*admission_mutex_);
    ++reserve_calls;
    if (reject_reservation) {
      return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                        "test configuration fence is held",
                                        {{"reason", "configuration_fence_held"}}});
    }
    return table.reserve(owner, resources);
  }

  [[nodiscard]] Result<void> reserve_batch(
      ResourceSlotTable& table,
      const std::vector<ResourceSlotReservationOwnerDTO>& owners) override {
    std::lock_guard lock(*admission_mutex_);
    ++reserve_calls;
    if (reject_reservation) {
      return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                        "test configuration fence is held",
                                        {{"reason", "configuration_fence_held"}}});
    }
    return table.reserve_batch(owners);
  }

  [[nodiscard]] std::vector<ResourceId> current_fenced_resources(
      const std::vector<ResourceId>& resources) const override {
    if (!reject_reservation) return {};
    if (!other_fence_resources.has_value()) return resources;
    std::vector<ResourceId> fenced;
    for (const auto& resource : resources) {
      const auto in_parent = held_fence_resources.has_value() &&
          std::find(held_fence_resources->begin(), held_fence_resources->end(),
                    resource) != held_fence_resources->end();
      const auto in_other = std::find(other_fence_resources->begin(),
                                      other_fence_resources->end(), resource) !=
                            other_fence_resources->end();
      if (in_parent || in_other) fenced.push_back(resource);
    }
    return fenced;
  }

  [[nodiscard]] bool exact_fence_covers(const ConfigurationFenceId& fence_id,
                                        const std::vector<ResourceId>& resources) const override {
    if (!reject_reservation || !held_fence_id.has_value() || *held_fence_id != fence_id) {
      return false;
    }
    return !held_fence_resources.has_value() ||
           std::all_of(resources.begin(), resources.end(), [&](const auto& resource) {
             return std::find(held_fence_resources->begin(), held_fence_resources->end(),
                              resource) != held_fence_resources->end();
           });
  }
  [[nodiscard]] bool exact_fence_is_held(
      const ConfigurationFenceId& fence_id,
      const ConfigurationFenceScopeDTO& scope) const override {
    return reject_reservation && held_fence_id.has_value() &&
           *held_fence_id == fence_id && held_scope.has_value() &&
           *held_scope == scope;
  }

  bool reject_reservation{true};
  std::optional<ConfigurationFenceId> held_fence_id;
  std::optional<ConfigurationFenceScopeDTO> held_scope;
  std::optional<std::vector<ResourceId>> held_fence_resources;
  std::optional<std::vector<ResourceId>> other_fence_resources;
  int reserve_calls{};

 private:
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
};

FakeOperationJournal default_journal;

[[nodiscard]] std::string test_digest(const char digit) {
  return "sha256:" + std::string(64U, digit);
}

ManagerLivenessRequiredSetDTO EmptyManagerLivenessSet() {
  ManagerLivenessRequiredSetDTO result{CellId("cell.test"),
                                       RetainedBundleId("bundle.test"),
                                       ContentDigest(test_digest('a')),
                                       RuntimeUnitId("manager"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

[[nodiscard]] ResolvedStopBasePlanDTO stop_base_plan_for(
    const RuntimeParticipantBinding& participant, const bool qualify_fast_fallback = false) {
  const auto path =
      std::filesystem::path(MANYFORGE_CORE_TEST_GOLDEN_DIR) / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) {
    bytes.pop_back();
  }
  auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) {
    throw std::logic_error(decoded.error().message);
  }

  auto plan = std::move(decoded.value());
  const auto& binding = participant.value();
  const auto& control = *binding.control_path;
  plan.resolved_stop_base_plan_id = ResolvedStopBasePlanId("pending");
  plan.retained_bundle_id = binding.retained_bundle_id;
  plan.graph_revision = binding.graph_revision;
  plan.control_domain_id = control.control_domain_id;
  plan.control_domain_revision = control.control_domain_revision;
  plan.control_binding_id = control.control_binding_id;
  plan.control_binding_revision = control.control_binding_revision;
  plan.controller_topology_id = control.controller_topology_id;
  plan.topology_revision = control.topology_revision;
  plan.control_route_id = control.control_route_id;
  plan.activation_set_id = control.activation_set_id;
  plan.active_control_configuration_id = control.active_control_configuration_id;
  plan.active_control_configuration_generation = control.active_control_configuration_generation;
  plan.source_runtime_unit_id = control.provider_runtime_unit_id;
  plan.source_runtime_epoch = control.provider_runtime_epoch;
  plan.stop_evidence_policy_id = control.stop_evidence_policy_id;
  plan.stop_evidence_policy_revision = control.stop_evidence_policy_revision;

  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  for (auto& stage : plan.escalation_stages) {
    if (stage.stage_index == 0U) {
      // The codec golden intentionally carries an opaque placeholder. This
      // execution fixture selects the real controlled-Stop schema, so its
      // default-only callers (notably grippers) need a complete typed profile.
      JsonValueDTO::Array default_limits;
      for (const auto& resource : binding.resources) {
        const bool angular = resource.coordinate_kind == CoordinateKind::kAngular;
        default_limits.emplace_back(JsonObjectDTO{
            {"resource_id", JsonValueDTO{resource.resource_id.value()}},
            {"max_deceleration",
             JsonValueDTO{JsonObjectDTO{
                 {"coordinate_kind", JsonValueDTO{angular ? "angular_rad_s2" : "linear_m_s2"}},
                 {angular ? "value_rad_s2" : "value_m_s2", JsonValueDTO{4.0}}}}},
            {"settle_velocity_threshold",
             JsonValueDTO{JsonObjectDTO{
                 {"coordinate_kind", JsonValueDTO{angular ? "angular_rad_s" : "linear_m_s"}},
                 {angular ? "value_rad_s" : "value_m_s", JsonValueDTO{0.02}}}}}});
      }
      stage.default_contract_parameters =
          JsonObjectDTO{{"stop_mode", JsonValueDTO{"soft_on_path"}},
                        {"deceleration_time_s", JsonValueDTO{1.0}},
                        {"jerk_limits_enabled", JsonValueDTO{false}},
                        {"allow_natural_completion_on_short_path", JsonValueDTO{false}},
                        {"per_resource_limits", JsonValueDTO{std::move(default_limits)}}};
      auto select_controlled_stop_contract = [](ProviderContractRefDTO& ref) {
        ref.contract_schema_id = SchemaId(std::string(kMotionControlledStopContractSchemaId));
        ref.contract_schema_revision =
            SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision));
      };
      if (stage.containment_command.provider_contract_ref.has_value()) {
        select_controlled_stop_contract(*stage.containment_command.provider_contract_ref);
      }
      if (stage.command_qualification.controlled_substitution_contract.has_value()) {
        select_controlled_stop_contract(
            stage.command_qualification.controlled_substitution_contract->provider_contract_ref);
        if (qualify_fast_fallback) {
          stage.command_qualification.controlled_substitution_contract->fallback_stop_modes
              .push_back(MotionControlledStopMode::kSpringBackFast);
        }
      }
      for (auto& requirement : stage.evidence_requirements) {
        if (requirement.requirement.provider_contract_ref.has_value()) {
          select_controlled_stop_contract(*requirement.requirement.provider_contract_ref);
        }
      }
    }
    std::visit([&](auto& scope) { scope.activation_set_id = control.activation_set_id; },
               stage.containment_scope);
    const auto capability_target =
        stage.containment_command.command_kind == StopContainmentCommandKind::kRegistered ||
        stage.containment_command.command_kind ==
            StopContainmentCommandKind::kCapabilityCommandCancel ||
        stage.containment_command.command_kind == StopContainmentCommandKind::kSafeOutputCommand ||
        stage.containment_command.command_kind == StopContainmentCommandKind::kLogicalCancel;
    if (capability_target && !binding.supporting_capability_provider_paths.empty()) {
      const auto& provider = binding.supporting_capability_provider_paths.front();
      stage.command_target.provider_instance_id = provider.provider_instance_id;
      stage.command_target.provider_manifest_id = provider.provider_manifest_id;
      stage.command_target.provider_manifest_revision = provider.provider_manifest_revision;
      stage.command_target.runtime_unit_id = provider.provider_runtime_unit_id;
      stage.command_target.provider_runtime_epoch = provider.provider_runtime_epoch;
      stage.command_target.runtime_unit_epoch = provider.provider_runtime_epoch;
    } else {
      stage.command_target.provider_instance_id = control.provider_instance_id;
      stage.command_target.provider_manifest_id = control.provider_manifest_id;
      stage.command_target.provider_manifest_revision = control.provider_manifest_revision;
      stage.command_target.runtime_unit_id = control.provider_runtime_unit_id;
      stage.command_target.provider_runtime_epoch = control.provider_runtime_epoch;
      stage.command_target.runtime_unit_epoch = control.provider_runtime_epoch;
    }
  }
  plan.escalation_stages.front().containment_granularity = control.containment_granularity;
  plan.escalation_stages.front().containment_scope =
      ResolvedStopResourceContainmentScopeDTO{control.activation_set_id, resources};
  auto made = MakeResolvedStopBasePlan(std::move(plan));
  if (!made.ok()) {
    throw std::logic_error(made.error().message + ": " + made.error().details.at("reason"));
  }
  return std::move(made.value());
}

[[nodiscard]] ControllerLifecycleExpectedDomain controller_domain_for(
    const RuntimeParticipantBinding& participant, const ResolvedStopBasePlanDTO& plan,
    std::vector<RuntimeParticipantBinding> domain_members = {}) {
  // Every binding routed in the participant's domain. A single-component
  // domain has only the participant itself.
  if (domain_members.empty()) domain_members.push_back(participant);
  std::sort(domain_members.begin(), domain_members.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.value().control_path->control_binding_id <
           rhs.value().control_path->control_binding_id;
  });
  const auto& binding = participant.value();
  const auto& control = *binding.control_path;
  const ControllerRoleId controller_role("hardware");
  std::vector<ControllerInterfaceInstanceDTO> all_interfaces;
  std::vector<DesiredControlBindingRouteDTO> routes;
  std::vector<ControllerLifecycleExpectedBindingDTO> binding_interfaces;
  std::vector<ControllerLifecycleExpectedEndpointDTO> endpoint_bindings;
  for (const auto& member : domain_members) {
    const auto& path = *member.value().control_path;
    std::vector<ControllerInterfaceInstanceDTO> interfaces;
    for (const auto& resource : member.value().resources) {
      interfaces.push_back(ControllerInterfaceInstanceDTO{resource.resource_id, "position"});
    }
    all_interfaces.insert(all_interfaces.end(), interfaces.begin(), interfaces.end());
    routes.push_back({path.control_binding_id, path.control_binding_revision, path.control_route_id});
    binding_interfaces.push_back({path.control_binding_id, path.control_binding_revision,
                                  path.control_interface_contract_id,
                                  path.control_interface_revision, interfaces, interfaces});
    endpoint_bindings.push_back({path.control_binding_id, path.control_binding_revision,
                                 path.control_route_id, EndpointRoleId("endpoint.command"),
                                 "controller.command", true, false});
  }
  std::sort(all_interfaces.begin(), all_interfaces.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.resource_id < rhs.resource_id;
  });
  ControllerLifecycleExpectedDomainDTO value{
      control.control_domain_id,
      control.control_domain_revision,
      control.controller_topology_id,
      control.topology_revision,
      ProductId("test.control.product"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision(test_digest('1')),
      ContentDigest(test_digest('2')),
      ContentDigest(test_digest('3')),
      plan.source_runtime_unit_id,
      plan.source_runtime_epoch,
      {control.controller_topology_id,
       control.topology_revision,
       {{controller_role,
         {{control.control_interface_contract_id,
           control.control_interface_revision,
           {ControllerClaimDirection::kCommand, ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{control.activation_set_id, {controller_role}, {}}},
       {{control.control_route_id,
         control.control_interface_contract_id,
         control.control_interface_revision,
         {controller_role},
         {control.activation_set_id}}}},
      {control.control_domain_id,
       control.control_domain_revision,
       control.controller_topology_id,
       control.topology_revision,
       control.activation_set_id,
       std::move(routes)},
      std::move(binding_interfaces),
      {{controller_role,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(test_digest('4')),
        "controller_interface.ControllerInterface",
        ContentDigest(test_digest('5')),
        all_interfaces,
        all_interfaces,
        {},
        {}}},
      std::move(endpoint_bindings)};
  auto sealed = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantExecutionRegistration registration(
    const RuntimeParticipantBinding& participant, IResolvedParticipantExecutionEngine& engine,
    const bool include_controlled_fallback = false,
    std::vector<RuntimeParticipantBinding> domain_members = {},
    const bool control_path_stop_targets = false, const bool qualify_fast_fallback = false) {
  auto plan = stop_base_plan_for(participant, qualify_fast_fallback);
  if (control_path_stop_targets) {
    // Every stage and its evidence source on the participant's own control
    // path, as a deployment resolves them for a trajectory controller.
    const auto& control = *participant.value().control_path;
    const auto rebind = [&](ResolvedStopRuntimeTargetDTO& target) {
      target.provider_instance_id = control.provider_instance_id;
      target.provider_manifest_id = control.provider_manifest_id;
      target.provider_manifest_revision = control.provider_manifest_revision;
      target.runtime_unit_id = control.provider_runtime_unit_id;
      target.provider_runtime_epoch = control.provider_runtime_epoch;
      target.runtime_unit_epoch = control.provider_runtime_epoch;
    };
    plan.resolved_stop_base_plan_id = ResolvedStopBasePlanId("pending");
    for (auto& stage : plan.escalation_stages) {
      rebind(stage.command_target);
      for (auto& evidence : stage.evidence_requirements) {
        rebind(evidence.resolved_source.runtime_target);
      }
    }
    auto sealed = MakeResolvedStopBasePlan(std::move(plan));
    if (!sealed.ok()) throw std::logic_error(sealed.error().message);
    plan = std::move(sealed.value());
  }
  if (include_controlled_fallback) {
    auto intermediate = plan.escalation_stages.front();
    intermediate.stage_index = 1U;
    intermediate.stage_kind = StopEscalationStageKind::kControlledFallback;
    intermediate.stop_escalation_stage_id = StopEscalationStageId("stage.intermediate");
    intermediate.command_qualification.controlled_substitution_contract.reset();
    auto& evidence = intermediate.evidence_requirements.front().requirement;
    evidence.evidence_item_id = StopEvidenceItemId("evidence.intermediate.rest");
    plan.escalation_stages.back().stage_index = 2U;
    plan.escalation_stages.insert(plan.escalation_stages.begin() + 1, std::move(intermediate));
    auto sealed = MakeResolvedStopBasePlan(std::move(plan));
    if (!sealed.ok()) throw std::logic_error(sealed.error().message);
    plan = std::move(sealed.value());
  }
  auto expected = controller_domain_for(participant, plan, std::move(domain_members));
  auto readiness = verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO{participant.value().controllable_component_id,
                                         ComponentLifecycleGeneration(1U), expected.value(), plan,
                                         kTestHardStopCompletionTimeoutNs},
      participant, expected);
  if (!readiness.ok()) {
    throw std::logic_error(readiness.error().message + ": " +
                           readiness.error().details.at("reason"));
  }
  std::vector<HardStopResourceLimitDTO> limits;
  limits.reserve(participant.value().resources.size());
  for (const auto& resource : participant.value().resources) {
    limits.push_back(HardStopResourceLimitDTO{
        resource.resource_id, resource.coordinate_kind == CoordinateKind::kAngular
                                  ? CoordinateVelocityDTO{AngularVelocityDTO{0.02}}
                                  : CoordinateVelocityDTO{LinearVelocityDTO{0.02}}});
  }
  std::optional<MotionInterruptStopProfileDTO> operator_stop_profile;
  std::optional<MotionInterruptStopProfileDTO> fault_stop_profile;
  if (participant.value().capability_id.value() == "execution.trajectory.execute") {
    MotionInterruptStopProfileDTO profile;
    profile.profile_id = "operator_soft_on_path";
    profile.jerk_limits_enabled = false;
    profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
    profile.deceleration_time_s = 1.0;
    profile.allow_natural_completion_on_short_path = true;
    profile.require_backend_confirmation = true;
    for (const auto& resource : participant.value().resources) {
      if (resource.coordinate_kind != CoordinateKind::kAngular ||
          !resource.source_joint_name.has_value()) {
        throw std::logic_error("test arm fault profile requires angular resources");
      }
      profile.per_joint_max_deceleration.emplace(*resource.source_joint_name,
                                                 AngularAccelerationDTO{4.0});
    }
    operator_stop_profile = profile;
    profile.profile_id = "fault_spring_back_hold";
    profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
    profile.allow_natural_completion_on_short_path = false;
    fault_stop_profile = std::move(profile);
  }
  return RuntimeParticipantExecutionRegistration{
      participant,
      std::move(readiness.value()),
      std::move(limits),
      MotionInterruptNodeCategory::kGeneric,
      &engine,
      dynamic_cast<IControlDomainExecutionPort*>(&engine),
      select_runtime_participant_policy(participant),
      dynamic_cast<IResolvedPreparedParticipantExecutionEngine*>(&engine),
      std::move(operator_stop_profile),
      std::move(fault_stop_profile),
      0.5};
}

[[nodiscard]] RuntimeParticipantExecutionRegistration registration_with_collision_cancel(
    const RuntimeParticipantBinding& participant, IResolvedParticipantExecutionEngine& engine) {
  auto result = registration(participant, engine, false, {}, true);
  auto plan = result.stop_readiness->value().resolved_stop_base_plan;
  auto cancel = plan.escalation_stages.back();
  cancel.stop_escalation_stage_id =
      StopEscalationStageId("runtime_collision_controller_goal_cancel");
  cancel.stage_kind = StopEscalationStageKind::kControlledFallback;
  cancel.containment_command = StopContainmentCommandDTO{
      StopContainmentCommandKind::kControllerGoalCancel, std::nullopt, std::nullopt};
  cancel.default_contract_parameters.reset();
  cancel.command_qualification.stop_action_class = StopActionClass::kControlled;
  for (auto& evidence : cancel.evidence_requirements) {
    evidence.requirement.evidence_item_id =
        StopEvidenceItemId(evidence.requirement.evidence_item_id.value() + ".collision_cancel");
  }
  plan.escalation_stages.insert(plan.escalation_stages.begin() + 1, std::move(cancel));
  for (std::size_t index = 0U; index < plan.escalation_stages.size(); ++index)
    plan.escalation_stages[index].stage_index = index;
  auto sealed = MakeResolvedStopBasePlan(std::move(plan));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  const auto expected = controller_domain_for(participant, sealed.value());
  auto readiness = verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO{participant.value().controllable_component_id,
                                         ComponentLifecycleGeneration(1U), expected.value(),
                                         std::move(sealed.value()),
                                         kTestHardStopCompletionTimeoutNs},
      participant, expected);
  if (!readiness.ok()) throw std::logic_error(readiness.error().message);
  result.stop_readiness = std::move(readiness.value());
  return result;
}

[[nodiscard]] RuntimeParticipantExecutionRegistration unqualified_registration(
    const RuntimeParticipantBinding& participant, IResolvedParticipantExecutionEngine& engine) {
  return RuntimeParticipantExecutionRegistration{
      participant,  std::nullopt,
      {},           MotionInterruptNodeCategory::kGeneric,
      &engine,      dynamic_cast<IControlDomainExecutionPort*>(&engine),
      std::nullopt, nullptr,
      std::nullopt, std::nullopt,
      std::nullopt};
}

[[nodiscard]] ActiveControlConfigurationDTO route_configuration_for(
    const RuntimeParticipantExecutionRegistration& registration) {
  const auto& binding = registration.binding.value();
  const auto& control = *binding.control_path;
  const auto& readiness = registration.stop_readiness->value();
  ActiveControlConfigurationDTO result{
      control.active_control_configuration_id,
      control.control_domain_id,
      control.control_domain_revision,
      {{control.control_binding_id, control.control_binding_revision, control.control_route_id}},
      control.activation_set_id,
      control.controller_topology_id,
      control.topology_revision,
      readiness.resolved_stop_base_plan.source_runtime_unit_id,
      readiness.resolved_stop_base_plan.source_runtime_epoch,
      readiness.resolved_stop_base_plan.source_sequence,
      {},
      control.active_control_configuration_generation,
      ActiveControlConfigurationStateDTO::kReconciled};
  if (!result.locally_valid()) {
    throw std::logic_error("test runtime route configuration is invalid");
  }
  return result;
}

[[nodiscard]] RuntimeParticipantBinding make_binding(
    std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt,
    bool include_second_angular_resource = false,
    std::string planner_capability_revision =
        "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
    bool include_collision_rule_body = false) {
  std::vector<std::string> compiled_group_joint_names{
      kind == CoordinateKind::kAngular ? "mf2_joint_1" : "mf2_finger_joint"};
  std::vector<RuntimeResourceBindingDTO> resources{RuntimeResourceBindingDTO{
      ResourceRoleId(kind == CoordinateKind::kAngular ? "arm_joints" : "fingers"),
      ResourceId(kind == CoordinateKind::kAngular ? "robot.joint_1" : "robot.finger"),
      kind == CoordinateKind::kAngular ? "joint_1" : "finger_joint",
      kind == CoordinateKind::kAngular ? "mf2_joint_1" : "mf2_finger_joint", kind}};
  if (kind == CoordinateKind::kAngular && include_second_angular_resource) {
    compiled_group_joint_names.emplace_back("mf2_joint_2");
    resources.push_back(RuntimeResourceBindingDTO{ResourceRoleId("arm_joints"),
                                                  ResourceId("robot.joint_2"), "joint_2",
                                                  "mf2_joint_2", CoordinateKind::kAngular});
  }
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_paths;
  if (kind == CoordinateKind::kAngular) {
    supporting_paths.push_back(RuntimeCapabilityProviderPathDTO{
        CapabilityId("planning.motion.joint_goal"),
        CapabilityDefinitionRevision(std::move(planner_capability_revision)),
        CapabilityProviderBindingId("planner-binding"),
        CapabilityProviderBindingRevision("planner-binding-revision"),
        ProviderInstanceId("planner-provider"), ProviderManifestId("planner-provider-manifest"),
        ProviderManifestRevision(test_digest('e')), RuntimeUnitId("runtime.native"),
        RuntimeEpoch("runtime-epoch.1")});
  }
  // The one collision body a test's collision rules may select.
  std::vector<ProgramCollisionRuleBodyBindingDTO> collision_rule_bodies;
  if (include_collision_rule_body) {
    const CollisionBodyRoleId body_role("gripper_collision");
    collision_rule_bodies.push_back(ProgramCollisionRuleBodyBindingDTO{
        {ControllableComponentId("robot.1"), body_role},
        ModelRoleId("body"),
        ModelInstanceId("robot.model"),
        ModelRevision("model-revision"),
        {make_component_collision_body_id(ControllableComponentId("robot.1"),
                                          ModelInstanceId("robot.model"), body_role, 0U)}});
  }
  RuntimeParticipantBindingDTO value{
      ProgramRevision(test_digest('0')),
      ProgramBindingId(test_digest('1')),
      RetainedBundleId(test_digest('6')),
      GraphRevision(test_digest('7')),
      ControllableComponentId("robot.1"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId(std::move(command_target)),
      CapabilityId(std::move(capability)),
      CapabilityDefinitionRevision("capability-revision"),
      std::move(command_schema),
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("command_group"), ModelInstanceId("robot.model"),
          kind == CoordinateKind::kAngular ? "mf2_arm_group" : "mf2_gripper_group",
          std::move(compiled_group_joint_names)}},
      {ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId("base")}, ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_base"},
       ProgramFrameBindingDTO{{ControllableComponentId("robot.1"), FrameRoleId("tcp")}, ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_tcp"}},
      std::move(resources),
      {},
      RuntimeControlPathDTO{
          ControlDomainId(kind == CoordinateKind::kAngular ? "domain.arm" : "domain.gripper"),
          ControlDomainRevision(test_digest('8')),
          ControlBindingId(kind == CoordinateKind::kAngular ? "binding.arm" : "binding.gripper"),
          ControlBindingRevision(test_digest('9')),
          ControlInterfaceContractId("interface"),
          ControlInterfaceRevision(test_digest('a')),
          ControllerTopologyId("topology.native"),
          TopologyRevision(test_digest('b')),
          ControlRouteId("route.native"),
          ActivationSetId("activation.native"),
          ActiveControlConfigurationId("active.native"),
          ActiveControlConfigurationGeneration(1U),
          ProviderInstanceId("provider.native"),
          ProviderManifestId("provider-manifest.native"),
          ProviderManifestRevision(test_digest('c')),
          RuntimeUnitId("runtime.native"),
          RuntimeEpoch("runtime-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision(test_digest('d')),
          ContainmentGranularity::kResource,
          kind == CoordinateKind::kAngular
              ? std::optional(RuntimeScaleRouteQualificationDTO{
                    ControlRouteId("route.native"), ScalingDomainId("scale.native"),
                    EndpointRoleId("runtime_scale_command"), EndpointRoleId("runtime_scale_state"),
                    std::nullopt, RuntimeScaleAcknowledgementKind::kAppliedState, 4.0, 100'000'000})
              : std::nullopt},
      std::nullopt,
      std::move(supporting_paths),
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.native"),
                                  RuntimeEpoch("runtime-epoch.1")}},
      std::move(collision_rule_bodies)};
  if (kind == CoordinateKind::kAngular) {
    value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
        {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding select_candidate_product(
    const RuntimeParticipantBinding& source) {
  auto value = source.value();
  value.program_revision = ProgramRevision(test_digest('2'));
  value.program_binding_id = ProgramBindingId(test_digest('3'));
  value.retained_bundle_id = RetainedBundleId(test_digest('e'));
  value.graph_revision = GraphRevision(test_digest('f'));
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding same_control_scope_program_alias(
    const RuntimeParticipantBinding& source) {
  auto value = source.value();
  value.program_revision = ProgramRevision(test_digest('2'));
  value.program_binding_id = ProgramBindingId(test_digest('3'));
  if (value.motion_endpoint) {
    value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
        {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding second_same_type_occurrence(
    const RuntimeParticipantBinding& source) {
  auto value = source.value();
  value.program_binding_id = ProgramBindingId(test_digest('4'));
  value.controllable_component_id = ControllableComponentId("robot.2");
  for (auto& model : value.model_bindings) {
    model.model_instance_id = ModelInstanceId("robot.model.2");
  }
  for (auto& group : value.model_groups) {
    group.model_instance_id = ModelInstanceId("robot.model.2");
    group.compiled_group_name = "mf2_robot_2_arm_group";
    for (auto& joint : group.expanded_compiled_joint_names) {
      joint = "mf2_robot_2_joint_1";
    }
  }
  for (auto& frame : value.frame_bindings) {
    frame.reference.component_id = value.controllable_component_id;
    frame.model_instance_id = ModelInstanceId("robot.model.2");
    frame.compiled_frame_name = "mf2_robot_2_" + frame.compiled_frame_name;
  }
  for (auto& resource : value.resources) {
    resource.resource_id = ResourceId("robot.2.joint_1");
    resource.compiled_resource_name = "mf2_robot_2_joint_1";
  }
  auto& control = *value.control_path;
  control.control_domain_id = ControlDomainId("domain.arm.2");
  control.control_binding_id = ControlBindingId("binding.arm.2");
  control.active_control_configuration_id = ActiveControlConfigurationId("active.native.2");
  control.provider_instance_id = ProviderInstanceId("provider.native.2");
  control.provider_runtime_epoch = RuntimeEpoch("runtime-epoch.1");
  control.state_authority_id = StateAuthorityId("state.native.2");
  for (auto& path : value.supporting_capability_provider_paths) {
    path.capability_provider_binding_id = CapabilityProviderBindingId("planner-binding.2");
    path.capability_provider_binding_revision =
        CapabilityProviderBindingRevision("planner-binding-revision.2");
    path.provider_instance_id = ProviderInstanceId("planner-provider.2");
    path.provider_runtime_epoch = RuntimeEpoch("runtime-epoch.1");
  }
  if (value.motion_endpoint) {
    value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
        {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding coordinated_occurrence(
    const RuntimeParticipantBinding& source, const std::string& suffix) {
  auto value = source.value();
  const bool angular = value.resources.front().coordinate_kind == CoordinateKind::kAngular;
  value.controllable_component_id = ControllableComponentId("robot." + suffix);
  for (auto& model : value.model_bindings) {
    model.model_instance_id = ModelInstanceId("robot.model." + suffix);
  }
  for (auto& group : value.model_groups) {
    group.model_instance_id = ModelInstanceId("robot.model." + suffix);
    group.compiled_group_name = "mf2_robot_" + suffix + (angular ? "_arm_group" : "_gripper_group");
    for (auto& joint : group.expanded_compiled_joint_names) {
      joint = "mf2_robot_" + suffix + (angular ? "_joint_1" : "_finger_joint");
    }
  }
  for (auto& frame : value.frame_bindings) {
    frame.reference.component_id = value.controllable_component_id;
    frame.model_instance_id = ModelInstanceId("robot.model." + suffix);
    frame.compiled_frame_name = "mf2_robot_" + suffix + "_" + frame.compiled_frame_name;
  }
  for (auto& resource : value.resources) {
    resource.resource_id = ResourceId("robot." + suffix + (angular ? ".joint_1" : ".finger"));
    resource.compiled_resource_name =
        "mf2_robot_" + suffix + (angular ? "_joint_1" : "_finger_joint");
  }
  auto& control = *value.control_path;
  control.control_domain_id =
      ControlDomainId(std::string(angular ? "domain.arm." : "domain.gripper.") + suffix);
  control.control_binding_id =
      ControlBindingId(std::string(angular ? "binding.arm." : "binding.gripper.") + suffix);
  control.active_control_configuration_id = ActiveControlConfigurationId("active.native." + suffix);
  control.provider_instance_id = ProviderInstanceId("provider.native." + suffix);
  control.state_authority_id = StateAuthorityId("state.native." + suffix);
  for (auto& path : value.supporting_capability_provider_paths) {
    path.capability_provider_binding_id = CapabilityProviderBindingId("planner-binding." + suffix);
    path.capability_provider_binding_revision =
        CapabilityProviderBindingRevision("planner-binding-revision." + suffix);
    path.provider_instance_id = ProviderInstanceId("planner-provider." + suffix);
  }
  if (value.motion_endpoint) {
    value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
        {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

/// `binding` resealed under the Program Binding of `program`. Program work of
/// one run names the captured Binding (run equality; one Binding per capture,
/// `program_execution_capture_mismatch`), so a second robot of the same
/// Program is a second target of that Binding, not a second Binding.
[[nodiscard]] RuntimeParticipantBinding with_program_binding_of(
    const RuntimeParticipantBinding& binding, const RuntimeParticipantBinding& program) {
  auto value = binding.value();
  value.program_binding_id = program.value().program_binding_id;
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

/// The second same-type occurrence in the same Program Binding as `source`.
[[nodiscard]] RuntimeParticipantBinding second_same_program_occurrence(
    const RuntimeParticipantBinding& source) {
  return with_program_binding_of(second_same_type_occurrence(source), source);
}

[[nodiscard]] RuntimeParticipantBinding second_occurrence_sharing_first_resource(
    const RuntimeParticipantBinding& source) {
  auto value = second_same_type_occurrence(source).value();
  value.controllable_component_id = source.value().controllable_component_id;
  for (auto& frame : value.frame_bindings)
    frame.reference.component_id = value.controllable_component_id;
  value.command_target_id = CommandTargetId("arm.overlap");
  value.resources.front().resource_id = source.value().resources.front().resource_id;
  if (value.motion_endpoint) {
    value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
        {value.controllable_component_id, value.command_target_id}, value.frame_bindings.back(), {}};
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding same_owner_control_binding_sibling(
    const RuntimeParticipantBinding& source) {
  auto value = source.value();
  value.program_revision = ProgramRevision(test_digest('2'));
  value.program_binding_id = ProgramBindingId(test_digest('3'));
  value.capability_id = CapabilityId("execution.trajectory.sibling");
  value.capability_definition_revision = CapabilityDefinitionRevision(test_digest('4'));
  auto& control = *value.control_path;
  control.control_binding_id = ControlBindingId("binding.arm.sibling");
  control.control_binding_revision = ControlBindingRevision(test_digest('5'));
  control.control_route_id = ControlRouteId("route.native.sibling");
  if (control.runtime_scale_qualification.has_value()) {
    control.runtime_scale_qualification->control_route_id = control.control_route_id;
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] ActiveControlConfigurationDTO route_configuration_for(
    const RuntimeParticipantExecutionRegistration& source,
    const RuntimeParticipantBinding& sibling) {
  auto result = route_configuration_for(source);
  const auto& sibling_path = *sibling.value().control_path;
  result.binding_routes.push_back({sibling_path.control_binding_id,
                                   sibling_path.control_binding_revision,
                                   sibling_path.control_route_id});
  std::sort(result.binding_routes.begin(), result.binding_routes.end(),
            CanonicalActiveControlConfigurationBindingRouteLess);
  if (!result.locally_valid()) {
    throw std::logic_error("test sibling route configuration is invalid");
  }
  return result;
}

/// Report each component's lifecycle to the Kernel as the coordinator would:
/// commands are admitted exactly while a component reports `enabled`.
/// Components left out are no longer in the cell.
void report_component_lifecycles(
    ResourceAddressedExecutionKernel& kernel,
    const std::vector<std::pair<ControllableComponentId, ComponentLifecycle>>& lifecycles) {
  std::vector<ComponentState> states;
  for (const auto& [component, lifecycle] : lifecycles) {
    ComponentState state;
    state.component_id = component.value();
    state.lifecycle = lifecycle;
    state.activity = ComponentActivity::kIdle;
    states.push_back(std::move(state));
  }
  kernel.observe_component_states(std::move(states));
}

/// Report the component of `registered` as the one enabled component.
void enable_component_commands(ResourceAddressedExecutionKernel& kernel,
                               const RuntimeParticipantExecutionRegistration& registered) {
  report_component_lifecycles(
      kernel, {{registered.binding.value().controllable_component_id, ComponentLifecycle::kEnabled}});
}

/// Move `source` into `owner`'s control domain under another component: the
/// generic graph contract allows several components in one domain.
[[nodiscard]] RuntimeParticipantBinding in_domain_of(const RuntimeParticipantBinding& source,
                                                     const RuntimeParticipantBinding& owner,
                                                     std::string component) {
  auto value = source.value();
  value.controllable_component_id = ControllableComponentId(std::move(component));
  value.control_path->control_domain_id = owner.value().control_path->control_domain_id;
  value.control_path->control_domain_revision =
      owner.value().control_path->control_domain_revision;
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding with_planner_capability(
    const RuntimeParticipantBinding& source, std::string capability_id) {
  const auto* definition = find_builtin_capability_definition(capability_id);
  if (definition == nullptr) {
    throw std::logic_error("test planner capability is not registered");
  }
  auto value = source.value();
  value.supporting_capability_provider_paths = {RuntimeCapabilityProviderPathDTO{
      CapabilityId(std::move(capability_id)),
      CapabilityDefinitionRevision(std::string(definition->capability_definition_revision)),
      CapabilityProviderBindingId("planner-binding"),
      CapabilityProviderBindingRevision("planner-binding-revision"),
      ProviderInstanceId("planner-provider"), ProviderManifestId("planner-provider-manifest"),
      ProviderManifestRevision(test_digest('e')), RuntimeUnitId("runtime.native"),
      RuntimeEpoch("runtime-epoch.1")}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding with_colocated_supporting_capability(
    const RuntimeParticipantBinding& source) {
  auto value = source.value();
  const auto& path = *value.control_path;
  for (auto& provider : value.supporting_capability_provider_paths) {
    provider.provider_instance_id = path.provider_instance_id;
    provider.provider_manifest_id = path.provider_manifest_id;
    provider.provider_manifest_revision = path.provider_manifest_revision;
    provider.provider_runtime_unit_id = path.provider_runtime_unit_id;
    provider.provider_runtime_epoch = path.provider_runtime_epoch;
  }
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

// ---- Default Program run of the Kernel tests (run equality, coordinator
// decision 2026-11-13): Program work names the run the Kernel holds and is
// scoped to a captured node. `make_claimed_kernel` claims run 1, registers
// `default_program_capture()` and starts every group; the shared operation
// builders scope each participant to its default node.

/// Components, targets and capabilities the shared builders and tests use.
inline constexpr std::array<const char*, 8> kDefaultProgramComponents{
    "gripper.1", "plc", "robot.1", "robot.2", "robot.3", "robot.4", "robot.a", "robot.b"};
inline constexpr std::array<const char*, 10> kDefaultProgramTargets{
    "arm", "arm.a", "arm.b", "arm.overlap", "gripper", "gripper.target",
    "missing", "other-arm", "other.target", "unknown"};
inline constexpr std::array<const char*, 3> kDefaultProgramCapabilities{
    "execution.trajectory.execute", "execution.trajectory.sibling", "tool.gripper.command"};

[[nodiscard]] inline bool default_program_motion(const CapabilityId& capability) {
  return capability.value() == "execution.trajectory.execute";
}

/// The default captured node of one (target, capability). Robot 1's arm
/// motion is the existing `move` node (index 0) of `program_execution_capture`.
[[nodiscard]] inline std::string default_program_node_id(const ProgramTargetReferenceDTO& target,
                                                         const CapabilityId& capability) {
  if (default_program_motion(capability) && target.component_id.value() == "robot.1" &&
      target.command_target_id.value() == "arm")
    return "move";
  return "x." + target.component_id.value() + "/" + target.command_target_id.value() + "/" +
         capability.value();
}

/// Node of participant `index` (from 0) of a coordinated motion of `count`
/// robots: its own synchronized (common-start) selection `sync<count>`.
[[nodiscard]] inline std::string default_program_sync_node_id(const std::size_t count,
                                                              const std::size_t index) {
  return "x.sync" + std::to_string(count) + "." + std::to_string(index + 1U);
}

/// Scope one participant to its default node (attempt 1).
inline void scope_default_program_participant(OperationParticipantIntentDTO& participant) {
  participant.execution_scope = ProgramNodeExecutionScopeDTO{
      ProgramNodeInstanceId(default_program_node_id(participant.target,
                                                    participant.required_capability_id)),
      NodeAttemptNumber(1U)};
}

/// Re-scope every participant of an independent intent after a test changed
/// its targets (manual work, which names no run, stays unscoped).
inline void rescope_default_program_intent(OperationIntentDTO& intent) {
  if (!intent.program_run_id) return;
  for (auto& participant : intent.participants) scope_default_program_participant(participant);
}

[[nodiscard]] OperationIntentDTO motion_operation(std::string request_id = "request.motion.1") {
  OperationIntentDTO intent{
      PublicRequestContextDTO{TestRequestKey(std::move(request_id)),
                              "test"},
      ProgramRevision(test_digest('0')),
      ProgramBindingId(test_digest('1')),
      ProgramRunNumber(1U),
      GraphRevision(test_digest('7')),
      RetainedBundleId(test_digest('6')),
      std::nullopt,
      {OperationParticipantIntentDTO{
          {ControllableComponentId("robot.1"), CommandTargetId("arm")},
          CapabilityId("execution.trajectory.execute"), std::nullopt,
          MotionParticipantIntentDTO{MoveId("move.1"),
                                     {ControllableComponentId("robot.1"), FrameRoleId("base")},
                                     CurrentTcpDTO{},
                                     TolerancesDTO{0.01, 0.001, 0.01},
                                     SpeedProfileDTO{},
                                     TimeoutsDTO{},
                                     CacheMode::kDefault,
                                     PlannerOverrideDTO{},
                                     MotionTargetModeDTO::kAbsolute,
                                     std::nullopt,
                                     std::nullopt,
                                     JointGoalDTO{{"joint_1"}, {0.2}},
                                     {{"joint_1", JointDynamicsLimitDTO{1.0, 2.0, std::nullopt}}},
                                     {}}}},
      OperationCoordinationPolicy::kIndependent};
  // Program work is scoped to its captured node (run equality).
  scope_default_program_participant(intent.participants.front());
  return intent;
}
[[nodiscard]] OperationIntentDTO candidate_motion_operation(
    std::string request_id = "request.motion.candidate") {
  auto operation = motion_operation(std::move(request_id));
  operation.program_revision = ProgramRevision(test_digest('2'));
  operation.program_binding_id = ProgramBindingId(test_digest('3'));
  operation.expected_retained_bundle_id = RetainedBundleId(test_digest('e'));
  operation.expected_graph_revision = GraphRevision(test_digest('f'));
  return operation;
}

void retarget(OperationParticipantIntentDTO& participant, const ControllableComponentId& component,
              const CommandTargetId& target) {
  participant.target = {component, target};
  if (auto* motion = std::get_if<MotionParticipantIntentDTO>(&participant.payload)) {
    motion->base_frame.component_id = component;
  }
  // A scoped Program participant follows its target to that target's node.
  if (participant.execution_scope) scope_default_program_participant(participant);
}

[[nodiscard]] OperationIntentDTO coordinated_motion_operation(
    std::string request_id, const std::size_t participant_count = 2U) {
  auto operation = motion_operation(std::move(request_id));
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  operation.participants.reserve(participant_count);
  for (std::size_t index = 1U; index < participant_count; ++index) {
    auto participant = operation.participants.front();
    const auto suffix = std::to_string(index + 1U);
    participant.target.component_id = ControllableComponentId("robot." + suffix);
    std::get<MotionParticipantIntentDTO>(participant.payload).base_frame.component_id = participant.target.component_id;
    std::get<MotionParticipantIntentDTO>(participant.payload).move_id = MoveId("move." + suffix);
    operation.participants.push_back(std::move(participant));
  }
  // Coordinated Program motion is an authored synchronized selection.
  for (std::size_t index = 0U; index < operation.participants.size(); ++index)
    operation.participants[index].execution_scope = ProgramNodeExecutionScopeDTO{
        ProgramNodeInstanceId(default_program_sync_node_id(participant_count, index)),
        NodeAttemptNumber(1U)};
  return operation;
}

/// Mixed or command-only coordinated work as work that names no run: a
/// captured synchronized selection holds only motion nodes (capture rule), so
/// it has no Program form. No-run, non-intrinsic coordinated work is admitted
/// with no claim (kops:~3666-3673; only intrinsic registrations are refused,
/// kops:1600-1604) and holds no cell claim, as the base admitted it with no
/// claim. Use on `make_unclaimed_kernel`.
[[nodiscard]] inline OperationIntentDTO no_run_coordinated_work(OperationIntentDTO operation) {
  operation.program_run_id.reset();
  for (auto& participant : operation.participants) participant.execution_scope.reset();
  return operation;
}

/// The coordinated motion-only builder as no-run work (beside the mixed
/// builders above, in the same tests).
[[nodiscard]] OperationIntentDTO no_run_coordinated_motion_operation(
    std::string request_id, const std::size_t participant_count = 2U) {
  return no_run_coordinated_work(
      coordinated_motion_operation(std::move(request_id), participant_count));
}

[[nodiscard]] OperationIntentDTO command_operation(std::string request_id = "request.command.1") {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  auto operation = motion_operation(std::move(request_id));
  operation.participants.front() = OperationParticipantIntentDTO{
      {ControllableComponentId("robot.1"), CommandTargetId("gripper.target")}, CapabilityId("tool.gripper.command"),
      std::nullopt,
      CapabilityCommandIntentDTO{
          schema.command_schema_id,
          schema.command_schema_revision,
          GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
          2.0,
          {CommandOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}};
  scope_default_program_participant(operation.participants.front());
  return operation;
}

[[nodiscard]] OperationIntentDTO coordinated_command_operation(
    std::string request_id, const std::size_t participant_count = 2U) {
  auto operation = command_operation(std::move(request_id));
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  operation.participants.reserve(participant_count);
  for (std::size_t index = 1U; index < participant_count; ++index) {
    auto participant = operation.participants.front();
    const auto suffix = std::to_string(index + 1U);
    participant.target.component_id = ControllableComponentId("robot." + suffix);

    operation.participants.push_back(std::move(participant));
  }
  return no_run_coordinated_work(std::move(operation));
}

[[nodiscard]] OperationIntentDTO coordinated_motion_command_operation(std::string request_id) {
  auto operation = motion_operation(std::move(request_id));
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  auto command = command_operation().participants.front();
  command.target.component_id = ControllableComponentId("robot.2");
  operation.participants.push_back(std::move(command));
  return no_run_coordinated_work(std::move(operation));
}

[[nodiscard]] RuntimeParticipantBinding grouped_command_binding() {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  return make_binding("arm", "tool.gripper.command", CoordinateKind::kLinear, schema);
}

// Surviving prepare-all/Stop/recovery fixtures: one motion and ordinary commands.
[[nodiscard]] OperationIntentDTO grouped_motion_commands_operation(
    std::string request_id, const std::size_t participant_count = 2U,
    const std::size_t motion_index = 0U) {
  auto operation = coordinated_motion_operation(std::move(request_id), participant_count);
  for (std::size_t index = 0; index < participant_count; ++index) {
    if (index == motion_index) continue;
    auto& participant = operation.participants[index];
    const auto command = command_operation().participants.front();
    participant.required_capability_id = command.required_capability_id;
    participant.payload = command.payload;
  }
  return no_run_coordinated_work(std::move(operation));
}

[[nodiscard]] OperationControlRequestDTO control_request(std::string request_id,
                                                         const OperationAcceptedDTO& accepted,
                                                         std::uint64_t expected_revision) {
  return OperationControlRequestDTO{
      PublicRequestContextDTO{TestRequestKey(std::move(request_id)),
                              "test"},
      accepted.operation_id, OperationRevision(expected_revision), std::nullopt};
}

[[nodiscard]] StopIntentDTO stop_request(std::string request_id,
                                         const OperationAcceptedDTO& accepted,
                                         std::uint64_t expected_revision) {
  return StopIntentDTO{
      PublicRequestContextDTO{TestRequestKey(std::move(request_id)),
                              "test"},
      std::vector<StopOperationTargetDTO>{
          StopOperationTargetDTO{accepted.operation_id, OperationRevision(expected_revision)}},
      std::nullopt};
}

/// Body of Stop all, Halt and the cell Stop.
[[nodiscard]] FullStopIntentDTO full_stop_request(std::string request_id) {
  return FullStopIntentDTO{PublicRequestContextDTO{TestRequestKey(std::move(request_id)), "test"},
                           {}};
}

/// S16 (Forced Program End) of a run.
[[nodiscard]] ProgramRunEndIntentDTO program_run_stop_request(
    std::string request_id, std::uint64_t program_run_id) {
  return ProgramRunEndIntentDTO{PublicRequestContextDTO{TestRequestKey(std::move(request_id)),
                                                        "test"},
                                ProgramRunNumber(program_run_id),
                                RequestAttributionDTO{RequestTransportDTO::kInternal,
                                                      RequestAssuranceDTO::kUnverified,
                                                      std::nullopt}};
}

[[nodiscard]] ProgramRunExecutionCaptureDTO program_execution_capture() {
  const auto intent = motion_operation();
  auto binding = std::make_shared<ProgramBindingDTO>(ProgramBindingDTO{
      std::string(kProgramBindingApiVersion),
      intent.program_binding_id,
      intent.program_revision,
      intent.expected_retained_bundle_id,
      intent.expected_graph_revision,
      {"test.catalog", "1", ContentDigest(test_digest('0')), ContentDigest(test_digest('0'))},
      {}, {}, {}, {}, {},
      {ContentDigest(test_digest('0')), {}, {}}});
  const auto physical =
      make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular).value();
  ProgramTargetBindingDTO target{
      {physical.controllable_component_id, physical.command_target_id}, {}, {}, {},
      {{ResourceRoleId("arm_joints"), {physical.resources.front().resource_id},
        {physical.resources.front().compiled_resource_name}}}, {}};
  binding->target_bindings.push_back(std::move(target));
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("move"),
                   "kernel.operation",
                   {{{ControllableComponentId("robot.1"), CommandTargetId("arm")}, CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("timer"), "orchestration.clock", {}}};
  table->named_selections = {{"arm", {0U}}, {"supervision", {1U}}};
  return {CellAdmissionClaimGeneration(1U),
          *intent.program_run_id,
          intent.program_revision,
          intent.program_binding_id,
          intent.expected_retained_bundle_id,
          intent.expected_graph_revision,
          std::move(table),
          std::move(binding)};
}

/// The default capture of `make_claimed_kernel`: `program_execution_capture`
/// (node 0 `move`, node 1 `timer`, groups `arm` and `supervision`) plus one
/// effectful node per (component, target, capability) the tests use, sorted
/// after `timer`, and the synchronized selections `sync2`/`sync3` of
/// `coordinated_motion_operation`.
[[nodiscard]] ProgramRunExecutionCaptureDTO default_program_capture(
    const CellAdmissionClaimGeneration claim_generation = CellAdmissionClaimGeneration(1U)) {
  auto capture = program_execution_capture();
  capture.claim_generation = claim_generation;
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  auto table = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  const auto resource_of = [](const std::string& component, const bool motion) {
    const std::string suffix = motion ? "joint_1" : "finger";
    return ResourceId(component == "robot.1" ? "robot." + suffix : component + "." + suffix);
  };
  std::vector<ProgramTargetBindingDTO> targets;
  std::vector<ProgramExecutionNodeDTO> extra;
  for (const auto* component : kDefaultProgramComponents) {
    for (const auto* command_target : kDefaultProgramTargets) {
      const ProgramTargetReferenceDTO target{ControllableComponentId(component),
                                             CommandTargetId(command_target)};
      targets.push_back(ProgramTargetBindingDTO{
          target, {}, {}, {},
          {{ResourceRoleId("arm_joints"), {resource_of(component, true)}, {"joint_1"}},
           {ResourceRoleId("fingers"), {resource_of(component, false)}, {"finger_joint"}}},
          {}});
      for (const auto* capability : kDefaultProgramCapabilities) {
        const CapabilityId capability_id(capability);
        const auto id = default_program_node_id(target, capability_id);
        if (id == "move") continue;
        extra.push_back(ProgramExecutionNodeDTO{
            ProgramNodeInstanceId(id),
            default_program_motion(capability_id) ? "kernel.operation" : "kernel.command_target",
            {{target, capability_id, true}}});
      }
    }
  }
  for (const std::size_t count : {2U, 3U})
    for (std::size_t index = 0U; index < count; ++index)
      extra.push_back(ProgramExecutionNodeDTO{
          ProgramNodeInstanceId(default_program_sync_node_id(count, index)),
          "kernel.operation",
          {{{ControllableComponentId("robot." + std::to_string(index + 1U)),
             CommandTargetId("arm")},
            CapabilityId("execution.trajectory.execute"),
            true}}});
  std::sort(extra.begin(), extra.end(),
            [](const auto& a, const auto& b) { return a.instance_id < b.instance_id; });
  for (auto& node : extra) table->nodes.push_back(std::move(node));
  for (const std::size_t count : {2U, 3U}) {
    ProgramExecutionSelectionDTO selection{"sync" + std::to_string(count), {},
                                           ProgramExecutionPolicy::kCommonStart};
    for (std::size_t index = 0U; index < count; ++index) {
      const ProgramNodeInstanceId id(default_program_sync_node_id(count, index));
      const auto node = std::lower_bound(
          table->nodes.begin(), table->nodes.end(), id,
          [](const auto& row, const auto& value) { return row.instance_id < value; });
      selection.node_indices.push_back(static_cast<std::uint32_t>(node - table->nodes.begin()));
    }
    std::sort(selection.node_indices.begin(), selection.node_indices.end());
    table->named_selections.push_back(std::move(selection));
  }
  std::sort(table->named_selections.begin(), table->named_selections.end(),
            [](const auto& a, const auto& b) { return a.group_id < b.group_id; });
  for (auto& target : targets) {
    const auto existing = std::find_if(binding->target_bindings.begin(),
                                       binding->target_bindings.end(), [&](const auto& row) {
                                         return row.target == target.target;
                                       });
    if (existing == binding->target_bindings.end())
      binding->target_bindings.push_back(std::move(target));
  }
  std::sort(binding->target_bindings.begin(), binding->target_bindings.end(),
            [](const auto& a, const auto& b) { return a.target < b.target; });
  capture.table = std::move(table);
  capture.binding = std::move(binding);
  return capture;
}

[[nodiscard]] Result<AuthoringAdmissionDTO> ready_cell_admission(
    ResourceAddressedExecutionKernel& kernel) {
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{1U,
                            ProgramCycleStateDTO::kIdleAfterReset, std::nullopt, std::nullopt,
                            std::nullopt},
      true,
      1U,
      1U,
      true,
      true,
      0U,
      0U,
      0U};
  return kernel.UpdateCellAdmissionReadiness(ready);
}

/// The default Program-run claim of each Kernel made by `make_claimed_kernel`
/// (run 1, the run `motion_operation()` names), so `claim_program_execution`
/// returns that claim instead of acquiring a second one.
[[nodiscard]] inline std::map<const ResourceAddressedExecutionKernel*, CellAdmissionClaimDTO>&
default_program_claims() {
  static std::map<const ResourceAddressedExecutionKernel*, CellAdmissionClaimDTO> claims;
  return claims;
}

[[nodiscard]] Result<CellAdmissionClaimDTO> acquire_program_claim(
    ResourceAddressedExecutionKernel& kernel) {
  const auto updated = ready_cell_admission(kernel);
  if (!updated.ok()) return Result<CellAdmissionClaimDTO>::Err(updated.error());
  return kernel.AcquireCellAdmissionClaim(CellAdmissionClaimIntentDTO{
      CellAdmissionClaimKindDTO::kProgramCycle,
      ApplicationRole::kProgramRunner,
      CellAdmissionOwnerTransactionId(ProgramRunNumber(1U)),
      TestRequestKey("request.program-cycle.claim"),
      kernel.GetAuthoringAdmission().admission_generation,
      {},
      ProgramCycleAdmissionEvidenceDTO{ProgramRevision(test_digest('0')),
                                       ProgramRevision(test_digest('0'))},
      std::nullopt});
}

/// Claims run 1 (run equality: Program work must name the run the Kernel
/// holds). Idempotent for a Kernel that already holds its default claim.
[[nodiscard]] Result<CellAdmissionClaimDTO> claim_program_execution(
    ResourceAddressedExecutionKernel& kernel) {
  auto& claims = default_program_claims();
  const auto held = claims.find(&kernel);
  if (held != claims.end() && kernel.get_stop_stream_facts().program_run_hold.has_value() &&
      kernel.get_stop_stream_facts().program_run_hold->program_run_id == ProgramRunNumber(1U))
    return Result<CellAdmissionClaimDTO>::Ok(held->second);
  auto acquired = acquire_program_claim(kernel);
  if (acquired.ok()) claims.insert_or_assign(&kernel, acquired.value());
  return acquired;
}

/// Captures `capture` for the Kernel's default claim of run 1 and starts every
/// group (Run). A capture the Kernel refuses (for example registrations with
/// mixed graph revisions) leaves the claim uncaptured; the refusal is the
/// test's own subject then.
inline void start_default_program(ResourceAddressedExecutionKernel& kernel,
                                  ProgramRunExecutionCaptureDTO capture) {
  const auto claim = claim_program_execution(kernel);
  if (!claim.ok()) return;
  capture.claim_generation = claim.value().claim_generation;
  if (!kernel.register_program_run_capture(std::move(capture)).ok()) return;
  const auto scope = kernel.get_program_execution_scope(ProgramRunNumber(1U));
  if (!scope.ok()) return;
  ProgramExecutionSelectionRequestDTO all{
      PublicRequestContextDTO{TestRequestKey("request.default-program.run"), "test"},
      ProgramRunNumber(1U),
      {"all"},
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
  const auto started = kernel.start_program_selection(all, scope.value().hold_revision);
  EXPECT_TRUE(started.ok()) << started.error().message;
}

/// Claims run 1 for another Program (its applied revision), captures
/// `default_program_capture()` re-identified as that Program and runs every
/// group: the Program form of work a test sends under another Program
/// Binding (run equality: Program work names the claimed, captured run).
/// Call on an unclaimed Kernel.
inline void establish_program_as(ResourceAddressedExecutionKernel& kernel,
                                 const ProgramRevision& program_revision,
                                 const ProgramBindingId& program_binding_id,
                                 const std::optional<RetainedBundleId>& retained_bundle_id = {},
                                 const std::optional<GraphRevision>& graph_revision = {},
                                 ProgramRunResolutionContext resolution_context = {}) {
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  const auto claim = kernel.AcquireCellAdmissionClaim(CellAdmissionClaimIntentDTO{
      CellAdmissionClaimKindDTO::kProgramCycle, ApplicationRole::kProgramRunner,
      CellAdmissionOwnerTransactionId(ProgramRunNumber(1U)),
      TestRequestKey("request.program-cycle.claim." + program_revision.value()),
      kernel.GetAuthoringAdmission().admission_generation, {},
      ProgramCycleAdmissionEvidenceDTO{program_revision, program_revision}, std::nullopt});
  ASSERT_TRUE(claim.ok()) << claim.error().message;
  auto capture = default_program_capture();
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  binding->program_revision = program_revision;
  binding->program_binding_id = program_binding_id;
  capture.program_revision = program_revision;
  capture.program_binding_id = program_binding_id;
  if (retained_bundle_id) {
    binding->retained_bundle_id = *retained_bundle_id;
    capture.retained_bundle_id = *retained_bundle_id;
  }
  if (graph_revision) {
    binding->graph_revision = *graph_revision;
    capture.graph_revision = *graph_revision;
  }
  capture.binding = std::move(binding);
  capture.resolution_context = std::move(resolution_context);
  capture.claim_generation = claim.value().claim_generation;
  const auto captured = kernel.register_program_run_capture(std::move(capture));
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  const auto scope = kernel.get_program_execution_scope(ProgramRunNumber(1U));
  ASSERT_TRUE(scope.ok()) << scope.error().message;
  const auto started = kernel.start_program_selection(
      ProgramExecutionSelectionRequestDTO{
          PublicRequestContextDTO{TestRequestKey("request.program.run." + program_revision.value()),
                                  "test"},
          ProgramRunNumber(1U),
          {"all"},
          RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                                std::nullopt}},
      scope.value().hold_revision);
  ASSERT_TRUE(started.ok()) << started.error().message;
}

/// The Kernel test default: a Kernel that holds the Program claim of run 1,
/// has captured `default_program_capture()` and runs every group (run
/// equality: Program work is scoped to the run the Kernel holds).
[[nodiscard]] std::unique_ptr<ResourceAddressedExecutionKernel> make_claimed_kernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config) {
  // A Program-cycle claim needs current Manager liveness
  // (kernel_cell_admission_owner.cpp:149-165, NonCycleReadinessLocked); a
  // config without a required set gets the empty one, as other tests do.
  if (!config.manager_liveness_required_set)
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
  auto kernel = manyforge::core::testing::make_heap_resource_addressed_kernel(
      std::move(registrations), clock, scheduler, journal, std::move(config));
  default_program_claims().erase(kernel.get());
  const auto claimed = claim_program_execution(*kernel);
  EXPECT_TRUE(claimed.ok()) << claimed.error().message;
  start_default_program(*kernel, default_program_capture());
  return kernel;
}

/// `config` able to admit the default Program-cycle claim (empty Manager
/// liveness set when absent), for Kernels a test constructs itself before
/// `establish_default_program`.
[[nodiscard]] inline ResourceAddressedExecutionKernelConfig claim_ready_config(
    ResourceAddressedExecutionKernelConfig config) {
  if (!config.manager_liveness_required_set)
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
  return config;
}

/// A Kernel with no claim yet whose config can admit a Program-cycle claim
/// (empty Manager liveness set when absent): for `establish_program_as`.
[[nodiscard]] std::unique_ptr<ResourceAddressedExecutionKernel> make_claim_ready_kernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config) {
  if (!config.manager_liveness_required_set)
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
  auto kernel = manyforge::core::testing::make_heap_resource_addressed_kernel(
      std::move(registrations), clock, scheduler, journal, std::move(config));
  default_program_claims().erase(kernel.get());
  return kernel;
}

/// Opt-out: the Program claim of run 1 without a capture, for tests that
/// register their own capture or test capture and claim behaviour.
[[nodiscard]] std::unique_ptr<ResourceAddressedExecutionKernel> make_uncaptured_kernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config) {
  if (!config.manager_liveness_required_set)
    config.manager_liveness_required_set = EmptyManagerLivenessSet();  // see make_claimed_kernel
  auto kernel = manyforge::core::testing::make_heap_resource_addressed_kernel(
      std::move(registrations), clock, scheduler, journal, std::move(config));
  default_program_claims().erase(kernel.get());
  const auto claimed = claim_program_execution(*kernel);
  EXPECT_TRUE(claimed.ok()) << claimed.error().message;
  return kernel;
}

/// Fixtures that construct their own Kernel (rule fixtures, port cells)
/// establish the same default Program; a test that claims and captures its
/// own run first constructs the fixture under `ScopedNoDefaultProgram`.
[[nodiscard]] inline bool& default_program_disabled() {
  static bool disabled = false;
  return disabled;
}
struct ScopedNoDefaultProgram final {
  ScopedNoDefaultProgram() { default_program_disabled() = true; }
  ~ScopedNoDefaultProgram() { default_program_disabled() = false; }
  ScopedNoDefaultProgram(const ScopedNoDefaultProgram&) = delete;
  ScopedNoDefaultProgram& operator=(const ScopedNoDefaultProgram&) = delete;
};

/// Claim run 1, capture `default_program_capture()` and run every group, as
/// `make_claimed_kernel` does (unless `ScopedNoDefaultProgram` is active).
inline void establish_default_program(ResourceAddressedExecutionKernel& kernel) {
  if (default_program_disabled()) return;
  default_program_claims().erase(&kernel);
  const auto claimed = claim_program_execution(kernel);
  EXPECT_TRUE(claimed.ok()) << claimed.error().message;
  start_default_program(kernel, default_program_capture());
}

/// Lazy form for fixtures whose tests may claim and capture their own run
/// before building an intent: establishes the default Program only when the
/// Kernel holds no Program claim yet.
inline void ensure_default_program(ResourceAddressedExecutionKernel& kernel) {
  if (kernel.get_stop_stream_facts().program_run_hold.has_value()) return;
  establish_default_program(kernel);
}

/// Opt-out: a Kernel with no Program claim (manual-only work, Program reset
/// and cycle-claim tests, Stops with no Program).
[[nodiscard]] std::unique_ptr<ResourceAddressedExecutionKernel> make_unclaimed_kernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config) {
  auto kernel = manyforge::core::testing::make_heap_resource_addressed_kernel(
      std::move(registrations), clock, scheduler, journal, std::move(config));
  default_program_claims().erase(kernel.get());
  return kernel;
}

/// Manual work (an intrinsic direct-control binding) names no run.
[[nodiscard]] OperationIntentDTO manual_motion_operation(
    std::string request_id = "request.motion.1") {
  auto intent = motion_operation(std::move(request_id));
  intent.program_run_id.reset();
  // Manual work carries no Program-node authority (kops:820).
  for (auto& participant : intent.participants) participant.execution_scope.reset();
  return intent;
}

[[nodiscard]] OperationIntentDTO scoped_motion_operation(std::string request_id) {
  auto intent = motion_operation(std::move(request_id));
  intent.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("move"), NodeAttemptNumber(1U)};
  return intent;
}

[[nodiscard]] ProgramExecutionSelectionRequestDTO program_selection_request(std::string request_id,
                                                                            std::string group) {
  return {motion_operation(std::move(request_id)).request_context,
          ProgramRunNumber(1U),
          {std::move(group)},
          {}};
}

/// Waits until the Stop event's covered participants all ended.
[[nodiscard]] bool wait_stop_complete(ResourceAddressedExecutionKernel& kernel,
                                      StopEventId event_id) {
  return wait_until([&] {
    const auto status = kernel.read_stop(event_id);
    return status.ok() && status.value().complete;
  });
}

void start_program_motion(ResourceAddressedExecutionKernel& kernel,
                          std::string group = "all", std::uint64_t program_run = 1U) {
  const auto scope = kernel.get_program_execution_scope(ProgramRunNumber(program_run));
  ASSERT_TRUE(scope.ok()) << scope.error().message;
  // The default Program of `make_claimed_kernel` already runs every group.
  if (group == "all" && scope.value().held_nodes.empty()) return;
  const auto request_id = "initial.run." + group;
  auto request = program_selection_request(request_id, std::move(group));
  request.program_run_id = ProgramRunNumber(program_run);
  const auto started =
      kernel.start_program_selection(request, scope.value().hold_revision);
  ASSERT_TRUE(started.ok()) << started.error().message;
}


}  // namespace

#pragma GCC diagnostic pop
