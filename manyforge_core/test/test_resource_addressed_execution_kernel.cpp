#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::core;

template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

class FakeParticipantEngine final : public IResolvedParticipantExecutionEngine,
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

  Result<std::uint64_t> execute(const ExecuteRequestDTO& request) override {
    ++execute_calls;
    Trace("engine:execute");
    last_execute = request;
    if (request.initial_runtime_scale.has_value()) {
      const auto initialized = set_runtime_scale(*request.initial_runtime_scale);
      if (!initialized.ok()) {
        return Result<std::uint64_t>::Err(initialized.error());
      }
    }
    status.state = ExecutionState::kPlanning;
    status.execution_id = next_execution_id;
    if (event_during_execute.has_value()) {
      const auto snapshot = callbacks;
      for (const auto& [id, callback] : snapshot) {
        (void)id;
        callback(*event_during_execute);
      }
      event_during_execute.reset();
    }
    return next_execute_result;
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

  StopResultDTO stop() override {
    ++stop_calls;
    Trace("engine:stop");
    return next_stop_result;
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
          next_stop_result.accepted ? std::optional<JsonValueDTO>(JsonValueDTO{true})
                                    : std::nullopt,
          next_stop_result.accepted ? std::nullopt
                                    : std::optional(next_stop_result.error.value_or(
                                          ErrorDTO{"controller_stop_rejected",
                                                   "controller rejected Stop",
                                                   {{"reason", "controller_stop_rejected"}}}))});
    }
    ++stop_calls;
    auto result = ParticipantStopStageResultDTO{stage,
                                                StopStageAttemptDispositionDTO::kDispatched,
                                                next_stop_result.accepted
                                                    ? ParticipantStopStageOutcomeDTO::kSucceeded
                                                    : ParticipantStopStageOutcomeDTO::kFailed,
                                                MotionControlledStopMode::kSoftOnPath,
                                                std::nullopt,
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
  PauseResultDTO pause() override {
    ++pause_calls;
    Trace("engine:pause");
    if (pause_effect_hook) {
      pause_effect_hook();
    }
    if (next_pause_result.accepted) {
      emit_transition(status.state, ExecutionState::kStopping);
    }
    return next_pause_result;
  }
  ResumeResultDTO resume(std::optional<std::uint64_t>) override {
    ++resume_calls;
    Trace("engine:resume");
    if (next_resume_result.accepted) {
      emit_transition(status.state, ExecutionState::kValidating);
    }
    return next_resume_result;
  }
  ResetResultDTO reset(std::optional<std::uint64_t>) override {
    ++reset_calls;
    Trace("engine:reset");
    if (next_reset_result.accepted) {
      emit_transition(status.state, ExecutionState::kIdle);
    }
    return next_reset_result;
  }
  AcknowledgeCompletedResultDTO acknowledge_completed(std::uint64_t,
                                                      std::optional<std::uint64_t>) override {
    ++acknowledge_calls;
    Trace("engine:acknowledge_completed");
    if (next_acknowledge_result.accepted) {
      emit_transition(status.state, ExecutionState::kIdle);
    }
    return next_acknowledge_result;
  }

  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) override {
    ++runtime_scale_calls;
    Trace("engine:set_runtime_scale");
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

  ExecutionStatus get_status() const override { return status; }

  SubscriptionHandle subscribe_events(EventCallback callback) override {
    const auto id = next_subscription_id++;
    callbacks.emplace(id, std::move(callback));
    return SubscriptionHandle{id};
  }

  void unsubscribe_events(SubscriptionHandle handle) override { callbacks.erase(handle.id); }

  [[nodiscard]] std::size_t subscription_count() const noexcept { return callbacks.size(); }

  void emit_transition(ExecutionState from, ExecutionState to,
                       std::optional<ErrorDTO> error = std::nullopt) {
    status.state = to;
    runtime_scale.active = to == ExecutionState::kExecuting || to == ExecutionState::kPaused;
    status.progress = to == ExecutionState::kCompleted ? 1.0 : 0.4;
    if (error.has_value()) {
      status.last_error = ErrorCode::kExecutionAborted;
    }
    const KernelEvent event{
        KernelEventType::kStateTransition, next_execution_id, 0, from, to, std::move(error), {}};
    const auto snapshot = callbacks;
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  void emit_error(ErrorDTO error) {
    status.last_error = ErrorCode::kExecutionAborted;
    const KernelEvent event{KernelEventType::kErrorRaised,
                            next_execution_id,
                            0,
                            status.state,
                            status.state,
                            std::move(error),
                            {}};
    const auto snapshot = callbacks;
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  int execute_calls{};
  int preview_calls{};
  int command_calls{};
  mutable int validate_command_calls{};
  int stop_gripper_calls{};
  int reset_gripper_calls{};
  std::atomic<int> stop_calls{};
  std::atomic<int> translate_stop_stage_calls{};
  int pause_calls{};
  int resume_calls{};
  int reset_calls{};
  int acknowledge_calls{};
  int runtime_scale_calls{};
  std::uint64_t next_execution_id{41U};
  Result<std::uint64_t> next_execute_result = Result<std::uint64_t>::Ok(41U);
  GripperCommandResultDTO next_command_result{true, 73U, 9U, std::nullopt};
  Result<void> next_command_validation = Result<void>::Ok();
  StopResultDTO next_stop_result{true, std::nullopt};
  PauseResultDTO next_pause_result{true, std::nullopt};
  ResumeResultDTO next_resume_result{true, std::nullopt};
  ResetResultDTO next_reset_result{true, std::nullopt};
  AcknowledgeCompletedResultDTO next_acknowledge_result{true, std::nullopt};
  Result<void> next_runtime_scale_result = Result<void>::Ok();
  RuntimeScaleMechanismStatusDTO runtime_scale{true, false, 1.0, 1.0, 0.0, 1U, 1, std::nullopt};
  ExecutionStatus status{};
  std::optional<KernelEvent> event_during_execute;
  std::function<void()> pause_effect_hook;
  std::function<void()> translate_stop_stage_hook;
  GripperCommandStatusDTO gripper_status{};
  std::optional<ExecuteRequestDTO> last_execute;
  std::optional<ExecuteRequestDTO> last_preview;
  std::optional<ResolvedRuntimeScaleCommandDTO> last_runtime_scale_command;
  std::optional<GripperCommandRequestDTO> last_command;
  std::optional<ResolvedParticipantStopStageDTO> last_stop_stage;
  std::vector<std::string>* timeline{};

 private:
  void Trace(std::string value) {
    if (timeline != nullptr) {
      timeline->push_back(std::move(value));
    }
  }

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

  Result<std::uint64_t> RecordOperationAccepted(const RequestOwnerEpoch&, const OperationIntentDTO&,
                                                const OperationAcceptedDTO&,
                                                const OperationStateDTO& state) override {
    if (operation_acceptance_hook) {
      operation_acceptance_hook();
    }
    return Record("operation_accepted", state);
  }

  Result<std::uint64_t> RecordControlAccepted(const RequestOwnerEpoch&,
                                              const OperationControlRequestDTO&,
                                              const OperationControlAcceptedDTO&,
                                              const OperationStateDTO& state) override {
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
      const StopFaultLatchDTO& fault_latch) noexcept override {
    std::lock_guard lock(mutex);
    calls.push_back("stop_surveillance_checkpoint");
    last_stop_surveillance = checkpoint;
    last_stop_fault_latch = fault_latch;
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

  Result<std::uint64_t> RecordOperationState(const RequestOwnerEpoch&,
                                             const OperationStateDTO& state,
                                             const RuntimeScaleStatusDTO*,
                                             const char* transition_reason) override {
    return Record(std::string("state:") + transition_reason, state);
  }

  Result<std::uint64_t> Record(std::string kind, const OperationStateDTO& state) {
    std::lock_guard lock(mutex);
    calls.push_back(kind);
    if (timeline != nullptr) {
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
  std::vector<std::string> calls;
  std::atomic<int> stop_acceptance_calls{};
  std::atomic<int> stop_terminal_calls{};
  std::atomic<int> stop_surveillance_calls{};
  std::atomic<int> stop_fault_clear_calls{};
  std::vector<OperationStateDTO> states;
  std::optional<EmergencyStopAcceptanceRecordDTO> last_stop_acceptance;
  std::optional<EmergencyStopProjectedIntentDTO> last_stop_intent;
  std::optional<StopAcceptedDTO> last_stop_accepted;
  std::optional<EmergencyStopAffectedStateSnapshotDTO> last_stop_affected_state;
  std::optional<StopStatusDTO> last_stop_status;
  std::optional<StopCertificateDTO> last_stop_certificate;
  std::optional<StopFaultLatchDTO> last_stop_fault_latch;
  std::optional<StopPostTerminalSurveillanceDTO> last_stop_surveillance;
  std::optional<StopFaultRecoveryIntentDTO> last_stop_recovery_intent;
  std::optional<StopFaultRecoveryResultDTO> last_stop_recovery_result;
  std::function<void()> operation_acceptance_hook;
  std::function<void()> stop_acceptance_hook;
  std::vector<StopId> projected_stop_acceptances;
  std::vector<StopId> projected_stop_terminals;
  std::uint64_t next_sequence{1U};
  std::vector<std::string>* timeline{};
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

  bool reject_reservation{true};
  int reserve_calls{};

 private:
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
};

FakeOperationJournal default_journal;

[[nodiscard]] std::string test_digest(const char digit) {
  return "sha256:" + std::string(64U, digit);
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
    const RuntimeParticipantBinding& participant, const ResolvedStopBasePlanDTO& plan) {
  const auto& binding = participant.value();
  const auto& control = *binding.control_path;
  std::vector<ControllerInterfaceInstanceDTO> interfaces;
  interfaces.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    interfaces.push_back(ControllerInterfaceInstanceDTO{resource.resource_id, "position"});
  }
  const ControllerRoleId controller_role("hardware");
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
       {{control.control_binding_id, control.control_binding_revision, control.control_route_id}}},
      {{control.control_binding_id, control.control_binding_revision,
        control.control_interface_contract_id, control.control_interface_revision, interfaces,
        interfaces}},
      {{controller_role,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(test_digest('4')),
        "controller_interface.ControllerInterface",
        ContentDigest(test_digest('5')),
        interfaces,
        interfaces,
        {},
        {}}},
      {{control.control_binding_id, control.control_binding_revision, control.control_route_id,
        EndpointRoleId("endpoint.command"), "controller.command", true, false}}};
  auto sealed = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantExecutionRegistration registration(
    const RuntimeParticipantBinding& participant, IResolvedParticipantExecutionEngine& engine) {
  auto plan = stop_base_plan_for(participant);
  auto expected = controller_domain_for(participant, plan);
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
  auto runtime_policy = verify_runtime_participant_policy_selection(
      RuntimePolicyDTO{RuntimePolicyId("runtime.test"),
                       RuntimePolicyRevision(test_digest('d')),
                       std::nullopt,
                       {},
                       {RuntimeMotionSpeedLimitDTO{RuntimePolicyCellScopeDTO{}, 0.5}},
                       {}},
      participant);
  if (!runtime_policy.ok()) {
    throw std::logic_error(runtime_policy.error().message + ": " +
                           runtime_policy.error().details.at("reason"));
  }
  return RuntimeParticipantExecutionRegistration{
      participant,
      std::move(readiness.value()),
      std::move(limits),
      MotionInterruptNodeCategory::kGeneric,
      &engine,
      dynamic_cast<IControlDomainExecutionPort*>(&engine),
      std::move(runtime_policy.value())};
}

[[nodiscard]] RuntimeParticipantExecutionRegistration unqualified_registration(
    const RuntimeParticipantBinding& participant, IResolvedParticipantExecutionEngine& engine) {
  return RuntimeParticipantExecutionRegistration{
      participant, std::nullopt,
      {},          MotionInterruptNodeCategory::kGeneric,
      &engine,     dynamic_cast<IControlDomainExecutionPort*>(&engine)};
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

[[nodiscard]] OperationIntentDTO candidate_motion_operation(
    std::string request_id = "request.motion.candidate") {
  auto operation = motion_operation(std::move(request_id));
  operation.program_revision = ProgramRevision(test_digest('2'));
  operation.program_binding_id = ProgramBindingId(test_digest('3'));
  operation.expected_retained_bundle_id = RetainedBundleId(test_digest('e'));
  operation.expected_graph_revision = GraphRevision(test_digest('f'));
  return operation;
}

[[nodiscard]] OperationIntentDTO command_operation() {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  auto operation = motion_operation("request.command.1");
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

TEST(ResourceAddressedExecutionKernelTest, ResolvesPortableMotionAndReservesExactResource) {
  FakeClock clock;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_TRUE(result.ok()) << result.error().message << ": "
                           << (result.error().details.contains("reason")
                                   ? result.error().details.at("reason")
                                   : "");
  EXPECT_EQ(result.value().accepted_at_ns, 1234);
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
  EXPECT_EQ(slots.front().state, ResourceSlotState::kReserved);

  const auto state = kernel.get_operation_state(result.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kPlanning);
  EXPECT_EQ(state.value().participants.front().command_target_id.value(), "arm");
}

TEST(ResourceAddressedExecutionKernelTest,
     DisableHandoffRuntimeSnapshotResolvesExactStaleAndConservativeFullScopes) {
  FakeClock clock;
  clock.set(1234);
  FakeParticipantEngine engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, default_journal,
      ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"),
                                             CellId("cell.test"), 8U});

  const auto component_id = binding.value().controllable_component_id;
  const DisableHandoffAdmissionQueryDTO full_query{
      component_id, DisableHandoffScopeRequestKindDTO::kFullStopRequired, std::nullopt};
  const auto full = kernel.capture_disable_handoff_runtime_snapshot(full_query);
  ASSERT_TRUE(full.ok()) << full.error().message;
  EXPECT_EQ(full.value().scope_resolution, DisableHandoffScopeResolutionDTO::kPromotedToFull);
  EXPECT_EQ(full.value().current_scope.controllable_component_ids,
            std::vector<ControllableComponentId>{component_id});
  EXPECT_EQ(full.value().current_cell_stop_generation, StopGeneration(0U));
  EXPECT_TRUE(full.value().affected_queued_operations.empty());
  EXPECT_EQ(full.value().affected_active_operation_count, 0U);

  DisableHandoffComponentScopeExpectationDTO expectation{
      ContentDigest("component-snapshot-1"),
      ActiveBundlePublicationGeneration(1U),
      ContentDigest("public-pointer-1"),
      binding.value().retained_bundle_id,
      binding.value().graph_revision,
      ComponentLifecycleGeneration(1U),
      full.value().current_scope,
      full.value().current_active_control_configurations};
  DisableHandoffAdmissionQueryDTO exact_query{
      component_id, DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation};
  const auto exact = kernel.capture_disable_handoff_runtime_snapshot(exact_query);
  ASSERT_TRUE(exact.ok()) << exact.error().message;
  EXPECT_EQ(exact.value().scope_resolution, DisableHandoffScopeResolutionDTO::kExact);
  ASSERT_TRUE(exact.value().current_component_lifecycle_generation.has_value());
  EXPECT_EQ(*exact.value().current_component_lifecycle_generation,
            ComponentLifecycleGeneration(1U));

  DisableHandoffRequestDTO persisted_request{
      {RequestId("request.disable.1"), RequestOwnerEpoch("kernel.epoch.1"), "cclc", std::nullopt},
      RequestId("request.public.disable.1"),
      LifecycleTransactionId("lifecycle.disable.1"),
      LifecycleTransactionGeneration(1U),
      component_id,
      exact.value().scope_resolution,
      exact.value().current_scope,
      ContentDigest("disable-admission-snapshot-1"),
      ActiveBundlePublicationGeneration(1U),
      ContentDigest("public-pointer-1"),
      exact.value().graph_revision,
      exact.value().retained_bundle_id,
      *exact.value().current_component_lifecycle_generation,
      exact.value().operation_resource_generation,
      ConfigurationFenceGeneration(0U),
      exact.value().current_active_control_configurations};
  ASSERT_TRUE(persisted_request.locally_valid());
  const auto revalidated = kernel.revalidate_disable_handoff_request(persisted_request);
  ASSERT_TRUE(revalidated.ok()) << revalidated.error().message;
  EXPECT_EQ(revalidated.value().scope_resolution, DisableHandoffScopeResolutionDTO::kExact);

  persisted_request.expected_component_lifecycle_generation = ComponentLifecycleGeneration(2U);
  const auto revalidated_stale = kernel.revalidate_disable_handoff_request(persisted_request);
  ASSERT_TRUE(revalidated_stale.ok()) << revalidated_stale.error().message;
  EXPECT_EQ(revalidated_stale.value().scope_resolution,
            DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved);

  exact_query.component_expectation->component_lifecycle_generation =
      ComponentLifecycleGeneration(2U);
  const auto stale = kernel.capture_disable_handoff_runtime_snapshot(exact_query);
  ASSERT_TRUE(stale.ok()) << stale.error().message;
  EXPECT_EQ(stale.value().scope_resolution,
            DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved);

  exact_query.controllable_component_id = ControllableComponentId("unknown.component");
  exact_query.component_expectation->component_scope.controllable_component_ids = {
      exact_query.controllable_component_id};
  const auto promoted = kernel.capture_disable_handoff_runtime_snapshot(exact_query);
  ASSERT_TRUE(promoted.ok()) << promoted.error().message;
  EXPECT_EQ(promoted.value().scope_resolution, DisableHandoffScopeResolutionDTO::kPromotedToFull);
  EXPECT_EQ(promoted.value().current_scope, full.value().current_scope);
}

TEST(ResourceAddressedExecutionKernelTest,
     RetainsUnqualifiedRegistrationForObservationButRefusesMotionAdmission) {
  FakeClock clock;
  FakeParticipantEngine engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto unqualified = registration(binding, engine);
  unqualified.stop_readiness.reset();
  unqualified.stop_execution_port = nullptr;

  ResourceAddressedExecutionKernel kernel(
      {std::move(unqualified)}, clock, default_journal,
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
     RefusesMotionWhenStopReadinessExistsButRuntimePolicyIsUnqualified) {
  FakeClock clock;
  FakeParticipantEngine engine;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  auto unqualified = registration(binding, engine);
  unqualified.runtime_policy.reset();

  ResourceAddressedExecutionKernel kernel(
      {std::move(unqualified)}, clock, default_journal,
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
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

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
  const auto& resolved_goal = std::get<ResolvedJointGoalDTO>(second_engine.last_execute->motion);
  EXPECT_EQ(resolved_goal.joint_names, (std::vector<std::string>{"mf2_robot_2_joint_1"}));

  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto first_still_planning = kernel.get_operation_state(first.value().operation_id);
  const auto second_executing = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(first_still_planning.ok()) << first_still_planning.error().message;
  ASSERT_TRUE(second_executing.ok()) << second_executing.error().message;
  EXPECT_EQ(first_still_planning.value().state, OperationStateKind::kPlanning);
  EXPECT_EQ(second_executing.value().state, OperationStateKind::kExecuting);

  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged =
      kernel.acknowledge_completed(control_request("request.robot.1.ack", first.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
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
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto overlapping_binding = second_occurrence_sharing_first_resource(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(overlapping_binding, second_engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

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
  EXPECT_EQ(first_state.value().state, OperationStateKind::kPlanning);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_id, first.value().operation_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     OneSingleParticipantEngineCannotOwnTwoResidentOperations) {
  FakeClock clock;
  FakeParticipantEngine engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, engine), registration(second_binding, engine)}, clock,
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
  EXPECT_EQ(first_slot->state, ResourceSlotState::kReserved);
  EXPECT_EQ(second_slot->state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     LatePriorExecutionEventCannotAttachToANewOperationDuringDispatch) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto first = kernel.execute(motion_operation("request.late-event.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.late-event.first.ack", first.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;

  engine.next_execution_id = 42U;
  engine.next_execute_result = Result<std::uint64_t>::Ok(42U);
  engine.event_during_execute =
      KernelEvent{KernelEventType::kStateTransition, 41U,          0, ExecutionState::kPlanning,
                  ExecutionState::kCompleted,        std::nullopt, {}};
  const auto second = kernel.execute(motion_operation("request.late-event.second"));
  ASSERT_TRUE(second.ok()) << second.error().message;
  const auto after_late_event = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(after_late_event.ok()) << after_late_event.error().message;
  EXPECT_EQ(after_late_event.value().state, OperationStateKind::kPlanning);

  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto after_exact_event = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(after_exact_event.ok()) << after_exact_event.error().message;
  EXPECT_EQ(after_exact_event.value().state, OperationStateKind::kExecuting);
}

TEST(ResourceAddressedExecutionKernelTest,
     ControlsStopAndRuntimeScaleRemainScopedToTheNamedDisjointOperation) {
  FakeClock clock;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto first = kernel.execute(motion_operation("request.scoped.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.scoped.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  const auto paused =
      kernel.pause(control_request("request.scoped.pause.first", first.value(), 1U));
  ASSERT_TRUE(paused.ok()) << paused.error().message;
  EXPECT_EQ(first_engine.pause_calls, 1);
  EXPECT_EQ(second_engine.pause_calls, 0);
  const auto second_after_pause = kernel.get_operation_state(second.value().operation_id);
  ASSERT_TRUE(second_after_pause.ok()) << second_after_pause.error().message;
  EXPECT_EQ(second_after_pause.value().state, OperationStateKind::kExecuting);

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
  EXPECT_EQ(first_after_second_stop.value().operation_revision, OperationRevision(2U));
  EXPECT_TRUE(second_after_stop.value().state == OperationStateKind::kStopping ||
              second_after_stop.value().state == OperationStateKind::kStopped);
}

TEST(ResourceAddressedExecutionKernelTest, BlockedControlOnOneOperationCannotDelayDisjointStop) {
  using namespace std::chrono_literals;

  FakeClock clock;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

  const auto first = kernel.execute(motion_operation("request.concurrent-control.first"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.concurrent-control.second");
  second_intent.program_binding_id = second_binding.value().program_binding_id;
  const auto second = kernel.execute(second_intent);
  ASSERT_TRUE(second.ok()) << second.error().message;
  first_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  second_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);

  std::promise<void> pause_entered_promise;
  auto pause_entered = pause_entered_promise.get_future();
  std::promise<void> release_pause_promise;
  auto release_pause = release_pause_promise.get_future().share();
  first_engine.pause_effect_hook = [&] {
    pause_entered_promise.set_value();
    release_pause.wait();
  };
  auto paused = std::async(std::launch::async, [&] {
    return kernel.pause(control_request("request.concurrent-control.pause", first.value(), 1U));
  });
  const auto pause_started = pause_entered.wait_for(1s);
  if (pause_started != std::future_status::ready) {
    release_pause_promise.set_value();
    (void)paused.get();
    FAIL() << "the injected Pause effect did not start";
  }

  auto stopped = std::async(std::launch::async, [&] {
    return kernel.stop(stop_request("request.concurrent-control.stop", second.value(), 1U));
  });
  const auto stop_status = stopped.wait_for(250ms);
  release_pause_promise.set_value();
  const auto pause_result = paused.get();
  ASSERT_TRUE(pause_result.ok()) << pause_result.error().message;
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation("request.stop-terminal-owner"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto stopped =
      kernel.stop(stop_request("request.stop-terminal-owner.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;

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
  EXPECT_FALSE(stop_status_field_error(terminal_status.value()).has_value());
  ASSERT_TRUE(terminal_status.value().stop_certificate.has_value());
  const auto certificate =
      kernel.get_stop_certificate(terminal_status.value().stop_certificate->stop_certificate_id);
  ASSERT_TRUE(certificate.ok()) << certificate.error().message;
  EXPECT_FALSE(stop_certificate_field_error(certificate.value()).has_value());
  EXPECT_EQ(certificate.value().stop_id, stopped.value().stop_id);
  EXPECT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() &&
           status.value().terminal_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));
  EXPECT_EQ(kernel.get_stop_fault_latch().has_value(),
            certificate.value().result != StopCertificateResultDTO::kSucceededNominal);
}

TEST(ResourceAddressedExecutionKernelTest, UsesOneInjectedGateForTheActualSlotTableReservation) {
  FakeClock clock;
  FakeParticipantEngine engine;
  FakeResourceAdmissionGate gate;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal,
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
  EXPECT_EQ(admitted.operation_resource_generation, 1U);
  EXPECT_EQ(admitted.affected_queued_operation_count, 1U);
  EXPECT_EQ(admitted.affected_active_operation_count, 0U);
}

TEST(ResourceAddressedExecutionKernelTest,
     ProductBackedResolverAdmitsAnExactProgramBindingOnPinnedPhysicalEngine) {
  FakeClock clock;
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
      {registration(physical, engine)}, clock, default_journal,
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
                       true),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionAllowsMotionSpecificPlannerProvidersOnOnePinnedParticipant) {
  FakeClock clock;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto physical =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto pose = with_planner_capability(physical, "planning.motion.pose_goal");
  const auto cartesian = with_planner_capability(physical, "planning.motion.cartesian_path");
  ResourceAddressedExecutionKernel kernel(
      {registration(pose, engine)}, clock, journal,
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

TEST(ResourceAddressedExecutionKernelTest,
     PreviewSessionRejectsAPlannerResolutionThatChangesThePinnedEngine) {
  FakeClock clock;
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
      {registration(pose, primary_engine), registration(unrelated, other_engine)}, clock, journal,
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
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
                       true),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  std::get<MotionParticipantIntentDTO>(intent.participants.front().payload).contact_policy_id =
      ContactPolicyId("selected_object_grasp");

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "contact_grasp_runtime_object_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, RejectsResourceRoleSpellingUsedAsASourceJointName) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                       std::nullopt, false,
                       "sha256:1111111111111111111111111111111111111111111111111111111111111111"),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_planner_capability_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, PreservesAuthoredSourceJointOrderingDuringResolution) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                 CoordinateKind::kAngular, std::nullopt, true),
                    engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("arm", "arm", "execution.trajectory.execute",
                                 CoordinateKind::kAngular, std::nullopt, true),
                    engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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

TEST(ResourceAddressedExecutionKernelTest, SubordinateEventsAdvanceExactSlotsAndOperationState) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
     PreservesTheExactErrorRaisedBeforeASeparateTerminalTransition) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  auto intent = motion_operation();
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "stale_request_owner_epoch");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     LinearGripperCommandUsesResolvedTargetAndReconcilesCompletion) {
  FakeClock clock;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

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
  EXPECT_EQ(engine.validate_command_calls, 1);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);

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
     CapabilityCommandPreviewValidatesWithoutReservationJournalOrDispatch) {
  FakeClock clock;
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
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

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
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  FakeOperationJournal journal;
  ResourceAddressedExecutionKernel kernel(
      {registration(make_binding("gripper", "gripper.target", "tool.gripper.command",
                                 CoordinateKind::kLinear, schema),
                    engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});

  const auto preview = kernel.preview_capability_command(command_operation());
  ASSERT_FALSE(preview.ok());
  EXPECT_EQ(preview.error().details.at("reason"), "gripper_busy");
  EXPECT_EQ(engine.command_calls, 0);
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(journal.calls, (std::vector<std::string>{"begin"}));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     GripperValidationPrecedesAcceptanceAndScopedStopResetUseCommandLifecycle) {
  FakeClock clock;
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
      clock, rejected_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
  EXPECT_EQ(engine.reset_gripper_calls, 1);
  EXPECT_EQ(engine.reset_calls, 0);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     OperationControlsAdvanceRevisionAndAcknowledgeReleasesResources) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto paused = kernel.pause(control_request("request.pause.1", executed.value(), 1U));
  ASSERT_TRUE(paused.ok()) << paused.error().message;
  EXPECT_EQ(paused.value().operation_revision_after_acceptance, OperationRevision(2U));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  engine.emit_transition(ExecutionState::kStopping, ExecutionState::kPaused);
  auto paused_state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(paused_state.ok());
  EXPECT_EQ(paused_state.value().state, OperationStateKind::kPaused);
  EXPECT_EQ(paused_state.value().operation_revision, OperationRevision(2U));

  const auto resumed = kernel.resume(control_request("request.resume.1", executed.value(), 2U));
  ASSERT_TRUE(resumed.ok()) << resumed.error().message;
  EXPECT_EQ(resumed.value().operation_revision_after_acceptance, OperationRevision(3U));
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);

  const auto acknowledged =
      kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 3U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  EXPECT_EQ(acknowledged.value().operation_revision_after_acceptance, OperationRevision(4U));
  EXPECT_EQ(engine.pause_calls, 1);
  EXPECT_EQ(engine.resume_calls, 1);
  EXPECT_EQ(engine.acknowledge_calls, 1);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
  const auto retained = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  EXPECT_EQ(retained.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(retained.value().operation_revision, OperationRevision(4U));

  const auto replay =
      kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 3U));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.acknowledge_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ResetSettlesFailedOperationAndRejectsStaleRevisionWithoutEffect) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed,
                         ErrorDTO{"planning_failed", "no path", {{"reason", "no_path"}}});

  const auto stale = kernel.reset(control_request("request.reset.stale", executed.value(), 2U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "operation_revision_stale");
  EXPECT_EQ(engine.reset_calls, 0);

  const auto reset = kernel.reset(control_request("request.reset.1", executed.value(), 1U));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  EXPECT_EQ(engine.reset_calls, 1);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     InitializesScalingAtomicallyWithTheExactResolvedExecutionBinding) {
  FakeClock clock;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});

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
     RuntimeScaleIsGenerationFencedReplaySafeAndZeroIsNotStop) {
  FakeClock clock;
  clock.set(10);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
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
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
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
  FakeParticipantEngine engine;
  engine.next_stop_result =
      StopResultDTO{false, ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                                    "controller rejected Stop",
                                    {{"reason", "controller_stop_rejected"}}}};
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
  EXPECT_EQ(engine.last_stop_stage->stage_index, 1U);
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
     BlockedNominalStageCannotDelayPreparedStrongerStageWatchdog) {
  FakeClock clock;
  FakeParticipantEngine engine;
  std::promise<void> nominal_entered_promise;
  auto nominal_entered = nominal_entered_promise.get_future();
  std::promise<void> release_nominal_promise;
  const auto release_nominal = release_nominal_promise.get_future().share();
  std::atomic<std::size_t> calls{};
  engine.translate_stop_stage_hook = [&] {
    if (calls.fetch_add(1U) == 0U) {
      nominal_entered_promise.set_value();
      release_nominal.wait();
    }
  };
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
  const auto operation = kernel.execute(motion_operation("request.blocked-stage.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.blocked-stage.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  const auto entered = nominal_entered.wait_for(std::chrono::seconds(10));
  if (entered != std::future_status::ready) {
    release_nominal_promise.set_value();
    FAIL() << "nominal Stop stage did not reach its injected blocking seam";
  }

  // The nominal stage's evidence deadline is two seconds in the retained
  // fixture. The StopControl watchdog observes the owner clock and advances
  // to the already-prepared software-hard stage without waiting for the
  // blocked nominal provider lane to return.
  clock.advance(2'100'000'000);
  const bool stronger_dispatched = wait_until(
      [&] { return engine.translate_stop_stage_calls.load() >= 2; }, std::chrono::seconds(4));
  release_nominal_promise.set_value();
  ASSERT_TRUE(stronger_dispatched)
      << "blocked nominal provider delayed the stronger prepared stage";
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return status.ok() && status.value().stop_certificate.has_value();
  }));
  const auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_TRUE(latch->trigger == StopFaultTriggerDTO::kEscalated ||
              latch->trigger == StopFaultTriggerDTO::kFailedUncontained ||
              latch->trigger == StopFaultTriggerDTO::kPartial);
}

TEST(ResourceAddressedExecutionKernelTest,
     ConcurrentDisjointStopsMintUniqueMonotonicCellIdentities) {
  FakeClock clock;
  clock.set(500);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});

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
     ExplicitFullAggregatesOneResidentStopAndFreshScopeWithoutRefenceOrRedispatch) {
  FakeClock clock;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
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
      predecessor.value().stop_generation,
      {}};
  const auto aggregate = kernel.full_stop(full_intent);
  if (!aggregate.ok()) {
    release_first_stage_promise.set_value();
    FAIL() << aggregate.error().message;
  }
  EXPECT_EQ(aggregate.value().resolution_disposition, StopResolutionDispositionDTO::kExact);
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
}

TEST(ResourceAddressedExecutionKernelTest,
     OrdinaryScopeCrossingResidentAndFreshWorkPromotesToOneFullAggregate) {
  FakeClock clock;
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
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
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto first = kernel.execute(motion_operation("request.aggregate-two.first.execute"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  auto second_intent = motion_operation("request.aggregate-two.second.execute");
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
  const auto first_stop =
      kernel.stop(stop_request("request.aggregate-two.first", first.value(), 1U));
  ASSERT_TRUE(first_stop.ok()) << first_stop.error().message;
  const auto entered_status = first_stage_entered.wait_for(std::chrono::seconds(10));
  if (entered_status != std::future_status::ready) {
    release_first_stage_promise.set_value();
    FAIL() << "first predecessor did not reach its blocking stage";
  }
  const auto second_stop =
      kernel.stop(stop_request("request.aggregate-two.second", second.value(), 1U));
  ASSERT_TRUE(second_stop.ok()) << second_stop.error().message;
  EXPECT_NE(first_stop.value().stop_id, second_stop.value().stop_id);

  const FullStopIntentDTO full_intent{
      PublicRequestContextDTO{RequestId("request.aggregate-two.full"),
                              RequestOwnerEpoch("kernel.epoch.1"), "test", std::nullopt},
      second_stop.value().stop_generation,
      {}};
  const auto aggregate = kernel.full_stop(full_intent);
  release_first_stage_promise.set_value();
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
  clock.set(100);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal,
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
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal,
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
      clock, journal,
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
      clock, journal,
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
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 1U});
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
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  journal.fail_next_kind = "stop_terminal";
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 8U});
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
  EXPECT_EQ(engine.stop_calls.load(), 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReplacementAdoptsProjectedTerminalCertificateWithoutRedispatch) {
  FakeClock clock;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  std::optional<RecoveredStopJournalStateDTO> recovered;
  {
    FakeParticipantEngine predecessor_engine;
    FakeOperationJournal predecessor_journal;
    ResourceAddressedExecutionKernel predecessor(
        {registration(binding, predecessor_engine)}, clock, predecessor_journal,
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
      return predecessor_journal.last_stop_accepted.has_value() &&
             predecessor_journal.last_stop_affected_state.has_value() &&
             predecessor_journal.last_stop_certificate.has_value();
    }));
    recovered = RecoveredStopJournalStateDTO{
        *predecessor_journal.last_stop_accepted, *predecessor_journal.last_stop_affected_state,
        predecessor_journal.last_stop_certificate, predecessor_journal.last_stop_fault_latch,
        std::nullopt};
  }

  FakeParticipantEngine replacement_engine;
  FakeOperationJournal replacement_journal;
  replacement_journal.recovery = OperationJournalRecoveryDTO{
      {}, {*recovered}, RequestOwnerEpoch("kernel.epoch.old"), {ResourceId("robot.joint_1")}};
  ResourceAddressedExecutionKernel replacement(
      {registration(binding, replacement_engine)}, clock, replacement_journal,
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

TEST(ResourceAddressedExecutionKernelTest,
     SurveillanceCheckpointFailureDegradesPersistenceAndRecoveryClearsOnlyAfterDurableCommit) {
  FakeClock clock;
  clock.set(100);
  ManualScheduler scheduler;
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

  ResourceAddressedExecutionKernelConfig config{RequestOwnerEpoch("kernel.epoch.1"),
                                                CellId("cell.test"), 32U};
  config.stop_surveillance =
      StopSurveillanceExecutionConfig{&scheduler, &observer, StopSurveillancePolicyDTO{10, 10, 8U}};
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
                                          clock, journal, std::move(config));
  const auto operation = kernel.execute(motion_operation("request.surveillance.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto stopped =
      kernel.stop(stop_request("request.surveillance.stop", operation.value(), 1U));
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
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
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return journal.stop_surveillance_calls.load() == 1 && status.ok() &&
           status.value().surveillance_projection.state == StopDurableProjectionStateDTO::kFailed;
  }));
  auto latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
  EXPECT_TRUE(latch->persistence_degraded);
  EXPECT_EQ(latch->recovery_status, StopFaultRecoveryStatusDTO::kWaitingForDurableRecord);

  observer.evidence_state = StopEvidenceItemStateDTO::kSatisfied;
  clock.set(120);
  scheduler.advance_to(120);
  ASSERT_TRUE(wait_until([&] {
    const auto status = kernel.get_stop_status(stopped.value().stop_id);
    return journal.stop_surveillance_calls.load() == 2 && status.ok() &&
           status.value().surveillance.has_value() &&
           status.value().surveillance->state ==
               StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved &&
           status.value().surveillance_projection.state == StopDurableProjectionStateDTO::kRecorded;
  }));

  auto blocked_intent = motion_operation("request.surveillance.blocked");
  blocked_intent.program_binding_id = unaffected_binding.value().program_binding_id;
  const auto blocked = kernel.execute(blocked_intent);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");

  latch = kernel.get_stop_fault_latch();
  ASSERT_TRUE(latch.has_value());
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

  const auto cleared = kernel.recover_stop_fault(clear_intent);
  ASSERT_TRUE(cleared.ok()) << cleared.error().message;
  EXPECT_TRUE(cleared.value().accepted);
  EXPECT_EQ(cleared.value().latch.state, StopFaultLatchStateDTO::kCleared);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 2);
  const auto clear_replay = kernel.recover_stop_fault(clear_intent);
  ASSERT_TRUE(clear_replay.ok()) << clear_replay.error().message;
  EXPECT_EQ(clear_replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(journal.stop_fault_clear_calls.load(), 2);

  auto admitted_intent = motion_operation("request.surveillance.admitted");
  admitted_intent.program_binding_id = unaffected_binding.value().program_binding_id;
  const auto admitted = kernel.execute(admitted_intent);
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(unaffected_engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     StopCancelsAReservedOperationWhileItsOrdinaryJournalIsBlockedWithoutDispatch) {
  FakeClock clock;
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
      {registration(binding, engine)}, clock, journal,
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
}

TEST(ResourceAddressedExecutionKernelTest,
     BlockedDisjointOrdinaryJournalCannotDelayAnActiveOperationsStopStage) {
  FakeClock clock;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
  clock.set(1000);
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto second_binding = second_same_type_occurrence(first_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(first_binding, first_engine), registration(second_binding, second_engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, default_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::vector<std::string> timeline;
  engine.timeline = &timeline;
  journal.timeline = &timeline;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  timeline.clear();

  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_GE(timeline.size(), 2U);
  EXPECT_EQ(timeline.front(), "journal:operation_accepted");
  EXPECT_EQ(timeline[1], "engine:execute");

  FakeParticipantEngine rejected_engine;
  FakeOperationJournal failing_journal;
  failing_journal.fail_next_kind = "operation_accepted";
  ResourceAddressedExecutionKernel failing_kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          rejected_engine)},
      clock, failing_journal,
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
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::vector<std::string> timeline;
  engine.timeline = &timeline;
  journal.timeline = &timeline;
  ResourceAddressedExecutionKernel kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          engine)},
      clock, journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  timeline.clear();
  const auto scaled = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.ordered", executed.value(), 1U, 0U, 0.5));
  ASSERT_TRUE(scaled.ok()) << scaled.error().message;
  ASSERT_GE(timeline.size(), 2U);
  EXPECT_EQ(timeline.front(), "journal:runtime_scale_accepted");
  EXPECT_EQ(timeline[1], "engine:set_runtime_scale");

  journal.fail_next_kind = "control_accepted";
  timeline.clear();
  const auto unpersisted_pause =
      kernel.pause(control_request("request.pause.unpersisted", executed.value(), 1U));
  ASSERT_FALSE(unpersisted_pause.ok());
  EXPECT_TRUE(timeline.empty() || timeline.front() == "journal:control_accepted");
  EXPECT_EQ(engine.pause_calls, 0);

  engine.next_pause_result =
      PauseResultDTO{false, ErrorDTO{"pause_failed",
                                     "controller rejected pause",
                                     {{"reason", "controller_pause_rejected"}}}};
  timeline.clear();
  const auto rejected_pause =
      kernel.pause(control_request("request.pause.rejected", executed.value(), 1U));
  ASSERT_TRUE(rejected_pause.ok()) << rejected_pause.error().message;
  ASSERT_GE(timeline.size(), 2U);
  EXPECT_EQ(timeline.front(), "journal:control_accepted");
  EXPECT_EQ(timeline[1], "engine:pause");
  const auto intervention = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(intervention.ok()) << intervention.error().message;
  EXPECT_EQ(intervention.value().state, OperationStateKind::kInterventionRequired);

  FakeParticipantEngine stop_engine;
  FakeOperationJournal stop_journal;
  ResourceAddressedExecutionKernel stop_kernel(
      {registration(
          make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
          stop_engine)},
      clock, stop_journal, {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
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
      {ResourceId("robot.joint_1")}};

  ResourceAddressedExecutionKernel kernel(
      {registration(binding, engine)}, clock, journal,
      {RequestOwnerEpoch("kernel.epoch.new"), CellId("cell.test"), 16U});
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
  EXPECT_EQ(blocked.error().details.at("reason"), "motion_admission_closed");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(engine.stop_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest,
     ReconfigurationPreservesActiveEngineThenSelectsCandidateProduct) {
  FakeClock clock;
  FakeParticipantEngine predecessor_engine;
  FakeParticipantEngine candidate_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto candidate_binding = select_candidate_product(predecessor_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, journal,
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
  EXPECT_EQ(occupied.front().state, ResourceSlotState::kReserved);
  ASSERT_TRUE(occupied.front().owner.has_value());
  EXPECT_EQ(occupied.front().owner->operation_id, predecessor.value().operation_id);

  predecessor_engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kCompleted);
  const auto acknowledged = kernel.acknowledge_completed(
      control_request("request.ack.predecessor", predecessor.value(), 1U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  EXPECT_FALSE(kernel.participant_engine_in_use(&predecessor_engine));
  const auto released = kernel.release_retired_engine(&predecessor_engine);
  ASSERT_TRUE(released.ok()) << released.error().message;
  EXPECT_EQ(predecessor_engine.subscription_count(), 0U);
  EXPECT_EQ(candidate_engine.subscription_count(), 1U);

  const auto candidate = kernel.execute(candidate_motion_operation());
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  EXPECT_EQ(predecessor_engine.execute_calls, 1);
  EXPECT_EQ(candidate_engine.execute_calls, 1);
  ASSERT_TRUE(candidate_engine.last_execute.has_value());
  EXPECT_EQ(candidate_engine.last_execute->binding.retained_bundle_id,
            RetainedBundleId(test_digest('e')));
}

TEST(ResourceAddressedExecutionKernelTest,
     GenericReconfigurationRequiresExactRoutePublicationBeforeAdmission) {
  FakeClock clock;
  FakeParticipantEngine predecessor_engine;
  FakeParticipantEngine candidate_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  const auto candidate_binding = select_candidate_product(predecessor_binding);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, journal,
      {RequestOwnerEpoch("kernel.epoch.1"), CellId("cell.test"), 16U});
  const auto candidate_registration = registration(candidate_binding, candidate_engine);

  const auto reconfigured = kernel.reconfigure_runtime_participants({candidate_registration});
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;
  const auto blocked =
      kernel.execute(candidate_motion_operation("request.motion.candidate.blocked"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "runtime_route_publication_required");

  const auto published = kernel.publish_runtime_route_configuration(
      route_configuration_for(candidate_registration), {candidate_registration});
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto readback = kernel.runtime_route_configuration_readback(
      published.value().active_configuration.control_domain_id,
      published.value().active_configuration.active_control_configuration_id,
      published.value().active_configuration.active_control_configuration_generation);
  ASSERT_TRUE(readback.ok()) << readback.error().message;
  EXPECT_EQ(readback.value(), published.value());
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
  const auto rejected =
      kernel.execute(candidate_motion_operation("request.motion.candidate.rejected"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "runtime_route_configuration_not_ready");
}

TEST(ResourceAddressedExecutionKernelTest,
     UnqualifiedBootstrapRemainsClosedUntilExactRouteReadinessPublication) {
  FakeClock clock;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  const auto binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {unqualified_registration(binding, engine)}, clock, journal,
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

  const auto qualified = registration(binding, engine);
  const auto published =
      kernel.publish_runtime_route_configuration(route_configuration_for(qualified), {qualified});
  ASSERT_TRUE(published.ok()) << published.error().message;
  EXPECT_TRUE(published.value().operation_admission_open);

  const auto admitted = kernel.execute(motion_operation("request.motion.bootstrap-open"));
  ASSERT_TRUE(admitted.ok()) << admitted.error().message;
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     FailedReconfigurationRollsBackCandidateSubscriptionAndSelection) {
  FakeClock clock;
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
      {registration(predecessor_binding, predecessor_engine)}, clock, journal,
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
  FakeParticipantEngine predecessor_engine;
  FakeOperationJournal journal;
  const auto predecessor_binding =
      make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernel kernel(
      {registration(predecessor_binding, predecessor_engine)}, clock, journal,
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

}  // namespace
