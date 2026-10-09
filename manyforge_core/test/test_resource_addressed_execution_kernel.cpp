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
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <manyforge_core/detail/coordinated_motion_preparation.hpp>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#ifdef MANYFORGE_TEST_SQLITE_OPERATION_JOURNAL
#include <sqlite3.h>

#include <manyforge_core/sqlite_operation_journal.hpp>
#endif
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    last_execute = request;
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

  AddressedParticipantStopFenceDisposition fence_addressed_participant_stop(
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
        return AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
      }
    }
    return AddressedParticipantStopFenceDisposition::kDispatchCommitted;
  }

  Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) override {
    ++participant_stop_settlement_calls;
    if (participant_stop_settlement_hook) {
      participant_stop_settlement_hook();
    }
    if (participant_stop_settlement_error.has_value()) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(*participant_stop_settlement_error);
    }
    const auto& certificate = request.aggregate_stop_certificate;
    const auto& stage = request.containment.request;
    const bool expected_disposition = request.effect_prevented_before_dispatch
                                          ? request.containment.attempt_disposition ==
                                                StopStageAttemptDispositionDTO::kNotDispatched
                                          : request.containment.attempt_disposition !=
                                                StopStageAttemptDispositionDTO::kNotDispatched;
    if (request.execution_id != next_execution_id || !certificate.locally_valid() ||
        request.containment.outcome != ParticipantStopStageOutcomeDTO::kSucceeded ||
        !expected_disposition || stage.stop_id != certificate.stop_id ||
        stage.stop_generation != certificate.stop_generation) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(ErrorDTO{
          "addressed_motion_stop_execution_mismatch", "fake addressed Stop identity is stale", {}});
    }
    {
      std::lock_guard lock(status_mutex_);
      if (!scripted_ending(status.state)) ended_phase = status.state;
      status.state = ExecutionState::kStopped;
      status.execution_id = request.execution_id;
    }
    recovery_assessment.assessment_revision += 1U;
    recovery_assessment.control_command_epoch += 1U;
    recovery_assessment.execution_id = request.execution_id;
    recovery_assessment.execution_state = ExecutionState::kStopped;
    recovery_assessment.recovery_class = RecoveryClass::kOrdinaryOperatorStop;
    recovery_assessment.status = RecoveryAssessmentStatus::kRetryReady;
    recovery_assessment.retry_admissible = true;
    recovery_assessment.stop_certificate_id = certificate.stop_certificate_id.value();
    recovery_assessment.stop_certificate_revision = certificate.stop_certificate_revision.value();
    recovery_assessment.stop_owner_generation = certificate.stop_generation.value();
    recovery_assessment.backend_stop_confirmed = true;
    recovery_assessment.allowed_actions = {RecoveryAction::kReplanFromStoppedState,
                                           RecoveryAction::kResetCycle};
    auto settlement = AddressedParticipantStopSettlementDTO{
        request.execution_id,     certificate,
        stage.operation_id,       stage.participant_id,
        stage.controller_goal_id, stage.controller_goal_generation,
        recovery_assessment};
    if (return_mismatched_participant_stop_settlement) {
      settlement.controller_goal_generation =
          ControllerGoalGeneration(stage.controller_goal_generation.value() + 1U);
    }
    return Result<AddressedParticipantStopSettlementDTO>::Ok(std::move(settlement));
  }

  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO request) override {
    ++prepare_motion_calls;
    Trace("engine:prepare_motion");
    if (prepare_motion_hook) prepare_motion_hook();
    if (prepare_call_failure.has_value()) return Result<std::uint64_t>::Err(*prepare_call_failure);
    {
      std::lock_guard lock(prepared_motion_mutex_);
      last_prepared_request = request;
      last_dispatched_participant.reset();
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
    }
    return CompletePreparation();
  }

  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t execution_id) const override {
    ++observe_prepared_motion_calls;
    auto result = [&] {
      std::lock_guard lock(prepared_motion_mutex_);
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
    auto private_validation = validate_gripper_command(request.command_request);
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
        std::nullopt,        std::nullopt};
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
      const ControllerGoalId& controller_goal_id,
      ControllerGoalGeneration controller_goal_generation) override {
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
      if (prepared_command_snapshot->controller_goal_id != controller_goal_id ||
          prepared_command_snapshot->controller_goal_generation != controller_goal_generation) {
        return Result<GripperCommandResultDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "fake prepared command goal identity conflict",
                     {{"reason", "prepared_command_goal_conflict"}}});
      }
      return Result<GripperCommandResultDTO>::Ok(*prepared_command_snapshot->dispatch_result);
    }
    prepared_command_snapshot->controller_goal_id = controller_goal_id;
    prepared_command_snapshot->controller_goal_generation = controller_goal_generation;
    auto dispatched = command_gripper(last_prepared_command_request->command_request);
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
                                                std::stop_token) override {
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

  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& stage,
                                                   const RemainingStopBudgetDTO&,
                                                   std::stop_token) override {
    Trace("engine:translate_stop_stage");
    last_stop_stage = stage;
    ++translate_stop_stage_calls;
    if (translate_stop_stage_hook) {
      translate_stop_stage_hook();
    }
    std::vector<StopEvidenceItemDTO> evidence;
    const auto& requirements = stage.participant_stop_plan.stage_commands.at(stage.stage_index)
                                   .resolved_stage.evidence_requirements;
    evidence.reserve(requirements.size());
    for (const auto& requirement : requirements) {
      evidence.push_back(StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
          next_stop_result.accepted ? StopEvidenceItemStateDTO::kSatisfied
                                    : StopEvidenceItemStateDTO::kFailed,
          requirement.resolved_source,
          requirement.resolved_source.runtime_target.provider_runtime_epoch, 1U, 0,
          next_stop_result.accepted
              ? std::optional<JsonValueDTO>(next_stop_evidence.value_or(JsonValueDTO{true}))
              : std::nullopt,
          next_stop_result.accepted ? std::nullopt
                                    : std::optional(next_stop_result.error.value_or(
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
                                                next_stop_result.accepted
                                                    ? ParticipantStopStageOutcomeDTO::kSucceeded
                                                    : ParticipantStopStageOutcomeDTO::kFailed,
                                                actual_stop_mode,
                                                std::nullopt,
                                                1U,
                                                stage.provider_runtime_epoch,
                                                1U,
                                                0,
                                                std::move(evidence),
                                                next_stop_result.error};
    if (!next_stop_result.accepted) {
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
    if (runtime_scale_effect_hook) {
      runtime_scale_effect_hook();
    }
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
                                        command.controller_goal_generation,
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

  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id, const ControlBindingId& control_binding_id) const override {
    if (!last_runtime_scale_command.has_value() ||
        last_runtime_scale_command->operation_id != operation_id ||
        last_runtime_scale_command->control_binding_id != control_binding_id ||
        !last_execute.has_value()) {
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
        command.controller_goal_generation,
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
        last_execute->binding.provider_runtime_epoch,
        runtime_scale.acknowledgement_sequence,
        runtime_scale.acknowledgement_timestamp_ns,
        runtime_scale.error.has_value() ? RuntimeScaleApplicationState::kFailed
        : runtime_scale.active          ? RuntimeScaleApplicationState::kApplied
                                        : RuntimeScaleApplicationState::kPending,
        runtime_scale.error});
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override {
    ++command_calls;
    Trace("engine:command_gripper");
    last_command = request;
    return next_command_result;
  }

  Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const override {
    ++validate_command_calls;
    return next_command_validation;
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
    runtime_scale.active = to == ExecutionState::kExecuting;
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
  std::atomic<int> prepare_command_calls{};
  mutable std::atomic<int> observe_prepared_command_calls{};
  std::atomic<int> dispatch_prepared_command_calls{};
  std::atomic<int> cancel_prepared_command_calls{};
  std::atomic<int> preview_calls{};
  std::atomic<int> command_calls{};
  mutable std::atomic<int> validate_command_calls{};
  mutable std::atomic<int> get_gripper_status_calls{};
  std::atomic<int> stop_gripper_calls{};
  std::atomic<int> reset_gripper_calls{};
  std::atomic<int> stop_calls{};
  std::atomic<int> translate_stop_stage_calls{};
  std::atomic<int> certified_reset_calls{};
  std::atomic<int> addressed_stop_fence_calls{};
  std::atomic<std::uint64_t> addressed_stop_fence_execution_id{};
  std::atomic<int> participant_stop_settlement_calls{};
  std::atomic<int> acknowledge_calls{};
  std::atomic<int> runtime_scale_calls{};
  mutable std::atomic<int> recovery_assessment_calls{};
  std::atomic<int> recovery_action_calls{};
  std::uint64_t next_execution_id{41U};
  Result<std::uint64_t> next_execute_result = Result<std::uint64_t>::Ok(41U);
  GripperCommandResultDTO next_command_result{true, 73U, 9U, std::nullopt};
  Result<void> next_command_validation = Result<void>::Ok();
  StopResultDTO next_stop_result{true, std::nullopt};
  std::optional<JsonValueDTO> next_stop_evidence;
  ResetResultDTO next_reset_result{true, std::nullopt};
  AcknowledgeCompletedResultDTO next_acknowledge_result{true, std::nullopt};
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
  bool prepare_bounded_time_law{};
  bool defer_prepared_dispatch_effect{};
  ExecutionState prepared_cancel_terminal_state{ExecutionState::kStopped};
  std::optional<ErrorDTO> prepare_failure;
  std::optional<ErrorDTO> prepare_call_failure;
  std::optional<ErrorDTO> prepare_command_failure;
  std::optional<ErrorDTO> prepared_dispatch_event_error;
  std::optional<ErrorDTO> participant_stop_settlement_error;
  bool return_mismatched_participant_stop_settlement{};
  std::function<void()> acknowledge_effect_hook;
  std::function<void()> execute_return_hook;
  std::function<void()> reset_effect_hook;
  std::function<void()> prepare_command_hook;
  std::function<void()> prepare_motion_hook;
  std::function<void()> cancel_prepared_motion_hook;
  std::function<void()> cancel_prepared_command_hook;
  mutable std::function<void()> observe_prepared_motion_hook;
  mutable std::function<void()> observe_prepared_command_hook;
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
  std::optional<ExecuteRequestDTO> last_execute;
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> last_prepared_request;
  mutable std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> prepared_snapshot;
  std::shared_ptr<const PreparedOperationParticipantDTO> last_dispatched_participant;
  std::optional<ResolvedPreparedCapabilityCommandExecutionRequestDTO> last_prepared_command_request;
  mutable std::optional<ResolvedPreparedCapabilityCommandSnapshotDTO> prepared_command_snapshot;
  std::optional<ExecuteRequestDTO> last_preview;
  std::optional<ResolvedRuntimeScaleCommandDTO> last_runtime_scale_command;
  std::optional<GripperCommandRequestDTO> last_command;
  std::optional<ResolvedParticipantStopStageDTO> last_stop_stage;
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
        request.binding.program_id,
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
        request.planning_profile,
        request.contact_policy_id.empty() ? std::nullopt
                                          : std::optional<std::string>{request.contact_policy_id},
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
    if (prepare_bounded_time_law) {
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
      if (!scope.stop_profile) {
        return Result<PreparedOperationParticipantDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                     "fake prepared scope has no Stop profile",
                     {{"reason", "stop_profile_missing"}}});
      }
      PreparedParticipantStopPlanContextDTO context{
          request.binding.operation_id,
          authority.operation_revision,
          request.binding.participant_id,
          request.binding.program_id,
          request.binding.program_revision,
          request.binding.program_binding_id,
          request.binding.program_run_id,
          scope.stop_profile,
          ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
          scope.scope_id,
          scope.node_category};
      auto stop = MakePreparedParticipantStopPlan(
          authority.stop_readiness.value().resolved_stop_base_plan, std::move(context),
          binding.resources, authority.retained_default_stop_limits);
      if (!stop.ok()) {
        return Result<PreparedOperationParticipantDTO>::Err(stop.error());
      }
      scopes.push_back({MotionInterruptScopeId(scope.scope_id), InterruptPolicyId(scope.policy_id),
                        scope.activation_mode, scope.arm_timeout_ns, scope.result_binding_key,
                        scope.required, *scope.stop_profile, scope.node_category, scope.metadata,
                        std::move(stop.value())});
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const auto& left, const auto& right) { return left.scope_id < right.scope_id; });

    std::optional<PreparedContactPolicyRefDTO> contact_policy;
    if (request.resolved_contact_grasp) {
      contact_policy =
          PreparedContactPolicyRefDTO{request.resolved_contact_grasp->contact_policy_id,
                                      request.resolved_contact_grasp->contact_policy_revision};
    }
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
        std::move(contact_policy),
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
        binding.program_id,
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

  void Trace(std::string value) {
    if (timeline != nullptr) {
      std::unique_lock lock(timeline_mutex != nullptr ? *timeline_mutex : fallback_timeline_mutex_);
      timeline->push_back(std::move(value));
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
  Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(const RequestOwnerEpoch&, std::int64_t,
                                                      std::size_t,
                                                      const std::vector<ResourceId>&) override {
    calls = {"begin"};
    states.clear();
    return Result<OperationJournalRecoveryDTO>::Ok(recovery);
  }

  Result<std::uint64_t> RecordOperationAccepted(
      const RequestOwnerEpoch&, const OperationIntentDTO&, const OperationAcceptedDTO&,
      const OperationStateDTO& state,
      const std::span<const OperationId> protected_operation_ids) override {
    {
      std::lock_guard lock(mutex);
      protected_operation_inventories.emplace_back(protected_operation_ids.begin(),
                                                   protected_operation_ids.end());
    }
    if (operation_acceptance_hook) {
      operation_acceptance_hook();
    }
    return Record("operation_accepted", state);
  }

  Result<std::uint64_t> RecordControlAccepted(const RequestOwnerEpoch&,
                                              const OperationControlRequestDTO&,
                                              const OperationControlAcceptedDTO&,
                                              const OperationStateDTO& state) override {
    if (control_acceptance_hook) {
      control_acceptance_hook();
    }
    return Record("control_accepted", state);
  }

  EmergencyStopAcceptanceProjectionResultDTO RecordStopAccepted(
      const RequestOwnerEpoch&, const EmergencyStopAcceptanceRecordDTO& acceptance,
      const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
      const EmergencyStopAffectedStateSnapshotDTO& affected_state) noexcept override {
    if (stop_acceptance_hook) {
      stop_acceptance_hook();
    }
    std::lock_guard lock(mutex);
    calls.push_back("stop_accepted");
    if (timeline != nullptr) {
      std::unique_lock timeline_lock(timeline_mutex != nullptr ? *timeline_mutex
                                                               : fallback_timeline_mutex_);
      timeline->push_back("journal:stop_accepted");
    }
    last_stop_acceptance = acceptance;
    last_stop_intent = intent;
    last_stop_accepted = accepted;
    last_stop_affected_state = affected_state;
    projected_stop_acceptances.push_back(accepted.stop_id);
    if (fail_next_kind == "stop_accepted") {
      fail_next_kind.reset();
      ++stop_acceptance_calls;
      return EmergencyStopAcceptanceProjectionResultDTO{
          EmergencyStopProjectionStateDTO::kFailed, std::nullopt,
          ErrorDTO{"operation_journal_test_failure",
                   "injected journal failure",
                   {{"reason", "injected_journal_failure"}}}};
    }
    states.insert(states.end(), affected_state.operations.begin(), affected_state.operations.end());
    ++stop_acceptance_calls;
    return EmergencyStopAcceptanceProjectionResultDTO{EmergencyStopProjectionStateDTO::kRecorded,
                                                      next_sequence++, std::nullopt};
  }

  StopForensicProjectionResultDTO RecordStopTerminal(
      const RequestOwnerEpoch&, const StopStatusDTO& status, const StopCertificateDTO& certificate,
      const std::optional<StopFaultLatchDTO>& fault_latch) noexcept override {
    std::lock_guard lock(mutex);
    calls.push_back("stop_terminal");
    last_stop_status = status;
    last_stop_certificate = certificate;
    last_stop_fault_latch = fault_latch;
    projected_stop_terminals.push_back(certificate.stop_id);
    ++stop_terminal_calls;
    if (fail_next_kind == "stop_terminal") {
      fail_next_kind.reset();
      return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed, std::nullopt,
                                             ErrorDTO{"operation_journal_test_failure",
                                                      "injected journal failure",
                                                      {{"reason", "injected_journal_failure"}}}};
    }
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kRecorded,
                                           next_sequence++, std::nullopt};
  }

  StopForensicProjectionResultDTO RecordStopSurveillanceCheckpoint(
      const RequestOwnerEpoch&, const StopPostTerminalSurveillanceDTO& checkpoint,
      const StopFaultLatchDTO& fault_latch, const std::int64_t projected_at_ns) noexcept override {
    std::lock_guard lock(mutex);
    calls.push_back("stop_surveillance_checkpoint");
    last_stop_surveillance = checkpoint;
    last_stop_fault_latch = fault_latch;
    last_stop_surveillance_projected_at_ns = projected_at_ns;
    ++stop_surveillance_calls;
    if (fail_next_kind == "stop_surveillance_checkpoint") {
      fail_next_kind.reset();
      return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed, std::nullopt,
                                             ErrorDTO{"operation_journal_test_failure",
                                                      "injected journal failure",
                                                      {{"reason", "injected_journal_failure"}}}};
    }
    return StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kRecorded,
                                           next_sequence++, std::nullopt};
  }

  Result<std::uint64_t> RecordStopFaultCleared(
      const RequestOwnerEpoch&, const StopFaultRecoveryIntentDTO& recovery_intent,
      const StopFaultRecoveryResultDTO& recovery_result, const StopFaultLatchDTO& cleared_latch,
      const StopStatusDTO& resulting_stop_status) override {
    if (stop_fault_clear_hook) stop_fault_clear_hook();
    std::lock_guard lock(mutex);
    ++stop_fault_clear_calls;
    calls.push_back("stop_fault_cleared");
    last_stop_recovery_intent = recovery_intent;
    last_stop_recovery_result = recovery_result;
    last_stop_fault_latch = cleared_latch;
    last_stop_status = resulting_stop_status;
    if (fail_next_kind == "stop_fault_cleared") {
      fail_next_kind.reset();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> CloseOwnerEpoch(const RequestOwnerEpoch&, std::int64_t) override {
    std::lock_guard lock(mutex);
    calls.push_back("owner_closed");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordRuntimeScaleAccepted(const RequestOwnerEpoch&,
                                                   const RuntimeScaleRequestDTO&,
                                                   const RuntimeScaleAcceptedDTO&,
                                                   const OperationStateDTO& state,
                                                   const RuntimeScaleStatusDTO&) override {
    return Record("runtime_scale_accepted", state);
  }

  Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch&, const OperationStateDTO& state, const RuntimeScaleStatusDTO*,
      const char* transition_reason, std::span<const PreparedMotionDiagnosticsDTO>) override {
    return Record(std::string("state:") + transition_reason, state);
  }

  Result<std::uint64_t> RecordCellAdmissionClaimAcquired(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&) override {
    if (cell_admission_acquire_hook) {
      cell_admission_acquire_hook();
    }
    std::lock_guard lock(mutex);
    calls.push_back("cell_admission_acquired");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordCellAdmissionClaimReleased(const RequestOwnerEpoch&,
                                                         const CellAdmissionClaimDTO&,
                                                         const CellAdmissionClaimReleaseDTO&,
                                                         AdmissionGeneration) override {
    std::lock_guard lock(mutex);
    if (std::exchange(fail_next_cell_release, false))
      return Result<std::uint64_t>::Err(
          ErrorDTO{"operation_journal_test_failure", "injected claim release failure", {}});
    calls.push_back("cell_admission_released");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch&, const CellAdmissionReconcileDTO&,
      const CellAdmissionReconcileValidationDTO&, AdmissionGeneration) override {
    std::lock_guard lock(mutex);
    calls.push_back("cell_admission_reconciled");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> RecordCellAdmissionGenerationAdvanced(const RequestOwnerEpoch&,
                                                              AdmissionGeneration,
                                                              AdmissionGeneration) override {
    if (cell_admission_generation_hook) {
      cell_admission_generation_hook();
    }
    std::lock_guard lock(mutex);
    calls.push_back("cell_admission_generation_advanced");
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  Result<std::uint64_t> Record(std::string kind, const OperationStateDTO& state) {
    if (record_hook) {
      record_hook(kind);
    }
    std::lock_guard lock(mutex);
    calls.push_back(kind);
    if (timeline != nullptr) {
      std::unique_lock timeline_lock(timeline_mutex != nullptr ? *timeline_mutex
                                                               : fallback_timeline_mutex_);
      timeline->push_back("journal:" + kind);
    }
    if (fail_next_kind == kind) {
      fail_next_kind.reset();
      return Result<std::uint64_t>::Err(ErrorDTO{"operation_journal_test_failure",
                                                 "injected journal failure",
                                                 {{"reason", "injected_journal_failure"}}});
    }
    states.push_back(state);
    return Result<std::uint64_t>::Ok(next_sequence++);
  }

  OperationJournalRecoveryDTO recovery;
  std::optional<std::string> fail_next_kind;
  bool fail_next_cell_release{};
  std::vector<std::string> calls;
  std::atomic<int> stop_acceptance_calls{};
  std::atomic<int> stop_terminal_calls{};
  std::atomic<int> stop_surveillance_calls{};
  std::atomic<int> stop_fault_clear_calls{};
  std::vector<OperationStateDTO> states;
  std::vector<std::vector<OperationId>> protected_operation_inventories;
  std::optional<EmergencyStopAcceptanceRecordDTO> last_stop_acceptance;
  std::optional<EmergencyStopProjectedIntentDTO> last_stop_intent;
  std::optional<StopAcceptedDTO> last_stop_accepted;
  std::optional<EmergencyStopAffectedStateSnapshotDTO> last_stop_affected_state;
  std::optional<StopStatusDTO> last_stop_status;
  std::optional<StopCertificateDTO> last_stop_certificate;
  std::optional<StopFaultLatchDTO> last_stop_fault_latch;
  std::optional<StopPostTerminalSurveillanceDTO> last_stop_surveillance;
  std::optional<std::int64_t> last_stop_surveillance_projected_at_ns;
  std::optional<StopFaultRecoveryIntentDTO> last_stop_recovery_intent;
  std::optional<StopFaultRecoveryResultDTO> last_stop_recovery_result;
  std::function<void()> operation_acceptance_hook;
  std::function<void()> control_acceptance_hook;
  std::function<void()> stop_acceptance_hook;
  std::function<void()> stop_fault_clear_hook;
  std::function<void()> cell_admission_acquire_hook;
  std::function<void()> cell_admission_generation_hook;
  std::function<void(std::string_view)> record_hook;
  std::vector<StopId> projected_stop_acceptances;
  std::vector<StopId> projected_stop_terminals;
  std::uint64_t next_sequence{1U};
  std::vector<std::string>* timeline{};
  std::mutex* timeline_mutex{};
  std::mutex fallback_timeline_mutex_;
  std::mutex mutex;
};

class FakeStopSurveillanceObserver final : public IStopSurveillanceObservationPort {
 public:
  StopSurveillanceObservationSampleDTO Observe(
      const StopSurveillanceObservationQueryDTO& query) noexcept override {
    try {
      ++observe_calls;
      const auto sequence = next_sequence.fetch_add(1U);
      const auto selected = evidence_state.load();
      return StopSurveillanceObservationSampleDTO{
          query.stop_certificate,
          query.scope_key,
          query.evidence_requirement.evidence_item_id,
          query.resolved_source,
          StopSurveillanceSourceAvailabilityDTO::kAvailable,
          StopEvidenceItemDTO{
              query.evidence_requirement.evidence_item_id, query.evidence_requirement.evidence_kind,
              selected, query.resolved_source,
              query.resolved_source.runtime_target.provider_runtime_epoch, sequence, 0,
              selected == StopEvidenceItemStateDTO::kSatisfied
                  ? std::optional<JsonValueDTO>(JsonValueDTO{true})
                  : std::nullopt,
              selected == StopEvidenceItemStateDTO::kFailed
                  ? std::optional<ErrorDTO>(
                        ErrorDTO{"continued_motion_observed", "continued motion observed", {}})
                  : std::nullopt},
          std::nullopt};
    } catch (...) {
      return StopSurveillanceObservationSampleDTO{
          query.stop_certificate,
          query.scope_key,
          query.evidence_requirement.evidence_item_id,
          query.resolved_source,
          StopSurveillanceSourceAvailabilityDTO::kUnavailable,
          std::nullopt,
          ErrorDTO{"stop_surveillance_test_observer_failure", "test observer failed", {}}};
    }
  }

  std::atomic<StopEvidenceItemStateDTO> evidence_state{StopEvidenceItemStateDTO::kFailed};
  std::atomic<std::uint64_t> next_sequence{1U};
  std::atomic<int> observe_calls{};
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
                                       RuntimeEpoch("manager.epoch.1"),
                                       RequestOwnerEpoch("manager.owner.1"),
                                       {}};
  result.required_runtime_set_digest = ComputeRequiredRuntimeSetDigest(result.required_receivers);
  return result;
}

[[nodiscard]] ResolvedStopBasePlanDTO stop_base_plan_for(
    const RuntimeParticipantBinding& participant) {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
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
    std::vector<RuntimeParticipantBinding> domain_members = {}) {
  auto plan = stop_base_plan_for(participant);
  if (include_controlled_fallback) {
    auto intermediate = plan.escalation_stages.front();
    intermediate.stage_index = 1U;
    intermediate.stage_kind = StopEscalationStageKind::kControlledFallback;
    intermediate.stop_escalation_stage_id = StopEscalationStageId("stage.intermediate");
    intermediate.command_qualification.controlled_substitution_contract.reset();
    intermediate.containment_command.command_phase_id =
        StopBudgetPhaseId("phase.intermediate.command");
    auto& evidence = intermediate.evidence_requirements.front().requirement;
    evidence.phase_id = StopBudgetPhaseId("phase.intermediate.rest");
    evidence.evidence_item_id = StopEvidenceItemId("evidence.intermediate.rest");
    plan.escalation_stages.back().stage_index = 2U;
    plan.escalation_stages.insert(plan.escalation_stages.begin() + 1, std::move(intermediate));
    plan.deadline_budget.phases.insert(
        plan.deadline_budget.phases.begin() + 2,
        {{StopBudgetPhaseId("phase.intermediate.command"), 2'250'000'000},
         {StopBudgetPhaseId("phase.intermediate.rest"), 2'500'000'000}});
    auto sealed = MakeResolvedStopBasePlan(std::move(plan));
    if (!sealed.ok()) throw std::logic_error(sealed.error().message);
    plan = std::move(sealed.value());
  }
  auto expected = controller_domain_for(participant, plan, std::move(domain_members));
  auto readiness = verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO{participant.value().controllable_component_id,
                                         ComponentLifecycleGeneration(1U), expected.value(), plan},
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

[[nodiscard]] OrphanResourceReconciliationFactsDTO orphan_reconciliation_facts(
    const RequestOwnerEpoch& owner_epoch, const CellId& cell_id,
    const std::uint64_t operation_resource_generation,
    const std::vector<RuntimeParticipantBinding>& bindings, std::vector<ResourceId> resources,
    const std::int64_t assessed_at_ns = 0) {
  std::sort(resources.begin(), resources.end());
  std::vector<OrphanResourceReconciliationFactDTO> facts;
  facts.reserve(resources.size());
  for (const auto& resource : resources) {
    const RuntimeParticipantBinding* selected = nullptr;
    for (const auto& binding : bindings) {
      const bool contains =
          std::any_of(binding.value().resources.begin(), binding.value().resources.end(),
                      [&](const auto& row) { return row.resource_id == resource; });
      if (contains) {
        if (selected != nullptr) throw std::logic_error("ambiguous orphan test resource");
        selected = &binding;
      }
    }
    if (selected == nullptr || !selected->value().control_path.has_value()) {
      throw std::logic_error("missing orphan test resource");
    }
    const auto& value = selected->value();
    const auto& path = *value.control_path;
    facts.push_back(OrphanResourceReconciliationFactDTO{
        resource, value.controllable_component_id, value.command_target_id, path.control_domain_id,
        path.control_binding_id, path.control_binding_revision,
        path.active_control_configuration_id, path.active_control_configuration_generation,
        path.provider_runtime_unit_id, path.provider_runtime_epoch,
        StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                         path.provider_runtime_epoch, 1U, 0, 100,
                                         ContentDigest(test_digest('4')), std::nullopt},
        ContentDigest(test_digest('5')), ContentDigest(test_digest('6'))});
  }
  return OrphanResourceReconciliationFactsDTO{owner_epoch,
                                              cell_id,
                                              bindings.front().value().retained_bundle_id,
                                              bindings.front().value().graph_revision,
                                              operation_resource_generation,
                                              assessed_at_ns,
                                              std::move(facts)};
}

[[nodiscard]] RuntimeParticipantBinding make_binding(
    std::string target_role, std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt,
    bool include_second_angular_resource = false,
    std::string planner_capability_revision =
        "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
    bool include_contact_grasp = false) {
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
  std::vector<ProgramContactGraspBindingDTO> contact_grasps;
  if (include_contact_grasp) {
    const CollisionBodyRoleId body_role("gripper_collision");
    contact_grasps.push_back(ProgramContactGraspBindingDTO{
        ContactPolicyId("selected_object_grasp"),
        ContactPolicyRevision("contact-policy-revision"),
        RuntimeObjectId("selected_object"),
        {ProgramContactGraspBodyBindingDTO{
            ComponentRoleId("robot"),
            TargetRoleId("gripper"),
            ControllableComponentId("robot.1"),
            CommandTargetId("gripper"),
            ModelRoleId("body"),
            body_role,
            ModelInstanceId("robot.model"),
            ModelRevision("model-revision"),
            {make_component_collision_body_id(ControllableComponentId("robot.1"),
                                              ModelInstanceId("robot.model"), body_role, 0U)}}},
        0.002,
        0.8});
  }
  RuntimeParticipantBindingDTO value{
      ProgramId("program.pick"),
      ProgramRevision(test_digest('0')),
      ProgramBindingId(test_digest('1')),
      RetainedBundleId(test_digest('6')),
      GraphRevision(test_digest('7')),
      ComponentRoleId("robot"),
      TargetRoleId(std::move(target_role)),
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
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
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
      std::move(contact_grasps)};
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
  value.program_id = ProgramId("program.direct-control");
  value.program_revision = ProgramRevision(test_digest('2'));
  value.program_binding_id = ProgramBindingId(test_digest('3'));
  value.component_role_id = ComponentRoleId("controlled_component");
  value.target_role_id = TargetRoleId("command_target");
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
  value.component_role_id = ComponentRoleId("robot_" + suffix);
  value.target_role_id = TargetRoleId(value.target_role_id.value() + "_" + suffix);
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
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding second_occurrence_sharing_first_resource(
    const RuntimeParticipantBinding& source) {
  auto value = second_same_type_occurrence(source).value();
  value.controllable_component_id = source.value().controllable_component_id;
  value.target_role_id = TargetRoleId("arm.overlap");
  value.resources.front().resource_id = source.value().resources.front().resource_id;
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

[[nodiscard]] RuntimeParticipantBinding indexed_same_owner_control_binding_sibling(
    const RuntimeParticipantBinding& source, const std::size_t index) {
  auto value = same_owner_control_binding_sibling(source).value();
  const auto suffix = std::to_string(index);
  value.program_binding_id = ProgramBindingId("program-binding.sibling." + suffix);
  value.capability_id = CapabilityId("execution.trajectory.sibling." + suffix);
  auto& control = *value.control_path;
  control.control_binding_id = ControlBindingId("binding.arm.sibling." + suffix);
  control.control_route_id = ControlRouteId("route.native.sibling." + suffix);
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

/// Publish one component result under the Kernel's restart exclusion, the
/// positive authority owner attachment holds while reconstructing facts.
[[nodiscard]] Result<ComponentCommandReadinessDTO> publish_under_restart_exclusion(
    ResourceAddressedExecutionKernel& kernel, const ControllableComponentId& component,
    const ComponentLifecycleGeneration generation, const bool eligible) {
  if (!kernel.SetRouteRecoveryPending(true).ok()) {
    throw std::logic_error("test restart exclusion could not be held");
  }
  auto published = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt, std::nullopt, component,
       generation,
       eligible});
  if (!kernel.SetRouteRecoveryPending(false).ok()) {
    throw std::logic_error("test restart exclusion could not be released");
  }
  return published;
}

/// Publish the component of `registered` as command-eligible at its sealed
/// generation.
void enable_component_commands(ResourceAddressedExecutionKernel& kernel,
                               const RuntimeParticipantExecutionRegistration& registered) {
  const auto& readiness = registered.stop_readiness->value();
  const auto enabled = publish_under_restart_exclusion(
      kernel, readiness.controllable_component_id, readiness.component_lifecycle_generation, true);
  ASSERT_TRUE(enabled.ok()) << enabled.error().message;
  ASSERT_TRUE(enabled.value().command_eligible);
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

[[nodiscard]] OperationIntentDTO motion_operation(std::string request_id = "request.motion.1") {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision(test_digest('0')),
      ProgramBindingId(test_digest('1')),
      ProgramRunId("run.1"),
      GraphRevision(test_digest('7')),
      RetainedBundleId(test_digest('6')),
      std::nullopt,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("arm"),
          CapabilityId("execution.trajectory.execute"), std::nullopt,
          MotionParticipantIntentDTO{MoveId("move.1"),
                                     FrameRoleId("base"),
                                     FrameRoleId("tcp"),
                                     TolerancesDTO{0.01, 0.001, 0.01},
                                     SpeedProfileDTO{},
                                     TimeoutsDTO{},
                                     CacheMode::kDefault,
                                     PlanningProfileDTO::kBalanced,
                                     MotionTargetModeDTO::kAbsolute,
                                     std::nullopt,
                                     std::nullopt,
                                     JointGoalDTO{{"joint_1"}, {0.2}},
                                     std::nullopt,
                                     {{"joint_1", JointDynamicsLimitDTO{1.0, 2.0, std::nullopt}}},
                                     {}}}},
      OperationCoordinationPolicy::kIndependent};
}

TEST(ResourceAddressedExecutionKernelTest,
     ProjectsResidentOperationsOnlyForExactProgramRunIdentities) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.resident-projection"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_TRUE(kernel.get_resident_operations({ProgramRunId("other.run")}).empty());
  const auto projected = kernel.get_resident_operations({ProgramRunId("run.1")});
  ASSERT_EQ(projected.size(), 1U);
  EXPECT_EQ(projected.front().operation_id, accepted.value().operation_id);
  EXPECT_EQ(projected.front().program_run_id, ProgramRunId("run.1"));
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

[[nodiscard]] OperationIntentDTO coordinated_motion_operation(
    std::string request_id, const std::size_t participant_count = 2U) {
  auto operation = motion_operation(std::move(request_id));
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  operation.participants.reserve(participant_count);
  for (std::size_t index = 1U; index < participant_count; ++index) {
    auto participant = operation.participants.front();
    const auto suffix = std::to_string(index + 1U);
    participant.component_role_id = ComponentRoleId("robot_" + suffix);
    participant.target_role_id = TargetRoleId("arm_" + suffix);
    std::get<MotionParticipantIntentDTO>(participant.payload).move_id = MoveId("move." + suffix);
    operation.participants.push_back(std::move(participant));
  }
  return operation;
}

[[nodiscard]] OperationIntentDTO command_operation(std::string request_id = "request.command.1") {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  auto operation = motion_operation(std::move(request_id));
  operation.participants.front() = OperationParticipantIntentDTO{
      ComponentRoleId("robot"), TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
      std::nullopt,
      CapabilityCommandIntentDTO{
          schema.command_schema_id,
          schema.command_schema_revision,
          GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
          2.0,
          {CommandDurationOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}};
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
    participant.component_role_id = ComponentRoleId("robot_" + suffix);
    participant.target_role_id = TargetRoleId("gripper_" + suffix);
    operation.participants.push_back(std::move(participant));
  }
  return operation;
}

[[nodiscard]] OperationIntentDTO coordinated_motion_command_operation(std::string request_id) {
  auto operation = motion_operation(std::move(request_id));
  operation.coordination_policy = OperationCoordinationPolicy::kCoordinatedAll;
  auto command = command_operation().participants.front();
  command.component_role_id = ComponentRoleId("robot_2");
  command.target_role_id = TargetRoleId("gripper_2");
  operation.participants.push_back(std::move(command));
  return operation;
}

[[nodiscard]] OperationControlRequestDTO control_request(std::string request_id,
                                                         const OperationAcceptedDTO& accepted,
                                                         std::uint64_t expected_revision) {
  return OperationControlRequestDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      accepted.operation_id, OperationRevision(expected_revision), std::nullopt};
}

[[nodiscard]] StopIntentDTO stop_request(std::string request_id,
                                         const OperationAcceptedDTO& accepted,
                                         std::uint64_t expected_revision,
                                         std::uint64_t expected_stop_generation = 0U) {
  return StopIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      std::vector<StopOperationTargetDTO>{
          StopOperationTargetDTO{accepted.operation_id, OperationRevision(expected_revision),
                                 StopGeneration(expected_stop_generation)}},
      std::nullopt};
}

[[nodiscard]] StopIntentDTO program_run_stop_request(
    std::string request_id, std::string program_run_id,
    std::string request_owner_epoch = "kernel.epoch.1") {
  return StopIntentDTO{PublicRequestContextDTO{RequestId(std::move(request_id)),
                                               RequestOwnerEpoch(std::move(request_owner_epoch)),
                                               "test", std::nullopt},
                       std::vector<StopProgramRunTargetDTO>{
                           StopProgramRunTargetDTO{ProgramRunId(std::move(program_run_id))}},
                       std::nullopt};
}

[[nodiscard]] RuntimeScaleRequestDTO runtime_scale_request(std::string request_id,
                                                           const OperationAcceptedDTO& accepted,
                                                           std::uint64_t expected_revision,
                                                           std::uint64_t expected_generation,
                                                           double scale) {
  return RuntimeScaleRequestDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      accepted.operation_id, OperationRevision(expected_revision),
      RuntimeScaleGeneration(expected_generation), scale};
}

[[nodiscard]] ProgramRunExecutionCaptureDTO program_execution_capture() {
  const auto intent = motion_operation();
  auto binding = std::make_shared<ProgramBindingDTO>(ProgramBindingDTO{
      std::string(kProgramBindingApiVersion),
      intent.program_binding_id,
      intent.program_id,
      intent.program_revision,
      intent.expected_retained_bundle_id,
      intent.expected_graph_revision,
      {"test.catalog", "1", ContentDigest(test_digest('0')), ContentDigest(test_digest('0'))},
      {},
      {},
      {ContentDigest(test_digest('0')), ContentDigest(test_digest('0')), {}, {}}});
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular).value();
  ProgramTargetRoleBindingDTO target{physical.target_role_id,
                                     physical.component_role_id,
                                     physical.controllable_component_id,
                                     physical.command_target_id,
                                     {},
                                     {{ResourceRoleId("arm_joints"),
                                       {physical.resources.front().resource_id},
                                       {physical.resources.front().compiled_resource_name}}},
                                     {},
                                     {}};
  binding->target_role_bindings.push_back(std::move(target));
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("move"),
                   "kernel.operation",
                   {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("timer"), "orchestration.clock", {}}};
  table->named_selections = {{"arm", {0U}}, {"supervision", {1U}}};
  return {intent.request_context.request_owner_epoch,
          RequestOwnerEpoch("program.owner"),
          CellAdmissionClaimGeneration(1U),
          intent.program_run_id,
          intent.program_id,
          intent.program_revision,
          intent.program_binding_id,
          intent.expected_retained_bundle_id,
          intent.expected_graph_revision,
          std::move(table),
          std::move(binding)};
}

[[nodiscard]] Result<AuthoringAdmissionDTO> ready_cell_admission(
    ResourceAddressedExecutionKernel& kernel) {
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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

[[nodiscard]] Result<CellAdmissionClaimDTO> claim_program_execution(
    ResourceAddressedExecutionKernel& kernel) {
  const auto updated = ready_cell_admission(kernel);
  if (!updated.ok()) return Result<CellAdmissionClaimDTO>::Err(updated.error());
  return kernel.AcquireCellAdmissionClaim(CellAdmissionClaimIntentDTO{
      CellAdmissionClaimKindDTO::kProgramCycle,
      CellAdmissionOwnerRoleDTO::kProgramRunner,
      RequestOwnerEpoch("program.owner"),
      "run.1",
      RequestId("request.program-cycle.claim"),
      ContentDigest(test_digest('0')),
      kernel.GetAuthoringAdmission().admission_generation,
      {},
      ProgramCycleAdmissionEvidenceDTO{ProgramRevision(test_digest('0')),
                                       ProgramRevision(test_digest('0'))},
      std::nullopt});
}

[[nodiscard]] OperationIntentDTO scoped_motion_operation(std::string request_id) {
  auto intent = motion_operation(std::move(request_id));
  intent.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("move"), "move.attempt.1"};
  return intent;
}

[[nodiscard]] ProgramExecutionSelectionRequestDTO program_selection_request(std::string request_id,
                                                                            std::string group) {
  return {motion_operation(std::move(request_id)).request_context,
          RequestOwnerEpoch("program.owner"),
          CellAdmissionClaimGeneration(1U),
          ProgramRunId("run.1"),
          {std::move(group)},
          {}};
}

void start_program_motion(ResourceAddressedExecutionKernel& kernel,
                          std::string group = "all", std::string program_run = "run.1",
                          std::string program_owner = "program.owner",
                          const std::uint64_t claim_generation = 1U) {
  const auto scope = kernel.get_program_execution_scope(ProgramRunId(program_run));
  ASSERT_TRUE(scope.ok()) << scope.error().message;
  const auto request_id = "initial.run." + group;
  auto request = program_selection_request(request_id, std::move(group));
  request.program_run_id = ProgramRunId(std::move(program_run));
  request.program_runner_owner_epoch = RequestOwnerEpoch(std::move(program_owner));
  request.claim_generation = CellAdmissionClaimGeneration(claim_generation);
  const auto started =
      kernel.start_program_selection(request, scope.value().selection_revision);
  ASSERT_TRUE(started.ok()) << started.error().message;
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramCaptureRequiresExplicitInitialMotionRunWithoutRecovery) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());

  const auto initial = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  ASSERT_EQ(initial.value().held_nodes.size(), 1U);
  EXPECT_EQ(initial.value().held_nodes.front().node_index, 0U);
  EXPECT_EQ(initial.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kReady);

  const auto before_run = kernel.execute(scoped_motion_operation("before.initial.run"));
  ASSERT_FALSE(before_run.ok());
  EXPECT_EQ(before_run.error().details.at("reason"), "program_node_stop_fence_active");
  EXPECT_EQ(engine.prepare_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);

  // Resume is the same release as Run: it also releases never-started work.
  const auto started = kernel.release_program_selection(
      program_selection_request("resume.as.initial.run", "arm"),
      initial.value().selection_revision);
  ASSERT_TRUE(started.ok()) << started.error().message;
  EXPECT_TRUE(started.value().held_nodes.empty());
  const auto admitted = kernel.execute(scoped_motion_operation("after.initial.run"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

[[nodiscard]] RuntimeScaleControlSelectorDTO runtime_scale_control_selector(
    std::string component_role = "robot", std::string target_role = "arm") {
  return RuntimeScaleControlSelectorDTO{ProgramBindingId(test_digest('1')),
                                        ComponentRoleId(std::move(component_role)),
                                        TargetRoleId(std::move(target_role))};
}

[[nodiscard]] RuntimeScaleControlRequestDTO runtime_scale_control_request(
    std::string request_id, std::uint64_t expected_generation, double scale) {
  return RuntimeScaleControlRequestDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      runtime_scale_control_selector(), RuntimeScaleControlGeneration(expected_generation), scale};
}

[[nodiscard]] RuntimeScaleControlSelectorDTO runtime_scale_group_selector() {
  return {ProgramBindingId(test_digest('1')),
          ComponentRoleId(""),
          TargetRoleId(""),
          {{ComponentRoleId("robot"), TargetRoleId("arm")},
           {ComponentRoleId("robot_2"), TargetRoleId("arm_2")}}};
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleGroupRetainsCanonicalAndLatestScopeWritesMembers) {
  for (const bool configured : {false, true}) {
    SCOPED_TRACE(configured);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeOperationJournal journal;
    FakeParticipantEngine first_engine, second_engine, third_engine;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine),
         registration(coordinated_occurrence(first, "2"), second_engine),
         registration(coordinated_occurrence(first, "3"), third_engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    ASSERT_TRUE(kernel
                    .set_runtime_scale_control(
                        runtime_scale_control_request("scale.independent.first", 0U, 0.2))
                    .ok());
    auto second_request = runtime_scale_control_request("scale.independent.second", 0U, 0.8);
    second_request.selector = runtime_scale_control_selector("robot_2", "arm_2");
    ASSERT_TRUE(kernel.set_runtime_scale_control(second_request).ok());
    auto group_request = runtime_scale_control_request("scale.synchronized", 0U, 0.35);
    group_request.selector = runtime_scale_group_selector();
    const auto initial = kernel.get_runtime_scale_control(group_request.selector);
    ASSERT_TRUE(initial.ok());
    ASSERT_TRUE(initial.value().supported);
    EXPECT_FALSE(initial.value().frozen);
    EXPECT_DOUBLE_EQ(initial.value().requested_scale, 1.0);
    EXPECT_EQ(initial.value().control_generation, RuntimeScaleControlGeneration(0U));
    EXPECT_FALSE(initial.value().control_binding_id);
    ASSERT_EQ(initial.value().group_members.size(), 2U);
    if (configured) {
      ASSERT_TRUE(kernel.set_runtime_scale_control(group_request).ok());
    }
    auto reversed = group_request.selector;
    std::reverse(reversed.group_targets.begin(), reversed.group_targets.end());
    const auto alias = kernel.get_runtime_scale_control(reversed);
    ASSERT_TRUE(alias.ok());
    EXPECT_DOUBLE_EQ(alias.value().requested_scale, configured ? 0.35 : 1.0);
    EXPECT_EQ(alias.value().control_generation,
              RuntimeScaleControlGeneration(configured ? 1U : 0U));
    auto overlapping = runtime_scale_control_request("scale.overlapping", 0U, 0.65);
    overlapping.selector = {ProgramBindingId(test_digest('1')),
                            ComponentRoleId(""),
                            TargetRoleId(""),
                            {{ComponentRoleId("robot_2"), TargetRoleId("arm_2")},
                             {ComponentRoleId("robot_3"), TargetRoleId("arm_3")}}};
    ASSERT_TRUE(kernel.set_runtime_scale_control(overlapping).ok());
    EXPECT_EQ(first_engine.runtime_scale_calls, 0);
    EXPECT_EQ(second_engine.runtime_scale_calls, 0);
    EXPECT_EQ(third_engine.runtime_scale_calls, 0);

    // Scope commands write through to the member settings used by ordinary
    // independent moves. The latest accepted scope affecting a member wins
    // only for that member: the overlapping command changes robot_2, but not
    // robot_1. The exact group setting remains its own replay/generation
    // authority and reports the now-mixed effective member values.
    const auto group_after_overlap = kernel.get_runtime_scale_control(group_request.selector);
    ASSERT_TRUE(group_after_overlap.ok());
    EXPECT_DOUBLE_EQ(group_after_overlap.value().requested_scale, configured ? 0.35 : 1.0);
    EXPECT_FALSE(group_after_overlap.value().effective_scale.has_value());
    EXPECT_EQ(group_after_overlap.value().application_state,
              RuntimeScaleApplicationState::kApplied);

    const auto admitted = kernel.execute(coordinated_motion_operation("motion.synchronized"));
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    for (const auto* engine : {&first_engine, &second_engine}) {
      ASSERT_TRUE(engine->last_prepared_request);
      const auto& scale = engine->last_prepared_request->execution_request.initial_runtime_scale;
      ASSERT_TRUE(scale);
      EXPECT_DOUBLE_EQ(scale->requested_scale, configured ? 0.35 : 1.0);
    }
    const auto first_setting = kernel.get_runtime_scale_control(runtime_scale_control_selector());
    const auto second_setting = kernel.get_runtime_scale_control(second_request.selector);
    ASSERT_TRUE(first_setting.ok());
    ASSERT_TRUE(second_setting.ok());
    EXPECT_DOUBLE_EQ(first_setting.value().requested_scale, configured ? 0.35 : 0.2);
    EXPECT_DOUBLE_EQ(second_setting.value().requested_scale, 0.65);
    EXPECT_TRUE(first_setting.value().frozen);
    const auto busy = kernel.get_runtime_scale_control(overlapping.selector);
    ASSERT_TRUE(busy.ok());
    EXPECT_TRUE(busy.value().frozen);
    ASSERT_TRUE(busy.value().reason);
    EXPECT_EQ(busy.value().reason->details.at("reason"), "runtime_scale_group_scope_busy");
    EXPECT_DOUBLE_EQ(busy.value().requested_scale, 0.65);
  }
}

TEST(ResourceAddressedExecutionKernelTest, RuntimeScaleGroupRejectsMalformedOrClosedMembers) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto first_registration = registration(first, first_engine);
  ResourceAddressedExecutionKernel kernel(
      {first_registration, registration(coordinated_occurrence(first, "2"), second_engine)}, clock,
      scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  for (int malformed = 0; malformed < 4; ++malformed) {
    auto request = runtime_scale_control_request("scale.bad." + std::to_string(malformed), 0U, 0.5);
    request.selector = runtime_scale_group_selector();
    if (malformed == 0) request.selector.component_role_id = ComponentRoleId("robot");
    if (malformed == 1) request.selector.group_targets.pop_back();
    if (malformed == 2) request.selector.group_targets[1] = request.selector.group_targets[0];
    if (malformed == 3) request.selector.group_targets[1].target_role_id = TargetRoleId("missing");
    const auto status = kernel.get_runtime_scale_control(request.selector);
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(status.value().supported);
    EXPECT_FALSE(kernel.set_runtime_scale_control(request).ok());
  }
  auto unknown = route_configuration_for(first_registration);
  unknown.state = ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  ASSERT_TRUE(kernel.publish_runtime_route_configuration(std::move(unknown)).ok());
  auto request = runtime_scale_control_request("scale.closed", 0U, 0.5);
  request.selector = runtime_scale_group_selector();
  const auto closed = kernel.get_runtime_scale_control(request.selector);
  ASSERT_TRUE(closed.ok());
  EXPECT_TRUE(closed.value().supported);
  EXPECT_TRUE(closed.value().frozen);
  EXPECT_FALSE(kernel.set_runtime_scale_control(request).ok());
  EXPECT_EQ(first_engine.runtime_scale_calls, 0);
  EXPECT_EQ(second_engine.runtime_scale_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     IndependentProgramGroupProjectsAndAppliesOneSettingToBothOperations) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 32U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  auto second_target = binding->target_role_bindings.front();
  second_target.component_role_id = ComponentRoleId("robot_2");
  second_target.target_role_id = TargetRoleId("arm_2");
  second_target.controllable_component_id = ControllableComponentId("robot_2");
  second_target.command_target_id = CommandTargetId("arm");
  binding->target_role_bindings.push_back(std::move(second_target));
  capture.binding = std::move(binding);
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("a"),
                   "kernel.operation",
                   {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("b"),
                   "kernel.operation",
                   {{TargetRoleId("arm_2"), CapabilityId("execution.trajectory.execute"), true}}}};
  table->named_selections = {{"robot_group", {0U, 1U}, ProgramExecutionPolicy::kIndependent}};
  capture.table = table;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);

  auto first_intent = motion_operation("group.scale.first");
  first_intent.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("a"), "attempt.a"};
  ASSERT_TRUE(kernel.execute(first_intent).ok());
  auto second_intent = motion_operation("group.scale.second");
  second_intent.participants.front().component_role_id = ComponentRoleId("robot_2");
  second_intent.participants.front().target_role_id = TargetRoleId("arm_2");
  second_intent.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("b"), "attempt.b"};
  ASSERT_TRUE(kernel.execute(second_intent).ok());
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  first_engine.runtime_scale.active = true;
  second_engine.runtime_scale.active = true;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());

  const auto projected = kernel.present_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(projected.ok()) << projected.error().message;
  EXPECT_EQ(projected.value().selector, runtime_scale_group_selector());
  EXPECT_FALSE(projected.value().frozen);
  const auto first_calls = first_engine.runtime_scale_calls.load();
  const auto second_calls = second_engine.runtime_scale_calls.load();
  auto request = runtime_scale_control_request("group.scale.set", 0U, 0.4);
  request.selector = projected.value().selector;
  const auto accepted = kernel.set_runtime_scale_control(request);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.runtime_scale_calls, first_calls + 1);
  EXPECT_EQ(second_engine.runtime_scale_calls, second_calls + 1);
  ASSERT_TRUE(first_engine.last_runtime_scale_command);
  ASSERT_TRUE(second_engine.last_runtime_scale_command);
  EXPECT_DOUBLE_EQ(first_engine.last_runtime_scale_command->requested_scale, 0.4);
  EXPECT_DOUBLE_EQ(second_engine.last_runtime_scale_command->requested_scale, 0.4);
  EXPECT_DOUBLE_EQ(kernel.get_runtime_scale_control(request.selector).value().requested_scale, 0.4);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleResolvesCapturedProgramRolesToPhysicalRegistrations) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 32U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());

  auto capture = program_execution_capture();
  const ProgramBindingId authored_binding(test_digest('a'));
  capture.program_binding_id = authored_binding;
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  binding->program_binding_id = authored_binding;
  auto second_target = binding->target_role_bindings.front();
  second_target.component_role_id = ComponentRoleId("robot_2");
  second_target.target_role_id = TargetRoleId("arm_2");
  second_target.controllable_component_id = second.value().controllable_component_id;
  second_target.command_target_id = second.value().command_target_id;
  binding->target_role_bindings.push_back(std::move(second_target));
  capture.binding = std::move(binding);
  capture.table = std::make_shared<ProgramExecutionTableDTO>(ProgramExecutionTableDTO{
      {{ProgramNodeInstanceId("a"),
        "kernel.operation",
        {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"), true}}},
       {ProgramNodeInstanceId("b"),
        "kernel.operation",
        {{TargetRoleId("arm_2"), CapabilityId("execution.trajectory.execute"), true}}}},
      {{"robot_group", {0U, 1U}, ProgramExecutionPolicy::kIndependent}}});
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());

  RuntimeScaleControlSelectorDTO selector{authored_binding,
                                          ComponentRoleId(""),
                                          TargetRoleId(""),
                                          {{ComponentRoleId("robot"), TargetRoleId("arm")},
                                           {ComponentRoleId("robot_2"), TargetRoleId("arm_2")}}};
  const auto status = kernel.get_runtime_scale_control(selector);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_TRUE(status.value().supported);
  ASSERT_EQ(status.value().group_members.size(), 2U);
  EXPECT_EQ(status.value().selector, selector);
}

TEST(ResourceAddressedExecutionKernelTest, ManualAdmissionExcludesHeldProgramAndAuthoringClaims) {
  for (const bool program_claim : {false, true}) {
    SCOPED_TRACE(program_claim);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    auto direct = registration(
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        engine);
    direct.intrinsic_direct_control = true;
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                  CellId("cell.test"), 8U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
    ASSERT_TRUE(ready_cell_admission(kernel).ok());
    if (program_claim) {
      ASSERT_TRUE(claim_program_execution(kernel).ok());
    } else {
      ASSERT_TRUE(
          kernel
              .AcquireCellAdmissionClaim({CellAdmissionClaimKindDTO::kAuthoringMutation,
                                          CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                                          RequestOwnerEpoch("authoring.owner"),
                                          "edit.1",
                                          RequestId("edit.1"),
                                          ContentDigest(test_digest('1')),
                                          kernel.GetAuthoringAdmission().admission_generation,
                                          {},
                                          {},
                                          {}})
              .ok());
    }
    auto manual_request = motion_operation();
    manual_request.program_run_id = ProgramRunId("manual.run");
    if (program_claim) {
      CellAdmissionClaimIntentDTO forged{
          CellAdmissionClaimKindDTO::kManualOperation,
          CellAdmissionOwnerRoleDTO::kManualOperationOwner,
          config.request_owner_epoch,
          "forged.manual",
          RequestId("forged.manual"),
          ContentDigest(test_digest('1')),
          kernel.GetAuthoringAdmission().admission_generation,
          {{ResourceId("arm.joint")}, {}},
          {},
          CellAdmissionInterventionEvidenceDTO{
              CellAdmissionOwnerRoleDTO::kManualOperationOwner, config.request_owner_epoch,
              "forged.manual", ContentDigest(test_digest('1')), CellAdmissionClaimGeneration(1U)}};
      ASSERT_TRUE(ValidateCellAdmissionClaimIntent(forged).ok());
      const auto external = kernel.AcquireCellAdmissionClaim(forged);
      ASSERT_FALSE(external.ok());
      EXPECT_EQ(external.error().details.at("reason"), "manual_operation_claim_requires_execute");
    }
    const auto manual = kernel.execute(manual_request);
    ASSERT_FALSE(manual.ok());
    EXPECT_EQ(manual.error().details.at("reason"), "positive_admission_required");
    EXPECT_EQ(engine.execute_calls, 0);
    EXPECT_EQ(engine.prepare_motion_calls, 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest, ManualNoEffectAndTerminalReleaseRemainRepeatable) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 4U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  journal.fail_next_kind = "operation_accepted";
  ASSERT_FALSE(kernel.execute(motion_operation("rejected.before.effect")).ok());
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  for (int attempt = 0; attempt < 8; ++attempt) {
    SCOPED_TRACE(attempt);
    const auto operation = kernel.execute(motion_operation("manual." + std::to_string(attempt)));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    ASSERT_TRUE(engine.last_prepared_request.has_value());
    EXPECT_TRUE(engine.last_prepared_request->execution_request.trajectory_relative_timeout);
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    if (attempt == 0) {
      journal.fail_next_kind = "control_accepted";
      EXPECT_FALSE(kernel.reconcile_manual_operation_admission().ok());
      EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
      journal.fail_next_cell_release = true;
      ASSERT_TRUE(wait_until([&] {
        const auto released = kernel.reconcile_manual_operation_admission();
        if (released.ok()) return false;
        EXPECT_EQ(released.error().code, "operation_journal_test_failure");
        return true;
      }));
      // Retired from the active table, but its claim is still held.
      EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
    }
    ASSERT_TRUE(wait_until([&] {
      return kernel.reconcile_manual_operation_admission().ok() &&
             kernel.GetAuthoringAdmission().authoring_allowed;
    }));
    EXPECT_FALSE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
    EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  }
  EXPECT_TRUE(claim_program_execution(kernel).ok());
}

TEST(ResourceAddressedExecutionKernelTest,
     ExecutionRequestTimeoutPolicyDistinguishesManualAndProgramMotion) {
  for (const bool manual : {false, true}) {
    SCOPED_TRACE(manual);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    auto target = registration(
        make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        engine);
    target.intrinsic_direct_control = manual;
    ResourceAddressedExecutionKernelConfig config{
        RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    ResourceAddressedExecutionKernel kernel(
        {target}, clock, scheduler, journal, std::move(config));

    if (manual) {
      ASSERT_TRUE(ready_cell_admission(kernel).ok());
    } else {
      ASSERT_TRUE(claim_program_execution(kernel).ok());
      ASSERT_TRUE(
          kernel.register_program_run_capture(program_execution_capture()).ok());
      start_program_motion(kernel);
    }

    auto intent = manual
                      ? motion_operation("request.timeout-policy.manual")
                      : scoped_motion_operation("request.timeout-policy.program");
    const auto accepted = kernel.execute(intent);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(engine.last_prepared_request.has_value());
    EXPECT_EQ(engine.last_prepared_request->execution_request
                  .trajectory_relative_timeout,
              manual);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ManualRetirementGuardKeepsSameRevisionTerminalFaultHeld) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  const auto operation = kernel.execute(motion_operation());
  ASSERT_TRUE(operation.ok());
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kStopped);
  const auto clean = kernel.get_operation_state(operation.value().operation_id).value();
  engine.emit_transition(ExecutionState::kStopped, ExecutionState::kStopped,
                         ErrorDTO{"execution_aborted", "late terminal fault", {}});
  const auto faulted = kernel.get_operation_state(operation.value().operation_id).value();
  ASSERT_EQ(clean.operation_revision, faulted.operation_revision);
  ASSERT_TRUE(faulted.participants.front().terminal_error.has_value());
  // Every terminal candidate enters the actual guarded automatic-retirement
  // path. No sleep or production test hook is needed to exercise its recheck.
  ASSERT_TRUE(kernel.reconcile_manual_operation_admission().ok());
  EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
  EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  ASSERT_TRUE(kernel
                  .reset(control_request("explicit.reset", operation.value(),
                                         faulted.operation_revision.value()))
                  .ok());
  ASSERT_TRUE(wait_until([&] {
    return kernel.reconcile_manual_operation_admission().ok() &&
           kernel.GetAuthoringAdmission().authoring_allowed;
  }));
  EXPECT_TRUE(claim_program_execution(kernel).ok());
}

TEST(ResourceAddressedExecutionKernelTest,
     RetiredManualOperationStaysResidentUntilItsAdmissionClaimIsReleased) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  const auto operation = kernel.execute(motion_operation("manual.retired"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto resident = [&] {
    const auto projection = kernel.get_operation_projection(operation.value().operation_id);
    const auto latest = kernel.get_latest_run_operations({ProgramRunId("run.1")});
    EXPECT_TRUE(projection.ok());
    EXPECT_EQ(latest.size(), 1U);
    EXPECT_EQ(latest.front().resident, projection.value().resident);
    return projection.value().resident;
  };

  // The automatic clean-terminal retirement leaves the active table, then
  // the claim release fails: the operation keeps its admission authority.
  journal.fail_next_cell_release = true;
  ASSERT_TRUE(wait_until([&] {
    const auto retired = kernel.reconcile_manual_operation_admission();
    if (retired.ok()) return false;
    EXPECT_EQ(retired.error().code, "operation_journal_test_failure");
    return true;
  }));
  EXPECT_EQ(kernel.get_operation_state(operation.value().operation_id).value().state,
            OperationStateKind::kCompleted);
  EXPECT_TRUE(kernel.get_resident_operations({ProgramRunId("run.1")}).empty());
  EXPECT_TRUE(resident());
  const auto held = kernel.GetAuthoringAdmission();
  EXPECT_FALSE(held.authoring_allowed);

  // Non-resident is published only with the release, so a generation read
  // after observing it is the one a following claim must name.
  ASSERT_TRUE(kernel.reconcile_manual_operation_admission().ok());
  EXPECT_FALSE(resident());
  const auto released = kernel.GetAuthoringAdmission();
  EXPECT_TRUE(released.authoring_allowed);
  EXPECT_GT(released.admission_generation.value(), held.admission_generation.value());
  const auto next = kernel.AcquireCellAdmissionClaim(
      {CellAdmissionClaimKindDTO::kAuthoringMutation,
       CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       RequestOwnerEpoch("authoring.owner"),
       "edit.after.manual",
       RequestId("edit.after.manual"),
       ContentDigest(test_digest('1')),
       released.admission_generation,
       {},
       {},
       {}});
  EXPECT_TRUE(next.ok()) << next.error().message;
}

#ifdef MANYFORGE_TEST_SQLITE_OPERATION_JOURNAL
class TemporaryOperationJournal final {
 public:
  TemporaryOperationJournal() {
    char path[] = "/tmp/manyforge-owner-journal-XXXXXX";
    const auto created = ::mkdtemp(path);
    if (created == nullptr) throw std::runtime_error("failed to create journal test root");
    directory = created;
  }
  ~TemporaryOperationJournal() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path directory;
};

TEST(ResourceAddressedExecutionKernelTest, LongServiceStopSurvivesRealJournalAndOwnerReplacement) {
  constexpr std::int64_t boundary = 9'007'199'254'740'991LL;
  for (const auto timestamp :
       std::array<std::int64_t, 5>{boundary, boundary + 1, boundary + 2, 105LL * 86400 * 1000000000,
                                   31LL * 366 * 86400 * 1000000000}) {
    SCOPED_TRACE(timestamp);
    TemporaryOperationJournal temporary;
    const auto database = temporary.directory / "operation.sqlite3";
    FakeClock clock;
    ManualScheduler scheduler;
    clock.set(timestamp);
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    std::optional<StopAcceptedDTO> accepted_stop;
    std::optional<StopCertificateDTO> terminal;
    {
      FakeParticipantEngine engine;
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ResourceAddressedExecutionKernel kernel(
          {registration(binding, engine)}, clock, scheduler, journal,
          {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
      const auto operation = kernel.execute(motion_operation());
      ASSERT_TRUE(operation.ok()) << operation.error().message;
      EXPECT_EQ(operation.value().accepted_at_ns, timestamp);
      EXPECT_EQ(engine.execute_calls, 1);
      clock.set(timestamp + 1);
      const auto stopped = kernel.stop(stop_request("request.stop", operation.value(), 1U));
      ASSERT_TRUE(stopped.ok()) << stopped.error().message;
      accepted_stop = stopped.value();
      EXPECT_EQ(accepted_stop->accepted_at_ns, timestamp + 1);
      ASSERT_TRUE(wait_until([&] {
        const auto status = kernel.get_stop_status(accepted_stop->stop_id);
        return status.ok() &&
               status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
      }));
      const auto status = kernel.get_stop_status(accepted_stop->stop_id);
      ASSERT_TRUE(status.ok());
      ASSERT_TRUE(status.value().stop_certificate.has_value());
      const auto certificate =
          kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
      ASSERT_TRUE(certificate.ok());
      terminal = certificate.value();
      EXPECT_EQ(engine.stop_calls.load(), 1);
      EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
    }
    // Reconstruct the real owner from unchanged persisted bytes, not a fake
    // journal recovery DTO. No prior motion or Stop is dispatched again.
    FakeParticipantEngine replacement_engine;
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ResourceAddressedExecutionKernel replacement(
        {registration(binding, replacement_engine)}, clock, scheduler, journal,
        {RequestOwnerEpoch("kernel.epoch.2"), CellId("cell.test"), 8U});
    const auto recovered = replacement.get_stop_certificate(terminal->stop_certificate_id);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    EXPECT_EQ(recovered.value(), *terminal);
    EXPECT_EQ(replacement_engine.execute_calls, 0);
    EXPECT_EQ(replacement_engine.stop_calls.load(), 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ManualOwnerReplacementUsesActualJournalAndOrphanSettlement) {
  for (const bool retired : {false, true}) {
    SCOPED_TRACE(retired);
    TemporaryOperationJournal temporary;
    const auto database = temporary.directory / "manual.sqlite3";
    FakeClock clock;
    ManualScheduler scheduler;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                  CellId("cell.test"), 8U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    {
      FakeParticipantEngine engine;
      auto direct = registration(binding, engine);
      direct.intrinsic_direct_control = true;
      SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
      ResourceAddressedExecutionKernel owner({direct}, clock, scheduler, journal, config);
      ASSERT_TRUE(ready_cell_admission(owner).ok());
      ASSERT_TRUE(owner.execute(motion_operation()).ok());
      if (retired) {
        engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
        ASSERT_TRUE(wait_until([&] {
          return owner.reconcile_manual_operation_admission().ok() &&
                 owner.GetAuthoringAdmission().authoring_allowed;
        }));
        ASSERT_TRUE(owner.GetAuthoringAdmission().authoring_allowed);
      }
      // With resident work the journal deliberately refuses clean close.
    }
    config.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.2");
    FakeParticipantEngine engine;
    auto direct = registration(binding, engine);
    direct.intrinsic_direct_control = true;
    SqliteOperationJournal journal(database, CellId("cell.test"), 8U);
    ResourceAddressedExecutionKernel successor({direct}, clock, scheduler, journal, config);
    ASSERT_TRUE(ready_cell_admission(successor).ok());
    if (!retired) {
      EXPECT_FALSE(successor.reconcile_manual_operation_admission().ok());
      EXPECT_FALSE(successor.GetAuthoringAdmission().authoring_allowed);
      const auto facts = orphan_reconciliation_facts(
          config.request_owner_epoch, config.cell_id,
          successor.resource_table_snapshot().operation_resource_generation, {binding},
          {binding.value().resources.front().resource_id}, clock.now());
      ASSERT_TRUE(successor.reconcile_orphaned_resources(facts).ok());
    }
    ASSERT_TRUE(successor.reconcile_manual_operation_admission().ok());
    EXPECT_TRUE(successor.GetAuthoringAdmission().authoring_allowed);
    auto next = motion_operation("manual.after.restart");
    next.request_context.request_owner_epoch = config.request_owner_epoch;
    ASSERT_TRUE(successor.execute(next).ok());
    EXPECT_EQ(engine.execute_calls, 1);  // no replay of predecessor motion
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    ASSERT_TRUE(wait_until([&] {
      return successor.reconcile_manual_operation_admission().ok() &&
             successor.GetAuthoringAdmission().authoring_allowed;
    }));
    EXPECT_TRUE(claim_program_execution(successor).ok());
  }
}

TEST(ResourceAddressedExecutionKernelTest, BootstrapInventoryCannotCertifyUnknownPredecessorRest) {
  // Unclosed epoch: no record, executing, already intervention-required,
  // or a held Program claim before the first operation exists.
  for (const int predecessor : {0, 1, 2, 3}) {
    for (const int selector : {0, 1, 2}) {
      SCOPED_TRACE(predecessor);
      SCOPED_TRACE(selector);
      TemporaryOperationJournal temporary;
      const auto original = temporary.directory / "original.sqlite3";
      const auto snapshot = temporary.directory / "unclosed.sqlite3";
      FakeClock clock;
      ManualScheduler scheduler;
      clock.set(100);
      const auto binding =
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
      FakeParticipantEngine engine;
      if (predecessor == 2) {
        engine.prepare_failure = ErrorDTO{"execution_rejected", "injected preparation failure", {}};
      }
      const auto qualified = registration(binding, engine);
      std::optional<OperationAcceptedDTO> accepted;
      {
        SqliteOperationJournal journal(original, CellId("cell.test"), 8U);
        ResourceAddressedExecutionKernelConfig original_config{RequestOwnerEpoch("kernel.epoch.1"),
                                                               CellId("cell.test"), 8U};
        original_config.manager_liveness_required_set = EmptyManagerLivenessSet();
        ResourceAddressedExecutionKernel owner({qualified}, clock, scheduler, journal,
                                               original_config);
        if (predecessor == 1 || predecessor == 2) {
          const auto result = owner.execute(motion_operation());
          ASSERT_TRUE(result.ok());
          accepted = result.value();
          ASSERT_EQ(engine.execute_calls, predecessor == 1 ? 1 : 0);
        } else if (predecessor == 3) {
          ASSERT_TRUE(owner
                          .UpdateCellAdmissionReadiness(CellAdmissionReadinessSnapshotDTO{
                              true,
                              ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
                                                    ProgramCycleStateDTO::kIdleAfterReset,
                                                    std::nullopt, std::nullopt, std::nullopt},
                              true, 1U, 1U, true, true, 0U, 0U, 0U})
                          .ok());
          const auto claim = owner.AcquireCellAdmissionClaim(CellAdmissionClaimIntentDTO{
              CellAdmissionClaimKindDTO::kProgramCycle,
              CellAdmissionOwnerRoleDTO::kProgramRunner,
              RequestOwnerEpoch("program.owner"),
              motion_operation().program_run_id.value(),
              RequestId("request.bootstrap.claim"),
              ContentDigest(test_digest('a')),
              owner.GetAuthoringAdmission().admission_generation,
              {},
              ProgramCycleAdmissionEvidenceDTO{ProgramRevision("program.1"),
                                               ProgramRevision("program.1")},
              std::nullopt});
          ASSERT_TRUE(claim.ok()) << claim.error().message;
        }
        // Transactional SQLite backup captures an unclosed real owner epoch,
        // including WAL contents, before orderly fixture teardown can close it.
        sqlite3 *source_raw = nullptr, *copy_raw = nullptr;
        ASSERT_EQ(sqlite3_open_v2(original.c_str(), &source_raw, SQLITE_OPEN_READONLY, nullptr),
                  SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> source(source_raw, sqlite3_close);
        ASSERT_EQ(sqlite3_open(snapshot.c_str(), &copy_raw), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> copy(copy_raw, sqlite3_close);
        auto* backup = sqlite3_backup_init(copy.get(), "main", source.get(), "main");
        ASSERT_NE(backup, nullptr);
        const auto copied = sqlite3_backup_step(backup, -1);
        EXPECT_EQ(sqlite3_backup_finish(backup), SQLITE_OK);
        ASSERT_EQ(copied, SQLITE_DONE);
      }
      ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.2"),
                                                    CellId("cell.test"), 8U};
      config.graph_revision = binding.value().graph_revision;
      config.empty_runtime_graph_revision = binding.value().graph_revision;
      config.empty_runtime_retained_bundle_id = binding.value().retained_bundle_id;
      config.cell_stop_deadline_budget =
          qualified.stop_readiness->value().resolved_stop_base_plan.deadline_budget;
      config.cell_stop_deadline_budget->phases.clear();
      for (const auto& resource : binding.value().resources)
        config.empty_runtime_resource_inventory.push_back(resource.resource_id);
      std::sort(config.empty_runtime_resource_inventory.begin(),
                config.empty_runtime_resource_inventory.end());
      SqliteOperationJournal journal(snapshot, CellId("cell.test"), 8U);
      ResourceAddressedExecutionKernel replacement({}, clock, scheduler, journal, config);
      const auto request = PublicRequestContextDTO{
          RequestId("request.bootstrap.stop"), config.request_owner_epoch, "test", std::nullopt};
      const auto stopped = [&] {
        if (selector == 0)
          return replacement.full_stop(FullStopIntentDTO{request, StopGeneration(0U), {}});
        StopIntentDTO intent = accepted.has_value()
                                   ? stop_request("request.bootstrap.stop", *accepted, 1U)
                                   : StopIntentDTO{request,
                                                   std::vector<StopOperationTargetDTO>{
                                                       {OperationId("operation.unknown"),
                                                        OperationRevision(1U), StopGeneration(0U)}},
                                                   std::nullopt,
                                                   {}};
        intent.request_context = request;
        if (selector == 2)
          intent.selector =
              std::vector<StopProgramRunTargetDTO>{{motion_operation().program_run_id}};
        return replacement.stop(intent);
      }();
      ASSERT_TRUE(stopped.ok()) << stopped.error().message;
      EXPECT_NE(stopped.value().resolution_disposition,
                StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
      ASSERT_TRUE(wait_until([&] {
        const auto current = replacement.get_stop_status(stopped.value().stop_id);
        return current.ok() && current.value().stop_certificate.has_value();
      }));
      const auto status = replacement.get_stop_status(stopped.value().stop_id);
      const auto certificate =
          replacement.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
      ASSERT_TRUE(certificate.ok());
      EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kFailedUncontained);
      EXPECT_EQ(certificate.value().scope_entries.size(), binding.value().resources.size());
      EXPECT_TRUE(replacement.get_stop_fault_latch().has_value());
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest, LongServiceJournalFailureCannotUndoNativeContainment) {
  TemporaryOperationJournal temporary;
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(31LL * 366 * 86400 * 1000000000);
  FakeParticipantEngine engine;
  std::atomic<bool> fail{false};
  SqliteOperationJournal journal(temporary.directory / "operation.sqlite3", CellId("cell.test"), 8U,
                                 [&] {
                                   if (fail.load())
                                     throw std::runtime_error("injected journal clock failure");
                                   return std::string("2026-09-06T12:00:00.000000001Z");
                                 });
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation());
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  fail.store(true);
  const auto stopped = kernel.stop(stop_request("request.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kFailed;
  }));
  EXPECT_EQ(engine.stop_calls.load(), 1);
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_TRUE(latch->persistence_degraded);
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kPersistenceDegraded);
  const auto attempted_motion = kernel.execute(motion_operation("request.after-audit-loss"));
  EXPECT_FALSE(attempted_motion.ok());
  fail.store(false);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramRunStopJoiningPlanningStopBesideHeldFailureStaysDurableAndUnlatched) {
  // Live incident 2026-10-02 (dual robot): robot B's IK failed before dispatch
  // and is held for intervention; the Program cancelled robot A's still-planning
  // sibling with an operation Stop, then stopped its run. The run Stop joins the
  // resident operation Stop. Its durable receipt must not conflict with the
  // creator's operation captures or recovery projection, so the ordinary Stop
  // neither degrades persistence nor latches the cell.
  TemporaryOperationJournal temporary;
  const auto database = temporary.directory / "operation.sqlite3";
  const auto epoch = RequestOwnerEpoch("kernel.epoch.1");
  FakeClock clock;
  ManualScheduler scheduler;
  std::optional<StopAcceptedDTO> created;
  {
    FakeParticipantEngine planning_engine;
    FakeParticipantEngine failed_engine;
    planning_engine.defer_prepare_completion = true;
    failed_engine.prepare_failure =
        ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)), "IK has no solution",
                 {{"reason", "native_ik_no_solution"}}};
    const auto planning_binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto failed_binding = second_same_type_occurrence(planning_binding);
    SqliteOperationJournal journal(database, CellId("cell.test"), 32U);
    ResourceAddressedExecutionKernel kernel({registration(planning_binding, planning_engine),
                                             registration(failed_binding, failed_engine)},
                                            clock, scheduler, journal,
                                            {epoch, CellId("cell.test"), 16U});

    auto failed_intent = motion_operation("request.joined.ik-failed");
    failed_intent.program_binding_id = failed_binding.value().program_binding_id;
    const auto failed = kernel.execute(failed_intent);
    ASSERT_TRUE(failed.ok()) << failed.error().message;
    const auto held = kernel.get_operation_state(failed.value().operation_id);
    ASSERT_TRUE(held.ok()) << held.error().message;
    ASSERT_EQ(held.value().state, OperationStateKind::kInterventionRequired);
    const auto planning = kernel.execute(motion_operation("request.joined.planning"));
    ASSERT_TRUE(planning.ok()) << planning.error().message;

    // Keep the operation Stop resident while the run Stop arrives.
    HeldSettlementCall cancellation;
    planning_engine.cancel_prepared_motion_hook = cancellation.Blocker();
    const auto operation_stop =
        kernel.stop(stop_request("request.joined.operation-stop", planning.value(), 1U));
    ASSERT_TRUE(operation_stop.ok()) << operation_stop.error().message;
    ASSERT_TRUE(cancellation.Entered());
    const auto run_stop = kernel.stop(program_run_stop_request("request.joined.run-stop", "run.1"));
    ASSERT_TRUE(run_stop.ok()) << run_stop.error().message;
    EXPECT_EQ(run_stop.value().stop_id, operation_stop.value().stop_id);
    EXPECT_EQ(run_stop.value().replay_status, RequestReplayStatusDTO::kAcceptedNew);
    ASSERT_EQ(run_stop.value().affected_operation_revisions.size(), 1U);
    EXPECT_EQ(run_stop.value().affected_operation_revisions.front().operation_id,
              planning.value().operation_id);
    // Observe durable receipts through an independent WAL reader: the owner's
    // journal API would contend with the projection worker it is observing.
    const auto acceptance_receipts = [&] {
      sqlite3* reader = nullptr;
      std::size_t count = 0U;
      if (sqlite3_open_v2(database.c_str(), &reader, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(reader,
                               "SELECT COUNT(*) FROM journal_records "
                               "WHERE record_kind='stop_acceptance_projected'",
                               -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
          count = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
        }
        sqlite3_finalize(statement);
      }
      sqlite3_close(reader);
      return count;
    };
    EXPECT_TRUE(wait_until([&] { return acceptance_receipts() == 2U; }));
    cancellation.Release();
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(operation_stop.value().stop_id);
      return status.ok() &&
             status.value().terminal_projection.state != StopDurableProjectionStateDTO::kPending;
    }));

    const auto status = kernel.get_stop_status(operation_stop.value().stop_id);
    ASSERT_TRUE(status.ok()) << status.error().message;
    EXPECT_EQ(status.value().state, StopStatusStateDTO::kSucceeded);
    EXPECT_EQ(status.value().acceptance_projection.state, StopDurableProjectionStateDTO::kRecorded)
        << (status.value().acceptance_projection.error.has_value()
                ? status.value().acceptance_projection.error->message
                : std::string{});
    EXPECT_EQ(status.value().terminal_projection.state, StopDurableProjectionStateDTO::kRecorded);
    EXPECT_FALSE(status.value().fault_latch.has_value());
    EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
    EXPECT_EQ(acceptance_receipts(), 2U);
    EXPECT_EQ(failed_engine.translate_stop_stage_calls.load(), 0);
    const auto retained = kernel.get_operation_state(failed.value().operation_id);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    EXPECT_EQ(retained.value().state, OperationStateKind::kInterventionRequired);
    const auto stopped = kernel.get_operation_state(planning.value().operation_id);
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    EXPECT_EQ(stopped.value().state, OperationStateKind::kStopped);
    created = operation_stop.value();
    journal.Close();
  }

  // The creator's recovery projection, not the joined receipt, is the durable
  // Stop truth a successor owner recovers.
  SqliteOperationJournal reopened(database, CellId("cell.test"), 32U);
  const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("kernel.epoch.2"), clock.now(),
                                                 16U, {ResourceId("arm")});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  ASSERT_EQ(recovery.value().recovered_stops.size(), 1U);
  EXPECT_EQ(recovery.value().recovered_stops.front().accepted.request_id, created->request_id);
  ASSERT_TRUE(recovery.value().recovered_stops.front().terminal_certificate.has_value());
  EXPECT_FALSE(recovery.value().recovered_stops.front().fault_latch.has_value());
}
#endif

TEST(ResourceAddressedExecutionKernelTest, StopDeadlineOverflowIsRejectedWithoutWrappedBudget) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  clock.set(std::numeric_limits<std::int64_t>::max() - 1);
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const FullStopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.overflow.stop"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      StopGeneration(0U), RequestAttributionDTO{}};
  const auto stopped = kernel.full_stop(intent);
  ASSERT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.error().details.at("reason"), "stop_deadline_overflow");
  EXPECT_EQ(engine.stop_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     IndependentlyQualifiedParticipantsMustShareOneStopEnvelope) {
  const auto first_binding =
      make_binding("arm.a", "arm.a", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding =
      make_binding("arm.b", "arm.b", "execution.trajectory.execute", CoordinateKind::kAngular);
  for (const bool change_outer : {false, true}) {
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine, second_engine;
    FakeOperationJournal journal;
    auto first = registration(first_binding, first_engine);
    auto second = registration(second_binding, second_engine);
    auto plan = second.stop_readiness->value().resolved_stop_base_plan;
    if (change_outer)
      ++plan.deadline_budget.outer_duration_ns;
    else
      --plan.deadline_budget.kernel_completion_reserve_ns;
    const auto sealed_plan = MakeResolvedStopBasePlan(std::move(plan));
    ASSERT_TRUE(sealed_plan.ok()) << sealed_plan.error().message;
    const auto domain = controller_domain_for(second_binding, sealed_plan.value());
    const auto ready = verify_runtime_participant_stop_readiness(
        RuntimeParticipantStopReadinessDTO{second_binding.value().controllable_component_id,
                                           ComponentLifecycleGeneration(1U), domain.value(),
                                           sealed_plan.value()},
        second_binding, domain);
    ASSERT_TRUE(ready.ok()) << ready.error().message;
    second.stop_readiness = ready.value();
    EXPECT_THROW((ResourceAddressedExecutionKernel(
                     {first, second}, clock, scheduler, journal,
                     {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U})),
                 std::invalid_argument);
    EXPECT_EQ(first_engine.stop_calls.load(), 0);
    EXPECT_EQ(second_engine.stop_calls.load(), 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest, ResolvesPortableMotionAndReservesExactResource) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_TRUE(result.ok()) << result.error().message << ": "
                           << (result.error().details.contains("reason")
                                   ? result.error().details.at("reason")
                                   : "");
  EXPECT_EQ(result.value().accepted_at_ns, 1234);
  EXPECT_EQ(engine.prepare_motion_calls, 1);
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(engine.execute_calls, 1);
  ASSERT_TRUE(engine.last_execute.has_value());
  EXPECT_EQ(engine.last_execute->binding.controllable_component_id.value(), "robot.1");
  EXPECT_EQ(engine.last_execute->binding.model_group_binding.compiled_group_name, "mf2_arm_group");
  EXPECT_EQ(compiled_frame_name(engine.last_execute->binding.base_frame), "mf2_base");
  EXPECT_EQ(compiled_frame_name(engine.last_execute->binding.tcp_frame), "mf2_tcp");
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.joint_names, std::vector<std::string>{"mf2_joint_1"});
  ASSERT_EQ(engine.last_execute->per_resource_dynamics_limits.size(), 1U);
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().resource_id.value(),
            "robot.joint_1");
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().compiled_joint_name,
            "mf2_joint_1");

  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_EQ(slots.front().resource_id.value(), "robot.joint_1");
  EXPECT_EQ(slots.front().state, ResourceSlotState::kDispatched);

  const auto state = kernel.get_operation_state(result.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kPrepared);
  EXPECT_EQ(state.value().participants.front().command_target_id.value(), "arm");
}

TEST(ResourceAddressedExecutionKernelTest,
     PreparedMotionDoesNotDispatchUntilTheExactPreparationCompletes) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepare_completion = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.motion.held-prepare"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message << ": "
                             << accepted.error().details.at("reason");
  EXPECT_EQ(engine.prepare_motion_calls, 1);
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  const auto preparing = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(preparing.ok()) << preparing.error().message;
  EXPECT_EQ(preparing.value().state, OperationStateKind::kPlanning);
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kReserved);

  const auto completed = engine.complete_preparation();
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  ASSERT_TRUE(wait_until([&] { return engine.dispatch_prepared_motion_calls == 1; }));
  EXPECT_EQ(engine.execute_calls, 1);
  const auto prepared = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().state, OperationStateKind::kPrepared);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreparationFailureIsDurableAndCannotDispatchPhysicalMotion) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "injected preparation failure",
                                    {{"reason", "injected_preparation_failure"}}};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.motion.rejected-prepare"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(engine.prepare_motion_calls, 1);
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  const auto failed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  EXPECT_EQ(failed.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(failed.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(failed.value().participants.front().terminal_error->details.at("reason"),
            "injected_preparation_failure");
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
}

TEST(ResourceAddressedExecutionKernelTest,
     RecoveryProjectionAndAcceptedActionUseExactOperationAndReplayFences) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "injected preparation failure",
                                    {{"reason", "injected_preparation_failure"}}};
  engine.recovery_assessment.assessment_revision = 7U;
  engine.recovery_assessment.control_command_epoch = 11U;
  engine.recovery_assessment.execution_id = 41U;
  engine.recovery_assessment.status = RecoveryAssessmentStatus::kActionRequired;
  engine.recovery_assessment.allowed_actions = {RecoveryAction::kAcknowledgeAndContinue};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.recovery.accept"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  ASSERT_EQ(operation.value().state, OperationStateKind::kInterventionRequired);

  const auto projection = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  EXPECT_EQ(projection.value().operation_revision, operation.value().operation_revision);
  EXPECT_EQ(projection.value().assessment.assessment_revision, 7U);
  ASSERT_EQ(projection.value().participants.size(), 1U);
  EXPECT_EQ(projection.value().participants.front().controllable_component_id,
            ControllableComponentId("robot.1"));
  EXPECT_EQ(projection.value().participants.front().command_target_id, CommandTargetId("arm"));
  EXPECT_EQ(projection.value().participants.front().resource_ids,
            std::vector<ResourceId>{ResourceId("robot.joint_1")});

  const KernelRecoveryActionRequestDTO request{"request.recovery.action",
                                               RecoveryAction::kAcknowledgeAndContinue,
                                               "operator.test",
                                               projection.value().assessment.assessment_revision,
                                               projection.value().assessment.control_command_epoch,
                                               projection.value().assessment.execution_id,
                                               projection.value().assessment.latch_id,
                                               projection.value().assessment.stop_certificate_id,
                                               projection.value().assessment.stop_owner_generation,
                                               "continue after inspection"};
  auto resulting_assessment = projection.value().assessment;
  resulting_assessment.assessment_revision = 8U;
  resulting_assessment.status = RecoveryAssessmentStatus::kUnavailable;
  engine.next_recovery_result = KernelRecoveryActionResultDTO{
      request.request_id, request.action, true, resulting_assessment, std::nullopt};

  const auto recovered = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(recovered.value().accepted);
  EXPECT_EQ(engine.recovery_action_calls, 1);
  const auto released_resources = kernel.resource_snapshot();
  ASSERT_EQ(released_resources.size(), 1U);
  EXPECT_EQ(released_resources.front().state, ResourceSlotState::kFree);
  EXPECT_FALSE(released_resources.front().owner.has_value());
  const auto historical = kernel.get_operation_projection(accepted.value().operation_id);
  ASSERT_TRUE(historical.ok()) << historical.error().message;
  EXPECT_FALSE(historical.value().resident);
  EXPECT_EQ(historical.value().operation.state, OperationStateKind::kInterventionRequired);

  const auto replay = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_TRUE(replay.value().accepted);
  EXPECT_EQ(engine.recovery_action_calls, 1);

  auto conflicting = request;
  conflicting.action = RecoveryAction::kRetryAfterSceneRepair;
  const auto conflict = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, conflicting);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"), "request_id_conflict");
}

TEST(ResourceAddressedExecutionKernelTest,
     RejectedRecoveryIsReplayableAndLeavesTheOperationResident) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.recovery.reject"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const KernelRecoveryActionRequestDTO request{"request.recovery.rejected",
                                               RecoveryAction::kRetryMotionFromCurrentState,
                                               "operator.test",
                                               1U,
                                               1U,
                                               41U,
                                               "",
                                               "",
                                               0U,
                                               "not currently admissible"};
  engine.next_recovery_result =
      KernelRecoveryActionResultDTO{request.request_id,
                                    request.action,
                                    false,
                                    {},
                                    ErrorDTO{"recovery_rejected",
                                             "recovery is not currently admissible",
                                             {{"reason", "recovery_not_admissible"}}}};

  const auto rejected = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_TRUE(rejected.ok()) << rejected.error().message;
  EXPECT_FALSE(rejected.value().accepted);
  const auto replay = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_FALSE(replay.value().accepted);
  EXPECT_EQ(engine.recovery_action_calls, 1);
  const auto resident = kernel.get_operation_projection(accepted.value().operation_id);
  ASSERT_TRUE(resident.ok()) << resident.error().message;
  EXPECT_TRUE(resident.value().resident);
}

TEST(ResourceAddressedExecutionKernelTest, RecoveryAssessmentFailsClosedWhenTheParticipantThrows) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.throw_during_recovery_assessment = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation("request.recovery.assessment-throw"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  const auto assessment = kernel.get_operation_recovery_assessment(accepted.value().operation_id);

  ASSERT_FALSE(assessment.ok());
  EXPECT_EQ(assessment.error().details.at("reason"), "operation_recovery_unavailable");
}

TEST(ResourceAddressedExecutionKernelTest,
     RecoveryAssessmentRejectsAnOperationChangedDuringTheOwnerRead) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation("request.recovery.assessment-race"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.recovery_assessment_hook = [&] {
    const auto stopped =
        kernel.stop(stop_request("request.recovery.assessment-race.stop", accepted.value(),
                                 operation.value().operation_revision.value()));
    EXPECT_TRUE(stopped.ok()) << stopped.error().message;
  };

  const auto assessment = kernel.get_operation_recovery_assessment(accepted.value().operation_id);

  ASSERT_FALSE(assessment.ok());
  EXPECT_EQ(assessment.error().details.at("reason"), "operation_recovery_stale");
}

TEST(ResourceAddressedExecutionKernelTest,
     ThrowingRecoveryEffectIsUnresolvedAndClosesMotionAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.throw_during_recovery = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation("request.recovery.action-throw"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const KernelRecoveryActionRequestDTO request{"request.recovery.action-throw.command",
                                               RecoveryAction::kRetryMotionFromCurrentState,
                                               "operator.test",
                                               1U,
                                               1U,
                                               41U,
                                               "",
                                               "",
                                               0U,
                                               "injected participant failure"};

  const auto recovered = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);

  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().details.at("reason"), "operation_recovery_outcome_unresolved");
  const auto replay = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().details.at("reason"), "operation_recovery_outcome_unresolved");
  EXPECT_EQ(engine.recovery_action_calls, 1);
  const auto blocked = kernel.execute(motion_operation("request.after-recovery-throw"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");
}

TEST(ResourceAddressedExecutionKernelTest,
     ConcurrentStopMakesTheParticipantRecoveryOutcomeUnresolved) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.recovery.stop-race"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const KernelRecoveryActionRequestDTO request{"request.recovery.raced",
                                               RecoveryAction::kRetryMotionFromCurrentState,
                                               "operator.test",
                                               1U,
                                               1U,
                                               41U,
                                               "",
                                               "",
                                               0U,
                                               "race with Stop"};
  engine.next_recovery_result =
      KernelRecoveryActionResultDTO{request.request_id, request.action, true, {}, std::nullopt};
  engine.recovery_action_hook = [&] {
    const auto stopped = kernel.stop(stop_request("request.stop.during-recovery", accepted.value(),
                                                  operation.value().operation_revision.value()));
    EXPECT_TRUE(stopped.ok()) << stopped.error().message;
  };

  const auto recovered = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_FALSE(recovered.ok());
  EXPECT_EQ(recovered.error().details.at("reason"), "operation_recovery_outcome_unresolved");
  const auto replay = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, request);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().details.at("reason"), "operation_recovery_outcome_unresolved");
  EXPECT_EQ(engine.recovery_action_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopCancelsHeldPreparationWithoutDispatchingAStopStageOrMotion) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepare_completion = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.motion.cancel-prepare"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.stop.cancel-prepare", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] { return engine.cancel_prepared_motion_calls == 1; }));
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
  EXPECT_EQ(certificate.value().scope_entries.front().dispatch_state,
            StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
  const auto& facts =
      std::get<StopParticipantScopeFactsDTO>(certificate.value().scope_entries.front().facts);
  EXPECT_EQ(facts.terminal_settlement,
            StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled);
  EXPECT_TRUE(certificate.value().attempted_stage_results.empty());
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  EXPECT_EQ(operation.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
  const auto recovery = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  EXPECT_EQ(recovery.value().assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(recovery.value().assessment.status, RecoveryAssessmentStatus::kRetryReady);

  const auto reset =
      kernel.reset(control_request("request.reset.cancelled-prepare", accepted.value(),
                                   operation.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    return engine.certified_reset_calls.load() == 1 &&
           kernel.resource_snapshot().front().state == ResourceSlotState::kFree;
  }));
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);

  engine.defer_prepare_completion = false;
  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  const auto next = kernel.execute(motion_operation("request.after-cancelled-prepare-reset"));
  ASSERT_TRUE(next.ok()) << next.error().message;
  EXPECT_TRUE(wait_until([&] { return engine.execute_calls.load() == 1; }));
}

TEST(ResourceAddressedExecutionKernelTest,
     StopReconcilesPreparationTerminalPublishedBeforeCancellation) {
  for (const auto terminal_state : {ExecutionState::kStopped, ExecutionState::kFailed}) {
    SCOPED_TRACE(to_string(terminal_state));
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    engine.defer_prepare_completion = true;
    // The worker may publish its terminal as soon as acceptance fences
    // dispatch, before the aggregate reducer has initialized participant work.
    // No repeat event is available from idempotent CancelPreparedMotion.
    engine.status_at_stop_fence = ExecutionStatus{};
    engine.status_at_stop_fence->state = terminal_state;
    engine.status_at_stop_fence->execution_id = engine.next_execution_id;
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto accepted = kernel.execute(motion_operation("request.early-terminal"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto stopped =
        kernel.stop(stop_request("request.early-terminal.stop", accepted.value(), 1U));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    const auto expected = terminal_state == ExecutionState::kStopped
                              ? StopStatusStateDTO::kSucceeded
                              : StopStatusStateDTO::kPartial;
    const auto settled = wait_until(
        [&] {
          const auto result = kernel.get_stop_status(stopped.value().stop_id);
          return result.ok() && result.value().state == expected;
        },
        std::chrono::seconds(1));
    EXPECT_TRUE(settled);
    EXPECT_EQ(engine.cancel_prepared_motion_calls, 1);
    EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
    EXPECT_EQ(engine.participant_stop_settlement_calls,
              terminal_state == ExecutionState::kStopped ? 1 : 0);
    if (!settled) {
      // Keep the failure, but release the pending fixture so independent
      // cases still run instead of aborting during Kernel destruction.
      engine.emit_transition(terminal_state, terminal_state);
      EXPECT_TRUE(wait_until([&] {
        return kernel.get_stop_status(stopped.value().stop_id).value().state == expected;
      }));
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     CancellationAcknowledgementOrAnotherExecutionCannotProveSettlement) {
  for (const bool unrelated_terminal : {false, true}) {
    SCOPED_TRACE(unrelated_terminal);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    engine.defer_prepare_completion = true;
    engine.prepared_cancel_terminal_state = ExecutionState::kValidating;
    engine.status_at_stop_fence = ExecutionStatus{};
    engine.status_at_stop_fence->state =
        unrelated_terminal ? ExecutionState::kStopped : ExecutionState::kValidating;
    engine.status_at_stop_fence->execution_id = engine.next_execution_id + unrelated_terminal;
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto accepted = kernel.execute(motion_operation("request.unsettled-prepare"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto reads = engine.status_read_calls.load();
    const auto stopped =
        kernel.stop(stop_request("request.unsettled-prepare.stop", accepted.value(), 1U));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    EXPECT_TRUE(wait_until([&] { return engine.status_read_calls >= reads + 2; }));
    EXPECT_FALSE(
        kernel.get_stop_status(stopped.value().stop_id).value().stop_certificate.has_value());
    EXPECT_EQ(engine.participant_stop_settlement_calls, 0);
    engine.emit_transition(ExecutionState::kValidating, ExecutionState::kStopped);
    EXPECT_TRUE(wait_until([&] {
      return kernel.get_stop_status(stopped.value().stop_id).value().state ==
             StopStatusStateDTO::kSucceeded;
    }));
    EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     PreparedCancellationFailureOrDeadlinePublishesUnsettledWithoutPhysicalStop) {
  for (const bool fails : {false, true}) {
    SCOPED_TRACE(fails);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    engine.defer_prepare_completion = true;
    engine.prepared_cancel_terminal_state = ExecutionState::kValidating;
    if (fails)
      engine.cancel_prepared_motion_hook = [] { throw std::runtime_error("cancel failed"); };
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto operation = kernel.execute(motion_operation("request.cancel-deadline"));
    ASSERT_TRUE(operation.ok());
    const auto stopped =
        kernel.stop(stop_request("request.cancel-deadline.stop", operation.value(), 1U));
    ASSERT_TRUE(stopped.ok());
    EXPECT_TRUE(wait_until([&] { return engine.cancel_prepared_motion_calls == 1; }));
    if (!fails) {
      clock.set(stopped.value().accepted_at_ns + stopped.value().remaining_outer_duration_ns);
      // A nonterminal event wakes the owner at its existing deadline; it is
      // not evidence of settlement and introduces no wall-clock sleep.
      engine.emit_transition(ExecutionState::kValidating, ExecutionState::kValidating);
    }
    const bool finished = wait_until(
        [&] {
          return kernel.get_stop_status(stopped.value().stop_id)
              .value()
              .stop_certificate.has_value();
        },
        std::chrono::milliseconds(500));
    EXPECT_TRUE(finished);
    if (finished) {
      const auto status = kernel.get_stop_status(stopped.value().stop_id).value();
      EXPECT_EQ(status.state, StopStatusStateDTO::kPartial);
      const auto certificate =
          kernel.get_stop_certificate(status.stop_certificate->stop_certificate_id).value();
      EXPECT_EQ(certificate.unsettled.size(), 1U);
      EXPECT_TRUE(certificate.attempted_stage_results.empty());
      ASSERT_TRUE(certificate.terminal_error.has_value());
      EXPECT_EQ(certificate.terminal_error->details.at("reason"),
                fails ? "prepared_motion_cancel_threw" : "prepared_cancellation_deadline_expired");
      EXPECT_TRUE(kernel.get_stop_fault_latch().has_value());
      EXPECT_EQ(kernel.get_operation_state(operation.value().operation_id).value().state,
                OperationStateKind::kInterventionRequired);
    }
    // Drain failure fixtures too, without turning an earlier EXPECT into a pass.
    engine.emit_transition(ExecutionState::kValidating, ExecutionState::kStopped);
    EXPECT_TRUE(wait_until([&] {
      return kernel.get_stop_status(stopped.value().stop_id).value().stop_certificate.has_value();
    }));
    if (finished) {
      EXPECT_EQ(kernel.get_stop_status(stopped.value().stop_id).value().state,
                StopStatusStateDTO::kPartial);
    }
    EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     StopDeadlineRetainsAndCancelsLatePreparationWithoutDispatch) {
  for (const bool coordinated : {false, true}) {
    SCOPED_TRACE(coordinated);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine, other;
    engine.defer_prepare_completion = true;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    auto registrations = std::vector{registration(binding, engine)};
    if (coordinated)
      registrations.push_back(registration(coordinated_occurrence(binding, "2"), other));
    ResourceAddressedExecutionKernel kernel(
        std::move(registrations), clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    HeldSettlementCall prepare;
    engine.prepare_motion_hook = prepare.Blocker();
    auto executing = std::async(std::launch::async, [&] {
      return kernel.execute(coordinated
                                ? coordinated_motion_operation("request.late-after-deadline")
                                : motion_operation("request.late-after-deadline"));
    });
    EXPECT_TRUE(prepare.Entered());
    const OperationAcceptedDTO expected{RequestId("request.late-after-deadline"),
                                        RequestReplayStatusDTO::kAcceptedNew,
                                        OperationId("operation.kernel.epoch.1.1"),
                                        OperationRevision(1U),
                                        {},
                                        0};
    const auto stopped =
        kernel.stop(stop_request("request.late-after-deadline.stop", expected, 1U));
    EXPECT_TRUE(stopped.ok());
    if (!stopped.ok()) {
      prepare.Release();
      executing.get();
      continue;
    }
    clock.set(stopped.value().accepted_at_ns + stopped.value().remaining_outer_duration_ns);
    const bool finished = wait_until(
        [&] {
          return kernel.get_stop_status(stopped.value().stop_id)
              .value()
              .stop_certificate.has_value();
        },
        std::chrono::seconds(6));
    EXPECT_TRUE(finished);
    if (finished) {
      EXPECT_EQ(kernel.get_stop_status(stopped.value().stop_id).value().state,
                StopStatusStateDTO::kPartial);
      EXPECT_TRUE(kernel.get_stop_fault_latch().has_value());
      const auto state = kernel.get_operation_state(expected.operation_id).value();
      const auto reset = kernel.reset(control_request("request.late-after-deadline.reset", expected,
                                                      state.operation_revision.value()));
      EXPECT_FALSE(reset.ok());
      if (!reset.ok()) {
        EXPECT_EQ(reset.error().details.at("reason"), "operation_effect_in_flight");
      }
    }
    prepare.Release();
    EXPECT_TRUE(executing.get().ok());
    engine.emit_transition(ExecutionState::kValidating, ExecutionState::kValidating);
    EXPECT_TRUE(wait_until([&] { return engine.cancel_prepared_motion_calls == 1; }));
    EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.execute_calls, 0);
    EXPECT_EQ(other.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
    if (finished) {
      EXPECT_EQ(kernel.get_stop_status(stopped.value().stop_id).value().state,
                StopStatusStateDTO::kPartial);
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     FailedPreparedCancellationCannotPublishANominalAggregateCertificate) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepare_completion = true;
  engine.prepared_cancel_terminal_state = ExecutionState::kFailed;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(motion_operation("request.motion.cancel-prepare-fails"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.stop.cancel-prepare-fails", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kPartial;
  }));

  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
  EXPECT_EQ(certificate.value().scope_entries.front().dispatch_state,
            StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
  const auto& facts =
      std::get<StopParticipantScopeFactsDTO>(certificate.value().scope_entries.front().facts);
  EXPECT_EQ(facts.terminal_settlement,
            StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled);
  ASSERT_EQ(certificate.value().unsettled.size(), 1U);
  ASSERT_TRUE(certificate.value().terminal_error.has_value());
  EXPECT_EQ(certificate.value().terminal_error->details.at("reason"),
            "prepared_cancel_terminal_failed");
  EXPECT_TRUE(kernel.get_stop_fault_latch().has_value());
  const auto next = kernel.execute(motion_operation("request.after-cancel-prepare-failure"));
  ASSERT_FALSE(next.ok());
  EXPECT_EQ(next.error().details.at("reason"), "motion_admission_closed");
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedMotionPreparesEveryParticipantBeforeDeterministicDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  std::vector<std::string> timeline;
  first_engine.timeline = &timeline;
  second_engine.timeline = &timeline;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.success"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_EQ(accepted.value().participant_assignments.size(), 2U);
  EXPECT_EQ(first_engine.prepare_motion_calls, 1);
  EXPECT_EQ(second_engine.prepare_motion_calls, 1);
  ASSERT_TRUE(first_engine.last_prepared_request);
  ASSERT_TRUE(second_engine.last_prepared_request);
  EXPECT_NE(first_engine.last_prepared_request->common_timing, nullptr);
  EXPECT_EQ(first_engine.last_prepared_request->common_timing,
            second_engine.last_prepared_request->common_timing);
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(timeline,
            (std::vector<std::string>{"engine:prepare_motion", "engine:prepare_motion",
                                      "engine:dispatch_prepared_motion", "engine:execute",
                                      "engine:set_runtime_scale", "engine:dispatch_prepared_motion",
                                      "engine:execute", "engine:set_runtime_scale"}));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kPrepared);
  ASSERT_EQ(state.value().participants.size(), 2U);
  const auto slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kDispatched;
  }));
}

TEST(ResourceAddressedExecutionKernelTest,
     MotionAdmissionCapsEachExactTargetAndPreservesRequestedCeilings) {
  for (const bool coordinated : {false, true}) {
    SCOPED_TRACE(coordinated);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine, second_engine;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    auto first_registration = registration(first, first_engine);
    auto second_registration = registration(coordinated_occurrence(first, "2"), second_engine);
    first_registration.deployment_maximum_tcp_linear_speed_mps = 0.15;
    second_registration.deployment_maximum_tcp_linear_speed_mps = 0.25;
    ResourceAddressedExecutionKernel kernel(
        {first_registration, second_registration}, clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    auto intent = coordinated ? coordinated_motion_operation("speed.coordinated")
                              : motion_operation("speed.single");
    for (auto& participant : intent.participants) {
      std::get<MotionParticipantIntentDTO>(participant.payload)
          .speed_profile.max_tcp_linear_speed_mps = 0.25;
    }
    const auto accepted = kernel.execute(intent);
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(first_engine.last_prepared_request);
    EXPECT_EQ(first_engine.last_prepared_request->execution_request.speed_profile
                  .max_tcp_linear_speed_mps, 0.15);
    if (coordinated) {
      ASSERT_TRUE(second_engine.last_prepared_request);
      EXPECT_EQ(second_engine.last_prepared_request->execution_request.speed_profile
                    .max_tcp_linear_speed_mps, 0.25);
    }
    EXPECT_EQ(std::get<MotionParticipantIntentDTO>(intent.participants.front().payload)
                  .speed_profile.max_tcp_linear_speed_mps, 0.25);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedRuntimeScalePublishesOneTargetWithoutPerParticipantEffects) {
  for (const bool delay_acceptance : {false, true}) {
    SCOPED_TRACE(delay_acceptance);
    FakeClock clock;
    clock.set(1);
    FakeOperationJournal delayed_journal;
    IOperationJournal& journal =
        delay_acceptance ? static_cast<IOperationJournal&>(delayed_journal) : default_journal;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine, second_engine;
    std::vector<std::string> timeline;
    first_engine.timeline = second_engine.timeline = &timeline;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine),
         registration(coordinated_occurrence(first, "2"), second_engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto accepted = kernel.execute(coordinated_motion_operation("scale.group.accept"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto timing = first_engine.last_prepared_request->common_timing;
    ASSERT_TRUE(timing);
    const auto first_id =
        first_engine.last_prepared_request->execution_request.binding.participant_id;
    const auto second_id =
        second_engine.last_prepared_request->execution_request.binding.participant_id;
    const TrajectoryRuntimeScaleBoundsDTO bounds{4.0, 16.0, 1.0, 1.0};
    auto joining = std::async(std::launch::async,
                              [&] { return timing->arrive_and_wait(first_id, bounds, {}); });
    ASSERT_TRUE(timing->arrive_and_wait(second_id, bounds, {}));
    ASSERT_TRUE(joining.get());
    const auto shared = timing->execution_clock();
    ASSERT_TRUE(shared);
    auto arming = std::async(std::launch::async,
                             [&] { return shared->start_and_wait(first_id, clock, 1.0, {}); });
    ASSERT_TRUE(shared->start_and_wait(second_id, clock, 1.0, {}));
    ASSERT_TRUE(arming.get());
    first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    first_engine.runtime_scale.active = second_engine.runtime_scale.active = true;
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    const auto before = timeline;
    delayed_journal.record_hook = [&](const std::string_view kind) {
      if (kind == "runtime_scale_accepted") clock.advance(1'000'000'000);
    };
    const auto request = runtime_scale_request("scale.shared.1", accepted.value(), 1U, 0U, 0.25);
    const auto scaled = kernel.set_runtime_scale(request);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message;
    EXPECT_EQ(timeline, before);
    ASSERT_TRUE(shared->target());
    if (delay_acceptance) {
      EXPECT_DOUBLE_EQ(shared->target()->scale, 1.0);
      EXPECT_FALSE(shared->target()->command);
      const auto failed = kernel.get_runtime_scale(accepted.value().operation_id);
      ASSERT_TRUE(failed.ok());
      EXPECT_EQ(failed.value().state, RuntimeScaleApplicationState::kFailed);
      for (const auto& row : failed.value().participants) {
        ASSERT_TRUE(row.error);
        EXPECT_EQ(row.error->details.at("reason"), "coordinated_runtime_scale_not_ready");
      }
      continue;
    }
    EXPECT_DOUBLE_EQ(shared->target()->scale, 0.25);
    ASSERT_TRUE(shared->target()->command);
    EXPECT_EQ(shared->target()->command->generation, RuntimeScaleGeneration(1U));
    EXPECT_EQ(shared->target()->command->operation_id, accepted.value().operation_id);
    const auto status = kernel.get_runtime_scale(accepted.value().operation_id);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kPending);
    for (const auto& participant : status.value().participants)
      EXPECT_EQ(participant.accepted_generation, RuntimeScaleGeneration(1U));
    EXPECT_TRUE(kernel.set_runtime_scale(request).ok());
    EXPECT_EQ(timeline, before);
    EXPECT_FALSE(kernel
                     .set_runtime_scale(runtime_scale_request("scale.shared.stale",
                                                              accepted.value(), 1U, 0U, 0.75))
                     .ok());
    EXPECT_DOUBLE_EQ(shared->target()->scale, 0.25);
    auto group_request = runtime_scale_control_request("scale.shared.group", 0U, 0.5);
    group_request.selector = runtime_scale_group_selector();
    const auto group_scaled = kernel.set_runtime_scale_control(group_request);
    ASSERT_TRUE(group_scaled.ok()) << group_scaled.error().message;
    EXPECT_EQ(group_scaled.value().control_generation, RuntimeScaleControlGeneration(1U));
    EXPECT_EQ(group_scaled.value().active_operation_scale_generation, RuntimeScaleGeneration(2U));
    EXPECT_EQ(timeline, before);
    EXPECT_DOUBLE_EQ(shared->target()->scale, 0.5);
    ASSERT_TRUE(kernel.set_runtime_scale_control(group_request).ok());
    EXPECT_EQ(timeline, before);
    auto group_alias = group_request;
    group_alias.request_context.request_id = RequestId("scale.shared.group.stale-alias");
    std::reverse(group_alias.selector.group_targets.begin(),
                 group_alias.selector.group_targets.end());
    EXPECT_FALSE(kernel.set_runtime_scale_control(group_alias).ok());
    EXPECT_DOUBLE_EQ(
        kernel.get_runtime_scale_control(runtime_scale_control_selector()).value().requested_scale,
        0.5);
    second_engine.last_runtime_scale_command->requested_runtime_scale_generation =
        RuntimeScaleGeneration(2U);
    second_engine.last_runtime_scale_command->requested_scale = 0.5;
    second_engine.runtime_scale.error = ErrorDTO{"peer_failed", "peer failed", {}};
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    // A retained failed peer cannot be hidden by accepting another shared target.
    EXPECT_FALSE(kernel
                     .set_runtime_scale(runtime_scale_request("scale.shared.failed",
                                                              accepted.value(), 1U, 2U, 0.75))
                     .ok());
    EXPECT_DOUBLE_EQ(shared->target()->scale, 0.5);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedRuntimeScaleStatusTracksEachParticipantAndRetainsPeerFailure) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto idle_control = kernel.present_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(idle_control.ok());
  EXPECT_EQ(idle_control.value().selector, runtime_scale_control_selector());
  const auto accepted = kernel.execute(coordinated_motion_operation("scale.group.status"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  auto status = kernel.get_runtime_scale(accepted.value().operation_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_EQ(status.value().participants.size(), 2U);
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kPending);
  const auto member_control = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(member_control.ok());
  EXPECT_TRUE(member_control.value().frozen);
  ASSERT_TRUE(member_control.value().reason);
  EXPECT_EQ(member_control.value().reason->details.at("reason"),
            "runtime_scale_group_control_required");
  const auto presented = kernel.present_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(presented.ok());
  EXPECT_EQ(presented.value().selector, runtime_scale_group_selector());
  EXPECT_EQ(presented.value().group_members.size(), 2U);
  EXPECT_EQ(presented.value().active_operation_id, accepted.value().operation_id);
  EXPECT_TRUE(presented.value().frozen);  // Group readiness is still authoritative.
  const auto explicit_group = kernel.get_runtime_scale_control(runtime_scale_group_selector());
  ASSERT_TRUE(explicit_group.ok());
  EXPECT_EQ(presented.value().reason, explicit_group.value().reason);
  const auto member_update = kernel.set_runtime_scale_control(
      runtime_scale_control_request("scale.member.rejected", 0U, 0.5));
  EXPECT_FALSE(member_update.ok());
  EXPECT_EQ(
      kernel.get_runtime_scale_control(runtime_scale_control_selector()).value().requested_scale,
      1.0);
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  first_engine.runtime_scale.active = true;
  second_engine.runtime_scale.active = false;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  status = kernel.get_runtime_scale(accepted.value().operation_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value().participants[0].state, RuntimeScaleApplicationState::kApplied);
  EXPECT_EQ(status.value().participants[1].state, RuntimeScaleApplicationState::kPending);
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kPending);
  second_engine.runtime_scale.active = true;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(kernel.get_runtime_scale(accepted.value().operation_id).value().state,
            RuntimeScaleApplicationState::kApplied);

  second_engine.runtime_scale.applied_scale = 0.5;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  status = kernel.get_runtime_scale(accepted.value().operation_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kPending);
  EXPECT_EQ(status.value().requested_scale, 1.0);
  EXPECT_EQ(status.value().effective_scale, 0.5);
  second_engine.runtime_scale.applied_scale = 1.0;

  second_engine.runtime_scale.error = ErrorDTO{"peer_scale_failed", "second engine failed", {}};
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  status = kernel.get_runtime_scale(accepted.value().operation_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kFailed);
  EXPECT_FALSE(status.value().participants[0].error);
  ASSERT_TRUE(status.value().participants[1].error);
  EXPECT_EQ(status.value().participants[1].error->code, "peer_scale_failed");
  // A subsequent healthy first-participant sample cannot hide its peer's failure.
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(kernel.get_runtime_scale(accepted.value().operation_id).value().state,
            RuntimeScaleApplicationState::kFailed);

  second_engine.runtime_scale.error.reset();
  second_engine.last_runtime_scale_command.reset();
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  status = kernel.get_runtime_scale(accepted.value().operation_id);
  ASSERT_TRUE(status.ok());
  EXPECT_FALSE(status.value().participants[0].error);
  ASSERT_TRUE(status.value().participants[1].error);
  EXPECT_EQ(status.value().participants[1].error->details.at("reason"),
            "runtime_scale_status_not_initialized");
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedTimeLawsCannotDispatchWithoutCommonPreparation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  first_engine.prepare_bounded_time_law = second_engine.prepare_bounded_time_law = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto accepted = kernel.execute(coordinated_motion_operation("request.common-time-law"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok());
  ASSERT_FALSE(state.value().participants.empty());
  ASSERT_TRUE(state.value().participants.front().terminal_error);
  EXPECT_EQ(state.value().participants.front().terminal_error->details.at("reason"),
            "coordinated_time_law_preparation_not_qualified");
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedMotionHoldsEveryDispatchUntilTheLastPreparationCompletes) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  second_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.held"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  const auto reserved_slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(reserved_slots.begin(), reserved_slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kReserved;
  }));

  ASSERT_TRUE(second_engine.complete_preparation().ok());
  ASSERT_TRUE(wait_until([&] {
    return first_engine.dispatch_prepared_motion_calls == 1 &&
           second_engine.dispatch_prepared_motion_calls == 1;
  }));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kPrepared);
}

void verify_prepared_common_clock_dispatch(const bool matching_bounds,
                                           const double common_multiplier = 1.0) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  first_engine.prepare_bounded_time_law = second_engine.prepare_bounded_time_law = true;
  first_engine.defer_prepare_completion = second_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto accepted = kernel.execute(coordinated_motion_operation("request.common-clock"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto common = first_engine.last_prepared_request->common_timing;
  ASSERT_NE(common, nullptr);
  ASSERT_EQ(common, second_engine.last_prepared_request->common_timing);
  const TrajectoryRuntimeScaleBoundsDTO bounds{matching_bounds ? 2.0 : 1.0, 8.0, 1.0,
                                               common_multiplier};
  auto joined = std::async(std::launch::async, [&] {
    return common->arrive_and_wait(
        first_engine.last_prepared_request->execution_request.binding.participant_id, bounds, {});
  });
  const auto other = common->arrive_and_wait(
      second_engine.last_prepared_request->execution_request.binding.participant_id, bounds, {});
  ASSERT_TRUE(joined.get());
  ASSERT_TRUE(other);
  ASSERT_TRUE(first_engine.complete_preparation().ok());
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  if (matching_bounds && common_multiplier == 1.0) {
    ASSERT_TRUE(second_engine.complete_preparation().ok());
    ASSERT_TRUE(wait_until([&] {
      return first_engine.dispatch_prepared_motion_calls == 1 &&
             second_engine.dispatch_prepared_motion_calls == 1;
    }));
  } else {
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(accepted.value().operation_id);
      return state.ok() && !state.value().participants.empty() &&
             state.value().participants.front().terminal_error.has_value();
    }));
    EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest, PreparedCommonClockAllowsExactCoordinatedDispatch) {
  verify_prepared_common_clock_dispatch(true);
}

TEST(ResourceAddressedExecutionKernelTest, PreparedCommonClockRejectsDifferentCandidateBounds) {
  verify_prepared_common_clock_dispatch(false);
}

TEST(ResourceAddressedExecutionKernelTest, PreparedCommonClockRejectsDifferentCandidateDilation) {
  verify_prepared_common_clock_dispatch(true, 2.0);
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedPreparationFailureCancelsPeersAndAtomicallyReleasesEveryReservation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  second_engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                           "injected coordinated preparation failure",
                                           {{"reason", "injected_coordinated_prepare_failure"}}};
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.failure"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls, 1);
  EXPECT_EQ(second_engine.cancel_prepared_motion_calls, 1);
  const auto released_slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(released_slots.begin(), released_slots.end(),
                          [](const auto& slot) { return slot.state == ResourceSlotState::kFree; }));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_EQ(state.value().participants.size(), 2U);
  EXPECT_EQ(state.value().participants[0].state, OperationStateKind::kCancelledPreDispatch);
  EXPECT_EQ(state.value().participants[1].state, OperationStateKind::kInterventionRequired);
}

TEST(ResourceAddressedExecutionKernelTest,
     HeldCoordinatedCancellationPinsEnginesAndCoalescesAbortNotifications) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  second_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto operation = kernel.execute(coordinated_motion_operation("request.abort-lifetime"));
  ASSERT_TRUE(operation.ok());
  second_engine.prepare_failure = ErrorDTO{"preparation_failed", "injected failure", {}};
  HeldSettlementCall held;
  first_engine.cancel_prepared_motion_hook = held.Blocker();
  auto failing =
      std::async(std::launch::async, [&] { return second_engine.complete_preparation(); });
  EXPECT_TRUE(held.Entered());
  const auto before = kernel.get_operation_state(operation.value().operation_id);
  EXPECT_TRUE(before.ok());
  EXPECT_FALSE(
      kernel.reset(control_request("request.abort-lifetime.reset", operation.value(), 1U)).ok());
  EXPECT_FALSE(kernel
                   .reconfigure_runtime_participants(
                       {registration(first, first_engine), registration(second, second_engine)})
                   .ok());
  second_engine.notify_prepared_motion();
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_EQ(second_engine.cancel_prepared_motion_calls.load(), 0);
  EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
  held.Release();
  EXPECT_TRUE(failing.get().ok());
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_EQ(second_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_FALSE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
  EXPECT_TRUE(kernel
                  .reconfigure_runtime_participants(
                      {registration(first, first_engine), registration(second, second_engine)})
                  .ok());
}

TEST(ResourceAddressedExecutionKernelTest, AcceptedResetCannotRetireHeldCoordinatedCancellation) {
  // Reset's first clock read after durable acceptance is Stop ingress, outside
  // the state/ordinary locks. Pin that existing boundary, not a wall-time race.
  class IngressClock final : public IClock {
   public:
    void HoldNextCallerRead() {
      caller_ = std::this_thread::get_id();
      armed_.store(true, std::memory_order_release);
    }
    TimePoint now() const override {
      if (armed_.load(std::memory_order_acquire) && caller_ == std::this_thread::get_id() &&
          armed_.exchange(false))
        held.Hold();
      return 0;
    }
    mutable HeldSettlementCall held;

   private:
    std::thread::id caller_;
    mutable std::atomic_bool armed_{};
  } clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  HeldSettlementCall held_prepare;
  second_engine.prepare_command_hook = held_prepare.Blocker();
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto first = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                  CoordinateKind::kLinear, schema);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(coordinated_command_operation("request.reset-before-abort"));
  });
  EXPECT_TRUE(held_prepare.Entered());
  const OperationAcceptedDTO expected{RequestId("request.reset-before-abort"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  journal.control_acceptance_hook = [&] { clock.HoldNextCallerRead(); };
  auto resetting = std::async(std::launch::async, [&] {
    return kernel.reset(control_request("request.reset-before-abort.reset", expected, 1U));
  });
  EXPECT_TRUE(clock.held.Entered());
  HeldSettlementCall held_cancel;
  first_engine.cancel_prepared_command_hook = held_cancel.Blocker();
  auto aborting = std::async(std::launch::async, [&] {
    first_engine.emit_error(ErrorDTO{"prepare_failed", "command preparation failed", {}});
    first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);
  });
  EXPECT_TRUE(held_cancel.Entered());
  held_prepare.Release();
  EXPECT_TRUE(executing.get().ok());
  clock.held.Release();
  EXPECT_TRUE(resetting.get().ok());
  // This never-dispatched scope can return a no-effect Stop. Its absence of
  // motion does not release the outstanding provider cancellation borrow.
  EXPECT_TRUE(kernel.get_operation_projection(expected.operation_id).value().resident);
  held_cancel.Release();
  aborting.get();
  EXPECT_TRUE(wait_until(
      [&] { return !kernel.get_operation_projection(expected.operation_id).value().resident; }));
  EXPECT_EQ(first_engine.dispatch_prepared_command_calls.load(), 0);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     HeldPreparedReadDrainsAnAlreadyCompletedResetWithoutAnotherCommand) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  engine.defer_prepare_completion = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto operation = kernel.execute(motion_operation("request.reset-held-read"));
  ASSERT_TRUE(operation.ok());
  HeldSettlementCall held;
  engine.observe_prepared_motion_hook = held.Blocker();
  auto reading = std::async(std::launch::async, [&] { engine.notify_prepared_motion(); });
  EXPECT_TRUE(held.Entered());
  EXPECT_TRUE(
      kernel.reset(control_request("request.reset-held-read.reset", operation.value(), 1U)).ok());
  EXPECT_TRUE(wait_until(
      [&] {
        const auto stop = kernel.get_stop_status(StopId("stop.kernel.epoch.1.1"));
        return stop.ok() && stop.value().stop_certificate.has_value();
      },
      std::chrono::milliseconds(1000)));
  EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
  EXPECT_TRUE(wait_until([&] { return engine.certified_reset_calls.load() == 1; },
                         std::chrono::milliseconds(1000)));
  held.Release();
  reading.get();
  EXPECT_TRUE(wait_until([&] {
    return !kernel.get_operation_projection(operation.value().operation_id).value().resident;
  }));
  EXPECT_EQ(engine.dispatch_prepared_motion_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     TerminalPreparationFailureAbortsBeforePrivateSessionCleanupAndCoalescesFeedback) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  first_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto operation = kernel.execute(coordinated_motion_operation("request.terminal-prepare"));
  ASSERT_TRUE(operation.ok());
  const ErrorDTO failure{"planner_failed", "exact original planner failure", {{"attempt", "1"}}};
  first_engine.emit_error(failure);
  // Native planning reports an error before its worker exits. That early
  // event is not yet a terminal prepared-session observation.
  EXPECT_EQ(first_engine.ObservePreparedMotion(first_engine.next_execution_id).value().state,
            ResolvedPreparedMotionExecutionStateDTO::kPreparing);
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 0);
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);
  // Cancel emits nested terminal feedback. It must join the original abort,
  // not issue another cancellation set or replace the original error.
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_EQ(second_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls.load(), 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls.load(), 0);
  const auto current = kernel.get_operation_projection(operation.value().operation_id);
  ASSERT_TRUE(current.ok());
  EXPECT_FALSE(current.value().resident);
  EXPECT_EQ(current.value().operation.state, OperationStateKind::kInterventionRequired);
  EXPECT_EQ(current.value().operation.participants.front().terminal_error, failure);
}

void exercise_late_coordinated_preparation(bool command, bool abort_finishes_first,
                                           bool late_call_fails) {
  SCOPED_TRACE(::testing::Message()
               << "command=" << command << " abort_finishes_first=" << abort_finishes_first
               << " late_call_fails=" << late_call_fails);
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  first_engine.defer_prepare_completion = true;
  second_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto second = coordinated_occurrence(
      command ? make_binding("gripper", "gripper.target", "tool.gripper.command",
                             CoordinateKind::kLinear, schema)
              : first,
      "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall held_prepare;
  HeldSettlementCall held_abort;
  HeldSettlementCall held_late_cancel;
  second_engine.prepare_motion_hook = held_prepare.Blocker();
  second_engine.prepare_command_hook = held_prepare.Blocker();
  second_engine.cancel_prepared_motion_hook = held_late_cancel.Blocker();
  second_engine.cancel_prepared_command_hook = held_late_cancel.Blocker();
  if (late_call_fails) {
    second_engine.prepare_call_failure = ErrorDTO{"late_prepare_failed", "late failure", {}};
    second_engine.prepare_command_failure = second_engine.prepare_call_failure;
  }
  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(command ? coordinated_motion_command_operation("request.late-prepare")
                                  : coordinated_motion_operation("request.late-prepare"));
  });
  EXPECT_TRUE(held_prepare.Entered());
  const OperationAcceptedDTO expected{RequestId("request.late-prepare"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  first_engine.prepare_failure = ErrorDTO{"first_prepare_failed", "first failure", {}};
  first_engine.cancel_prepared_motion_hook = held_abort.Blocker();
  auto aborting =
      std::async(std::launch::async, [&] { return first_engine.complete_preparation(); });
  EXPECT_TRUE(held_abort.Entered());
  if (abort_finishes_first) {
    held_abort.Release();
    EXPECT_TRUE(aborting.get().ok());
  }
  EXPECT_TRUE(kernel.get_operation_projection(expected.operation_id).value().resident);
  EXPECT_FALSE(
      kernel.reset(control_request("request.late-prepare.early-reset", expected, 1U)).ok());
  held_prepare.Release();
  if (!late_call_fails) {
    EXPECT_TRUE(held_late_cancel.Entered());
    EXPECT_TRUE(kernel.get_operation_projection(expected.operation_id).value().resident);
    EXPECT_FALSE(
        kernel.reset(control_request("request.late-prepare.cancel-reset", expected, 1U)).ok());
  }
  held_late_cancel.Release();
  EXPECT_TRUE(executing.get().ok());
  if (!abort_finishes_first) {
    held_abort.Release();
    EXPECT_TRUE(aborting.get().ok());
  }
  EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
  EXPECT_EQ(second_engine.cancel_prepared_motion_calls.load(),
            !command && !late_call_fails ? 1 : 0);
  EXPECT_EQ(second_engine.cancel_prepared_command_calls.load(),
            command && !late_call_fails ? 1 : 0);
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls.load(), 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls.load(), 0);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls.load(), 0);
  const auto current = kernel.get_operation_projection(expected.operation_id);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().operation.state, OperationStateKind::kInterventionRequired);
  if (current.value().resident) {
    EXPECT_TRUE(kernel.reset(control_request("request.late-prepare.reset", expected, 1U)).ok());
    EXPECT_TRUE(wait_until(
        [&] { return !kernel.get_operation_projection(expected.operation_id).value().resident; }));
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     LateInitialPrepareProductsAreCancelledAfterSiblingAbort) {
  for (bool command : {false, true})
    for (bool abort_finishes_first : {false, true})
      exercise_late_coordinated_preparation(command, abort_finishes_first, false);
}

TEST(ResourceAddressedExecutionKernelTest,
     LateInitialPrepareFailuresReleaseTheirOwnBorrowAfterAbort) {
  for (bool command : {false, true})
    for (bool abort_finishes_first : {false, true})
      exercise_late_coordinated_preparation(command, abort_finishes_first, true);
}

#ifdef MANYFORGE_TEST_SQLITE_OPERATION_JOURNAL
TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedAbortPreservesSiblingReadClaimAndFaultAcrossSqliteReopen) {
  TemporaryOperationJournal temporary;
  const auto database = temporary.directory / "sibling-read.sqlite3";
  FakeClock clock;
  ManualScheduler scheduler;
  std::optional<OperationStateDTO> terminal_state;
  {
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    // Keep the first preparation unresolved so the notification actually
    // acquires a read claim. An already captured immutable product is not
    // observed again merely because a duplicate notification arrives.
    first_engine.defer_prepare_completion = true;
    second_engine.defer_prepare_completion = true;
    SqliteOperationJournal journal(database, CellId("cell.test"), 32U);
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto second = coordinated_occurrence(first, "2");
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
        journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto operation = kernel.execute(coordinated_motion_operation("request.abort-sibling"));
    ASSERT_TRUE(operation.ok());
    HeldSettlementCall held;
    first_engine.observe_prepared_motion_hook = held.Blocker();
    auto reading = std::async(std::launch::async, [&] { first_engine.notify_prepared_motion(); });
    EXPECT_TRUE(held.Entered());
    second_engine.prepare_failure = ErrorDTO{"prepare_failed", "injected sibling failure", {}};
    EXPECT_TRUE(second_engine.complete_preparation().ok());
    EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
    EXPECT_EQ(second_engine.cancel_prepared_motion_calls.load(), 1);
    EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
    EXPECT_FALSE(
        kernel.reset(control_request("request.abort-sibling.early-reset", operation.value(), 1U))
            .ok());
    EXPECT_FALSE(kernel.release_retired_engine(&first_engine).ok());
    held.Release();
    reading.get();
    EXPECT_EQ(first_engine.cancel_prepared_motion_calls.load(), 1);
    EXPECT_TRUE(
        kernel.reset(control_request("request.abort-sibling.reset", operation.value(), 1U)).ok());
    EXPECT_TRUE(wait_until([&] {
      return !kernel.get_operation_projection(operation.value().operation_id).value().resident;
    }));
    terminal_state = kernel.get_operation_state(operation.value().operation_id).value();
    EXPECT_EQ(terminal_state->state, OperationStateKind::kInterventionRequired);
  }
  SqliteOperationJournal replacement(database, CellId("cell.test"), 32U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("kernel.epoch.2"), clock.now(), 16U, {});
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), *terminal_state);
}
#endif

TEST(ResourceAddressedExecutionKernelTest,
     StopDuringCoordinatedPrepareBarrierCancelsAllParticipantsWithoutPhysicalDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  second_engine.defer_prepare_completion = true;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.stop"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.stop.coordinated.prepare", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    return first_engine.cancel_prepared_motion_calls == 1 &&
           second_engine.cancel_prepared_motion_calls == 1;
  }));
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 0);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  EXPECT_TRUE(std::all_of(certificate.value().scope_entries.begin(),
                          certificate.value().scope_entries.end(), [](const auto& entry) {
                            return entry.dispatch_state ==
                                   StopScopeDispatchStateDTO::kNeverDispatchedCancelled;
                          }));
}

TEST(ResourceAddressedExecutionKernelTest,
     StopAtPreparedDispatchHandoffPreventsEffectWithoutEscalation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepared_dispatch_effect = true;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted = kernel.execute(motion_operation("request.motion.dispatch-handoff"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_EQ(engine.dispatch_prepared_motion_calls, 1);
  ASSERT_EQ(engine.execute_calls, 0);

  const auto stopped =
      kernel.stop(stop_request("request.stop.dispatch-handoff", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  EXPECT_EQ(engine.cancel_prepared_motion_calls, 1);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
  EXPECT_EQ(certificate.value().scope_entries.front().dispatch_state,
            StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
  EXPECT_TRUE(certificate.value().attempted_stage_results.empty());
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeCollisionUsesTheAddressedStopOwnerAndDeclaredCoordinatedScopeExactlyOnce) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.collision"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message << ": "
                             << accepted.error().details.at("reason");
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  first_engine.emit_collision({{"contact_a", "component/robot/body/tool/collision/0"},
                               {"contact_b", "scene/workpiece/shape/0"}});
  std::optional<StopId> stop_id;
  ASSERT_TRUE(wait_until([&] {
    const auto operation = kernel.get_operation_state(accepted.value().operation_id);
    if (!operation.ok() || !operation.value().active_stop_id.has_value()) {
      return false;
    }
    stop_id = operation.value().active_stop_id;
    return first_engine.translate_stop_stage_calls.load() == 1 &&
           second_engine.translate_stop_stage_calls.load() == 1;
  }));
  ASSERT_TRUE(stop_id.has_value());
  ASSERT_TRUE(first_engine.last_stop_stage.has_value());
  ASSERT_TRUE(second_engine.last_stop_stage.has_value());
  for (const auto* stage : {&*first_engine.last_stop_stage, &*second_engine.last_stop_stage}) {
    ASSERT_TRUE(stage->participant_stop_plan.participant_stop_profile.has_value());
    EXPECT_EQ(stage->participant_stop_plan.participant_stop_profile->profile_id,
              "fault_spring_back_hold");
    EXPECT_EQ(stage->participant_stop_plan.participant_stop_profile->stop_mode,
              MotionInterruptStopMode::kSpringBackHold);
    EXPECT_EQ(stage->participant_stop_plan.participant_stop_profile_purpose,
              ParticipantStopProfilePurposeDTO::kFault);
    EXPECT_FALSE(stage->participant_stop_plan.motion_interrupt_scope_id.has_value());
  }
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(*stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  const auto status = kernel.get_stop_status(*stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().accepted.resolution_disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_EQ(status.value().accepted.resolution_reason,
            StopScopeResolutionReasonDTO::kExactCurrentScope);
  ASSERT_EQ(status.value().accepted.participant_budget_allocations.size(), 2U);
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().requested_selector.kind,
            StopRequestedSelectorKindDTO::kOrdinaryOperations);
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  ASSERT_EQ(certificate.value().attempted_stage_results.size(), 2U);
  EXPECT_TRUE(std::all_of(certificate.value().scope_entries.begin(),
                          certificate.value().scope_entries.end(), [](const auto& entry) {
                            return entry.dispatch_state == StopScopeDispatchStateDTO::kDispatched;
                          }));

  second_engine.emit_collision();
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopCertificateReusesBackendPublishedTerminalResourceState) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(motion_operation("request.motion.collision.backend-terminal"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  engine.translate_stop_stage_hook = [&] {
    engine.emit_transition(ExecutionState::kStopping, ExecutionState::kStopped);
  };

  engine.emit_collision({{"contact_a", "component/robot/body/tool/collision/0"},
                         {"contact_b", "scene/workpiece/shape/0"},
                         {"runtime_query_engine_status", "ok"}});
  std::optional<StopId> stop_id;
  ASSERT_TRUE(wait_until([&] {
    const auto operation = kernel.get_operation_state(accepted.value().operation_id);
    if (!operation.ok() || !operation.value().active_stop_id.has_value()) {
      return false;
    }
    stop_id = operation.value().active_stop_id;
    const auto status = kernel.get_stop_status(*stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  EXPECT_EQ(operation.value().state, OperationStateKind::kStopped);
  ASSERT_EQ(operation.value().participants.size(), 1U);
  EXPECT_EQ(operation.value().participants.front().state, OperationStateKind::kStopped);
  ASSERT_TRUE(operation.value().participants.front().terminal_error.has_value());
  const auto& terminal_error = *operation.value().participants.front().terminal_error;
  EXPECT_EQ(terminal_error.code, std::string(to_string(ErrorCode::kExecutionAborted)));
  EXPECT_EQ(terminal_error.message, "Runtime collision containment stopped the operation");
  EXPECT_EQ(terminal_error.details.at("reason"), "runtime_collision_fault");
  EXPECT_EQ(terminal_error.details.at("contact_a"), "component/robot/body/tool/collision/0");
  EXPECT_EQ(terminal_error.details.at("contact_b"), "scene/workpiece/shape/0");
  EXPECT_EQ(terminal_error.details.at("runtime_query_engine_status"), "ok");
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopResolverPrefersDirectPathWhenSupportingProviderIsCoLocated) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto binding = with_colocated_supporting_capability(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular));
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(motion_operation("request.motion.collision.colocated-stop-provider"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  std::vector<std::tuple<StopContainmentCommandKind, bool, bool>> resolved_authorities;
  std::mutex resolved_authorities_mutex;
  engine.translate_stop_stage_hook = [&] {
    if (!engine.last_stop_stage.has_value()) {
      return;
    }
    const auto& stage = *engine.last_stop_stage;
    const auto kind = stage.participant_stop_plan.stage_commands.at(stage.stage_index)
                          .resolved_stage.containment_command.command_kind;
    {
      std::lock_guard lock(resolved_authorities_mutex);
      resolved_authorities.emplace_back(kind, stage.capability_provider_binding_id.has_value(),
                                        stage.capability_provider_binding_revision.has_value());
    }
    engine.next_stop_result =
        StopResultDTO{false, ErrorDTO{"controller_stop_rejected",
                                      "advance to retained fallback",
                                      {{"reason", "controller_stop_rejected"}}}};
  };

  engine.emit_collision();
  std::optional<StopId> stop_id;
  ASSERT_TRUE(wait_until([&] {
    const auto operation = kernel.get_operation_state(accepted.value().operation_id);
    if (!operation.ok() || !operation.value().active_stop_id.has_value()) {
      return false;
    }
    stop_id = operation.value().active_stop_id;
    const auto status = kernel.get_stop_status(*stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));

  std::lock_guard lock(resolved_authorities_mutex);
  const auto registered = std::find_if(
      resolved_authorities.begin(), resolved_authorities.end(), [](const auto& authority) {
        return std::get<0>(authority) == StopContainmentCommandKind::kRegistered;
      });
  ASSERT_NE(registered, resolved_authorities.end());
  EXPECT_FALSE(std::get<1>(*registered));
  EXPECT_FALSE(std::get<2>(*registered));
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedOverlapRejectsBeforePrepareAndLeavesTheResourceTableUnchanged) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto overlapping_value = coordinated_occurrence(first, "2").value();
  overlapping_value.controllable_component_id = first.value().controllable_component_id;
  overlapping_value.resources.front().resource_id = first.value().resources.front().resource_id;
  auto overlapping = verify_runtime_participant_binding(std::move(overlapping_value));
  ASSERT_TRUE(overlapping.ok()) << overlapping.error().message;
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(overlapping.value(), second_engine)}, clock,
      scheduler, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto rejected =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.overlap"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "coordinated_participant_resource_overlap");
  EXPECT_EQ(first_engine.prepare_motion_calls, 0);
  EXPECT_EQ(second_engine.prepare_motion_calls, 0);
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     PartialDispatchStopsOnlyReleasedParticipantsAndCancelsTheUnreleasedRemainder) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeParticipantEngine third_engine;
  second_engine.next_execute_result =
      Result<std::uint64_t>::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                          "injected second dispatch failure",
                                          {{"reason", "injected_dispatch_failure"}}});
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  const auto third = coordinated_occurrence(first, "3");
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine),
       registration(third, third_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_motion_operation("request.motion.coordinated.partial", 3U));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(third_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(third_engine.cancel_prepared_motion_calls, 1);
  ASSERT_TRUE(wait_until([&] {
    return first_engine.translate_stop_stage_calls.load() == 1 &&
           second_engine.translate_stop_stage_calls.load() == 1;
  }));
  EXPECT_EQ(third_engine.translate_stop_stage_calls.load(), 0);
  const StopId stop_id("stop.kernel.epoch.1.1");
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto status = kernel.get_stop_status(stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_TRUE(std::any_of(journal.calls.begin(), journal.calls.end(), [](const auto& call) {
      return call == "state:operation_partial_dispatch";
    }));
  }
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().participants.size(), 3U);
  EXPECT_EQ(state.value().participants[2].state, OperationStateKind::kCancelledPreDispatch);

  const auto reset =
      kernel.reset(control_request("request.reset.coordinated.partial", accepted.value(),
                                   state.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto current_slots = kernel.resource_snapshot();
    return first_engine.certified_reset_calls.load() == 1 &&
           second_engine.certified_reset_calls.load() == 1 &&
           std::all_of(current_slots.begin(), current_slots.end(),
                       [](const auto& slot) { return slot.state == ResourceSlotState::kFree; });
  }));
  EXPECT_EQ(third_engine.certified_reset_calls.load(), 1);
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     PartialDispatchCancellationPinsTheEngineUntilTheCallDrains) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine, third_engine;
  FakeOperationJournal journal;
  second_engine.next_execute_result =
      Result<std::uint64_t>::Err(ErrorDTO{"execution_rejected", "injected partial dispatch", {}});
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine),
       registration(coordinated_occurrence(first, "3"), third_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall held;
  third_engine.cancel_prepared_motion_hook = held.Blocker();
  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(coordinated_motion_operation("request.partial-held", 3U));
  });
  EXPECT_TRUE(held.Entered());
  const OperationAcceptedDTO expected{RequestId("request.partial-held"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  EXPECT_TRUE(wait_until(
      [&] {
        const auto stop = kernel.get_stop_status(StopId("stop.kernel.epoch.1.1"));
        return stop.ok() && stop.value().state == StopStatusStateDTO::kSucceeded;
      },
      std::chrono::milliseconds(1000)));
  const auto state = kernel.get_operation_state(expected.operation_id).value();
  EXPECT_FALSE(kernel
                   .reset(control_request("request.partial-held.too-early", expected,
                                          state.operation_revision.value()))
                   .ok());
  EXPECT_EQ(third_engine.certified_reset_calls.load(), 0);
  auto reconfiguring = std::async(std::launch::async, [&] {
    return kernel.reconfigure_runtime_participants({registration(first, first_engine)});
  });
  EXPECT_EQ(reconfiguring.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  EXPECT_TRUE(kernel.get_operation_projection(expected.operation_id).value().resident);
  held.Release();
  EXPECT_TRUE(executing.get().ok());
  // Ordinary reconfiguration is serialized behind initial submission. Its
  // post-drain admission may reject or retain this terminal operation, but
  // it cannot replace the engine while the cancellation is borrowed.
  const auto reconfigured = reconfiguring.get();
  if (reconfigured.ok()) {
    EXPECT_TRUE(kernel.participant_engine_in_use(&third_engine));
  }
  EXPECT_TRUE(kernel
                  .reset(control_request("request.partial-held.reset", expected,
                                         state.operation_revision.value()))
                  .ok());
  EXPECT_TRUE(wait_until(
      [&] { return !kernel.get_operation_projection(expected.operation_id).value().resident; }));
  EXPECT_EQ(third_engine.dispatch_prepared_motion_calls.load(), 0);
  EXPECT_EQ(third_engine.cancel_prepared_motion_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     BlockedPartialDispatchJournalCannotDelayEmergencyContainment) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeParticipantEngine third_engine;
  second_engine.next_execute_result =
      Result<std::uint64_t>::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                          "injected second dispatch failure",
                                          {{"reason", "injected_dispatch_failure"}}});
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  const auto third = coordinated_occurrence(first, "3");
  FakeOperationJournal journal;
  std::promise<void> partial_journal_entered;
  std::promise<void> release_partial_journal;
  const auto release_future = release_partial_journal.get_future().share();
  journal.record_hook = [&](const std::string_view kind) {
    if (kind != "state:operation_partial_dispatch") {
      return;
    }
    partial_journal_entered.set_value();
    release_future.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine),
       registration(third, third_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(
        coordinated_motion_operation("request.motion.coordinated.partial.blocked-journal", 3U));
  });
  ASSERT_EQ(partial_journal_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  EXPECT_EQ(executing.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(StopId("stop.kernel.epoch.1.1"));
    return first_engine.translate_stop_stage_calls.load() == 1 &&
           second_engine.translate_stop_stage_calls.load() == 1 && status.ok() &&
           status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  EXPECT_EQ(third_engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(third_engine.cancel_prepared_motion_calls, 1);

  release_partial_journal.set_value();
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedCapabilityCommandsPrepareAllBeforeDeterministicDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  std::vector<std::string> timeline;
  first_engine.timeline = &timeline;
  second_engine.timeline = &timeline;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto first = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                  CoordinateKind::kLinear, schema);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_command_operation("request.command.coordinated.success"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.prepare_command_calls, 1);
  EXPECT_EQ(second_engine.prepare_command_calls, 1);
  EXPECT_EQ(first_engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(timeline, (std::vector<std::string>{
                          "engine:prepare_command", "engine:prepare_command",
                          "engine:dispatch_prepared_command", "engine:command_gripper",
                          "engine:dispatch_prepared_command", "engine:command_gripper"}));
  ASSERT_TRUE(first_engine.prepared_command_snapshot.has_value());
  ASSERT_TRUE(second_engine.prepared_command_snapshot.has_value());
  EXPECT_TRUE(std::holds_alternative<PreparedCapabilityCommandDTO>(
      first_engine.prepared_command_snapshot->participant->payload));
  EXPECT_TRUE(std::holds_alternative<PreparedCapabilityCommandDTO>(
      second_engine.prepared_command_snapshot->participant->payload));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kExecuting);
  const auto slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kDispatched;
  }));
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedCommandPreparationFailureCancelsPreparedPeersAndReleasesAllResources) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  second_engine.prepare_command_failure =
      ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
               "injected coordinated command preparation failure",
               {{"reason", "injected_command_prepare_failure"}}};
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto first = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                  CoordinateKind::kLinear, schema);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_command_operation("request.command.coordinated.failure"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.cancel_prepared_command_calls.load(), 1);
  EXPECT_EQ(second_engine.cancel_prepared_command_calls.load(), 0);
  EXPECT_EQ(first_engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls, 0);
  const auto slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(slots.begin(), slots.end(),
                          [](const auto& slot) { return slot.state == ResourceSlotState::kFree; }));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_EQ(state.value().participants.size(), 2U);
  EXPECT_EQ(state.value().participants[0].state, OperationStateKind::kCancelledPreDispatch);
  EXPECT_EQ(state.value().participants[1].state, OperationStateKind::kInterventionRequired);
}

TEST(ResourceAddressedExecutionKernelTest,
     MixedCoordinatedOperationHoldsMotionUntilCommandPreparationCompletes) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine motion_engine;
  FakeParticipantEngine command_engine;
  std::vector<std::string> timeline;
  motion_engine.timeline = &timeline;
  command_engine.timeline = &timeline;
  std::promise<void> command_prepare_entered;
  std::promise<void> release_command_prepare;
  auto release_future = release_command_prepare.get_future().share();
  command_engine.prepare_command_hook = [&] {
    command_prepare_entered.set_value();
    release_future.wait();
  };
  const auto motion =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto command_source = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                           CoordinateKind::kLinear, schema);
  const auto command = coordinated_occurrence(command_source, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(motion, motion_engine), registration(command, command_engine)}, clock,
      scheduler, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(coordinated_motion_command_operation("request.mixed.coordinated.held"));
  });
  ASSERT_EQ(command_prepare_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  EXPECT_EQ(motion_engine.prepare_motion_calls, 1);
  EXPECT_EQ(motion_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(command_engine.dispatch_prepared_command_calls, 0);
  const auto reserved_slots = kernel.resource_snapshot();
  EXPECT_TRUE(std::all_of(reserved_slots.begin(), reserved_slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kReserved;
  }));

  release_command_prepare.set_value();
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(timeline,
            (std::vector<std::string>{
                "engine:prepare_motion", "engine:prepare_command",
                "engine:dispatch_prepared_motion", "engine:execute", "engine:set_runtime_scale",
                "engine:dispatch_prepared_command", "engine:command_gripper"}));
  EXPECT_EQ(motion_engine.dispatch_prepared_motion_calls, 1);
  EXPECT_EQ(command_engine.dispatch_prepared_command_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopDuringCoordinatedCommandPreparationCancelsEveryPreparationWithoutDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  std::promise<void> second_prepare_entered;
  std::promise<void> release_second_prepare;
  auto release_future = release_second_prepare.get_future().share();
  second_engine.prepare_command_hook = [&] {
    second_prepare_entered.set_value();
    release_future.wait();
  };
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto first = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                  CoordinateKind::kLinear, schema);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(coordinated_command_operation("request.command.coordinated.stop"));
  });
  ASSERT_EQ(second_prepare_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  const OperationAcceptedDTO expected{RequestId("request.command.coordinated.stop"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  const auto stopped = kernel.stop(stop_request("request.stop.coordinated.command", expected, 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  release_second_prepare.set_value();
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(wait_until([&] {
    return first_engine.cancel_prepared_command_calls.load() == 1 &&
           second_engine.cancel_prepared_command_calls.load() == 1;
  }));
  EXPECT_EQ(first_engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(first_engine.command_calls, 0);
  EXPECT_EQ(second_engine.command_calls, 0);
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 0);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  EXPECT_TRUE(std::all_of(certificate.value().scope_entries.begin(),
                          certificate.value().scope_entries.end(), [](const auto& entry) {
                            return entry.dispatch_state ==
                                   StopScopeDispatchStateDTO::kNeverDispatchedCancelled;
                          }));
}

TEST(ResourceAddressedExecutionKernelTest,
     PartialCapabilityCommandDispatchStopsAttemptedAndCancelsUnreleasedParticipants) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeParticipantEngine third_engine;
  second_engine.next_command_result =
      GripperCommandResultDTO{false, 0U, 0U,
                              ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                       "injected command dispatch failure",
                                       {{"reason", "injected_command_dispatch_failure"}}}};
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto first = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                  CoordinateKind::kLinear, schema);
  const auto second = coordinated_occurrence(first, "2");
  const auto third = coordinated_occurrence(first, "3");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine),
       registration(third, third_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto accepted =
      kernel.execute(coordinated_command_operation("request.command.coordinated.partial", 3U));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(first_engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(second_engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(third_engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(third_engine.cancel_prepared_command_calls.load(), 1);
  ASSERT_TRUE(wait_until([&] {
    return first_engine.translate_stop_stage_calls.load() == 1 &&
           second_engine.translate_stop_stage_calls.load() == 1;
  }));
  EXPECT_EQ(third_engine.translate_stop_stage_calls.load(), 0);
  const StopId stop_id("stop.kernel.epoch.1.1");
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto status = kernel.get_stop_status(stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().participants.size(), 3U);
  EXPECT_EQ(state.value().participants[2].state, OperationStateKind::kCancelledPreDispatch);
}

TEST(ResourceAddressedExecutionKernelTest,
     RetainsUnqualifiedRegistrationForObservationButRefusesMotionAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto unqualified = unqualified_registration(binding, engine);
  unqualified.stop_execution_port = nullptr;

  ResourceAddressedExecutionKernel kernel(
      {std::move(unqualified)}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"),
          CellId("cell.test"),
          8U,
          {},
          nullptr,
          binding.value().graph_revision,
          StopDeadlineBudgetDTO{1'000'000'000, 100'000'000, {}}});

  const auto result = kernel.execute(motion_operation());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_route_publication_required");
  EXPECT_EQ(engine.execute_calls, 0);
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     RefusesMotionWhenStopReadinessExistsButParticipantPolicyIsAbsent) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto unqualified = registration(binding, engine);
  unqualified.participant_policy.reset();

  ResourceAddressedExecutionKernel kernel(
      {std::move(unqualified)}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_policy_missing");
  EXPECT_EQ(engine.execute_calls, 0);
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     DisjointSameTypeBindingsRunConcurrentlyOnTheirExactPhysicalEngines) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto first = kernel.execute(motion_operation("request.robot.1"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first_engine.execute_calls, 1);
  EXPECT_EQ(second_engine.execute_calls, 0);
  ASSERT_TRUE(first_engine.last_execute.has_value());
  EXPECT_EQ(first_engine.last_execute->binding.controllable_component_id,
            ControllableComponentId("robot.1"));
  EXPECT_EQ(first_engine.last_execute->binding.resources.front(), ResourceId("robot.joint_1"));
  auto second_operation = motion_operation("request.robot.2");
  second_operation.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_operation);
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(first_engine.execute_calls, 1);
  EXPECT_EQ(second_engine.execute_calls, 1);
  ASSERT_TRUE(second_engine.last_execute.has_value());
  EXPECT_EQ(second_engine.last_execute->binding.controllable_component_id,
            ControllableComponentId("robot.2"));
  EXPECT_EQ(second_engine.last_execute->binding.resources.front(), ResourceId("robot.2.joint_1"));
  ASSERT_TRUE(first_engine.last_prepared_request);
  ASSERT_TRUE(second_engine.last_prepared_request);
  EXPECT_FALSE(first_engine.last_prepared_request->common_timing);
  EXPECT_FALSE(second_engine.last_prepared_request->common_timing);
  const auto& resolved_goal = std::get<ResolvedJointGoalDTO>(second_engine.last_execute->motion);
  EXPECT_EQ(resolved_goal.joint_names, (std::vector<std::string>{"mf2_robot_2_joint_1"}));

  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto first_still_prepared = kernel.get_operation_state(first.value().operation_id);
  const auto second_executing = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(first_still_prepared.ok()) << first_still_prepared.error().message;
  ASSERT_TRUE(second_executing.ok()) << second_executing.error().message;
  EXPECT_EQ(first_still_prepared.value().state, OperationStateKind::kPrepared);
  EXPECT_EQ(second_executing.value().state, OperationStateKind::kExecuting);

  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged =
      kernel.acknowledge_completed(control_request("request.robot.1.ack", first.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    const auto first_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
      return slot.resource_id == ResourceId("robot.joint_1");
    });
    return first_engine.acknowledge_calls.load() == 1 && first_slot != slots.end() &&
           first_slot->state == ResourceSlotState::kFree;
  }));
  const auto first_historical = kernel.get_operation_state(first.value().operation_id);
  ASSERT_TRUE(first_historical.ok()) << first_historical.error().message;
  EXPECT_EQ(first_historical.value().state, OperationStateKind::kCompleted);
  const auto second_after_first_ack = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(second_after_first_ack.ok()) << second_after_first_ack.error().message;
  EXPECT_EQ(second_after_first_ack.value().state, OperationStateKind::kExecuting);

  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 2U);
  const auto first_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.resource_id == ResourceId("robot.joint_1");
  });
  const auto second_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.resource_id == ResourceId("robot.2.joint_1");
  });
  ASSERT_NE(first_slot, slots.end());
  ASSERT_NE(second_slot, slots.end());
  EXPECT_EQ(first_slot->state, ResourceSlotState::kFree);
  EXPECT_EQ(second_slot->state, ResourceSlotState::kDispatched);
}

TEST(ResourceAddressedExecutionKernelTest,
     OverlappingOperationIsRejectedAtomicallyWithoutDisturbingResidentOwner) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto overlapping_binding = second_occurrence_sharing_first_resource(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(overlapping_binding, second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto first = kernel.execute(motion_operation("request.overlap.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto overlapping = motion_operation("request.overlap.second");
  overlapping.program_binding_id = overlapping_binding.value().program_binding_id;
  overlapping.participants.front().target_role_id = TargetRoleId("arm.overlap");
  const auto rejected = kernel.execute(overlapping);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "resource_scope_busy");
  EXPECT_EQ(first_engine.execute_calls, 1);
  EXPECT_EQ(second_engine.execute_calls, 0);

  const auto first_state = kernel.get_operation_state(first.value().operation_id);
  ASSERT_TRUE(first_state.ok()) << first_state.error().message;
  EXPECT_EQ(first_state.value().state, OperationStateKind::kPrepared);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_id, first.value().operation_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     OneSingleParticipantEngineCannotOwnTwoResidentOperations) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, engine), registration(second_binding, engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto first = kernel.execute(motion_operation("request.shared-engine.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second = motion_operation("request.shared-engine.second");
  second.program_binding_id = second_binding.value().program_binding_id;
  const auto rejected = kernel.execute(second);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "participant_engine_busy");
  EXPECT_EQ(engine.execute_calls, 1);

  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 2U);
  const auto first_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.resource_id == ResourceId("robot.joint_1");
  });
  const auto second_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
    return slot.resource_id == ResourceId("robot.2.joint_1");
  });
  ASSERT_NE(first_slot, slots.end());
  ASSERT_NE(second_slot, slots.end());
  EXPECT_EQ(first_slot->state, ResourceSlotState::kDispatched);
  EXPECT_EQ(second_slot->state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     LatePriorExecutionEventCannotAttachToANewOperationDuringDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto first = kernel.execute(motion_operation("request.late-event.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.late-event.first.ack", first.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return !slots.empty() && slots.front().state == ResourceSlotState::kFree;
  }));

  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  engine.event_during_execute =
      KernelEvent{KernelEventType::kStateTransition, 41U,          0, ExecutionState::kPlanning,
                  ExecutionState::kCompleted,        std::nullopt, {}};
  const auto second = kernel.execute(motion_operation("request.late-event.second"));
  ASSERT_TRUE(second.ok()) << second.error().message;
  const auto after_late_event = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(after_late_event.ok()) << after_late_event.error().message;
  EXPECT_EQ(after_late_event.value().state, OperationStateKind::kPrepared);

  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto after_exact_event = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(after_exact_event.ok()) << after_exact_event.error().message;
  EXPECT_EQ(after_exact_event.value().state, OperationStateKind::kExecuting);
}

TEST(ResourceAddressedExecutionKernelTest,
     ControlsStopAndRuntimeScaleRemainScopedToTheNamedDisjointOperation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto first = kernel.execute(motion_operation("request.scoped.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.scoped.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  const auto scaled = kernel.set_runtime_scale(
      runtime_scale_request("request.scoped.scale.second", second.value(), 1U, 0U, 0.4));
  ASSERT_TRUE(scaled.ok()) << scaled.error().message;
  EXPECT_EQ(first_engine.runtime_scale_calls, 1);
  EXPECT_EQ(second_engine.runtime_scale_calls, 2);
  const auto second_scale = kernel.get_runtime_scale(second.value().operation_id);
  ASSERT_TRUE(second_scale.ok()) << second_scale.error().message;
  EXPECT_EQ(second_scale.value().runtime_scale_generation, RuntimeScaleGeneration(1U));
  EXPECT_DOUBLE_EQ(second_scale.value().requested_scale, 0.4);

  const auto stopped = kernel.stop(stop_request("request.scoped.stop.second", second.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] { return second_engine.stop_calls.load() == 1; }));
  EXPECT_EQ(first_engine.stop_calls, 0);
  EXPECT_EQ(second_engine.stop_calls, 1);
  const auto first_after_second_stop = kernel.get_operation_state(first.value().operation_id);
  const auto second_after_stop = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(first_after_second_stop.ok()) << first_after_second_stop.error().message;
  ASSERT_TRUE(second_after_stop.ok()) << second_after_stop.error().message;
  EXPECT_EQ(first_after_second_stop.value().operation_revision, OperationRevision(1U));
  EXPECT_EQ(first_after_second_stop.value().state, OperationStateKind::kExecuting);
  EXPECT_TRUE(second_after_stop.value().state == OperationStateKind::kStopping ||
              second_after_stop.value().state == OperationStateKind::kStopped);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramRunStopAtomicallyCapturesTheExactRunAndReplaysOnce) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto first = kernel.execute(motion_operation("request.program-run.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.program-run.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  const auto request = program_run_stop_request("request.program-run.stop", "run.1");
  const auto accepted = kernel.stop(request);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto replay = kernel.stop(request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().stop_id, accepted.value().stop_id);
  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; }));
  EXPECT_EQ(accepted.value().affected_operation_revisions.size(), 2U);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(accepted.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto terminal_replay = kernel.stop(request);
  ASSERT_TRUE(terminal_replay.ok()) << terminal_replay.error().message;
  EXPECT_EQ(terminal_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(terminal_replay.value().stop_id, accepted.value().stop_id);

  const auto later_stop =
      kernel.stop(program_run_stop_request("request.program-run.later-stop", "run.1"));
  ASSERT_TRUE(later_stop.ok()) << later_stop.error().message;
  EXPECT_NE(later_stop.value().stop_id, accepted.value().stop_id);
  EXPECT_GT(later_stop.value().stop_generation.value(), accepted.value().stop_generation.value());
  EXPECT_EQ(later_stop.value().resolution_disposition,
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
  EXPECT_FALSE(kernel
                   .release_program_run_stop_fence(ProgramRunId("run.1"), accepted.value().stop_id,
                                                   accepted.value().stop_generation)
                   .ok());

  const auto conflict =
      kernel.stop(program_run_stop_request("request.program-run.stop", "run.other"));
  ASSERT_TRUE(conflict.ok()) << conflict.error().message;
  EXPECT_EQ(conflict.value().resolution_disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(conflict.value().resolution_reason,
            StopScopeResolutionReasonDTO::kRequestIdentityConflict);
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);

  const auto stale = kernel.stop(
      program_run_stop_request("request.program-run.stale", "run.1", "kernel.epoch.stale"));
  ASSERT_TRUE(stale.ok()) << stale.error().message;
  EXPECT_TRUE(std::any_of(stale.value().stale_diagnostics.begin(),
                          stale.value().stale_diagnostics.end(), [](const auto& diagnostic) {
                            return diagnostic.field == StopScopeStaleFieldDTO::kRequestOwnerEpoch;
                          }));
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     UndispatchedPreparationFailureStaysRecoverableWhileDispatchedPeerStops) {
  // Live incident 2026-10-02: one robot's IK failed before dispatch and was
  // held for intervention; the Program failure pause then stopped the whole
  // run. The never-dispatched attempt has no physical effect to contain, so
  // the Stop must neither execute stages for it nor latch its resources.
  for (const bool full : {false, true}) {
    SCOPED_TRACE(full ? "full Stop" : "Program-run Stop");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine dispatched_engine;
    FakeParticipantEngine failed_engine;
    failed_engine.prepare_failure =
        ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)), "IK has no solution",
                 {{"reason", "native_ik_no_solution"}}};
    const auto dispatched_binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto failed_binding = second_same_type_occurrence(dispatched_binding);
    ResourceAddressedExecutionKernel kernel({registration(dispatched_binding, dispatched_engine),
                                             registration(failed_binding, failed_engine)},
                                            clock, scheduler, default_journal,
                                            {RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 16U});

    const auto dispatched = kernel.execute(motion_operation("request.incident.dispatched"));
    ASSERT_TRUE(dispatched.ok()) << dispatched.error().message;
    dispatched_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    auto failed_intent = motion_operation("request.incident.ik-failed");
    failed_intent.program_binding_id = failed_binding.value().program_binding_id;
    const auto failed = kernel.execute(failed_intent);
    ASSERT_TRUE(failed.ok()) << failed.error().message;
    EXPECT_EQ(failed_engine.dispatch_prepared_motion_calls, 0);
    const auto held = kernel.get_operation_state(failed.value().operation_id);
    ASSERT_TRUE(held.ok()) << held.error().message;
    ASSERT_EQ(held.value().state, OperationStateKind::kInterventionRequired);

    // The dispatched peer still needs its own exact Stop evidence.
    HeldSettlementCall peer_stage;
    dispatched_engine.translate_stop_stage_hook = peer_stage.Blocker();
    const auto stopped =
        full ? kernel.full_stop(FullStopIntentDTO{
                   PublicRequestContextDTO{RequestId("request.incident.full-stop"),
                                           RequestOwnerEpoch("kernel.epoch.1"), "test",
                                           std::nullopt},
                   StopGeneration(0U),
                   {}})
             : kernel.stop(program_run_stop_request("request.incident.stop", "run.1"));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
    EXPECT_EQ(stopped.value().affected_operation_revisions.front().operation_id,
              dispatched.value().operation_id);
    ASSERT_TRUE(peer_stage.Entered());
    const auto pending = kernel.get_stop_status(stopped.value().stop_id);
    ASSERT_TRUE(pending.ok()) << pending.error().message;
    EXPECT_FALSE(pending.value().stop_certificate.has_value());
    peer_stage.Release();
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));

    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    ASSERT_TRUE(status.ok()) << status.error().message;
    EXPECT_EQ(status.value().state, StopStatusStateDTO::kSucceeded);
    const auto certificate =
        kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
    ASSERT_TRUE(certificate.ok()) << certificate.error().message;
    EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededNominal);
    EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
    ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
    EXPECT_EQ(std::get<StopParticipantScopeKeyDTO>(
                  certificate.value().scope_entries.front().key.subject)
                  .operation_id,
              dispatched.value().operation_id);
    EXPECT_EQ(dispatched_engine.translate_stop_stage_calls.load(), 1);
    EXPECT_EQ(failed_engine.translate_stop_stage_calls.load(), 0);
    EXPECT_EQ(failed_engine.stop_calls.load(), 0);
    EXPECT_EQ(failed_engine.cancel_prepared_motion_calls.load(), 0);

    // The failed attempt keeps its exact failure and recovery authority.
    const auto retained = kernel.get_operation_state(failed.value().operation_id);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    EXPECT_EQ(retained.value().state, OperationStateKind::kInterventionRequired);
    EXPECT_FALSE(retained.value().active_stop_id.has_value());
    ASSERT_TRUE(retained.value().participants.front().terminal_error.has_value());
    EXPECT_EQ(retained.value().participants.front().terminal_error->details.at("reason"),
              "native_ik_no_solution");
    EXPECT_TRUE(kernel.get_operation_recovery_assessment(failed.value().operation_id).ok());

    // A Stop naming only the failed attempt has nothing physical to contain.
    const auto direct = kernel.stop(stop_request("request.incident.direct", failed.value(),
                                                 retained.value().operation_revision.value()));
    ASSERT_TRUE(direct.ok()) << direct.error().message;
    EXPECT_EQ(direct.value().resolution_disposition,
              StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
    EXPECT_TRUE(direct.value().affected_operation_revisions.empty());
    EXPECT_EQ(failed_engine.translate_stop_stage_calls.load(), 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     PreparationFailureRacingStopSettlesWithoutLatchAndStaysRecoverable) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine dispatched_engine;
  FakeParticipantEngine failed_engine;
  failed_engine.defer_prepare_completion = true;
  const auto dispatched_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto failed_binding = second_same_type_occurrence(dispatched_binding);
  ResourceAddressedExecutionKernel kernel({registration(dispatched_binding, dispatched_engine),
                                           registration(failed_binding, failed_engine)},
                                          clock, scheduler, default_journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto dispatched = kernel.execute(motion_operation("request.race.dispatched"));
  ASSERT_TRUE(dispatched.ok()) << dispatched.error().message;
  dispatched_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  auto failed_intent = motion_operation("request.race.planning");
  failed_intent.program_binding_id = failed_binding.value().program_binding_id;
  const auto failed = kernel.execute(failed_intent);
  ASSERT_TRUE(failed.ok()) << failed.error().message;

  // Stop captures the still-planning attempt; its IK failure lands, with the
  // owner's Failed terminal, before the Stop's idempotent cancellation returns.
  HeldSettlementCall cancellation;
  failed_engine.cancel_prepared_motion_hook = cancellation.Blocker();
  const auto stopped = kernel.stop(program_run_stop_request("request.race.stop", "run.1"));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 2U);
  ASSERT_TRUE(cancellation.Entered());
  failed_engine.prepare_failure =
      ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)), "IK has no solution",
               {{"reason", "native_ik_no_solution"}}};
  ASSERT_TRUE(failed_engine.complete_preparation().ok());
  failed_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);
  cancellation.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));

  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().state, StopStatusStateDTO::kSucceeded);
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
  ASSERT_EQ(certificate.value().scope_entries.size(), 2U);
  for (const auto& entry : certificate.value().scope_entries) {
    const auto& key = std::get<StopParticipantScopeKeyDTO>(entry.key.subject);
    const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry.facts);
    if (key.operation_id == failed.value().operation_id) {
      EXPECT_EQ(entry.dispatch_state, StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
      EXPECT_EQ(facts.terminal_settlement,
                StopParticipantScopeFactsDTO::TerminalSettlementState::kNotRequired);
    } else {
      EXPECT_EQ(entry.dispatch_state, StopScopeDispatchStateDTO::kDispatched);
    }
  }
  EXPECT_EQ(dispatched_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(failed_engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(failed_engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(failed_engine.participant_stop_settlement_calls.load(), 0);

  const auto retained = kernel.get_operation_state(failed.value().operation_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(retained.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(retained.value().participants.front().terminal_error->details.at("reason"),
            "native_ik_no_solution");
  EXPECT_TRUE(kernel.get_operation_recovery_assessment(failed.value().operation_id).ok());
  const auto peer = kernel.get_operation_state(dispatched.value().operation_id);
  ASSERT_TRUE(peer.ok()) << peer.error().message;
  EXPECT_EQ(peer.value().state, OperationStateKind::kStopped);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopFaultLatchHoldsNoRestObligationForSettledNeverDispatchedParticipant) {
  // A planning participant cancelled before dispatch has no Stop stage and so
  // no motion-rest source. When a dispatched peer escalates, the latch keeps
  // the peer's resources for physical recovery but must not hold the
  // never-dispatched robot's resources hostage to an observation that cannot
  // exist.
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine dispatched_engine;
  FakeParticipantEngine planning_engine;
  planning_engine.defer_prepare_completion = true;
  dispatched_engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{"controller_stop_rejected", "controller rejected Stop", {}}};
  const auto dispatched_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto planning_binding = second_same_type_occurrence(dispatched_binding);
  ResourceAddressedExecutionKernel kernel({registration(dispatched_binding, dispatched_engine),
                                           registration(planning_binding, planning_engine)},
                                          clock, scheduler, default_journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto dispatched = kernel.execute(motion_operation("request.latch.dispatched"));
  ASSERT_TRUE(dispatched.ok()) << dispatched.error().message;
  dispatched_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  auto planning_intent = motion_operation("request.latch.planning");
  planning_intent.program_binding_id = planning_binding.value().program_binding_id;
  const auto planning = kernel.execute(planning_intent);
  ASSERT_TRUE(planning.ok()) << planning.error().message;

  // The nominal stage is rejected; the stronger prepared stage contains.
  dispatched_engine.translate_stop_stage_hook = [&, calls = std::make_shared<unsigned>(0U)] {
    if ((*calls)++ > 0U) dispatched_engine.next_stop_result = StopResultDTO{true, std::nullopt};
  };
  const auto stopped = kernel.stop(program_run_stop_request("request.latch.stop", "run.1"));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 2U);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededEscalated);
  std::vector<ResourceId> dispatched_resources;
  std::vector<ResourceId> planning_resources;
  for (const auto& entry : certificate.value().scope_entries) {
    const auto& key = std::get<StopParticipantScopeKeyDTO>(entry.key.subject);
    const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry.facts);
    if (key.operation_id == planning.value().operation_id) {
      EXPECT_EQ(entry.dispatch_state, StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
      EXPECT_EQ(facts.terminal_settlement,
                StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled);
      planning_resources = facts.resources;
    } else {
      dispatched_resources = facts.resources;
    }
  }
  ASSERT_FALSE(dispatched_resources.empty());
  ASSERT_FALSE(planning_resources.empty());
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kEscalated);
  EXPECT_EQ(latch->captured_resources, dispatched_resources);
  std::vector<OperationId> operations{dispatched.value().operation_id,
                                      planning.value().operation_id};
  std::sort(operations.begin(), operations.end());
  EXPECT_EQ(latch->captured_operation_ids, operations);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopRacingNaturalCompletionSettlesNominalWithoutLatchButFailuresEscalate) {
  // Live 2026-10-06: an ordinary Stop was accepted while the goal executed;
  // the goal reached its endpoint before its controlled stage ran. The barrier
  // places that natural completion (or an abort) between acceptance and the
  // stage result. Only the exact natural-completion settlement avoids
  // escalation; a still-active goal whose Stop fails, or an aborted goal,
  // escalates and latches as before.
  enum class Race { kNaturalCompletion, kStillActiveStopFailed, kAbortedBeforeStage };
  for (const auto race :
       {Race::kNaturalCompletion, Race::kStillActiveStopFailed, Race::kAbortedBeforeStage}) {
    SCOPED_TRACE(static_cast<int>(race));
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(binding, engine)}, clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto accepted = kernel.execute(motion_operation("request.race.natural"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    const auto executing = kernel.get_operation_state(accepted.value().operation_id);
    ASSERT_TRUE(executing.ok()) << executing.error().message;
    ASSERT_EQ(executing.value().state, OperationStateKind::kExecuting);

    if (race == Race::kNaturalCompletion) {
      engine.next_stop_evidence = JsonValueDTO{JsonObjectDTO{
          {"completed_by_natural_completion", JsonValueDTO{true}},
          {"provider_diagnostics",
           JsonValueDTO{JsonObjectDTO{{"natural_completion_basis",
                                       JsonValueDTO{"goal_completed_before_stop_stage"}}}}}}};
    } else {
      engine.next_stop_result =
          StopResultDTO{false, ErrorDTO{"motion_interrupt_backend_stop_failed",
                                        "native prepared goal no longer owns the active execution",
                                        {{"reason", "prepared_goal_not_active"}}}};
    }
    HeldSettlementCall nominal_stage;
    engine.translate_stop_stage_hook = [&, gate = nominal_stage.Blocker(),
                                        calls = std::make_shared<unsigned>(0U)] {
      if ((*calls)++ == 0U) {
        gate();
      } else {
        engine.next_stop_result = StopResultDTO{true, std::nullopt};
      }
    };
    const auto stopped = kernel.stop(stop_request("request.race.stop", accepted.value(),
                                                  executing.value().operation_revision.value()));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_TRUE(nominal_stage.Entered());
    if (race == Race::kNaturalCompletion) {
      engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
    } else if (race == Race::kAbortedBeforeStage) {
      engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kFailed,
                             ErrorDTO{"execution_aborted", "goal aborted", {}});
    }
    nominal_stage.Release();
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));

    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    ASSERT_TRUE(status.ok()) << status.error().message;
    EXPECT_EQ(status.value().state, StopStatusStateDTO::kSucceeded);
    const auto certificate =
        kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
    ASSERT_TRUE(certificate.ok()) << certificate.error().message;
    if (race == Race::kNaturalCompletion) {
      EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededNominal);
      EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
      EXPECT_EQ(engine.translate_stop_stage_calls.load(), 1);
      ASSERT_EQ(certificate.value().attempted_stage_results.size(), 1U);
      const auto& attempt = certificate.value().attempted_stage_results.front().result;
      EXPECT_EQ(attempt.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
      ASSERT_FALSE(attempt.evidence_items.empty());
      ASSERT_TRUE(attempt.evidence_items.front().evidence.has_value());
      const auto& evidence =
          std::get<JsonObjectDTO>(attempt.evidence_items.front().evidence->value);
      EXPECT_TRUE(std::get<bool>(evidence.at("completed_by_natural_completion").value));
      // The accepted Stop owns the terminal boundary; the natural completion
      // is retained as its evidence rather than replacing it.
      const auto terminal = kernel.get_operation_state(accepted.value().operation_id);
      ASSERT_TRUE(terminal.ok()) << terminal.error().message;
      EXPECT_EQ(terminal.value().state, OperationStateKind::kStopped);
      EXPECT_FALSE(terminal.value().participants.front().terminal_error.has_value());
    } else {
      EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededEscalated);
      EXPECT_EQ(engine.translate_stop_stage_calls.load(), 2);
      const auto latch = kernel.get_stop_fault_latch();
      ASSERT_TRUE(latch.has_value());
      EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kEscalated);
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     RacingOrUncertainDispatchNeverTakesTheNoEffectStopShortcut) {
  {
    SCOPED_TRACE("coordinated pre-dispatch abort still owns a sibling borrow");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    FakeOperationJournal journal;
    second_engine.defer_prepare_completion = true;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto second = coordinated_occurrence(first, "2");
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine), registration(second, second_engine)}, clock,
        scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto operation = kernel.execute(coordinated_motion_operation("request.racing.abort"));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    second_engine.prepare_failure = ErrorDTO{"preparation_failed", "injected failure", {}};
    // Like ExecutionKernel, the local status names the prepared execution, so
    // a terminal published before Stop work exists is still read.
    first_engine.status.execution_id = first_engine.next_execution_id;
    second_engine.status.execution_id = second_engine.next_execution_id;
    HeldSettlementCall held;
    first_engine.cancel_prepared_motion_hook = held.Blocker();
    auto failing =
        std::async(std::launch::async, [&] { return second_engine.complete_preparation(); });
    ASSERT_TRUE(held.Entered());
    // ExecutionKernel commits the failed preparation's own Failed terminal.
    second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);
    const auto before = kernel.get_operation_state(operation.value().operation_id);
    ASSERT_TRUE(before.ok()) << before.error().message;
    ASSERT_EQ(before.value().state, OperationStateKind::kInterventionRequired);
    const auto stopped = kernel.stop(program_run_stop_request("request.racing.stop", "run.1"));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    EXPECT_NE(stopped.value().resolution_disposition,
              StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
    ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
    EXPECT_EQ(stopped.value().affected_operation_revisions.front().operation_id,
              operation.value().operation_id);
    held.Release();
    EXPECT_TRUE(failing.get().ok());
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));
    EXPECT_EQ(first_engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(second_engine.dispatch_prepared_motion_calls, 0);
  }
  {
    SCOPED_TRACE("dispatched work later held for intervention");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto operation = kernel.execute(motion_operation("request.uncertain.dispatched"));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kInterventionRequired);
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(operation.value().operation_id);
      return state.ok() && state.value().state == OperationStateKind::kInterventionRequired;
    }));
    const auto stopped = kernel.stop(program_run_stop_request("request.uncertain.stop", "run.1"));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    EXPECT_NE(stopped.value().resolution_disposition,
              StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
    ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
    ASSERT_TRUE(wait_until([&] { return engine.translate_stop_stage_calls.load() >= 1; }));
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramRunStopClosesKnownIdleRunUntilExactSuccessfulRelease) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  const auto claim = kernel.AcquireCellAdmissionClaim(CellAdmissionClaimIntentDTO{
      CellAdmissionClaimKindDTO::kProgramCycle,
      CellAdmissionOwnerRoleDTO::kProgramRunner,
      RequestOwnerEpoch("program.owner"),
      "run.1",
      RequestId("request.program-cycle.claim"),
      ContentDigest("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
      kernel.GetAuthoringAdmission().admission_generation,
      {},
      ProgramCycleAdmissionEvidenceDTO{ProgramRevision(test_digest('0')),
                                       ProgramRevision(test_digest('0'))},
      std::nullopt});
  ASSERT_TRUE(claim.ok()) << claim.error().message;

  const auto stopped =
      kernel.stop(program_run_stop_request("request.program-run.before-execute", "run.1"));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  // Capture may complete after this Stop, but registration cannot reopen it.
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  EXPECT_EQ(stopped.value().resolution_disposition,
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_TRUE(stopped.value().affected_operation_revisions.empty());

  const auto rejected = kernel.execute(motion_operation("request.program-run.late-execute"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "program_run_stop_fence_active");
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  const auto stale_release = kernel.release_program_run_stop_fence(
      ProgramRunId("run.1"), StopId("stop.stale"), stopped.value().stop_generation);
  ASSERT_FALSE(stale_release.ok());
  EXPECT_EQ(stale_release.error().details.at("reason"), "program_run_stop_fence_stale");

  // A new Stop intent is a new admission fence, even when the same physical
  // rest needs no new actuation. An older Resume cannot erase it.
  const auto newest =
      kernel.stop(program_run_stop_request("request.program-run.newer-stop", "run.1"));
  ASSERT_TRUE(newest.ok()) << newest.error().message;
  EXPECT_NE(newest.value().stop_id, stopped.value().stop_id);
  EXPECT_EQ(newest.value().resolution_disposition,
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_EQ(engine.stop_calls.load(), 0);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(newest.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  EXPECT_FALSE(kernel
                   .release_program_run_stop_fence(ProgramRunId("run.1"), stopped.value().stop_id,
                                                   stopped.value().stop_generation)
                   .ok());
  EXPECT_FALSE(kernel.execute(motion_operation("request.program-run.still-fenced")).ok());
  const auto released = kernel.release_program_run_stop_fence(
      ProgramRunId("run.1"), newest.value().stop_id, newest.value().stop_generation);
  ASSERT_TRUE(released.ok()) << released.error().message;
  // Releasing the Stop resumes execution only: the never-started node is
  // still Ready and needs the explicit initial Run.
  const auto still_ready =
      kernel.execute(scoped_motion_operation("request.program-run.after-release"));
  ASSERT_FALSE(still_ready.ok());
  EXPECT_EQ(still_ready.error().details.at("reason"), "program_node_stop_fence_active");
  start_program_motion(kernel);
  const auto admitted =
      kernel.execute(scoped_motion_operation("request.program-run.after-run"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest, ProgramHaltKeepsReadyWorkReadyAfterRelease) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());

  // Execute, then Halt before Run.
  const auto halted = kernel.stop(program_run_stop_request("request.halt.ready", "run.1"));
  ASSERT_TRUE(halted.ok()) << halted.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(halted.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto during = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(during.ok()) << during.error().message;
  ASSERT_EQ(during.value().held_nodes.size(), 1U);
  EXPECT_EQ(during.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kReady);

  // Neither Run nor Resume ends the Halt; only its release does.
  const auto resume_while_halted = kernel.release_program_selection(
      program_selection_request("resume.while.halted", "arm"), during.value().selection_revision);
  ASSERT_FALSE(resume_while_halted.ok());
  EXPECT_EQ(resume_while_halted.error().details.at("reason"), "program_selection_not_releasable");

  ASSERT_TRUE(kernel
                  .release_program_run_stop_fence(ProgramRunId("run.1"), halted.value().stop_id,
                                                  halted.value().stop_generation)
                  .ok());
  const auto after = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_EQ(after.value().held_nodes.size(), 1U);
  EXPECT_EQ(after.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kReady);
  const auto before_run = kernel.execute(scoped_motion_operation("halt.ready.before.run"));
  ASSERT_FALSE(before_run.ok());
  EXPECT_EQ(before_run.error().details.at("reason"), "program_node_stop_fence_active");
  EXPECT_EQ(engine.execute_calls, 0);

  start_program_motion(kernel);
  const auto admitted = kernel.execute(scoped_motion_operation("halt.ready.after.run"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest, ProgramHaltStopsRunningWorkUntilResumeAfterRelease) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  // Execute and Run: the motion node is Running (between operations here).
  start_program_motion(kernel);

  const auto halted = kernel.stop(program_run_stop_request("request.halt.running", "run.1"));
  ASSERT_TRUE(halted.ok()) << halted.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(halted.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  ASSERT_TRUE(kernel
                  .release_program_run_stop_fence(ProgramRunId("run.1"), halted.value().stop_id,
                                                  halted.value().stop_generation)
                  .ok());

  // Execute after Halt resumed execution, not motion: the node is Stopped.
  const auto after = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_EQ(after.value().held_nodes.size(), 1U);
  EXPECT_EQ(after.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kHeld);
  const auto before_resume = kernel.execute(scoped_motion_operation("halt.running.before.resume"));
  ASSERT_FALSE(before_resume.ok());
  EXPECT_EQ(before_resume.error().details.at("reason"), "program_node_stop_fence_active");

  const auto resumed = kernel.release_program_selection(
      program_selection_request("resume.after.halt", "arm"), after.value().selection_revision);
  ASSERT_TRUE(resumed.ok()) << resumed.error().message;
  EXPECT_TRUE(resumed.value().held_nodes.empty());
  const auto admitted = kernel.execute(scoped_motion_operation("halt.running.after.resume"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest, ProgramResumeReleasesMixedStoppedAndReadyWorkAtOnce) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  // Canonical order: move (0), place (1), timer (2).
  table->nodes.insert(table->nodes.begin() + 1,
                      {ProgramNodeInstanceId("place"),
                       "kernel.operation",
                       {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"),
                         true}}});
  table->named_selections = {{"arm", {0U}}, {"place", {1U}}, {"supervision", {2U}}};
  capture.table = table;
  const auto registered = kernel.register_program_run_capture(std::move(capture));
  ASSERT_TRUE(registered.ok()) << registered.error().message;

  // Run only "arm", then Halt and Execute: "move" is Stopped, "place" never
  // started and stays Ready.
  start_program_motion(kernel, "arm");
  const auto halted = kernel.stop(program_run_stop_request("request.halt.mixed", "run.1"));
  ASSERT_TRUE(halted.ok()) << halted.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(halted.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  ASSERT_TRUE(kernel
                  .release_program_run_stop_fence(ProgramRunId("run.1"), halted.value().stop_id,
                                                  halted.value().stop_generation)
                  .ok());
  const auto mixed = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(mixed.ok()) << mixed.error().message;
  ASSERT_EQ(mixed.value().held_nodes.size(), 2U);
  for (const auto& node : mixed.value().held_nodes) {
    // Node 0 is "move", node 1 is "place".
    EXPECT_EQ(node.state, node.node_index == 0U ? ProgramNodeHoldStateDTO::kHeld
                                                : ProgramNodeHoldStateDTO::kReady);
  }

  // Run and Resume are one release: both kinds of work continue together.
  const auto released = kernel.release_program_selection(
      program_selection_request("resume.mixed", "all"), mixed.value().selection_revision);
  ASSERT_TRUE(released.ok()) << released.error().message;
  EXPECT_TRUE(released.value().held_nodes.empty());
  const auto admitted = kernel.execute(scoped_motion_operation("mixed.after.resume"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest, ProgramSceneClaimOrdersAgainstStopWithoutDelayingIt) {
  using namespace std::chrono_literals;
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  config.cell_admission_settlement_validator = [](const auto&, const auto&) {
    return Result<void>::Ok();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, std::move(config));
  const auto parent = claim_program_execution(kernel);
  ASSERT_TRUE(parent.ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  table->nodes.front().owner_api = "scene.carried_attachment";
  capture.table = table;
  CellAdmissionClaimIntentDTO intent{
      CellAdmissionClaimKindDTO::kAuthoredScene,
      CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner,
      RequestOwnerEpoch("scene.owner"),
      "scene.transaction",
      RequestId("scene.claim"),
      ContentDigest(test_digest('0')),
      kernel.GetAuthoringAdmission().admission_generation,
      {},
      {},
      {},
      ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("program.owner"), ProgramRunId("run.1"),
                                      parent.value().claim_generation, "move", "move.attempt",
                                      "scene.carried_attachment"}};
  // First tick can precede Manager's first Starting/Running publication.
  // Current Idle alone must never authorize a child without its sealed capture.
  EXPECT_EQ(kernel.AcquireCellAdmissionClaim(intent).error().details.at("reason"),
            "program_node_parent_not_current");
  ASSERT_TRUE(kernel.register_program_run_capture(std::move(capture)).ok());
  start_program_motion(kernel);
  const CellAdmissionReadinessSnapshotDTO idle{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"),
                            1U,
                            ProgramCycleStateDTO::kIdleAfterReset,
                            {},
                            {},
                            {}},
      true,
      1U,
      1U,
      true,
      true,
      0U,
      0U,
      0U};
  for (int failure = 0; failure != 7; ++failure) {
    auto unavailable = idle;
    switch (failure) {
      case 0:
        unavailable.program_cycle_status_current = false;
        break;
      case 1:
        unavailable.program_cycle_status->program_runner_owner_epoch = RequestOwnerEpoch("foreign");
        break;
      case 2:
        unavailable.deployment_status_current = false;
        break;
      case 3:
        unavailable.blocking_fault_count = 1U;
        break;
      default:
        unavailable.program_cycle_status->state =
            failure == 4   ? ProgramCycleStateDTO::kStopping
            : failure == 5 ? ProgramCycleStateDTO::kResetting
                           : ProgramCycleStateDTO::kTerminalAwaitingReset;
        unavailable.program_cycle_status->program_run_id = ProgramRunId("run.1");
        unavailable.program_cycle_status->cycle_claim_generation = parent.value().claim_generation;
        unavailable.program_cycle_status->terminal_or_settlement_reference =
            ContentDigest(test_digest('0'));
    }
    ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(unavailable).ok());
    intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
    EXPECT_FALSE(kernel.AcquireCellAdmissionClaim(intent).ok()) << failure;
  }
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(idle).ok());
  intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
  auto wrong_owner = intent;
  wrong_owner.program_node_evidence->owner_api = "scene.transaction";
  EXPECT_EQ(kernel.AcquireCellAdmissionClaim(wrong_owner).error().details.at("reason"),
            "program_execution_scope_unknown");
  auto stopped = kernel.stop_program_selection(program_selection_request("stop.before", "arm"));
  ASSERT_TRUE(stopped.ok());
  EXPECT_EQ(kernel.AcquireCellAdmissionClaim(intent).error().details.at("reason"),
            "program_node_stop_fence_active");
  ASSERT_TRUE(kernel
                  .release_program_selection(program_selection_request("resume.before", "arm"),
                                             stopped.value().state->selection_revision)
                  .ok());

  HeldSettlementCall held_write;
  journal.cell_admission_acquire_hook = held_write.Blocker();
  auto acquiring =
      std::async(std::launch::async, [&] { return kernel.AcquireCellAdmissionClaim(intent); });
  const bool entered = held_write.Entered();
  if (!entered) held_write.Release();
  ASSERT_TRUE(entered);
  auto stopping = std::async(std::launch::async, [&] {
    return kernel.stop_program_selection(program_selection_request("stop.crossed", "arm"));
  });
  const bool stop_returned = stopping.wait_for(250ms) == std::future_status::ready;
  if (!stop_returned) held_write.Release();
  stopped = stopping.get();
  EXPECT_TRUE(stop_returned);  // A barrier assertion, not a real-time platform qualification.
  ASSERT_TRUE(stopped.ok());
  const auto while_writing =
      kernel.release_program_selection(program_selection_request("resume.crossed", "arm"),
                                       stopped.value().state->selection_revision);
  held_write.Release();
  const auto child = acquiring.get();
  ASSERT_TRUE(child.ok()) << child.error().message;
  ASSERT_FALSE(while_writing.ok());
  EXPECT_EQ(while_writing.error().details.at("reason"), "program_run_admission_in_flight");
  EXPECT_EQ(kernel.AcquireCellAdmissionClaim(intent).value(), child.value());
  EXPECT_EQ(kernel
                .release_program_selection(program_selection_request("resume.held", "arm"),
                                           stopped.value().state->selection_revision)
                .error()
                .details.at("reason"),
            "program_children_unsettled");
  const auto& value = child.value();
  ASSERT_TRUE(kernel
                  .ReleaseCellAdmissionClaim({value.intent.owner_role,
                                              value.intent.owner_epoch,
                                              value.intent.owner_transaction_id,
                                              value.intent.request_id,
                                              value.intent.request_payload_digest,
                                              value.acquired_admission_generation,
                                              value.claim_generation,
                                              {CellAdmissionSettlementKindDTO::kOwnerCommit,
                                               ContentDigest(test_digest('0'))}})
                  .ok());
  EXPECT_TRUE(kernel
                  .release_program_selection(program_selection_request("resume.settled", "arm"),
                                             stopped.value().state->selection_revision)
                  .ok());

  // Reset cleanup must retain the whole-run fence even while Manager's
  // readiness still says Running. Retiring a settled child is not permission
  // to start another one before scene restoration and parent-claim release.
  auto running = idle;
  running.program_cycle_status->state = ProgramCycleStateDTO::kRunning;
  running.program_cycle_status->program_run_id = ProgramRunId("run.1");
  running.program_cycle_status->cycle_claim_generation = parent.value().claim_generation;
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(running).ok());
  journal.cell_admission_acquire_hook = {};
  intent.owner_transaction_id = "scene.before-reset";
  intent.request_id = RequestId("scene.before-reset.claim");
  intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
  const auto before_reset = kernel.AcquireCellAdmissionClaim(intent);
  ASSERT_TRUE(before_reset.ok()) << before_reset.error().message;
  const auto reset_stop = kernel.stop(program_run_stop_request("reset.containment", "run.1"));
  ASSERT_TRUE(reset_stop.ok()) << reset_stop.error().message;
  const auto& pending = before_reset.value();
  ASSERT_TRUE(kernel
                  .ReleaseCellAdmissionClaim({pending.intent.owner_role,
                                              pending.intent.owner_epoch,
                                              pending.intent.owner_transaction_id,
                                              pending.intent.request_id,
                                              pending.intent.request_payload_digest,
                                              pending.acquired_admission_generation,
                                              pending.claim_generation,
                                              {CellAdmissionSettlementKindDTO::kOwnerCommit,
                                               ContentDigest(test_digest('0'))}})
                  .ok());
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(reset_stop.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  intent.owner_transaction_id = "scene.after-reset-cleanup";
  intent.request_id = RequestId("scene.after-reset-cleanup.claim");
  intent.program_node_evidence->node_attempt_id = "move.new-attempt";
  intent.expected_admission_generation = kernel.GetAuthoringAdmission().admission_generation;
  const auto late_child = kernel.AcquireCellAdmissionClaim(intent);
  ASSERT_FALSE(late_child.ok());
  EXPECT_EQ(late_child.error().details.at("reason"), "program_node_stop_fence_active");
}

TEST(ResourceAddressedExecutionKernelTest, ProgramSelectionIdleOverlapHasOneHoldAndNoPhysicalStop) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  const auto arm = kernel.stop_program_selection(program_selection_request("stop.arm", "arm"));
  ASSERT_TRUE(arm.ok()) << arm.error().message;
  ASSERT_TRUE(arm.value().state.has_value());
  EXPECT_FALSE(arm.value().fresh_containment.has_value());
  EXPECT_EQ(arm.value().state->held_nodes.size(), 1U);
  EXPECT_FALSE(kernel.execute(scoped_motion_operation("late.arm")).ok());
  const auto all = kernel.stop_program_selection(program_selection_request("stop.all", "all"));
  ASSERT_TRUE(all.ok()) << all.error().message;
  ASSERT_TRUE(all.value().state.has_value());
  EXPECT_FALSE(all.value().fresh_containment.has_value());
  EXPECT_EQ(all.value().state->held_nodes.size(), 2U);
  EXPECT_FALSE(kernel
                   .release_program_selection(program_selection_request("old.resume", "arm"),
                                              arm.value().state->selection_revision)
                   .ok());
  const auto released = kernel.release_program_selection(
      program_selection_request("resume.arm", "arm"), all.value().state->selection_revision);
  ASSERT_TRUE(released.ok()) << released.error().message;
  ASSERT_EQ(released.value().held_nodes.size(), 1U);
  EXPECT_EQ(released.value().held_nodes.front().node_index, 1U);
  EXPECT_EQ(released.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kHeld);
  const auto replay = kernel.stop_program_selection(program_selection_request("stop.arm", "arm"));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().state->selection_revision, released.value().selection_revision);
  ASSERT_EQ(replay.value().state->held_nodes.size(), 1U);
  EXPECT_EQ(replay.value().state->held_nodes.front().node_index, 1U);
  // The overlapping `all` selection has no remaining independent lock on arm.
  const auto motion = kernel.execute(scoped_motion_operation("fresh.arm"));
  ASSERT_TRUE(motion.ok()) << motion.error().message;
  const auto logical = kernel.release_program_selection(
      program_selection_request("resume.remaining", "all"), released.value().selection_revision);
  ASSERT_TRUE(logical.ok()) << logical.error().message;
  EXPECT_TRUE(logical.value().held_nodes.empty());
  EXPECT_TRUE(kernel.get_operation_projection(motion.value().operation_id).value().resident);
  EXPECT_EQ(engine.stop_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest, ProgramSelectionUnknownContainsThroughExistingFullStop) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  ASSERT_TRUE(kernel.execute(scoped_motion_operation("active.arm")).ok());
  const auto stopped =
      kernel.stop_program_selection(program_selection_request("unknown.stop", "missing"));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(stopped.value().fresh_containment.has_value());
  EXPECT_FALSE(stopped.value().state.has_value());
  EXPECT_TRUE(stopped.value().selection_error.has_value());
  EXPECT_EQ(stopped.value().fresh_containment->resolution_disposition,
            StopResolutionDispositionDTO::kPromotedToFull);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().fresh_containment->stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  EXPECT_EQ(engine.stop_calls.load(), 1);
  EXPECT_FALSE(kernel.execute(scoped_motion_operation("late.after.unknown.stop")).ok());
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramSelectionAdoptsStopAndContainsOnlyFreshIndependentWork) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  const auto base =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 128U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(coordinated_occurrence(base, "a"), first_engine),
       registration(coordinated_occurrence(base, "b"), second_engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("a"),
                   "kernel.operation",
                   {{TargetRoleId("arm_a"), CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("b"),
                   "kernel.operation",
                   {{TargetRoleId("arm_b"), CapabilityId("execution.trajectory.execute"), true}}}};
  table->named_selections = {{"a", {0U}}, {"b", {1U}}};
  capture.table = table;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);
  std::vector<OperationAcceptedDTO> operations;
  for (const auto* name : {"a", "b"}) {
    auto intent = motion_operation(std::string("execute.") + name);
    auto& participant = intent.participants.front();
    participant.component_role_id = ComponentRoleId(std::string("robot_") + name);
    participant.target_role_id = TargetRoleId(std::string("arm_") + name);
    participant.execution_scope =
        ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId(name), "attempt.1"};
    const auto admitted = kernel.execute(intent);
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    operations.push_back(admitted.value());
  }
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  HeldSettlementCall stage;
  first_engine.translate_stop_stage_hook = stage.Blocker();
  const auto first = kernel.stop_program_selection(program_selection_request("stop.a", "a"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(first.value().fresh_containment.has_value());
  ASSERT_TRUE(stage.Entered());
  EXPECT_EQ(second_engine.stop_calls.load(), 0);
  const auto both = kernel.stop_program_selection(program_selection_request("stop.all", "all"));
  ASSERT_TRUE(both.ok()) << both.error().message;
  ASSERT_TRUE(both.value().fresh_containment.has_value());
  EXPECT_EQ(both.value().fresh_containment->resolution_disposition,
            StopResolutionDispositionDTO::kExact);
  EXPECT_NE(first.value().fresh_containment->stop_id, both.value().fresh_containment->stop_id);
  ASSERT_TRUE(both.value().state.has_value());
  EXPECT_FALSE(kernel
                   .release_program_selection(program_selection_request("early.resume", "all"),
                                              both.value().state->selection_revision)
                   .ok());
  stage.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_program_execution_scope(ProgramRunId("run.1"));
    return state.ok() && state.value().held_nodes.size() == 2U &&
           std::all_of(state.value().held_nodes.begin(), state.value().held_nodes.end(),
                       [](const auto& row) { return row.state == ProgramNodeHoldStateDTO::kHeld; });
  }));
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
  EXPECT_TRUE(
      kernel.get_program_execution_scope(ProgramRunId("run.1")).value().pending_effects.empty());
  const auto first_status = kernel.get_stop_status(first.value().fresh_containment->stop_id);
  ASSERT_TRUE(first_status.ok());
  ASSERT_TRUE(first_status.value().stop_certificate.has_value());
  const auto certificate_id = first_status.value().stop_certificate->stop_certificate_id;
  // A stays held while unrelated, already-settled B receives further Stops.
  // Rotate the 64-entry diagnostic history without issuing physical commands.
  const auto second_binding = coordinated_occurrence(base, "b").value();
  for (int index = 0; index < 65; ++index) {
    const auto idle_stop = kernel.stop(
        StopIntentDTO{{RequestId("history.rotate." + std::to_string(index)),
                       RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
                      std::vector<StopComponentTargetDTO>{{second_binding.controllable_component_id,
                                                           second_binding.graph_revision,
                                                           ComponentLifecycleGeneration(1U)}},
                      std::nullopt,
                      {}});
    ASSERT_TRUE(idle_stop.ok()) << idle_stop.error().message;
    ASSERT_EQ(idle_stop.value().resolution_disposition,
              StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(idle_stop.value().stop_id);
      return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded &&
             status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
    }));
  }
  EXPECT_FALSE(kernel.get_stop_certificate(certificate_id).ok());
  const auto repeated =
      kernel.stop_program_selection(program_selection_request("stop.a.again", "a"));
  ASSERT_TRUE(repeated.ok()) << repeated.error().message;
  EXPECT_FALSE(repeated.value().fresh_containment.has_value());
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
  const auto state = kernel.get_operation_state(operations.front().operation_id);
  ASSERT_TRUE(state.ok());
  const auto reset = kernel.reset(control_request("history.reset.a", operations.front(),
                                                  state.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto projection = kernel.get_operation_projection(operations.front().operation_id);
    return projection.ok() && !projection.value().resident;
  }));
  const auto released =
      kernel.release_program_selection(program_selection_request("history.release.a", "a"),
                                       repeated.value().state->selection_revision);
  ASSERT_TRUE(released.ok()) << released.error().message;
  ASSERT_EQ(released.value().held_nodes.size(), 1U);
  EXPECT_EQ(released.value().held_nodes.front().node_index, 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramGroupUnionIsAcceptedOnceAndDeduplicatesOverlappingSelections) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  auto request = program_selection_request("stop.union", "arm");
  request.group_ids = {"arm", "supervision", "arm"};
  const auto stopped = kernel.stop_program_selection(request);
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_FALSE(stopped.value().selection_error);
  EXPECT_EQ(stopped.value().selected_node_indices, (std::vector<std::uint32_t>{0U, 1U}));
  EXPECT_TRUE(stopped.value().widened_node_indices.empty());
  EXPECT_FALSE(stopped.value().fresh_containment);
  ASSERT_TRUE(stopped.value().state);
  EXPECT_EQ(stopped.value().state->held_nodes.size(), 2U);
  const auto replay = kernel.stop_program_selection(request);
  ASSERT_TRUE(replay.ok());
  ASSERT_TRUE(replay.value().state);
  EXPECT_EQ(replay.value().state->selection_revision, stopped.value().state->selection_revision);
  EXPECT_EQ(engine.stop_calls.load(), 0);
  auto release = request;
  release.request_context.request_id = RequestId("release.union");
  const auto resumed =
      kernel.release_program_selection(release, stopped.value().state->selection_revision);
  ASSERT_TRUE(resumed.ok()) << resumed.error().message;
  EXPECT_TRUE(resumed.value().held_nodes.empty());
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramMemberStopContainsCoordinatedPeersButNotIndependentRobot) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine, independent_engine;
  const auto base =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(base, first_engine),
       registration(coordinated_occurrence(base, "2"), second_engine),
       registration(coordinated_occurrence(base, "3"), independent_engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("a"),
                   "kernel.operation",
                   {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("b"),
                   "kernel.operation",
                   {{TargetRoleId("arm_2"), CapabilityId("execution.trajectory.execute"), true}}},
                  {ProgramNodeInstanceId("c"),
                   "kernel.operation",
                   {{TargetRoleId("arm_3"), CapabilityId("execution.trajectory.execute"), true}}}};
  table->named_selections = {
      {"a", {0U}}, {"both_arms", {0U, 1U}, ProgramExecutionPolicy::kCommonStart}, {"c", {2U}}};
  capture.table = table;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);
  auto incomplete = motion_operation("execute.synchronized.member.alone");
  incomplete.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("a"), "attempt.alone"};
  const auto incomplete_result = kernel.execute(incomplete);
  ASSERT_FALSE(incomplete_result.ok());
  EXPECT_EQ(incomplete_result.error().details.at("reason"),
            "program_execution_synchronization_required");
  EXPECT_EQ(first_engine.prepare_motion_calls, 0);
  auto coordinated = coordinated_motion_operation("execute.synchronized");
  coordinated.participants[0].execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("a"), "attempt.1"};
  coordinated.participants[1].execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("b"), "attempt.1"};
  const auto admitted = kernel.execute(coordinated);
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  auto independent = motion_operation("execute.independent");
  independent.participants.front().component_role_id = ComponentRoleId("robot_3");
  independent.participants.front().target_role_id = TargetRoleId("arm_3");
  independent.participants.front().execution_scope =
      ProgramNodeExecutionScopeDTO{ProgramNodeInstanceId("c"), "attempt.1"};
  const auto other = kernel.execute(independent);
  ASSERT_TRUE(other.ok()) << other.error().message;
  for (auto* engine : {&first_engine, &second_engine, &independent_engine})
    engine->emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  HeldSettlementCall second_settlement;
  second_engine.translate_stop_stage_hook = second_settlement.Blocker();
  const auto stopped = kernel.stop_program_selection(program_selection_request("stop.a", "a"));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(stopped.value().fresh_containment);
  EXPECT_EQ(stopped.value().selected_node_indices, std::vector<std::uint32_t>{0U});
  EXPECT_EQ(stopped.value().widened_node_indices, std::vector<std::uint32_t>{1U});
  ASSERT_TRUE(second_settlement.Entered());
  ASSERT_TRUE(wait_until([&] { return first_engine.stop_calls.load() == 1; }));
  EXPECT_EQ(independent_engine.stop_calls.load(), 0);
  const auto pending = kernel.get_stop_status(stopped.value().fresh_containment->stop_id);
  ASSERT_TRUE(pending.ok());
  EXPECT_NE(pending.value().state, StopStatusStateDTO::kSucceeded);

  // An overlapping selector adopts the same containment, not another Stop.
  const auto overlap =
      kernel.stop_program_selection(program_selection_request("stop.both", "both_arms"));
  ASSERT_TRUE(overlap.ok()) << overlap.error().message;
  EXPECT_FALSE(overlap.value().fresh_containment);
  second_settlement.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().fresh_containment->stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto scope = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(scope.ok());
  ASSERT_EQ(scope.value().held_nodes.size(), 2U);
  for (const auto& node : scope.value().held_nodes) {
    EXPECT_LT(node.node_index, 2U);
    EXPECT_EQ(node.state, ProgramNodeHoldStateDTO::kHeld);
  }
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
  EXPECT_EQ(independent_engine.stop_calls.load(), 0);
  const auto other_state = kernel.get_operation_state(other.value().operation_id);
  ASSERT_TRUE(other_state.ok());
  EXPECT_EQ(other_state.value().state, OperationStateKind::kExecuting);
}

TEST(ResourceAddressedExecutionKernelTest,
     ManualHandoverFindsEveryProducerButNotQueriesOrOtherRobot) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine arm;
  FakeParticipantEngine tool;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), arm);
  direct.intrinsic_direct_control = true;
  const auto tool_binding =
      make_binding("gripper", "gripper", "tool.gripper.command", CoordinateKind::kLinear);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct, registration(tool_binding, tool)}, clock,
                                          scheduler, journal, config);
  auto manual = motion_operation("manual.inspect");
  manual.program_run_id = ProgramRunId("manual.run");
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  const auto idle = kernel.get_manual_program_handover(manual);
  ASSERT_TRUE(idle.ok());
  EXPECT_FALSE(idle.value());
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  auto alias = binding->target_role_bindings.front();
  alias.target_role_id = TargetRoleId("arm_alias");
  binding->target_role_bindings.push_back(alias);
  auto gripper = alias;
  gripper.target_role_id = tool_binding.value().target_role_id;
  gripper.component_role_id = tool_binding.value().component_role_id;
  gripper.controllable_component_id = tool_binding.value().controllable_component_id;
  gripper.command_target_id = tool_binding.value().command_target_id;
  binding->target_role_bindings.push_back(gripper);
  capture.binding = binding;
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {
      {ProgramNodeInstanceId("move"),
       "kernel.operation",
       {{TargetRoleId("arm"), CapabilityId("execution.trajectory.execute"), true}}},
      {ProgramNodeInstanceId("read"),
       "kernel.command_target",
       {{TargetRoleId("arm"), CapabilityId("state.observed.current")}}},
      {ProgramNodeInstanceId("second_move"),
       "kernel.operation",
       {{TargetRoleId("arm_alias"), CapabilityId("execution.trajectory.execute"), true}}},
      {ProgramNodeInstanceId("sleep"), "orchestration.clock", {}},
      {ProgramNodeInstanceId("tool"),
       "kernel.command_target",
       {{TargetRoleId("gripper"), CapabilityId("tool.gripper.command"), true}}}};
  table->named_selections = {
      {"arm_first", {0U}}, {"arm_producers", {0U, 2U}}, {"supervision", {1U, 3U}}};
  capture.table = table;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);
  EXPECT_FALSE(kernel.get_manual_program_handover(manual).ok());
  ASSERT_TRUE(
      kernel.stop_program_selection(program_selection_request("hold.first", "arm_first")).ok());
  EXPECT_FALSE(kernel.get_manual_program_handover(manual).ok());
  ASSERT_TRUE(
      kernel.stop_program_selection(program_selection_request("hold.producers", "arm_producers"))
          .ok());
  const auto held = kernel.get_manual_program_handover(manual);
  ASSERT_TRUE(held.ok()) << held.error().message;
  ASSERT_TRUE(held.value());
  EXPECT_EQ(held.value()->producer_node_indices, (std::vector<std::uint32_t>{0U, 2U}));
  EXPECT_EQ(held.value()->capture.table, capture.table);
  EXPECT_EQ(held.value()->capture.binding, capture.binding);
  const auto scope = kernel.get_program_execution_scope(capture.program_run_id);
  ASSERT_TRUE(scope.ok());
  EXPECT_EQ(scope.value().held_nodes.size(), 2U);  // query/tool branches remain enabled
  EXPECT_EQ(arm.prepare_motion_calls, 0);
  EXPECT_EQ(tool.prepare_motion_calls, 0);
  EXPECT_EQ(arm.execute_calls, 0);
  EXPECT_EQ(tool.execute_calls, 0);
  ASSERT_TRUE(
      kernel.stop_program_selection(program_selection_request("hold.new", "supervision")).ok());
  const auto newer = kernel.get_manual_program_handover(manual);
  ASSERT_TRUE(newer.ok());
  ASSERT_TRUE(newer.value());
  EXPECT_GT(newer.value()->selection_revision, held.value()->selection_revision);
}

TEST(ResourceAddressedExecutionKernelTest, ManualUnusedTargetAcceptsInitialSelectionRevision) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine arm;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), arm);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>();
  table->nodes = {{ProgramNodeInstanceId("read"),
                   "kernel.command_target",
                   {{TargetRoleId("arm"), CapabilityId("state.observed.current")}}}};
  capture.table = table;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  auto manual = motion_operation("manual.unused.target");
  manual.program_run_id = ProgramRunId("manual.run");
  const auto proof = kernel.get_manual_program_handover(manual);
  ASSERT_TRUE(proof.ok()) << proof.error().message;
  ASSERT_TRUE(proof.value());
  EXPECT_EQ(proof.value()->selection_revision, 0U);
  EXPECT_TRUE(proof.value()->producer_node_indices.empty());
  manual.manual_handover = ManualProgramHandoverContextDTO{
      capture.program_runner_owner_epoch, capture.program_run_id, capture.claim_generation, 0U};
  EXPECT_TRUE(validate_operation_intent(manual).ok());
  manual.manual_handover->selection_revision = 9007199254740992ULL;
  EXPECT_FALSE(validate_operation_intent(manual).ok());
  EXPECT_EQ(arm.prepare_motion_calls, 0);
  EXPECT_EQ(arm.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, ManualAvailabilityDistinguishesSettledCleanupFromBusy) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto alias = same_control_scope_program_alias(physical);
  auto direct = registration(alias, engine);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({registration(physical, engine), direct}, clock,
                                          scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  auto manual = motion_operation("manual.availability");
  manual.program_id = alias.value().program_id;
  manual.program_revision = alias.value().program_revision;
  manual.program_binding_id = alias.value().program_binding_id;
  manual.program_run_id = ProgramRunId("manual.run");
  manual.participants.front().component_role_id = alias.value().component_role_id;
  manual.participants.front().target_role_id = alias.value().target_role_id;
  const auto idle = kernel.get_manual_control_availability(manual);
  ASSERT_TRUE(idle.ok());
  EXPECT_TRUE(idle.value().available);
  EXPECT_FALSE(idle.value().handover);
  const auto parent = claim_program_execution(kernel);
  ASSERT_TRUE(parent.ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  ASSERT_TRUE(kernel
                  .UpdateCellAdmissionReadiness(
                      {true,
                       ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                                             ProgramCycleStateDTO::kRunning, ProgramRunId("run.1"),
                                             parent.value().claim_generation, std::nullopt},
                       true, 1U, 1U, true, true, 0U, 0U, 0U})
                  .ok());
  const auto active = kernel.execute(scoped_motion_operation("program.active"));
  ASSERT_TRUE(active.ok()) << active.error().message;
  ASSERT_TRUE(wait_until([&] { return engine.execute_calls == 1; }));
  EXPECT_FALSE(kernel.get_manual_control_availability(manual).value().available);
  const auto stopped =
      kernel.stop_program_selection(program_selection_request("manual.hold", "arm"));
  ASSERT_TRUE(stopped.ok());
  ASSERT_TRUE(stopped.value().fresh_containment);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().fresh_containment->stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  ASSERT_TRUE(wait_until([&] {
    const auto value = kernel.get_manual_control_availability(manual);
    return value.ok() && value.value().available && value.value().cleanup_required;
  }));
  EXPECT_EQ(engine.certified_reset_calls, 0);  // The query never cleans up.
  const auto current = kernel.get_operation_state(active.value().operation_id);
  ASSERT_TRUE(current.ok());
  ASSERT_TRUE(kernel
                  .reset(control_request("owner.cleanup", active.value(),
                                         current.value().operation_revision.value()))
                  .ok());
  ASSERT_TRUE(wait_until([&] {
    (void)kernel.reconcile_subordinate_state();
    const auto value = kernel.get_manual_control_availability(manual);
    return value.ok() && value.value().available && !value.value().cleanup_required;
  }));
  const auto scope = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(scope.ok());
  ASSERT_EQ(scope.value().held_nodes.size(), 1U);
  EXPECT_EQ(scope.value().held_nodes.front().node_index, 0U);
  EXPECT_EQ(scope.value().held_nodes.front().state, ProgramNodeHoldStateDTO::kHeld);
}

TEST(ResourceAddressedExecutionKernelTest, HeldManualAdmissionRetainsParentAndStopWinsDispatch) {
  // Clean completion, selected Stop, whole-run Stop, Stop across durable
  // admission, and related/unrelated Stop during preparation share the owner.
  for (const int scenario : {0, 1, 2, 3, 4, 5, 6}) {
    SCOPED_TRACE(scenario);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    auto direct = registration(
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        engine);
    direct.intrinsic_direct_control = true;
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                  CellId("cell.test"), 8U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
    const auto parent = claim_program_execution(kernel);
    ASSERT_TRUE(parent.ok());
    ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
    start_program_motion(kernel);
    const CellAdmissionReadinessSnapshotDTO running{
        true,
        ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 2U,
                              ProgramCycleStateDTO::kRunning, ProgramRunId("run.1"),
                              parent.value().claim_generation, std::nullopt},
        true,
        1U,
        1U,
        true,
        true,
        0U,
        0U,
        0U};
    ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(running).ok());
    auto availability_intent = motion_operation("manual.availability");
    availability_intent.program_run_id = ProgramRunId("manual.run");
    const auto blocked = kernel.get_manual_control_availability(availability_intent);
    ASSERT_TRUE(blocked.ok());
    EXPECT_FALSE(blocked.value().available);
    EXPECT_EQ(blocked.value().unavailable_reason, "manual_handover_producer_not_held");
    const auto stopped =
        kernel.stop_program_selection(program_selection_request("manual.hold", "arm"));
    ASSERT_TRUE(stopped.ok());
    auto manual = motion_operation("manual.held");
    manual.program_run_id = ProgramRunId("manual.run");
    const auto query = kernel.get_manual_program_handover(manual);
    ASSERT_TRUE(query.ok());
    ASSERT_TRUE(query.value());
    manual.manual_handover = ManualProgramHandoverContextDTO{
        RequestOwnerEpoch("program.owner"), ProgramRunId("run.1"), parent.value().claim_generation,
        query.value()->selection_revision};
    const auto available = kernel.get_manual_control_availability(manual);
    ASSERT_TRUE(available.ok());
    EXPECT_TRUE(available.value().available);
    EXPECT_FALSE(available.value().cleanup_required);
    ASSERT_TRUE(available.value().handover);
    EXPECT_EQ(available.value().handover->selection_revision, query.value()->selection_revision);
    auto stale = manual;
    ++stale.manual_handover->selection_revision;
    EXPECT_FALSE(kernel.execute(stale).ok());
    EXPECT_EQ(engine.execute_calls, 0);
    if (scenario == 3) {
      journal.cell_admission_acquire_hook = [&] {
        EXPECT_TRUE(
            kernel.stop_program_selection(program_selection_request("manual.crossing", "arm"))
                .ok());
      };
    }
    if (scenario == 4 || scenario == 5) engine.defer_prepare_completion = true;
    const auto admitted = kernel.execute(manual);
    if (scenario == 3) {
      EXPECT_FALSE(admitted.ok());
      EXPECT_EQ(engine.execute_calls, 0);
      EXPECT_EQ(engine.prepare_motion_calls, 0);
      const auto current_scope = kernel.get_program_execution_scope(ProgramRunId("run.1"));
      ASSERT_TRUE(current_scope.ok());
      ASSERT_TRUE(
          kernel
              .release_program_selection(program_selection_request("manual.aborted.release", "arm"),
                                         current_scope.value().selection_revision)
              .ok());
      continue;
    }
    const auto occupied = kernel.get_manual_control_availability(manual);
    ASSERT_TRUE(occupied.ok());
    EXPECT_FALSE(occupied.value().available);
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    EXPECT_FALSE(
        kernel
            .release_program_selection(program_selection_request("manual.must-not-resume", "arm"),
                                       query.value()->selection_revision)
            .ok());
    const auto replay = kernel.execute(manual);
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
    if (scenario == 0) {
      ASSERT_TRUE(wait_until([&] {
        return engine.execute_calls == 1 && engine.get_status().state == ExecutionState::kPlanning;
      }));
      engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
      ASSERT_TRUE(wait_until([&] {
        (void)kernel.reconcile_subordinate_state();
        return kernel.reconcile_manual_operation_admission().ok() &&
               kernel
                   .release_program_selection(
                       program_selection_request("manual.resume-after-retirement", "arm"),
                       query.value()->selection_revision)
                   .ok();
      }));
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);  // Program claim survives
      EXPECT_EQ(engine.execute_calls, 1);
      continue;
    }
    if (scenario == 6) {
      ASSERT_TRUE(wait_until([&] {
        return engine.execute_calls == 1 && engine.get_status().state == ExecutionState::kPlanning;
      }));
      engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
      const auto unrelated = kernel.stop_program_selection(
          program_selection_request("manual.unrelated", "supervision"));
      ASSERT_TRUE(unrelated.ok());
      EXPECT_FALSE(unrelated.value().fresh_containment);
      const auto state = kernel.get_operation_state(admitted.value().operation_id);
      ASSERT_TRUE(state.ok());
      EXPECT_EQ(state.value().state, OperationStateKind::kExecuting);
      EXPECT_EQ(engine.stop_calls.load(), 0);
      continue;
    }
    if (scenario < 4) {
      ASSERT_TRUE(wait_until([&] { return engine.execute_calls == 1; }));
    }
    if (scenario == 2) {
      const auto full = kernel.stop(program_run_stop_request("manual.full", "run.1"));
      ASSERT_TRUE(full.ok()) << full.error().message;
    } else {
      const auto selected = kernel.stop_program_selection(
          program_selection_request("manual.selected", scenario == 5 ? "supervision" : "arm"));
      ASSERT_TRUE(selected.ok());
      EXPECT_EQ(selected.value().fresh_containment.has_value(), scenario != 5);
    }
    if (scenario >= 4) {
      ASSERT_TRUE(engine.complete_preparation().ok());
      ASSERT_TRUE(wait_until([&] { return engine.cancel_prepared_motion_calls.load() > 0; }));
      EXPECT_EQ(engine.execute_calls, 0);
    } else {
      ASSERT_TRUE(wait_until([&] { return engine.stop_calls.load() == 1; }));
    }
    // The owner lets the Stop that won terminalize before retiring the Kernel:
    // retirement first disconnects participant events, so a Stop still waiting
    // on the cancelled preparation's terminal could never settle.
    std::optional<StopStatusDTO> settled;
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(admitted.value().operation_id);
      if (!state.ok() || !state.value().active_stop_id.has_value()) return false;
      const auto status = kernel.get_stop_status(*state.value().active_stop_id);
      if (!status.ok() || !status.value().stop_certificate.has_value()) return false;
      settled = status.value();
      return true;
    }));
    EXPECT_EQ(settled->state, StopStatusStateDTO::kSucceeded);
    const auto stopped_state = kernel.get_operation_state(admitted.value().operation_id);
    ASSERT_TRUE(stopped_state.ok());
    EXPECT_EQ(stopped_state.value().state, OperationStateKind::kStopped);
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  }
}

TEST(ResourceAddressedExecutionKernelTest, ProgramCaptureIsClaimBoundAndCannotBeRebound) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  EXPECT_FALSE(kernel.register_program_run_capture(program_execution_capture()).ok());
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto stale = program_execution_capture();
  stale.request_owner_epoch = RequestOwnerEpoch("old.kernel");
  EXPECT_FALSE(kernel.register_program_run_capture(stale).ok());
  stale = program_execution_capture();
  stale.program_runner_owner_epoch = RequestOwnerEpoch("old.runner");
  EXPECT_FALSE(kernel.register_program_run_capture(stale).ok());
  stale = program_execution_capture();
  stale.claim_generation = CellAdmissionClaimGeneration(2U);
  EXPECT_FALSE(kernel.register_program_run_capture(stale).ok());
  stale = program_execution_capture();
  stale.binding.reset();
  EXPECT_FALSE(kernel.register_program_run_capture(stale).ok());
  stale = program_execution_capture();
  auto wrong_binding = std::make_shared<ProgramBindingDTO>(*stale.binding);
  wrong_binding->program_binding_id = ProgramBindingId(test_digest('9'));
  stale.binding = wrong_binding;
  EXPECT_FALSE(kernel.register_program_run_capture(stale).ok());
  EXPECT_FALSE(kernel.execute(scoped_motion_operation("before-capture")).ok());
  auto capture = program_execution_capture();
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  EXPECT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  auto altered = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  altered->named_selections.front().node_indices = {0U, 1U};
  capture.table = altered;
  const auto rebound = kernel.register_program_run_capture(capture);
  ASSERT_FALSE(rebound.ok());
  EXPECT_EQ(rebound.error().details.at("reason"), "program_execution_capture_rebound");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramPhysicalEffectRequiresCatalogQualifiedEffectfulAssociation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  auto table = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  table->nodes.front().target_effects.front().effectful = false;
  capture.table = std::move(table);
  ASSERT_TRUE(kernel.register_program_run_capture(std::move(capture)).ok());

  const auto rejected = kernel.execute(scoped_motion_operation("catalog.read-only"));

  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "program_execution_scope_effect_mismatch");
  EXPECT_EQ(engine.prepare_motion_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramMotionContinuationSurvivesFailedPreparationAndClearsOnCompletion) {
  for (const auto action :
       {RecoveryAction::kReplanFromStoppedState, RecoveryAction::kRetryMotionFromCurrentState,
        RecoveryAction::kAcknowledgeAndContinue, RecoveryAction::kRetryAfterSceneRepair}) {
    SCOPED_TRACE(static_cast<int>(action));
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                  CellId("cell.test"), 16U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal, std::move(config));
    ASSERT_TRUE(claim_program_execution(kernel).ok());
    ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
    start_program_motion(kernel);
    const auto first = kernel.execute(scoped_motion_operation("continuation.first"));
    ASSERT_TRUE(first.ok()) << first.error().message;
    ASSERT_TRUE(engine.prepared_snapshot && engine.prepared_snapshot->participant);
    const auto original =
        std::get<PreparedMotionExecutionDTO>(engine.prepared_snapshot->participant->payload)
            .plan_request.motion;
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    const auto stopped =
        kernel.stop_program_selection(program_selection_request("continuation.stop", "arm"));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_TRUE(wait_until([&] {
      const auto scope = kernel.get_program_execution_scope(ProgramRunId("run.1"));
      return scope.ok() && scope.value().pending_effects.empty();
    }));
    // A failed preparation cleared the candidate, not the last actual motion.
    engine.prepared_snapshot->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
    engine.prepared_snapshot->participant.reset();
    const auto recover = [&](const OperationAcceptedDTO& operation, std::string id) {
      const auto state = kernel.get_operation_state(operation.operation_id);
      EXPECT_TRUE(state.ok());
      KernelRecoveryActionRequestDTO request{id, action, "program_runner", 1U, 1U, 41U, "",
                                             "", 0U,     "resume"};
      engine.next_recovery_result = {id, request.action, true, {}, std::nullopt};
      return kernel.apply_operation_recovery_action(operation.operation_id,
                                                    state.value().operation_revision, request);
    };
    const auto recovered = recover(first.value(), "continuation.recover.first");
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_TRUE(recovered.value().accepted);
    const auto scope = kernel.get_program_execution_scope(ProgramRunId("run.1"));
    ASSERT_TRUE(scope.ok());
    ASSERT_TRUE(
        kernel
            .release_program_selection(program_selection_request("continuation.resume", "arm"),
                                       scope.value().selection_revision)
            .ok());
    // The old attempt cannot replay itself under a fresh command identity.
    const auto old_attempt = kernel.execute(scoped_motion_operation("continuation.old-attempt"));
    ASSERT_FALSE(old_attempt.ok());
    EXPECT_EQ(old_attempt.error().details.at("reason"), "program_motion_continuation_mismatch");
    auto next = scoped_motion_operation("continuation.failed-prepare");
    std::get<JointGoalDTO>(
        std::get<MotionParticipantIntentDTO>(next.participants.front().payload).motion)
        .positions_rad = {0.8};
    next.participants.front().execution_scope->node_attempt_id = "move.attempt.2";
    auto crossed_frame = next;
    crossed_frame.request_context.request_id = RequestId("continuation.crossed-frame");
    std::get<MotionParticipantIntentDTO>(crossed_frame.participants.front().payload)
        .base_frame_role_id = FrameRoleId("tcp");
    const auto crossed = kernel.execute(crossed_frame);
    ASSERT_FALSE(crossed.ok());
    EXPECT_EQ(crossed.error().details.at("reason"), "program_motion_continuation_mismatch");
    engine.prepare_call_failure = ErrorDTO{"test", "failed before preparation", {}};
    const auto failed = kernel.execute(next);
    ASSERT_TRUE(failed.ok()) << failed.error().message;
    const auto failed_recovery = recover(failed.value(), "continuation.recover.failed");
    ASSERT_TRUE(failed_recovery.ok()) << failed_recovery.error().message;
    ASSERT_TRUE(failed_recovery.value().accepted);
    engine.prepare_call_failure.reset();
    next.request_context.request_id = RequestId("continuation.next");
    next.participants.front().execution_scope->node_attempt_id = "move.attempt.3";
    const auto resumed = kernel.execute(next);
    ASSERT_TRUE(resumed.ok()) << resumed.error().message;
    ASSERT_TRUE(engine.last_prepared_request && engine.last_prepared_request->continuation);
    EXPECT_EQ(engine.last_prepared_request->continuation->motion, original);
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    const auto current = kernel.get_operation_state(resumed.value().operation_id);
    ASSERT_TRUE(current.ok());
    ASSERT_TRUE(
        kernel
            .acknowledge_completed(control_request("continuation.ack", resumed.value(),
                                                   current.value().operation_revision.value()))
            .ok());
    ASSERT_TRUE(wait_until([&] {
      return !kernel.get_operation_projection(resumed.value().operation_id).value().resident;
    }));
    next.request_context.request_id = RequestId("continuation.new-cycle-visit");
    next.participants.front().execution_scope->node_attempt_id = "move.attempt.4";
    ASSERT_TRUE(kernel.execute(next).ok());
    ASSERT_TRUE(engine.last_prepared_request);
    EXPECT_FALSE(engine.last_prepared_request->continuation);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramContinuationObservationFailureHasNoCleanupEffect) {
  for (const bool throws : {false, true}) {
    SCOPED_TRACE(throws);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                  CellId("cell.test"), 16U};
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal, std::move(config));
    ASSERT_TRUE(claim_program_execution(kernel).ok());
    ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
    start_program_motion(kernel);
    const auto operation = kernel.execute(scoped_motion_operation("capture.failure"));
    ASSERT_TRUE(operation.ok());
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    const auto stop = kernel.stop(program_run_stop_request("capture.stop", "run.1"));
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_until([&] {
      auto result = kernel.get_stop_status(stop.value().stop_id);
      return result.ok() && result.value().state == StopStatusStateDTO::kSucceeded;
    }));
    const auto before = engine.prepared_snapshot;
    if (throws)
      engine.observe_prepared_motion_hook = [] { throw std::runtime_error("observer"); };
    else
      engine.prepared_snapshot->execution_id += 1U;
    const auto state = kernel.get_operation_state(operation.value().operation_id);
    ASSERT_TRUE(state.ok());
    KernelRecoveryActionRequestDTO request{"capture.recover",
                                           RecoveryAction::kReplanFromStoppedState,
                                           "program_runner",
                                           1U,
                                           1U,
                                           41U,
                                           "",
                                           "",
                                           0U,
                                           "resume"};
    engine.next_recovery_result = {request.request_id, request.action, true, {}, std::nullopt};
    const auto rejected = kernel.apply_operation_recovery_action(
        operation.value().operation_id, state.value().operation_revision, request);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "operation_recovery_invalid");
    EXPECT_EQ(engine.recovery_action_calls.load(), 0);
    EXPECT_TRUE(kernel.get_operation_projection(operation.value().operation_id).value().resident);
    engine.observe_prepared_motion_hook = {};
    engine.prepared_snapshot = before;
    const auto retried = kernel.apply_operation_recovery_action(
        operation.value().operation_id, state.value().operation_revision, request);
    ASSERT_TRUE(retried.ok()) << retried.error().message;
    EXPECT_TRUE(retried.value().accepted);
    EXPECT_EQ(engine.recovery_action_calls.load(), 1);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedRecoveryRevisionIgnoresEquivalentObservationSamples) {
  // Participant owners refresh an unchanged recovery assessment in place with
  // each newer observation sample (only the freshness deadline moves). The
  // composed coordinated assessment must keep the revision a recovery request
  // names; only a semantic participant change may advance it.
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto accepted = kernel.execute(coordinated_motion_operation("coordinated.equivalent"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("coordinated.equivalent.stop", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));

  const auto ready = [](FakeParticipantEngine& engine, std::int64_t sample_ns) {
    engine.recovery_assessment.assessment_revision = 3U;
    engine.recovery_assessment.control_command_epoch = 5U;
    engine.recovery_assessment.execution_state = ExecutionState::kStopped;
    engine.recovery_assessment.recovery_class = RecoveryClass::kOrdinaryOperatorStop;
    engine.recovery_assessment.status = RecoveryAssessmentStatus::kRetryReady;
    engine.recovery_assessment.retry_admissible = true;
    engine.recovery_assessment.backend_stop_confirmed = true;
    engine.recovery_assessment.observed_state_available = true;
    engine.recovery_assessment.observed_state_fresh = true;
    engine.recovery_assessment.observed_state_at_rest = true;
    engine.recovery_assessment.allowed_actions = {RecoveryAction::kRetryMotionFromCurrentState};
    engine.recovery_assessment.reason_code = "ordinary_operator_stop_ready";
    engine.recovery_assessment.assessed_at_ns = sample_ns;
    engine.recovery_assessment.observed_state_valid_until_ns = sample_ns + 500'000'000;
  };
  ready(first_engine, 100);
  ready(second_engine, 100);
  const auto initial = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  const auto revision = initial.value().assessment.assessment_revision;
  for (std::int64_t sample = 120; sample <= 200; sample += 20) {
    ready(first_engine, sample);
    ready(second_engine, sample + 5);
    const auto polled = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
    ASSERT_TRUE(polled.ok()) << polled.error().message;
    EXPECT_EQ(polled.value().assessment.assessment_revision, revision);
    EXPECT_EQ(polled.value().assessment.status, RecoveryAssessmentStatus::kRetryReady);
  }

  // A recovery naming that revision is accepted after further equivalent samples.
  ready(first_engine, 240);
  ready(second_engine, 245);
  const auto& composed = initial.value().assessment;
  const KernelRecoveryActionRequestDTO recovery{"coordinated.equivalent.recover",
                                                RecoveryAction::kRetryMotionFromCurrentState,
                                                "operator.test",
                                                revision,
                                                composed.control_command_epoch,
                                                composed.execution_id,
                                                composed.latch_id,
                                                composed.stop_certificate_id,
                                                composed.stop_owner_generation,
                                                "test"};
  first_engine.next_recovery_result =
      KernelRecoveryActionResultDTO{recovery.request_id, recovery.action, true, {}, std::nullopt};
  second_engine.next_recovery_result = first_engine.next_recovery_result;
  const auto recovered = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, initial.value().operation_revision, recovery);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(recovered.value().accepted)
      << (recovered.value().error.has_value() ? recovered.value().error->code : std::string{});
  EXPECT_EQ(first_engine.recovery_action_calls.load(), 1);
  EXPECT_EQ(second_engine.recovery_action_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedRecoveryRevisionAdvancesOnSemanticParticipantChange) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto accepted = kernel.execute(coordinated_motion_operation("coordinated.semantic"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped = kernel.stop(stop_request("coordinated.semantic.stop", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto before = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(before.ok()) << before.error().message;
  second_engine.recovery_assessment.observed_state_fresh =
      !second_engine.recovery_assessment.observed_state_fresh;
  const auto after = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(after.ok()) << after.error().message;
  EXPECT_EQ(after.value().assessment.assessment_revision,
            before.value().assessment.assessment_revision + 1U);
}

TEST(ResourceAddressedExecutionKernelTest, RecoveryWaitsForSharedStopCertificateBeforeRetirement) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(coordinated_occurrence(binding, "a"), first_engine),
       registration(coordinated_occurrence(binding, "b"), second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  std::vector<OperationAcceptedDTO> operations;
  for (const auto* suffix : {"a", "b"}) {
    auto intent = motion_operation(std::string("shared.recovery.") + suffix);
    intent.participants.front().component_role_id = ComponentRoleId(std::string("robot_") + suffix);
    intent.participants.front().target_role_id = TargetRoleId(std::string("arm_") + suffix);
    const auto admitted = kernel.execute(intent);
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    operations.push_back(admitted.value());
  }
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  HeldSettlementCall held_peer;
  second_engine.translate_stop_stage_hook = held_peer.Blocker();
  const auto stop = kernel.stop(program_run_stop_request("shared.recovery.stop", "run.1"));
  ASSERT_TRUE(stop.ok()) << stop.error().message;
  ASSERT_EQ(stop.value().affected_operation_revisions.size(), 2U);
  ASSERT_TRUE(held_peer.Entered());
  // This read becomes available once A's local Stop work drains. B still
  // prevents the *shared* certificate from committing.
  ASSERT_TRUE(wait_until([&] {
    return kernel.get_operation_recovery_assessment(operations.front().operation_id).ok();
  }));
  KernelRecoveryActionRequestDTO recovery{"shared.recovery.early",
                                          RecoveryAction::kRetryMotionFromCurrentState,
                                          "operator.test",
                                          1U,
                                          1U,
                                          41U,
                                          "",
                                          "",
                                          0U,
                                          "test"};
  first_engine.next_recovery_result =
      KernelRecoveryActionResultDTO{recovery.request_id, recovery.action, true, {}, std::nullopt};
  auto state = kernel.get_operation_state(operations.front().operation_id);
  ASSERT_TRUE(state.ok());
  const auto early = kernel.apply_operation_recovery_action(
      operations.front().operation_id, state.value().operation_revision, recovery);
  ASSERT_FALSE(early.ok());
  EXPECT_EQ(early.error().details.at("reason"), "operation_recovery_busy");
  EXPECT_EQ(first_engine.recovery_action_calls.load(), 0);
  held_peer.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stop.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  state = kernel.get_operation_state(operations.front().operation_id);
  ASSERT_TRUE(state.ok());
  recovery.request_id = "shared.recovery.after-settlement";
  first_engine.next_recovery_result =
      KernelRecoveryActionResultDTO{recovery.request_id, recovery.action, true, {}, std::nullopt};
  const auto recovered = kernel.apply_operation_recovery_action(
      operations.front().operation_id, state.value().operation_revision, recovery);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_TRUE(recovered.value().accepted);
  EXPECT_EQ(first_engine.recovery_action_calls.load(), 1);
  EXPECT_FALSE(kernel.get_operation_projection(operations.front().operation_id).value().resident);
  EXPECT_TRUE(kernel.get_operation_projection(operations.back().operation_id).value().resident);
}

TEST(ResourceAddressedExecutionKernelTest, ReconciledProgramClaimCannotRetainAdmissionAuthority) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  config.cell_admission_reconcile_validator = [](const auto& held, const auto&,
                                                 const auto& request) {
    CellAdmissionReconcileValidationDTO validation{request.current_owner_epochs, {}};
    for (const auto& claim : held) {
      validation.settlements.push_back(
          {claim, CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.successor"),
           CellAdmissionSettlementKindDTO::kReconciled, request.reconciliation_evidence_digest});
    }
    return Result<CellAdmissionReconcileValidationDTO>::Ok(std::move(validation));
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  const auto claim = claim_program_execution(kernel);
  ASSERT_TRUE(claim.ok());
  auto capture = program_execution_capture();
  capture.claim_generation = claim.value().claim_generation;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  const CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel.epoch.1"),
      {claim.value()},
      {{CellAdmissionOwnerRoleDTO::kProgramRunner, RequestOwnerEpoch("program.successor")}},
      ContentDigest(test_digest('2'))};
  const auto reconciled = kernel.ReconcileCellAdmissionClaims(reconcile);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_FALSE(kernel.register_program_run_capture(capture).ok());
  EXPECT_FALSE(kernel.execute(scoped_motion_operation("after-owner-replacement")).ok());
  const auto receipt =
      kernel.GetCellAdmissionRecoveryReceipt({claim.value().intent, claim.value(), {}});
  ASSERT_TRUE(receipt.ok());
  EXPECT_TRUE(receipt.value().held_claims.empty());
  EXPECT_EQ(engine.execute_calls, 0);
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.successor"),
                            1U,
                            ProgramCycleStateDTO::kIdleAfterReset,
                            {},
                            {},
                            {}},
      true,
      1U,
      1U,
      true,
      true,
      0U,
      0U,
      0U};
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  auto successor_intent = claim.value().intent;
  successor_intent.owner_epoch = RequestOwnerEpoch("program.successor");
  successor_intent.owner_transaction_id = "run.2";
  successor_intent.request_id = RequestId("successor-claim");
  successor_intent.expected_admission_generation =
      kernel.GetAuthoringAdmission().admission_generation;
  const auto successor = kernel.AcquireCellAdmissionClaim(successor_intent);
  ASSERT_TRUE(successor.ok()) << successor.error().message;
  capture.program_runner_owner_epoch = successor_intent.owner_epoch;
  capture.program_run_id = ProgramRunId("run.2");
  // Same current coordinates with a predecessor claim generation must fail.
  EXPECT_FALSE(kernel.register_program_run_capture(capture).ok());
  capture.claim_generation = successor.value().claim_generation;
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel, "all", "run.2", "program.successor",
                       successor.value().claim_generation.value());
  // Whether retained or compacted, replaying old reconciliation cannot
  // reinstall its predecessor or discard the current successor capture.
  (void)kernel.ReconcileCellAdmissionClaims(reconcile);
  auto next = scoped_motion_operation("successor-operation");
  next.program_run_id = capture.program_run_id;
  const auto admitted = kernel.execute(next);
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgramCaptureRejectsForgedOriginAndPreservesDerivedTrace) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  EXPECT_FALSE(kernel.execute(motion_operation("missing-origin")).ok());
  auto forged = scoped_motion_operation("unknown-origin");
  forged.participants.front().execution_scope->node_instance_id = ProgramNodeInstanceId("unknown");
  EXPECT_FALSE(kernel.execute(forged).ok());
  forged.participants.front().execution_scope->node_instance_id = ProgramNodeInstanceId("timer");
  EXPECT_FALSE(kernel.execute(forged).ok());
  forged = scoped_motion_operation("wrong-target");
  forged.participants.front().target_role_id = TargetRoleId("other-arm");
  EXPECT_FALSE(kernel.execute(forged).ok());
  forged = scoped_motion_operation("wrong-binding");
  forged.program_binding_id = ProgramBindingId(test_digest('2'));
  EXPECT_FALSE(kernel.execute(forged).ok());
  auto valid = scoped_motion_operation("derived-phase");
  ActionSpanDTO trace;
  trace.action_source = ActionSource::kProgramNode;
  trace.action_span_id = "move.attempt.1:approach";
  trace.program_run_id = "run.1";
  trace.node_instance_id = "move/approach";
  trace.node_attempt_id = "move.attempt.1:approach";
  trace.parent_node_attempt_id = "move.attempt.1";
  valid.participants.front().action_span = trace;
  const auto accepted = kernel.execute(valid);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().participants.front().execution_scope,
            valid.participants.front().execution_scope);
}

TEST(ResourceAddressedExecutionKernelTest, ProgramStopOvertakesResolvedButUnadmittedWork) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto registered = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  ResourceAddressedExecutionKernel* owner = nullptr;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  config.participant_resolver = [&](const OperationIntentDTO&, std::size_t) {
    // Deterministic crossing: Stop wins after the first admission check and
    // before the exact binding returns. No scheduler sleep or BT tick.
    const auto stopped = owner->stop(program_run_stop_request("overtaking-stop", "run.1"));
    EXPECT_TRUE(stopped.ok());
    return Result<RuntimeParticipantExecutionRegistration>::Ok(registered);
  };
  ResourceAddressedExecutionKernel kernel({registered}, clock, scheduler, default_journal,
                                          std::move(config));
  owner = &kernel;
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  start_program_motion(kernel);
  const auto late = kernel.execute(scoped_motion_operation("late-resolved-work"));
  ASSERT_FALSE(late.ok());
  EXPECT_EQ(late.error().details.at("reason"), "program_run_stop_fence_active");
  EXPECT_EQ(engine.prepare_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, BlockedControlOnOneOperationCannotDelayDisjointStop) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto first = kernel.execute(motion_operation("request.concurrent-control.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.concurrent-control.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  first_engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(first.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kCompleted;
  }));

  std::promise<void> ack_entered_promise;
  auto ack_entered = ack_entered_promise.get_future();
  std::promise<void> release_ack_promise;
  auto release_ack = release_ack_promise.get_future().share();
  std::atomic<bool> ack_effect_returned{};
  first_engine.acknowledge_effect_hook = [&] {
    ack_entered_promise.set_value();
    release_ack.wait();
    ack_effect_returned.store(true, std::memory_order_release);
  };
  auto acknowledged = std::async(std::launch::async, [&] {
    return kernel.acknowledge_completed(
        control_request("request.concurrent-control.ack", first.value(), 1U));
  });
  const auto ack_started = ack_entered.wait_for(1s);
  if (ack_started != std::future_status::ready) {
    release_ack_promise.set_value();
    (void)acknowledged.get();
    FAIL() << "the injected acknowledgement effect did not start";
  }

  auto stopped = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.concurrent-control.stop", second.value(), 1U));
  });
  const auto stop_status = stopped.wait_for(250ms);
  release_ack_promise.set_value();
  const auto ack_result = acknowledged.get();
  ASSERT_TRUE(ack_result.ok()) << ack_result.error().message;
  ASSERT_TRUE(wait_until([&] { return ack_effect_returned.load(std::memory_order_acquire); }));
  ASSERT_EQ(stop_status, std::future_status::ready)
      << "disjoint Stop waited for another operation's subordinate control";
  const auto stop_result = stopped.get();
  ASSERT_TRUE(stop_result.ok()) << stop_result.error().message;
  ASSERT_TRUE(wait_until([&] { return second_engine.stop_calls.load() == 1; }));
  EXPECT_EQ(first_engine.stop_calls, 0);
  EXPECT_EQ(second_engine.stop_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     AcceptedStopRetainsTerminalAuthorityOverLateNaturalCompletion) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto initial_cell_stop = kernel.cell_stop_action_projection();
  EXPECT_EQ(initial_cell_stop.expected_stop_generation, StopGeneration(0U));
  EXPECT_FALSE(initial_cell_stop.latest_terminal.has_value());
  EXPECT_FALSE(initial_cell_stop.fault.has_value());
  const auto operation = kernel.execute(motion_operation("request.stop-terminal-owner"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-terminal-owner.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(engine.addressed_stop_fence_calls, 1);
  EXPECT_EQ(engine.addressed_stop_fence_execution_id, engine.next_execution_id);

  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  const auto state = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kStopping);
  EXPECT_EQ(state.value().active_stop_id, std::optional(stopped.value().stop_id));

  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && (status.value().state == StopStatusStateDTO::kSucceeded ||
                           status.value().state == StopStatusStateDTO::kPartial ||
                           status.value().state == StopStatusStateDTO::kFailed);
  }));
  const auto terminal_status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(terminal_status.ok()) << terminal_status.error().message;
  EXPECT_EQ(terminal_status.value().state, StopStatusStateDTO::kSucceeded);
  EXPECT_FALSE(stop_status_field_error(terminal_status.value()).has_value());
  ASSERT_TRUE(terminal_status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(terminal_status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_FALSE(stop_certificate_field_error(certificate.value()).has_value());
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededNominal);
  EXPECT_EQ(certificate.value().stop_id, stopped.value().stop_id);
  EXPECT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));
  EXPECT_EQ(kernel.get_stop_fault_latch().has_value(),
            certificate.value().result != StopCertificateResultDTO::kSucceededNominal);
  const auto cell_stop = kernel.cell_stop_action_projection();
  EXPECT_EQ(cell_stop.expected_stop_generation, stopped.value().stop_generation);
  ASSERT_TRUE(cell_stop.latest_terminal.has_value());
  EXPECT_EQ(cell_stop.latest_terminal->certificate.stop_certificate_id,
            certificate.value().stop_certificate_id);
  EXPECT_EQ(cell_stop.latest_terminal->result, certificate.value().result);
  EXPECT_FALSE(cell_stop.latest_terminal->executed_stages.empty());
  EXPECT_EQ(cell_stop.fault.has_value(), kernel.get_stop_fault_latch().has_value());
  const auto final_state = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(final_state.ok()) << final_state.error().message;
  EXPECT_EQ(final_state.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     SuccessfulAddressedStopCommitsTheExactParticipantRecoveryFactOnce) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto operation = kernel.execute(motion_operation("request.stop-recovery-owner"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-recovery-owner.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(operation.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kStopped &&
           engine.participant_stop_settlement_calls.load() == 1;
  }));

  const auto recovery = kernel.get_operation_recovery_assessment(operation.value().operation_id);
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  EXPECT_EQ(recovery.value().assessment.recovery_class, RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(recovery.value().assessment.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(recovery.value().assessment.backend_stop_confirmed);
  EXPECT_EQ(recovery.value().assessment.stop_certificate_id,
            "stop-certificate." + stopped.value().stop_id.value());
  EXPECT_EQ(engine.stop_calls, 1);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     AggregateStopCannotFinishBeforeContainedParticipantSettlementReturns) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  std::promise<void> settlement_entered_promise;
  auto settlement_entered = settlement_entered_promise.get_future();
  std::promise<void> release_settlement_promise;
  auto release_settlement = release_settlement_promise.get_future().share();
  engine.participant_stop_settlement_hook = [&] {
    settlement_entered_promise.set_value();
    release_settlement.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto operation = kernel.execute(motion_operation("request.stop-settlement-order"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-settlement-order.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  if (settlement_entered.wait_for(1s) != std::future_status::ready) {
    release_settlement_promise.set_value();
    FAIL() << "participant settlement was not reached";
  }

  const auto pending_status = kernel.get_stop_status(stopped.value().stop_id);
  EXPECT_TRUE(pending_status.ok())
      << (pending_status.ok() ? std::string{} : pending_status.error().message);
  if (pending_status.ok()) {
    EXPECT_NE(pending_status.value().state, StopStatusStateDTO::kSucceeded);
  }
  const auto pending_operation = kernel.get_operation_state(operation.value().operation_id);
  EXPECT_TRUE(pending_operation.ok())
      << (pending_operation.ok() ? std::string{} : pending_operation.error().message);
  if (pending_operation.ok()) {
    EXPECT_EQ(pending_operation.value().state, OperationStateKind::kStopping);
  }

  release_settlement_promise.set_value();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ContainedParticipantSettlementFailureProducesPartialStopTruth) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.participant_stop_settlement_error =
      ErrorDTO{"participant_terminal_unavailable",
               "participant could not commit the addressed Stop terminal",
               {}};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto operation = kernel.execute(motion_operation("request.stop-settlement-failure"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-settlement-failure.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kPartial;
  }));

  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_EQ(certificate.value().dispatched_stopped.size(), 1U);
  ASSERT_EQ(certificate.value().unsettled.size(), 1U);
  EXPECT_EQ(certificate.value().dispatched_stopped.front(), certificate.value().unsettled.front());
  EXPECT_TRUE(certificate.value().failed.empty());
  ASSERT_TRUE(certificate.value().terminal_error.has_value());
  EXPECT_EQ(certificate.value().terminal_error->code, "participant_terminal_unavailable");
  EXPECT_TRUE(kernel.get_stop_fault_latch().has_value());
  const auto operation_state = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(operation_state.ok()) << operation_state.error().message;
  EXPECT_EQ(operation_state.value().state, OperationStateKind::kInterventionRequired);
  EXPECT_EQ(engine.stop_calls, 1);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     MismatchedParticipantSettlementCannotClaimAggregateStopSuccess) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.return_mismatched_participant_stop_settlement = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto operation = kernel.execute(motion_operation("request.stop-settlement-mismatch"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-settlement-mismatch.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kPartial;
  }));

  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kPartial);
  ASSERT_TRUE(certificate.value().terminal_error.has_value());
  EXPECT_EQ(certificate.value().terminal_error->details.at("reason"),
            "participant_stop_settlement_mismatch");
  ASSERT_EQ(certificate.value().dispatched_stopped.size(), 1U);
  ASSERT_EQ(certificate.value().unsettled.size(), 1U);
  EXPECT_EQ(engine.stop_calls, 1);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest, UsesOneInjectedGateForTheActualSlotTableReservation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U, {}, &gate});

  const auto blocked = kernel.execute(motion_operation());
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "configuration_fence_held");
  EXPECT_EQ(gate.reserve_calls, 1);
  EXPECT_EQ(engine.execute_calls, 0);
  const auto blocked_slots = kernel.resource_snapshot();
  ASSERT_EQ(blocked_slots.size(), 1U);
  EXPECT_EQ(blocked_slots.front().state, ResourceSlotState::kFree);

  const auto admission =
      kernel.resource_admission_table()->admission_snapshot({ResourceId("robot.joint_1")});
  EXPECT_EQ(admission.operation_resource_generation, 0U);
  ASSERT_EQ(admission.active_control_configurations.size(), 1U);
  EXPECT_EQ(admission.affected_queued_operation_count, 0U);
  EXPECT_EQ(admission.affected_active_operation_count, 0U);

  gate.reject_reservation = false;
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(gate.reserve_calls, 2);
  EXPECT_EQ(engine.execute_calls, 1);
  const auto admitted =
      kernel.resource_admission_table()->admission_snapshot({ResourceId("robot.joint_1")});
  EXPECT_EQ(admitted.operation_resource_generation, 2U);
  EXPECT_EQ(admitted.affected_queued_operation_count, 0U);
  EXPECT_EQ(admitted.affected_active_operation_count, 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     StagesUnreadyCandidateConfigurationFactsOnlyUnderTheInjectedApplyFence) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {unqualified_registration(binding, engine)}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U, {}, &gate});
  const auto& path = *binding.value().control_path;
  const ConfigurationFenceScopeDTO scope{
      {binding.value().controllable_component_id},
      {{path.control_domain_id, std::nullopt, path.control_domain_revision}},
      {{path.control_binding_id, std::nullopt, path.control_binding_revision}},
      {binding.value().resources.front().resource_id}};
  ASSERT_TRUE(scope.valid());
  EXPECT_TRUE(kernel.resource_admission_table()
                  ->admission_snapshot(scope.resources)
                  .active_control_configurations.empty());

  const auto staged = kernel.stage_current_runtime_configuration_facts_under_apply_fence(scope);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  const auto admission = kernel.resource_admission_table()->admission_snapshot(scope.resources);
  ASSERT_EQ(admission.active_control_configurations.size(), 1U);
  EXPECT_TRUE(admission.route_published_active_control_configurations.empty());
  EXPECT_EQ(admission.active_control_configurations.front().control_domain_id,
            path.control_domain_id);
  EXPECT_EQ(admission.active_control_configurations.front().active_control_configuration_id,
            path.active_control_configuration_id);
  EXPECT_EQ(admission.active_control_configurations.front().active_control_configuration_generation,
            path.active_control_configuration_generation);
  const auto blocked = kernel.execute(motion_operation());
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "runtime_route_publication_required");

  gate.reject_reservation = false;
  const auto unfenced = kernel.stage_current_runtime_configuration_facts_under_apply_fence(scope);
  ASSERT_FALSE(unfenced.ok());
  EXPECT_EQ(unfenced.error().details.at("reason"), "runtime_configuration_staging_fence_missing");
}

TEST(ResourceAddressedExecutionKernelTest,
     ProductBackedResolverAdmitsAnExactProgramBindingOnPinnedPhysicalEngine) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1234);
  FakeParticipantEngine engine;
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto dynamic_value = physical.value();
  dynamic_value.program_id = ProgramId("program.dynamic");
  dynamic_value.program_revision = ProgramRevision(test_digest('4'));
  dynamic_value.program_binding_id = ProgramBindingId(test_digest('5'));
  auto dynamic = verify_runtime_participant_binding(std::move(dynamic_value));
  ASSERT_TRUE(dynamic.ok()) << dynamic.error().message;

  auto intent = motion_operation("request.dynamic.1");
  intent.program_id = ProgramId("program.dynamic");
  intent.program_revision = ProgramRevision(test_digest('4'));
  intent.program_binding_id = ProgramBindingId(test_digest('5'));
  std::size_t resolver_calls = 0U;
  ResourceAddressedExecutionKernel kernel(
      {registration(physical, engine)}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U,
          [&](const OperationIntentDTO&,
              const std::size_t) -> Result<RuntimeParticipantExecutionRegistration> {
            ++resolver_calls;
            return Result<RuntimeParticipantExecutionRegistration>::Ok(
                registration(dynamic.value(), engine));
          }});

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(resolver_calls, 1U);
  EXPECT_EQ(engine.execute_calls, 1);
  ASSERT_TRUE(engine.last_execute.has_value());
  EXPECT_EQ(engine.last_execute->binding.program_id, intent.program_id);
  EXPECT_EQ(engine.last_execute->binding.program_binding_id, intent.program_binding_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     ResolvesProgramContactGraspAndSelectedRuntimeObjectBeforeSubordinateCall) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
                       true),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.contact_policy_id = ContactPolicyId("selected_object_grasp");
  motion.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("selected_object"), SceneResourceId("part.7")}};

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->resolved_contact_grasp.has_value());
  const auto& resolved = *engine.last_execute->resolved_contact_grasp;
  EXPECT_EQ(resolved.contact_policy_id.value(), "selected_object_grasp");
  EXPECT_EQ(resolved.runtime_object_id.value(), "selected_object");
  EXPECT_EQ(resolved.selected_object_id.value(), "part.7");
  EXPECT_EQ(resolved.allowed_robot_body_ids.size(), 1U);
  EXPECT_EQ(engine.last_execute->runtime_object_bindings, motion.runtime_object_bindings);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionUsesTheExecutionBuilderWithoutDispatchOrJournalEffects) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1234);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
                       true),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation("request.preview.1");
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.contact_policy_id = ContactPolicyId("selected_object_grasp");
  motion.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("selected_object"), SceneResourceId("part.7")}};

  auto session = kernel.begin_motion_preview_session(intent);
  ASSERT_TRUE(session.ok()) << session.error().message;
  auto prepared = session.value()->prepare_motion(intent);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(prepared.value().resolved_contact_grasp.has_value());
  EXPECT_EQ(prepared.value().resolved_contact_grasp->selected_object_id.value(), "part.7");
  EXPECT_EQ(prepared.value().binding.controllable_component_id.value(), "robot.1");
  auto state = session.value()->current_state();
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().joint_names, std::vector<std::string>{"mf2_joint_1"});
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.preview_calls, 0);
  EXPECT_EQ(journal.calls, std::vector<std::string>{"begin"});
}

TEST(ResourceAddressedExecutionKernelTest, PreviewConflictsUsePhysicalResourcesAndEngine) {
  for (const int scenario : {0, 1, 2}) {
    SCOPED_TRACE(scenario);  // disjoint, same resources, shared engine
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    FakeOperationJournal journal;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto second = scenario == 1 ? second_occurrence_sharing_first_resource(first)
                                      : second_same_type_occurrence(first);
    auto& preview_engine = scenario == 2 ? first_engine : second_engine;
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine), registration(second, preview_engine)}, clock, scheduler,
        journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto active = kernel.execute(motion_operation("active.arm"));
    ASSERT_TRUE(active.ok()) << active.error().message;
    auto preview = motion_operation("preview.other");
    preview.program_binding_id = second.value().program_binding_id;
    preview.participants.front().component_role_id = second.value().component_role_id;
    preview.participants.front().target_role_id = second.value().target_role_id;
    auto session = kernel.begin_motion_preview_session(preview);
    ASSERT_EQ(session.ok(), scenario == 0) << (session.ok() ? "" : session.error().message);
    if (session.ok()) {
      EXPECT_TRUE(session.value()->prepare_motion(preview).ok());
    } else {
      EXPECT_EQ(session.error().details.at("reason"), "motion_preview_operation_busy");
    }
    EXPECT_EQ(second_engine.execute_calls, 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest, HeldManualPreviewRechecksContextBetweenLinkedLegs) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto direct = registration(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  direct.intrinsic_direct_control = true;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, journal, config);
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  const auto capture = program_execution_capture();
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);
  auto manual = motion_operation("manual.preview");
  manual.program_run_id = ProgramRunId("manual.run");
  EXPECT_FALSE(kernel.begin_motion_preview_session(manual).ok());
  ASSERT_TRUE(kernel.stop_program_selection(program_selection_request("held", "arm")).ok());
  EXPECT_FALSE(kernel.begin_motion_preview_session(manual).ok());  // context required
  const auto held = kernel.get_manual_program_handover(manual);
  ASSERT_TRUE(held.ok());
  ASSERT_TRUE(held.value());
  manual.manual_handover =
      ManualProgramHandoverContextDTO{capture.program_runner_owner_epoch, capture.program_run_id,
                                      capture.claim_generation, held.value()->selection_revision};
  auto session = kernel.begin_motion_preview_session(manual);
  ASSERT_TRUE(session.ok()) << session.error().message;
  EXPECT_TRUE(session.value()->prepare_motion(manual).ok());
  ASSERT_TRUE(kernel.stop_program_selection(program_selection_request("new.stop", "arm")).ok());
  const auto stale = session.value()->prepare_motion(manual);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "manual_handover_scope_stale");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.prepare_motion_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionAllowsMotionSpecificPlannerProvidersOnOnePinnedParticipant) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto pose = with_planner_capability(physical, "planning.motion.pose_goal");
  const auto cartesian = with_planner_capability(physical, "planning.motion.cartesian_path");
  ResourceAddressedExecutionKernel kernel(
      {registration(pose, engine)}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U,
          [&](const OperationIntentDTO& operation,
              const std::size_t) -> Result<RuntimeParticipantExecutionRegistration> {
            const auto& motion =
                std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
            const auto& selected =
                std::holds_alternative<CartesianPathDTO>(motion.motion) ? cartesian : pose;
            return Result<RuntimeParticipantExecutionRegistration>::Ok(
                registration(selected, engine));
          }});
  auto primary = motion_operation("request.preview.pose");
  auto& primary_motion = std::get<MotionParticipantIntentDTO>(primary.participants.front().payload);
  primary_motion.motion = PoseGoalDTO{Pose3DTO{}};
  primary_motion.per_joint_dynamics_limits.clear();

  auto session = kernel.begin_motion_preview_session(primary);
  ASSERT_TRUE(session.ok()) << session.error().message;
  auto linked = motion_operation("request.preview.cartesian");
  auto& linked_motion = std::get<MotionParticipantIntentDTO>(linked.participants.front().payload);
  linked_motion.move_id = MoveId("move.cartesian");
  linked_motion.motion = CartesianPathDTO{{Pose3DTO{}, Pose3DTO{{0.0, 0.0, 0.1}, {}}}};
  linked_motion.per_joint_dynamics_limits.clear();

  auto prepared = session.value()->prepare_motion(linked);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().binding.planner_capability_binding.capability_id.value(),
            "planning.motion.cartesian_path");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.preview_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, RefusesEveryMotionInterruptScopeBeforePreparation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, config);
  ASSERT_TRUE(ready_cell_admission(kernel).ok());
  auto intent = motion_operation("request.interrupt.scope");
  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope.contact";
  scope.policy_id = "interrupt.contact";
  std::get<MotionParticipantIntentDTO>(intent.participants.front().payload)
      .motion_interrupt_scopes.push_back(scope);

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_interrupt_policy_not_admitted");
  EXPECT_EQ(engine.execute_calls, 0);
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrdinaryJerkPolicyIsSharedByProgramManualPreviewAndExecution) {
  for (const bool manual : {false, true}) {
    for (const std::optional<double> requested : {std::optional<double>{}, {5.0}, {11.0}}) {
      SCOPED_TRACE(manual);
      SCOPED_TRACE(requested.value_or(-1.0));
      FakeClock clock;
      ManualScheduler scheduler;
      FakeParticipantEngine engine;
      FakeOperationJournal journal;
      auto target = registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine);
      target.intrinsic_direct_control = manual;
      OrdinaryMotionJerkPolicy profile{true, false, {}};
      for (const auto& resource : target.binding.value().resources)
        profile.limits.emplace(*resource.source_joint_name, AngularJerkDTO{10.0});
      auto policy =
          apply_deployment_motion_jerk_policy(*target.participant_policy, target.binding, profile);
      ASSERT_TRUE(policy.ok());
      target.participant_policy = std::move(policy.value());
      ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                    CellId("cell.test"), 8U};
      config.manager_liveness_required_set = EmptyManagerLivenessSet();
      ResourceAddressedExecutionKernel kernel({target}, clock, scheduler, journal, config);
      ASSERT_TRUE(ready_cell_admission(kernel).ok());
      auto intent = motion_operation("request.ordinary.jerk");
      auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
      motion.per_joint_dynamics_limits.clear();
      if (requested) motion.per_joint_dynamics_limits["joint_1"].max_jerk_rad_s3 = *requested;
      auto session = kernel.begin_motion_preview_session(intent);
      ASSERT_TRUE(session.ok()) << session.error().message;
      const auto preview = session.value()->prepare_motion(intent);
      session.value().reset();
      const auto execution = kernel.execute(intent);
      if (requested && *requested > 10.0) {
        EXPECT_FALSE(preview.ok());
        EXPECT_FALSE(execution.ok());
        EXPECT_EQ(engine.execute_calls, 0);
        continue;
      }
      ASSERT_TRUE(preview.ok()) << preview.error().message;
      ASSERT_TRUE(execution.ok()) << execution.error().message;
      ASSERT_TRUE(engine.last_execute);
      EXPECT_EQ(preview.value().per_resource_dynamics_limits,
                engine.last_execute->per_resource_dynamics_limits);
      ASSERT_EQ(preview.value().per_resource_dynamics_limits.size(),
                target.binding.value().resources.size());
      for (const auto& row : preview.value().per_resource_dynamics_limits)
        EXPECT_LE(*row.limit.max_jerk_rad_s3, 10.0);
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest, ManualCeilingsApplyAtBothPreviewAndExecutionBoundaries) {
  for (const bool manual : {false, true}) {
    for (const std::optional<double> requested : {std::optional<double>{}, {0.2}, {9.0}}) {
      SCOPED_TRACE(manual);
      SCOPED_TRACE(requested.value_or(-1.0));
      FakeClock clock;
      ManualScheduler scheduler;
      FakeParticipantEngine engine;
      FakeOperationJournal journal;
      auto target = registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine);
      target.intrinsic_direct_control = manual;
      target.direct_control_joint_limits = {{"joint_1", {0.7, 1.2, std::nullopt}}};
      ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                    CellId("cell.test"), 8U};
      config.manager_liveness_required_set = EmptyManagerLivenessSet();
      ResourceAddressedExecutionKernel kernel({target}, clock, scheduler, journal, config);
      ASSERT_TRUE(ready_cell_admission(kernel).ok());
      auto intent = motion_operation("request.manual.caps");
      auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
      motion.per_joint_dynamics_limits.clear();
      if (requested.has_value())
        motion.per_joint_dynamics_limits["joint_1"] = {*requested, *requested, std::nullopt};
      auto session = kernel.begin_motion_preview_session(intent);
      ASSERT_TRUE(session.ok()) << session.error().message;
      const auto prepared = session.value()->prepare_motion(intent);
      ASSERT_TRUE(prepared.ok()) << prepared.error().message;
      session.value().reset();  // Release preview's submission lock before execution.
      const auto accepted = kernel.execute(intent);
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      ASSERT_TRUE(engine.last_execute.has_value());
      EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits,
                prepared.value().per_resource_dynamics_limits);
      const auto& caps = prepared.value().per_resource_dynamics_limits;
      if (!manual && !requested.has_value()) {
        EXPECT_TRUE(caps.empty());
      } else {
        ASSERT_EQ(caps.size(), 1U);
        EXPECT_EQ(caps.front().compiled_joint_name, "mf2_joint_1");
        EXPECT_DOUBLE_EQ(*caps.front().limit.max_velocity_rad_s,
                         manual ? std::min(requested.value_or(0.7), 0.7) : *requested);
        EXPECT_DOUBLE_EQ(*caps.front().limit.max_acceleration_rad_s2,
                         manual ? std::min(requested.value_or(1.2), 1.2) : *requested);
      }
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionRejectsAPlannerResolutionThatChangesThePinnedEngine) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine primary_engine;
  FakeParticipantEngine other_engine;
  FakeOperationJournal journal;
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto pose = with_planner_capability(physical, "planning.motion.pose_goal");
  const auto cartesian = with_planner_capability(physical, "planning.motion.cartesian_path");
  const auto unrelated =
      make_binding("gripper", "gripper.target", "tool.gripper.command", CoordinateKind::kLinear,
                   ProgramCommandSchemaRefDTO{
                       CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
                       CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))});
  ResourceAddressedExecutionKernel kernel(
      {registration(pose, primary_engine), registration(unrelated, other_engine)}, clock, scheduler,
      journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U,
          [&](const OperationIntentDTO& operation,
              const std::size_t) -> Result<RuntimeParticipantExecutionRegistration> {
            const auto& motion =
                std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
            const bool is_cartesian = std::holds_alternative<CartesianPathDTO>(motion.motion);
            return Result<RuntimeParticipantExecutionRegistration>::Ok(registration(
                is_cartesian ? cartesian : pose,
                is_cartesian ? static_cast<IResolvedParticipantExecutionEngine&>(other_engine)
                             : static_cast<IResolvedParticipantExecutionEngine&>(primary_engine)));
          }});
  auto primary = motion_operation("request.preview.engine.pose");
  auto& primary_motion = std::get<MotionParticipantIntentDTO>(primary.participants.front().payload);
  primary_motion.motion = PoseGoalDTO{Pose3DTO{}};
  primary_motion.per_joint_dynamics_limits.clear();

  auto session = kernel.begin_motion_preview_session(primary);
  ASSERT_TRUE(session.ok()) << session.error().message;
  auto linked = motion_operation("request.preview.engine.cartesian");
  auto& linked_motion = std::get<MotionParticipantIntentDTO>(linked.participants.front().payload);
  linked_motion.move_id = MoveId("move.engine.cartesian");
  linked_motion.motion = CartesianPathDTO{{Pose3DTO{}, Pose3DTO{{0.0, 0.0, 0.1}, {}}}};
  linked_motion.per_joint_dynamics_limits.clear();

  auto prepared = session.value()->prepare_motion(linked);
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().details.at("reason"), "motion_preview_engine_changed");
  EXPECT_EQ(primary_engine.execute_calls, 0);
  EXPECT_EQ(other_engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionRejectsAStaleOwnerBeforePrivateResolution) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation("request.preview.stale");
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");

  auto session = kernel.begin_motion_preview_session(intent);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().details.at("reason"), "stale_request_owner_epoch");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.preview_calls, 0);
  EXPECT_EQ(journal.calls, std::vector<std::string>{"begin"});
}

TEST(ResourceAddressedExecutionKernelTest,
     RejectsProgramContactGraspWithoutItsRuntimeObjectBeforeEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
                       true),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto expect_policy_object_named = [](const ErrorDTO& error) {
    EXPECT_EQ(error.code, "missing_reference");
    EXPECT_EQ(error.details.at("reason"), "contact_grasp_runtime_object_unresolved");
    // The one reference is the runtime object the contact policy requires.
    EXPECT_EQ(error.details.at("reference_type"), "runtime_object");
    EXPECT_EQ(error.details.at("reference_id"), "selected_object");
    EXPECT_FALSE(error.details.contains("target_role_id"));
  };
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.contact_policy_id = ContactPolicyId("selected_object_grasp");

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  expect_policy_object_named(result.error());
  EXPECT_EQ(engine.execute_calls, 0);

  // Selecting a different runtime object still names the one the policy needs,
  // never the selected one.
  auto other = motion_operation("request.contact_grasp.other_object");
  auto& other_motion = std::get<MotionParticipantIntentDTO>(other.participants.front().payload);
  other_motion.contact_policy_id = ContactPolicyId("selected_object_grasp");
  other_motion.runtime_object_bindings = {RuntimeObjectReferenceBindingDTO{
      RuntimeObjectId("other_object"), SceneResourceId("part.7")}};
  const auto other_result = kernel.execute(other);
  ASSERT_FALSE(other_result.ok());
  expect_policy_object_named(other_result.error());
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, RejectsResourceRoleSpellingUsedAsASourceJointName) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  std::get<JointGoalDTO>(motion.motion).joint_names = {"arm_joints"};

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "joint_goal_source_joint_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, RejectsStalePlannerCapabilityRevision) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:1111111111111111111111111111111111111111111111111111111111111111"),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_planner_capability_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, PreservesAuthoredSourceJointOrderingDuringResolution) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                 CoordinateKind::kAngular, std::nullopt, true),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.motion = JointGoalDTO{{"joint_2", "joint_1"}, {0.4, -0.2}};
  motion.per_joint_dynamics_limits = {{"joint_2", JointDynamicsLimitDTO{0.8, 1.5, std::nullopt}}};

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.resources,
            (std::vector<ResourceId>{ResourceId("robot.joint_2"), ResourceId("robot.joint_1")}));
  EXPECT_EQ(goal.joint_names, (std::vector<std::string>{"mf2_joint_2", "mf2_joint_1"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{0.4, -0.2}));
  ASSERT_EQ(engine.last_execute->per_resource_dynamics_limits.size(), 1U);
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().resource_id.value(),
            "robot.joint_2");
}

TEST(ResourceAddressedExecutionKernelTest,
     ResolvesAValidSourceJointSubsetWithoutRoleCardinalityCoupling) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                 CoordinateKind::kAngular, std::nullopt, true),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.motion = JointGoalDTO{{"joint_2"}, {0.4}};
  motion.per_joint_dynamics_limits.clear();

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.resources, (std::vector<ResourceId>{ResourceId("robot.joint_2")}));
  EXPECT_EQ(goal.joint_names, (std::vector<std::string>{"mf2_joint_2"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{0.4}));
}

TEST(ResourceAddressedExecutionKernelTest,
     ExactReplayDoesNotRepeatSubordinateExecutionAndConflictFails) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto intent = motion_operation();

  const auto first = kernel.execute(intent);
  ASSERT_TRUE(first.ok()) << first.error().message;
  const auto replay = kernel.execute(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().operation_id, first.value().operation_id);
  EXPECT_EQ(engine.execute_calls, 1);

  auto conflicting = intent;
  std::get<JointGoalDTO>(
      std::get<MotionParticipantIntentDTO>(conflicting.participants.front().payload).motion)
      .positions_rad.front() = 0.3;
  const auto conflict = kernel.execute(conflicting);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ExactOperationReplayRemainsObservableWhileStopClosesFreshAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto intent = motion_operation("request.acceptance-reply-lost");
  const auto accepted = kernel.execute(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  const auto stopped = kernel.full_stop(
      FullStopIntentDTO{{RequestId("request.acceptance-reply-lost.stop"),
                         RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
                        StopGeneration(0U),
                        {}});
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;

  const auto replay = kernel.execute(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().operation_id, accepted.value().operation_id);
  EXPECT_EQ(engine.execute_calls, 1);

  auto conflicting = intent;
  std::get<JointGoalDTO>(
      std::get<MotionParticipantIntentDTO>(conflicting.participants.front().payload).motion)
      .positions_rad.front() = 0.3;
  const auto conflict = kernel.execute(conflicting);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"), "request_id_conflict");

  const auto fresh = kernel.execute(motion_operation("request.fresh-during-stop"));
  ASSERT_FALSE(fresh.ok());
  EXPECT_EQ(fresh.error().details.at("reason"), "motion_admission_closed");
}

TEST(ResourceAddressedExecutionKernelTest, SubordinateEventsAdvanceExactSlotsAndOperationState) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  clock.set(2000);
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  auto executing = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(executing.ok());
  EXPECT_EQ(executing.value().state, OperationStateKind::kExecuting);
  EXPECT_DOUBLE_EQ(executing.value().participants.front().progress, 0.4);

  clock.set(3000);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  auto completed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(completed.ok());
  EXPECT_EQ(completed.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(completed.value().updated_at_ns, 3000);

  clock.set(4000);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto unchanged = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(unchanged.ok());
  EXPECT_EQ(unchanged.value().updated_at_ns, 3000);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProgressSamplesStayCurrentWithoutJournalWritesAndTerminalStillCommits) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok());
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto baseline = journal.calls.size();
  const auto revision =
      kernel.get_operation_state(accepted.value().operation_id).value().operation_revision;
  for (int sample = 1; sample <= 1000; ++sample) {
    clock.set(sample * 1'000'000);
    engine.status.progress = sample / 1001.0;
    engine.runtime_scale.acknowledgement_timestamp_ns = clock.now();
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    const auto current = kernel.get_operation_state(accepted.value().operation_id);
    ASSERT_TRUE(current.ok());
    EXPECT_DOUBLE_EQ(current.value().participants.front().progress, engine.status.progress);
    EXPECT_EQ(current.value().operation_revision, revision);
    EXPECT_EQ(current.value().updated_at_ns, clock.now());
  }
  EXPECT_EQ(journal.calls.size(), baseline);
  const auto prior = kernel.get_operation_state(accepted.value().operation_id).value();
  for (const auto invalid :
       {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    engine.status.progress = invalid;
    const auto rejected = kernel.reconcile_subordinate_state();
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"), "subordinate_progress_invalid");
    EXPECT_EQ(kernel.get_operation_state(accepted.value().operation_id).value(), prior);
    EXPECT_EQ(journal.calls.size(), baseline);
  }
  journal.fail_next_kind = "state:subordinate_state_observed";
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  EXPECT_EQ(kernel.get_operation_state(accepted.value().operation_id).value(), prior);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto terminal_state = kernel.get_operation_state(accepted.value().operation_id).value();
  EXPECT_EQ(terminal_state.state, OperationStateKind::kCompleted);
  EXPECT_DOUBLE_EQ(terminal_state.participants.front().progress, 1.0);
  EXPECT_EQ(journal.states.back(), terminal_state);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreservesTheExactErrorRaisedBeforeASeparateTerminalTransition) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  engine.emit_error(ErrorDTO{"planning_failed", "exact planner failure", {{"reason", "no_path"}}});
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);

  const auto failed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  ASSERT_TRUE(failed.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(failed.value().participants.front().terminal_error->details.at("reason"), "no_path");
  EXPECT_EQ(failed.value().participants.front().terminal_error->message, "exact planner failure");
}

TEST(ResourceAddressedExecutionKernelTest,
     DoesNotAttachARecoveredSubordinateErrorToSuccessfulCompletion) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  engine.emit_error(
      ErrorDTO{"planning_warning", "recoverable planner warning", {{"reason", "recovered"}}});
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);

  const auto completed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  EXPECT_EQ(completed.value().state, OperationStateKind::kCompleted);
  EXPECT_FALSE(completed.value().participants.front().terminal_error.has_value());
}

TEST(ResourceAddressedExecutionKernelTest, RejectsStaleEpochBeforeResolutionOrSubordinateEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "stale_request_owner_epoch");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest, FocusedGripperReplaysAndSettlesExactBackendCommand) {
  for (const int schedule : {0, 1, 2}) {
    SCOPED_TRACE(schedule);
    const bool hold_dispatch_return = schedule != 0;
    const bool expire_dispatch_return = schedule == 2;
    class Backend final : public IGripperCommandBackend {
     public:
      mutable std::mutex mutex;
      GripperCommandStatusDTO status;
      GripperCommandRequestDTO last_request;
      std::atomic<int> commands{};
      std::atomic<int> invalidations{};
      std::function<void()> return_hook;
      GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override {
        {
          std::lock_guard lock(mutex);
          ++commands;
          last_request = request;
          status.gripper_id = request.gripper_id;
          status.command_id = 41U;  // deliberately not preparation id 1
          status.generation = 73U;
          status.state = GripperCommandState::kExecuting;
        }
        if (return_hook) return_hook();
        return {true, 41U, 73U, std::nullopt};
      }
      GripperCommandStatusDTO get_gripper_status(const std::string&) const override {
        std::lock_guard lock(mutex);
        return status;
      }
      GripperCommandResultDTO mark_gripper_grasp_lost(const std::string&, std::uint64_t,
                                                      std::uint64_t, const std::string&,
                                                      const std::string&,
                                                      const std::string&) override {
        return {};
      }
      void invalidate_gripper_commands(GripperInvalidationReason) noexcept override {
        std::lock_guard lock(mutex);
        status.state = GripperCommandState::kIdle;
        ++status.generation;
        ++invalidations;
      }
      void authorize_gripper_commands() noexcept override {}
    } backend;
    FakeClock clock;
    ManualScheduler scheduler;
    GripperParticipantEngine gripper(backend, clock);
    FakeParticipantEngine stop_port;
    stop_port.translate_stop_stage_hook = [&] {
      backend.invalidate_gripper_commands(GripperInvalidationReason::kStop);
    };
    const ProgramCommandSchemaRefDTO schema{
        CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
        CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
    auto row = registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                         CoordinateKind::kLinear, schema),
                            gripper);
    row.stop_execution_port = &stop_port;
    if (expire_dispatch_return) {
      // FakeClock owns expiry; the production worker's bounded wait is only
      // its wake mechanism. Keep that wait well below the held provider's
      // failure escape, and reseal the entire coherent test envelope.
      auto plan = row.stop_readiness->value().resolved_stop_base_plan;
      plan.deadline_budget.outer_duration_ns /= 1000;
      plan.deadline_budget.kernel_completion_reserve_ns /= 1000;
      for (auto& phase : plan.deadline_budget.phases) phase.deadline_offset_ns /= 1000;
      for (auto& stage : plan.escalation_stages) {
        for (auto& evidence : stage.evidence_requirements) {
          auto& dwell = evidence.requirement.observation_semantics.minimum_dwell_ns;
          if (dwell) *dwell /= 1000;
        }
      }
      const auto sealed = MakeResolvedStopBasePlan(std::move(plan));
      ASSERT_TRUE(sealed.ok()) << sealed.error().message << ": "
                               << sealed.error().details.at("reason");
      const auto domain = controller_domain_for(row.binding, sealed.value());
      const auto ready = verify_runtime_participant_stop_readiness(
          RuntimeParticipantStopReadinessDTO{row.binding.value().controllable_component_id,
                                             ComponentLifecycleGeneration(1U), domain.value(),
                                             sealed.value()},
          row.binding, domain);
      ASSERT_TRUE(ready.ok()) << ready.error().message;
      row.stop_readiness = ready.value();
    }
    ResourceAddressedExecutionKernel kernel(
        {row}, clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    HeldSettlementCall dispatch_return;
    if (hold_dispatch_return) backend.return_hook = dispatch_return.Blocker();
    auto executing =
        std::async(std::launch::async, [&] { return kernel.execute(command_operation()); });
    if (hold_dispatch_return) {
      ASSERT_TRUE(dispatch_return.Entered());
    } else {
      ASSERT_EQ(executing.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    }
    const auto stopped = kernel.full_stop(FullStopIntentDTO{
        {RequestId("focused.stop"), RequestOwnerEpoch("kernel.epoch.1"), "test", {}},
        StopGeneration(0U),
        {}});
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_TRUE(wait_until(
        [&] {
          const auto status = kernel.get_stop_status(stopped.value().stop_id);
          return status.ok() &&
                 std::any_of(status.value().attempted_stage_results.begin(),
                             status.value().attempted_stage_results.end(), [](const auto& attempt) {
                               return attempt.authority ==
                                          StopStageResultAuthorityDTO::kAuthoritative &&
                                      attempt.result.outcome ==
                                          ParticipantStopStageOutcomeDTO::kSucceeded &&
                                      !attempt.result.error.has_value();
                             });
        },
        std::chrono::seconds(1)));
    if (hold_dispatch_return) {
      EXPECT_FALSE(kernel.get_stop_status(stopped.value().stop_id).value().stop_certificate);
      EXPECT_EQ(gripper.ObservePreparedCapabilityCommand(1U).value().state,
                ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested);
      EXPECT_EQ(gripper.get_recovery_assessment().status, RecoveryAssessmentStatus::kUnavailable);
      EXPECT_EQ(backend.invalidations.load(), 1);
    }
    if (expire_dispatch_return) {
      clock.set(stopped.value().accepted_at_ns + stopped.value().fixed_outer_duration_ns);
      const bool terminal = wait_until(
          [&] {
            const auto status = kernel.get_stop_status(stopped.value().stop_id);
            return status.ok() && status.value().stop_certificate.has_value();
          },
          std::chrono::seconds(1));
      // Always release a failed test's call before any future destructor joins.
      if (!terminal) dispatch_return.Release();
      ASSERT_TRUE(terminal);
      const auto status = kernel.get_stop_status(stopped.value().stop_id).value();
      const auto certificate =
          kernel.get_stop_certificate(status.stop_certificate->stop_certificate_id).value();
      EXPECT_EQ(certificate.result, StopCertificateResultDTO::kPartial);
      ASSERT_EQ(certificate.scope_entries.size(), 1U);
      EXPECT_EQ(std::get<StopParticipantScopeFactsDTO>(certificate.scope_entries.front().facts)
                    .terminal_settlement,
                StopParticipantScopeFactsDTO::TerminalSettlementState::kContainedUnsettled);
      ASSERT_TRUE(certificate.terminal_error);
      EXPECT_EQ(certificate.terminal_error->details.at("reason"),
                "participant_stop_dispatch_settlement_deadline_expired");
      // Physical/semantic settlement does not wait on durable projection.
      // Compare the stable latch only after that independent write completes.
      const bool recorded = wait_until(
          [&] {
            const auto current = kernel.get_stop_fault_latch();
            return current &&
                   current->terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
          },
          std::chrono::seconds(1));
      if (!recorded) dispatch_return.Release();
      ASSERT_TRUE(recorded);
      const auto latch = kernel.get_stop_fault_latch();
      ASSERT_TRUE(latch);
      EXPECT_EQ(backend.invalidations.load(), 1);
      dispatch_return.Release();
      const auto accepted = executing.get();
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      EXPECT_EQ(kernel.get_stop_certificate(certificate.stop_certificate_id).value(), certificate);
      EXPECT_EQ(kernel.get_stop_fault_latch(), latch);
      EXPECT_EQ(kernel.get_operation_state(accepted.value().operation_id).value().state,
                OperationStateKind::kInterventionRequired);
      EXPECT_EQ(gripper.get_recovery_assessment().status, RecoveryAssessmentStatus::kUnavailable);
      EXPECT_EQ(backend.invalidations.load(), 1);
      EXPECT_FALSE(kernel.execute(command_operation("command.after-unsettled-stop")).ok());
      continue;
    }
    dispatch_return.Release();
    const auto accepted = executing.get();
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto prepared = gripper.ObservePreparedCapabilityCommand(1U);
    ASSERT_TRUE(prepared.ok());
    const auto& snapshot = prepared.value();
    ASSERT_TRUE(snapshot.participant && snapshot.controller_goal_id &&
                snapshot.controller_goal_generation && snapshot.dispatch_result);
    EXPECT_NE(snapshot.preparation_id, snapshot.dispatch_result->command_id);
    const auto replay = [&] {
      return gripper.DispatchPreparedCapabilityCommand(
          snapshot.preparation_id, snapshot.participant->prepared_contract_digest,
          *snapshot.controller_goal_id, *snapshot.controller_goal_generation);
    };
    ASSERT_TRUE(replay().ok());
    EXPECT_EQ(backend.commands.load(), 1);
    EXPECT_FALSE(gripper.CancelPreparedCapabilityCommand(snapshot.preparation_id).ok());
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(accepted.value().operation_id);
      return state.ok() && state.value().state == OperationStateKind::kStopped;
    }));
    EXPECT_EQ(gripper.get_recovery_assessment().execution_id, snapshot.preparation_id);
    EXPECT_EQ(gripper.get_recovery_assessment().status, RecoveryAssessmentStatus::kRetryReady);
    ASSERT_TRUE(replay().ok());  // lost response cannot re-emit after certified Stop
    EXPECT_EQ(backend.commands.load(), 1);
    EXPECT_EQ(backend.invalidations.load(), 2);  // physical containment + local settlement

    // A separate, never-dispatched preparation can be cancelled and replayed
    // without touching the backend, even when its preparation number matches.
    GripperParticipantEngine cancelled(backend, clock);
    const ResolvedPreparedCapabilityCommandExecutionRequestDTO request{
        snapshot.participant,   row.binding,
        *row.stop_readiness,    row.retained_default_stop_limits,
        row.stop_node_category, backend.last_request};
    const auto pending = cancelled.PrepareCapabilityCommand(request);
    ASSERT_TRUE(pending.ok());
    ASSERT_TRUE(cancelled.CancelPreparedCapabilityCommand(pending.value()).ok());
    const auto exact = cancelled.PrepareCapabilityCommand(request);
    ASSERT_TRUE(exact.ok());
    EXPECT_EQ(exact.value(), pending.value());
    EXPECT_EQ(cancelled.fence_addressed_participant_stop(pending.value()),
              AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch);
    EXPECT_FALSE(cancelled
                     .DispatchPreparedCapabilityCommand(
                         pending.value(), snapshot.participant->prepared_contract_digest,
                         *snapshot.controller_goal_id, *snapshot.controller_goal_generation)
                     .ok());
    EXPECT_EQ(backend.commands.load(), 1);
    EXPECT_EQ(backend.invalidations.load(), 2);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     HeldPreparedJournalWriteCannotReleaseASecondPreparationAdvance) {
  for (const bool command : {false, true}) {
    SCOPED_TRACE(command);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    const ProgramCommandSchemaRefDTO schema{
        CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
        CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
    const auto binding = command ? make_binding("gripper", "gripper.target", "tool.gripper.command",
                                                CoordinateKind::kLinear, schema)
                                 : make_binding("arm", "arm", "execution.trajectory.execute",
                                                CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(binding, engine)}, clock, scheduler, journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    HeldSettlementCall prepared_write;
    journal.record_hook = [&, hold_prepared_write = prepared_write.Blocker(),
                           command](std::string_view kind) {
      if (kind ==
          (command ? "state:subordinate_command_prepared" : "state:subordinate_motion_prepared"))
        hold_prepared_write();
    };
    auto executing = std::async(std::launch::async, [&] {
      return kernel.execute(command ? command_operation() : motion_operation());
    });
    ASSERT_TRUE(prepared_write.Entered());
    auto observing =
        std::async(std::launch::async, [&] { return kernel.reconcile_subordinate_state(); });
    EXPECT_EQ(observing.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    prepared_write.Release();
    const auto admitted = executing.get();
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    const auto observed = observing.get();
    EXPECT_TRUE(observed.ok()) << observed.error().message;
    EXPECT_EQ(engine.cancel_prepared_motion_calls.load(), 0);
    EXPECT_EQ(engine.cancel_prepared_command_calls.load(), 0);
    EXPECT_EQ(engine.dispatch_prepared_motion_calls.load(), command ? 0 : 1);
    EXPECT_EQ(engine.dispatch_prepared_command_calls.load(), command ? 1 : 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     CommandReturnSettlesStopWhileSiblingJournalWriteRemainsHeld) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine motion_engine, command_engine;
  FakeOperationJournal journal;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto motion =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto command =
      coordinated_occurrence(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                          CoordinateKind::kLinear, schema),
                             "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(motion, motion_engine), registration(command, command_engine)}, clock,
      scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall dispatch_return, observation_write;
  command_engine.dispatch_prepared_command_hook = dispatch_return.Blocker();
  journal.record_hook = [&, hold_observation_write =
                                observation_write.Blocker()](std::string_view kind) {
    if (kind == "state:subordinate_state_observed") hold_observation_write();
  };
  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(coordinated_motion_command_operation("command.held-sibling"));
  });
  ASSERT_TRUE(dispatch_return.Entered());
  auto observing = std::async(std::launch::async, [&] {
    motion_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  });
  ASSERT_TRUE(observation_write.Entered());
  const auto stopped = kernel.full_stop(FullStopIntentDTO{
      {RequestId("stop.held-sibling"), RequestOwnerEpoch("kernel.epoch.1"), "test", {}},
      StopGeneration(0U),
      {}});
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_TRUE(wait_until(
      [&] {
        const auto status = kernel.get_stop_status(stopped.value().stop_id);
        return status.ok() &&
               std::count_if(
                   status.value().attempted_stage_results.begin(),
                   status.value().attempted_stage_results.end(), [](const auto& attempt) {
                     return attempt.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                            attempt.result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
                            !attempt.result.error.has_value();
                   }) == 2;
      },
      std::chrono::seconds(1)));
  dispatch_return.Release();
  const bool settled_before_write = wait_until(
      [&] {
        const auto status = kernel.get_stop_status(stopped.value().stop_id);
        return status.ok() && status.value().stop_certificate.has_value();
      },
      std::chrono::seconds(1));
  EXPECT_TRUE(settled_before_write);
  observation_write.Release();
  observing.get();
  const auto admitted = executing.get();
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(kernel.get_operation_state(admitted.value().operation_id).value().state,
            OperationStateKind::kStopped);
  EXPECT_EQ(command_engine.participant_stop_settlement_calls.load(), 1);
  EXPECT_EQ(command_engine.cancel_prepared_command_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     LinearGripperCommandUsesResolvedTargetAndReconcilesCompletion) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto accepted = kernel.execute(command_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(engine.last_command.has_value());
  EXPECT_EQ(engine.last_command->gripper_id, "gripper.target");
  EXPECT_EQ(engine.last_command->group_id, "mf2_gripper_group");
  EXPECT_EQ(engine.last_command->joint_name, "mf2_finger_joint");
  EXPECT_DOUBLE_EQ(engine.last_command->target_position, 0.02);
  ASSERT_TRUE(engine.last_command->max_effort.has_value());
  EXPECT_DOUBLE_EQ(*engine.last_command->max_effort, 8.0);
  EXPECT_DOUBLE_EQ(engine.last_command->duration_s, 0.5);
  EXPECT_DOUBLE_EQ(engine.last_command->completion_timeout_s, 2.0);
  EXPECT_EQ(engine.validate_command_calls, 2);
  EXPECT_EQ(engine.prepare_command_calls, 1);
  EXPECT_EQ(engine.dispatch_prepared_command_calls, 1);
  ASSERT_TRUE(engine.prepared_command_snapshot.has_value());
  ASSERT_TRUE(engine.prepared_command_snapshot->participant);
  EXPECT_TRUE(std::holds_alternative<PreparedCapabilityCommandDTO>(
      engine.prepared_command_snapshot->participant->payload));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);

  const auto replay = kernel.execute(command_operation());
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.prepare_command_calls, 1);
  EXPECT_EQ(engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(engine.command_calls, 1);

  engine.gripper_status.state = GripperCommandState::kCompleted;
  engine.gripper_status.progress = 1.0;
  const auto reconciled = kernel.reconcile_subordinate_state();
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kCompleted);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReconciliationSkipsCapabilityCommandWhilePreparedDispatchEffectIsInFlight) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.gripper_status.state = GripperCommandState::kIdle;
  std::promise<void> dispatch_entered;
  std::promise<void> release_dispatch;
  const auto release_future = release_dispatch.get_future().share();
  engine.dispatch_prepared_command_hook = [&] {
    dispatch_entered.set_value();
    release_future.wait();
  };
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  auto executing =
      std::async(std::launch::async, [&] { return kernel.execute(command_operation()); });
  ASSERT_EQ(dispatch_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  const OperationId operation_id("operation.kernel.epoch.1.1");
  const auto before_reconciliation = kernel.get_operation_state(operation_id);
  ASSERT_TRUE(before_reconciliation.ok()) << before_reconciliation.error().message;
  const auto status_polls_before = engine.get_gripper_status_calls.load();

  const auto reconciled_while_dispatching = kernel.reconcile_subordinate_state();
  ASSERT_TRUE(reconciled_while_dispatching.ok()) << reconciled_while_dispatching.error().message;
  EXPECT_EQ(engine.get_gripper_status_calls.load(), status_polls_before);
  const auto unchanged = kernel.get_operation_state(operation_id);
  ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
  EXPECT_EQ(unchanged.value(), before_reconciliation.value());

  release_dispatch.set_value();
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().operation_id, operation_id);
  engine.gripper_status.state = GripperCommandState::kCompleted;
  engine.gripper_status.command_id = engine.next_command_result.command_id;
  engine.gripper_status.generation = engine.next_command_result.generation;
  engine.gripper_status.progress = 1.0;

  const auto reconciled_completion = kernel.reconcile_subordinate_state();
  ASSERT_TRUE(reconciled_completion.ok()) << reconciled_completion.error().message;
  EXPECT_EQ(engine.get_gripper_status_calls.load(), status_polls_before + 1);
  const auto completed = kernel.get_operation_state(operation_id);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  EXPECT_EQ(completed.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(completed.value().participants.front().state, OperationStateKind::kCompleted);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopDuringCapabilityCommandPrepareCancelsWithoutDispatchOrStopStage) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  std::promise<void> prepare_entered;
  std::promise<void> release_prepare;
  auto release_future = release_prepare.get_future().share();
  engine.prepare_command_hook = [&] {
    prepare_entered.set_value();
    release_future.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  auto executing =
      std::async(std::launch::async, [&] { return kernel.execute(command_operation()); });
  ASSERT_EQ(prepare_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  const OperationAcceptedDTO expected{RequestId("request.command.1"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  const auto stopped = kernel.stop(stop_request("request.command.prepare.stop", expected, 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  release_prepare.set_value();
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(wait_until([&] { return engine.cancel_prepared_command_calls.load() > 0; }));
  EXPECT_EQ(engine.cancel_prepared_command_calls.load(), 1);
  EXPECT_EQ(engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(engine.command_calls, 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
  EXPECT_EQ(certificate.value().scope_entries.front().dispatch_state,
            StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
  EXPECT_TRUE(certificate.value().attempted_stage_results.empty());
}

TEST(ResourceAddressedExecutionKernelTest,
     StopWaitingOnCommandPreparationIsWokenWhenThePreparationBorrowEnds) {
  // A Stop accepted during the Prepare call has no product to cancel yet; it
  // waits for that call and the following prepared read to end. Ending the
  // last borrow is that Stop's only remaining wakeup: here the read fails, so
  // no dispatch barrier runs, and the manual clock never expires the Stop.
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  engine.observe_prepared_command_failure =
      ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
               "injected prepared command read failure",
               {{"reason", "injected_prepared_command_read_failure"}}};
  std::promise<void> prepare_entered;
  std::promise<void> release_prepare;
  auto release_future = release_prepare.get_future().share();
  engine.prepare_command_hook = [&] {
    prepare_entered.set_value();
    release_future.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  auto executing =
      std::async(std::launch::async, [&] { return kernel.execute(command_operation()); });
  ASSERT_EQ(prepare_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  const OperationAcceptedDTO expected{RequestId("request.command.1"),
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      OperationId("operation.kernel.epoch.1.1"),
                                      OperationRevision(1U),
                                      {},
                                      0};
  const auto stopped = kernel.stop(stop_request("request.command.prepare.stop", expected, 1U));
  if (!stopped.ok()) release_prepare.set_value();
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  // The first Stop pass publishes its scope in the same lock hold in which it
  // found the Prepare call in flight and chose to wait for it.
  const bool stop_waits_on_prepare = wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && !status.value().scope_entries.empty();
  });
  release_prepare.set_value();
  ASSERT_TRUE(stop_waits_on_prepare);
  const auto accepted = executing.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(engine.observe_prepared_command_calls.load(), 1);

  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  EXPECT_EQ(engine.cancel_prepared_command_calls.load(), 1);
  EXPECT_EQ(engine.dispatch_prepared_command_calls, 0);
  EXPECT_EQ(engine.command_calls, 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  // The injected read failure is retained as participant evidence only when it
  // is recorded before the Stop's settling pass, so the certificate result is
  // not asserted here; settlement without any effect is.
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  ASSERT_EQ(certificate.value().scope_entries.size(), 1U);
  EXPECT_EQ(certificate.value().scope_entries.front().dispatch_state,
            StopScopeDispatchStateDTO::kNeverDispatchedCancelled);
  EXPECT_TRUE(certificate.value().attempted_stage_results.empty());
}

TEST(ResourceAddressedExecutionKernelTest,
     CapabilityCommandPreviewValidatesWithoutReservationJournalOrDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.gripper_status.state = GripperCommandState::kIdle;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto preview = kernel.preview_capability_command(command_operation());
  ASSERT_TRUE(preview.ok()) << preview.error().message;
  EXPECT_EQ(preview.value().binding.program_id.value(), "program.pick");
  EXPECT_EQ(preview.value().binding.command_target_id.value(), "gripper.target");
  EXPECT_EQ(preview.value().command_request.joint_name, "mf2_finger_joint");
  EXPECT_DOUBLE_EQ(preview.value().command_request.target_position, 0.02);
  EXPECT_EQ(engine.validate_command_calls, 1);
  EXPECT_EQ(engine.command_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(journal.calls, (std::vector<std::string>{"begin"}));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest, CapabilityCommandPreviewRejectsBusyTargetWithoutEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto preview = kernel.preview_capability_command(command_operation());
  ASSERT_FALSE(preview.ok());
  EXPECT_EQ(preview.error().details.at("reason"), "gripper_busy");
  EXPECT_EQ(engine.command_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(journal.calls, (std::vector<std::string>{"begin"}));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest, CapabilityPreviewAllowsIndependentActiveArm) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine arm;
  FakeParticipantEngine gripper;
  gripper.gripper_status.state = GripperCommandState::kIdle;
  FakeOperationJournal journal;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  ResourceAddressedExecutionKernel kernel(
      {registration(
           make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
           arm),
       registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    gripper)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  ASSERT_TRUE(kernel.execute(motion_operation("active.arm")).ok());
  const auto preview = kernel.preview_capability_command(command_operation("preview.gripper"));
  ASSERT_TRUE(preview.ok()) << preview.error().message;
  EXPECT_EQ(gripper.validate_command_calls, 1);
  EXPECT_EQ(gripper.command_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     GripperValidationPrecedesAcceptanceAndScopedStopResetUseCommandLifecycle) {
  FakeClock clock;
  ManualScheduler scheduler;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};

  FakeParticipantEngine rejected_engine;
  rejected_engine.next_command_validation = Result<void>::Err(
      ErrorDTO{"gripper_position_limit_violation", "outside retained limits", {}});
  FakeOperationJournal rejected_journal;
  ResourceAddressedExecutionKernel rejected_kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    rejected_engine)},
      clock, scheduler, rejected_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto rejected = rejected_kernel.execute(command_operation());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "gripper_position_limit_violation");
  EXPECT_EQ(rejected_engine.validate_command_calls, 1);
  EXPECT_EQ(rejected_engine.command_calls, 0);
  EXPECT_EQ(rejected_journal.calls, (std::vector<std::string>{"begin"}));
  EXPECT_EQ(rejected_kernel.resource_snapshot().front().state, ResourceSlotState::kFree);

  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto accepted = kernel.execute(command_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  const auto stopped = kernel.stop(stop_request("request.command.stop", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(accepted.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kStopped;
  }));
  EXPECT_EQ(engine.stop_gripper_calls, 0);
  EXPECT_EQ(engine.stop_calls, 1);
  EXPECT_EQ(engine.translate_stop_stage_calls, 1);
  EXPECT_EQ(engine.participant_stop_settlement_calls, 1);
  const auto stopped_recovery =
      kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(stopped_recovery.ok()) << stopped_recovery.error().message;
  EXPECT_EQ(stopped_recovery.value().assessment.recovery_class,
            RecoveryClass::kOrdinaryOperatorStop);
  EXPECT_EQ(stopped_recovery.value().assessment.status, RecoveryAssessmentStatus::kRetryReady);
  EXPECT_TRUE(stopped_recovery.value().assessment.backend_stop_confirmed);
  const auto stopped_state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(stopped_state.ok()) << stopped_state.error().message;
  EXPECT_EQ(stopped_state.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  const auto reconciled_stop = kernel.reconcile_subordinate_state();
  ASSERT_TRUE(reconciled_stop.ok()) << reconciled_stop.error().message;
  const auto retained_stop = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(retained_stop.ok()) << retained_stop.error().message;
  EXPECT_EQ(retained_stop.value().state, OperationStateKind::kStopped);

  const auto reset = kernel.reset(control_request(
      "request.command.reset", accepted.value(), stopped_state.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    return engine.reset_gripper_calls.load() == 1 &&
           kernel.resource_snapshot().front().state == ResourceSlotState::kFree;
  }));
}

TEST(ResourceAddressedExecutionKernelTest,
     OperationProjectionReportsResidencyUntilAcknowledgementEffectsFinish) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation("request.settlement.first"));
  ASSERT_TRUE(executed.ok()) << executed.error().message << ": "
                             << executed.error().details.at("reason");
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);

  std::promise<void> acknowledgement_entered_promise;
  auto acknowledgement_entered = acknowledgement_entered_promise.get_future();
  std::promise<void> release_acknowledgement_promise;
  auto release_acknowledgement = release_acknowledgement_promise.get_future().share();
  engine.acknowledge_effect_hook = [&] {
    acknowledgement_entered_promise.set_value();
    release_acknowledgement.wait();
  };

  const auto acknowledged =
      kernel.acknowledge_completed(control_request("request.settlement.ack", executed.value(), 1U));
  if (!acknowledged.ok()) {
    release_acknowledgement_promise.set_value();
    FAIL() << acknowledged.error().message;
  }
  if (acknowledgement_entered.wait_for(1s) != std::future_status::ready) {
    release_acknowledgement_promise.set_value();
    FAIL() << "the injected acknowledgement effect did not start";
  }

  const auto resident = kernel.get_operation_projection(executed.value().operation_id);
  const auto current_run = kernel.get_latest_run_operations({ProgramRunId("run.1")});
  const auto overlapping = kernel.execute(motion_operation("request.settlement.too-early"));
  release_acknowledgement_promise.set_value();

  ASSERT_TRUE(resident.ok()) << resident.error().message;
  ASSERT_EQ(current_run.size(), 1U);
  EXPECT_EQ(current_run.front(), resident.value());
  EXPECT_TRUE(resident.value().resident);
  ASSERT_FALSE(overlapping.ok());
  EXPECT_EQ(overlapping.error().details.at("reason"), "participant_engine_busy");
  ASSERT_TRUE(wait_until([&] {
    const auto projection = kernel.get_operation_projection(executed.value().operation_id);
    return projection.ok() && !projection.value().resident;
  }));
  const auto historical = kernel.get_operation_projection(executed.value().operation_id);
  ASSERT_TRUE(historical.ok()) << historical.error().message;
  EXPECT_FALSE(historical.value().resident);
  EXPECT_EQ(historical.value().operation.state, OperationStateKind::kCompleted);
  EXPECT_EQ(historical.value().operation.operation_revision,
            acknowledged.value().operation_revision_after_acceptance);
  EXPECT_EQ(kernel.get_latest_run_operations({ProgramRunId("run.1")}),
            std::vector<OperationReadProjectionDTO>{historical.value()});

  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  const auto next = kernel.execute(motion_operation("request.settlement.next"));
  ASSERT_TRUE(next.ok()) << next.error().message;
  const auto next_run = kernel.get_latest_run_operations({ProgramRunId("run.1")});
  ASSERT_EQ(next_run.size(), 1U);
  EXPECT_TRUE(next_run.front().resident);
  EXPECT_EQ(next_run.front().operation.operation_id, next.value().operation_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     RejectedOrThrowingSettlementRetainsOneVisibleFaultWithoutAutomaticRetry) {
  for (const bool initial_reset : {false, true}) {
    for (const bool throws : {false, true}) {
      for (const auto persistence : {"recorded", "rejected", "threw"}) {
        SCOPED_TRACE(std::string(persistence) + (throws ? "/effect-threw" : "/effect-rejected"));
        FakeClock clock;
        ManualScheduler scheduler;
        FakeParticipantEngine engine;
        FakeOperationJournal journal;
        ResourceAddressedExecutionKernel kernel(
            {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                       CoordinateKind::kAngular),
                          engine)},
            clock, scheduler, journal,
            {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
        const auto executed = kernel.execute(motion_operation("request.failed-settlement"));
        ASSERT_TRUE(executed.ok()) << executed.error().message;
        if (initial_reset) {
          engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
          ASSERT_TRUE(
              kernel.stop(stop_request("request.failed-settlement.stop", executed.value(), 1U))
                  .ok());
          ASSERT_TRUE(wait_until([&] {
            const auto state = kernel.get_operation_projection(executed.value().operation_id);
            return state.ok() && state.value().operation.state == OperationStateKind::kStopped;
          }));
          ASSERT_TRUE(wait_until([&] { return journal.stop_terminal_calls.load() == 1; }));
        } else {
          engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
        }
        if (throws) {
          auto& hook = initial_reset ? engine.reset_effect_hook : engine.acknowledge_effect_hook;
          hook = [] { throw std::runtime_error("settlement threw"); };
        } else {
          if (initial_reset)
            engine.next_reset_result = {false,
                                        ErrorDTO{"settlement_rejected", "settlement rejected", {}}};
          else
            engine.next_acknowledge_result = {
                false, ErrorDTO{"settlement_rejected", "settlement rejected", {}}};
        }
        const std::string failure_kind = initial_reset
                                             ? "state:operation_reset_failed"
                                             : "state:operation_acknowledge_completed_failed";
        std::atomic_int failure_records{};
        journal.record_hook = [&](std::string_view kind) {
          if (kind != failure_kind) return;
          ++failure_records;
          if (std::string_view(persistence) == "threw")
            throw std::runtime_error("journal unavailable");
        };
        if (std::string_view(persistence) == "rejected") {
          journal.fail_next_kind = failure_kind;
        }
        const auto before = kernel.get_operation_projection(executed.value().operation_id);
        ASSERT_TRUE(before.ok());
        const auto request = control_request("request.failed-settlement.cleanup", executed.value(),
                                             before.value().operation.operation_revision.value());
        const auto cleanup =
            initial_reset ? kernel.reset(request) : kernel.acknowledge_completed(request);
        ASSERT_TRUE(cleanup.ok()) << cleanup.error().message;
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_operation_projection(executed.value().operation_id);
          if (!state.ok() ||
              state.value().operation.state != OperationStateKind::kInterventionRequired ||
              failure_records.load() != 1)
            return false;
          const auto& failure = state.value().operation.participants.front().terminal_error;
          return failure.has_value() && (std::string_view(persistence) == "recorded" ||
                                         failure->details.contains("settlement_fault_persistence"));
        }));
        const auto failed = kernel.get_operation_projection(executed.value().operation_id);
        ASSERT_TRUE(failed.ok());
        ASSERT_TRUE(failed.value().resident);
        ASSERT_TRUE(failed.value().operation.participants.front().terminal_error.has_value());
        const auto& details = failed.value().operation.participants.front().terminal_error->details;
        if (std::string_view(persistence) == "recorded") {
          EXPECT_FALSE(details.contains("settlement_fault_persistence"));
        } else {
          EXPECT_EQ(details.at("settlement_fault_persistence"), "failed");
          EXPECT_FALSE(details.at("persistence_error_code").empty());
        }
        EXPECT_EQ(failure_records.load(), 1);
        const auto terminal_records = journal.stop_terminal_calls.load();
        const auto stopped =
            kernel.full_stop(FullStopIntentDTO{{RequestId("request.failed-settlement.full-stop"),
                                                RequestOwnerEpoch("kernel.epoch.1"),
                                                "test",
                                                {}},
                                               StopGeneration(0U),
                                               {}});
        ASSERT_TRUE(stopped.ok()) << stopped.error().message;
        ASSERT_TRUE(wait_until([&] {
          const auto current = kernel.get_operation_projection(executed.value().operation_id);
          return current.ok() && current.value().operation.state == OperationStateKind::kStopped &&
                 journal.stop_terminal_calls.load() == terminal_records + 1;
        }));
        const auto contained = kernel.get_operation_projection(executed.value().operation_id);
        ASSERT_TRUE(contained.ok());
        ASSERT_TRUE(contained.value().resident);
        EXPECT_EQ(contained.value().operation.participants.front().terminal_error,
                  failed.value().operation.participants.front().terminal_error);
        std::size_t journal_size;
        {
          std::lock_guard lock(journal.mutex);
          journal_size = journal.calls.size();
        }
        for (int tick = 0; tick < 1000; ++tick) {
          ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
          const auto repeated = kernel.acknowledge_completed(control_request(
              "request.failed-settlement.retry." + std::to_string(tick), executed.value(),
              contained.value().operation.operation_revision.value()));
          EXPECT_FALSE(repeated.ok());
          const auto current = kernel.get_operation_projection(executed.value().operation_id);
          ASSERT_TRUE(current.ok());
          EXPECT_TRUE(current.value().resident);
          EXPECT_EQ(current.value().operation.participants.front().terminal_error,
                    failed.value().operation.participants.front().terminal_error);
        }
        EXPECT_EQ(engine.acknowledge_calls.load(), initial_reset ? 0 : 1);
        EXPECT_EQ(engine.certified_reset_calls.load(), initial_reset ? 1 : 0);
        EXPECT_EQ(failure_records.load(), 1);
        {
          std::lock_guard lock(journal.mutex);
          EXPECT_EQ(journal.calls.size(), journal_size);
        }
        engine.reset_effect_hook = {};
        engine.next_reset_result = {true, std::nullopt};
        const auto reset = kernel.reset(
            control_request("request.failed-settlement.explicit-reset", executed.value(),
                            contained.value().operation.operation_revision.value()));
        ASSERT_TRUE(reset.ok()) << reset.error().message;
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_operation_projection(executed.value().operation_id);
          return state.ok() && !state.value().resident;
        }));
        engine.next_execution_id = 42U;
        engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
        const auto next = kernel.execute(motion_operation("request.failed-settlement.next"));
        ASSERT_FALSE(next.ok());
        EXPECT_EQ(next.error().details.at("reason"), "motion_admission_closed");
        // An operation Reset must not clear the independent Full Stop latch.
      }
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     SettlementFaultFencesOnlyItsResourcesUntilExplicitDurableReset) {
  for (const bool initial_reset : {false, true}) {
    for (const bool fault_write_fails : {false, true}) {
      SCOPED_TRACE(initial_reset);
      SCOPED_TRACE(fault_write_fails);
      FakeClock clock;
      ManualScheduler scheduler;
      FakeParticipantEngine first_engine, second_engine;
      FakeOperationJournal journal;
      const auto first =
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
      ResourceAddressedExecutionKernel kernel(
          {registration(first, first_engine),
           registration(coordinated_occurrence(first, "2"), second_engine)},
          clock, scheduler, journal,
          {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
      const auto executed = kernel.execute(motion_operation("request.scoped-fault"));
      ASSERT_TRUE(executed.ok()) << executed.error().message;
      if (initial_reset) {
        first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
        ASSERT_TRUE(
            kernel.stop(stop_request("request.scoped-fault.stop", executed.value(), 1U)).ok());
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_operation_state(executed.value().operation_id);
          return state.ok() && state.value().state == OperationStateKind::kStopped &&
                 journal.stop_terminal_calls.load() == 1;
        }));
        first_engine.next_reset_result = {
            false, ErrorDTO{"settlement_rejected", "settlement rejected", {}}};
      } else {
        first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
        first_engine.next_acknowledge_result = {
            false, ErrorDTO{"settlement_rejected", "settlement rejected", {}}};
      }
      if (fault_write_fails)
        journal.fail_next_kind = initial_reset ? "state:operation_reset_failed"
                                               : "state:operation_acknowledge_completed_failed";
      const auto before = kernel.get_operation_state(executed.value().operation_id);
      ASSERT_TRUE(before.ok());
      const auto request = control_request("request.scoped-fault.cleanup", executed.value(),
                                           before.value().operation_revision.value());
      ASSERT_TRUE(
          (initial_reset ? kernel.reset(request) : kernel.acknowledge_completed(request)).ok());
      ASSERT_TRUE(wait_until([&] {
        const auto state = kernel.get_operation_state(executed.value().operation_id);
        return state.ok() && state.value().state == OperationStateKind::kInterventionRequired;
      }));
      const auto blocked = kernel.execute(motion_operation("request.scoped-fault.blocked"));
      ASSERT_FALSE(blocked.ok());
      EXPECT_NE(blocked.error().details.at("reason"), "motion_admission_closed");
      auto disjoint = motion_operation("request.scoped-fault.disjoint");
      disjoint.program_run_id = ProgramRunId("run.2");
      disjoint.participants.front().component_role_id = ComponentRoleId("robot_2");
      disjoint.participants.front().target_role_id = TargetRoleId("arm_2");
      const auto concurrent = kernel.execute(std::move(disjoint));
      ASSERT_TRUE(concurrent.ok()) << concurrent.error().message;
      const auto failed = kernel.get_operation_state(executed.value().operation_id);
      ASSERT_TRUE(failed.ok());
      first_engine.next_reset_result = {true, std::nullopt};
      // Even after a failed fault write, recovery must first persist the exact
      // fault-bearing state. A failed acceptance cannot issue cleanup effects.
      journal.fail_next_kind = "control_accepted";
      const auto reset_request =
          control_request("request.scoped-fault.reset-rejected", executed.value(),
                          failed.value().operation_revision.value());
      const auto rejected = kernel.reset(reset_request);
      ASSERT_FALSE(rejected.ok());
      EXPECT_EQ(first_engine.certified_reset_calls.load(), initial_reset ? 1 : 0);
      const auto reset =
          kernel.reset(control_request("request.scoped-fault.reset", executed.value(),
                                       failed.value().operation_revision.value()));
      ASSERT_TRUE(reset.ok()) << reset.error().message;
      ASSERT_TRUE(wait_until([&] {
        const auto state = kernel.get_operation_projection(executed.value().operation_id);
        return state.ok() && !state.value().resident;
      }));
      first_engine.next_execution_id = 42U;
      first_engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
      const auto next = kernel.execute(motion_operation("request.scoped-fault.next"));
      ASSERT_TRUE(next.ok()) << next.error().message;
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest, HeldCleanupFaultWriteCannotDelayDisjointStop) {
  using namespace std::chrono_literals;
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  FakeOperationJournal journal;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall fault_write;
  const auto first_operation = kernel.execute(motion_operation("request.held-fault.first"));
  ASSERT_TRUE(first_operation.ok());
  auto other_intent = motion_operation("request.held-fault.other");
  other_intent.program_run_id = ProgramRunId("run.2");
  other_intent.participants.front().component_role_id = ComponentRoleId("robot_2");
  other_intent.participants.front().target_role_id = TargetRoleId("arm_2");
  const auto other = kernel.execute(std::move(other_intent));
  ASSERT_TRUE(other.ok()) << other.error().message;
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  first_engine.next_acknowledge_result = {false,
                                          ErrorDTO{"cleanup_rejected", "cleanup rejected", {}}};
  journal.record_hook = [&, hold_fault_write = fault_write.Blocker()](std::string_view kind) {
    if (kind == "state:operation_acknowledge_completed_failed") hold_fault_write();
  };
  ASSERT_TRUE(kernel
                  .acknowledge_completed(
                      control_request("request.held-fault.ack", first_operation.value(), 1U))
                  .ok());
  ASSERT_TRUE(fault_write.Entered());
  const auto before_reconfigure = kernel.resource_snapshot();
  const auto reconfigured = kernel.reconfigure_runtime_participants(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)});
  ASSERT_FALSE(reconfigured.ok());
  EXPECT_EQ(reconfigured.error().details.at("reason"), "operation_control_reconfiguration_busy");
  EXPECT_EQ(kernel.resource_snapshot(), before_reconfigure);
  auto stopping = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.held-fault.stop-other", other.value(), 1U));
  });
  const auto accepted_before_write = stopping.wait_for(1s) == std::future_status::ready;
  EXPECT_TRUE(accepted_before_write);
  if (accepted_before_write) {
    EXPECT_TRUE(wait_until([&] { return second_engine.translate_stop_stage_calls.load() == 1; }));
  }
  fault_write.Release();
  const auto stopped = stopping.get();
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(first_engine.certified_reset_calls.load(), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     HeldObservationWriteCannotDelayOverlappingStopOrRestoreItsOldState) {
  using namespace std::chrono_literals;
  for (const bool full_stop : {false, true}) {
    SCOPED_TRACE(full_stop ? "Full Stop" : "operation Stop");
    for (const bool scale_observation : {false, true}) {
      SCOPED_TRACE(scale_observation ? "scale observation" : "lifecycle observation");
      FakeClock clock;
      ManualScheduler scheduler;
      FakeParticipantEngine engine;
      FakeOperationJournal journal;
      ResourceAddressedExecutionKernel kernel(
          {registration(
              make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
              engine)},
          clock, scheduler, journal,
          {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
      const auto operation = kernel.execute(motion_operation("request.held-observation"));
      ASSERT_TRUE(operation.ok());
      engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
      ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());

      HeldSettlementCall held_write;
      journal.record_hook = [&, hold_held_write = held_write.Blocker(),
                             scale_observation](std::string_view kind) {
        if (kind == (scale_observation ? "state:runtime_scale_status_observed"
                                       : "state:subordinate_state_observed")) {
          hold_held_write();
        }
      };
      auto observing = std::async(std::launch::async, [&] {
        if (scale_observation) {
          engine.runtime_scale.applied_scale = 0.6;
          return kernel.reconcile_subordinate_state();
        }
        engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
        return Result<void>::Ok();
      });
      const bool entered = held_write.Entered();
      EXPECT_TRUE(entered);
      if (!entered) {
        held_write.Release();
        static_cast<void>(observing.get());
        continue;
      }
      auto stopping = std::async(std::launch::async, [&] {
        if (full_stop)
          return kernel.full_stop(
              FullStopIntentDTO{{RequestId("request.held-observation.full-stop"),
                                 RequestOwnerEpoch("kernel.epoch.1"),
                                 "test",
                                 {}},
                                StopGeneration(0U),
                                {}});
        return kernel.stop(stop_request("request.held-observation.stop", operation.value(), 1U));
      });
      const bool accepted_while_held = stopping.wait_for(1s) == std::future_status::ready;
      EXPECT_TRUE(accepted_while_held);
      if (accepted_while_held) {
        EXPECT_TRUE(wait_until([&] {
          const auto current = kernel.get_operation_state(operation.value().operation_id);
          return engine.translate_stop_stage_calls.load() == 1 &&
                 engine.participant_stop_settlement_calls.load() == 1 && current.ok() &&
                 current.value().state == OperationStateKind::kStopped;
        }));
      }
      // Always release before joining: a regression must fail, not hang the suite.
      held_write.Release();
      static_cast<void>(observing.get());
      const auto stopped = stopping.get();
      ASSERT_TRUE(stopped.ok()) << stopped.error().message;
      const auto current = kernel.get_operation_state(operation.value().operation_id);
      ASSERT_TRUE(current.ok());
      EXPECT_EQ(current.value().active_stop_id, stopped.value().stop_id);
      EXPECT_NE(current.value().state, OperationStateKind::kCompleted);
      EXPECT_EQ(
          current.value().operation_revision,
          stopped.value().affected_operation_revisions.front().operation_revision_after_acceptance);
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ConcurrentParticipantCapturesKeepBothUpdatesAtAnUnchangedClock) {
  for (const bool second_first : {false, true}) {
    SCOPED_TRACE(second_first);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    FakeOperationJournal journal;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine),
         registration(coordinated_occurrence(first, "2"), second_engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto operation =
        kernel.execute(coordinated_motion_operation("request.concurrent-captures"));
    ASSERT_TRUE(operation.ok());
    first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    const auto before = kernel.get_operation_state(operation.value().operation_id);
    ASSERT_TRUE(before.ok());
    HeldSettlementCall held_write;
    journal.record_hook = [&, hold_held_write = held_write.Blocker(),
                           held_first =
                               std::make_shared<std::atomic<unsigned>>(0)](std::string_view kind) {
      if (kind == "state:subordinate_state_observed" && !held_first->exchange(true))
        hold_held_write();
    };
    auto& early = second_first ? second_engine : first_engine;
    auto& late = second_first ? first_engine : second_engine;
    auto first_write = std::async(std::launch::async, [&] {
      early.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
    });
    const bool entered = held_write.Entered();
    EXPECT_TRUE(entered);
    if (!entered) {
      held_write.Release();
      first_write.get();
      continue;
    }
    std::promise<void> second_started;
    auto second_write = std::async(std::launch::async, [&] {
      second_started.set_value();
      late.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
    });
    second_started.get_future().wait();
    held_write.Release();
    first_write.get();
    second_write.get();
    const auto current = kernel.get_operation_state(operation.value().operation_id);
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current.value().operation_revision, before.value().operation_revision);
    EXPECT_EQ(current.value().updated_at_ns, before.value().updated_at_ns);
    EXPECT_GT(current.value().journal_capture_order, before.value().journal_capture_order);
    EXPECT_EQ(current.value().state, OperationStateKind::kCompleted);
    for (const auto& participant : current.value().participants)
      EXPECT_EQ(participant.state, OperationStateKind::kCompleted);
    std::lock_guard journal_lock(journal.mutex);
    ASSERT_FALSE(journal.states.empty());
    EXPECT_EQ(journal.states.back(), current.value());
  }
}

TEST(ResourceAddressedExecutionKernelTest, ThrownObservationWriteReleasesCommitForReconciliation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto operation = kernel.execute(motion_operation("request.thrown-capture"));
  ASSERT_TRUE(operation.ok());
  const auto before = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(before.ok());
  std::atomic_bool failed{};
  journal.record_hook = [&](std::string_view kind) {
    if (kind == "state:subordinate_state_observed" && !failed.exchange(true))
      throw std::runtime_error("injected ordinary write failure");
  };
  EXPECT_NO_THROW(engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted));
  EXPECT_EQ(kernel.get_operation_state(operation.value().operation_id).value(), before.value());
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(kernel.get_operation_state(operation.value().operation_id).value().state,
            OperationStateKind::kCompleted);
}

TEST(ResourceAddressedExecutionKernelTest, ThrownAdmissionWritesReleaseTheirNondurableClaims) {
  for (const bool coordinated : {false, true}) {
    SCOPED_TRACE(coordinated);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    FakeOperationJournal journal;
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine),
         registration(coordinated_occurrence(first, "2"), second_engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    journal.record_hook = [](std::string_view kind) {
      if (kind == "operation_accepted") throw std::runtime_error("admission write threw");
    };
    const auto rejected =
        kernel.execute(coordinated ? coordinated_motion_operation("request.throw-admission")
                                   : motion_operation("request.throw-admission"));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "operation_journal_exception");
    for (const auto& slot : kernel.resource_snapshot())
      EXPECT_EQ(slot.state, ResourceSlotState::kFree);
    EXPECT_EQ(first_engine.execute_calls, 0);
    EXPECT_EQ(second_engine.execute_calls, 0);
    journal.record_hook = {};
    const auto operation =
        kernel.execute(coordinated ? coordinated_motion_operation("request.after-throw-admission")
                                   : motion_operation("request.after-throw-admission"));
    ASSERT_TRUE(operation.ok());
    first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    if (coordinated)
      second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(operation.value().operation_id);
      return state.ok() && state.value().state == OperationStateKind::kCompleted;
    }));
    journal.record_hook = [](std::string_view kind) {
      if (kind == "control_accepted") throw std::runtime_error("control write threw");
    };
    const auto failed_control = kernel.acknowledge_completed(
        control_request("request.throw-acknowledge", operation.value(), 1U));
    ASSERT_FALSE(failed_control.ok());
    EXPECT_EQ(failed_control.error().code, "operation_journal_exception");
    const auto current = kernel.get_operation_projection(operation.value().operation_id);
    ASSERT_TRUE(current.ok());
    EXPECT_FALSE(current.value().control_pending);
    EXPECT_EQ(current.value().operation.operation_revision.value(), 1U);
    journal.record_hook = {};
    EXPECT_TRUE(kernel
                    .acknowledge_completed(control_request("request.after-throw-acknowledge",
                                                           operation.value(), 1U))
                    .ok());
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     RecoveryAndOrdinaryCommitExcludeEachOtherWithoutHoldingProviderMutex) {
  for (const bool storage_first : {false, true}) {
    SCOPED_TRACE(storage_first);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(binding, engine)}, clock, scheduler, journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto operation = kernel.execute(motion_operation("request.recovery-commit"));
    ASSERT_TRUE(operation.ok());
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
    const KernelRecoveryActionRequestDTO recovery{"request.recovery-commit.recover",
                                                  RecoveryAction::kRetryMotionFromCurrentState,
                                                  "operator.test",
                                                  1U,
                                                  1U,
                                                  41U,
                                                  "",
                                                  "",
                                                  0U,
                                                  "test"};
    engine.next_recovery_result =
        KernelRecoveryActionResultDTO{recovery.request_id, recovery.action, true, {}, std::nullopt};
    const auto scale =
        runtime_scale_request("request.recovery-commit.scale", operation.value(), 1U, 0U, 0.5);
    HeldSettlementCall held;
    if (storage_first) {
      journal.record_hook = [&, hold_held = held.Blocker()](std::string_view kind) {
        if (kind == "runtime_scale_accepted") hold_held();
      };
      auto changing =
          std::async(std::launch::async, [&] { return kernel.set_runtime_scale(scale); });
      EXPECT_TRUE(held.Entered());
      const auto blocked = kernel.apply_operation_recovery_action(operation.value().operation_id,
                                                                  OperationRevision(1U), recovery);
      EXPECT_FALSE(blocked.ok());
      EXPECT_EQ(engine.recovery_action_calls, 0);
      held.Release();
      EXPECT_TRUE(changing.get().ok());
    } else {
      engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kInterventionRequired);
      engine.recovery_action_hook = held.Blocker();
      auto recovering = std::async(std::launch::async, [&] {
        return kernel.apply_operation_recovery_action(operation.value().operation_id,
                                                      OperationRevision(1U), recovery);
      });
      EXPECT_TRUE(held.Entered());
      const auto blocked = kernel.set_runtime_scale(scale);
      EXPECT_FALSE(blocked.ok());
      const auto before = kernel.get_operation_state(operation.value().operation_id);
      engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
      EXPECT_EQ(kernel.get_operation_state(operation.value().operation_id).value(), before.value());
      const auto reads = engine.status_read_calls.load();
      const auto prepared_reads = engine.observe_prepared_motion_calls.load();
      engine.notify_prepared_motion();
      EXPECT_TRUE(kernel.reconcile_subordinate_state().ok());
      EXPECT_EQ(engine.status_read_calls.load(), reads);
      EXPECT_EQ(engine.observe_prepared_motion_calls.load(), prepared_reads);
      EXPECT_FALSE(kernel.reconfigure_runtime_participants({registration(binding, engine)}).ok());
      held.Release();
      const auto recovered = recovering.get();
      ASSERT_TRUE(recovered.ok()) << recovered.error().message;
      EXPECT_TRUE(recovered.value().accepted);
      EXPECT_FALSE(
          kernel.get_operation_projection(operation.value().operation_id).value().resident);
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest, RecoveryCannotRetireAnInitialDispatchStillReturning) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall held;
  engine.execute_return_hook = held.Blocker();
  const auto intent = motion_operation("request.recovery-dispatch");
  auto executing = std::async(std::launch::async, [&] { return kernel.execute(intent); });
  EXPECT_TRUE(held.Entered());
  const auto resident = kernel.get_resident_operations({intent.program_run_id});
  EXPECT_EQ(resident.size(), 1U);
  if (!resident.empty()) {
    const KernelRecoveryActionRequestDTO request{"request.recovery-during-dispatch",
                                                 RecoveryAction::kRetryMotionFromCurrentState,
                                                 "operator.test",
                                                 1U,
                                                 1U,
                                                 41U,
                                                 "",
                                                 "",
                                                 0U,
                                                 "test"};
    engine.next_recovery_result =
        KernelRecoveryActionResultDTO{request.request_id, request.action, true, {}, std::nullopt};
    const auto recovery = kernel.apply_operation_recovery_action(
        resident.front().operation_id, resident.front().operation_revision, request);
    EXPECT_FALSE(recovery.ok());
    EXPECT_EQ(engine.recovery_action_calls, 0);
    EXPECT_TRUE(kernel.get_operation_projection(resident.front().operation_id).value().resident);
  }
  held.Release();
  EXPECT_TRUE(executing.get().ok());
}

TEST(ResourceAddressedExecutionKernelTest,
     NewResetCannotOvertakeAContainingStopStillSettlingAnotherOperation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.reset.aggregate-first"));
  ASSERT_TRUE(first.ok());
  auto second_intent = motion_operation("request.reset.aggregate-second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  ASSERT_TRUE(kernel.execute(second_intent).ok());
  HeldSettlementCall held;
  second_engine.translate_stop_stage_hook = held.Blocker();
  const auto stopped = kernel.full_stop(FullStopIntentDTO{
      {RequestId("request.reset.aggregate-stop"), RequestOwnerEpoch("kernel.epoch.1"), "test", {}},
      StopGeneration(0U),
      {}});
  ASSERT_TRUE(stopped.ok());
  EXPECT_TRUE(held.Entered());
  EXPECT_TRUE(wait_until([&] {
    const auto current = kernel.get_operation_state(first.value().operation_id);
    return current.ok() && current.value().state == OperationStateKind::kStopped;
  }));
  const auto before = kernel.get_operation_state(first.value().operation_id);
  const auto reset =
      kernel.reset(control_request("request.reset.before-certificate", first.value(), 2U));
  EXPECT_FALSE(reset.ok());
  if (!reset.ok()) {
    EXPECT_EQ(reset.error().details.at("reason"), "operation_effect_in_flight");
  }
  EXPECT_EQ(first_engine.certified_reset_calls.load(), 0);
  EXPECT_EQ(kernel.get_operation_state(first.value().operation_id).value(), before.value());
  held.Release();
  EXPECT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  EXPECT_TRUE(
      kernel.reset(control_request("request.reset.after-certificate", first.value(), 2U)).ok());
  EXPECT_TRUE(wait_until([&] {
    const auto current = kernel.get_operation_projection(first.value().operation_id);
    return current.ok() && !current.value().resident;
  }));
}

TEST(ResourceAddressedExecutionKernelTest, RetiringUnusedEnginePreservesResidentCleanupLane) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine retired_engine, unused_engine, current_engine;
  std::mutex worker_mutex;
  std::vector<std::thread::id> cleanup_workers;
  const auto record_worker = [&] {
    std::lock_guard lock(worker_mutex);
    cleanup_workers.push_back(std::this_thread::get_id());
  };
  retired_engine.acknowledge_effect_hook = record_worker;
  retired_engine.reset_effect_hook = record_worker;
  FakeOperationJournal journal;
  const auto original =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto candidate = registration(select_candidate_product(original), current_engine);
  ResourceAddressedExecutionKernel kernel(
      {registration(original, retired_engine),
       registration(coordinated_occurrence(original, "2"), unused_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  HeldSettlementCall fault_write;
  const auto executed = kernel.execute(motion_operation("request.retired-cleanup"));
  ASSERT_TRUE(executed.ok());
  const auto replaced =
      kernel.publish_runtime_route_configuration(route_configuration_for(candidate), {candidate});
  ASSERT_TRUE(replaced.ok()) << replaced.error().message;
  EXPECT_TRUE(kernel.participant_engine_in_use(&retired_engine));
  EXPECT_FALSE(kernel.participant_engine_in_use(&unused_engine));
  retired_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  retired_engine.next_acknowledge_result = {false,
                                            ErrorDTO{"cleanup_rejected", "cleanup rejected", {}}};
  journal.record_hook = [&, hold_fault_write = fault_write.Blocker()](std::string_view kind) {
    if (kind == "state:operation_acknowledge_completed_failed") hold_fault_write();
  };
  ASSERT_TRUE(kernel
                  .acknowledge_completed(
                      control_request("request.retired-cleanup.ack", executed.value(), 1U))
                  .ok());
  ASSERT_TRUE(fault_write.Entered());
  const auto before = kernel.resource_snapshot();
  const auto busy = kernel.release_retired_engine(&unused_engine);
  ASSERT_FALSE(busy.ok());
  EXPECT_EQ(busy.error().details.at("reason"), "operation_control_reconfiguration_busy");
  EXPECT_EQ(kernel.resource_snapshot(), before);
  EXPECT_EQ(unused_engine.subscription_count(), 1U);
  fault_write.Release();
  ASSERT_TRUE(wait_until([&] { return kernel.release_retired_engine(&unused_engine).ok(); }));
  EXPECT_EQ(unused_engine.subscription_count(), 0U);
  const auto fault = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(fault.ok());
  ASSERT_EQ(fault.value().state, OperationStateKind::kInterventionRequired);
  const auto reset = kernel.reset(control_request("request.retired-cleanup.reset", executed.value(),
                                                  fault.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_projection(executed.value().operation_id);
    return state.ok() && !state.value().resident;
  }));
  EXPECT_EQ(retired_engine.certified_reset_calls.load(), 1);
  EXPECT_EQ(current_engine.certified_reset_calls.load(), 0);
  {
    std::lock_guard lock(worker_mutex);
    ASSERT_EQ(cleanup_workers.size(), 2U);
    EXPECT_EQ(cleanup_workers.front(), cleanup_workers.back())
        << "retiring an unrelated engine must preserve the resident cleanup worker";
  }
  ASSERT_TRUE(wait_until([&] { return kernel.release_retired_engine(&retired_engine).ok(); }));
  EXPECT_EQ(retired_engine.subscription_count(), 0U);
  EXPECT_EQ(current_engine.subscription_count(), 1U);
}

TEST(ResourceAddressedExecutionKernelTest, HeldResetAndFullStopHaveOneOrderedCleanupEffect) {
  for (const bool reset_returns_first : {false, true}) {
    for (const int reset_result : {0, 1, 2}) {
      for (const bool stop_fails : {false, true}) {
        const bool reset_fails = reset_result != 0;
        SCOPED_TRACE(reset_returns_first);
        SCOPED_TRACE(reset_result);
        SCOPED_TRACE(stop_fails);
        FakeClock clock;
        ManualScheduler scheduler;
        FakeParticipantEngine engine;
        FakeOperationJournal journal;
        ResourceAddressedExecutionKernel kernel(
            {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                       CoordinateKind::kAngular),
                          engine)},
            clock, scheduler, journal,
            {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
        HeldSettlementCall reset_effect, stop_settlement;
        const auto executed = kernel.execute(motion_operation("request.overlap-reset"));
        ASSERT_TRUE(executed.ok());
        engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
        engine.next_acknowledge_result = {false,
                                          ErrorDTO{"cleanup_rejected", "cleanup rejected", {}}};
        ASSERT_TRUE(kernel
                        .acknowledge_completed(
                            control_request("request.overlap-reset.ack", executed.value(), 1U))
                        .ok());
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_operation_state(executed.value().operation_id);
          return state.ok() && state.value().state == OperationStateKind::kInterventionRequired;
        }));
        const auto fault = kernel.get_operation_state(executed.value().operation_id);
        ASSERT_TRUE(fault.ok());
        engine.reset_effect_hook = [&, hold_reset_effect = reset_effect.Blocker(), reset_result] {
          hold_reset_effect();
          if (reset_result == 2) throw std::runtime_error("reset failed");
        };
        engine.participant_stop_settlement_hook = stop_settlement.Blocker();
        if (stop_fails)
          engine.participant_stop_settlement_error =
              ErrorDTO{"participant_terminal_unavailable", "terminal settlement failed", {}};
        if (reset_fails)
          engine.next_reset_result = {false,
                                      ErrorDTO{"overlap_reset_rejected", "reset rejected", {}}};
        ASSERT_TRUE(kernel
                        .reset(control_request("request.overlap-reset.reset", executed.value(),
                                               fault.value().operation_revision.value()))
                        .ok());
        ASSERT_TRUE(reset_effect.Entered());
        const auto stopped =
            kernel.full_stop(FullStopIntentDTO{{RequestId("request.overlap-reset.full-stop"),
                                                RequestOwnerEpoch("kernel.epoch.1"),
                                                "test",
                                                {}},
                                               StopGeneration(0U),
                                               {}});
        ASSERT_TRUE(stopped.ok()) << stopped.error().message;
        ASSERT_TRUE(stop_settlement.Entered());
        if (reset_returns_first) {
          reset_effect.Release();
          if (!reset_fails) {
            EXPECT_TRUE(
                wait_until([&] { return engine.get_status().state == ExecutionState::kIdle; }));
          } else {
            EXPECT_TRUE(wait_until([&] {
              const auto state = kernel.get_operation_state(executed.value().operation_id);
              if (!state.ok() || !state.value().participants.front().terminal_error.has_value())
                return false;
              const auto& failure = *state.value().participants.front().terminal_error;
              return reset_result == 1
                         ? failure.code == "overlap_reset_rejected"
                         : failure.details.contains("reason") &&
                               failure.details.at("reason") == "operation_control_effect_threw";
            }));
            if (stop_fails) {
              engine.emit_error(ErrorDTO{"stop_feedback_failed", "later Stop feedback", {}});
              engine.emit_transition(ExecutionState::kStopping, ExecutionState::kStopped);
            }
          }
          const auto retained = kernel.get_operation_projection(executed.value().operation_id);
          ASSERT_TRUE(retained.ok());
          EXPECT_TRUE(retained.value().resident);
        }
        stop_settlement.Release();
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_stop_status(stopped.value().stop_id);
          return state.ok() && state.value().state == (stop_fails ? StopStatusStateDTO::kPartial
                                                                  : StopStatusStateDTO::kSucceeded);
        }));
        const auto status = kernel.get_stop_status(stopped.value().stop_id);
        ASSERT_TRUE(status.ok());
        ASSERT_TRUE(status.value().stop_certificate.has_value());
        const auto certificate =
            kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
        ASSERT_TRUE(certificate.ok());
        reset_effect.Release();
        std::string reconfiguration_error;
        ASSERT_TRUE(wait_until([&] {
          const auto result = kernel.reconfigure_runtime_participants({registration(
              make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
              engine)});
          if (!result.ok()) reconfiguration_error = result.error().message;
          return result.ok();
        })) << reconfiguration_error;
        // Replacement is admitted only once the old completion callback has
        // consumed its exact result; do not mistake the pre-existing Stop fault
        // for proof that a successful Reset callback has finished.
        ASSERT_TRUE(wait_until([&] {
          const auto state = kernel.get_operation_projection(executed.value().operation_id);
          if (!state.ok()) return false;
          if (!reset_fails && !stop_fails) return !state.value().resident;
          if (!state.value().resident ||
              state.value().operation.state != OperationStateKind::kInterventionRequired)
            return false;
          if (!reset_fails) return true;
          const auto& failure = state.value().operation.participants.front().terminal_error;
          return failure.has_value() &&
                 (reset_result == 1
                      ? failure->code == "overlap_reset_rejected"
                      : failure->details.contains("reason") &&
                            failure->details.at("reason") == "operation_control_effect_threw");
        }));
        EXPECT_EQ(engine.certified_reset_calls.load(), 1);
        const auto unchanged =
            kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
        ASSERT_TRUE(unchanged.ok());
        EXPECT_EQ(unchanged.value(), certificate.value());
        EXPECT_EQ(kernel.get_stop_fault_latch().has_value(), stop_fails);
      }
    }
  }
}

TEST(ResourceAddressedExecutionKernelTest, VisibleCleanupFaultAlreadyHasAReusableControlLane) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation("request.control-lane"));
  ASSERT_TRUE(executed.ok());
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  engine.next_acknowledge_result = {false, ErrorDTO{"cleanup_rejected", "cleanup rejected", {}}};
  std::atomic_bool reset_accepted{};
  journal.record_hook = [&](std::string_view kind) {
    if (kind != "state:operation_acknowledge_completed_failed") return;
    const auto fault = kernel.get_operation_state(executed.value().operation_id);
    ASSERT_TRUE(fault.ok());
    // Re-enter at fault publication while the old completion callback is
    // still running. Its moved result must no longer reserve the worker lane.
    reset_accepted = kernel
                         .reset(control_request("request.control-lane.reset", executed.value(),
                                                fault.value().operation_revision.value()))
                         .ok();
  };
  ASSERT_TRUE(
      kernel
          .acknowledge_completed(control_request("request.control-lane.ack", executed.value(), 1U))
          .ok());
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_projection(executed.value().operation_id);
    return state.ok() && !state.value().resident;
  }));
  EXPECT_TRUE(reset_accepted.load());
  EXPECT_EQ(engine.acknowledge_calls.load(), 1);
  EXPECT_EQ(engine.certified_reset_calls.load(), 1);
  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  const auto next = kernel.execute(motion_operation("request.control-lane.next"));
  ASSERT_TRUE(next.ok()) << next.error().message;
}

TEST(ResourceAddressedExecutionKernelTest, CompletionCleanupCannotOvertakeInitialDispatchAndStop) {
  for (const auto feedback : {ExecutionState::kExecuting, ExecutionState::kCompleted}) {
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    HeldSettlementCall dispatch;
    engine.execute_return_hook = [&engine, feedback, hold = dispatch.Blocker()] {
      engine.emit_transition(ExecutionState::kPlanning, feedback);
      hold();
    };
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    auto executing = std::async(std::launch::async, [&] {
      return kernel.execute(motion_operation("request.superseded-settlement"));
    });
    ASSERT_TRUE(dispatch.Entered());
    const auto operations = kernel.get_resident_operations({ProgramRunId("run.1")});
    ASSERT_EQ(operations.size(), 1U);
    const auto& operation = operations.front();
    ASSERT_EQ(operation.state, feedback == ExecutionState::kCompleted
                                   ? OperationStateKind::kCompleted
                                   : OperationStateKind::kExecuting);
    const auto reset =
        kernel.reset(OperationControlRequestDTO{{RequestId("request.superseded-settlement.reset"),
                                                 RequestOwnerEpoch("kernel.epoch.1"),
                                                 "test",
                                                 {}},
                                                operation.operation_id,
                                                operation.operation_revision,
                                                {}});
    EXPECT_FALSE(reset.ok());
    if (reset.ok()) {
      dispatch.Release();
      (void)executing.get();
      return;
    }
    EXPECT_EQ(reset.error().details.at("reason"), feedback == ExecutionState::kCompleted
                                                      ? "operation_control_state_invalid"
                                                      : "operation_effect_in_flight");
    if (feedback == ExecutionState::kCompleted) {
      const auto ack = kernel.acknowledge_completed(
          OperationControlRequestDTO{{RequestId("request.superseded-settlement.ack"),
                                      RequestOwnerEpoch("kernel.epoch.1"),
                                      "test",
                                      {}},
                                     operation.operation_id,
                                     operation.operation_revision,
                                     {}});
      EXPECT_FALSE(ack.ok());
      if (ack.ok()) {
        dispatch.Release();
        (void)executing.get();
        return;
      }
      if (!ack.ok()) {
        EXPECT_EQ(ack.error().details.at("reason"), "operation_effect_in_flight");
      }
    }
    const auto stopped =
        kernel.full_stop(FullStopIntentDTO{{RequestId("request.superseded-settlement.stop"),
                                            RequestOwnerEpoch("kernel.epoch.1"),
                                            "test",
                                            {}},
                                           StopGeneration(0U),
                                           {}});
    dispatch.Release();
    const auto execution_result = executing.get();
    ASSERT_TRUE(execution_result.ok()) << execution_result.error().message;
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    EXPECT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(operation.operation_id);
      return state.ok() && state.value().state == OperationStateKind::kStopped;
    }));
    EXPECT_EQ(engine.acknowledge_calls.load(), 0);
    EXPECT_EQ(engine.certified_reset_calls.load(), 0);
    EXPECT_FALSE(kernel.get_operation_state(operation.operation_id)
                     .value()
                     .participants.front()
                     .terminal_error.has_value());
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(std::count(journal.calls.begin(), journal.calls.end(),
                         "state:operation_acknowledge_completed_failed"),
              0);
  }
}

#ifdef MANYFORGE_TEST_SQLITE_OPERATION_JOURNAL
TEST(ResourceAddressedExecutionKernelTest, SettlementFailureRemainsTraceableAfterJournalReopen) {
  TemporaryOperationJournal temporary;
  const auto database = temporary.directory / "operation.sqlite3";
  FakeClock clock;
  ManualScheduler scheduler;
  const auto epoch = RequestOwnerEpoch("kernel.epoch.1");
  std::optional<OperationId> failed_operation;
  {
    FakeParticipantEngine engine;
    SqliteOperationJournal journal(database, CellId("cell.test"), 32U);
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, journal, {epoch, CellId("cell.test"), 16U});
    const auto executed = kernel.execute(motion_operation("request.durable-settlement"));
    ASSERT_TRUE(executed.ok()) << executed.error().message;
    failed_operation = executed.value().operation_id;
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    engine.next_acknowledge_result = {false,
                                      ErrorDTO{"settlement_rejected", "settlement rejected", {}}};
    ASSERT_TRUE(kernel
                    .acknowledge_completed(
                        control_request("request.durable-settlement.ack", executed.value(), 1U))
                    .ok());
    ASSERT_TRUE(wait_until([&] {
      const auto events = journal.ReadEvents(epoch, 0U, 32U);
      return events.ok() &&
             std::any_of(
                 events.value().events.begin(), events.value().events.end(), [](const auto& event) {
                   return event.payload_json.find("operation_acknowledge_completed_failed") !=
                          std::string::npos;
                 });
    }));
    // Lose the persistence handle before destructor cleanup: the successor
    // must reconcile an unclosed owner, as after process loss.
    journal.Close();
  }
  SqliteOperationJournal reopened(database, CellId("cell.test"), 32U);
  const auto events = reopened.ReadEvents(epoch, 0U, 32U);
  ASSERT_TRUE(events.ok()) << events.error().message;
  const auto failure =
      std::find_if(events.value().events.begin(), events.value().events.end(), [](const auto& row) {
        return row.record_kind == "state_observed" &&
               row.payload_json.find("operation_acknowledge_completed_failed") != std::string::npos;
      });
  ASSERT_NE(failure, events.value().events.end());
  EXPECT_EQ(failure->operation_id, failed_operation);
  EXPECT_NE(failure->payload_json.find("settlement_rejected"), std::string::npos);
  EXPECT_NE(failure->payload_json.find("intervention_required"), std::string::npos);
  // A new process still starts with unresolved predecessor evidence, never
  // with permission to repeat the failed physical acknowledgement automatically.
  const auto recovery = reopened.BeginOwnerEpoch(RequestOwnerEpoch("kernel.epoch.2"), clock.now(),
                                                 16U, {ResourceId("arm")});
  ASSERT_TRUE(recovery.ok()) << recovery.error().message;
  EXPECT_EQ(recovery.value().replaced_unclosed_owner_epoch, epoch);
  EXPECT_FALSE(recovery.value().unreconciled_resources.empty());
  ASSERT_EQ(recovery.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovery.value().historical_operations.front().operation_id, failed_operation);
}
#endif

TEST(ResourceAddressedExecutionKernelTest,
     OperationControlsAdvanceRevisionAndAcknowledgeReleasesResources) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message << ": "
                             << executed.error().details.at("reason");
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);

  std::optional<OperationControlAcceptedDTO> acknowledged;
  ASSERT_TRUE(wait_until([&] {
    auto candidate =
        kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 1U));
    if (!candidate.ok()) {
      return false;
    }
    acknowledged = std::move(candidate.value());
    return true;
  }));
  ASSERT_TRUE(acknowledged.has_value());
  EXPECT_EQ(acknowledged->operation_revision_after_acceptance, OperationRevision(2U));
  ASSERT_TRUE(wait_until([&] {
    return engine.acknowledge_calls.load() == 1 &&
           kernel.resource_snapshot().front().state == ResourceSlotState::kFree;
  }));
  const auto retained = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(retained.value().operation_revision, OperationRevision(2U));

  const auto replay =
      kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 1U));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.acknowledge_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ResetSettlesFailedOperationAndRejectsStaleRevisionWithoutEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed,
                         ErrorDTO{"planning_failed", "no path", {{"reason", "no_path"}}});

  const auto stale = kernel.reset(control_request("request.reset.stale", executed.value(), 2U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "operation_revision_stale");

  const auto reset = kernel.reset(control_request("request.reset.1", executed.value(), 1U));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    return engine.certified_reset_calls.load() == 1 &&
           kernel.resource_snapshot().front().state == ResourceSlotState::kFree;
  }));
}

TEST(ResourceAddressedExecutionKernelTest,
     ResetOfExecutingMotionUsesOneScopedStopThenCertifiedLocalRetirement) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation("request.reset.executing"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto reset =
      kernel.reset(control_request("request.reset.executing.control", executed.value(), 1U));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  EXPECT_EQ(reset.value().operation_revision_after_acceptance, OperationRevision(2U));
  ASSERT_TRUE(wait_until([&] {
    return engine.translate_stop_stage_calls.load() == 1 &&
           engine.certified_reset_calls.load() == 1 &&
           kernel.resource_snapshot().front().state == ResourceSlotState::kFree;
  }));
  EXPECT_EQ(engine.stop_calls.load(), 1);

  const auto retained = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(retained.value().operation_revision, OperationRevision(3U));

  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  const auto next = kernel.execute(motion_operation("request.reset.executing.next"));
  ASSERT_TRUE(next.ok()) << next.error().message;
  EXPECT_NE(next.value().operation_id, executed.value().operation_id);
  EXPECT_EQ(engine.stop_calls.load(), 1);
  EXPECT_EQ(engine.certified_reset_calls.load(), 1);
  EXPECT_EQ(engine.execute_calls.load(), 2);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
}

TEST(ResourceAddressedExecutionKernelTest,
     CoordinatedResetUsesOneStopEnvelopeThenRetiresEveryDispatchedParticipant) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = coordinated_occurrence(first, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto executed =
      kernel.execute(coordinated_motion_operation("request.control.coordinated.reset"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto reset = kernel.reset(
      control_request("request.control.coordinated.reset.control", executed.value(), 1U));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return first_engine.translate_stop_stage_calls.load() == 1 &&
           second_engine.translate_stop_stage_calls.load() == 1 &&
           first_engine.certified_reset_calls.load() == 1 &&
           second_engine.certified_reset_calls.load() == 1 &&
           std::all_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot.state == ResourceSlotState::kFree; });
  }));
  EXPECT_EQ(first_engine.stop_calls.load(), 1);
  EXPECT_EQ(second_engine.stop_calls.load(), 1);
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(state.value().operation_revision, OperationRevision(3U));
}

TEST(ResourceAddressedExecutionKernelTest,
     MixedCompletedParticipantsAcknowledgeWithoutCommandBackendEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine motion_engine;
  FakeParticipantEngine command_engine;
  const auto motion =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto command_source = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                           CoordinateKind::kLinear, schema);
  const auto command = coordinated_occurrence(command_source, "2");
  ResourceAddressedExecutionKernel kernel(
      {registration(motion, motion_engine), registration(command, command_engine)}, clock,
      scheduler, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto executed =
      kernel.execute(coordinated_motion_command_operation("request.control.mixed.acknowledge"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  motion_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  command_engine.gripper_status.state = GripperCommandState::kCompleted;
  command_engine.gripper_status.progress = 1.0;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto completed = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  ASSERT_EQ(completed.value().state, OperationStateKind::kCompleted);

  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.control.mixed.acknowledge.control", executed.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return motion_engine.acknowledge_calls.load() == 1 &&
           std::all_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot.state == ResourceSlotState::kFree; });
  }));
  EXPECT_EQ(command_engine.acknowledge_calls.load(), 0);
  const auto retained = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(retained.value().operation_revision, OperationRevision(2U));
}

TEST(ResourceAddressedExecutionKernelTest,
     BlockedControlJournalCannotDelayOverlappingEmergencyStop) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::promise<void> journal_entered;
  std::promise<void> release_journal;
  const auto release_future = release_journal.get_future().share();
  journal.control_acceptance_hook = [&] {
    journal_entered.set_value();
    release_future.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation("request.control.blocked-journal"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  auto resetting = std::async(std::launch::async, [&] {
    return kernel.reset(
        control_request("request.control.blocked-journal.reset", executed.value(), 1U));
  });
  ASSERT_EQ(journal_entered.get_future().wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  const auto stopped =
      kernel.stop(stop_request("request.control.blocked-journal.stop", executed.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(resetting.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
  ASSERT_TRUE(wait_until([&] { return engine.translate_stop_stage_calls.load() == 1; }));
  EXPECT_EQ(engine.certified_reset_calls.load(), 0);

  release_journal.set_value();
  const auto reset = resetting.get();
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
}

TEST(ResourceAddressedExecutionKernelTest,
     MalformedOperationControlFailsBeforeJournalOrParticipantEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation("request.control.malformed"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  auto malformed = control_request("request.control.malformed.reset", executed.value(), 1U);
  malformed.reason = std::string(kMaximumStopReasonBytes + 1U, 'x');
  const auto rejected = kernel.reset(malformed);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "operation_control_request_invalid");
  EXPECT_EQ(engine.certified_reset_calls.load(), 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(std::count(journal.calls.begin(), journal.calls.end(), "control_accepted"), 0);
  }
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().operation_revision, OperationRevision(1U));
  EXPECT_EQ(state.value().state, OperationStateKind::kExecuting);
}

TEST(ResourceAddressedExecutionKernelTest,
     InitializesScalingAtomicallyWithTheExactResolvedExecutionBinding) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_EQ(engine.runtime_scale_calls, 1);
  const auto& exact = *engine.last_runtime_scale_command;
  EXPECT_EQ(exact.operation_id, executed.value().operation_id);
  EXPECT_EQ(exact.participant_id, executed.value().participant_assignments.front().participant_id);
  EXPECT_EQ(exact.controller_goal_id, engine.last_execute->binding.controller_goal_id);
  EXPECT_EQ(exact.control_binding_id, engine.last_execute->binding.control_binding_id);
  EXPECT_EQ(exact.control_route_id, engine.last_execute->binding.control_route_id);
  EXPECT_EQ(exact.resources, engine.last_execute->binding.resources);
  EXPECT_EQ(exact.expected_runtime_scale_generation, RuntimeScaleGeneration(0U));
  EXPECT_EQ(exact.requested_runtime_scale_generation, RuntimeScaleGeneration(0U));
  EXPECT_DOUBLE_EQ(exact.requested_scale, 1.0);

  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().participants.size(), 1U);
  const auto& participant = state.value().participants.front();
  EXPECT_EQ(participant.controller_goal_id, engine.last_execute->binding.controller_goal_id);
  EXPECT_EQ(participant.resources, engine.last_execute->binding.resources);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_id, executed.value().operation_id);
  EXPECT_EQ(slots.front().owner->participant_id, participant.participant_id);
  EXPECT_EQ(slots.front().owner->control_binding_id, participant.control_binding_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleDedupValidatesLiveScalarsAndRetainsFailedPersistence) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation("request.scale.invalid-observation"));
  ASSERT_TRUE(executed.ok());
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto good = engine.runtime_scale;
  const std::vector<std::function<void(RuntimeScaleMechanismStatusDTO&)>> invalidate{
      [](auto& value) { value.acknowledgement_timestamp_ns = -1; },
      [](auto& value) { value.acknowledgement_sequence = 9'007'199'254'740'992ULL; },
      [](auto& value) {
        value.virtual_trajectory_time_s = std::numeric_limits<double>::quiet_NaN();
      },
      [](auto& value) { value.virtual_trajectory_time_s = -1.0; },
      [](auto& value) { value.commanded_scale = -0.1; },
      [](auto& value) { value.applied_scale = 1.1; },
      [](auto& value) { value.applied_scale = std::numeric_limits<double>::infinity(); }};
  for (const auto& corrupt : invalidate) {
    corrupt(engine.runtime_scale);
    journal.fail_next_kind = "state:runtime_scale_status_invalid";
    const auto failed_write = kernel.reconcile_subordinate_state();
    ASSERT_FALSE(failed_write.ok());
    EXPECT_EQ(failed_write.error().code, "operation_journal_test_failure");
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    const auto observed = kernel.get_runtime_scale(executed.value().operation_id);
    ASSERT_TRUE(observed.ok());
    EXPECT_EQ(observed.value().state, RuntimeScaleApplicationState::kFailed);
    const auto& participant = observed.value().participants.front();
    ASSERT_TRUE(participant.error.has_value());
    EXPECT_EQ(participant.error->details.at("reason"), "runtime_scale_status_invalid");
    EXPECT_EQ(participant.acknowledgement_timestamp_ns, good.acknowledgement_timestamp_ns);
    EXPECT_EQ(participant.acknowledgement_sequence, good.acknowledgement_sequence);
    EXPECT_DOUBLE_EQ(participant.virtual_trajectory_time_s, good.virtual_trajectory_time_s);
    EXPECT_DOUBLE_EQ(participant.commanded_scale, good.commanded_scale);
    EXPECT_DOUBLE_EQ(participant.applied_scale, good.applied_scale);
    const auto count = journal.calls.size();
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    EXPECT_EQ(journal.calls.size(), count);
    engine.runtime_scale = good;
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
    EXPECT_EQ(kernel.get_runtime_scale(executed.value().operation_id).value().state,
              RuntimeScaleApplicationState::kApplied);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleObservationRefreshDoesNotJournalUnchangedSamples) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation("request.scale.observation"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto records = [&] {
    std::lock_guard lock(journal.mutex);
    return journal.calls.size();
  };
  const auto baseline = records();
  for (std::int64_t sample = 1; sample <= 1000; ++sample) {
    clock.set(sample * 1'000'000);
    engine.runtime_scale.acknowledgement_timestamp_ns = clock.now();
    ++engine.runtime_scale.acknowledgement_sequence;
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  }
  EXPECT_EQ(records(), baseline);
  auto current = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().updated_at_ns, clock.now());
  EXPECT_EQ(current.value().participants.front().acknowledgement_sequence,
            engine.runtime_scale.acknowledgement_sequence);

  // A stale source still creates one failure, not silence or repeated rows.
  clock.set(clock.now() + 100'000'001);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  current = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().state, RuntimeScaleApplicationState::kFailed);
  EXPECT_EQ(records(), baseline + 1U);
  for (int sample = 0; sample < 100; ++sample) {
    ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  }
  EXPECT_EQ(records(), baseline + 1U);
  engine.runtime_scale.acknowledgement_timestamp_ns = clock.now();
  ++engine.runtime_scale.acknowledgement_sequence;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 2U);

  // Operation progress remains current but is not a scaler audit transition.
  engine.runtime_scale.virtual_trajectory_time_s = 0.5;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 2U);
  current = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(current.ok());
  EXPECT_DOUBLE_EQ(current.value().participants.front().virtual_trajectory_time_s, 0.5);
  engine.runtime_scale.applied_scale = 0.6;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 3U);

  // Failed reads are an episode too; a recovered current sample is recorded.
  const auto command = engine.last_runtime_scale_command;
  engine.last_runtime_scale_command.reset();
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 4U);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 4U);
  engine.last_runtime_scale_command = command;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  EXPECT_EQ(records(), baseline + 5U);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlPersistsAcrossOperationsWithoutClientRebinding) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});

  const auto initial = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  EXPECT_TRUE(initial.value().supported);
  EXPECT_FALSE(initial.value().frozen);
  EXPECT_EQ(initial.value().control_generation, RuntimeScaleControlGeneration(0U));
  EXPECT_DOUBLE_EQ(initial.value().requested_scale, 1.0);
  EXPECT_FALSE(initial.value().active_operation_id.has_value());

  const auto request = runtime_scale_control_request("request.scale.control.1", 0U, 0.4);
  const auto configured = kernel.set_runtime_scale_control(request);
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  EXPECT_EQ(configured.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(configured.value().requested_scale, 0.4);
  EXPECT_FALSE(configured.value().active_operation_id.has_value());
  EXPECT_EQ(engine.runtime_scale_calls, 0);

  const auto replay = kernel.set_runtime_scale_control(request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.runtime_scale_calls, 0);
  auto conflicting_request = request;
  conflicting_request.scale = 0.5;
  const auto conflict = kernel.set_runtime_scale_control(conflicting_request);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  EXPECT_EQ(engine.runtime_scale_calls, 0);

  const auto first = kernel.execute(motion_operation("request.scale.control.first-operation"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.4);
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_DOUBLE_EQ(engine.last_runtime_scale_command->requested_scale, 0.4);

  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.scale.control.first-ack", first.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return !slots.empty() && slots.front().state == ResourceSlotState::kFree;
  }));

  const auto between = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(between.ok()) << between.error().message;
  EXPECT_EQ(between.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(between.value().requested_scale, 0.4);
  EXPECT_FALSE(between.value().active_operation_id.has_value());

  const auto second = kernel.execute(motion_operation("request.scale.control.second-operation"));
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.4);
  const auto active = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(active.ok()) << active.error().message;
  EXPECT_EQ(active.value().active_operation_id, second.value().operation_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     StateConditionedExecutionControlsDoNotExhaustOwnerLifetimeCapacity) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 2U});

  for (std::uint64_t cycle = 0U; cycle < 8U; ++cycle) {
    const auto executed =
        kernel.execute(motion_operation("request.bounded.execute." + std::to_string(cycle)));
    ASSERT_TRUE(executed.ok()) << "cycle " << cycle << ": " << executed.error().message;
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
    const auto acknowledged = kernel.acknowledge_completed(
        control_request("request.bounded.ack." + std::to_string(cycle), executed.value(), 1U));
    ASSERT_TRUE(acknowledged.ok()) << "cycle " << cycle << ": " << acknowledged.error().message;
    ASSERT_TRUE(wait_until(
        [&] { return kernel.resource_snapshot().front().state == ResourceSlotState::kFree; }));
  }

  auto control = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(control.ok()) << control.error().message;
  for (std::uint64_t update = 0U; update < 8U; ++update) {
    const auto accepted = kernel.set_runtime_scale_control(runtime_scale_control_request(
        "request.bounded.scale-control." + std::to_string(update),
        control.value().control_generation.value(), 0.1 * static_cast<double>((update % 8U) + 1U)));
    ASSERT_TRUE(accepted.ok()) << "update " << update << ": " << accepted.error().message;
    control = kernel.get_runtime_scale_control(runtime_scale_control_selector());
    ASSERT_TRUE(control.ok()) << control.error().message;
  }

  const auto active = kernel.execute(motion_operation("request.bounded.scale-operation"));
  ASSERT_TRUE(active.ok()) << active.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  for (std::uint64_t update = 0U; update < 8U; ++update) {
    const auto status = kernel.get_runtime_scale(active.value().operation_id);
    ASSERT_TRUE(status.ok()) << status.error().message;
    const auto accepted = kernel.set_runtime_scale(runtime_scale_request(
        "request.bounded.scale." + std::to_string(update), active.value(),
        status.value().operation_revision.value(), status.value().runtime_scale_generation.value(),
        0.1 * static_cast<double>((update % 8U) + 1U)));
    ASSERT_TRUE(accepted.ok()) << "update " << update << ": " << accepted.error().message;
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlRetimesTheCurrentOperationAndKeepsOneScopeGeneration) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation("request.scale.control.live-operation"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  const auto accepted = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.control.live", 0U, 0.25));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().active_operation_id, executed.value().operation_id);
  EXPECT_EQ(accepted.value().control_generation, RuntimeScaleControlGeneration(1U));
  ASSERT_TRUE(accepted.value().active_operation_scale_generation.has_value());
  EXPECT_EQ(*accepted.value().active_operation_scale_generation, RuntimeScaleGeneration(1U));
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_DOUBLE_EQ(engine.last_runtime_scale_command->requested_scale, 0.25);

  const auto status = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(status.value().requested_scale, 0.25);
  EXPECT_DOUBLE_EQ(*status.value().effective_scale, 0.25);
  EXPECT_EQ(status.value().application_state, RuntimeScaleApplicationState::kApplied);

  const auto stale = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.control.stale", 0U, 0.5));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "runtime_scale_control_generation_stale");
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlAppliesAcrossProgramAndDirectControlAliases) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto program_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto direct_control_binding = same_control_scope_program_alias(program_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(program_binding, engine), registration(direct_control_binding, engine)}, clock,
      scheduler, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});

  const auto configured = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.control.shared", 0U, 0.4));
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  const RuntimeScaleControlSelectorDTO direct_selector{
      direct_control_binding.value().program_binding_id,
      direct_control_binding.value().component_role_id,
      direct_control_binding.value().target_role_id};
  const auto shared_projection = kernel.get_runtime_scale_control(direct_selector);
  ASSERT_TRUE(shared_projection.ok()) << shared_projection.error().message;
  EXPECT_EQ(shared_projection.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(shared_projection.value().requested_scale, 0.4);

  auto direct = motion_operation("request.scale.control.direct");
  direct.program_id = direct_control_binding.value().program_id;
  direct.program_revision = direct_control_binding.value().program_revision;
  direct.program_binding_id = direct_control_binding.value().program_binding_id;
  direct.participants.front().component_role_id = direct_control_binding.value().component_role_id;
  direct.participants.front().target_role_id = direct_control_binding.value().target_role_id;
  const auto executed = kernel.execute(direct);
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.4);

  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto status = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().active_operation_id, executed.value().operation_id);

  const auto retimed = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.control.shared.live", 1U, 0.25));
  ASSERT_TRUE(retimed.ok()) << retimed.error().message;
  EXPECT_EQ(retimed.value().active_operation_id, executed.value().operation_id);
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_DOUBLE_EQ(engine.last_runtime_scale_command->requested_scale, 0.25);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleCapabilityDoesNotCreateAnotherPersistentSetting) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto motion =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto scale_value = motion.value();
  scale_value.capability_id = CapabilityId("execution.runtime_scale.operation_scoped");
  scale_value.control_path->control_binding_id = ControlBindingId("binding.arm.scale");
  scale_value.control_path->control_binding_revision = ControlBindingRevision(test_digest('e'));
  const auto scale = verify_runtime_participant_binding(std::move(scale_value));
  ASSERT_TRUE(scale.ok()) << scale.error().message;
  const auto direct_motion = same_control_scope_program_alias(motion);
  const auto direct_scale = same_control_scope_program_alias(scale.value());
  ResourceAddressedExecutionKernel kernel(
      {registration(scale.value(), engine), registration(motion, engine),
       registration(direct_scale, engine), registration(direct_motion, engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const RuntimeScaleControlSelectorDTO direct_selector{direct_motion.value().program_binding_id,
                                                       direct_motion.value().component_role_id,
                                                       direct_motion.value().target_role_id};

  const auto initial = kernel.get_runtime_scale_control(direct_selector);
  ASSERT_TRUE(initial.ok());
  ASSERT_TRUE(initial.value().supported);
  EXPECT_FALSE(initial.value().frozen);
  EXPECT_EQ(initial.value().control_binding_id, motion.value().control_path->control_binding_id);
  auto request = runtime_scale_control_request("request.scale.alias.direct", 0U, 0.6);
  request.selector = direct_selector;
  const auto configured = kernel.set_runtime_scale_control(request);
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  const auto program = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(program.ok());
  ASSERT_TRUE(program.value().supported);
  EXPECT_EQ(program.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(program.value().requested_scale, 0.6);

  const auto executed = kernel.execute(motion_operation("request.scale.alias.motion"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.6);
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto active_scale = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(active_scale.ok());
  const auto private_scale = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.alias.operation", executed.value(),
                            active_scale.value().operation_revision.value(),
                            active_scale.value().runtime_scale_generation.value(), 0.4));
  ASSERT_TRUE(private_scale.ok()) << private_scale.error().message;
  const auto persistent = kernel.get_runtime_scale_control(direct_selector);
  ASSERT_TRUE(persistent.ok());
  EXPECT_DOUBLE_EQ(persistent.value().requested_scale, 0.6);
  EXPECT_EQ(persistent.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_EQ(persistent.value().active_operation_id, executed.value().operation_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     OperationScaleCapabilityAloneDoesNotOwnPersistentScaling) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("arm", "arm", "execution.runtime_scale.operation_scoped",
                                 CoordinateKind::kAngular),
                    engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto status = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(status.ok());
  EXPECT_FALSE(status.value().supported);
  EXPECT_TRUE(status.value().frozen);
  ASSERT_TRUE(status.value().reason.has_value());
  EXPECT_EQ(status.value().reason->details.at("reason"), "runtime_scale_control_unsupported");
  const auto set = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.no-motion", 0U, 0.4));
  ASSERT_FALSE(set.ok());
  EXPECT_EQ(engine.runtime_scale_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlStillRejectsDistinctMotionAuthorities) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto other_value = first.value();
  other_value.control_path->control_binding_id = ControlBindingId("binding.arm.other");
  other_value.control_path->control_binding_revision = ControlBindingRevision(test_digest('e'));
  const auto other = verify_runtime_participant_binding(std::move(other_value));
  ASSERT_TRUE(other.ok()) << other.error().message;
  // Exact-key uniqueness rejects this even before the scale resolver runs.
  // Do not relax that stronger registration invariant to merge aliases.
  EXPECT_THROW(
      (ResourceAddressedExecutionKernel(
          {registration(first, engine), registration(other.value(), engine)}, clock, scheduler,
          default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U})),
      std::invalid_argument);
  EXPECT_EQ(engine.runtime_scale_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlNeverDelaysStopAndCannotUndoItsFence) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation("request.scale.control.stop-race"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  std::promise<void> scale_entered_promise;
  auto scale_entered = scale_entered_promise.get_future();
  std::promise<void> release_scale_promise;
  const auto release_scale = release_scale_promise.get_future().share();
  engine.runtime_scale_effect_hook = [&] {
    scale_entered_promise.set_value();
    release_scale.wait();
  };
  auto scale_future = std::async(std::launch::async, [&] {
    return kernel.set_runtime_scale_control(
        runtime_scale_control_request("request.scale.control.stop-race.update", 0U, 0.4));
  });
  ASSERT_EQ(scale_entered.wait_for(10s), std::future_status::ready);

  auto stop_future = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.scale.control.stop-race.stop", executed.value(), 1U));
  });
  const bool stop_was_not_delayed = stop_future.wait_for(250ms) == std::future_status::ready;
  release_scale_promise.set_value();
  ASSERT_TRUE(stop_was_not_delayed)
      << "an in-flight runtime-scale provider call delayed Stop acceptance";

  const auto stopped = stop_future.get();
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  const auto scaled = scale_future.get();
  ASSERT_TRUE(scaled.ok()) << scaled.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(executed.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kStopped;
  }));
  const auto terminal = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(terminal.ok()) << terminal.error().message;
  EXPECT_EQ(terminal.value().state, OperationStateKind::kStopped);
  EXPECT_EQ(terminal.value().active_stop_id, stopped.value().stop_id);

  const auto setting = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(setting.ok()) << setting.error().message;
  EXPECT_EQ(setting.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(setting.value().requested_scale, 0.4);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlSurvivesStoppedResetAndSeedsTheNextMove) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});

  const auto configured = kernel.set_runtime_scale_control(
      runtime_scale_control_request("request.scale.control.before-reset", 0U, 0.35));
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  const auto first = kernel.execute(motion_operation("request.scale.control.reset.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.35);
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  const auto stopped =
      kernel.stop(stop_request("request.scale.control.reset.stop", first.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(first.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kStopped;
  }));
  const auto stopped_state = kernel.get_operation_state(first.value().operation_id);
  ASSERT_TRUE(stopped_state.ok()) << stopped_state.error().message;
  const auto reset =
      kernel.reset(control_request("request.scale.control.reset.release", first.value(),
                                   stopped_state.value().operation_revision.value()));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  ASSERT_TRUE(wait_until(
      [&] { return kernel.resource_snapshot().front().state == ResourceSlotState::kFree; }));

  const auto between = kernel.get_runtime_scale_control(runtime_scale_control_selector());
  ASSERT_TRUE(between.ok()) << between.error().message;
  EXPECT_EQ(between.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(between.value().requested_scale, 0.35);
  EXPECT_FALSE(between.value().active_operation_id.has_value());

  const auto second = kernel.execute(motion_operation("request.scale.control.reset.second"));
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  EXPECT_DOUBLE_EQ(engine.last_execute->initial_runtime_scale->requested_scale, 0.35);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleIsGenerationFencedReplaySafeAndZeroIsNotStop) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(10);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message << ": "
                             << executed.error().details.at("reason");
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto first_request =
      runtime_scale_request("request.scale.1", executed.value(), 1U, 0U, 0.25);
  const auto first = kernel.set_runtime_scale(first_request);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().runtime_scale_generation, RuntimeScaleGeneration(1U));
  EXPECT_EQ(engine.runtime_scale_calls, 2);
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_EQ(engine.last_runtime_scale_command->operation_revision, OperationRevision(1U));
  EXPECT_EQ(engine.last_runtime_scale_command->requested_runtime_scale_generation,
            RuntimeScaleGeneration(1U));
  EXPECT_DOUBLE_EQ(engine.last_runtime_scale_command->requested_scale, 0.25);

  const auto replay = kernel.set_runtime_scale(first_request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  auto conflict = first_request;
  conflict.scale = 0.5;
  const auto conflicting = kernel.set_runtime_scale(conflict);
  ASSERT_FALSE(conflicting.ok());
  EXPECT_EQ(conflicting.error().code, "request_id_conflict");
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  const auto stale = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.stale", executed.value(), 1U, 0U, 0.5));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "runtime_scale_generation_stale");
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  const auto held = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.hold", executed.value(), 1U, 1U, 0.0));
  ASSERT_TRUE(held.ok()) << held.error().message;
  EXPECT_EQ(held.value().runtime_scale_generation, RuntimeScaleGeneration(2U));
  EXPECT_EQ(engine.stop_calls, 0);
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kExecuting);
  const auto status = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kApplied);
  EXPECT_DOUBLE_EQ(status.value().requested_scale, 0.0);
  EXPECT_DOUBLE_EQ(status.value().effective_scale, 0.0);
  ASSERT_EQ(status.value().participants.size(), 1U);
  EXPECT_TRUE(status.value().participants.front().supported);
  EXPECT_EQ(status.value().participants.front().accepted_generation, RuntimeScaleGeneration(2U));
}

TEST(ResourceAddressedExecutionKernelTest,
     ScopedStopAdvancesOwnerAtomicallyAndNeverRepeatsItsEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  std::promise<void> stage_entered_promise;
  auto stage_entered = stage_entered_promise.get_future();
  std::promise<void> release_stage_promise;
  const auto release_stage = release_stage_promise.get_future().share();
  engine.translate_stop_stage_hook = [&] {
    stage_entered_promise.set_value();
    release_stage.wait();
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto request = stop_request("request.stop.1", executed.value(), 1U);
  const auto stopped = kernel.stop(request);
  const bool stage_started =
      stopped.ok() && stage_entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  const auto replay =
      stopped.ok()
          ? kernel.stop(request)
          : Result<StopAcceptedDTO>::Err(ErrorDTO{"test_stop_failed", "initial Stop failed", {}});
  release_stage_promise.set_value();
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(stage_started);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  ASSERT_TRUE(wait_until([&] { return engine.stop_calls.load() == 1; }));
  ASSERT_TRUE(engine.last_stop_stage.has_value());
  ASSERT_TRUE(engine.last_stop_stage->participant_stop_plan.participant_stop_profile.has_value());
  const auto& operator_profile =
      *engine.last_stop_stage->participant_stop_plan.participant_stop_profile;
  EXPECT_EQ(operator_profile.profile_id, "operator_soft_on_path");
  EXPECT_EQ(operator_profile.stop_mode, MotionInterruptStopMode::kSoftOnPath);
  EXPECT_EQ(operator_profile.deceleration_time_s, std::optional<double>{1.0});
  EXPECT_TRUE(operator_profile.allow_natural_completion_on_short_path);
  EXPECT_EQ(operator_profile.per_joint_max_deceleration.at("joint_1"),
            CoordinateAccelerationDTO{AngularAccelerationDTO{4.0}});
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.participant_stop_profile_purpose,
            ParticipantStopProfilePurposeDTO::kOperator);
  EXPECT_EQ(stopped.value().stop_generation, StopGeneration(1U));
  ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
  EXPECT_EQ(
      stopped.value().affected_operation_revisions.front().operation_revision_after_acceptance,
      OperationRevision(2U));
  EXPECT_EQ(engine.stop_calls, 1);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_TRUE(slots.front().state == ResourceSlotState::kStopping ||
              slots.front().state == ResourceSlotState::kTerminal);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_revision, OperationRevision(2U));
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_TRUE(state.value().state == OperationStateKind::kStopping ||
              state.value().state == OperationStateKind::kStopped);
  EXPECT_EQ(state.value().operation_revision, OperationRevision(2U));
  EXPECT_EQ(state.value().active_stop_id, stopped.value().stop_id);
  EXPECT_EQ(state.value().active_stop_generation, StopGeneration(1U));

  EXPECT_FALSE(stop_accepted_field_error(replay.value()).has_value());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().stop_id, stopped.value().stop_id);
  EXPECT_EQ(engine.stop_calls, 1);

  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));

  const auto joined = kernel.stop(stop_request("request.stop.join", executed.value(), 2U, 1U));
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  EXPECT_FALSE(stop_accepted_field_error(joined.value()).has_value());
  EXPECT_NE(joined.value().stop_id, stopped.value().stop_id);
  EXPECT_EQ(joined.value().stop_generation, StopGeneration(2U));
  EXPECT_EQ(engine.stop_calls, 1);

  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(joined.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));

  const auto stale = kernel.stop(stop_request("request.stop.stale", executed.value(), 1U, 0U));
  ASSERT_TRUE(stale.ok()) << stale.error().message;
  EXPECT_FALSE(stop_accepted_field_error(stale.value()).has_value());
  EXPECT_EQ(stale.value().stop_generation, StopGeneration(3U));
  EXPECT_EQ(engine.stop_calls, 1);

  const auto scaling_after_stop = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.after-stop", executed.value(), 2U, 0U, 0.5));
  ASSERT_FALSE(scaling_after_stop.ok());
  EXPECT_EQ(scaling_after_stop.error().details.at("reason"), "runtime_scale_state_invalid");
  EXPECT_EQ(engine.runtime_scale_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     AcceptedStopSurfacesSubordinateRejectionAsInterventionRequired) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "controller rejected Stop",
                                    {{"reason", "controller_stop_rejected"}}}};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine, true)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto accepted = kernel.stop(stop_request("request.stop.reject", executed.value(), 1U));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(wait_until(
      [&] {
        const auto current = kernel.get_operation_state(executed.value().operation_id);
        return current.ok() && current.value().state == OperationStateKind::kInterventionRequired;
      },
      std::chrono::seconds(10)));
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 2);
  ASSERT_TRUE(engine.last_stop_stage.has_value());
  EXPECT_EQ(engine.last_stop_stage->stage_index, 2U);
  EXPECT_TRUE(engine.last_stop_stage->capability_provider_binding_id.has_value());
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.stage_commands.front()
                .resolved_stage.containment_command.command_kind,
            StopContainmentCommandKind::kMotionControlledStop);
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(state.value().participants.front().terminal_error->details.at("reason"),
            "controller_stop_rejected");
}

TEST(ResourceAddressedExecutionKernelTest,
     CompletedNominalResultAtOrAfterItsDeadlineCannotSuppressEscalation) {
  // The completion receiver reads the clock while holding the Kernel lock,
  // after the bounded executor has stored its result. Advance there so the
  // test cannot accidentally pass through a pending-result watchdog race.
  class CompletionClock final : public IClock {
   public:
    void AdvanceAtNextCallerRead(TimePoint value) {
      caller_ = std::this_thread::get_id();
      completion_time_ = value;
      armed_.store(true, std::memory_order_release);
    }
    TimePoint now() const override {
      if (armed_.load(std::memory_order_acquire) && caller_ == std::this_thread::get_id() &&
          armed_.exchange(false)) {
        current_.store(completion_time_, std::memory_order_release);
      }
      return current_.load(std::memory_order_acquire);
    }

   private:
    std::thread::id caller_;
    TimePoint completion_time_{};
    mutable std::atomic_bool armed_{};
    mutable std::atomic<TimePoint> current_{};
  };
  for (const TimePoint relative_to_deadline : {-1, 0, 1}) {
    SCOPED_TRACE(relative_to_deadline);
    CompletionClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    std::atomic<unsigned> calls{};
    engine.translate_stop_stage_hook = [&] {
      if (calls.fetch_add(1U) == 0U)
        clock.AdvanceAtNextCallerRead(2'000'000'000 + relative_to_deadline);
    };
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto operation = kernel.execute(motion_operation("request.deadline-result.execute"));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    const auto stopped =
        kernel.stop(stop_request("request.deadline-result.stop", operation.value(), 1U));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));
    const auto status = kernel.get_stop_status(stopped.value().stop_id).value();
    const bool expired = relative_to_deadline >= 0;
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), expired ? 2 : 1);
    const auto certificate =
        kernel.get_stop_certificate(status.stop_certificate->stop_certificate_id);
    ASSERT_TRUE(certificate.ok());
    EXPECT_EQ(certificate.value().result, expired ? StopCertificateResultDTO::kSucceededEscalated
                                                  : StopCertificateResultDTO::kSucceededNominal)
        << (certificate.value().terminal_error ? certificate.value().terminal_error->message
                                               : "no terminal diagnostic");
    ASSERT_FALSE(status.attempted_stage_results.empty());
    EXPECT_EQ(status.attempted_stage_results.front().authority,
              expired ? StopStageResultAuthorityDTO::kSupersededLate
                      : StopStageResultAuthorityDTO::kAuthoritative);
    EXPECT_EQ(kernel.get_stop_fault_latch().has_value(), expired);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     BlockedNominalStageCannotDelayPreparedStrongerStageWatchdog) {
  for (const auto& [timeout, intermediate] :
       {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
    SCOPED_TRACE(timeout ? "timeout" : "immediate failure");
    SCOPED_TRACE(intermediate ? "three-stage chain" : "two-stage chain");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeParticipantEngine unaffected_engine;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto unaffected_binding = second_same_type_occurrence(binding);
    ResourceAddressedExecutionKernel kernel(
        {registration(binding, engine, intermediate),
         registration(unaffected_binding, unaffected_engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    // Release both seams before destroying the Kernel, including assertion failures.
    HeldSettlementCall nominal, strongest;
    if (!timeout)
      engine.next_stop_result = StopResultDTO{
          false, ErrorDTO{"controller_stop_rejected", "controller rejected Stop", {}}};
    engine.translate_stop_stage_hook = [&, hold_nominal = nominal.Blocker(),
                                        hold_strongest = strongest.Blocker(), timeout,
                                        calls = std::make_shared<std::atomic<unsigned>>(0)] {
      if (calls->fetch_add(1U) == 0U) {
        if (timeout) hold_nominal();
      } else {
        hold_strongest();
      }
    };
    const auto operation = kernel.execute(motion_operation("request.blocked-stage.execute"));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    const auto stopped =
        kernel.stop(stop_request("request.blocked-stage.stop", operation.value(), 1U));
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    // The nominal stage's evidence deadline is two seconds in the retained
    // fixture. The StopControl watchdog observes the owner clock and advances
    // to the already-prepared software-hard stage without waiting for the
    // blocked nominal provider lane to return.
    if (timeout) {
      ASSERT_TRUE(nominal.Entered());
      clock.advance(2'100'000'000);
      ASSERT_TRUE(wait_until([&] { return engine.translate_stop_stage_calls.load() >= 2; },
                             std::chrono::seconds(4)));
    }
    ASSERT_TRUE(strongest.Entered())
        << "blocked nominal provider delayed the stronger prepared stage";
    ASSERT_TRUE(engine.last_stop_stage.has_value());
    EXPECT_EQ(engine.last_stop_stage->stage_index, intermediate ? 2U : 1U);
    EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.stage_commands.back()
                  .resolved_stage.stage_kind,
              StopEscalationStageKind::kSoftwareHard);
    EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.deadline_budget.outer_duration_ns,
              5'000'000'000);
    EXPECT_EQ(engine.translate_stop_stage_calls.load(), 2);
    const auto pending = kernel.get_stop_status(stopped.value().stop_id);
    ASSERT_TRUE(pending.ok());
    EXPECT_FALSE(pending.value().stop_certificate.has_value());
    const auto early_latch = kernel.get_stop_fault_latch();
    ASSERT_TRUE(early_latch.has_value());
    EXPECT_FALSE(early_latch->stop_certificate.has_value());
    EXPECT_EQ(early_latch->trigger, StopFaultTriggerDTO::kEscalated);
    EXPECT_EQ(pending.value().fault_latch, early_latch);
    EXPECT_TRUE(kernel.cell_stop_action_projection().fault.has_value());
    const auto offer = kernel.get_stop_fault_recovery_offer();
    ASSERT_TRUE(offer.ok());
    ASSERT_TRUE(offer.value().has_value());
    EXPECT_EQ(offer.value()->latch, *early_latch);
    EXPECT_TRUE(offer.value()->configuration_expectations.empty());
    EXPECT_FALSE(stop_fault_recovery_offer_field_error(*offer.value()).has_value());
    auto forged_ready = *offer.value();
    forged_ready.latch.recovery_status = StopFaultRecoveryStatusDTO::kReady;
    EXPECT_TRUE(stop_fault_recovery_offer_field_error(forged_ready).has_value());
    auto blocked_intent = motion_operation("request.blocked-stage.unaffected");
    blocked_intent.program_binding_id = unaffected_binding.value().program_binding_id;
    const auto blocked = kernel.execute(blocked_intent);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");
    // The rejected nominal call has returned before the strongest lane starts.
    if (!timeout) engine.next_stop_result = StopResultDTO{true, std::nullopt};
    strongest.Release();
    nominal.Release();
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(stopped.value().stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));
    const auto latch = kernel.get_stop_fault_latch();
    ASSERT_TRUE(latch.has_value());
    EXPECT_EQ(latch->latch_id, early_latch->latch_id);
    EXPECT_EQ(latch->latch_generation, early_latch->latch_generation);
    EXPECT_TRUE(latch->stop_certificate.has_value());
    EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kEscalated);
    EXPECT_EQ(latch->state, StopFaultLatchStateDTO::kLatched);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     ConcurrentDisjointStopsMintUniqueMonotonicCellIdentities) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(500);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});

  const auto first = kernel.execute(motion_operation("request.concurrent-stop.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.concurrent-stop.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  auto first_future = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.concurrent-stop.first", first.value(), 1U));
  });
  auto second_future = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.concurrent-stop.second", second.value(), 1U));
  });
  const auto first_stop = first_future.get();
  const auto second_stop = second_future.get();
  ASSERT_TRUE(first_stop.ok()) << first_stop.error().message;
  ASSERT_TRUE(second_stop.ok()) << second_stop.error().message;
  EXPECT_NE(first_stop.value().stop_id, second_stop.value().stop_id);
  const std::vector<std::uint64_t> generations{first_stop.value().stop_generation.value(),
                                               second_stop.value().stop_generation.value()};
  EXPECT_TRUE((generations == std::vector<std::uint64_t>{1U, 2U} ||
               generations == std::vector<std::uint64_t>{2U, 1U}));
  ASSERT_TRUE(wait_until(
      [&] {
        return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1 &&
               journal.stop_acceptance_calls.load() == 2;
      },
      std::chrono::seconds(10)));
}

TEST(ResourceAddressedExecutionKernelTest,
     PendingCellStopFaultPreventsDisjointPrepareDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine, second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  HeldSettlementCall strongest;
  const auto first = kernel.execute(motion_operation("request.fault-gate.first"));
  ASSERT_TRUE(first.ok());
  second_engine.defer_prepare_completion = true;
  auto intent = motion_operation("request.fault-gate.second");
  intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(intent);
  ASSERT_TRUE(second.ok());
  first_engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{"controller_stop_rejected", "controller rejected Stop", {}}};
  first_engine.translate_stop_stage_hook = [calls = std::make_shared<std::atomic<unsigned>>(0),
                                            hold = strongest.Blocker()] {
    if (calls->fetch_add(1) > 0) hold();
  };
  const auto stopped = kernel.stop(stop_request("request.fault-gate.stop", first.value(), 1U));
  ASSERT_TRUE(stopped.ok());
  ASSERT_TRUE(strongest.Entered());
  ASSERT_TRUE(kernel.get_stop_fault_latch().has_value());
  ASSERT_TRUE(second_engine.complete_preparation().ok());
  ASSERT_TRUE(wait_until([&] { return second_engine.cancel_prepared_motion_calls.load() > 0; }));
  EXPECT_EQ(second_engine.dispatch_prepared_motion_calls.load(), 0);
  first_engine.next_stop_result = StopResultDTO{true, std::nullopt};
  strongest.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  })) << "released Stop must settle before the Kernel test fixture is destroyed";
}

TEST(ResourceAddressedExecutionKernelTest,
     RecognizedMalformedIngressPromotesToFullAndProjectsExactAdapterTruth) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto operation = kernel.execute(motion_operation("request.malformed-stop.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;

  const MalformedStopIngressDTO ingress{
      RequestId("malformed-stop.http.1"), RequestOwnerEpoch("kernel.epoch.1"),
      RequestAttributionDTO{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                            std::nullopt},
      "/api/execution/stops", "recognized_stop_payload_invalid"};
  const auto stopped = kernel.recognized_malformed_stop(ingress);
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(stopped.value().resolution_disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(stopped.value().resolution_reason,
            StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent);
  const auto selector_digest = ComputeRequestedStopSelectorDigest(ingress);
  ASSERT_TRUE(selector_digest.ok()) << selector_digest.error().message;
  EXPECT_EQ(stopped.value().requested_selector_digest, selector_digest.value());

  ASSERT_TRUE(wait_until([&] { return journal.stop_acceptance_calls.load() == 1; }));
  {
    std::lock_guard lock(journal.mutex);
    ASSERT_TRUE(journal.last_stop_acceptance.has_value());
    EXPECT_FALSE(journal.last_stop_acceptance->normalized_request_digest.has_value());
    EXPECT_EQ(journal.last_stop_acceptance->malformed_ingress_reason, ingress.reason);
    ASSERT_TRUE(journal.last_stop_intent.has_value());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(*journal.last_stop_intent));
  }
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kScopePromotion);
}

[[nodiscard]] std::vector<StopFaultRecoveryConfigurationExpectationDTO>
registered_domain_configurations(const std::vector<RuntimeParticipantBinding>& bindings) {
  std::vector<StopFaultRecoveryConfigurationExpectationDTO> rows;
  for (const auto& binding : bindings) {
    const auto& path = *binding.value().control_path;
    rows.push_back({path.control_domain_id, path.active_control_configuration_id,
                    path.active_control_configuration_generation});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.control_domain_id < right.control_domain_id;
  });
  return rows;
}

/// Models the native recovery-facts owner for an idle cell-wide Stop: every
/// captured resource is observed settled, an uncontained terminal also has
/// exact external reconciliation, and the configuration rows come from the
/// same shared owner the Kernel offer uses.
[[nodiscard]] StopFaultRecoveryFactsResolver installed_domain_recovery_facts(
    std::vector<RuntimeParticipantBinding> installed) {
  return [installed = std::move(installed)](
             const StopStatusDTO& status,
             const std::int64_t assessed_at_ns) -> Result<StopFaultRecoveryFactsDTO> {
    if (!status.fault_latch.has_value() || !status.stop_certificate.has_value()) {
      return Result<StopFaultRecoveryFactsDTO>::Err(
          ErrorDTO{"stop_recovery_test_truth_missing", "terminal truth is missing", {}});
    }
    std::vector<const RuntimeParticipantBinding*> pointers;
    for (const auto& binding : installed) pointers.push_back(&binding);
    auto expectations = StopFaultRecoveryConfigurationExpectations(status, pointers);
    if (!expectations.ok()) {
      return Result<StopFaultRecoveryFactsDTO>::Err(expectations.error());
    }
    std::vector<StopFaultRecoveryConfigurationFactDTO> configurations;
    for (const auto& expectation : expectations.value()) {
      configurations.push_back({expectation, true, ContentDigest(test_digest('3'))});
    }
    const auto& latch = *status.fault_latch;
    std::vector<StopFaultRecoveryResourceFactDTO> resources;
    std::vector<StopFaultRecoveryResourceFactDTO> external;
    for (const auto& resource : latch.captured_resources) {
      resources.push_back(
          StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                           RuntimeEpoch("observed-state.epoch"), 1U, 0, 1'000,
                                           ContentDigest(test_digest('1')), std::nullopt});
      if (latch.trigger == StopFaultTriggerDTO::kFailedUncontained) {
        external.push_back(
            StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                             RuntimeEpoch("external-reconciliation.epoch"), 1U, 0,
                                             1'000, ContentDigest(test_digest('2')), std::nullopt});
      }
    }
    return Result<StopFaultRecoveryFactsDTO>::Ok(
        StopFaultRecoveryFactsDTO{assessed_at_ns,
                                  latch.cell_id,
                                  latch.retained_bundle_id,
                                  latch.graph_revision,
                                  *status.stop_certificate,
                                  latch.latch_id,
                                  latch.latch_generation,
                                  status.acceptance_projection,
                                  status.terminal_projection,
                                  status.surveillance_projection,
                                  status.surveillance,
                                  std::move(resources),
                                  std::move(configurations),
                                  {},
                                  std::move(external),
                                  {},
                                  {},
                                  {}});
  };
}

[[nodiscard]] MalformedStopIngressDTO idle_malformed_stop(const std::string& request_id,
                                                          const std::string& owner_epoch) {
  return MalformedStopIngressDTO{
      RequestId(request_id), RequestOwnerEpoch(owner_epoch),
      RequestAttributionDTO{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                            std::nullopt},
      "/api/execution/stops", "recognized_stop_payload_invalid"};
}

[[nodiscard]] StopFaultRecoveryIntentDTO idle_stop_recovery_intent(
    const std::string& request_id, const std::string& owner_epoch,
    const StopFaultRecoveryOfferDTO& offer) {
  const auto& latch = offer.latch;
  return StopFaultRecoveryIntentDTO{
      PublicRequestContextDTO{RequestId(request_id), RequestOwnerEpoch(owner_epoch), "test",
                              std::nullopt},
      latch.latch_id,
      latch.latch_generation,
      latch.recovery_assessment_revision,
      latch.stop_certificate->stop_certificate_id,
      latch.stop_certificate->stop_certificate_revision,
      latch.cell_id,
      latch.retained_bundle_id,
      latch.graph_revision,
      offer.configuration_expectations,
      std::string("operator verified the idle cell")};
}

[[nodiscard]] bool stop_terminal_recorded(const ResourceAddressedExecutionKernel& kernel,
                                          const StopId& stop_id) {
  const auto status = kernel.get_stop_status(stop_id);
  return status.ok() && status.value().stop_certificate.has_value() &&
         status.value().acceptance_projection.state == StopDurableProjectionStateDTO::kRecorded &&
         status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
}

TEST(ResourceAddressedExecutionKernelTest,
     IdleCellWideStopOffersInstalledDomainConfigurationsAndRecoversToClear) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = second_same_type_occurrence(first);
  const std::vector<RuntimeParticipantBinding> installed{first, second};
  const auto expected_rows = registered_domain_configurations(installed);
  ASSERT_EQ(expected_rows.size(), 2U);
  ASSERT_NE(expected_rows.front().control_domain_id, expected_rows.back().control_domain_id);

  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.stop_fault_recovery_facts_resolver = installed_domain_recovery_facts(installed);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine), registration(second, second_engine)}, clock, scheduler,
      journal, std::move(config));

  // An idle cell: a recognized-malformed Stop is promoted to Full and latches
  // without any participant, operation or effect-uncertain resource.
  const auto malformed = kernel.recognized_malformed_stop(
      idle_malformed_stop("malformed-stop.idle.1", "kernel.epoch.1"));
  ASSERT_TRUE(malformed.ok()) << malformed.error().message;
  EXPECT_EQ(malformed.value().resolution_disposition,
            StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_TRUE(malformed.value().affected_operation_revisions.empty());
  ASSERT_TRUE(
      wait_until([&] { return stop_terminal_recorded(kernel, malformed.value().stop_id); }));

  // A later explicit Full Stop with no work neither replaces nor clears it.
  const auto full = kernel.full_stop(FullStopIntentDTO{
      PublicRequestContextDTO{RequestId("request.idle.full"), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      StopGeneration(0U),
      {}});
  ASSERT_TRUE(full.ok()) << full.error().message;
  EXPECT_EQ(full.value().resolution_reason, StopScopeResolutionReasonDTO::kExplicitFullStop);
  ASSERT_TRUE(wait_until([&] { return stop_terminal_recorded(kernel, full.value().stop_id); }));

  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kScopePromotion);
  EXPECT_EQ(latch->stop_id, malformed.value().stop_id);
  ASSERT_TRUE(latch->stop_certificate.has_value());
  EXPECT_TRUE(latch->captured_operation_ids.empty());
  EXPECT_TRUE(latch->captured_resources.empty());
  const auto status = kernel.get_stop_status(malformed.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_TRUE(std::none_of(status.value().scope_entries.begin(), status.value().scope_entries.end(),
                           [](const auto& scope) {
                             return std::holds_alternative<StopParticipantScopeFactsDTO>(
                                 scope.facts);
                           }));

  const auto offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_TRUE(offer.value().has_value());
  EXPECT_EQ(offer.value()->latch, *latch);
  EXPECT_EQ(offer.value()->configuration_expectations, expected_rows);
  EXPECT_FALSE(offer.value()->recovery_unavailable_reason.has_value());
  EXPECT_FALSE(stop_fault_recovery_offer_field_error(*offer.value()).has_value());
  // Reading the offer does not change the Stop scope, certificate or capture.
  EXPECT_EQ(kernel.get_stop_status(malformed.value().stop_id).value(), status.value());

  // The shared owner fails closed instead of fabricating rows.
  std::vector<const RuntimeParticipantBinding*> pointers{&first, &second};
  const auto foreign = select_candidate_product(first);
  std::vector<const RuntimeParticipantBinding*> foreign_pointers{&foreign};
  const auto crossed = StopFaultRecoveryConfigurationExpectations(status.value(), foreign_pointers);
  ASSERT_FALSE(crossed.ok());
  EXPECT_EQ(crossed.error().code, kStopFaultRecoveryInstalledConfigurationInvalid);
  auto scoped = status.value();
  scoped.accepted.resolution_disposition = StopResolutionDispositionDTO::kExact;
  scoped.accepted.resolution_reason = StopScopeResolutionReasonDTO::kExactCurrentScope;
  const auto missing = StopFaultRecoveryConfigurationExpectations(scoped, pointers);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, kStopFaultRecoveryParticipantConfigurationMissing);

  ASSERT_TRUE(kernel.refresh_stop_fault_recovery_assessment().ok());
  const auto ready = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(ready.ok()) << ready.error().message;
  ASSERT_TRUE(ready.value().has_value());
  EXPECT_EQ(ready.value()->latch.recovery_status, StopFaultRecoveryStatusDTO::kReady);
  EXPECT_EQ(ready.value()->configuration_expectations, expected_rows);
  EXPECT_FALSE(kernel.execute(motion_operation("request.idle.blocked")).ok());

  const auto cleared = kernel.recover_stop_fault(
      idle_stop_recovery_intent("request.idle.clear", "kernel.epoch.1", *ready.value()));
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  EXPECT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 1);
  const auto after = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(after.ok()) << after.error().message;
  EXPECT_FALSE(after.value().has_value());
  // Clearing the latch authorizes no motion by itself; it only reopens
  // ordinary admission for a new explicit request.
  EXPECT_EQ(first_engine.execute_calls, 0);
  EXPECT_EQ(second_engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     IdleCellWideStopWithOnlyAnEffectUncertainResourceOffersInstalledDomainsAndClears) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const ResourceId uncertain("robot.joint_1");
  journal.recovery = OperationJournalRecoveryDTO{{},
                                                 {},
                                                 RequestOwnerEpoch("kernel.epoch.old"),
                                                 {uncertain},
                                                 AdmissionGeneration(0U),
                                                 CellAdmissionClaimGeneration(0U),
                                                 {},
                                                 {},
                                                 {}};
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.new"),
                                                CellId("cell.test"), 16U};
  config.stop_fault_recovery_facts_resolver = installed_domain_recovery_facts({binding});
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  ASSERT_EQ(kernel.resource_snapshot().front().state,
            ResourceSlotState::kUnreconciledExternalOrOrphanGoal);

  const auto malformed = kernel.recognized_malformed_stop(
      idle_malformed_stop("malformed-stop.uncertain.1", "kernel.epoch.new"));
  ASSERT_TRUE(malformed.ok()) << malformed.error().message;
  EXPECT_EQ(malformed.value().resolution_disposition,
            StopResolutionDispositionDTO::kPromotedToFull);
  ASSERT_TRUE(
      wait_until([&] { return stop_terminal_recorded(kernel, malformed.value().stop_id); }));

  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  ASSERT_TRUE(latch->stop_certificate.has_value());
  // The uncontained effect-uncertain resource is the stronger terminal
  // trigger; the promotion still made this Stop cell-wide.
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kFailedUncontained);
  EXPECT_TRUE(latch->captured_operation_ids.empty());
  EXPECT_EQ(latch->captured_resources, std::vector<ResourceId>{uncertain});
  const auto status = kernel.get_stop_status(malformed.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_TRUE(std::none_of(status.value().scope_entries.begin(), status.value().scope_entries.end(),
                           [](const auto& scope) {
                             return std::holds_alternative<StopParticipantScopeFactsDTO>(
                                 scope.facts);
                           }));

  const auto offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_TRUE(offer.value().has_value());
  EXPECT_EQ(offer.value()->latch, *latch);
  EXPECT_EQ(offer.value()->configuration_expectations, registered_domain_configurations({binding}));
  EXPECT_FALSE(offer.value()->recovery_unavailable_reason.has_value());
  EXPECT_EQ(kernel.get_stop_status(malformed.value().stop_id).value(), status.value());

  ASSERT_TRUE(kernel.refresh_stop_fault_recovery_assessment().ok());
  const auto ready = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(ready.ok()) << ready.error().message;
  ASSERT_TRUE(ready.value().has_value());
  EXPECT_EQ(ready.value()->latch.recovery_status, StopFaultRecoveryStatusDTO::kReady);
  EXPECT_EQ(ready.value()->latch.captured_resources, std::vector<ResourceId>{uncertain});

  const auto cleared = kernel.recover_stop_fault(
      idle_stop_recovery_intent("request.uncertain.clear", "kernel.epoch.new", *ready.value()));
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  EXPECT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  // The fault clear never reconciles the orphan resource itself.
  ASSERT_EQ(kernel.resource_snapshot().size(), 1U);
  EXPECT_EQ(kernel.resource_snapshot().front().state,
            ResourceSlotState::kUnreconciledExternalOrOrphanGoal);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.stop_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     IdleCellWideStopWithoutInstalledDomainsPublishesTheLatchWithRecoveryUnavailable) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto qualified = registration(binding, engine);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 8U};
  config.graph_revision = binding.value().graph_revision;
  config.empty_runtime_graph_revision = binding.value().graph_revision;
  config.empty_runtime_retained_bundle_id = binding.value().retained_bundle_id;
  config.cell_stop_deadline_budget =
      qualified.stop_readiness->value().resolved_stop_base_plan.deadline_budget;
  config.cell_stop_deadline_budget->phases.clear();
  config.stop_fault_recovery_facts_resolver = installed_domain_recovery_facts({});
  ResourceAddressedExecutionKernel kernel({}, clock, scheduler, journal, std::move(config));

  const auto malformed = kernel.recognized_malformed_stop(
      idle_malformed_stop("malformed-stop.empty.1", "kernel.epoch.1"));
  ASSERT_TRUE(malformed.ok()) << malformed.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(malformed.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  ASSERT_TRUE(latch->stop_certificate.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kScopePromotion);

  const auto offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_TRUE(offer.value().has_value());
  // The recovery assessment may advance between the two reads; the offer
  // must name the same latch and Stop, not a byte-identical snapshot.
  EXPECT_EQ(offer.value()->latch.latch_id, latch->latch_id);
  EXPECT_EQ(offer.value()->latch.latch_generation, latch->latch_generation);
  EXPECT_EQ(offer.value()->latch.stop_id, latch->stop_id);
  EXPECT_EQ(offer.value()->latch.stop_certificate, latch->stop_certificate);
  EXPECT_EQ(offer.value()->latch.trigger, latch->trigger);
  EXPECT_TRUE(offer.value()->configuration_expectations.empty());
  ASSERT_TRUE(offer.value()->recovery_unavailable_reason.has_value());
  EXPECT_EQ(*offer.value()->recovery_unavailable_reason,
            kStopFaultRecoveryInstalledConfigurationUnavailable);
  EXPECT_FALSE(stop_fault_recovery_offer_field_error(*offer.value()).has_value());
  // An empty table without the explicit unavailability is never a valid
  // proof, and an unavailable offer cannot also carry rows.
  auto empty_proof = *offer.value();
  empty_proof.recovery_unavailable_reason.reset();
  EXPECT_TRUE(stop_fault_recovery_offer_field_error(empty_proof).has_value());
  auto fabricated = *offer.value();
  fabricated.configuration_expectations = registered_domain_configurations({binding});
  EXPECT_TRUE(stop_fault_recovery_offer_field_error(fabricated).has_value());

  // The facts owner fails closed the same way; the latch stays latched.
  const auto refreshed = kernel.refresh_stop_fault_recovery_assessment();
  ASSERT_FALSE(refreshed.ok());
  EXPECT_EQ(refreshed.error().code, kStopFaultRecoveryInstalledConfigurationUnavailable);
  const auto still = kernel.get_stop_fault_latch();
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(still->state, StopFaultLatchStateDTO::kLatched);
  EXPECT_NE(still->recovery_status, StopFaultRecoveryStatusDTO::kReady);
}

TEST(ResourceAddressedExecutionKernelTest,
     StaleExplicitFullAggregatesOneResidentStopAndFreshScopeWithoutRefenceOrRedispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.aggregate.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.aggregate.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  std::promise<void> first_stage_entered_promise;
  auto first_stage_entered = first_stage_entered_promise.get_future();
  std::promise<void> release_first_stage_promise;
  const auto release_first_stage = release_first_stage_promise.get_future().share();
  first_engine.translate_stop_stage_hook = [&] {
    first_stage_entered_promise.set_value();
    release_first_stage.wait();
  };
  const auto predecessor =
      kernel.stop(stop_request("request.aggregate.predecessor", first.value(), 1U));
  ASSERT_TRUE(predecessor.ok()) << predecessor.error().message;
  const auto entered_status = first_stage_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_first_stage_promise.set_value();
    FAIL() << "predecessor Stop stage did not reach its blocking seam";
  }

  clock.advance(100);
  const FullStopIntentDTO full_intent{
      PublicRequestContextDTO{RequestId("request.aggregate.full"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      StopGeneration(0U),
      {}};
  const auto aggregate = kernel.full_stop(full_intent);
  if (!aggregate.ok()) {
    release_first_stage_promise.set_value();
    FAIL() << aggregate.error().message;
  }
  EXPECT_EQ(aggregate.value().resolution_disposition,
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved);
  ASSERT_FALSE(aggregate.value().stale_diagnostics.empty());
  EXPECT_EQ(aggregate.value().resolution_reason, StopScopeResolutionReasonDTO::kExplicitFullStop);
  ASSERT_EQ(aggregate.value().joined_predecessor_stops.size(), 1U);
  EXPECT_EQ(aggregate.value().joined_predecessor_stops.front().predecessor_stop_id,
            predecessor.value().stop_id);
  EXPECT_EQ(aggregate.value().remaining_outer_duration_ns,
            predecessor.value().remaining_outer_duration_ns - 100);
  ASSERT_EQ(aggregate.value().affected_operation_revisions.size(), 2U);
  const auto inherited = std::find_if(
      aggregate.value().affected_operation_revisions.begin(),
      aggregate.value().affected_operation_revisions.end(),
      [&](const auto& affected) { return affected.operation_id == first.value().operation_id; });
  ASSERT_NE(inherited, aggregate.value().affected_operation_revisions.end());
  EXPECT_EQ(
      inherited->operation_revision_after_acceptance,
      predecessor.value().affected_operation_revisions.front().operation_revision_after_acceptance);
  EXPECT_TRUE(std::any_of(aggregate.value().participant_budget_allocations.begin(),
                          aggregate.value().participant_budget_allocations.end(),
                          [&](const auto& budget) {
                            return budget.operation_id == first.value().operation_id &&
                                   budget.origin_stop_id == predecessor.value().stop_id;
                          }));
  EXPECT_TRUE(std::any_of(aggregate.value().participant_budget_allocations.begin(),
                          aggregate.value().participant_budget_allocations.end(),
                          [&](const auto& budget) {
                            return budget.operation_id == second.value().operation_id &&
                                   budget.origin_stop_id == aggregate.value().stop_id;
                          }));
  EXPECT_EQ(stop_accepted_field_error(aggregate.value()), std::nullopt);
  ASSERT_TRUE(wait_until(
      [&] {
        std::lock_guard lock(journal.mutex);
        return journal.last_stop_accepted.has_value() &&
               journal.last_stop_accepted->stop_id == aggregate.value().stop_id;
      },
      std::chrono::seconds(10)));
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(*journal.last_stop_accepted, aggregate.value());
    ASSERT_TRUE(journal.last_stop_affected_state.has_value());
    ASSERT_EQ(journal.last_stop_affected_state->operations.size(), 2U);
    const auto inherited_state = std::find_if(
        journal.last_stop_affected_state->operations.begin(),
        journal.last_stop_affected_state->operations.end(),
        [&](const auto& state) { return state.operation_id == first.value().operation_id; });
    ASSERT_NE(inherited_state, journal.last_stop_affected_state->operations.end());
    EXPECT_EQ(inherited_state->active_stop_id, predecessor.value().stop_id);
    EXPECT_EQ(inherited_state->active_stop_generation, predecessor.value().stop_generation);
  }

  const auto later_overlap =
      kernel.stop(stop_request("request.aggregate.later-overlap", first.value(), 2U, 1U));
  release_first_stage_promise.set_value();
  ASSERT_TRUE(later_overlap.ok()) << later_overlap.error().message;
  EXPECT_EQ(later_overlap.value().stop_id, aggregate.value().stop_id);
  EXPECT_EQ(later_overlap.value().joined_predecessor_stops,
            aggregate.value().joined_predecessor_stops);
  EXPECT_EQ(stop_accepted_field_error(later_overlap.value()), std::nullopt);

  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; },
      std::chrono::seconds(10)));
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
  ASSERT_TRUE(wait_until(
      [&] {
        const auto status = kernel.get_stop_status(aggregate.value().stop_id);
        return status.ok() && status.value().stop_certificate.has_value();
      },
      std::chrono::seconds(10)));
  const auto status = kernel.get_stop_status(aggregate.value().stop_id);
  ASSERT_TRUE(status.ok());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok());
  EXPECT_EQ(stop_certificate_field_error(certificate.value()), std::nullopt);
  EXPECT_EQ(certificate.value().resolution_disposition,
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved);
  EXPECT_EQ(certificate.value().predecessor_certificates.size(), 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrdinaryScopeCrossingResidentAndFreshWorkPromotesToOneFullAggregate) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.aggregate-ordinary.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.aggregate-ordinary.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  std::promise<void> first_stage_entered_promise;
  auto first_stage_entered = first_stage_entered_promise.get_future();
  std::promise<void> release_first_stage_promise;
  const auto release_first_stage = release_first_stage_promise.get_future().share();
  first_engine.translate_stop_stage_hook = [&] {
    first_stage_entered_promise.set_value();
    release_first_stage.wait();
  };
  const auto predecessor =
      kernel.stop(stop_request("request.aggregate-ordinary.predecessor", first.value(), 1U));
  ASSERT_TRUE(predecessor.ok()) << predecessor.error().message;
  const auto entered_status = first_stage_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_first_stage_promise.set_value();
    FAIL() << "predecessor Stop stage did not reach its blocking seam";
  }

  std::vector<StopOperationTargetDTO> targets{
      {first.value().operation_id, OperationRevision(2U), StopGeneration(1U)},
      {second.value().operation_id, OperationRevision(1U), StopGeneration(0U)}};
  std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
    return left.operation_id < right.operation_id;
  });
  const StopIntentDTO crossing{
      PublicRequestContextDTO{RequestId("request.aggregate-ordinary.crossing"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      std::move(targets),
      std::nullopt,
      {}};
  const auto aggregate = kernel.stop(crossing);
  release_first_stage_promise.set_value();
  ASSERT_TRUE(aggregate.ok()) << aggregate.error().message;
  EXPECT_EQ(aggregate.value().resolution_disposition,
            StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(aggregate.value().resolution_reason,
            StopScopeResolutionReasonDTO::kOverlappingStopTransactions);
  EXPECT_EQ(aggregate.value().requested_selector_digest,
            ComputeRequestedStopSelectorDigest(crossing).value());
  ASSERT_EQ(aggregate.value().joined_predecessor_stops.size(), 1U);
  EXPECT_EQ(aggregate.value().joined_predecessor_stops.front().predecessor_stop_id,
            predecessor.value().stop_id);
  EXPECT_EQ(stop_accepted_field_error(aggregate.value()), std::nullopt);
  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; },
      std::chrono::seconds(10)));
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     FullAggregateRetainsTwoResidentStopsAndNeverRepeatsEitherStage) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.aggregate-two.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.aggregate-two.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  std::promise<void> first_stage_entered_promise;
  auto first_stage_entered = first_stage_entered_promise.get_future();
  std::promise<void> second_stage_entered_promise;
  auto second_stage_entered = second_stage_entered_promise.get_future();
  std::promise<void> release_stages_promise;
  const auto release_stages = release_stages_promise.get_future().share();
  first_engine.translate_stop_stage_hook = [&] {
    first_stage_entered_promise.set_value();
    release_stages.wait();
  };
  second_engine.translate_stop_stage_hook = [&] {
    second_stage_entered_promise.set_value();
    release_stages.wait();
  };
  const auto first_stop =
      kernel.stop(stop_request("request.aggregate-two.first", first.value(), 1U));
  ASSERT_TRUE(first_stop.ok()) << first_stop.error().message;
  const auto entered_status = first_stage_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_stages_promise.set_value();
    FAIL() << "first predecessor did not reach its blocking stage";
  }
  const auto second_stop =
      kernel.stop(stop_request("request.aggregate-two.second", second.value(), 1U));
  if (!second_stop.ok()) {
    release_stages_promise.set_value();
    FAIL() << second_stop.error().message;
  }
  const auto second_entered_status = second_stage_entered.wait_for(std::chrono::seconds(10));
  if (second_entered_status != std::future_status::ready) {
    release_stages_promise.set_value();
    FAIL() << "second predecessor did not reach its blocking stage";
  }
  EXPECT_NE(first_stop.value().stop_id, second_stop.value().stop_id);

  const FullStopIntentDTO full_intent{
      PublicRequestContextDTO{RequestId("request.aggregate-two.full"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      second_stop.value().stop_generation,
      {}};
  const auto aggregate = kernel.full_stop(full_intent);
  release_stages_promise.set_value();
  ASSERT_TRUE(aggregate.ok()) << aggregate.error().message;
  ASSERT_EQ(aggregate.value().joined_predecessor_stops.size(), 2U);
  EXPECT_EQ(aggregate.value().joined_predecessor_stops[0].predecessor_stop_id,
            first_stop.value().stop_id);
  EXPECT_EQ(aggregate.value().joined_predecessor_stops[1].predecessor_stop_id,
            second_stop.value().stop_id);
  EXPECT_EQ(aggregate.value().affected_operation_revisions.size(), 2U);
  EXPECT_EQ(stop_accepted_field_error(aggregate.value()), std::nullopt);
  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; },
      std::chrono::seconds(10)));
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     CorrelationExhaustionCoalescesFreshScopeWithResidentPredecessorTruth) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"),
                                             32U,
                                             {},
                                             nullptr,
                                             std::nullopt,
                                             std::nullopt,
                                             1U});
  const auto first = kernel.execute(motion_operation("request.reserve-full.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.reserve-full.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  std::promise<void> first_stage_entered_promise;
  auto first_stage_entered = first_stage_entered_promise.get_future();
  std::promise<void> release_first_stage_promise;
  const auto release_first_stage = release_first_stage_promise.get_future().share();
  first_engine.translate_stop_stage_hook = [&] {
    first_stage_entered_promise.set_value();
    release_first_stage.wait();
  };
  const auto predecessor =
      kernel.stop(stop_request("request.reserve-full.first", first.value(), 1U));
  ASSERT_TRUE(predecessor.ok()) << predecessor.error().message;
  const auto entered_status = first_stage_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_first_stage_promise.set_value();
    FAIL() << "predecessor did not reach its blocking stage";
  }

  const auto coalesced =
      kernel.stop(stop_request("request.reserve-full.second", second.value(), 1U));
  release_first_stage_promise.set_value();
  ASSERT_TRUE(coalesced.ok()) << coalesced.error().message;
  EXPECT_EQ(coalesced.value().resolution_disposition,
            StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(coalesced.value().resolution_reason,
            StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced);
  ASSERT_EQ(coalesced.value().joined_predecessor_stops.size(), 1U);
  EXPECT_EQ(coalesced.value().joined_predecessor_stops.front().predecessor_stop_id,
            predecessor.value().stop_id);
  EXPECT_EQ(coalesced.value().affected_operation_revisions.size(), 2U);
  EXPECT_EQ(stop_accepted_field_error(coalesced.value()), std::nullopt);
  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; },
      std::chrono::seconds(10)));
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     TerminalHandoffReusesCorrelationSlotDespiteTerminalJournalFailure) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"),
                                             32U,
                                             {},
                                             nullptr,
                                             std::nullopt,
                                             std::nullopt,
                                             1U});
  const auto first = kernel.execute(motion_operation("request.reuse.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.reuse.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  journal.fail_next_kind = "stop_terminal";
  const auto first_stop = kernel.stop(stop_request("request.reuse.first", first.value(), 1U));
  ASSERT_TRUE(first_stop.ok()) << first_stop.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(first_stop.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kFailed;
  }));

  const auto second_stop = kernel.stop(stop_request("request.reuse.second", second.value(), 1U));
  ASSERT_TRUE(second_stop.ok()) << second_stop.error().message;
  EXPECT_EQ(second_stop.value().resolution_disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_EQ(second_stop.value().resolution_reason,
            StopScopeResolutionReasonDTO::kExactCurrentScope);
  EXPECT_TRUE(second_stop.value().joined_predecessor_stops.empty());
  EXPECT_EQ(second_stop.value().stop_generation, StopGeneration(2U));
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(second_stop.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  EXPECT_EQ(first_engine.translate_stop_stage_calls.load(), 1);
  EXPECT_EQ(second_engine.translate_stop_stage_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     SlowAcceptanceProjectionCannotBeOverwrittenWhenItsEmergencySlotIsReused) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  std::promise<void> first_projection_entered_promise;
  auto first_projection_entered = first_projection_entered_promise.get_future();
  std::promise<void> release_first_projection_promise;
  const auto release_first_projection = release_first_projection_promise.get_future().share();
  std::atomic<std::size_t> projection_entries{};
  journal.stop_acceptance_hook = [&] {
    if (projection_entries.fetch_add(1U) == 0U) {
      first_projection_entered_promise.set_value();
      release_first_projection.wait();
    }
  };

  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"),
                                             32U,
                                             {},
                                             nullptr,
                                             std::nullopt,
                                             std::nullopt,
                                             1U});
  const auto first = kernel.execute(motion_operation("request.projection-reuse.first.execute"));
  auto second_intent = motion_operation("request.projection-reuse.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);

  std::optional<StopAcceptedDTO> first_stop;
  std::optional<StopAcceptedDTO> second_stop;
  bool first_projection_started = false;
  bool first_terminal = false;
  bool second_terminal = false;
  if (first.ok() && second.ok()) {
    auto accepted = kernel.stop(stop_request("request.projection-reuse.first", first.value(), 1U));
    if (accepted.ok()) {
      first_stop = accepted.value();
      first_projection_started =
          first_projection_entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
      first_terminal = wait_until([&] {
        const auto status = kernel.get_stop_status(first_stop->stop_id);
        return status.ok() && status.value().stop_certificate.has_value();
      });
      auto next = kernel.stop(stop_request("request.projection-reuse.second", second.value(), 1U));
      if (next.ok()) {
        second_stop = next.value();
        second_terminal = wait_until([&] {
          const auto status = kernel.get_stop_status(second_stop->stop_id);
          return status.ok() && status.value().stop_certificate.has_value();
        });
      }
    }
  }
  release_first_projection_promise.set_value();

  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  ASSERT_TRUE(first_stop.has_value());
  ASSERT_TRUE(first_projection_started);
  ASSERT_TRUE(first_terminal);
  ASSERT_TRUE(second_stop.has_value());
  ASSERT_TRUE(second_terminal);
  ASSERT_TRUE(wait_until([&] {
    return journal.stop_acceptance_calls.load() == 2 && journal.stop_terminal_calls.load() == 2;
  }));
  ASSERT_EQ(journal.projected_stop_acceptances.size(), 2U);
  ASSERT_EQ(journal.projected_stop_terminals.size(), 2U);
  auto projected_acceptances = journal.projected_stop_acceptances;
  auto projected_terminals = journal.projected_stop_terminals;
  const std::vector<StopId> expected{first_stop->stop_id, second_stop->stop_id};
  std::sort(projected_acceptances.begin(), projected_acceptances.end());
  std::sort(projected_terminals.begin(), projected_terminals.end());
  EXPECT_EQ(projected_acceptances, expected);
  EXPECT_EQ(projected_terminals, expected);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProjectionPoolExhaustionCannotTerminateOrPinAnAlreadyAcceptedStop) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  std::promise<void> projection_entered_promise;
  auto projection_entered = projection_entered_promise.get_future();
  std::promise<void> release_projection_promise;
  const auto release_projection = release_projection_promise.get_future().share();
  std::atomic<bool> first_projection{true};
  journal.stop_acceptance_hook = [&] {
    if (first_projection.exchange(false)) {
      projection_entered_promise.set_value();
      release_projection.wait();
    }
  };

  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"),
                                             8U,
                                             {},
                                             nullptr,
                                             std::nullopt,
                                             std::nullopt,
                                             1U,
                                             2U,
                                             1U});
  const auto first = kernel.execute(motion_operation("request.projection-capacity.execute.1"));
  auto second_intent = motion_operation("request.projection-capacity.execute.2");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);

  bool first_projection_started = false;
  bool stop_failed = false;
  bool terminal_failed_closed = false;
  std::optional<ErrorDTO> terminal_projection_error;
  if (first.ok() && second.ok()) {
    const std::array operations{first.value(), second.value()};
    for (std::size_t index = 0U; index < operations.size(); ++index) {
      auto stopped = kernel.stop(stop_request(
          "request.projection-capacity.stop." + std::to_string(index), operations[index], 1U));
      if (!stopped.ok()) {
        stop_failed = true;
        break;
      }
      if (index == 0U) {
        first_projection_started =
            projection_entered.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
      }
      if (!wait_until([&] {
            const auto status = kernel.get_stop_status(stopped.value().stop_id);
            if (!status.ok() || !status.value().stop_certificate.has_value()) {
              return false;
            }
            if (status.value().terminal_projection.state ==
                StopDurableProjectionStateDTO::kFailed) {
              terminal_failed_closed = true;
              terminal_projection_error = status.value().terminal_projection.error;
            }
            return true;
          })) {
        stop_failed = true;
        break;
      }
    }
  } else {
    stop_failed = true;
  }
  release_projection_promise.set_value();

  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_TRUE(first_projection_started);
  EXPECT_FALSE(stop_failed);
  EXPECT_TRUE(terminal_failed_closed);
  ASSERT_TRUE(terminal_projection_error.has_value());
  EXPECT_EQ(terminal_projection_error->code,
            "operation_journal_stop_terminal_projection_capacity_exhausted");
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_TRUE(latch->persistence_degraded);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopBypassesOrdinaryLedgerCapacityAndProjectionFailureCannotUndoContainment) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 1U});
  const auto first = kernel.execute(motion_operation("request.stop-journal.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  journal.fail_next_kind = "stop_accepted";

  const auto stopped = kernel.stop(stop_request("request.stop-journal.stop", first.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    return first_engine.stop_calls.load() == 1 && journal.stop_acceptance_calls.load() == 1;
  }));
  const auto stopped_state = kernel.get_operation_state(first.value().operation_id);
  ASSERT_TRUE(stopped_state.ok());
  EXPECT_TRUE(stopped_state.value().state == OperationStateKind::kStopping ||
              stopped_state.value().state == OperationStateKind::kStopped);

  auto second = motion_operation("request.stop-journal.second.execute");
  second.program_binding_id = second_binding.value().program_binding_id;
  ASSERT_TRUE(wait_until([&] {
    const auto preview = kernel.begin_motion_preview_session(second);
    return !preview.ok() && preview.error().details.contains("reason") &&
           preview.error().details.at("reason") == "motion_admission_closed";
  }));
  const auto rejected = kernel.execute(second);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "motion_admission_closed");
  EXPECT_EQ(second_engine.execute_calls, 0);

  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kPersistenceDegraded);
  EXPECT_TRUE(latch->persistence_degraded);
  EXPECT_EQ(latch->terminal_projection.state, StopDurableProjectionStateDTO::kRecorded);
  EXPECT_EQ(journal.stop_terminal_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     TerminalProjectionFailureRetainsCertificateAndLatchesPersistenceFault) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  journal.fail_next_kind = "stop_terminal";
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation("request.terminal-projection.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.terminal-projection.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kFailed;
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_FALSE(stop_certificate_field_error(certificate.value()).has_value());
  ASSERT_TRUE(status.value().fault_latch.has_value());
  EXPECT_EQ(status.value().fault_latch->trigger, StopFaultTriggerDTO::kPersistenceDegraded);
  EXPECT_EQ(status.value().fault_latch->terminal_projection.state,
            StopDurableProjectionStateDTO::kFailed);
  // One projection attempt: a journal failure is a storage fault, never
  // retried on a timer.
  ASSERT_TRUE(status.value().terminal_projection.retry_generation.has_value());
  EXPECT_EQ(*status.value().terminal_projection.retry_generation, 1U);
  EXPECT_EQ(journal.stop_terminal_calls.load(), 1);
  EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Background), 0U);
  EXPECT_EQ(engine.stop_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReplacementAdoptsProjectedTerminalCertificateWithoutRedispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  std::optional<RecoveredStopJournalStateDTO> recovered;
  {
    FakeParticipantEngine predecessor_engine;
    FakeOperationJournal predecessor_journal;
    ResourceAddressedExecutionKernel predecessor(
        {registration(binding, predecessor_engine)}, clock, scheduler, predecessor_journal,
        {RequestOwnerEpoch("kernel.epoch.old"), CellId("cell.test"), 8U});
    auto intent = motion_operation("request.recovered-stop.execute");
    intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");
    const auto operation = predecessor.execute(intent);
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    auto stop_intent = stop_request("request.recovered-stop.stop", operation.value(), 1U);
    stop_intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");
    const auto stopped = predecessor.stop(stop_intent);
    ASSERT_TRUE(stopped.ok()) << stopped.error().message;
    ASSERT_TRUE(wait_until([&] {
      std::lock_guard lock(predecessor_journal.mutex);
      return predecessor_journal.last_stop_accepted.has_value() &&
             predecessor_journal.last_stop_affected_state.has_value() &&
             predecessor_journal.last_stop_certificate.has_value();
    }));
    {
      std::lock_guard lock(predecessor_journal.mutex);
      recovered = RecoveredStopJournalStateDTO{
          *predecessor_journal.last_stop_accepted, *predecessor_journal.last_stop_affected_state,
          predecessor_journal.last_stop_certificate, predecessor_journal.last_stop_fault_latch,
          std::nullopt};
    }
  }

  FakeParticipantEngine replacement_engine;
  FakeOperationJournal replacement_journal;
  replacement_journal.recovery = OperationJournalRecoveryDTO{{},
                                                             {*recovered},
                                                             RequestOwnerEpoch("kernel.epoch.old"),
                                                             {ResourceId("robot.joint_1")},
                                                             AdmissionGeneration(0U),
                                                             CellAdmissionClaimGeneration(0U),
                                                             {},
                                                             {},
                                                             {}};
  ResourceAddressedExecutionKernel replacement(
      {registration(binding, replacement_engine)}, clock, scheduler, replacement_journal,
      {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 8U});
  const auto status = replacement.get_stop_status(recovered->accepted.stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  ASSERT_TRUE(status.value().stop_certificate.has_value());
  const auto certificate =
      replacement.get_stop_certificate(status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_EQ(certificate.value(), *recovered->terminal_certificate);
  EXPECT_EQ(replacement.get_stop_fault_latch(), recovered->fault_latch);
  EXPECT_EQ(replacement_engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(replacement_engine.stop_calls.load(), 0);
  ASSERT_EQ(replacement.resource_snapshot().size(), 1U);
  EXPECT_EQ(replacement.resource_snapshot().front().state,
            ResourceSlotState::kUnreconciledExternalOrOrphanGoal);
}

void VerifyStopFaultRecoveryMotionAdmission(const bool latch_manager_liveness_loss,
                                            const bool concurrent_fault_during_clear = false) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeStopSurveillanceObserver observer;
  FakeParticipantEngine stopped_engine;
  FakeParticipantEngine unaffected_engine;
  FakeOperationJournal journal;
  stopped_engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{"controller_stop_rejected", "controller rejected Stop", {}}};

  const auto stopped_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto unaffected_binding = second_same_type_occurrence(stopped_binding);
  const auto& control = *stopped_binding.value().control_path;
  const StopFaultRecoveryConfigurationExpectationDTO expected_configuration{
      control.control_domain_id, control.active_control_configuration_id,
      control.active_control_configuration_generation};
  ManagerLivenessRequiredSetDTO liveness{
      CellId("cell.test"),
      RetainedBundleId("bundle.test"),
      ContentDigest(test_digest('4')),
      RuntimeUnitId("manager"),
      RuntimeEpoch("manager.epoch.1"),
      RequestOwnerEpoch("manager.owner.1"),
      {{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"), RuntimePolicyId("policy.1"),
        RuntimePolicyRevision("1")}}};
  liveness.required_runtime_set_digest =
      ComputeRequiredRuntimeSetDigest(liveness.required_receivers);

  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 32U};
  config.manager_liveness_required_set = liveness;
  config.stop_surveillance =
      StopSurveillanceExecutionConfig{&observer, StopSurveillancePolicyDTO{10, 10, 8U}};
  config.stop_fault_recovery_facts_resolver =
      [expected_configuration](
          const StopStatusDTO& status,
          const std::int64_t assessed_at_ns) -> Result<StopFaultRecoveryFactsDTO> {
    if (!status.fault_latch.has_value() || !status.stop_certificate.has_value()) {
      return Result<StopFaultRecoveryFactsDTO>::Err(
          ErrorDTO{"stop_recovery_test_truth_missing", "terminal truth is missing", {}});
    }
    const auto& latch = *status.fault_latch;
    std::vector<StopFaultRecoveryResourceFactDTO> resources;
    resources.reserve(latch.captured_resources.size());
    for (const auto& resource : latch.captured_resources) {
      resources.push_back(
          StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                           RuntimeEpoch("observed-state.epoch"), 1U, 0, 1'000,
                                           ContentDigest(test_digest('1')), std::nullopt});
    }
    std::vector<StopFaultRecoveryResourceFactDTO> external;
    if (latch.trigger == StopFaultTriggerDTO::kFailedUncontained) {
      external.reserve(latch.captured_resources.size());
      for (const auto& resource : latch.captured_resources) {
        external.push_back(
            StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                             RuntimeEpoch("external-reconciliation.epoch"), 1U, 0,
                                             1'000, ContentDigest(test_digest('2')), std::nullopt});
      }
    }
    return Result<StopFaultRecoveryFactsDTO>::Ok(StopFaultRecoveryFactsDTO{
        assessed_at_ns,
        latch.cell_id,
        latch.retained_bundle_id,
        latch.graph_revision,
        *status.stop_certificate,
        latch.latch_id,
        latch.latch_generation,
        status.acceptance_projection,
        status.terminal_projection,
        status.surveillance_projection,
        status.surveillance,
        std::move(resources),
        {StopFaultRecoveryConfigurationFactDTO{expected_configuration, true,
                                               ContentDigest(test_digest('3'))}},
        {},
        std::move(external),
        {},
        {},
        {}});
  };

  ResourceAddressedExecutionKernel kernel({registration(stopped_binding, stopped_engine),
                                           registration(unaffected_binding, unaffected_engine)},
                                          clock, scheduler, journal, std::move(config));
  const auto operation = kernel.execute(motion_operation("request.surveillance.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  HeldSettlementCall concurrent_strongest;
  std::optional<OperationAcceptedDTO> concurrent_operation;
  std::optional<StopAcceptedDTO> concurrent_stop;
  if (concurrent_fault_during_clear) {
    auto intent = motion_operation("request.surveillance.concurrent");
    intent.program_binding_id = unaffected_binding.value().program_binding_id;
    const auto admitted = kernel.execute(intent);
    ASSERT_TRUE(admitted.ok());
    concurrent_operation = admitted.value();
    unaffected_engine.next_stop_result =
        StopResultDTO{false, ErrorDTO{"controller_stop_rejected", "controller rejected Stop", {}}};
    unaffected_engine.translate_stop_stage_hook =
        [&, hold_concurrent_strongest = concurrent_strongest.Blocker(),
         concurrent_calls = std::make_shared<std::atomic<unsigned>>(0)] {
          if (concurrent_calls->fetch_add(1U) > 0U) hold_concurrent_strongest();
        };
  }
  std::optional<StopAcceptedDTO> stopped;
  if (latch_manager_liveness_loss) {
    kernel.LatchAndContain(ManagerLivenessLossIntentDTO{
        liveness.cell_id, liveness.active_bundle_id, liveness.required_runtime_set_digest,
        liveness.required_receivers.front().runtime_unit_id,
        liveness.required_receivers.front().runtime_epoch,
        liveness.required_receivers.front().runtime_policy_id,
        liveness.required_receivers.front().runtime_policy_revision,
        liveness.manager_runtime_unit_id, liveness.manager_runtime_epoch,
        liveness.manager_owner_epoch, 1U, ManagerLivenessLossKindDTO::kExpiry});
    ASSERT_TRUE(wait_until([&] {
      std::lock_guard lock(journal.mutex);
      if (!journal.last_stop_accepted.has_value()) {
        return false;
      }
      stopped = journal.last_stop_accepted;
      return true;
    }));
  } else {
    const auto accepted =
        kernel.stop(stop_request("request.surveillance.stop", operation.value(), 1U));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    stopped = accepted.value();
  }
  ASSERT_TRUE(stopped.has_value());
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped->stop_id);
    return status.ok() && status.value().stop_certificate.has_value() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded &&
           status.value().surveillance.has_value();
  }));
  EXPECT_EQ(scheduler.last_priority(), SchedulerPriority::Monitoring);

  {
    std::lock_guard lock(journal.mutex);
    journal.fail_next_kind = "stop_surveillance_checkpoint";
  }
  clock.set(110);
  scheduler.advance_to(110);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped->stop_id);
    return journal.stop_surveillance_calls.load() == 1 && status.ok() &&
           status.value().surveillance_projection.state == StopDurableProjectionStateDTO::kFailed;
  }));
  {
    std::lock_guard lock(journal.mutex);
    ASSERT_TRUE(journal.last_stop_surveillance_projected_at_ns.has_value());
    EXPECT_EQ(*journal.last_stop_surveillance_projected_at_ns, 110);
  }
  auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_TRUE(latch->persistence_degraded);
  EXPECT_EQ(latch->recovery_status, StopFaultRecoveryStatusDTO::kWaitingForDurableRecord);

  observer.evidence_state = StopEvidenceItemStateDTO::kSatisfied;
  clock.set(120);
  scheduler.advance_to(120);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped->stop_id);
    return journal.stop_surveillance_calls.load() >= 2 && status.ok() &&
           status.value().surveillance.has_value() &&
           status.value().surveillance->state ==
               StopPostTerminalSurveillanceStateDTO::kMonitoring &&
           status.value().surveillance_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(journal.stop_surveillance_calls.load(), 2);
    ASSERT_TRUE(journal.last_stop_surveillance_projected_at_ns.has_value());
    EXPECT_EQ(*journal.last_stop_surveillance_projected_at_ns, 120);
    EXPECT_EQ(scheduler.pending_count(SchedulerPriority::Background), 0U);
  }
  // The retained evidence contract requires 100 ms of continuous rest. A
  // first satisfied sample cannot collapse that dwell into one observation.
  clock.set(100'000'120);
  scheduler.advance_to(100'000'120);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped->stop_id);
    return journal.stop_surveillance_calls.load() >= 3 && status.ok() &&
           status.value().surveillance.has_value() &&
           status.value().surveillance->state ==
               StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved &&
           status.value().surveillance_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));
  {
    std::lock_guard lock(journal.mutex);
    ASSERT_TRUE(journal.last_stop_surveillance_projected_at_ns.has_value());
    EXPECT_EQ(*journal.last_stop_surveillance_projected_at_ns, 100'000'120);
  }

  auto blocked_intent = motion_operation("request.surveillance.blocked");
  blocked_intent.program_binding_id = unaffected_binding.value().program_binding_id;
  const auto blocked = kernel.execute(blocked_intent);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");

  latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  const auto offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_TRUE(offer.value().has_value());
  EXPECT_EQ(offer.value()->latch, *latch);
  EXPECT_EQ(offer.value()->configuration_expectations,
            std::vector<StopFaultRecoveryConfigurationExpectationDTO>{expected_configuration});
  const auto make_recovery = [&](std::string request_id, const StopFaultLatchDTO& current) {
    return StopFaultRecoveryIntentDTO{
        PublicRequestContextDTO{RequestId(std::move(request_id)),
                                RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
        current.latch_id,
        current.latch_generation,
        current.recovery_assessment_revision,
        current.stop_certificate->stop_certificate_id,
        current.stop_certificate->stop_certificate_revision,
        current.cell_id,
        current.retained_bundle_id,
        current.graph_revision,
        {expected_configuration},
        std::string("verified current owner facts")};
  };
  // Surveillance may publish a newer view between the recovery snapshot and
  // its compare-and-set. The production reconciliation lane retries that
  // explicit conflict; mirror the same contract instead of assuming this
  // test thread always wins the race.
  std::optional<ErrorDTO> unexpected_refresh_error;
  const bool refresh_completed = wait_until([&] {
    const auto result = kernel.refresh_stop_fault_recovery_assessment();
    if (result.ok()) return true;
    const auto reason = result.error().details.find("reason");
    if (reason != result.error().details.end() &&
        reason->second == "stop_fault_recovery_assessment_conflict") {
      return false;
    }
    unexpected_refresh_error = result.error();
    return true;
  });
  ASSERT_FALSE(unexpected_refresh_error.has_value()) << unexpected_refresh_error->message;
  ASSERT_TRUE(refresh_completed);
  const auto refreshed = kernel.get_stop_fault_latch();
  ASSERT_TRUE(refreshed.has_value());
  EXPECT_EQ(refreshed->state, StopFaultLatchStateDTO::kLatched);
  EXPECT_EQ(refreshed->recovery_status, StopFaultRecoveryStatusDTO::kReady);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 0);
  ASSERT_TRUE(kernel.refresh_stop_fault_recovery_assessment().ok());
  EXPECT_EQ(kernel.get_stop_fault_latch(), refreshed);
  EXPECT_FALSE(kernel.execute(blocked_intent).ok());
  const auto assessment_intent = make_recovery("request.surveillance.assess", *latch);
  const auto assessed = kernel.recover_stop_fault(assessment_intent);
  ASSERT_TRUE(assessed.ok()) << assessed.error().message;
  EXPECT_FALSE(assessed.value().accepted);
  EXPECT_EQ(assessed.value().latch.recovery_status, StopFaultRecoveryStatusDTO::kReady);
  const auto assessment_replay = kernel.recover_stop_fault(assessment_intent);
  ASSERT_TRUE(assessment_replay.ok()) << assessment_replay.error().message;
  EXPECT_EQ(assessment_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(assessment_replay.value().latch, assessed.value().latch);

  const auto current = kernel.get_stop_fault_latch();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(*current, assessed.value().latch);
  const auto clear_intent = make_recovery("request.surveillance.clear", *current);
  {
    std::lock_guard lock(journal.mutex);
    journal.fail_next_kind = "stop_fault_cleared";
  }
  const auto failed_clear = kernel.recover_stop_fault(clear_intent);
  ASSERT_FALSE(failed_clear.ok());
  EXPECT_EQ(kernel.get_stop_fault_latch(), current);

  if (concurrent_fault_during_clear) {
    journal.stop_fault_clear_hook = [&] {
      const auto result = kernel.stop(
          stop_request("request.surveillance.concurrent.stop", *concurrent_operation, 1U));
      ASSERT_TRUE(result.ok());
      concurrent_stop = result.value();
      ASSERT_TRUE(concurrent_strongest.Entered());
    };
  }
  const auto cleared = kernel.recover_stop_fault(clear_intent);
  journal.stop_fault_clear_hook = {};
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  EXPECT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 2);
  const auto clear_replay = kernel.recover_stop_fault(clear_intent);
  ASSERT_TRUE(clear_replay.ok()) << clear_replay.error().message;
  EXPECT_EQ(clear_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 2);
  if (concurrent_fault_during_clear) {
    ASSERT_TRUE(concurrent_stop.has_value());
    const auto pending = kernel.get_stop_fault_latch();
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->stop_id, concurrent_stop->stop_id);
    EXPECT_FALSE(pending->stop_certificate.has_value());
    EXPECT_NE(pending->latch_id, cleared.value().latch.latch_id);
    const auto blocked = kernel.execute(motion_operation("request.surveillance.still-blocked"));
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");
    unaffected_engine.next_stop_result = StopResultDTO{true, std::nullopt};
    concurrent_strongest.Release();
    ASSERT_TRUE(wait_until([&] {
      const auto status = kernel.get_stop_status(concurrent_stop->stop_id);
      return status.ok() && status.value().stop_certificate.has_value();
    }));
    EXPECT_EQ(kernel.get_stop_fault_latch()->latch_id, pending->latch_id);
    EXPECT_TRUE(kernel.cell_stop_action_projection().fault.has_value());
    return;
  }
  const auto cleared_offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(cleared_offer.ok()) << cleared_offer.error().message;
  EXPECT_FALSE(cleared_offer.value().has_value());
  EXPECT_FALSE(kernel.cell_stop_action_projection().fault.has_value());

  auto admitted_intent = motion_operation("request.surveillance.admitted");
  admitted_intent.program_binding_id = unaffected_binding.value().program_binding_id;
  if (latch_manager_liveness_loss) {
    const auto preview = kernel.begin_motion_preview_session(admitted_intent);
    ASSERT_FALSE(preview.ok());
    EXPECT_EQ(preview.error().details.at("reason"), "motion_admission_closed");
  }
  const auto admitted = kernel.execute(admitted_intent);
  if (latch_manager_liveness_loss) {
    ASSERT_FALSE(admitted.ok());
    EXPECT_EQ(admitted.error().details.at("reason"), "motion_admission_closed");
    EXPECT_EQ(unaffected_engine.execute_calls, 0);
  } else {
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    EXPECT_EQ(unaffected_engine.execute_calls, 1);

    // A cleared Stop-fault latch remains retained as history. It must never
    // contaminate recovery for a later ordinary Stop or turn that operation
    // into Reset-only intervention.
    const auto ordinary_stop =
        kernel.stop(stop_request("request.surveillance.admitted.stop", admitted.value(), 1U));
    ASSERT_TRUE(ordinary_stop.ok()) << ordinary_stop.error().message;
    ASSERT_TRUE(wait_until([&] {
      const auto state = kernel.get_operation_state(admitted.value().operation_id);
      return state.ok() && state.value().state == OperationStateKind::kStopped;
    }));
    const auto ordinary_recovery =
        kernel.get_operation_recovery_assessment(admitted.value().operation_id);
    ASSERT_TRUE(ordinary_recovery.ok()) << ordinary_recovery.error().message;
    EXPECT_EQ(ordinary_recovery.value().assessment.recovery_class,
              RecoveryClass::kOrdinaryOperatorStop);
    EXPECT_EQ(ordinary_recovery.value().assessment.status, RecoveryAssessmentStatus::kRetryReady);
    EXPECT_TRUE(ordinary_recovery.value().assessment.retry_admissible);
    EXPECT_FALSE(kernel.cell_stop_action_projection().fault.has_value());
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     SurveillanceCheckpointFailureDegradesPersistenceAndRecoveryClearsOnlyAfterDurableCommit) {
  VerifyStopFaultRecoveryMotionAdmission(false);
}

TEST(ResourceAddressedExecutionKernelTest,
     ManagerLossInhibitSurvivesOtherwiseValidStopFaultRecovery) {
  VerifyStopFaultRecoveryMotionAdmission(true);
}

[[nodiscard]] StopFaultRecoveryFactsResolver settled_single_domain_recovery_facts(
    StopFaultRecoveryConfigurationExpectationDTO expected_configuration) {
  return [expected_configuration](
             const StopStatusDTO& status,
             const std::int64_t assessed_at_ns) -> Result<StopFaultRecoveryFactsDTO> {
    if (!status.fault_latch.has_value() || !status.stop_certificate.has_value()) {
      return Result<StopFaultRecoveryFactsDTO>::Err(
          ErrorDTO{"stop_recovery_test_truth_missing", "terminal truth is missing", {}});
    }
    const auto& latch = *status.fault_latch;
    std::vector<StopFaultRecoveryResourceFactDTO> resources;
    std::vector<StopFaultRecoveryResourceFactDTO> external;
    for (const auto& resource : latch.captured_resources) {
      resources.push_back(
          StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                           RuntimeEpoch("observed-state.epoch"), 1U, 0, 1'000,
                                           ContentDigest(test_digest('1')), std::nullopt});
      if (latch.trigger == StopFaultTriggerDTO::kFailedUncontained) {
        external.push_back(
            StopFaultRecoveryResourceFactDTO{resource, StopFaultRecoveryResourceStateDTO::kSettled,
                                             RuntimeEpoch("external-reconciliation.epoch"), 1U, 0,
                                             1'000, ContentDigest(test_digest('2')), std::nullopt});
      }
    }
    return Result<StopFaultRecoveryFactsDTO>::Ok(StopFaultRecoveryFactsDTO{
        assessed_at_ns,
        latch.cell_id,
        latch.retained_bundle_id,
        latch.graph_revision,
        *status.stop_certificate,
        latch.latch_id,
        latch.latch_generation,
        status.acceptance_projection,
        status.terminal_projection,
        status.surveillance_projection,
        status.surveillance,
        std::move(resources),
        {StopFaultRecoveryConfigurationFactDTO{expected_configuration, true,
                                               ContentDigest(test_digest('3'))}},
        {},
        std::move(external),
        {},
        {},
        {}});
  };
}

/// Exact operator recovery of one ready offer (the only way a latch clears).
[[nodiscard]] StopFaultRecoveryIntentDTO operator_stop_fault_recovery(
    const StopFaultRecoveryOfferDTO& offer, const std::string& request_id) {
  const auto& latch = offer.latch;
  return StopFaultRecoveryIntentDTO{
      PublicRequestContextDTO{RequestId(request_id), RequestOwnerEpoch("kernel.epoch.1"),
                              "composer", std::nullopt},
      latch.latch_id,
      latch.latch_generation,
      latch.recovery_assessment_revision,
      latch.stop_certificate->stop_certificate_id,
      latch.stop_certificate->stop_certificate_revision,
      latch.cell_id,
      latch.retained_bundle_id,
      latch.graph_revision,
      offer.configuration_expectations,
      std::string("operator recovered the cell")};
}

/// Addendum 2 (2026-10-08): the Kernel attributes its own availability-loss
/// containment for traceability, but only the operator's exact recovery
/// clears the latch, and neither that nor returning readiness resumes,
/// replays or redispatches the interrupted operation.
TEST(ResourceAddressedExecutionKernelTest,
     AvailabilityLatchOperatorRecoveryNeverResumesOrReplaysInterruptedMotion) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeStopSurveillanceObserver observer;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{"controller_unreachable", "provider is unavailable", {}}};
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto& control = *binding.value().control_path;
  const StopFaultRecoveryConfigurationExpectationDTO expected_configuration{
      control.control_domain_id, control.active_control_configuration_id,
      control.active_control_configuration_generation};
  ManagerLivenessRequiredSetDTO liveness{
      CellId("cell.test"),
      RetainedBundleId("bundle.test"),
      ContentDigest(test_digest('4')),
      RuntimeUnitId("manager"),
      RuntimeEpoch("manager.epoch.1"),
      RequestOwnerEpoch("manager.owner.1"),
      {{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"), RuntimePolicyId("policy.1"),
        RuntimePolicyRevision("1")}}};
  liveness.required_runtime_set_digest =
      ComputeRequiredRuntimeSetDigest(liveness.required_receivers);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 32U};
  config.manager_liveness_required_set = liveness;
  config.stop_surveillance =
      StopSurveillanceExecutionConfig{&observer, StopSurveillancePolicyDTO{10, 10, 8U}};
  config.stop_fault_recovery_facts_resolver =
      settled_single_domain_recovery_facts(expected_configuration);
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));

  const auto operation = kernel.execute(motion_operation("request.availability.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  ASSERT_EQ(engine.execute_calls, 1);

  // The provider's Manager stops reporting: the Kernel issues its own
  // containment Full Stop, which the unreachable controller cannot certify.
  kernel.LatchAndContain(ManagerLivenessLossIntentDTO{
      liveness.cell_id, liveness.active_bundle_id, liveness.required_runtime_set_digest,
      liveness.required_receivers.front().runtime_unit_id,
      liveness.required_receivers.front().runtime_epoch,
      liveness.required_receivers.front().runtime_policy_id,
      liveness.required_receivers.front().runtime_policy_revision,
      liveness.manager_runtime_unit_id, liveness.manager_runtime_epoch,
      liveness.manager_owner_epoch, 1U, ManagerLivenessLossKindDTO::kExpiry});
  std::optional<StopAcceptedDTO> stopped;
  ASSERT_TRUE(wait_until([&] {
    std::lock_guard lock(journal.mutex);
    if (!journal.last_stop_accepted.has_value()) return false;
    stopped = journal.last_stop_accepted;
    return true;
  }));
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped->stop_id);
    return status.ok() && status.value().stop_certificate.has_value() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded &&
           status.value().surveillance.has_value();
  }));
  const auto interrupted = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(interrupted.ok()) << interrupted.error().message;
  const auto interrupted_state = interrupted.value().state;
  EXPECT_NE(interrupted_state, OperationStateKind::kExecuting);
  EXPECT_NE(interrupted_state, OperationStateKind::kCompleted);

  // Fresh physical rest satisfies the retained surveillance dwell.
  observer.evidence_state = StopEvidenceItemStateDTO::kSatisfied;
  const auto surveillance_reaches = [&](const StopPostTerminalSurveillanceStateDTO state) {
    return wait_until([&] {
      const auto status = kernel.get_stop_status(stopped->stop_id);
      return status.ok() && status.value().surveillance.has_value() &&
             status.value().surveillance->state == state &&
             status.value().surveillance_projection.state ==
                 StopDurableProjectionStateDTO::kRecorded;
    });
  };
  clock.set(120);
  scheduler.advance_to(120);
  ASSERT_TRUE(surveillance_reaches(StopPostTerminalSurveillanceStateDTO::kMonitoring));
  clock.set(100'000'120);
  scheduler.advance_to(100'000'120);
  ASSERT_TRUE(surveillance_reaches(StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved));
  auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->stop_id, stopped->stop_id);
  EXPECT_EQ(kernel.get_stop_fault_latch_origin(*latch),
            StopFaultLatchOriginDTO::kProviderAvailabilityLoss);

  std::optional<StopFaultRecoveryOfferDTO> ready;
  ASSERT_TRUE(wait_until([&] {
    static_cast<void>(kernel.refresh_stop_fault_recovery_assessment());
    const auto offer = kernel.get_stop_fault_recovery_offer();
    if (!offer.ok() || !offer.value().has_value() ||
        offer.value()->latch.recovery_status != StopFaultRecoveryStatusDTO::kReady) {
      return false;
    }
    ready = *offer.value();
    return true;
  }));

  // The unreachable controller leaves containment unproven; the ready
  // assessment already required settled rest and external reconciliation.
  ASSERT_EQ(ready->latch.trigger, StopFaultTriggerDTO::kFailedUncontained);
  EXPECT_EQ(kernel.get_stop_fault_latch_origin(ready->latch),
            StopFaultLatchOriginDTO::kProviderAvailabilityLoss);
  // Nothing clears the latch by itself: a ready assessment is only an offer.
  ASSERT_TRUE(kernel.get_stop_fault_latch().has_value());
  EXPECT_EQ(kernel.get_stop_fault_latch()->state, StopFaultLatchStateDTO::kLatched);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 0);

  const auto intent = operator_stop_fault_recovery(*ready, "request.availability.recover");
  const auto cleared = kernel.recover_stop_fault(intent);
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  ASSERT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  const auto replay = kernel.recover_stop_fault(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 1);

  // Nothing resumes, replays or redispatches: the interrupted operation stays
  // terminal and the engine sees no further execute call.
  for (const std::int64_t at : {300'000'120, 400'000'120}) {
    clock.set(at);
    scheduler.advance_to(at);
  }
  const auto after = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(after.ok()) << after.error().message;
  EXPECT_EQ(after.value().state, interrupted_state);
  EXPECT_EQ(engine.execute_calls, 1);
  // Admission stays closed while the availability loss is still latched in
  // the Kernel; only a new explicit request after readiness may move again.
  const auto blocked = kernel.execute(motion_operation("request.availability.after-clear"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     OperatorAndMalformedStopLatchesAreNeverAttributedToAvailabilityLoss) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.stop_fault_recovery_facts_resolver = installed_domain_recovery_facts({binding});
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));
  const auto malformed = kernel.recognized_malformed_stop(
      idle_malformed_stop("malformed-stop.origin.1", "kernel.epoch.1"));
  ASSERT_TRUE(malformed.ok()) << malformed.error().message;
  ASSERT_TRUE(
      wait_until([&] { return stop_terminal_recorded(kernel, malformed.value().stop_id); }));
  ASSERT_TRUE(kernel.refresh_stop_fault_recovery_assessment().ok());
  const auto offer = kernel.get_stop_fault_recovery_offer();
  ASSERT_TRUE(offer.ok()) << offer.error().message;
  ASSERT_TRUE(offer.value().has_value());
  EXPECT_EQ(offer.value()->latch.trigger, StopFaultTriggerDTO::kScopePromotion);
  const auto origin = kernel.get_stop_fault_latch_origin(offer.value()->latch);
  EXPECT_EQ(origin, StopFaultLatchOriginDTO::kMalformedStopPromotion);

  // An explicit operator Full Stop is attributed to the operator.
  const auto full = kernel.full_stop(FullStopIntentDTO{
      PublicRequestContextDTO{RequestId("request.origin.full"),
                              RequestOwnerEpoch("kernel.epoch.1"), "kernel.manager_liveness",
                              std::nullopt},
      StopGeneration(0U),
      {}});
  ASSERT_TRUE(full.ok()) << full.error().message;
  ASSERT_TRUE(wait_until([&] { return stop_terminal_recorded(kernel, full.value().stop_id); }));
  auto full_latch = *offer.value();
  full_latch.latch.stop_id = full.value().stop_id;
  full_latch.latch.stop_generation = full.value().stop_generation;
  // A caller-supplied source label never forges the Kernel's attribution.
  EXPECT_EQ(kernel.get_stop_fault_latch_origin(full_latch.latch),
            StopFaultLatchOriginDTO::kOperatorFullStop);
  EXPECT_EQ(engine.execute_calls, 0);
}

/// Addendum 2 (2026-10-08): a CCLC component incident latches even an idle
/// cell through the ordinary Stop ingress, once per incident, and only the
/// operator's exact recovery clears it. No motion is admitted or replayed.
TEST(ResourceAddressedExecutionKernelTest,
     ComponentIncidentContainmentLatchesAnIdleCellOncePerIncident) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(100);
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.stop_fault_recovery_facts_resolver = installed_domain_recovery_facts({binding});
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));
  ASSERT_FALSE(kernel.get_stop_fault_latch().has_value());

  const ComponentIncidentContainmentIntentDTO intent{
      "incident.arm.1", ControllableComponentId("component.arm"),
      LifecycleBlockingCauseClassDTO::kProviderLost};
  IKernelCellAdmissionPort& port = kernel;
  const auto contained = port.RequestComponentIncidentContainment(intent);
  ASSERT_TRUE(contained.ok()) << contained.error().message;
  EXPECT_TRUE(contained.value().affected_operation_ids.empty());
  EXPECT_FALSE(contained.value().program_run_id.has_value());
  ASSERT_TRUE(wait_until(
      [&] { return stop_terminal_recorded(kernel, contained.value().stop_id); }));

  // A repeated request for the same incident replays the same Stop.
  const auto replay = port.RequestComponentIncidentContainment(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().stop_id, contained.value().stop_id);
  EXPECT_EQ(replay.value().stop_generation, contained.value().stop_generation);
  auto invalid = intent;
  invalid.cause_class = LifecycleBlockingCauseClassDTO::kAvailability;
  EXPECT_FALSE(port.RequestComponentIncidentContainment(invalid).ok());

  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->stop_id, contained.value().stop_id);
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kPhysicalRestUnproven);
  EXPECT_EQ(kernel.get_stop_fault_latch_origin(*latch),
            StopFaultLatchOriginDTO::kComponentIncident);
  const auto status = kernel.get_stop_status(contained.value().stop_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  // System attribution under the Kernel's own request identity.
  EXPECT_EQ(status.value().accepted.resolution_reason,
            StopScopeResolutionReasonDTO::kExplicitFullStop);
  EXPECT_FALSE(kernel.execute(motion_operation("request.incident.blocked")).ok());
  EXPECT_EQ(engine.execute_calls, 0);

  std::optional<StopFaultRecoveryOfferDTO> ready;
  ASSERT_TRUE(wait_until([&] {
    clock.advance(10'000'000);
    scheduler.advance_to(clock.now());
    static_cast<void>(kernel.refresh_stop_fault_recovery_assessment());
    const auto offer = kernel.get_stop_fault_recovery_offer();
    if (!offer.ok() || !offer.value().has_value() ||
        offer.value()->latch.recovery_status != StopFaultRecoveryStatusDTO::kReady) {
      return false;
    }
    ready = *offer.value();
    return true;
  }));
  // A ready assessment clears nothing by itself.
  EXPECT_EQ(kernel.get_stop_fault_latch()->state, StopFaultLatchStateDTO::kLatched);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 0);
  const auto cleared = kernel.recover_stop_fault(
      idle_stop_recovery_intent("request.incident.clear", "kernel.epoch.1", *ready));
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  EXPECT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     DurableClearOfOlderStopFaultCannotReopenWhileNewerFaultIsStillContaining) {
  VerifyStopFaultRecoveryMotionAdmission(false, true);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopCancelsAReservedOperationWhileItsOrdinaryJournalIsBlockedWithoutDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::promise<void> journal_entered_promise;
  auto journal_entered = journal_entered_promise.get_future();
  std::promise<void> release_journal_promise;
  const auto release_journal = release_journal_promise.get_future().share();
  journal.operation_acceptance_hook = [&] {
    journal_entered_promise.set_value();
    release_journal.wait();
  };
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  auto execution = std::async(std::launch::async, [&] {
    return kernel.execute(motion_operation("request.blocked-journal.execute"));
  });
  const auto entered_status = journal_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_journal_promise.set_value();
    static_cast<void>(execution.get());
    FAIL() << "ordinary journal did not reach its blocking test seam";
  }

  const StopIntentDTO stop_intent{
      PublicRequestContextDTO{RequestId("request.blocked-journal.stop"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      std::vector<StopComponentTargetDTO>{{binding.value().controllable_component_id,
                                           binding.value().graph_revision,
                                           ComponentLifecycleGeneration(1U)}},
      std::nullopt,
      {}};
  const auto stopped = kernel.stop(stop_intent);
  release_journal_promise.set_value();
  const auto executed = execution.get();
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_EQ(stopped.value().affected_operation_revisions.size(), 1U);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);

  ASSERT_TRUE(executed.ok()) << executed.error().message;
  EXPECT_EQ(executed.value().operation_id,
            stopped.value().affected_operation_revisions.front().operation_id);
  ASSERT_TRUE(wait_until([&] {
    const auto current = kernel.get_operation_state(executed.value().operation_id);
    return current.ok() && current.value().state == OperationStateKind::kStopped;
  }));
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), 0);
  EXPECT_EQ(engine.participant_stop_settlement_calls.load(), 0);
}

void VerifyRecognizedFullStopDoesNotWaitForBlockedCellAdmissionJournalWrite(
    const bool retain_active_resource) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::promise<void> claim_journal_entered_promise;
  auto claim_journal_entered = claim_journal_entered_promise.get_future();
  std::promise<void> release_claim_journal_promise;
  const auto release_claim_journal = release_claim_journal_promise.get_future().share();
  journal.cell_admission_acquire_hook = [&] {
    claim_journal_entered_promise.set_value();
    release_claim_journal.wait();
  };
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.cell_admission_settlement_validator = [](const auto&, const auto&) {
    return Result<void>::Ok();
  };
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));
  if (retain_active_resource) {
    const auto executed = kernel.execute(motion_operation("request.active-before-claim"));
    ASSERT_TRUE(executed.ok()) << executed.error().message;
  }
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  const auto generation = kernel.GetAuthoringAdmission().admission_generation;
  const CellAdmissionClaimIntentDTO claim{
      CellAdmissionClaimKindDTO::kAuthoringMutation,
      CellAdmissionOwnerRoleDTO::kAuthoringOwner,
      RequestOwnerEpoch("authoring.owner.1"),
      "document.transaction.1",
      RequestId("request.cell-admission.blocked"),
      ContentDigest("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
      generation,
      {},
      std::nullopt,
      std::nullopt};
  auto acquiring =
      std::async(std::launch::async, [&] { return kernel.AcquireCellAdmissionClaim(claim); });
  const auto entered_status = claim_journal_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_claim_journal_promise.set_value();
    static_cast<void>(acquiring.get());
    FAIL() << "cell-admission journal did not reach its blocking seam";
  }

  const FullStopIntentDTO stop{
      PublicRequestContextDTO{RequestId("request.full-stop.independent"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      StopGeneration(0U),
      {}};
  const auto accepted = kernel.full_stop(stop);
  release_claim_journal_promise.set_value();
  const auto acquired = acquiring.get();
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  EXPECT_EQ(accepted.value().resolution_reason, StopScopeResolutionReasonDTO::kExplicitFullStop);
  EXPECT_EQ(accepted.value().affected_operation_revisions.empty(), !retain_active_resource);
}

TEST(ResourceAddressedExecutionKernelTest,
     RecognizedFullStopDoesNotWaitForBlockedCellAdmissionJournalWrite) {
  VerifyRecognizedFullStopDoesNotWaitForBlockedCellAdmissionJournalWrite(false);
}

TEST(ResourceAddressedExecutionKernelTest,
     ActiveResourceFullStopDoesNotWaitForBlockedCellAdmissionJournalWrite) {
  VerifyRecognizedFullStopDoesNotWaitForBlockedCellAdmissionJournalWrite(true);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReceiverExpiryPreemptsBlockedUnfenceAndContainsActiveOperation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ManagerLivenessRequiredSetDTO liveness{
      CellId("cell.test"),
      RetainedBundleId("bundle.test"),
      ContentDigest(test_digest('1')),
      RuntimeUnitId("manager"),
      RuntimeEpoch("manager.epoch.1"),
      RequestOwnerEpoch("manager.owner.1"),
      {{RuntimeUnitId("runtime.a"), RuntimeEpoch("runtime.a.epoch"), RuntimePolicyId("policy.1"),
        RuntimePolicyRevision("1")}}};
  liveness.required_runtime_set_digest =
      ComputeRequiredRuntimeSetDigest(liveness.required_receivers);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.manager_liveness_required_set = liveness;
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));

  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  ManagerLivenessReceiver receiver(
      ManagerLivenessReceiverConfig{
          liveness.cell_id, liveness.required_receivers.front().runtime_unit_id,
          liveness.required_receivers.front().runtime_epoch, liveness.manager_runtime_unit_id,
          liveness.manager_runtime_epoch, liveness.manager_owner_epoch,
          ManagerLivenessPolicyDTO{liveness.required_receivers.front().runtime_policy_id,
                                   liveness.required_receivers.front().runtime_policy_revision, 10,
                                   30, ProductContractId("safe.1"), ProductContractRevision("1")},
          liveness.active_bundle_id, liveness.required_runtime_set_digest},
      clock, scheduler,
      []() -> ContentDigest { throw std::runtime_error("injected local-safe failure"); }, kernel,
      kernel);
  ASSERT_TRUE(
      receiver
          .Renew(ManagerHeartbeatDTO{liveness.cell_id, liveness.manager_runtime_unit_id,
                                     liveness.manager_runtime_epoch, liveness.manager_owner_epoch,
                                     1U, liveness.required_receivers.front().runtime_policy_id,
                                     liveness.required_receivers.front().runtime_policy_revision})
          .ok());
  const auto operation = kernel.execute(motion_operation("request.manager-loss.active"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;

  std::promise<void> publication_entered_promise;
  auto publication_entered = publication_entered_promise.get_future();
  std::promise<void> release_publication_promise;
  auto release_publication = release_publication_promise.get_future().share();
  journal.cell_admission_generation_hook = [&] {
    publication_entered_promise.set_value();
    release_publication.wait();
  };
  auto publishing = std::async(std::launch::async, [&] {
    return receiver.CompleteReconciliation(ManagerLivenessReconciliationAck{
        liveness.cell_id, liveness.manager_runtime_unit_id, liveness.manager_runtime_epoch,
        liveness.manager_owner_epoch, liveness.active_bundle_id,
        liveness.required_runtime_set_digest, ContentDigest(test_digest('7'))});
  });
  if (publication_entered.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    release_publication_promise.set_value();
    static_cast<void>(publishing.get());
    FAIL() << "Manager-liveness publication did not reach its blocking seam";
  }

  clock.set(30);
  auto expiring = std::async(std::launch::async, [&] { scheduler.advance_to(30); });
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(operation.value().operation_id);
    return state.ok() && state.value().state != OperationStateKind::kPrepared;
  }));
  EXPECT_EQ(receiver.status().state, ManagerLivenessStateDTO::kExpired);
  EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  release_publication_promise.set_value();
  expiring.get();
  const auto reconciliation = publishing.get();
  ASSERT_FALSE(reconciliation.ok());
  EXPECT_EQ(reconciliation.error().details.at("reason"),
            "manager_liveness_reconciliation_superseded");
  EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  const auto contained = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(contained.ok()) << contained.error().message;
  EXPECT_NE(contained.value().state, OperationStateKind::kPrepared);
}

TEST(ResourceAddressedExecutionKernelTest,
     CellClaimAndOrdinaryAdmissionShareOneDeterministicKernelOrdering) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::promise<void> claim_linearized_promise;
  auto claim_linearized = claim_linearized_promise.get_future();
  std::promise<void> release_claim_promise;
  auto release_claim = release_claim_promise.get_future().share();
  journal.cell_admission_acquire_hook = [&] {
    claim_linearized_promise.set_value();
    release_claim.wait();
  };
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel({registration(binding, engine)}, clock, scheduler,
                                          journal, std::move(config));
  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  const CellAdmissionClaimIntentDTO claim{CellAdmissionClaimKindDTO::kAuthoringMutation,
                                          CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                                          RequestOwnerEpoch("authoring.owner.1"),
                                          "transaction.1",
                                          RequestId("request.cell-ordering"),
                                          ContentDigest(test_digest('9')),
                                          kernel.GetAuthoringAdmission().admission_generation,
                                          {},
                                          std::nullopt,
                                          std::nullopt};
  auto acquiring =
      std::async(std::launch::async, [&] { return kernel.AcquireCellAdmissionClaim(claim); });
  claim_linearized.wait();
  auto executing = std::async(std::launch::async, [&] {
    return kernel.execute(motion_operation("request.motion.cell-ordering"));
  });
  EXPECT_EQ(engine.execute_calls.load(), 0);
  release_claim_promise.set_value();
  ASSERT_TRUE(acquiring.get().ok());
  ASSERT_TRUE(executing.get().ok());
  EXPECT_EQ(engine.execute_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     BlockedDisjointOrdinaryJournalCannotDelayAnActiveOperationsStopStage) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto first = kernel.execute(motion_operation("request.blocked-disjoint.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;

  std::promise<void> journal_entered_promise;
  auto journal_entered = journal_entered_promise.get_future();
  std::promise<void> release_journal_promise;
  const auto release_journal = release_journal_promise.get_future().share();
  journal.operation_acceptance_hook = [&] {
    journal_entered_promise.set_value();
    release_journal.wait();
  };
  auto second_intent = motion_operation("request.blocked-disjoint.second.execute");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  auto second_execution =
      std::async(std::launch::async, [&] { return kernel.execute(second_intent); });
  const auto entered_status = journal_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_journal_promise.set_value();
    static_cast<void>(second_execution.get());
    FAIL() << "disjoint ordinary journal did not reach its blocking test seam";
  }

  const auto stopped =
      kernel.stop(stop_request("request.blocked-disjoint.stop", first.value(), 1U));
  const bool stage_progressed =
      stopped.ok() && wait_until([&] { return first_engine.stop_calls.load() == 1; });
  release_journal_promise.set_value();
  const auto second = second_execution.get();

  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_TRUE(stage_progressed);
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(second_engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrdinaryRequestIdReusePromotesStopToFullWithoutExtendingTheDeadline) {
  FakeClock clock;
  ManualScheduler scheduler;
  clock.set(1000);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.reused-for-stop"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.reused-for-stop.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;

  const auto promoted = kernel.stop(stop_request("request.reused-for-stop", first.value(), 1U));
  ASSERT_TRUE(promoted.ok()) << promoted.error().message;
  EXPECT_EQ(promoted.value().resolution_disposition, StopResolutionDispositionDTO::kPromotedToFull);
  ASSERT_EQ(promoted.value().affected_operation_revisions.size(), 2U);
  ASSERT_TRUE(wait_until(
      [&] { return first_engine.stop_calls.load() == 1 && second_engine.stop_calls.load() == 1; },
      std::chrono::seconds(10)));
  EXPECT_EQ(promoted.value().remaining_outer_duration_ns, promoted.value().fixed_outer_duration_ns);
}

TEST(ResourceAddressedExecutionKernelTest,
     QualifiedScaleFailureAndMechanismMismatchRemainVisibleAfterAcceptance) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  engine.next_runtime_scale_result =
      Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                 "scale handoff failed",
                                 {{"reason", "scale_handoff_failed"}}});

  const auto accepted = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.failure", executed.value(), 1U, 0U, 0.5));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto failed = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(failed.ok());
  EXPECT_EQ(failed.value().state, RuntimeScaleApplicationState::kFailed);
  ASSERT_TRUE(failed.value().participants.front().error.has_value());
  EXPECT_EQ(failed.value().participants.front().error->details.at("reason"),
            "scale_handoff_failed");

  FakeParticipantEngine unsupported_engine;
  unsupported_engine.runtime_scale.supported = false;
  ResourceAddressedExecutionKernel unsupported_kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          unsupported_engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  auto unsupported_intent = motion_operation("request.motion.unsupported");
  const auto unsupported = unsupported_kernel.execute(unsupported_intent);
  ASSERT_TRUE(unsupported.ok()) << unsupported.error().message;
  EXPECT_EQ(unsupported_engine.execute_calls, 1);
  const auto unsupported_state =
      unsupported_kernel.get_operation_state(unsupported.value().operation_id);
  ASSERT_TRUE(unsupported_state.ok()) << unsupported_state.error().message;
  EXPECT_EQ(unsupported_state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(unsupported_state.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(unsupported_state.value().participants.front().terminal_error->details.at("reason"),
            "runtime_scale_mechanism_unsupported");
  EXPECT_EQ(unsupported_kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
}

TEST(ResourceAddressedExecutionKernelTest,
     DirectControlSpanIsRetainedAndOversizedInputFailsBeforeEffect) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  auto direct = motion_operation("request.direct.1");
  direct.participants.front().action_span =
      ActionSpanDTO{ActionSource::kDirectControl, "direct/session.1", "run.1", "", "", ""};
  const auto accepted = kernel.execute(direct);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->action_span.has_value());
  EXPECT_EQ(engine.last_execute->action_span->action_source, ActionSource::kDirectControl);
  EXPECT_EQ(engine.last_execute->action_span->action_span_id, "direct/session.1");

  auto oversized = motion_operation("request.direct.oversized");
  oversized.participants.front().action_span =
      ActionSpanDTO{ActionSource::kDirectControl,
                    std::string(kMaxActionSpanFieldBytes + 1U, 'x'),
                    "run.2",
                    "",
                    "",
                    ""};
  const auto rejected = kernel.execute(oversized);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "operation_action_span_invalid");
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     DurableAcceptancePrecedesEffectsAndWriteFailurePreventsDispatch) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::vector<std::string> timeline;
  std::mutex timeline_mutex;
  engine.timeline = &timeline;
  engine.timeline_mutex = &timeline_mutex;
  journal.timeline = &timeline;
  journal.timeline_mutex = &timeline_mutex;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  {
    std::lock_guard lock(timeline_mutex);
    timeline.clear();
  }

  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(timeline, (std::vector<std::string>{
                          "journal:operation_accepted",
                          "engine:prepare_motion",
                          "journal:state:subordinate_motion_admitted",
                          "journal:state:subordinate_motion_prepared",
                          "engine:dispatch_prepared_motion",
                          "engine:execute",
                          "engine:set_runtime_scale",
                      }));

  FakeParticipantEngine rejected_engine;
  FakeOperationJournal failing_journal;
  failing_journal.fail_next_kind = "operation_accepted";
  ResourceAddressedExecutionKernel failing_kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          rejected_engine)},
      clock, scheduler, failing_journal,
      {RequestOwnerEpoch("kernel.epoch.failure"), CellId("cell.test"), 16U});
  auto rejected_intent = motion_operation("request.motion.failure");
  rejected_intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.failure");
  const auto rejected = failing_kernel.execute(rejected_intent);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "injected_journal_failure");
  EXPECT_EQ(rejected_engine.execute_calls, 0);
  EXPECT_EQ(failing_kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     ScaleStopAndRejectedControlRemainDurableBeforeTheirEffects) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::vector<std::string> timeline;
  std::mutex timeline_mutex;
  engine.timeline = &timeline;
  engine.timeline_mutex = &timeline_mutex;
  journal.timeline = &timeline;
  journal.timeline_mutex = &timeline_mutex;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  {
    std::lock_guard lock(timeline_mutex);
    timeline.clear();
  }
  const auto scaled = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.ordered", executed.value(), 1U, 0U, 0.5));
  ASSERT_TRUE(scaled.ok()) << scaled.error().message;
  {
    std::lock_guard lock(timeline_mutex);
    ASSERT_GE(timeline.size(), 2U);
    EXPECT_EQ(timeline.front(), "journal:runtime_scale_accepted");
    EXPECT_EQ(timeline[1], "engine:set_runtime_scale");
  }

  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(executed.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kCompleted;
  }));
  const auto completed_revision =
      kernel.get_operation_state(executed.value().operation_id).value().operation_revision;
  journal.fail_next_kind = "control_accepted";
  {
    std::lock_guard lock(timeline_mutex);
    timeline.clear();
  }
  const auto unpersisted_ack = kernel.acknowledge_completed(control_request(
      "request.ack.unpersisted", executed.value(), completed_revision.value()));
  ASSERT_FALSE(unpersisted_ack.ok());
  {
    std::lock_guard lock(timeline_mutex);
    EXPECT_TRUE(timeline.empty() || timeline.front() == "journal:control_accepted");
  }
  EXPECT_EQ(engine.acknowledge_calls.load(), 0);

  engine.next_acknowledge_result =
      AcknowledgeCompletedResultDTO{false, ErrorDTO{"acknowledge_failed",
                                                    "controller rejected acknowledgement",
                                                    {{"reason", "controller_ack_rejected"}}}};
  {
    std::lock_guard lock(timeline_mutex);
    timeline.clear();
  }
  const auto rejected_ack = kernel.acknowledge_completed(control_request(
      "request.ack.rejected", executed.value(), completed_revision.value()));
  ASSERT_TRUE(rejected_ack.ok()) << rejected_ack.error().message;
  ASSERT_TRUE(wait_until([&] {
    std::lock_guard lock(timeline_mutex);
    return timeline.size() >= 2U;
  }));
  EXPECT_EQ(engine.acknowledge_calls.load(), 1);
  {
    std::lock_guard lock(timeline_mutex);
    ASSERT_GE(timeline.size(), 2U);
    EXPECT_EQ(timeline.front(), "journal:control_accepted");
    EXPECT_EQ(timeline[1], "engine:acknowledge_completed");
  }
  std::optional<OperationStateDTO> intervention;
  ASSERT_TRUE(wait_until([&] {
    auto candidate = kernel.get_operation_state(executed.value().operation_id);
    if (!candidate.ok() || candidate.value().state != OperationStateKind::kInterventionRequired) {
      return false;
    }
    intervention = std::move(candidate.value());
    return true;
  }));
  ASSERT_TRUE(intervention.has_value());
  EXPECT_EQ(intervention->state, OperationStateKind::kInterventionRequired);

  FakeParticipantEngine stop_engine;
  FakeOperationJournal stop_journal;
  ResourceAddressedExecutionKernel stop_kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          stop_engine)},
      clock, scheduler, stop_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto stop_execution = stop_kernel.execute(motion_operation("request.motion.stop-order"));
  ASSERT_TRUE(stop_execution.ok()) << stop_execution.error().message;
  stop_engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  const auto stopped =
      stop_kernel.stop(stop_request("request.stop.ordered", stop_execution.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    return stop_journal.stop_acceptance_calls.load() == 1 && stop_engine.stop_calls.load() == 1;
  }));
  EXPECT_EQ(stop_engine.translate_stop_stage_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReplacementRecoveryPublishesHistoryAndDoesNotResumeOrStopOrphanWork) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto& value = binding.value();
  const auto& control = *value.control_path;
  const OperationId old_operation("operation:old-owner:1");
  journal.recovery = OperationJournalRecoveryDTO{
      {OperationStateDTO{
          old_operation,
          OperationRevision(3U),
          ProgramBindingId("program-binding"),
          ProgramRunId("run.old"),
          OperationCoordinationPolicy::kIndependent,
          OperationStateKind::kInterventionRequired,
          {OperationParticipantStateDTO{
              OperationParticipantId("operation:old-owner:1:participant:1"),
              ControllerGoalId("operation:old-owner:1:controller-goal:1"),
              ControllerGoalGeneration(1U),
              value.controllable_component_id,
              value.command_target_id,
              value.capability_id,
              control.control_domain_id,
              control.control_domain_revision,
              control.control_binding_id,
              control.control_binding_revision,
              value.capability_provider_path
                  ? std::optional(value.capability_provider_path->capability_provider_binding_id)
                  : std::nullopt,
              value.capability_provider_path
                  ? std::optional(
                        value.capability_provider_path->capability_provider_binding_revision)
                  : std::nullopt,
              OperationParticipantPayloadKind::kMotion,
              OperationStateKind::kInterventionRequired,
              0.4,
              {ResourceId("robot.joint_1")},
              ErrorDTO{"execution_host_replaced",
                       "old host was replaced",
                       {{"reason", "unreconciled_external_or_orphan_goal"}}}}},
          50,
          std::nullopt,
          std::nullopt,
          RuntimeScaleGeneration(0U)}},
      {},
      RequestOwnerEpoch("kernel.epoch.old"),
      {ResourceId("robot.joint_1")},
      AdmissionGeneration(0U),
      CellAdmissionClaimGeneration(0U),
      {},
      {},
      {}};

  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 1U});
  const auto historical = kernel.get_operation_state(old_operation);
  ASSERT_TRUE(historical.ok()) << historical.error().message;
  EXPECT_EQ(historical.value().state, OperationStateKind::kInterventionRequired);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_EQ(slots.front().state, ResourceSlotState::kUnreconciledExternalOrOrphanGoal);
  EXPECT_FALSE(slots.front().owner.has_value());
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.stop_calls, 0);

  auto intent = motion_operation("request.motion.after-replacement");
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.new");
  const auto blocked = kernel.execute(intent);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "operation_capacity_full");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.stop_calls, 0);

  const auto snapshot = kernel.resource_table_snapshot();
  auto facts =
      orphan_reconciliation_facts(RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"),
                                  snapshot.operation_resource_generation, {binding},
                                  {ResourceId("robot.joint_1")}, clock.now());

  auto mismatched = facts;
  mismatched.resources.front().provider_runtime_epoch = RuntimeEpoch("runtime.epoch.foreign");
  const auto rejected_mismatch = kernel.reconcile_orphaned_resources(mismatched);
  ASSERT_FALSE(rejected_mismatch.ok());
  EXPECT_EQ(rejected_mismatch.error().details.at("reason"),
            "orphan_resource_reconciliation_binding_mismatch");
  EXPECT_EQ(kernel.resource_table_snapshot(), snapshot);

  clock.advance(101);
  const auto rejected_stale = kernel.reconcile_orphaned_resources(facts);
  ASSERT_FALSE(rejected_stale.ok());
  EXPECT_EQ(rejected_stale.error().details.at("reason"),
            "orphan_resource_reconciliation_scope_or_freshness_changed");
  EXPECT_EQ(kernel.resource_table_snapshot(), snapshot);

  facts = orphan_reconciliation_facts(RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"),
                                      snapshot.operation_resource_generation, {binding},
                                      {ResourceId("robot.joint_1")}, clock.now());
  const auto reconciled = kernel.reconcile_orphaned_resources(facts);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_TRUE(reconciled.value().remaining_unreconciled_resources.empty());
  auto admitted_intent = intent;
  admitted_intent.request_context.request_id = RequestId("request.motion.after-reconciliation");
  const auto admitted = kernel.execute(admitted_intent);
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(engine.execute_calls, 1);
  ASSERT_EQ(journal.protected_operation_inventories.size(), 1U);
  EXPECT_EQ(journal.protected_operation_inventories.front(),
            std::vector<OperationId>{admitted.value().operation_id});
}

TEST(ResourceAddressedExecutionKernelTest,
     ReplacementRecoveryReopensOnlyAReconciledResourceSubsetAndSkipsCleanClose) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine motion_engine;
  FakeParticipantEngine gripper_engine;
  FakeOperationJournal journal;
  const ProgramCommandSchemaRefDTO command_schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto motion =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto gripper = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                    CoordinateKind::kLinear, command_schema);
  journal.recovery =
      OperationJournalRecoveryDTO{{},
                                  {},
                                  RequestOwnerEpoch("kernel.epoch.old"),
                                  {ResourceId("robot.finger"), ResourceId("robot.joint_1")},
                                  AdmissionGeneration(0U),
                                  CellAdmissionClaimGeneration(0U),
                                  {},
                                  {},
                                  {}};

  {
    ResourceAddressedExecutionKernel kernel(
        {registration(motion, motion_engine), registration(gripper, gripper_engine)}, clock,
        scheduler, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
    const auto snapshot = kernel.resource_table_snapshot();
    auto facts =
        orphan_reconciliation_facts(RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"),
                                    snapshot.operation_resource_generation, {motion},
                                    {ResourceId("robot.joint_1")}, clock.now());
    const auto reconciled = kernel.reconcile_orphaned_resources(facts);
    ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
    ASSERT_EQ(reconciled.value().remaining_unreconciled_resources,
              std::vector<ResourceId>({ResourceId("robot.finger")}));

    const auto motion_accepted =
        kernel.execute(motion_operation("request.motion.reconciled-subset"));
    ASSERT_TRUE(motion_accepted.ok()) << motion_accepted.error().message;
    EXPECT_EQ(motion_engine.execute_calls, 1);

    const auto gripper_rejected =
        kernel.execute(command_operation("request.command.orphan-subset"));
    ASSERT_FALSE(gripper_rejected.ok());
    EXPECT_EQ(gripper_rejected.error().details.at("reason"), "resource_scope_busy");
    EXPECT_EQ(gripper_engine.prepare_command_calls, 0);
  }

  EXPECT_EQ(std::count(journal.calls.begin(), journal.calls.end(), "owner_closed"), 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrphanRecoveryAcceptsEveryRoutedBindingOfOnePhysicalResourceOwner) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto source =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto sibling = same_owner_control_binding_sibling(source);
  const auto source_registration = registration(source, engine);
  const auto configuration = route_configuration_for(source_registration, sibling);
  journal.recovery = OperationJournalRecoveryDTO{{},
                                                 {},
                                                 RequestOwnerEpoch("kernel.epoch.old"),
                                                 {ResourceId("robot.joint_1")},
                                                 AdmissionGeneration(0U),
                                                 CellAdmissionClaimGeneration(0U),
                                                 {},
                                                 {},
                                                 {}};

  ResourceAddressedExecutionKernel kernel(
      {unqualified_registration(source, engine), unqualified_registration(sibling, engine)}, clock,
      scheduler, journal, {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 16U});
  const auto adopted = kernel.adopt_runtime_route_configuration_predecessor(
      configuration,
      {unqualified_registration(source, engine), unqualified_registration(sibling, engine)});
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  const auto before = kernel.resource_table_snapshot();
  const auto facts = orphan_reconciliation_facts(
      RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"),
      before.operation_resource_generation, {source}, {ResourceId("robot.joint_1")}, clock.now());

  const auto reconciled = kernel.reconcile_orphaned_resources(facts);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_TRUE(reconciled.value().remaining_unreconciled_resources.empty());
  EXPECT_EQ(reconciled.value().operation_resource_generation,
            before.operation_resource_generation + 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrphanRecoveryReconcilesBoundedScaleRoutedSameOwnerSiblings) {
  constexpr std::size_t kBindingCount = 2048U;
  constexpr std::size_t kSharedResourceCount = 32U;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto source_value =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular).value();
  std::vector<ResourceId> shared_resources{source_value.resources.front().resource_id};
  shared_resources.reserve(kSharedResourceCount);
  for (std::size_t index = 1U; index < kSharedResourceCount; ++index) {
    const auto suffix = std::to_string(index);
    const auto source_joint_name = "joint_scale_" + suffix;
    const auto compiled_joint_name = "mf2_joint_scale_" + suffix;
    source_value.resources.push_back({ResourceRoleId("arm_joints"),
                                      ResourceId("robot.joint.scale." + suffix), source_joint_name,
                                      compiled_joint_name, CoordinateKind::kAngular});
    source_value.model_groups.front().expanded_compiled_joint_names.push_back(compiled_joint_name);
    shared_resources.emplace_back("robot.joint.scale." + suffix);
  }
  std::sort(
      source_value.resources.begin(), source_value.resources.end(),
      [](const auto& left, const auto& right) { return left.resource_id < right.resource_id; });
  std::sort(shared_resources.begin(), shared_resources.end());
  auto sealed_source = verify_runtime_participant_binding(std::move(source_value));
  ASSERT_TRUE(sealed_source.ok()) << sealed_source.error().message;
  auto source = std::move(sealed_source.value());

  std::vector<RuntimeParticipantBinding> bindings;
  bindings.reserve(kBindingCount);
  bindings.push_back(source);
  for (std::size_t index = 1U; index < kBindingCount; ++index) {
    bindings.push_back(indexed_same_owner_control_binding_sibling(source, index));
  }

  std::vector<RuntimeParticipantExecutionRegistration> registrations;
  registrations.reserve(bindings.size());
  for (const auto& binding : bindings) {
    registrations.push_back(unqualified_registration(binding, engine));
  }

  auto configuration = route_configuration_for(registration(source, engine));
  configuration.binding_routes.clear();
  configuration.binding_routes.reserve(bindings.size());
  for (const auto& binding : bindings) {
    const auto& path = *binding.value().control_path;
    configuration.binding_routes.push_back(
        {path.control_binding_id, path.control_binding_revision, path.control_route_id});
  }
  std::sort(configuration.binding_routes.begin(), configuration.binding_routes.end(),
            CanonicalActiveControlConfigurationBindingRouteLess);
  ASSERT_TRUE(configuration.locally_valid());

  journal.recovery = OperationJournalRecoveryDTO{{},
                                                 {},
                                                 RequestOwnerEpoch("kernel.epoch.old"),
                                                 shared_resources,
                                                 AdmissionGeneration(0U),
                                                 CellAdmissionClaimGeneration(0U),
                                                 {},
                                                 {},
                                                 {}};
  ResourceAddressedExecutionKernel kernel(
      registrations, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 16U});
  const auto adopted =
      kernel.adopt_runtime_route_configuration_predecessor(configuration, registrations);
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  const auto before = kernel.resource_table_snapshot();
  const auto facts = orphan_reconciliation_facts(
      RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"),
      before.operation_resource_generation, {source}, shared_resources, clock.now());

  const auto reconciled = kernel.reconcile_orphaned_resources(facts);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_TRUE(reconciled.value().remaining_unreconciled_resources.empty());
  EXPECT_EQ(reconciled.value().reconciled_resources.size(), kSharedResourceCount);
  EXPECT_EQ(reconciled.value().operation_resource_generation,
            before.operation_resource_generation + 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     OrphanRecoveryRejectsForeignSiblingEpochRouteAndMissingFactSourceAtomically) {
  const auto exercise_rejection = [](RuntimeParticipantBinding sibling, bool install_wrong_route,
                                     bool remove_fact_source) {
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    const auto source =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    const auto source_registration = registration(source, engine);
    const auto configuration = route_configuration_for(source_registration, sibling);
    journal.recovery = OperationJournalRecoveryDTO{{},
                                                   {},
                                                   RequestOwnerEpoch("kernel.epoch.old"),
                                                   {ResourceId("robot.joint_1")},
                                                   AdmissionGeneration(0U),
                                                   CellAdmissionClaimGeneration(0U),
                                                   {},
                                                   {},
                                                   {}};
    ResourceAddressedExecutionKernel kernel(
        {unqualified_registration(source, engine), unqualified_registration(sibling, engine)},
        clock, scheduler, journal,
        {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 16U});
    const auto adopted = kernel.adopt_runtime_route_configuration_predecessor(
        configuration,
        {unqualified_registration(source, engine), unqualified_registration(sibling, engine)});
    ASSERT_TRUE(adopted.ok()) << adopted.error().message;

    if (install_wrong_route) {
      auto wrong = sibling.value();
      wrong.control_path->control_route_id = ControlRouteId("route.native.wrong");
      if (wrong.control_path->runtime_scale_qualification.has_value()) {
        wrong.control_path->runtime_scale_qualification->control_route_id =
            wrong.control_path->control_route_id;
      }
      auto sealed = verify_runtime_participant_binding(std::move(wrong));
      ASSERT_TRUE(sealed.ok()) << sealed.error().message;
      const auto installed = kernel.reconfigure_runtime_participants(
          {unqualified_registration(source, engine),
           unqualified_registration(sealed.value(), engine)});
      ASSERT_TRUE(installed.ok()) << installed.error().message;
    }

    const auto before = kernel.resource_table_snapshot();
    auto facts = orphan_reconciliation_facts(
        RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"),
        before.operation_resource_generation, {source}, {ResourceId("robot.joint_1")}, clock.now());
    if (remove_fact_source) {
      facts.resources.front().control_binding_id = ControlBindingId("binding.arm.not-registered");
    }
    const auto rejected = kernel.reconcile_orphaned_resources(facts);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(kernel.resource_table_snapshot(), before);
  };

  const auto source =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  exercise_rejection(same_owner_control_binding_sibling(source), false, true);
  exercise_rejection(same_owner_control_binding_sibling(source), true, false);

  auto foreign_value = same_owner_control_binding_sibling(source).value();
  foreign_value.controllable_component_id = ControllableComponentId("robot.foreign");
  auto foreign = verify_runtime_participant_binding(std::move(foreign_value));
  ASSERT_TRUE(foreign.ok()) << foreign.error().message;
  exercise_rejection(std::move(foreign.value()), false, false);

  auto stale_value = same_owner_control_binding_sibling(source).value();
  stale_value.control_path->provider_runtime_epoch = RuntimeEpoch("runtime-epoch.stale");
  for (auto& path : stale_value.supporting_capability_provider_paths) {
    path.provider_runtime_epoch = RuntimeEpoch("runtime-epoch.stale");
  }
  for (auto& epoch : stale_value.runtime_unit_epochs) {
    epoch.runtime_epoch = RuntimeEpoch("runtime-epoch.stale");
  }
  auto stale = verify_runtime_participant_binding(std::move(stale_value));
  ASSERT_TRUE(stale.ok()) << stale.error().message;
  exercise_rejection(std::move(stale.value()), false, false);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReconfigurationPreservesActiveEngineThenSelectsCandidateProduct) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine;
  FakeParticipantEngine candidate_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto candidate_binding = select_candidate_product(predecessor_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto predecessor = kernel.execute(motion_operation("request.motion.predecessor"));
  ASSERT_TRUE(predecessor.ok()) << predecessor.error().message;
  ASSERT_TRUE(kernel.participant_engine_in_use(&predecessor_engine));

  const auto candidate_registration = registration(candidate_binding, candidate_engine);
  const auto reconfigured = kernel.publish_runtime_route_configuration(
      route_configuration_for(candidate_registration), {candidate_registration});
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;
  EXPECT_TRUE(reconfigured.value().operation_admission_open);
  ASSERT_EQ(reconfigured.value().readiness_rows.size(), 1U);
  EXPECT_TRUE(kernel.participant_engine_in_use(&predecessor_engine));
  EXPECT_TRUE(kernel.participant_engine_in_use(&candidate_engine));
  EXPECT_EQ(predecessor_engine.subscription_count(), 1U);
  EXPECT_EQ(candidate_engine.subscription_count(), 1U);
  const auto occupied = kernel.resource_snapshot();
  ASSERT_EQ(occupied.size(), 1U);
  EXPECT_EQ(occupied.front().state, ResourceSlotState::kDispatched);
  ASSERT_TRUE(occupied.front().owner.has_value());
  EXPECT_EQ(occupied.front().owner->operation_id, predecessor.value().operation_id);

  predecessor_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.ack.predecessor", predecessor.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] { return !kernel.participant_engine_in_use(&predecessor_engine); }));
  // Removing the resident operation precedes the cleanup worker returning.
  // Retry only the exact no-effect refusal while that worker still owns its lane.
  std::optional<Result<void>> released;
  ASSERT_TRUE(wait_until([&] {
    released = kernel.release_retired_engine(&predecessor_engine);
    return released->ok() ||
           released->error().details.at("reason") != "operation_control_reconfiguration_busy";
  }));
  ASSERT_TRUE(released->ok()) << released->error().message;
  EXPECT_EQ(predecessor_engine.subscription_count(), 0U);
  EXPECT_EQ(candidate_engine.subscription_count(), 1U);

  enable_component_commands(kernel, candidate_registration);
  const auto candidate = kernel.execute(candidate_motion_operation());
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  EXPECT_EQ(predecessor_engine.execute_calls, 1);
  EXPECT_EQ(candidate_engine.execute_calls, 1);
  ASSERT_TRUE(candidate_engine.last_execute.has_value());
  EXPECT_EQ(candidate_engine.last_execute->binding.retained_bundle_id,
            RetainedBundleId(test_digest('e')));
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleControlFreezesOnlyTheUnreconciledRouteAndRetainsItsSetting) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeParticipantEngine peer_engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto peer = coordinated_occurrence(binding, "2");
  const auto first_registration = registration(binding, engine);
  const auto peer_registration = registration(peer, peer_engine);
  ResourceAddressedExecutionKernel kernel(
      {first_registration, peer_registration}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto saved_request = runtime_scale_control_request("scale.saved", 0U, 0.4);
  ASSERT_TRUE(kernel.set_runtime_scale_control(saved_request).ok());
  const auto admitted = kernel.execute(motion_operation("motion.before-unknown-route"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  const auto calls = engine.runtime_scale_calls.load();

  auto unknown = route_configuration_for(first_registration);
  unknown.state = ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  ASSERT_TRUE(kernel.publish_runtime_route_configuration(std::move(unknown)).ok());
  const auto closed = kernel.get_runtime_scale_control(saved_request.selector);
  ASSERT_TRUE(closed.ok());
  EXPECT_TRUE(closed.value().supported);
  EXPECT_TRUE(closed.value().frozen);
  EXPECT_EQ(closed.value().active_operation_id, admitted.value().operation_id);
  EXPECT_EQ(closed.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(closed.value().requested_scale, 0.4);
  ASSERT_TRUE(closed.value().reason.has_value());
  EXPECT_EQ(closed.value().reason->details.at("reason"), "runtime_route_configuration_not_ready");
  const auto refused =
      kernel.set_runtime_scale_control(runtime_scale_control_request("scale.closed", 1U, 0.2));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "runtime_route_configuration_not_ready");
  EXPECT_EQ(engine.runtime_scale_calls, calls);
  const auto replay = kernel.set_runtime_scale_control(saved_request);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.runtime_scale_calls, calls);

  auto peer_request = runtime_scale_control_request("scale.peer", 0U, 0.7);
  peer_request.selector = runtime_scale_control_selector("robot_2", "arm_2");
  const auto peer_status = kernel.get_runtime_scale_control(peer_request.selector);
  ASSERT_TRUE(peer_status.ok());
  EXPECT_FALSE(peer_status.value().frozen);
  ASSERT_TRUE(kernel.set_runtime_scale_control(peer_request).ok());

  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  ASSERT_TRUE(
      kernel
          .acknowledge_completed(control_request("motion.ack-before-restore", admitted.value(), 1U))
          .ok());
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return std::all_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot.state == ResourceSlotState::kFree; });
  }));
  ASSERT_TRUE(kernel
                  .publish_runtime_route_configuration(route_configuration_for(first_registration),
                                                       {first_registration, peer_registration})
                  .ok());
  const auto restored = kernel.get_runtime_scale_control(saved_request.selector);
  ASSERT_TRUE(restored.ok());
  EXPECT_FALSE(restored.value().frozen);
  EXPECT_FALSE(restored.value().reason.has_value());
  EXPECT_EQ(restored.value().control_generation, RuntimeScaleControlGeneration(1U));
  EXPECT_DOUBLE_EQ(restored.value().requested_scale, 0.4);
  ASSERT_TRUE(
      kernel.set_runtime_scale_control(runtime_scale_control_request("scale.restored", 1U, 0.6))
          .ok());
  EXPECT_DOUBLE_EQ(kernel.get_runtime_scale_control(peer_request.selector).value().requested_scale,
                   0.7);
}

TEST(ResourceAddressedExecutionKernelTest,
     GenericReconfigurationRequiresExactRoutePublicationBeforeAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine;
  FakeParticipantEngine candidate_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto candidate_binding = select_candidate_product(predecessor_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto candidate_registration = registration(candidate_binding, candidate_engine);

  const auto reconfigured = kernel.reconfigure_runtime_participants({candidate_registration});
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;
  const auto blocked =
      kernel.execute(candidate_motion_operation("request.motion.candidate.blocked"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "runtime_route_publication_required");

  auto scale_request = runtime_scale_control_request("scale.before-publication", 0U, 0.4);
  scale_request.selector.program_binding_id = candidate_binding.value().program_binding_id;
  const auto scale_blocked = kernel.get_runtime_scale_control(scale_request.selector);
  ASSERT_TRUE(scale_blocked.ok());
  EXPECT_TRUE(scale_blocked.value().supported);
  EXPECT_TRUE(scale_blocked.value().frozen);
  ASSERT_TRUE(scale_blocked.value().reason.has_value());
  EXPECT_EQ(scale_blocked.value().reason->details.at("reason"),
            "runtime_route_configuration_not_ready");
  const auto scale_refused = kernel.set_runtime_scale_control(scale_request);
  ASSERT_FALSE(scale_refused.ok());
  EXPECT_EQ(scale_refused.error().details.at("reason"), "runtime_route_configuration_not_ready");
  EXPECT_EQ(candidate_engine.runtime_scale_calls, 0);

  const auto published = kernel.publish_runtime_route_configuration(
      route_configuration_for(candidate_registration), {candidate_registration});
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto readback = kernel.runtime_route_configuration_readback(
      published.value().active_configuration.control_domain_id,
      published.value().active_configuration.active_control_configuration_id,
      published.value().active_configuration.active_control_configuration_generation);
  ASSERT_TRUE(readback.ok()) << readback.error().message;
  EXPECT_EQ(readback.value(), published.value());
  enable_component_commands(kernel, candidate_registration);
  const auto scale_open = kernel.get_runtime_scale_control(scale_request.selector);
  ASSERT_TRUE(scale_open.ok());
  EXPECT_FALSE(scale_open.value().frozen);
  ASSERT_TRUE(kernel.set_runtime_scale_control(scale_request).ok());
  const auto admitted =
      kernel.execute(candidate_motion_operation("request.motion.candidate.admitted"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;

  auto unreconciled = route_configuration_for(candidate_registration);
  unreconciled.state = ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  const auto closed = kernel.publish_runtime_route_configuration(std::move(unreconciled));
  ASSERT_TRUE(closed.ok()) << closed.error().message;
  EXPECT_FALSE(closed.value().operation_admission_open);
  candidate_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.ack.route-publication", admitted.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto slots = kernel.resource_snapshot();
    return !slots.empty() && slots.front().state == ResourceSlotState::kFree;
  }));
  const auto rejected =
      kernel.execute(candidate_motion_operation("request.motion.candidate.rejected"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "runtime_route_configuration_not_ready");
}

TEST(ResourceAddressedExecutionKernelTest, RouteReadinessNeverGrantsComponentCommandEligibility) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel({registered}, clock, scheduler, journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto published =
      kernel.publish_runtime_route_configuration(route_configuration_for(registered), {registered});
  ASSERT_TRUE(published.ok()) << published.error().message;
  // The route and its Stop readiness are coherent, yet nothing enabled the
  // component: route readiness alone is not command permission.
  EXPECT_TRUE(published.value().operation_admission_open);
  const auto blocked = kernel.execute(motion_operation("request.motion.route-only"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "component_command_not_eligible");
  EXPECT_EQ(engine.execute_calls, 0);

  enable_component_commands(kernel, registered);
  const auto admitted = kernel.execute(motion_operation("request.motion.enabled"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest, RouteRepublicationNeitherEnablesNorClosesAComponent) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel({registered}, clock, scheduler, journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto configuration = route_configuration_for(registered);
  ASSERT_TRUE(kernel.publish_runtime_route_configuration(configuration, {registered}).ok());
  const auto component = binding.value().controllable_component_id;
  const auto generation = registered.stop_readiness->value().component_lifecycle_generation;

  // A Disabled component keeps its route: republishing it stays route-ready
  // and still refuses commands.
  ASSERT_TRUE(publish_under_restart_exclusion(kernel, component, generation, false).ok());
  const auto republished = kernel.publish_runtime_route_configuration(configuration, {registered});
  ASSERT_TRUE(republished.ok()) << republished.error().message;
  EXPECT_TRUE(republished.value().operation_admission_open);
  const auto disabled = kernel.execute(motion_operation("request.motion.disabled-republished"));
  ASSERT_FALSE(disabled.ok());
  EXPECT_EQ(disabled.error().details.at("reason"), "component_command_not_eligible");

  // An enabled component is not closed by route republication either.
  ASSERT_TRUE(publish_under_restart_exclusion(kernel, component, generation, true).ok());
  ASSERT_TRUE(kernel.publish_runtime_route_configuration(configuration, {registered}).ok());
  const auto enabled = kernel.execute(motion_operation("request.motion.enabled-republished"));
  ASSERT_TRUE(enabled.ok()) << enabled.error().message;
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentReadinessRefusesAStaleGenerationAndResealsWithoutRouteChurn) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel({registered}, clock, scheduler, journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto published =
      kernel.publish_runtime_route_configuration(route_configuration_for(registered), {registered});
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto component = binding.value().controllable_component_id;

  // A newer generation re-seals the participant with the same engine, route
  // configuration and Stop plan; only the component generation changes.
  const auto advanced =
      publish_under_restart_exclusion(kernel, component, ComponentLifecycleGeneration(3U), true);
  ASSERT_TRUE(advanced.ok()) << advanced.error().message;
  EXPECT_EQ(advanced.value().component_lifecycle_generation, ComponentLifecycleGeneration(3U));
  const auto route = kernel.current_runtime_route_configuration(
      published.value().active_configuration.control_domain_id);
  ASSERT_TRUE(route.ok() && route.value().has_value());
  EXPECT_EQ(route.value()->active_configuration, published.value().active_configuration);
  ASSERT_EQ(route.value()->readiness_rows.size(), 1U);
  EXPECT_EQ(route.value()->readiness_rows.front().component_lifecycle_generation,
            ComponentLifecycleGeneration(3U));
  EXPECT_EQ(route.value()->readiness_rows.front().resolved_stop_base_plan_id,
            published.value().readiness_rows.front().resolved_stop_base_plan_id);
  const auto admitted = kernel.execute(motion_operation("request.motion.resealed"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;

  // Composition that read older facts can never overtake a newer result.
  const auto stale =
      publish_under_restart_exclusion(kernel, component, ComponentLifecycleGeneration(2U), true);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "component_readiness_generation_stale");
  const auto current = kernel.component_command_readiness(component);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().component_lifecycle_generation, ComponentLifecycleGeneration(3U));
}

TEST(ResourceAddressedExecutionKernelTest,
     SharedDomainEligibilityStaysSpecificToEachComponent) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine arm_engine;
  FakeParticipantEngine peer_engine;
  FakeOperationJournal journal;
  const auto arm =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto peer = in_domain_of(
      make_binding("gripper", "gripper.target", "tool.gripper.command",
                   CoordinateKind::kLinear, schema),
      arm, "robot.2");
  const auto arm_registration = registration(arm, arm_engine, false, {arm, peer});
  const auto peer_registration = registration(peer, peer_engine, false, {arm, peer});
  ResourceAddressedExecutionKernel kernel({arm_registration, peer_registration}, clock, scheduler,
                                          journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  const auto published = kernel.publish_runtime_route_configuration(
      route_configuration_for(arm_registration, peer),
      {arm_registration, peer_registration});
  ASSERT_TRUE(published.ok()) << published.error().message;

  // Enabling the peer never enables the arm's component.
  enable_component_commands(kernel, peer_registration);
  const auto blocked = kernel.execute(motion_operation("request.motion.peer-enabled"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "component_command_not_eligible");

  // Closing the peer never blocks an independently enabled arm.
  enable_component_commands(kernel, arm_registration);
  ASSERT_TRUE(publish_under_restart_exclusion(
                  kernel, peer.value().controllable_component_id,
                  peer_registration.stop_readiness->value().component_lifecycle_generation, false)
                  .ok());
  const auto admitted = kernel.execute(motion_operation("request.motion.arm-enabled"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(arm_engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     GripperOnlyComponentInstallsUnderItsApplyFenceAndStaysClosedUntilEligible) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine gripper_engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.gripper-only-apply");

  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  const auto binding = make_binding("gripper", "gripper.target", "tool.gripper.command",
                                    CoordinateKind::kLinear, schema);
  auto gripper_registration = registration(binding, gripper_engine);
  // A gripper-only capability command has no arm motion policy requirement.
  gripper_registration.participant_policy.reset();

  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U, {}, &gate};
  config.empty_runtime_retained_bundle_id = RetainedBundleId(test_digest('4'));
  config.empty_runtime_graph_revision = GraphRevision(test_digest('5'));
  config.prequalified_max_live_stop_participants = 1U;
  ResourceAddressedExecutionKernel kernel({}, clock, scheduler, journal, std::move(config));

  const auto& component = binding.value().controllable_component_id;
  const auto& path = *binding.value().control_path;
  const ConfigurationFenceScopeDTO scope{
      {component},
      {{path.control_domain_id, std::nullopt, path.control_domain_revision}},
      {{path.control_binding_id, std::nullopt, path.control_binding_revision}},
      {binding.value().resources.front().resource_id}};
  ASSERT_TRUE(scope.valid());
  gate.held_scope = scope;
  gate.held_fence_resources = scope.resources;

  const auto installed = kernel.reconfigure_runtime_component_set(
      {gripper_registration}, {}, std::nullopt, binding.value().retained_bundle_id,
      binding.value().graph_revision, *gate.held_fence_id, scope);
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_TRUE(kernel.participant_engine_in_use(&gripper_engine));
  const auto installed_resources = kernel.resource_table_snapshot();
  ASSERT_EQ(installed_resources.slots.size(), 1U);
  EXPECT_EQ(installed_resources.slots.front().resource_id,
            binding.value().resources.front().resource_id);

  const auto route = kernel.publish_runtime_route_configuration(
      route_configuration_for(gripper_registration), {gripper_registration});
  ASSERT_TRUE(route.ok()) << route.error().message;
  const auto initially_ineligible = kernel.execute(command_operation("request.gripper.disabled"));
  ASSERT_FALSE(initially_ineligible.ok());
  EXPECT_EQ(initially_ineligible.error().details.at("reason"),
            "component_command_not_eligible");
  EXPECT_EQ(gripper_engine.prepare_command_calls, 0);

  const auto eligible = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       *gate.held_fence_id, component,
       gripper_registration.stop_readiness->value().component_lifecycle_generation, true});
  ASSERT_TRUE(eligible.ok()) << eligible.error().message;
  EXPECT_TRUE(eligible.value().command_eligible);

  // Even an explicitly eligible component cannot dispatch until the exact
  // Apply fence is released.
  const auto fenced = kernel.execute(command_operation("request.gripper.fenced"));
  ASSERT_FALSE(fenced.ok());
  EXPECT_EQ(fenced.error().details.at("reason"), "configuration_fence_held");
  EXPECT_EQ(gripper_engine.prepare_command_calls, 0);

  gate.reject_reservation = false;
  const auto admitted = kernel.execute(command_operation("request.gripper.enabled"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(gripper_engine.prepare_command_calls, 1);
  EXPECT_EQ(gripper_engine.dispatch_prepared_command_calls, 1);
  EXPECT_EQ(gripper_engine.command_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ApplyReadinessRequiresThatExactParentFenceNotAnyFence) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.parent-apply");
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel(
      {registered}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U, {}, &gate});
  ASSERT_TRUE(kernel
                  .publish_runtime_route_configuration(route_configuration_for(registered),
                                                       {registered})
                  .ok());
  const auto component = binding.value().controllable_component_id;
  const auto generation = registered.stop_readiness->value().component_lifecycle_generation;

  // Every resource is fenced, but by a fence this publication does not own.
  const auto foreign = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.other-apply"), component, generation, true});
  ASSERT_FALSE(foreign.ok());
  EXPECT_EQ(foreign.error().details.at("reason"), "component_readiness_apply_fence_not_held");

  const auto owned = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.parent-apply"), component, generation, true});
  ASSERT_TRUE(owned.ok()) << owned.error().message;
  EXPECT_TRUE(owned.value().command_eligible);

  // Once the parent fence is released it is no longer authority.
  gate.reject_reservation = false;
  const auto released = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.parent-apply"), component, generation, false});
  ASSERT_FALSE(released.ok());
  EXPECT_EQ(released.error().details.at("reason"), "component_readiness_apply_fence_not_held");
}

TEST(ResourceAddressedExecutionKernelTest,
     ApplyReadinessRejectsAnExactFenceThatCoversOnlyPartOfTheComponent) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.parent-apply");
  const auto binding = make_binding(
      "arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
      std::nullopt, true);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel(
      {registered}, clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U, {}, &gate});
  ASSERT_TRUE(kernel.publish_runtime_route_configuration(
                  route_configuration_for(registered), {registered}).ok());
  const auto component = binding.value().controllable_component_id;
  const auto generation =
      registered.stop_readiness->value().component_lifecycle_generation;
  ASSERT_EQ(binding.value().resources.size(), 2U);
  gate.held_fence_resources =
      std::vector<ResourceId>{binding.value().resources.front().resource_id};
  gate.other_fence_resources =
      std::vector<ResourceId>{binding.value().resources.back().resource_id};
  const std::vector<ResourceId> all_resources{
      binding.value().resources.front().resource_id,
      binding.value().resources.back().resource_id};
  // Aggregate fencing of every resource is not authority for this Apply.
  EXPECT_EQ(gate.current_fenced_resources(all_resources), all_resources);

  const auto partial = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.parent-apply"), component, generation, true});
  ASSERT_FALSE(partial.ok());
  EXPECT_EQ(partial.error().details.at("reason"),
            "component_readiness_apply_fence_not_held");
  const auto closed = kernel.component_command_readiness(component);
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed.value().command_eligible);

  gate.held_fence_resources->push_back(binding.value().resources.back().resource_id);
  const auto complete = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.parent-apply"), component, generation, true});
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  EXPECT_TRUE(complete.value().command_eligible);
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentEligibilityChangesOnlyUnderItsExactHeldLifecycleClaim) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto registered = registration(binding, engine);
  ResourceAddressedExecutionKernel kernel({registered}, clock, scheduler, journal,
                                          {RequestOwnerEpoch("kernel.epoch.1"),
                                           CellId("cell.test"), 16U});
  ASSERT_TRUE(kernel
                  .publish_runtime_route_configuration(route_configuration_for(registered),
                                                       {registered})
                  .ok());
  enable_component_commands(kernel, registered);
  const auto component = binding.value().controllable_component_id;
  const auto closed =
      kernel.close_component_command_eligibility(LifecycleTransactionId("lifecycle.unheld"),
                                                 component);
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().details.at("reason"), "component_readiness_claim_not_held");
  const auto refused = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kLifecycleClaim, LifecycleTransactionId("lifecycle.unheld"),
       std::nullopt, component, ComponentLifecycleGeneration(2U), false});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "component_readiness_claim_not_held");
  // Absence of another claim is not permission: composition must prove the
  // restart exclusion or the parent Apply fence.
  const auto unexcluded = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt, std::nullopt, component,
       ComponentLifecycleGeneration(2U), true});
  ASSERT_FALSE(unexcluded.ok());
  EXPECT_EQ(unexcluded.error().details.at("reason"),
            "component_readiness_restart_exclusion_not_held");
  const auto unfenced = kernel.publish_component_command_readiness(
      {ComponentReadinessAuthorityDTO::kParentApplyFence, std::nullopt,
       ConfigurationFenceId("fence.not-held"), component, ComponentLifecycleGeneration(2U), true});
  ASSERT_FALSE(unfenced.ok());
  EXPECT_EQ(unfenced.error().details.at("reason"), "component_readiness_apply_fence_not_held");
  const auto current = kernel.component_command_readiness(component);
  ASSERT_TRUE(current.ok());
  EXPECT_TRUE(current.value().command_eligible);
  EXPECT_EQ(current.value().component_lifecycle_generation, ComponentLifecycleGeneration(1U));
}

TEST(ResourceAddressedExecutionKernelTest,
     UnqualifiedBootstrapRemainsClosedUntilExactRouteReadinessPublication) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {unqualified_registration(binding, engine)}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"),
                                             16U,
                                             {},
                                             nullptr,
                                             binding.value().graph_revision,
                                             std::nullopt});

  const auto blocked = kernel.execute(motion_operation("request.motion.bootstrap-closed"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "runtime_route_publication_required");
  EXPECT_EQ(engine.execute_calls, 0);
  ASSERT_TRUE(binding.value().control_path.has_value());
  const auto bootstrap =
      kernel.current_runtime_route_configuration(binding.value().control_path->control_domain_id);
  ASSERT_TRUE(bootstrap.ok()) << bootstrap.error().message;
  EXPECT_FALSE(bootstrap.value().has_value());

  const auto qualified = registration(binding, engine);
  const auto published =
      kernel.publish_runtime_route_configuration(route_configuration_for(qualified), {qualified});
  ASSERT_TRUE(published.ok()) << published.error().message;
  EXPECT_TRUE(published.value().operation_admission_open);
  enable_component_commands(kernel, qualified);

  const auto admitted = kernel.execute(motion_operation("request.motion.bootstrap-open"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     EmptyRuntimeBootstrapOwnsCellAdmissionBeforeParticipantInstallation) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.empty_runtime_retained_bundle_id = RetainedBundleId(test_digest('6'));
  config.empty_runtime_graph_revision = GraphRevision(test_digest('7'));
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  config.cell_admission_settlement_validator = [](const auto&, const auto&) {
    return Result<void>::Ok();
  };
  ResourceAddressedExecutionKernel kernel({}, clock, scheduler, journal, std::move(config));

  const CellAdmissionReadinessSnapshotDTO ready{
      true,
      ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner"), 1U,
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
  ASSERT_TRUE(kernel.UpdateCellAdmissionReadiness(ready).ok());
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  EXPECT_TRUE(kernel.resource_snapshot().empty());

  const auto successor = select_candidate_product(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular));
  const auto installed = kernel.reconfigure_runtime_participants({registration(successor, engine)});
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_EQ(kernel.resource_snapshot().size(), 1U);

  auto mixed_value = same_owner_control_binding_sibling(successor).value();
  mixed_value.graph_revision = GraphRevision(test_digest('a'));
  auto mixed = verify_runtime_participant_binding(std::move(mixed_value));
  ASSERT_TRUE(mixed.ok()) << mixed.error().message;
  const auto rejected = kernel.reconfigure_runtime_participants(
      {registration(successor, engine), registration(mixed.value(), engine)});
  ASSERT_FALSE(rejected.ok());
  EXPECT_NE(
      rejected.error().message.find("runtime participants do not share one current graph revision"),
      std::string::npos);
  EXPECT_EQ(kernel.resource_snapshot().size(), 1U);
}

TEST(ResourceAddressedExecutionKernelTest,
     EmptyComponentSetApplyKeepsTheCandidateProductIdentity) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.empty-candidate");
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U, {}, &gate};
  config.empty_runtime_retained_bundle_id = RetainedBundleId(test_digest('6'));
  config.empty_runtime_graph_revision = GraphRevision(test_digest('7'));
  ResourceAddressedExecutionKernel kernel({}, clock, scheduler, journal,
                                          std::move(config));

  const ConfigurationFenceScopeDTO scope{};
  ASSERT_TRUE(scope.valid());
  gate.held_scope = scope;
  const RetainedBundleId candidate_bundle(test_digest('8'));
  const GraphRevision candidate_graph(test_digest('9'));
  const auto installed = kernel.reconfigure_runtime_component_set(
      {}, {}, std::nullopt, candidate_bundle, candidate_graph,
      *gate.held_fence_id, scope);
  ASSERT_TRUE(installed.ok()) << installed.error().message;

  auto next = make_binding("arm", "arm", "execution.trajectory.execute",
                           CoordinateKind::kAngular).value();
  next.retained_bundle_id = candidate_bundle;
  next.graph_revision = candidate_graph;
  auto next_binding = verify_runtime_participant_binding(std::move(next));
  ASSERT_TRUE(next_binding.ok()) << next_binding.error().message;
  const auto selected = kernel.reconfigure_runtime_participants(
      {unqualified_registration(next_binding.value(), engine)});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentSetRejectsMixedCandidateProductIdentityWithoutReplacingCurrentRegistrations) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine first_engine, second_engine;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.mixed-candidate-product");
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = coordinated_occurrence(first_binding, "2");
  const auto first = registration(first_binding, first_engine);
  const auto second = registration(second_binding, second_engine);
  ResourceAddressedExecutionKernel kernel(
      {first, second}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});
  const auto current_slots = kernel.resource_table_snapshot();

  const ConfigurationFenceScopeDTO no_controller_change{};
  ASSERT_TRUE(no_controller_change.valid());
  gate.held_scope = no_controller_change;
  const RetainedBundleId candidate_bundle(test_digest('8'));
  const GraphRevision candidate_graph(test_digest('9'));
  const std::vector<ControllableComponentId> retained_components{
      first_binding.value().controllable_component_id,
      second_binding.value().controllable_component_id};
  ASSERT_TRUE(std::is_sorted(retained_components.begin(), retained_components.end()));
  ASSERT_TRUE(gate.exact_fence_is_held(*gate.held_fence_id, no_controller_change));

  auto candidate_first_value = first_binding.value();
  candidate_first_value.retained_bundle_id = candidate_bundle;
  candidate_first_value.graph_revision = candidate_graph;
  auto candidate_first = verify_runtime_participant_binding(std::move(candidate_first_value));
  ASSERT_TRUE(candidate_first.ok()) << candidate_first.error().message;

  auto candidate_second_graph_value = second_binding.value();
  candidate_second_graph_value.retained_bundle_id = candidate_bundle;
  candidate_second_graph_value.graph_revision = GraphRevision(test_digest('a'));
  auto candidate_second_graph =
      verify_runtime_participant_binding(std::move(candidate_second_graph_value));
  ASSERT_TRUE(candidate_second_graph.ok()) << candidate_second_graph.error().message;
  const auto graph_mixed = kernel.reconfigure_runtime_component_set(
      {registration(candidate_first.value(), first_engine),
       registration(candidate_second_graph.value(), second_engine)},
      {}, std::nullopt, candidate_bundle, candidate_graph, *gate.held_fence_id,
      no_controller_change, retained_components);
  ASSERT_FALSE(graph_mixed.ok());
  EXPECT_EQ(graph_mixed.error().details.at("reason"),
            "runtime_component_set_product_mismatch");
  EXPECT_EQ(kernel.resource_table_snapshot(), current_slots);

  auto candidate_second_bundle_value = second_binding.value();
  candidate_second_bundle_value.retained_bundle_id = RetainedBundleId(test_digest('b'));
  candidate_second_bundle_value.graph_revision = candidate_graph;
  auto candidate_second_bundle =
      verify_runtime_participant_binding(std::move(candidate_second_bundle_value));
  ASSERT_TRUE(candidate_second_bundle.ok()) << candidate_second_bundle.error().message;
  const auto bundle_mixed = kernel.reconfigure_runtime_component_set(
      {registration(candidate_first.value(), first_engine),
       registration(candidate_second_bundle.value(), second_engine)},
      {}, std::nullopt, candidate_bundle, candidate_graph, *gate.held_fence_id,
      no_controller_change, retained_components);
  ASSERT_FALSE(bundle_mixed.ok());
  EXPECT_EQ(bundle_mixed.error().details.at("reason"),
            "runtime_component_set_product_mismatch");
  EXPECT_EQ(kernel.resource_table_snapshot(), current_slots);
  EXPECT_TRUE(kernel.participant_engine_in_use(&first_engine));
  EXPECT_TRUE(kernel.participant_engine_in_use(&second_engine));
  EXPECT_EQ(first_engine.subscription_count(), 1U);
  EXPECT_EQ(second_engine.subscription_count(), 1U);

  gate.reject_reservation = false;
  const auto dispatched = kernel.execute(
      coordinated_motion_operation("request.motion.mixed-candidate-original"));
  ASSERT_TRUE(dispatched.ok()) << dispatched.error().message;
  ASSERT_EQ(dispatched.value().participant_assignments.size(), 2U);
  EXPECT_EQ(first_engine.execute_calls, 1);
  EXPECT_EQ(second_engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     PhysicalProductReconfigurationInstallsStopDeadlineBeforeRouteReadiness) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeOperationJournal journal;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.empty_runtime_retained_bundle_id = RetainedBundleId(test_digest('6'));
  config.empty_runtime_graph_revision = GraphRevision(test_digest('7'));
  config.prequalified_max_live_stop_participants = 1U;
  ResourceAddressedExecutionKernel kernel({}, clock, scheduler, journal, std::move(config));

  const auto binding = select_candidate_product(
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular));
  const StopDeadlineBudgetDTO product_budget{3'000'000'000, 100'000'000, {}};
  int commit_calls = 0;
  const auto installed = kernel.reconfigure_runtime_product(
      {unqualified_registration(binding, engine)}, {}, product_budget, [&]() {
        ++commit_calls;
        return Result<void>::Ok();
      });
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_EQ(commit_calls, 1);

  const auto stopped = kernel.full_stop(
      FullStopIntentDTO{{RequestId("request.product-before-readiness.stop"),
                         RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
                        StopGeneration(0U),
                        {}});
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(stopped.value().fixed_outer_duration_ns, product_budget.outer_duration_ns);
}

TEST(ResourceAddressedExecutionKernelTest,
     OwnerReplacementAdoptsDurableRoutePredecessorWithoutOpeningAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto predecessor_configuration =
      route_configuration_for(registration(predecessor_binding, engine));

  auto current_value = predecessor_binding.value();
  const RuntimeEpoch current_epoch("runtime-epoch.2");
  current_value.control_path->provider_runtime_epoch = current_epoch;
  for (auto& provider : current_value.supporting_capability_provider_paths) {
    provider.provider_runtime_epoch = current_epoch;
  }
  for (auto& runtime : current_value.runtime_unit_epochs) {
    runtime.runtime_epoch = current_epoch;
  }
  auto current_binding = verify_runtime_participant_binding(std::move(current_value));
  ASSERT_TRUE(current_binding.ok()) << current_binding.error().message;

  ResourceAddressedExecutionKernel kernel(
      {unqualified_registration(current_binding.value(), engine)}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.2"),
                                             CellId("cell.test"), 16U});
  const auto adopted = kernel.adopt_runtime_route_configuration_predecessor(
      predecessor_configuration, {unqualified_registration(current_binding.value(), engine)});
  ASSERT_TRUE(adopted.ok()) << adopted.error().message;
  EXPECT_EQ(adopted.value().active_configuration, predecessor_configuration);
  EXPECT_TRUE(adopted.value().readiness_rows.empty());
  EXPECT_FALSE(adopted.value().operation_admission_open);

  const auto readback =
      kernel.current_runtime_route_configuration(predecessor_configuration.control_domain_id);
  ASSERT_TRUE(readback.ok()) << readback.error().message;
  ASSERT_TRUE(readback.value().has_value());
  EXPECT_EQ(*readback.value(), adopted.value());

  auto intent = motion_operation("request.motion.predecessor-closed");
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.2");
  const auto blocked = kernel.execute(intent);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "runtime_route_publication_required");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     RouteRecoveryClaimRequiresAdoptedReplacementAndSettledResources) {
  for (int scenario = 0; scenario < 4; ++scenario) {
    SCOPED_TRACE(scenario);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    FakeOperationJournal journal;
    const auto binding =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    auto predecessor = route_configuration_for(registration(binding, engine));
    if (scenario != 1) predecessor.source_runtime_epoch = RuntimeEpoch("predecessor.epoch");
    if (scenario == 2)
      journal.recovery.unreconciled_resources = {binding.value().resources.front().resource_id};
    ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.2"),
                                                  CellId("cell.test"), 16U};
    // Even a composed generic intervention validator cannot mint CCLC recovery
    // through ordinary ingress.
    config.cell_admission_intervention_validator = [](const auto&, const auto&) {
      return Result<void>::Ok();
    };
    config.cell_admission_settlement_validator = [](const auto&, const auto&) {
      return Result<void>::Ok();
    };
    ResourceAddressedExecutionKernel kernel({unqualified_registration(binding, engine)}, clock,
                                            scheduler, journal, std::move(config));
    if (scenario != 3) {
      ASSERT_TRUE(kernel
                      .adopt_runtime_route_configuration_predecessor(
                          predecessor, {unqualified_registration(binding, engine)})
                      .ok());
    }
    CellAdmissionClaimIntentDTO intent{
        CellAdmissionClaimKindDTO::kInterventionOrRecovery,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        RequestOwnerEpoch("cclc.owner"),
        "route-selection.recovery.1",
        RequestId("recovery.1"),
        ContentDigest(test_digest('a')),
        kernel.GetAuthoringAdmission().admission_generation,
        {{},
         {{predecessor.control_domain_id, predecessor.active_control_configuration_id,
           predecessor.active_control_configuration_generation}}},
        {},
        {},
        {}};
    intent.intervention_evidence =
        CellAdmissionInterventionEvidenceDTO{intent.owner_role,
                                             intent.owner_epoch,
                                             intent.owner_transaction_id,
                                             intent.request_payload_digest,
                                             {}};
    ASSERT_TRUE(ValidateCellAdmissionClaimIntent(intent).ok());
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
    EXPECT_FALSE(kernel.AcquireCellAdmissionClaim(intent).ok());
    auto foreign = predecessor;
    foreign.source_runtime_unit_id = RuntimeUnitId("foreign.runtime");
    EXPECT_FALSE(kernel.AcquireRouteRecoveryCellAdmissionClaim(intent, foreign).ok());
    auto foreign_scope = intent;
    foreign_scope.scope.configurations.front().active_configuration_id =
        ActiveControlConfigurationId("foreign");
    const auto refused_scope =
        kernel.AcquireRouteRecoveryCellAdmissionClaim(foreign_scope, predecessor);
    ASSERT_FALSE(refused_scope.ok());
    EXPECT_EQ(refused_scope.error().details.at("intervention_offer_reason"),
              "route_recovery_predecessor_not_current");
    const auto acquired = kernel.AcquireRouteRecoveryCellAdmissionClaim(intent, predecessor);
    EXPECT_EQ(acquired.ok(), scenario == 0);
    if (scenario == 0) {
      ASSERT_TRUE(acquired.ok()) << acquired.error().message;
      EXPECT_EQ(kernel.AcquireRouteRecoveryCellAdmissionClaim(intent, predecessor).value(),
                acquired.value());
      EXPECT_EQ(acquired.value().intent, intent);
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
      const auto& claim = acquired.value();
      ASSERT_TRUE(
          kernel
              .ReleaseCellAdmissionClaim({intent.owner_role,
                                          intent.owner_epoch,
                                          intent.owner_transaction_id,
                                          intent.request_id,
                                          intent.request_payload_digest,
                                          claim.acquired_admission_generation,
                                          claim.claim_generation,
                                          {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
                                           ContentDigest(test_digest('b'))}})
              .ok());
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
      EXPECT_FALSE(kernel.current_runtime_route_configuration(predecessor.control_domain_id)
                       .value()
                       ->operation_admission_open);
    }
    EXPECT_EQ(engine.execute_calls, 0);
  }
}

TEST(ResourceAddressedExecutionKernelTest,
     FailedReconfigurationRollsBackCandidateSubscriptionAndSelection) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine;
  FakeParticipantEngine candidate_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto candidate_value = select_candidate_product(predecessor_binding).value();
  candidate_value.control_path->active_control_configuration_id =
      ActiveControlConfigurationId("active.replaced");
  auto sealed_candidate = verify_runtime_participant_binding(std::move(candidate_value));
  ASSERT_TRUE(sealed_candidate.ok()) << sealed_candidate.error().message;
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto predecessor = kernel.execute(motion_operation("request.motion.predecessor"));
  ASSERT_TRUE(predecessor.ok()) << predecessor.error().message;

  const auto rejected = kernel.reconfigure_runtime_participants(
      {registration(sealed_candidate.value(), candidate_engine)});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "runtime_active_control_configuration_changed");
  EXPECT_EQ(predecessor_engine.subscription_count(), 1U);
  EXPECT_EQ(candidate_engine.subscription_count(), 0U);
  EXPECT_TRUE(kernel.participant_engine_in_use(&predecessor_engine));
  EXPECT_FALSE(kernel.participant_engine_in_use(&candidate_engine));
}

TEST(ResourceAddressedExecutionKernelTest,
     QuiescentReconfigurationToEmptyRetiresCommandsAndTheOldEngine) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, scheduler, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto reconfigured = kernel.reconfigure_runtime_participants({});
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;
  EXPECT_FALSE(kernel.participant_engine_in_use(&predecessor_engine));
  const auto released = kernel.release_retired_engine(&predecessor_engine);
  ASSERT_TRUE(released.ok()) << released.error().message;
  EXPECT_EQ(predecessor_engine.subscription_count(), 0U);
  EXPECT_TRUE(kernel.resource_snapshot().empty());

  const auto unavailable = kernel.execute(motion_operation("request.motion.empty-runtime"));
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "runtime_participant_unavailable");
  EXPECT_EQ(predecessor_engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentSetRemovalRequiresItsExactFenceAndRetainsTheOtherEngine) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine retained_engine, removed_engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.component-removal");
  const auto retained_binding = make_binding(
      "arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto removed_binding = coordinated_occurrence(retained_binding, "2");
  const auto retained = registration(retained_binding, retained_engine);
  const auto removed = registration(removed_binding, removed_engine);
  ResourceAddressedExecutionKernel kernel(
      {retained, removed}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});

  const auto& removed_path = *removed_binding.value().control_path;
  const ConfigurationFenceScopeDTO scope{
      {removed_binding.value().controllable_component_id},
      {{removed_path.control_domain_id, removed_path.control_domain_revision, std::nullopt}},
      {{removed_path.control_binding_id, removed_path.control_binding_revision, std::nullopt}},
      {removed_binding.value().resources.front().resource_id}};
  ASSERT_TRUE(scope.valid());
  gate.held_scope = scope;
  const auto wrong_fence = kernel.reconfigure_runtime_component_set(
      {retained}, {}, std::nullopt, retained_binding.value().retained_bundle_id,
      retained_binding.value().graph_revision,
      ConfigurationFenceId("fence.other"), scope);
  ASSERT_FALSE(wrong_fence.ok());
  EXPECT_EQ(wrong_fence.error().details.at("reason"),
            "runtime_component_set_fence_missing");
  EXPECT_TRUE(kernel.participant_engine_in_use(&retained_engine));
  EXPECT_TRUE(kernel.participant_engine_in_use(&removed_engine));

  auto incomplete = scope;
  incomplete.controllable_component_ids.front() =
      retained_binding.value().controllable_component_id;
  ASSERT_TRUE(incomplete.valid());
  const auto wrong_scope = kernel.reconfigure_runtime_component_set(
      {retained}, {}, std::nullopt, retained_binding.value().retained_bundle_id,
      retained_binding.value().graph_revision, *gate.held_fence_id, incomplete);
  ASSERT_FALSE(wrong_scope.ok());
  EXPECT_EQ(wrong_scope.error().details.at("reason"),
            "runtime_component_set_fence_missing");
  gate.held_scope = incomplete;
  const auto unfenced = kernel.reconfigure_runtime_component_set(
      {retained}, {}, std::nullopt, retained_binding.value().retained_bundle_id,
      retained_binding.value().graph_revision, *gate.held_fence_id, incomplete);
  ASSERT_FALSE(unfenced.ok());
  EXPECT_EQ(unfenced.error().details.at("reason"),
            "runtime_component_set_scope_incomplete");

  gate.held_scope = scope;
  const auto removed_result = kernel.reconfigure_runtime_component_set(
      {retained}, {}, std::nullopt, retained_binding.value().retained_bundle_id,
      retained_binding.value().graph_revision, *gate.held_fence_id, scope);
  ASSERT_TRUE(removed_result.ok()) << removed_result.error().message;
  EXPECT_TRUE(kernel.participant_engine_in_use(&retained_engine));
  EXPECT_FALSE(kernel.participant_engine_in_use(&removed_engine));
  EXPECT_EQ(retained_engine.subscription_count(), 1U);
  ASSERT_TRUE(kernel.release_retired_engine(&removed_engine).ok());
  EXPECT_EQ(removed_engine.subscription_count(), 0U);
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentSetRejectsCandidateRevisionsOutsideItsExactFence) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine, candidate_engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.candidate-revision-mismatch");
  const auto predecessor_binding = make_binding(
      "arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto candidate_value = predecessor_binding.value();
  candidate_value.retained_bundle_id = RetainedBundleId(test_digest('8'));
  candidate_value.graph_revision = GraphRevision(test_digest('9'));
  candidate_value.control_path->control_domain_revision = ControlDomainRevision(test_digest('a'));
  auto candidate_binding = verify_runtime_participant_binding(std::move(candidate_value));
  ASSERT_TRUE(candidate_binding.ok()) << candidate_binding.error().message;
  const auto predecessor = registration(predecessor_binding, predecessor_engine);
  const auto candidate = registration(candidate_binding.value(), candidate_engine);
  ResourceAddressedExecutionKernel kernel(
      {predecessor}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});
  const auto previous_slots = kernel.resource_table_snapshot();

  const auto& old_path = *predecessor_binding.value().control_path;
  const auto& new_path = *candidate_binding.value().value().control_path;
  const ConfigurationFenceScopeDTO scope{
      {predecessor_binding.value().controllable_component_id},
      {{old_path.control_domain_id, old_path.control_domain_revision,
        ControlDomainRevision(test_digest('b'))}},
      {{old_path.control_binding_id, old_path.control_binding_revision,
        new_path.control_binding_revision}},
      {predecessor_binding.value().resources.front().resource_id}};
  ASSERT_TRUE(scope.valid());
  gate.held_scope = scope;

  const auto rejected = kernel.reconfigure_runtime_component_set(
      {candidate}, {}, std::nullopt, candidate_binding.value().value().retained_bundle_id,
      candidate_binding.value().value().graph_revision, *gate.held_fence_id, scope);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_component_set_scope_incomplete");
  EXPECT_EQ(kernel.resource_table_snapshot(), previous_slots);
  EXPECT_TRUE(kernel.participant_engine_in_use(&predecessor_engine));
  EXPECT_FALSE(kernel.participant_engine_in_use(&candidate_engine));
  EXPECT_EQ(predecessor_engine.subscription_count(), 1U);
  EXPECT_EQ(candidate_engine.subscription_count(), 0U);
}

TEST(ResourceAddressedExecutionKernelTest,
     RetainedControllerCanResealAProductWithoutAnUnrelatedResourceFence) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine, replacement_engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.product-reseal");
  const auto predecessor = make_binding(
      "arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto candidate_value = predecessor.value();
  candidate_value.retained_bundle_id = RetainedBundleId(test_digest('8'));
  candidate_value.graph_revision = GraphRevision(test_digest('9'));
  candidate_value.program_revision = ProgramRevision(test_digest('a'));
  auto candidate = verify_runtime_participant_binding(std::move(candidate_value));
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto predecessor_registration = registration(predecessor, engine);
  const auto candidate_registration = registration(candidate.value(), engine);
  ResourceAddressedExecutionKernel kernel(
      {predecessor_registration}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});

  const ConfigurationFenceScopeDTO no_physical_change{};
  gate.held_scope = no_physical_change;
  const auto wrong_fence = kernel.reconfigure_runtime_component_set(
      {candidate_registration}, {}, std::nullopt,
      candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision,
      ConfigurationFenceId("fence.wrong"), no_physical_change,
      {predecessor.value().controllable_component_id});
  ASSERT_FALSE(wrong_fence.ok());
  EXPECT_EQ(wrong_fence.error().details.at("reason"),
            "runtime_component_set_fence_missing");
  auto changed_safety_ceiling = candidate_registration;
  changed_safety_ceiling.deployment_maximum_tcp_linear_speed_mps = 0.25;
  const auto changed_runtime = kernel.reconfigure_runtime_component_set(
      {changed_safety_ceiling}, {}, std::nullopt,
      candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, *gate.held_fence_id,
      no_physical_change, {predecessor.value().controllable_component_id});
  ASSERT_FALSE(changed_runtime.ok());
  EXPECT_EQ(changed_runtime.error().details.at("reason"),
            "runtime_component_set_scope_incomplete");
  EXPECT_TRUE(kernel.participant_engine_in_use(&engine));
  EXPECT_EQ(engine.subscription_count(), 1U);

  const auto resealed = kernel.reconfigure_runtime_component_set(
      {candidate_registration}, {}, std::nullopt,
      candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, *gate.held_fence_id,
      no_physical_change, {predecessor.value().controllable_component_id});
  ASSERT_TRUE(resealed.ok()) << resealed.error().message;
  EXPECT_TRUE(kernel.participant_engine_in_use(&engine));
  EXPECT_EQ(engine.subscription_count(), 1U);

  const auto replacement_registration = registration(candidate.value(), replacement_engine);
  const auto changed_engine = kernel.reconfigure_runtime_component_set(
      {replacement_registration}, {}, std::nullopt,
      candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, *gate.held_fence_id,
      no_physical_change, {predecessor.value().controllable_component_id});
  ASSERT_FALSE(changed_engine.ok());
  EXPECT_EQ(changed_engine.error().details.at("reason"),
            "runtime_component_set_scope_incomplete");
  EXPECT_TRUE(kernel.participant_engine_in_use(&engine));
  EXPECT_EQ(engine.subscription_count(), 1U);
  EXPECT_FALSE(kernel.participant_engine_in_use(&replacement_engine));
  EXPECT_EQ(replacement_engine.subscription_count(), 0U);

  auto changed_route = candidate.value().value();
  changed_route.control_path->control_route_id = ControlRouteId("route.unfenced");
  if (changed_route.control_path->runtime_scale_qualification.has_value()) {
    changed_route.control_path->runtime_scale_qualification->control_route_id =
        ControlRouteId("route.unfenced");
  }
  auto changed_binding = verify_runtime_participant_binding(std::move(changed_route));
  ASSERT_TRUE(changed_binding.ok()) << changed_binding.error().message;
  const auto rejected = kernel.reconfigure_runtime_component_set(
      {registration(changed_binding.value(), engine)}, {}, std::nullopt,
      candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision,
      *gate.held_fence_id, no_physical_change,
      {predecessor.value().controllable_component_id});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_component_set_scope_incomplete");
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentSetRechecksFenceAfterPreparingCandidateSubscriptions) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine predecessor_engine, candidate_engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  const auto binding = make_binding(
      "arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto& path = *binding.value().control_path;
  const ConfigurationFenceScopeDTO scope{
      {binding.value().controllable_component_id},
      {{path.control_domain_id, path.control_domain_revision, path.control_domain_revision}},
      {{path.control_binding_id, path.control_binding_revision, path.control_binding_revision}},
      {binding.value().resources.front().resource_id}};
  ASSERT_TRUE(scope.valid());
  const ConfigurationFenceId fence_id("fence.released-during-preparation");
  gate.held_fence_id = fence_id;
  gate.held_scope = scope;
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, predecessor_engine)}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});
  const auto previous_slots = kernel.resource_table_snapshot();
  // The subscription is prepared after the initial authority check and before
  // mutation admission. Inject release at this exact boundary, without sleeps.
  candidate_engine.subscribe_hook = [&] {
    std::lock_guard lock(*gate.shared_admission_mutex());
    gate.reject_reservation = false;
    gate.held_fence_id.reset();
  };
  int commits = 0;
  const auto result = kernel.reconfigure_runtime_component_set(
      {registration(binding, candidate_engine)}, {}, std::nullopt,
      binding.value().retained_bundle_id, binding.value().graph_revision,
      fence_id, scope, {}, [&] {
        ++commits;
        return Result<void>::Ok();
      });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_component_set_fence_missing");
  EXPECT_EQ(commits, 0);
  EXPECT_EQ(kernel.resource_table_snapshot(), previous_slots);
  EXPECT_TRUE(kernel.participant_engine_in_use(&predecessor_engine));
  EXPECT_FALSE(kernel.participant_engine_in_use(&candidate_engine));
  EXPECT_EQ(predecessor_engine.subscription_count(), 1U);
  EXPECT_EQ(candidate_engine.subscription_count(), 0U);
}

TEST(ResourceAddressedExecutionKernelTest,
     ComponentSetPublicationCommitRunsOnlyAfterCorePreflightAndFailsClosed) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  FakeResourceAdmissionGate gate;
  gate.held_fence_id = ConfigurationFenceId("fence.component-set-commit");
  const auto predecessor =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto candidate_value = predecessor.value();
  candidate_value.retained_bundle_id = RetainedBundleId(test_digest('8'));
  candidate_value.graph_revision = GraphRevision(test_digest('9'));
  candidate_value.program_revision = ProgramRevision(test_digest('a'));
  auto candidate = verify_runtime_participant_binding(std::move(candidate_value));
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto predecessor_registration = registration(predecessor, engine);
  const auto candidate_registration = registration(candidate.value(), engine);
  ResourceAddressedExecutionKernel kernel(
      {predecessor_registration}, clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U, {}, &gate});

  const ConfigurationFenceScopeDTO unchanged_resources{};
  gate.held_scope = unchanged_resources;
  int commit_calls = 0;
  const auto observe_commit = [&]() {
    ++commit_calls;
    return Result<void>::Ok();
  };

  const auto preflight_refusal = kernel.reconfigure_runtime_component_set(
      {candidate_registration}, {}, std::nullopt, candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, ConfigurationFenceId("fence.wrong"),
      unchanged_resources, {predecessor.value().controllable_component_id}, observe_commit);
  ASSERT_FALSE(preflight_refusal.ok());
  EXPECT_EQ(preflight_refusal.error().details.at("reason"), "runtime_component_set_fence_missing");
  EXPECT_EQ(commit_calls, 0);
  EXPECT_TRUE(kernel.participant_engine_in_use(&engine));

  const auto installed = kernel.reconfigure_runtime_component_set(
      {candidate_registration}, {}, std::nullopt, candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, *gate.held_fence_id, unchanged_resources,
      {predecessor.value().controllable_component_id}, observe_commit);
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_EQ(commit_calls, 1);
  EXPECT_TRUE(kernel.participant_engine_in_use(&engine));

  const auto failed_commit = kernel.reconfigure_runtime_component_set(
      {candidate_registration}, {}, std::nullopt, candidate.value().value().retained_bundle_id,
      candidate.value().value().graph_revision, *gate.held_fence_id, unchanged_resources,
      {predecessor.value().controllable_component_id}, [&]() {
        ++commit_calls;
        return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
                                          "injected observer update failure",
                                          {}});
      });
  ASSERT_FALSE(failed_commit.ok());
  EXPECT_EQ(failed_commit.error().details.at("reason"),
            "runtime_component_set_publication_commit_failed");
  EXPECT_EQ(commit_calls, 2);
  EXPECT_TRUE(gate.reject_reservation);

  // Even if an outer caller mistakenly opens the external fence after this
  // partial publication, the Kernel remains closed against command admission.
  gate.reject_reservation = false;
  const auto refused = kernel.execute(motion_operation("request.commit-failure-closed"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "motion_admission_closed");
}

TEST(ResourceAddressedExecutionKernelTest,
     KernelRetirementDrainsAnAlreadyEnteredCapturedEngineCallback) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto kernel = std::make_unique<ResourceAddressedExecutionKernel>(
      std::vector<RuntimeParticipantExecutionRegistration>{registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"),
                                             8U});
  const auto executed = kernel->execute(motion_operation("request.retirement.captured-event"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;

  HeldSettlementCall callback;
  journal.record_hook = [hold = callback.Blocker()](const std::string_view kind) {
    if (kind == "state:subordinate_state_observed") hold();
  };
  std::promise<void> unsubscribed_promise;
  auto unsubscribed = unsubscribed_promise.get_future();
  std::atomic_bool unsubscribe_observed{};
  engine.unsubscribe_hook = [&] {
    if (!unsubscribe_observed.exchange(true)) unsubscribed_promise.set_value();
  };

  auto dispatched = std::async(std::launch::async, [&] {
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  });
  ASSERT_TRUE(callback.Entered());

  auto retiring = std::async(std::launch::async, [owner = kernel.release()] { delete owner; });
  ASSERT_EQ(unsubscribed.wait_for(10s), std::future_status::ready)
      << "Kernel destruction did not close and unsubscribe event ingress";
  EXPECT_EQ(engine.subscription_count(), 0U);
  EXPECT_EQ(retiring.wait_for(0s), std::future_status::timeout)
      << "Kernel state retired while an entered callback still referenced it";
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(std::find(journal.calls.begin(), journal.calls.end(),
                        "owner_closed"),
              journal.calls.end())
        << "durable owner epoch closed before captured callback drained";
  }

  callback.Release();
  dispatched.get();
  retiring.get();
  EXPECT_NE(std::find(journal.calls.begin(), journal.calls.end(),
                      "state:subordinate_state_observed"),
            journal.calls.end());
  EXPECT_NE(std::find(journal.calls.begin(), journal.calls.end(), "owner_closed"),
            journal.calls.end());
}

TEST(ResourceAddressedExecutionKernelTest,
     CapturedBeforeUnsubscribeCallbackCannotEnterRetiredKernel) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto kernel = std::make_unique<ResourceAddressedExecutionKernel>(
      std::vector<RuntimeParticipantExecutionRegistration>{registration(
          make_binding("arm", "arm", "execution.trajectory.execute",
                       CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{
          RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto executed =
      kernel->execute(motion_operation("request.retirement.stale-capture"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;

  HeldSettlementCall captured;
  auto dispatch = std::async(std::launch::async, [&] {
    engine.emit_transition_after_callback_capture(
        ExecutionState::kPlanning, ExecutionState::kCompleted,
        captured.Blocker());
  });
  ASSERT_TRUE(captured.Entered())
      << "fake publisher did not snapshot the Kernel callback";

  // The callback object now exists independently of the publisher's registry.
  // Complete Close, unsubscribe, ingress drain and owner retirement before it
  // is invoked; unlike the entered-callback test above, Drain has nothing to
  // wait for here.
  kernel.reset();
  ASSERT_EQ(engine.subscription_count(), 0U);
  const auto journal_calls_after_retirement = journal.calls;
  const auto status_reads_after_retirement = engine.status_read_calls.load();
  ASSERT_NE(std::find(journal_calls_after_retirement.begin(),
                      journal_calls_after_retirement.end(), "owner_closed"),
            journal_calls_after_retirement.end());

  captured.Release();
  dispatch.get();

  // Invocation of the stale snapshot returns through closed ingress. It does
  // not sample the participant, submit lane work, or touch the retired owner's
  // journal/state.
  EXPECT_EQ(engine.status_read_calls.load(), status_reads_after_retirement);
  EXPECT_EQ(journal.calls, journal_calls_after_retirement);
}

TEST(ResourceAddressedExecutionKernelTest,
     KernelRetirementSettlesAcceptedCompletionThatLandsDuringShutdown) {
  using namespace std::chrono_literals;

  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto kernel = std::make_unique<ResourceAddressedExecutionKernel>(
      std::vector<RuntimeParticipantExecutionRegistration>{registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"),
                                             8U});
  const auto executed = kernel->execute(motion_operation("request.retirement.completion"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  // Hold the actual participant effect after the accepted request has entered
  // its operation-control lane, not a later journal write that the worker may
  // or may not reach before retirement.
  HeldSettlementCall acknowledgement_effect;
  auto retirement_started = std::make_shared<std::atomic_bool>();
  auto effect_finished_after_retirement_start =
      std::make_shared<std::atomic_bool>();
  engine.acknowledge_effect_hook =
      [hold_effect = acknowledgement_effect.Blocker(), retirement_started,
       effect_finished_after_retirement_start] {
        hold_effect();
        effect_finished_after_retirement_start->store(
            retirement_started->load(std::memory_order_acquire),
            std::memory_order_release);
      };
  auto retirement_started_promise = std::make_shared<std::promise<void>>();
  auto retirement_entered = retirement_started_promise->get_future();
  auto retirement_observed = std::make_shared<std::atomic_bool>();
  engine.unsubscribe_hook = [retirement_started, retirement_started_promise,
                             retirement_observed] {
    retirement_started->store(true, std::memory_order_release);
    if (!retirement_observed->exchange(true)) {
      retirement_started_promise->set_value();
    }
  };
  ASSERT_TRUE(kernel->acknowledge_completed(
                  control_request("request.retirement.completion.ack", executed.value(), 1U))
                  .ok());
  ASSERT_TRUE(acknowledgement_effect.Entered());

  auto retiring =
      std::async(std::launch::async, [owner = kernel.release()] { delete owner; });
  if (retirement_entered.wait_for(10s) != std::future_status::ready) {
    acknowledgement_effect.Release();
    retiring.get();
    FAIL() << "Kernel retirement did not close and unsubscribe event ingress";
  }
  EXPECT_EQ(engine.subscription_count(), 0U);
  EXPECT_EQ(retiring.wait_for(0s), std::future_status::timeout)
      << "Kernel retirement did not wait for its accepted acknowledgement effect";
  EXPECT_FALSE(effect_finished_after_retirement_start->load(
      std::memory_order_acquire));
  {
    std::lock_guard lock(journal.mutex);
    EXPECT_EQ(std::find(journal.calls.begin(), journal.calls.end(),
                        "owner_closed"),
              journal.calls.end())
        << "durable owner epoch closed before the accepted effect settled";
  }

  acknowledgement_effect.Release();
  retiring.get();

  // The accepted effect finished after retirement began and before the lane
  // retired and permitted the durable owner epoch to close.
  EXPECT_TRUE(effect_finished_after_retirement_start->load(
      std::memory_order_acquire));
  EXPECT_EQ(engine.subscription_count(), 0U);
  const auto closed = std::find(journal.calls.begin(), journal.calls.end(), "owner_closed");
  ASSERT_NE(closed, journal.calls.end());
}

TEST(ResourceAddressedExecutionKernelTest,
     FailedRetirementSettlementRetiresIngressWithoutClaimingSuccess) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  auto kernel = std::make_unique<ResourceAddressedExecutionKernel>(
      std::vector<RuntimeParticipantExecutionRegistration>{registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"),
                                             8U});
  const auto executed = kernel->execute(motion_operation("request.retirement.failure"));
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  engine.next_acknowledge_result = {false, ErrorDTO{"cleanup_rejected", "cleanup rejected", {}}};
  HeldSettlementCall settlement;
  std::atomic_bool retirement_started{};
  std::atomic_bool settled_after_retirement_start{};
  const auto test_thread = std::this_thread::get_id();
  journal.record_hook = [&, hold_write = settlement.Blocker()](const std::string_view kind) {
    if (kind != "state:operation_acknowledge_completed_failed" ||
        std::this_thread::get_id() == test_thread) {
      return;
    }
    hold_write();
    if (retirement_started.load(std::memory_order_acquire)) {
      settled_after_retirement_start.store(true, std::memory_order_release);
    }
  };
  ASSERT_TRUE(kernel->acknowledge_completed(
                  control_request("request.retirement.failure.ack", executed.value(), 1U))
                  .ok());
  ASSERT_TRUE(settlement.Entered());

  retirement_started.store(true, std::memory_order_release);
  std::thread retiring([owner = kernel.release()] { delete owner; });
  settlement.Release();
  retiring.join();

  // The exceptional settlement still drains before the lanes and subscriptions
  // disappear, and an unsuccessful cleanup is never recorded as success.
  EXPECT_TRUE(settled_after_retirement_start.load(std::memory_order_acquire));
  EXPECT_EQ(engine.subscription_count(), 0U);
  ASSERT_FALSE(journal.states.empty());
  EXPECT_NE(journal.states.back().state, OperationStateKind::kCompleted);
  EXPECT_NE(journal.states.back().state, OperationStateKind::kStopped);
  const auto closed =
      std::find(journal.calls.begin(), journal.calls.end(), "owner_closed");
  ASSERT_NE(closed, journal.calls.end());
  EXPECT_NE(std::find(journal.calls.begin(), closed,
                      "state:operation_acknowledge_completed_failed"),
            closed);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReplacedUnclosedEpochRetiresWithoutCleanCloseOrReadiness) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  // Process loss of the previous owner: the durable epoch never closed, so
  // clean-close eligibility stays withheld from this successor.
  journal.recovery.replaced_unclosed_owner_epoch = RequestOwnerEpoch("kernel.epoch.predecessor");
  auto kernel = std::make_unique<ResourceAddressedExecutionKernel>(
      std::vector<RuntimeParticipantExecutionRegistration>{registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"),
                                             8U});
  EXPECT_EQ(std::count(journal.calls.begin(), journal.calls.end(), "owner_closed"), 0);

  kernel.reset();

  // Teardown still removed the subscription, but it must not have asserted that
  // this owner cleanly closed the durable epoch.
  EXPECT_EQ(engine.subscription_count(), 0U);
  EXPECT_EQ(std::find(journal.calls.begin(), journal.calls.end(), "owner_closed"),
            journal.calls.end());
}

// V13.7.5 verification: native motion-preview admission while a failed
// motion is held. A pre-dispatch failure stays resident (intervention
// required) until recovery or Reset and keeps execution admission exclusive.
// Because it provably never dispatched, an effect-free preview of a corrected
// target for the same robot is admitted; a held failure with a dispatched or
// uncertain effect still refuses preview.
TEST(ResourceAddressedExecutionKernelTest,
     HeldPreDispatchFailureAdmitsSameTargetPreviewButNotExecution) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeParticipantEngine other_engine;
  FakeOperationJournal journal;
  engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "injected IK failure", {{"reason", "native_ik_no_solution"}}};
  engine.recovery_assessment.assessment_revision = 7U;
  engine.recovery_assessment.control_command_epoch = 11U;
  engine.recovery_assessment.execution_id = 41U;
  engine.recovery_assessment.status = RecoveryAssessmentStatus::kActionRequired;
  engine.recovery_assessment.allowed_actions = {RecoveryAction::kAcknowledgeAndContinue};
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second = second_same_type_occurrence(first);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, engine), registration(second, other_engine)}, clock, scheduler,
      journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto failed = kernel.execute(motion_operation("request.held.failure"));
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  const auto held = kernel.get_operation_state(failed.value().operation_id);
  ASSERT_TRUE(held.ok()) << held.error().message;
  ASSERT_EQ(held.value().state, OperationStateKind::kInterventionRequired);

  {
    auto same = kernel.begin_motion_preview_session(motion_operation("preview.same"));
    ASSERT_TRUE(same.ok()) << same.error().message;
  }
  auto disjoint = motion_operation("preview.disjoint");
  disjoint.program_binding_id = second.value().program_binding_id;
  disjoint.participants.front().component_role_id = second.value().component_role_id;
  disjoint.participants.front().target_role_id = second.value().target_role_id;
  EXPECT_TRUE(kernel.begin_motion_preview_session(disjoint).ok());
  // Preview had no effect: the held failure, its revision and the robot's
  // execution exclusion are unchanged.
  const auto after_preview = kernel.get_operation_state(failed.value().operation_id);
  ASSERT_TRUE(after_preview.ok()) << after_preview.error().message;
  EXPECT_EQ(after_preview.value(), held.value());
  EXPECT_EQ(engine.prepare_motion_calls, 1);
  const auto started = kernel.execute(motion_operation("request.held.second-start"));
  EXPECT_FALSE(started.ok());
  EXPECT_EQ(engine.prepare_motion_calls, 1);
  EXPECT_EQ(kernel.get_operation_state(failed.value().operation_id).value().state,
            OperationStateKind::kInterventionRequired);

  const auto projection = kernel.get_operation_recovery_assessment(failed.value().operation_id);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  const KernelRecoveryActionRequestDTO request{"request.recovery.preview",
                                               RecoveryAction::kAcknowledgeAndContinue,
                                               "operator.test",
                                               projection.value().assessment.assessment_revision,
                                               projection.value().assessment.control_command_epoch,
                                               projection.value().assessment.execution_id,
                                               projection.value().assessment.latch_id,
                                               projection.value().assessment.stop_certificate_id,
                                               projection.value().assessment.stop_owner_generation,
                                               "release the held failure"};
  auto resulting = projection.value().assessment;
  resulting.assessment_revision = 8U;
  resulting.status = RecoveryAssessmentStatus::kUnavailable;
  engine.next_recovery_result =
      KernelRecoveryActionResultDTO{request.request_id, request.action, true, resulting, std::nullopt};
  const auto recovered = kernel.apply_operation_recovery_action(
      failed.value().operation_id, held.value().operation_revision, request);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;

  const auto released = kernel.begin_motion_preview_session(motion_operation("preview.released"));
  EXPECT_TRUE(released.ok()) << (released.ok() ? "" : released.error().message);
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(other_engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, HeldFailureWithDispatchedEffectStillRefusesPreview) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation("request.held.dispatched"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kInterventionRequired);
  ASSERT_TRUE(wait_until([&] {
    const auto state = kernel.get_operation_state(operation.value().operation_id);
    return state.ok() && state.value().state == OperationStateKind::kInterventionRequired;
  }));
  const auto preview = kernel.begin_motion_preview_session(motion_operation("preview.dispatched"));
  ASSERT_FALSE(preview.ok());
  EXPECT_EQ(preview.error().details.at("reason"), "motion_preview_operation_busy");
}

// V13.7.5 verification: an unresolved Stop fault latch closes preview like
// every other motion admission.
TEST(ResourceAddressedExecutionKernelTest, StopFaultLatchClosesMotionPreview) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepare_completion = true;
  engine.prepared_cancel_terminal_state = ExecutionState::kFailed;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  const auto accepted = kernel.execute(motion_operation("request.motion.latched"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.stop.latched", accepted.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] { return kernel.get_stop_fault_latch().has_value(); }));

  const auto preview = kernel.begin_motion_preview_session(motion_operation("preview.latched"));
  ASSERT_FALSE(preview.ok());
  EXPECT_EQ(preview.error().details.at("reason"), "motion_admission_closed");
}

// --- Operator-alarm motion attention (spec 450 §5.2, spec 530 §11) -------

constexpr Duration kAttentionMaximumAgeNs = 2'000'000'000;
constexpr const char* kAttentionHostBootId = "boot.test";

[[nodiscard]] OperatorAlarmAttentionMemberDTO alarm_member(
    std::string condition, OperatorAlarmAttentionGateDTO gate,
    std::vector<std::string> resources) {
  OperatorAlarmAttentionMemberDTO member;
  member.condition_id = std::move(condition);
  member.activation_id = member.condition_id + ".activation.1";
  member.record_revision = 1U;
  member.gate = gate;
  member.subject_kind = resources.empty() ? "cell" : "component";
  member.subject_id = resources.empty() ? "cell.test" : "robot";
  for (auto& resource : resources) member.resource_ids.emplace_back(std::move(resource));
  return member;
}

/// The active configuration every `make_binding` registration declares.
[[nodiscard]] OperatorAlarmAttentionConfigurationDTO kernel_active_configuration() {
  return {RetainedBundleId(test_digest('6')), GraphRevision(test_digest('7'))};
}

[[nodiscard]] OperatorAlarmAttentionSnapshotDTO attention_snapshot(
    std::uint64_t generation, std::vector<OperatorAlarmAttentionMemberDTO> members = {},
    bool reconciled = true, std::string owner_epoch = "alarm.owner.1",
    std::uint64_t owner_incarnation_us = 1U,
    std::optional<OperatorAlarmAttentionConfigurationDTO> active_configuration =
        kernel_active_configuration()) {
  OperatorAlarmAttentionSnapshotDTO snapshot;
  snapshot.active_configuration = std::move(active_configuration);
  snapshot.cell_id = CellId("cell.test");
  snapshot.owner_epoch = std::move(owner_epoch);
  snapshot.owner_host_boot_id = kAttentionHostBootId;
  snapshot.owner_incarnation_us = owner_incarnation_us;
  snapshot.generation = generation;
  snapshot.reconciled = reconciled;
  std::sort(members.begin(), members.end(),
            [](const auto& left, const auto& right) { return left.condition_id < right.condition_id; });
  snapshot.members = std::move(members);
  return snapshot;
}

[[nodiscard]] bool all_slots_free(const ResourceAddressedExecutionKernel& kernel) {
  const auto slots = kernel.resource_snapshot();
  return std::all_of(slots.begin(), slots.end(),
                     [](const auto& slot) { return slot.state == ResourceSlotState::kFree; });
}

[[nodiscard]] ResourceAddressedExecutionKernelConfig attention_config() {
  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 16U};
  config.operator_alarm_attention = OperatorAlarmAttentionPolicyDTO{kAttentionMaximumAgeNs,
                                                                    kAttentionHostBootId};
  return config;
}

[[nodiscard]] ProgramNodeMotionAttentionCheckDTO node_attention_check(std::string request_id,
                                                                     std::string node) {
  const auto selection = program_selection_request(std::move(request_id), "all");
  return {selection.request_context,       selection.program_runner_owner_epoch,
          selection.claim_generation,      selection.program_run_id,
          ProgramNodeInstanceId(std::move(node)), {}};
}

/// The canonical JSON membership carried in a refusal's details.
[[nodiscard]] std::vector<std::string> refusal_conditions(const ErrorDTO& error,
                                                         const std::string& key) {
  const auto& text = error.details.at(key);
  std::vector<std::string> conditions;
  const std::string marker = "\"condition_id\":\"";
  for (auto at = text.find(marker); at != std::string::npos; at = text.find(marker, at)) {
    at += marker.size();
    conditions.push_back(text.substr(at, text.find('"', at) - at));
  }
  return conditions;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     InterlockAndAcknowledgementGatesRejectIntersectingRunReportingBothMemberships) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());

  // An acknowledged condition interlock is still published as gating; the
  // Kernel rejects with the interlock reason and carries both memberships.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      3U, {alarm_member("interlock.arm", OperatorAlarmAttentionGateDTO::kConditionInterlock,
                                        {"robot.joint_1"}),
                           alarm_member("ack.arm", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto both = kernel.execute(motion_operation("alarm.both"));
  ASSERT_FALSE(both.ok());
  EXPECT_EQ(both.error().code, "operator_alarm_interlock_active");
  EXPECT_EQ(both.error().details.at("reason"), "operator_alarm_interlock_active");
  EXPECT_EQ(both.error().details.at("owner_epoch"), "alarm.owner.1");
  EXPECT_EQ(both.error().details.at("generation"), "3");
  EXPECT_EQ(refusal_conditions(both.error(), "interlock_alarms"),
            std::vector<std::string>{"interlock.arm"});
  EXPECT_EQ(refusal_conditions(both.error(), "unacknowledged_alarms"),
            std::vector<std::string>{"ack.arm"});

  // Acknowledging cannot release the interlock: only its clear does.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      4U, {alarm_member("interlock.arm",
                                        OperatorAlarmAttentionGateDTO::kConditionInterlock,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto interlock = kernel.execute(motion_operation("alarm.interlock"));
  ASSERT_FALSE(interlock.ok());
  EXPECT_EQ(interlock.error().code, "operator_alarm_interlock_active");
  EXPECT_TRUE(refusal_conditions(interlock.error(), "unacknowledged_alarms").empty());

  // An acknowledgement-gated record gates until it is acknowledged.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      5U, {alarm_member("ack.arm", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto unacknowledged = kernel.execute(motion_operation("alarm.unacknowledged"));
  ASSERT_FALSE(unacknowledged.ok());
  EXPECT_EQ(unacknowledged.error().code, "unacknowledged_operator_alarms");
  EXPECT_TRUE(refusal_conditions(unacknowledged.error(), "interlock_alarms").empty());
  EXPECT_EQ(engine.prepare_motion_calls, 0);
  EXPECT_TRUE(all_slots_free(kernel));

  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(6U)).ok());
  const auto admitted = kernel.execute(motion_operation("alarm.acknowledged"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     DisjointResourcesContinueWhileCellGlobalSubjectBlocksEveryRun) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      1U, {alarm_member("other.robot",
                                        OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.9.joint_1"})}))
                  .ok());
  const auto disjoint = kernel.execute(motion_operation("alarm.disjoint"));
  ASSERT_TRUE(disjoint.ok()) << disjoint.error().message;

  FakeParticipantEngine second_engine;
  ResourceAddressedExecutionKernel blocked(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          second_engine)},
      clock, scheduler, default_journal, attention_config());
  ASSERT_TRUE(blocked
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      1U, {alarm_member("cell.scope", OperatorAlarmAttentionGateDTO::kConditionInterlock,
                                        {})}))
                  .ok());
  const auto cell = blocked.execute(motion_operation("alarm.cell"));
  ASSERT_FALSE(cell.ok());
  EXPECT_EQ(cell.error().code, "operator_alarm_interlock_active");
  EXPECT_EQ(refusal_conditions(cell.error(), "interlock_alarms"),
            std::vector<std::string>{"cell.scope"});
  EXPECT_EQ(second_engine.prepare_motion_calls, 0);
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     CoordinatedAdmissionUsesTheUnionOfParticipantResources) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(first, first_engine),
       registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, default_journal, attention_config());
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      1U, {alarm_member("second.robot",
                                        OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.2.joint_1"})}))
                  .ok());
  const auto rejected = kernel.execute(coordinated_motion_operation("alarm.coordinated"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "unacknowledged_operator_alarms");
  EXPECT_EQ(first_engine.prepare_motion_calls, 0);
  EXPECT_EQ(second_engine.prepare_motion_calls, 0);
  EXPECT_TRUE(all_slots_free(kernel));
  // The first participant alone is disjoint from the alarm and continues.
  const auto single = kernel.execute(motion_operation("alarm.first.only"));
  ASSERT_TRUE(single.ok()) << single.error().message;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     FinalPreDispatchRecheckRefusesAnAlarmRaisedAfterAdmission) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.defer_prepare_completion = true;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(1U)).ok());
  const auto accepted = kernel.execute(motion_operation("alarm.predispatch"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(engine.prepare_motion_calls, 1);

  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      2U, {alarm_member("late.arm", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  ASSERT_TRUE(engine.complete_preparation().ok());
  ASSERT_TRUE(wait_until([&] {
    const auto projection = kernel.get_operation_projection(accepted.value().operation_id);
    return projection.ok() && !projection.value().resident;
  }));
  EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.cancel_prepared_motion_calls, 1);
  EXPECT_TRUE(all_slots_free(kernel));
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_EQ(state.value().participants.size(), 1U);
  ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(state.value().participants.front().terminal_error->code,
            "unacknowledged_operator_alarms");
}

/// Publishes a blocking alarm, or ages the projection past its bound, while
/// the Kernel's prepared-state journal write has released its state lock.
void raise_attention_during_write(ResourceAddressedExecutionKernel& kernel, FakeClock& clock,
                                  const bool stale) {
  if (stale) {
    clock.advance(kAttentionMaximumAgeNs + 1);
    return;
  }
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      2U, {alarm_member("late.arm", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.2.joint_1", "robot.joint_1"})}))
                  .ok());
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     AttentionRaisedDuringThePreparedStateWriteRefusesDispatch) {
  for (const bool stale : {false, true}) {
    SCOPED_TRACE(stale ? "projection aged out" : "blocking alarm published");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    engine.defer_prepare_completion = true;
    FakeOperationJournal journal;
    HeldSettlementCall held;
    journal.record_hook = [&, block = held.Blocker()](std::string_view kind) {
      if (kind == "state:subordinate_motion_prepared") block();
    };
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, journal, attention_config());
    ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(1U)).ok());
    const auto accepted = kernel.execute(motion_operation("alarm.write.window"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    auto prepared = std::async(std::launch::async, [&] { return engine.complete_preparation(); });
    ASSERT_TRUE(held.Entered());
    raise_attention_during_write(kernel, clock, stale);
    held.Release();
    ASSERT_TRUE(prepared.get().ok());
    ASSERT_TRUE(wait_until([&] {
      const auto projection = kernel.get_operation_projection(accepted.value().operation_id);
      return projection.ok() && !projection.value().resident;
    }));
    EXPECT_EQ(engine.dispatch_prepared_motion_calls, 0);
    EXPECT_EQ(engine.execute_calls, 0);
    EXPECT_EQ(engine.cancel_prepared_motion_calls, 1);
    EXPECT_TRUE(all_slots_free(kernel));
    const auto state = kernel.get_operation_state(accepted.value().operation_id);
    ASSERT_TRUE(state.ok()) << state.error().message;
    EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
    ASSERT_EQ(state.value().participants.size(), 1U);
    ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
    EXPECT_EQ(state.value().participants.front().terminal_error->code,
              stale ? "operator_alarm_state_unavailable" : "unacknowledged_operator_alarms");
  }
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     AttentionRaisedDuringTheCoordinatedBarrierWriteCancelsEveryParticipant) {
  for (const bool stale : {false, true}) {
    SCOPED_TRACE(stale ? "projection aged out" : "blocking alarm published");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine first_engine;
    FakeParticipantEngine second_engine;
    first_engine.defer_prepare_completion = second_engine.defer_prepare_completion = true;
    FakeOperationJournal journal;
    HeldSettlementCall held;
    journal.record_hook = [&, block = held.Blocker()](std::string_view kind) {
      if (kind == "state:coordinated_prepare_barrier_complete") block();
    };
    const auto first =
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
    ResourceAddressedExecutionKernel kernel(
        {registration(first, first_engine),
         registration(coordinated_occurrence(first, "2"), second_engine)},
        clock, scheduler, journal, attention_config());
    ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(1U)).ok());
    const auto accepted = kernel.execute(coordinated_motion_operation("alarm.barrier.window"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    auto prepared = std::async(std::launch::async, [&] {
      const auto first_prepared = first_engine.complete_preparation();
      return first_prepared.ok() ? second_engine.complete_preparation() : first_prepared;
    });
    ASSERT_TRUE(held.Entered());
    raise_attention_during_write(kernel, clock, stale);
    held.Release();
    ASSERT_TRUE(prepared.get().ok());
    ASSERT_TRUE(wait_until([&] {
      const auto projection = kernel.get_operation_projection(accepted.value().operation_id);
      return projection.ok() && !projection.value().resident;
    }));
    for (const auto* engine : {&first_engine, &second_engine}) {
      EXPECT_EQ(engine->dispatch_prepared_motion_calls, 0);
      EXPECT_EQ(engine->cancel_prepared_motion_calls, 1);
    }
    EXPECT_TRUE(all_slots_free(kernel));
    const auto state = kernel.get_operation_state(accepted.value().operation_id);
    ASSERT_TRUE(state.ok()) << state.error().message;
    EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
    ASSERT_EQ(state.value().participants.size(), 2U);
    for (const auto& participant : state.value().participants) {
      ASSERT_TRUE(participant.terminal_error.has_value());
      EXPECT_EQ(participant.terminal_error->code,
                stale ? "operator_alarm_state_unavailable" : "unacknowledged_operator_alarms");
    }
  }
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     MissingUnreconciledOrStaleProjectionFailsClosed) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  const auto availability = [&](const std::string& request) {
    const auto result = kernel.execute(motion_operation(request));
    EXPECT_FALSE(result.ok());
    if (result.ok()) return std::string("admitted");
    EXPECT_EQ(result.error().code, "operator_alarm_state_unavailable");
    return result.error().details.at("availability");
  };
  EXPECT_EQ(availability("alarm.missing"), "missing");
  ASSERT_TRUE(
      kernel.PublishOperatorAlarmAttention(attention_snapshot(1U, {}, /*reconciled=*/false)).ok());
  EXPECT_EQ(availability("alarm.unreconciled"), "unreconciled");
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(2U)).ok());
  clock.advance(kAttentionMaximumAgeNs + 1);
  EXPECT_EQ(availability("alarm.stale"), "stale");
  EXPECT_EQ(engine.prepare_motion_calls, 0);
  // A same-generation heartbeat refreshes freshness.
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(2U)).ok());
  const auto admitted = kernel.execute(motion_operation("alarm.fresh"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     PublicationIsGenerationAndOwnerEpochFenced) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  const auto accepted = kernel.PublishOperatorAlarmAttention(attention_snapshot(7U));
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().owner_epoch, "alarm.owner.1");
  EXPECT_EQ(accepted.value().generation, 7U);
  const auto older = kernel.PublishOperatorAlarmAttention(attention_snapshot(
      6U, {alarm_member("old", OperatorAlarmAttentionGateDTO::kAcknowledgement, {"robot.joint_1"})}));
  ASSERT_FALSE(older.ok());
  EXPECT_EQ(older.error().details.at("reason"), "operator_alarm_attention_generation_stale");
  // One generation names one content: identical content is the heartbeat,
  // different content under the retained generation is refused.
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(7U)).ok());
  const auto conflicting = kernel.PublishOperatorAlarmAttention(attention_snapshot(
      7U, {alarm_member("same.generation", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                        {"robot.joint_1"})}));
  ASSERT_FALSE(conflicting.ok());
  EXPECT_EQ(conflicting.error().code, "operator_alarm_attention_conflict");
  EXPECT_EQ(conflicting.error().details.at("reason"),
            "operator_alarm_attention_generation_conflict");
  // A restarted owner (greater incarnation) replaces the retained epoch; the
  // older incarnation cannot return.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(
                      attention_snapshot(1U, {}, true, "alarm.owner.2", 2U))
                  .ok());
  const auto replaced = kernel.PublishOperatorAlarmAttention(attention_snapshot(8U));
  ASSERT_FALSE(replaced.ok());
  EXPECT_EQ(replaced.error().details.at("reason"),
            "operator_alarm_attention_owner_incarnation_stale");
  auto foreign = attention_snapshot(2U, {}, true, "alarm.owner.2", 2U);
  foreign.cell_id = CellId("cell.other");
  EXPECT_FALSE(kernel.PublishOperatorAlarmAttention(foreign).ok());
  // Single-host precondition: another host boot is refused, never compared.
  auto other_host = attention_snapshot(1U, {}, true, "alarm.owner.3", 3U);
  other_host.owner_host_boot_id = "boot.other";
  const auto other_host_refused = kernel.PublishOperatorAlarmAttention(other_host);
  ASSERT_FALSE(other_host_refused.ok());
  EXPECT_EQ(other_host_refused.error().details.at("reason"),
            "operator_alarm_attention_host_mismatch");
  auto invalid = attention_snapshot(
      2U, {alarm_member("bad", OperatorAlarmAttentionGateDTO::kAcknowledgement, {})}, true,
      "alarm.owner.2", 2U);
  invalid.members.front().subject_id = "cell.other";
  EXPECT_FALSE(kernel.PublishOperatorAlarmAttention(invalid).ok());
  ASSERT_TRUE(kernel.execute(motion_operation("alarm.after.fence")).ok());
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     OwnerEpochsAreOrderedByIncarnationWithoutAnyReplacementBound) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  const auto epoch = [](std::size_t index) { return "alarm.owner." + std::to_string(index); };
  // Far more than any former retirement bound: every newer incarnation wins.
  constexpr std::size_t kReplacements = 200U;
  for (std::size_t index = 0U; index <= kReplacements; ++index) {
    ASSERT_TRUE(kernel
                    .PublishOperatorAlarmAttention(
                        attention_snapshot(1U, {}, true, epoch(index), 1'000U + index))
                    .ok())
        << index;
  }
  // Every older incarnation is refused, however long ago it was replaced.
  for (const std::size_t older : {std::size_t{0U}, std::size_t{1U}, kReplacements - 1U}) {
    const auto refused = kernel.PublishOperatorAlarmAttention(
        attention_snapshot(9U, {}, true, epoch(older), 1'000U + older));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "operator_alarm_attention_owner_incarnation_stale");
  }
  // An equal incarnation under another epoch is a conflict, not a successor.
  const auto equal = kernel.PublishOperatorAlarmAttention(
      attention_snapshot(1U, {}, true, "alarm.owner.impostor", 1'000U + kReplacements));
  ASSERT_FALSE(equal.ok());
  EXPECT_EQ(equal.error().details.at("reason"),
            "operator_alarm_attention_owner_incarnation_conflict");
  // The retained epoch cannot change its incarnation either.
  const auto reincarnated = kernel.PublishOperatorAlarmAttention(
      attention_snapshot(2U, {}, true, epoch(kReplacements), 5'000U));
  ASSERT_FALSE(reincarnated.ok());
  EXPECT_EQ(reincarnated.error().details.at("reason"),
            "operator_alarm_attention_owner_incarnation_conflict");
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(
                      attention_snapshot(2U, {}, true, epoch(kReplacements), 1'000U + kReplacements))
                  .ok());
  ASSERT_TRUE(kernel.execute(motion_operation("alarm.after.rollover")).ok());
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     ProgramRunReleaseAndItsNoEffectCheckUseTheSelectedNodesResources) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  auto config = attention_config();
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  ASSERT_TRUE(kernel.register_program_run_capture(program_execution_capture()).ok());
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      1U, {alarm_member("arm.alarm", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto initial = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(initial.ok()) << initial.error().message;

  // The timer node has no effectful target and is not gated.
  EXPECT_TRUE(kernel.check_program_node_motion_attention(node_attention_check("check.timer", "timer"))
                  .ok());
  const auto check =
      kernel.check_program_node_motion_attention(node_attention_check("check.move", "move"));
  ASSERT_FALSE(check.ok());
  EXPECT_EQ(check.error().code, "unacknowledged_operator_alarms");
  const auto unknown =
      kernel.check_program_node_motion_attention(node_attention_check("check.unknown", "absent"));
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "program_node_unknown");
  const auto refused = kernel.start_program_selection(program_selection_request("run.arm", "arm"),
                                                      initial.value().selection_revision);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "unacknowledged_operator_alarms");
  const auto unchanged = kernel.get_program_execution_scope(ProgramRunId("run.1"));
  ASSERT_TRUE(unchanged.ok());
  EXPECT_EQ(unchanged.value().selection_revision, initial.value().selection_revision);

  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(2U)).ok());
  const auto started = kernel.start_program_selection(
      program_selection_request("run.arm.acknowledged", "arm"), initial.value().selection_revision);
  ASSERT_TRUE(started.ok()) << started.error().message;
  const auto admitted = kernel.execute(scoped_motion_operation("alarm.scoped.motion"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     MotionProducingRecoveryIsRefusedWhileNonMovingAcknowledgeContinues) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "injected preparation failure",
                                    {{"reason", "injected_preparation_failure"}}};
  engine.recovery_assessment.assessment_revision = 7U;
  engine.recovery_assessment.status = RecoveryAssessmentStatus::kActionRequired;
  engine.recovery_assessment.allowed_actions = {RecoveryAction::kRetryMotionFromCurrentState,
                                                RecoveryAction::kAcknowledgeAndContinue};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(1U)).ok());
  const auto accepted = kernel.execute(motion_operation("alarm.recovery.motion"));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto operation = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  ASSERT_EQ(operation.value().state, OperationStateKind::kInterventionRequired);
  const auto projection = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
  ASSERT_TRUE(projection.ok()) << projection.error().message;
  const auto request = [&](std::string id, RecoveryAction action) {
    const auto& assessment = projection.value().assessment;
    return KernelRecoveryActionRequestDTO{std::move(id),
                                          action,
                                          "operator.test",
                                          assessment.assessment_revision,
                                          assessment.control_command_epoch,
                                          assessment.execution_id,
                                          assessment.latch_id,
                                          assessment.stop_certificate_id,
                                          assessment.stop_owner_generation,
                                          "after alarm"};
  };
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      2U, {alarm_member("arm.fault", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto refused = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision,
      request("alarm.recovery.retry", RecoveryAction::kRetryMotionFromCurrentState));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "unacknowledged_operator_alarms");
  EXPECT_EQ(engine.recovery_action_calls, 0);

  // Acknowledge-and-continue moves nothing and stays available.
  const auto acknowledge = request("alarm.recovery.continue", RecoveryAction::kAcknowledgeAndContinue);
  auto resulting = projection.value().assessment;
  resulting.assessment_revision = 8U;
  resulting.status = RecoveryAssessmentStatus::kUnavailable;
  engine.next_recovery_result =
      KernelRecoveryActionResultDTO{acknowledge.request_id, acknowledge.action, true, resulting,
                                    std::nullopt};
  const auto continued = kernel.apply_operation_recovery_action(
      accepted.value().operation_id, operation.value().operation_revision, acknowledge);
  ASSERT_TRUE(continued.ok()) << continued.error().message;
  EXPECT_EQ(engine.recovery_action_calls, 1);
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     RecoveryCheckTestsOnlyTheHeldNodesResources) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  auto config = attention_config();
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, std::move(config));
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  // Robot A's node "move" and robot B's node "move.b" in one captured run.
  auto capture = program_execution_capture();
  auto binding = std::make_shared<ProgramBindingDTO>(*capture.binding);
  auto robot_b = binding->target_role_bindings.front();
  robot_b.target_role_id = TargetRoleId("arm_b");
  robot_b.resource_role_bindings.front().resource_ids = {ResourceId("robot.b.joint_1")};
  binding->target_role_bindings.push_back(std::move(robot_b));
  capture.binding = std::move(binding);
  auto table = std::make_shared<ProgramExecutionTableDTO>(*capture.table);
  table->nodes.insert(table->nodes.begin() + 1,
                      {ProgramNodeInstanceId("move.b"),
                       "kernel.operation",
                       {{TargetRoleId("arm_b"), CapabilityId("execution.trajectory.execute"),
                         true}}});
  table->named_selections = {{"arm", {0U, 1U}}, {"supervision", {2U}}};
  capture.table = std::move(table);
  ASSERT_TRUE(kernel.register_program_run_capture(std::move(capture)).ok());

  // B's alarm does not block recovery of A's held node.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      1U, {alarm_member("robot_b.alarm",
                                        OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.b.joint_1"})}))
                  .ok());
  EXPECT_TRUE(
      kernel.check_program_node_motion_attention(node_attention_check("recover.a", "move")).ok());
  const auto b_refused =
      kernel.check_program_node_motion_attention(node_attention_check("recover.b", "move.b"));
  ASSERT_FALSE(b_refused.ok());
  EXPECT_EQ(b_refused.error().code, "unacknowledged_operator_alarms");

  // A's own alarm does.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      2U, {alarm_member("robot_a.alarm",
                                        OperatorAlarmAttentionGateDTO::kConditionInterlock,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto a_refused =
      kernel.check_program_node_motion_attention(node_attention_check("recover.a.own", "move"));
  ASSERT_FALSE(a_refused.ok());
  EXPECT_EQ(a_refused.error().code, "operator_alarm_interlock_active");
  EXPECT_TRUE(
      kernel.check_program_node_motion_attention(node_attention_check("recover.b.clear", "move.b"))
          .ok());
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     ManualOperationIsNeverAttentionGatedAtExecuteOrRecovery) {
  // The manual identity is fixed at admission (intrinsic direct control), so
  // every motion-producing path of the same operation agrees with Execute.
  const auto manual_kernel_config = [] {
    auto config = attention_config();
    config.manager_liveness_required_set = EmptyManagerLivenessSet();
    return config;
  };
  const auto gating = [](std::uint64_t generation) {
    return attention_snapshot(
        generation,
        {alarm_member("arm.interlock", OperatorAlarmAttentionGateDTO::kConditionInterlock,
                      {"robot.joint_1"}),
         alarm_member("arm.fault", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                      {"robot.joint_1"})});
  };
  {
    SCOPED_TRACE("Execute");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    auto direct = registration(
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        engine);
    direct.intrinsic_direct_control = true;
    ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, default_journal,
                                            manual_kernel_config());
    ASSERT_TRUE(ready_cell_admission(kernel).ok());
    ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(gating(1U)).ok());
    const auto admitted = kernel.execute(motion_operation("manual.alarm.motion"));
    ASSERT_TRUE(admitted.ok()) << admitted.error().message;
    ASSERT_TRUE(wait_until([&] {
      return engine.execute_calls == 1 && engine.get_status().state == ExecutionState::kPlanning;
    }));
    engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  }
  {
    SCOPED_TRACE("motion-producing recovery");
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    engine.prepare_failure = ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                      "injected preparation failure",
                                      {{"reason", "injected_preparation_failure"}}};
    engine.recovery_assessment.assessment_revision = 7U;
    engine.recovery_assessment.status = RecoveryAssessmentStatus::kActionRequired;
    engine.recovery_assessment.allowed_actions = {RecoveryAction::kRetryMotionFromCurrentState};
    auto direct = registration(
        make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        engine);
    direct.intrinsic_direct_control = true;
    ResourceAddressedExecutionKernel kernel({direct}, clock, scheduler, default_journal,
                                            manual_kernel_config());
    ASSERT_TRUE(ready_cell_admission(kernel).ok());
    ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(gating(1U)).ok());
    const auto accepted = kernel.execute(motion_operation("manual.alarm.recovery"));
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    const auto operation = kernel.get_operation_state(accepted.value().operation_id);
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    ASSERT_EQ(operation.value().state, OperationStateKind::kInterventionRequired);
    const auto projection = kernel.get_operation_recovery_assessment(accepted.value().operation_id);
    ASSERT_TRUE(projection.ok()) << projection.error().message;
    const auto& assessment = projection.value().assessment;
    const KernelRecoveryActionRequestDTO retry{"manual.alarm.retry",
                                               RecoveryAction::kRetryMotionFromCurrentState,
                                               "operator.test",
                                               assessment.assessment_revision,
                                               assessment.control_command_epoch,
                                               assessment.execution_id,
                                               assessment.latch_id,
                                               assessment.stop_certificate_id,
                                               assessment.stop_owner_generation,
                                               "manual retry under alarm"};
    auto resulting = assessment;
    resulting.assessment_revision = 8U;
    resulting.status = RecoveryAssessmentStatus::kUnavailable;
    engine.next_recovery_result =
        KernelRecoveryActionResultDTO{retry.request_id, retry.action, true, resulting, std::nullopt};
    ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(gating(2U)).ok());
    const auto recovered = kernel.apply_operation_recovery_action(
        accepted.value().operation_id, operation.value().operation_revision, retry);
    ASSERT_TRUE(recovered.ok()) << recovered.error().code << ": " << recovered.error().message;
    EXPECT_EQ(engine.recovery_action_calls, 1);
  }
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     AttentionJudgedAgainstAnotherConfigurationIsUnavailable) {
  // An Apply renamed the robot's resources (robot.old.joint_1 is now
  // robot.joint_1). Until the owner observes that configuration, its
  // unacknowledged fault still names the old ids and cannot be tested against
  // this Kernel's resources: Run is refused instead of admitted as disjoint.
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  const OperatorAlarmAttentionConfigurationDTO previous{RetainedBundleId(test_digest('4')),
                                                         GraphRevision(test_digest('5'))};
  const auto fault = alarm_member("arm.fault", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                  {"robot.old.joint_1"});
  for (const auto& [generation, configuration] :
       std::vector<std::pair<std::uint64_t, std::optional<OperatorAlarmAttentionConfigurationDTO>>>{
           {1U, previous}, {2U, std::nullopt}}) {
    ASSERT_TRUE(kernel
                    .PublishOperatorAlarmAttention(attention_snapshot(
                        generation, {fault}, true, "alarm.owner.1", 1U, configuration))
                    .ok());
    const auto refused = kernel.execute(motion_operation("alarm.renamed." +
                                                         std::to_string(generation)));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "operator_alarm_state_unavailable");
    EXPECT_EQ(refused.error().details.at("availability"), "configuration_mismatch");
    EXPECT_EQ(refused.error().details.at("generation"), std::to_string(generation));
    EXPECT_EQ(engine.prepare_motion_calls, 0);
    EXPECT_TRUE(all_slots_free(kernel));
  }

  // The owner reconciled to the Kernel's configuration: a fault naming the
  // renamed robot's current resources gates until acknowledged.
  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      3U, {alarm_member("arm.fault", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto unacknowledged = kernel.execute(motion_operation("alarm.renamed.unacknowledged"));
  ASSERT_FALSE(unacknowledged.ok());
  EXPECT_EQ(unacknowledged.error().code, "unacknowledged_operator_alarms");
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(4U)).ok());
  const auto admitted = kernel.execute(motion_operation("alarm.renamed.acknowledged"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest,
     RefusalMembershipDetailsAreBoundedByWholeEntries) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal, attention_config());
  std::vector<OperatorAlarmAttentionMemberDTO> members;
  for (std::size_t index = 0U; index < kMaximumOperatorAlarmAttentionMembers; ++index) {
    auto id = std::to_string(index);
    id.insert(0U, 4U - id.size(), '0');
    members.push_back(alarm_member("arm.fault." + id + std::string(512U, 'x'),
                                   OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                   {"robot.joint_1"}));
  }
  ASSERT_TRUE(kernel.PublishOperatorAlarmAttention(attention_snapshot(1U, members)).ok());
  const auto refused = kernel.execute(motion_operation("alarm.many"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "unacknowledged_operator_alarms");
  const auto& membership = refused.error().details.at("unacknowledged_alarms");
  EXPECT_LE(membership.size(), kMaximumOperatorAlarmRefusalMembershipBytes);
  const auto conditions = refusal_conditions(refused.error(), "unacknowledged_alarms");
  ASSERT_FALSE(conditions.empty());
  EXPECT_LT(conditions.size(), members.size());
  EXPECT_EQ(conditions.front(), members.front().condition_id);
  EXPECT_EQ(refused.error().details.at("alarm_membership_truncated"), "true");

  ASSERT_TRUE(kernel
                  .PublishOperatorAlarmAttention(attention_snapshot(
                      2U, {alarm_member("arm.fault", OperatorAlarmAttentionGateDTO::kAcknowledgement,
                                        {"robot.joint_1"})}))
                  .ok());
  const auto single = kernel.execute(motion_operation("alarm.single"));
  ASSERT_FALSE(single.ok());
  EXPECT_FALSE(single.error().details.contains("alarm_membership_truncated"));
}

TEST(ResourceAddressedExecutionKernelOperatorAlarmTest, CompositionWithoutAlarmOwnerIsNotGated) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});
  ASSERT_TRUE(kernel.execute(motion_operation("alarm.unconfigured")).ok());
}


// --- Track 1 step 3: participant facts, Stop-path collisions -----------------

[[nodiscard]] ExecutionStatus fact_status(std::uint64_t execution_id, ExecutionState phase,
                                          ParticipantExecutionFact fact) {
  ExecutionStatus status;
  status.state = phase;
  status.execution_id = execution_id;
  status.fact = std::move(fact);
  return status;
}

// The addressed Kernel, not the participant, commits operation outcomes from
// the reported facts. The planning-deadline fact keeps today's Stopped outcome.
TEST(ResourceAddressedExecutionKernelTest, AddressedKernelDerivesOutcomesFromParticipantFacts) {
  struct Case {
    const char* name;
    ExecutionState phase;
    ParticipantExecutionFact fact;
    OperationStateKind expected;
  };
  const ErrorDTO timeout{"planning_timeout", "Planning timed out", {}};
  const ErrorDTO failure{"validation_collision", "collision", {{"reason", "validation_failed"}}};
  const std::vector<Case> cases{
      {"completion", ExecutionState::kExecuting,
       {ParticipantExecutionFactKind::kComputedCompletion,
        ParticipantEffectKnowledge::kEffectSettled, std::nullopt},
       OperationStateKind::kCompleted},
      {"planning_deadline", ExecutionState::kPlanning,
       {ParticipantExecutionFactKind::kPlanningDeadlineExpired, ParticipantEffectKnowledge::kNoEffect,
        timeout},
       OperationStateKind::kStopped},
      {"local_failure", ExecutionState::kValidating,
       {ParticipantExecutionFactKind::kLocalFailure, ParticipantEffectKnowledge::kNoEffect, failure},
       OperationStateKind::kFailed},
      {"uncertain_effect", ExecutionState::kExecuting,
       {ParticipantExecutionFactKind::kLocalFailure, ParticipantEffectKnowledge::kEffectUncertain,
        failure},
       OperationStateKind::kInterventionRequired},
      {"preempted_without_stop", ExecutionState::kExecuting,
       {ParticipantExecutionFactKind::kPreemptedByAddressedStop,
        ParticipantEffectKnowledge::kEffectSettled, std::nullopt},
       OperationStateKind::kInterventionRequired},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    FakeClock clock;
    ManualScheduler scheduler;
    FakeParticipantEngine engine;
    ResourceAddressedExecutionKernel kernel(
        {registration(
            make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
            engine)},
        clock, scheduler, default_journal,
        {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
    const auto operation = kernel.execute(motion_operation(std::string("request.fact.") + item.name));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    {
      std::lock_guard lock(engine.status_mutex_);
      engine.status = fact_status(engine.next_execution_id, item.phase, item.fact);
    }
    KernelEvent settled{KernelEventType::kParticipantSettled,
                        engine.next_execution_id,
                        0,
                        item.phase,
                        item.phase,
                        item.fact.error,
                        {}};
    settled.participant_fact = item.fact;
    engine.deliver_event(settled);
    const auto state = kernel.get_operation_state(operation.value().operation_id);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value().participants.front().state, item.expected);
    // The existing operation aggregate keeps its rule: a failed participant
    // makes the operation InterventionRequired.
    EXPECT_EQ(state.value().state, item.expected == OperationStateKind::kFailed
                                       ? OperationStateKind::kInterventionRequired
                                       : item.expected);
    if (item.fact.error.has_value()) {
      ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
      EXPECT_EQ(*state.value().participants.front().terminal_error, *item.fact.error);
    }
  }
}

// Parity with the former participant Stop workers: a collision observed while
// a Stop is braking on-path aborts that stage at once (no stage deadline),
// engages the strongest authorized stage and ends InterventionRequired with
// the collision as the participant's cause.
TEST(ResourceAddressedExecutionKernelTest,
     CollisionDuringOnPathStopEscalatesToStrongestStageAndEndsInterventionRequired) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  HeldSettlementCall on_path;
  engine.translate_stop_stage_hook = [hold = on_path.Blocker(),
                                      calls = std::make_shared<std::atomic<unsigned>>(0)] {
    if (calls->fetch_add(1U) == 0U) hold();
  };
  const auto operation = kernel.execute(motion_operation("request.stop-path-collision"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-path-collision.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(on_path.Entered());
  ASSERT_TRUE(engine.last_stop_stage.has_value());
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.participant_stop_profile->stop_mode,
            MotionInterruptStopMode::kSoftOnPath);

  // No owner-clock advance: escalation is immediate, not the stage deadline.
  engine.emit_collision({{"contact_a", "component/robot/body/tool/collision/0"},
                         {"contact_b", "scene/fixture/shape/0"}});
  ASSERT_TRUE(wait_until([&] { return engine.translate_stop_stage_calls.load() >= 2; },
                         std::chrono::seconds(4)));
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.stage_commands
                .at(engine.last_stop_stage->stage_index)
                .resolved_stage.stage_kind,
            StopEscalationStageKind::kSoftwareHard);
  on_path.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto status = kernel.get_stop_status(stopped.value().stop_id).value();
  const auto certificate =
      kernel.get_stop_certificate(status.stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok());
  EXPECT_EQ(certificate.value().result, StopCertificateResultDTO::kSucceededEscalated);
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_EQ(latch->trigger, StopFaultTriggerDTO::kEscalated);
  const auto state = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  const auto& participant = state.value().participants.front();
  EXPECT_EQ(participant.state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(participant.terminal_error.has_value());
  EXPECT_EQ(participant.terminal_error->details.at("reason"), "stop_path_collision");
  EXPECT_EQ(participant.terminal_error->details.at("contact_a"),
            "component/robot/body/tool/collision/0");
  EXPECT_EQ(participant.terminal_error->details.at("contact_b"), "scene/fixture/shape/0");
}

// The same applies while the runtime-collision containment brakes through
// its spring-back replacement: a further collision on that path engages the
// strongest stage at once and the operation ends InterventionRequired.
TEST(ResourceAddressedExecutionKernelTest,
     CollisionDuringSpringBackReplacementEscalatesToStrongestStageAndEndsInterventionRequired) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  HeldSettlementCall spring_back;
  engine.translate_stop_stage_hook = [hold = spring_back.Blocker(),
                                      calls = std::make_shared<std::atomic<unsigned>>(0)] {
    if (calls->fetch_add(1U) == 0U) hold();
  };
  const auto operation = kernel.execute(motion_operation("request.spring-back-collision"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  engine.emit_collision({{"contact_a", "component/robot/body/link4/collision/0"},
                         {"contact_b", "scene/wall/shape/0"}});
  ASSERT_TRUE(spring_back.Entered());
  ASSERT_TRUE(engine.last_stop_stage.has_value());
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.participant_stop_profile->stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  const auto stop_id = kernel.get_operation_state(operation.value().operation_id)
                           .value()
                           .active_stop_id;
  ASSERT_TRUE(stop_id.has_value());

  engine.emit_collision({{"contact_a", "component/robot/body/link5/collision/0"},
                         {"contact_b", "scene/wall/shape/0"}});
  ASSERT_TRUE(wait_until([&] { return engine.translate_stop_stage_calls.load() >= 2; },
                         std::chrono::seconds(4)));
  EXPECT_EQ(engine.last_stop_stage->participant_stop_plan.stage_commands
                .at(engine.last_stop_stage->stage_index)
                .resolved_stage.stage_kind,
            StopEscalationStageKind::kSoftwareHard);
  spring_back.Release();
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(*stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto state = kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(state.value().participants.front().terminal_error->details.at("reason"),
            "stop_path_collision");
  EXPECT_EQ(state.value().participants.front().terminal_error->details.at("contact_a"),
            "component/robot/body/link5/collision/0");
}

// A collision fact after the Stop has committed its terminal is evidence only:
// it neither reopens that Stop nor opens another one.
TEST(ResourceAddressedExecutionKernelTest, CollisionAfterTerminalStopDoesNotReopenIt) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, scheduler, default_journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation("request.late-collision"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.late-collision.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().state == StopStatusStateDTO::kSucceeded;
  }));
  const auto before = kernel.get_operation_state(operation.value().operation_id).value();
  ASSERT_EQ(before.state, OperationStateKind::kStopped);
  const auto stage_calls = engine.translate_stop_stage_calls.load();

  engine.emit_collision({{"contact_a", "component/robot/body/tool/collision/0"},
                         {"contact_b", "scene/workpiece/shape/0"}});
  scheduler.advance_to(clock.now());
  const auto after = kernel.get_operation_state(operation.value().operation_id).value();
  EXPECT_EQ(after.state, OperationStateKind::kStopped);
  EXPECT_EQ(after.operation_revision, before.operation_revision);
  EXPECT_EQ(after.active_stop_id, before.active_stop_id);
  EXPECT_EQ(after.participants.front().terminal_error, before.participants.front().terminal_error);
  EXPECT_EQ(engine.translate_stop_stage_calls.load(), stage_calls);
  EXPECT_EQ(kernel.get_stop_status(stopped.value().stop_id).value().state,
            StopStatusStateDTO::kSucceeded);
  EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
}

}  // namespace
