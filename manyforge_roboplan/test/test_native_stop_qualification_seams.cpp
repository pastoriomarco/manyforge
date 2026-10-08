#include "native_stop_qualification_fixture.hpp"
#include "native_stop_qualification_probe.hpp"

#include <gtest/gtest.h>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace std::chrono_literals;
namespace fixture = qualification_fixture;

class RecordingProbe final : public detail::INativeStopQualificationProbe {
public:
  explicit RecordingProbe(std::optional<detail::NativeStopQualificationEvent>
                              blocked = std::nullopt)
      : blocked_(blocked) {}

  void observe(detail::NativeStopQualificationEvent event,
               std::string_view detail_text) override {
    std::unique_lock lock(mutex_);
    events_.push_back({event, std::string(detail_text)});
    cv_.notify_all();
    if (blocked_.has_value() && *blocked_ == event) {
      cv_.wait(lock, [this] { return released_; });
    }
  }

  [[nodiscard]] bool wait_for(detail::NativeStopQualificationEvent event,
                              std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return first_index_locked(event).has_value(); });
  }

  [[nodiscard]] std::optional<std::size_t>
  first_index(detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    return first_index_locked(event);
  }

  [[nodiscard]] std::string
  first_detail(detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(),
                     [event](const auto &row) { return row.first == event; });
    return found == events_.end() ? std::string{} : found->second;
  }

  [[nodiscard]] std::size_t
  count(detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(events_.begin(), events_.end(),
                      [event](const auto &row) { return row.first == event; }));
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

private:
  [[nodiscard]] std::optional<std::size_t>
  first_index_locked(detail::NativeStopQualificationEvent event) const {
    for (std::size_t index = 0U; index < events_.size(); ++index) {
      if (events_[index].first == event) {
        return index;
      }
    }
    return std::nullopt;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<detail::NativeStopQualificationEvent> blocked_;
  std::vector<std::pair<detail::NativeStopQualificationEvent, std::string>>
      events_;
  bool released_{};
};

[[nodiscard]] bool wait_until_executing(RoboPlanExecutionBackend &backend,
                                        double minimum_progress = 0.0) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = backend.get_execution_snapshot();
    if (snapshot.state == core::ExecutionState::kExecuting &&
        snapshot.progress >= minimum_progress) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] core::ResolvedParticipantStopStageDTO direct_stage_fixture() {
  const core::OperationId operation_id("operation.direct");
  const core::OperationRevision operation_revision(1U);
  const core::OperationParticipantId participant_id("participant.direct");
  const auto binding = fixture::make_binding();
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  std::vector<core::ResourceId> resources;
  resources.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  core::ResolvedStopRuntimeTargetDTO target{
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_unit_id,
      core::ProductId("product.native"),
      core::ProductContractId("product.contract.native"),
      core::ProductContractRevision("product-contract-revision"),
      core::ContentDigest("runtime-target-digest"),
      path.provider_runtime_epoch,
      path.provider_runtime_epoch,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  core::ResolvedStopEscalationStageDTO resolved_command{
      0U,
      core::StopEscalationStageId("stage.cancel"),
      core::StopEscalationStageKind::kNominal,
      core::ContainmentGranularity::kResource,
      core::StopContainmentCommandDTO{
          core::StopBudgetPhaseId("command"),
          core::StopContainmentCommandKind::kControllerGoalCancel, std::nullopt,
          std::nullopt},
      std::nullopt,
      core::ResolvedStopCommandQualificationDTO{},
      target,
      core::ResolvedStopResourceContainmentScopeDTO{path.activation_set_id,
                                                    resources},
      core::StopEvidenceConjunction::kAllOf,
      {core::ResolvedStopEvidenceRequirementDTO{
          core::StopEvidenceRequirementDTO{
              core::StopEvidenceItemId("goal.terminal"),
              core::StopBudgetPhaseId("terminal"),
              core::StopEvidenceRequirementKind::kControllerGoalTerminal,
              std::nullopt, std::nullopt,
              core::StopEvidenceSourceRequirementDTO{
                  core::StopEvidenceSourceKind::kControllerGoalStatus,
                  100'000'000,
                  std::nullopt,
                  {}},
              core::StopEvidenceObservationDTO{
                  core::StopEvidenceObservationKind::kPostCommandSample,
                  std::nullopt}},
          core::ResolvedStopEvidenceSourceDTO{
              core::StopEvidenceSourceKind::kControllerGoalStatus, target,
              std::nullopt, std::nullopt, std::nullopt}}}};
  return core::ResolvedParticipantStopStageDTO{
      core::StopId("stop.direct"),
      core::StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      core::ControllerGoalId("goal.direct"),
      core::ControllerGoalGeneration(1U),
      value.controllable_component_id,
      value.command_target_id,
      value.capability_id,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      std::nullopt,
      std::nullopt,
      path.provider_runtime_epoch,
      resources,
      core::ContainmentGranularity::kResource,
      path.stop_evidence_policy_id,
      path.stop_evidence_policy_revision,
      core::PreparedParticipantStopPlanDTO{
          core::ContentDigest("plan.digest"),
          core::ResolvedStopBasePlanId("base.plan"),
          operation_id,
          operation_revision,
          participant_id,
          value.program_id,
          value.program_revision,
          value.program_binding_id,
          core::ProgramRunId("program.run"),
          std::nullopt,
          core::ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          core::StopDeadlineBudgetDTO{
              1'000'000'000,
              100'000'000,
              {{core::StopBudgetPhaseId("command"), 500'000'000},
               {core::StopBudgetPhaseId("terminal"), 900'000'000}}},
          {core::PreparedStopStageCommandDTO{std::move(resolved_command),
                                             std::nullopt, std::nullopt}}},
      0U,
      core::ContentDigest("scope.digest")};
}

[[nodiscard]] core::ResolvedParticipantStopStageDTO
controlled_stage_fixture(double jerk_limit = 1'000'000.0) {
  auto stage = direct_stage_fixture();
  auto &plan = stage.participant_stop_plan;
  auto &prepared = plan.stage_commands.front();
  auto &command = prepared.resolved_stage;
  const auto binding = fixture::make_binding();
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  const core::ProviderContractRefDTO contract{
      core::ProviderContractRoleId("stop_contract"),
      core::SchemaId(std::string(core::kMotionControlledStopContractSchemaId)),
      core::SchemaRevision(
          std::string(core::kMotionControlledStopContractSchemaRevision))};
  core::JsonValueDTO::Array limits;
  for (const auto &resource : value.resources) {
    limits.emplace_back(core::JsonObjectDTO{
        {"resource_id", core::JsonValueDTO{resource.resource_id.value()}},
        {"max_deceleration",
         core::JsonValueDTO{core::JsonObjectDTO{
             {"coordinate_kind", core::JsonValueDTO{"angular_rad_s2"}},
             {"value_rad_s2", core::JsonValueDTO{4.0}},
         }}},
        {"max_jerk",
         core::JsonValueDTO{core::JsonObjectDTO{
             {"coordinate_kind", core::JsonValueDTO{"angular_rad_s3"}},
             {"value_rad_s3", core::JsonValueDTO{jerk_limit}},
         }}},
        {"settle_velocity_threshold",
         core::JsonValueDTO{core::JsonObjectDTO{
             {"coordinate_kind", core::JsonValueDTO{"angular_rad_s"}},
             {"value_rad_s", core::JsonValueDTO{0.01}},
         }}},
    });
  }
  core::JsonObjectDTO parameters{
      {"stop_mode", core::JsonValueDTO{"soft_on_path"}},
      {"jerk_limits_enabled", core::JsonValueDTO{true}},
      {"deceleration_time_s", core::JsonValueDTO{1.0}},
      {"per_resource_limits", core::JsonValueDTO{std::move(limits)}},
      {"allow_natural_completion_on_short_path", core::JsonValueDTO{true}},
  };
  command.stop_escalation_stage_id =
      core::StopEscalationStageId("motion_controlled_stop");
  command.containment_command = core::StopContainmentCommandDTO{
      core::StopBudgetPhaseId("command"),
      core::StopContainmentCommandKind::kMotionControlledStop, contract,
      std::nullopt};
  command.default_contract_parameters = parameters;
  command.evidence_requirements = {core::ResolvedStopEvidenceRequirementDTO{
      core::StopEvidenceRequirementDTO{
          core::StopEvidenceItemId("controlled.rest"),
          core::StopBudgetPhaseId("terminal"),
          core::StopEvidenceRequirementKind::kMotionRest, contract,
          std::nullopt,
          core::StopEvidenceSourceRequirementDTO{
              core::StopEvidenceSourceKind::kBindingStateAuthority,
              100'000'000,
              core::EndpointRoleId("state"),
              {core::StateAuthorityClass::kPredicted}},
          core::StopEvidenceObservationDTO{
              core::StopEvidenceObservationKind::kPostCommandDwell,
              10'000'000}},
      core::ResolvedStopEvidenceSourceDTO{
          core::StopEvidenceSourceKind::kBindingStateAuthority,
          command.command_target, path.state_authority_id,
          path.state_authority_class, contract}}};
  prepared.validated_contract_parameters = std::move(parameters);
  prepared.parameter_source =
      core::StopStageParameterSourceDTO::kDeploymentDefault;
  plan.deadline_budget = core::StopDeadlineBudgetDTO{
      3'000'000'000,
      200'000'000,
      {{core::StopBudgetPhaseId("command"), 300'000'000},
       {core::StopBudgetPhaseId("terminal"), 1'750'000'000}}};
  return stage;
}

[[nodiscard]] core::RemainingStopBudgetDTO
remaining_budget(const core::ResolvedParticipantStopStageDTO &stage) {
  core::RemainingStopBudgetDTO budget{
      stage.stop_id,
      stage.stop_generation,
      stage.participant_stop_plan.deadline_budget.outer_duration_ns,
      {}};
  for (const auto &phase : stage.participant_stop_plan.deadline_budget.phases) {
    budget.phases.push_back({phase.phase_id, phase.deadline_offset_ns});
  }
  return budget;
}

[[nodiscard]] core::ResolvedParticipantStopStageDTO
software_hard_stage_fixture() {
  auto stage = controlled_stage_fixture();
  auto hard = stage.participant_stop_plan.stage_commands.front();
  auto &command = hard.resolved_stage;
  const core::ProviderContractRefDTO contract{
      core::ProviderContractRoleId("software_hard_stop"),
      core::SchemaId(std::string(detail::kNativeSoftwareHardContractSchemaId)),
      core::SchemaRevision(
          std::string(detail::kNativeSoftwareHardContractSchemaRevision))};
  command.stage_index = 1U;
  command.stop_escalation_stage_id =
      core::StopEscalationStageId("software_hard");
  command.stage_kind = core::StopEscalationStageKind::kSoftwareHard;
  command.containment_command = {
      core::StopBudgetPhaseId("hard_command"),
      core::StopContainmentCommandKind::kRegistered, contract,
      core::RegisteredStopCommandKindId("manyforge.stop.software_hard")};
  command.default_contract_parameters = core::JsonObjectDTO{};
  hard.validated_contract_parameters = core::JsonObjectDTO{};
  auto &evidence = command.evidence_requirements.front();
  evidence.requirement.evidence_item_id = core::StopEvidenceItemId("hard.rest");
  evidence.requirement.phase_id = core::StopBudgetPhaseId("hard_terminal");
  evidence.requirement.provider_contract_ref = contract;
  evidence.resolved_source.provider_contract_ref = contract;
  stage.participant_stop_plan.stage_commands.push_back(std::move(hard));
  stage.participant_stop_plan.deadline_budget = {
      500'000'000,
      50'000'000,
      {{core::StopBudgetPhaseId("command"), 50'000'000},
       {core::StopBudgetPhaseId("terminal"), 150'000'000},
       {core::StopBudgetPhaseId("hard_command"), 200'000'000},
       {core::StopBudgetPhaseId("hard_terminal"), 400'000'000}}};
  stage.stage_index = 1U;
  return stage;
}

// Controlled arm stage requiring both fresh motion rest and the exact native
// goal-terminal observation, with an explicit natural-completion policy.
[[nodiscard]] core::ResolvedParticipantStopStageDTO
controlled_stage_with_goal_terminal(bool allow_natural_completion) {
  auto stage = controlled_stage_fixture();
  auto &prepared = stage.participant_stop_plan.stage_commands.front();
  auto &parameters = *prepared.validated_contract_parameters;
  parameters["allow_natural_completion_on_short_path"] =
      core::JsonValueDTO{allow_natural_completion};
  prepared.resolved_stage.default_contract_parameters = parameters;
  prepared.resolved_stage.evidence_requirements.push_back(
      direct_stage_fixture()
          .participant_stop_plan.stage_commands.front()
          .resolved_stage.evidence_requirements.front());
  return stage;
}

TEST(NativeStopQualificationSeamTest,
     TerminalGoalStillAcceptsExactHardStopButRequiresFreshRestDwell) {
  // Fresh rest, one sample, actual motion, Reset, newer execution.
  for (const int source_case : {0, 1, 2, 3, 4}) {
    WallClock clock;
    SimpleScheduler scheduler;
    const auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RecordingProbe probe;
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, {}, probe,
        source_case == 0 || source_case == 2 ? &scheduler : nullptr);
    EXPECT_FALSE(
        backend->stop().accepted); // Legacy idle contract is unchanged.
    const auto stage = software_hard_stage_fixture();
    auto execution =
        backend->execute_trajectory(fixture::trajectory(binding, 5.0));
    ASSERT_TRUE(wait_until_executing(*backend));
    detail::NativeStopQualificationAccess::install_prepared_goal_membership(
        *backend, stage, core::ContentDigest("prepared.terminal-hard"),
        execution);
    const auto nominal =
        backend->execute_controlled_stop(fixture::soft_profile(binding, 0.10));
    ASSERT_TRUE(nominal.accepted);
    ASSERT_TRUE(nominal.backend_stop_confirmed);
    ASSERT_EQ(backend->get_execution_snapshot().state,
              core::ExecutionState::kStopped);
    if (source_case == 2)
      channel->force_observed_velocities({0.5, -0.5});
    const auto requests =
        detail::NativeStopQualificationAccess::hard_stop_requests(*backend);
    const auto reads = channel->state_read_count();
    auto pending = std::async(std::launch::async, [&] {
      return backend->TranslateStopStage(stage, remaining_budget(stage), {});
    });
    if (source_case == 1 || source_case >= 3) {
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (channel->state_read_count() == reads &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      ASSERT_GT(channel->state_read_count(), reads);
      if (source_case == 3) {
        ASSERT_TRUE(backend->reset().accepted);
      } else if (source_case == 4) {
        auto newer =
            backend->execute_trajectory(fixture::trajectory(binding, 0.02));
        ASSERT_TRUE(newer.wait_for_completion(2'000'000'000));
      } else {
        const auto positions =
            backend->get_execution_snapshot().joint_positions_rad;
        const std::vector<double> velocities(positions.size(), 0.0);
        const auto now = clock.now();
        ASSERT_TRUE(
            channel->publish_state(positions, velocities, now, now).applied());
      }
    }
    const auto result = pending.get();
    EXPECT_EQ(
        detail::NativeStopQualificationAccess::hard_stop_requests(*backend),
        requests + (source_case == 3 ? 2U : 1U));
    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kDispatched);
    if (source_case == 0) {
      ASSERT_EQ(result.outcome,
                core::ParticipantStopStageOutcomeDTO::kSucceeded)
          << (result.error ? result.error->message : "no error");
      const auto &payload = std::get<core::JsonObjectDTO>(
          result.evidence_items.front().evidence->value);
      const auto &rest =
          std::get<core::JsonObjectDTO>(payload.at("post_command_rest").value);
      EXPECT_NE(std::get<std::string>(rest.at("first_sample_sequence").value),
                std::get<std::string>(rest.at("last_sample_sequence").value));
      EXPECT_GE(std::get<double>(rest.at("dwell_ns").value), 10'000'000.0);
      // An old exact goal must still reject after a newer goal has also ended.
      auto newer =
          backend->execute_trajectory(fixture::trajectory(binding, 0.02));
      ASSERT_TRUE(newer.wait_for_completion(2'000'000'000));
      const auto before_stale =
          detail::NativeStopQualificationAccess::hard_stop_requests(*backend);
      const auto stale =
          backend->TranslateStopStage(stage, remaining_budget(stage), {});
      EXPECT_EQ(stale.attempt_disposition,
                core::StopStageAttemptDispositionDTO::kNotDispatched);
      EXPECT_EQ(
          detail::NativeStopQualificationAccess::hard_stop_requests(*backend),
          before_stale);
    } else {
      ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->details.at("reason"),
                source_case >= 3 ? "stop_evidence_execution_changed"
                                 : "stop_evidence_not_at_rest");
    }
  }
}

TEST(NativeStopQualificationSeamTest,
     HardStageObservesAlreadyCompletedGoalWithoutRewritingItsTerminalEvent) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe, &scheduler);
  auto stage = software_hard_stage_fixture();
  auto terminal = direct_stage_fixture()
                      .participant_stop_plan.stage_commands.front()
                      .resolved_stage.evidence_requirements.front();
  terminal.requirement.phase_id = core::StopBudgetPhaseId("hard_terminal");
  stage.participant_stop_plan.stage_commands.back()
      .resolved_stage.evidence_requirements.push_back(std::move(terminal));

  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 0.02));
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  clock.advance(30'000'000);
  ASSERT_TRUE(execution.wait_for_completion(2'000'000'000));
  const auto completed = backend->get_execution_snapshot();
  ASSERT_EQ(completed.state, core::ExecutionState::kCompleted);
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.completed-hard"),
      execution);
  clock.advance(1'000'000'000); // The transition event is no longer fresh.

  auto pending = std::async(std::launch::async, [&] {
    return backend->TranslateStopStage(stage, remaining_budget(stage), {});
  });
  const auto wall_deadline = std::chrono::steady_clock::now() + 2s;
  while (pending.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < wall_deadline) {
    clock.advance(10'000'000);
    scheduler.advance_to(clock.now());
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_EQ(pending.wait_for(0ms), std::future_status::ready);
  const auto result = pending.get();
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  ASSERT_EQ(result.evidence_items.size(), 2U);
  const auto &goal_evidence = result.evidence_items.back();
  EXPECT_EQ(goal_evidence.source_sequence, completed.sequence_number);
  const auto &payload =
      std::get<core::JsonObjectDTO>(goal_evidence.evidence->value);
  const auto &observation = std::get<core::JsonObjectDTO>(
      payload.at("native_goal_observation").value);
  EXPECT_GE(std::get<double>(observation.at("transition_age_ns").value),
            1'000'000'000.0);
  EXPECT_EQ(backend->get_execution_snapshot().state, completed.state);
  EXPECT_EQ(backend->get_execution_snapshot().timestamp_ns,
            completed.timestamp_ns);
  EXPECT_EQ(backend->get_execution_snapshot().sequence_number,
            completed.sequence_number);
}

TEST(NativeStopQualificationSeamTest,
     ControlledStageAfterRacingNaturalCompletionSettlesAtRestWithoutStop) {
  // Live 2026-10-06 (Historian 249-254): an ordinary Stop was accepted while
  // this exact goal executed near its endpoint. The goal completed before the
  // controlled stage arrived; the stage failed "prepared_goal_not_active" and
  // forced a software-hard escalation and Stop-fault latch. Injected time
  // places the natural completion between acceptance and stage invocation.
  core::FakeClock clock;
  const auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  const auto stage = controlled_stage_with_goal_terminal(true);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 0.02));
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  // Admitted and still executing at Stop acceptance (injected time frozen).
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.raced-natural"),
      execution);
  ASSERT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kExecuting);
  // The goal reaches its accepted endpoint before the stage is invoked.
  clock.advance(30'000'000);
  ASSERT_TRUE(execution.wait_for_completion(2'000'000'000));
  const auto completed = backend->get_execution_snapshot();
  ASSERT_EQ(completed.state, core::ExecutionState::kCompleted);
  const auto hard_requests =
      detail::NativeStopQualificationAccess::hard_stop_requests(*backend);
  // Every evidence read observes one fresh at-rest sample 5 ms later.
  channel->set_read_hook([&clock, raw = channel.get(),
                          positions = completed.joint_positions_rad] {
    clock.advance(5'000'000);
    const auto now = clock.now();
    (void)raw->publish_state(positions,
                             std::vector<double>(positions.size(), 0.0), now,
                             now);
  });

  const auto result =
      backend->TranslateStopStage(stage, remaining_budget(stage), {});
  channel->set_read_hook({});

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  ASSERT_EQ(result.evidence_items.size(), 2U);
  for (const auto &item : result.evidence_items) {
    EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kSatisfied);
    ASSERT_TRUE(item.evidence);
    const auto &evidence = std::get<core::JsonObjectDTO>(item.evidence->value);
    EXPECT_TRUE(
        std::get<bool>(evidence.at("completed_by_natural_completion").value));
    const auto &diagnostics = std::get<core::JsonObjectDTO>(
        evidence.at("provider_diagnostics").value);
    EXPECT_EQ(std::get<std::string>(
                  diagnostics.at("natural_completion_basis").value),
              "goal_completed_before_stop_stage");
  }
  const auto &rest = std::get<core::JsonObjectDTO>(
      std::get<core::JsonObjectDTO>(result.evidence_items.front().evidence->value)
          .at("post_command_rest")
          .value);
  EXPECT_GE(std::get<double>(rest.at("dwell_ns").value), 10'000'000.0);
  // Goal terminality names the exact Completed transition of this goal.
  EXPECT_EQ(result.evidence_items.back().source_sequence,
            completed.sequence_number);
  EXPECT_EQ(result.controller_acknowledgement_sequence,
            completed.sequence_number);
  // No Stop effect was issued and the natural terminal is not rewritten.
  EXPECT_EQ(detail::NativeStopQualificationAccess::hard_stop_requests(*backend),
            hard_requests);
  EXPECT_EQ(
      probe.count(detail::NativeStopQualificationEvent::kNominalProviderEntry),
      0U);
  EXPECT_EQ(
      probe.count(detail::NativeStopQualificationEvent::kPhysicalDispatch), 0U);
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kCompleted);
  EXPECT_EQ(backend->get_execution_snapshot().sequence_number,
            completed.sequence_number);
}

TEST(NativeStopQualificationSeamTest,
     ControlledStageStillFailsForEveryOtherEndOfThePreparedGoal) {
  // Only this goal's own profile-authorized natural completion settles. A
  // disallowing profile, a Stopped or Failed (aborted) terminal, and a newer
  // generation all remain a failed, undispatched stage so the Kernel escalates.
  enum class End { kCompletedWithoutPolicy, kStopped, kFailed, kNewerGoal };
  for (const auto end : {End::kCompletedWithoutPolicy, End::kStopped,
                         End::kFailed, End::kNewerGoal}) {
    SCOPED_TRACE(static_cast<int>(end));
    core::FakeClock clock;
    clock.set(1'000'000'000);
    const auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RecordingProbe probe(
        end == End::kFailed
            ? std::optional(
                  detail::NativeStopQualificationEvent::kExecutionLoopArmed)
            : std::nullopt);
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, {}, probe);
    auto release =
        std::shared_ptr<void>(nullptr, [&](void *) { probe.release(); });
    const auto stage =
        controlled_stage_with_goal_terminal(end != End::kCompletedWithoutPolicy);
    auto execution = backend->execute_trajectory(
        fixture::trajectory(binding, end == End::kStopped ? 5.0 : 0.02));
    ASSERT_TRUE(probe.wait_for(
        detail::NativeStopQualificationEvent::kExecutionLoopArmed));
    detail::NativeStopQualificationAccess::install_prepared_goal_membership(
        *backend, stage, core::ContentDigest("prepared.other-end"), execution);
    std::string expected_state = "Completed";
    switch (end) {
    case End::kCompletedWithoutPolicy:
    case End::kNewerGoal:
      clock.advance(30'000'000);
      break;
    case End::kStopped:
      ASSERT_TRUE(backend->stop().accepted);
      expected_state = "Stopped";
      break;
    case End::kFailed:
      clock.set(999'999'999); // The goal aborts on an invalid time law.
      release.reset();
      expected_state = "Failed";
      break;
    }
    ASSERT_TRUE(execution.wait_for_completion(2'000'000'000));
    if (end == End::kNewerGoal) {
      ASSERT_EQ(backend->get_execution_snapshot().state,
                core::ExecutionState::kCompleted);
      auto tiny = fixture::trajectory(binding, 1e-10);
      tiny.waypoints.back().positions_rad = tiny.waypoints.front().positions_rad;
      auto newer = backend->execute_trajectory(tiny);
      ASSERT_TRUE(newer.wait_for_completion(2'000'000'000));
    }
    ASSERT_EQ(backend->get_execution_snapshot().state,
              end == End::kStopped  ? core::ExecutionState::kStopped
              : end == End::kFailed ? core::ExecutionState::kFailed
                                    : core::ExecutionState::kCompleted);
    const auto hard_requests =
        detail::NativeStopQualificationAccess::hard_stop_requests(*backend);

    const auto result =
        backend->TranslateStopStage(stage, remaining_budget(stage), {});

    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"), "prepared_goal_not_active");
    EXPECT_EQ(result.error->details.at("current_execution_state"),
              expected_state);
    EXPECT_EQ(result.error->details.at("current_execution_activity"),
              "terminal_or_inactive");
    EXPECT_EQ(result.error->details.at("prepared_execution_generation") !=
                  result.error->details.at("current_execution_generation"),
              end == End::kNewerGoal);
    for (const auto &item : result.evidence_items) {
      EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kMissing);
    }
    EXPECT_EQ(
        detail::NativeStopQualificationAccess::hard_stop_requests(*backend),
        hard_requests);
    EXPECT_EQ(probe.count(
                  detail::NativeStopQualificationEvent::kNominalProviderEntry),
              0U);
  }
}

TEST(NativeStopQualificationSeamTest,
     SoftwareHardStopDoesNotWaitForTheRealNominalAdapterMutex) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));

  auto nominal_adapter_lock =
      detail::NativeStopQualificationAccess::lock_nominal_adapter(*backend);
  auto nominal = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });
  const bool nominal_reached_adapter = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalAdapterLockAttempt);
  if (!nominal_reached_adapter) {
    nominal_adapter_lock.unlock();
  }
  ASSERT_TRUE(nominal_reached_adapter);

  auto hard = std::async(std::launch::async, [&] { return backend->stop(); });
  const auto hard_status = hard.wait_for(100ms);
  nominal_adapter_lock.unlock();
  ASSERT_EQ(hard_status, std::future_status::ready);
  EXPECT_TRUE(hard.get().accepted);
  ASSERT_EQ(nominal.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(nominal.get().accepted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     DirectStageCancellationReturnsExactSupersededNoEffectWithoutDispatch) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto stage = direct_stage_fixture();
  const core::RemainingStopBudgetDTO budget{
      stage.stop_id, stage.stop_generation, 1'000'000'000, {}};

  const auto result =
      backend->TranslateStopStage(stage, budget, cancellation.get_token());

  EXPECT_EQ(result.request.stop_id, stage.stop_id);
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.outcome,
            core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "stop_stage_superseded_before_dispatch");
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kIdle);
}

TEST(NativeStopQualificationSeamTest,
     ZeroOrExpiredRemainingBudgetNeverDispatchesAStopStage) {
  WallClock clock;
  const auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  const auto stage = direct_stage_fixture();
  for (const auto remaining : {0LL, -1LL}) {
    auto budget = remaining_budget(stage);
    budget.remaining_outer_duration_ns = remaining;
    const auto result =
        backend->TranslateStopStage(stage, budget, std::stop_token{});
    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, "motion_interrupt_backend_stop_failed");
    EXPECT_EQ(result.error->details.at("reason"),
              "resolved_stop_stage_budget_invalid");
    EXPECT_EQ(backend->get_execution_snapshot().state,
              core::ExecutionState::kIdle);
  }
}

TEST(NativeStopQualificationSeamTest,
     ExactPreparedGoalCancelStopsOnlyItsActiveNativeExecution) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  const auto stage = direct_stage_fixture();
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.direct"), execution);

  const auto result = backend->TranslateStopStage(
      stage, remaining_budget(stage), std::stop_token{});

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded);
  ASSERT_EQ(result.evidence_items.size(), 1U);
  EXPECT_EQ(result.evidence_items.front().state,
            core::StopEvidenceItemStateDTO::kSatisfied);
  ASSERT_TRUE(result.controller_acknowledgement_sequence.has_value());
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);

  auto later_execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  EXPECT_FALSE(
      backend->stopped_trajectory_time_s(execution.execution_generation));
  const auto crossed = backend->TranslateStopStage(
      stage, remaining_budget(stage), std::stop_token{});
  EXPECT_EQ(crossed.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(crossed.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(crossed.error.has_value());
  EXPECT_EQ(crossed.error->details.at("reason"), "prepared_goal_not_active");
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kExecuting);
  EXPECT_TRUE(backend->stop().accepted);
  EXPECT_TRUE(later_execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     ExactPreparedArmStageExecutesOneSecondControlledStopExactlyOnce) {
  WallClock clock;
  SimpleScheduler scheduler;
  auto binding = fixture::make_binding(/*distinct_compiled_joint_names=*/true);
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe, &scheduler);
  const auto stage = controlled_stage_fixture();
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend, 0.05));
  EXPECT_FALSE(
      backend->stopped_trajectory_time_s(execution.execution_generation));
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.controlled"), execution);

  const auto started = std::chrono::steady_clock::now();
  const auto result = backend->TranslateStopStage(
      stage, remaining_budget(stage), std::stop_token{});
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  EXPECT_FALSE(result.recoverable_substitution_reason.has_value());
  ASSERT_EQ(result.evidence_items.size(), 1U);
  const auto *evidence = std::get_if<core::JsonObjectDTO>(
      &result.evidence_items.front().evidence->value);
  ASSERT_NE(evidence, nullptr);
  EXPECT_EQ(std::get<std::string>(evidence->at("profile_id").value),
            "operator_soft_on_path");
  EXPECT_EQ(
      probe.count(detail::NativeStopQualificationEvent::kNominalProviderEntry),
      1U);
  EXPECT_EQ(
      probe.count(detail::NativeStopQualificationEvent::kPhysicalDispatch), 1U);
  EXPECT_GE(elapsed, 900ms);
  EXPECT_LT(elapsed, 1500ms);
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
  const auto cursor =
      backend->stopped_trajectory_time_s(execution.execution_generation);
  ASSERT_TRUE(cursor);
  EXPECT_GT(*cursor, 0.25);
  EXPECT_LT(*cursor, 5.0);
  const auto original =
      core::sample_trajectory(fixture::trajectory(binding, 5.0), *cursor);
  ASSERT_TRUE(original.ok());
  const auto final_positions =
      backend->get_execution_snapshot().joint_positions_rad;
  ASSERT_EQ(original.value().positions_rad.size(), final_positions.size());
  for (std::size_t i = 0; i < final_positions.size(); ++i)
    EXPECT_NEAR(original.value().positions_rad[i], final_positions[i], 1e-8);
  EXPECT_FALSE(
      backend->stopped_trajectory_time_s(execution.execution_generation + 1U));
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     TinyCompletedTrajectoryRetainsItsExactEndpointWithFrozenClock) {
  core::FakeClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto trajectory = fixture::trajectory(binding, 1e-10);
  trajectory.waypoints.back().positions_rad =
      trajectory.waypoints.front().positions_rad;
  auto execution = backend->execute_trajectory(trajectory);
  ASSERT_TRUE(execution.wait_for_completion(2'000'000'000LL));
  ASSERT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kCompleted);
  const auto cursor =
      backend->stopped_trajectory_time_s(execution.execution_generation);
  ASSERT_TRUE(cursor);
  EXPECT_DOUBLE_EQ(*cursor, trajectory.waypoints.back().time_from_start_s);
}

TEST(NativeStopQualificationSeamTest,
     CancelledReplacementDoesNotRetainItsPlannedPathEndpoint) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend, 0.05));
  std::stop_source cancellation;
  auto pending = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30), cancellation.get_token());
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  bool replaying = false;
  while (std::chrono::steady_clock::now() < deadline &&
         pending.wait_for(0ms) != std::future_status::ready) {
    replaying = backend->get_execution_snapshot().state ==
                core::ExecutionState::kStopping;
    if (replaying)
      break;
    std::this_thread::sleep_for(1ms);
  }
  cancellation.request_stop();
  ASSERT_EQ(pending.wait_for(2s), std::future_status::ready);
  const auto result = pending.get();
  ASSERT_TRUE(replaying);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "controlled_stop_cancelled");
  EXPECT_FALSE(
      backend->stopped_trajectory_time_s(execution.execution_generation));
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     PlannerDiagnosticOverflowProjectsOneStableStopStageFailure) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  const auto stage = controlled_stage_fixture(0.01);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend, 0.05));
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.diagnostic"), execution);

  const auto result = backend->TranslateStopStage(
      stage, remaining_budget(stage), std::stop_token{});

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_LE(result.error->details.size(), 8U);
  EXPECT_EQ(result.error->details.at("reason"),
            "soft_on_path_retimer_not_dispatchable");
  EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
  EXPECT_EQ(result.error->details.at("details_truncated"), "true");
  EXPECT_GT(std::stod(result.error->details.at("handoff_path_time_s")), 0.0);
  EXPECT_LT(std::stod(result.error->details.at("handoff_path_time_s")), 5.0);
  EXPECT_TRUE(backend->stop().accepted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     PreparedOffChoiceIgnoresInactiveJerkValues) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend, 0.05));
  auto profile = fixture::soft_profile(binding, 0.30);
  for (auto &[joint, value] : profile.per_joint_max_jerk)
    value = core::AngularJerkDTO{0.000001};
  auto on = backend->execute_controlled_stop(profile);
  EXPECT_FALSE(on.accepted);
  profile.jerk_limits_enabled = false;
  auto off = backend->execute_controlled_stop(profile);
  EXPECT_TRUE(off.accepted)
      << (off.error ? off.error->message : "no diagnostic");
  EXPECT_TRUE(off.backend_stop_confirmed);
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
  if (!off.accepted)
    (void)backend->stop();
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     NearEndpointNaturalCompletionUsesTheOriginalGoalWithoutReplacement) {
  for (bool spring_back : {false, true}) {
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RoboPlanExecutionBackendConfig config;
    config.soft_on_path_stop_traj_delta_s = 0.02;
    config.soft_on_path_sample_period_s = 0.01;
    RecordingProbe probe;
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, config, probe, &scheduler);
    auto stage = controlled_stage_fixture();
    auto &parameters = *stage.participant_stop_plan.stage_commands.front()
                            .validated_contract_parameters;
    parameters["deceleration_time_s"] = core::JsonValueDTO{0.15};
    stage.participant_stop_plan.stage_commands.front()
        .resolved_stage.evidence_requirements.push_back(
            direct_stage_fixture()
                .participant_stop_plan.stage_commands.front()
                .resolved_stage.evidence_requirements.front());
    if (spring_back) {
      // This non-default choice is explicit in the exact prepared contract.
      parameters["stop_mode"] =
          core::JsonValueDTO{std::string("spring_back_hold")};
      parameters.erase("recoverable_substitution_mode");
    }
    stage.participant_stop_plan.stage_commands.front()
        .resolved_stage.default_contract_parameters = parameters;

    auto execution =
        backend->execute_trajectory(fixture::trajectory(binding, 1.0));
    ASSERT_TRUE(probe.wait_for(
        detail::NativeStopQualificationEvent::kExecutionLoopArmed));
    clock.advance(950'000'000);
    ASSERT_TRUE(wait_until_executing(*backend, 0.94));
    detail::NativeStopQualificationAccess::install_prepared_goal_membership(
        *backend, stage, core::ContentDigest("prepared.natural"), execution);

    auto stopped = std::async(std::launch::async, [&] {
      return backend->TranslateStopStage(stage, remaining_budget(stage),
                                         std::stop_token{});
    });
    ASSERT_TRUE(probe.wait_for(
        detail::NativeStopQualificationEvent::kNominalResultWaitEnter));
    EXPECT_EQ(
        probe.first_detail(
            detail::NativeStopQualificationEvent::kNominalResultWaitEnter),
        "natural_completion");
    clock.advance(100'000'000);
    const auto completion_deadline = std::chrono::steady_clock::now() + 2s;
    while (stopped.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < completion_deadline) {
      clock.advance(10'000'000);
      scheduler.advance_to(clock.now());
      std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(stopped.wait_for(0ms), std::future_status::ready);
    const auto result = stopped.get();

    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kDispatched);
    ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "no diagnostic");
    EXPECT_EQ(result.actual_provider_contract_mode,
              spring_back ? core::MotionControlledStopMode::kSpringBackHold
                          : core::MotionControlledStopMode::kSoftOnPath);
    EXPECT_EQ(
        probe.count(detail::NativeStopQualificationEvent::kPhysicalDispatch),
        0U);
    EXPECT_EQ(backend->get_execution_snapshot().state,
              core::ExecutionState::kCompleted);
    const auto cursor =
        backend->stopped_trajectory_time_s(execution.execution_generation);
    ASSERT_TRUE(cursor);
    EXPECT_DOUBLE_EQ(*cursor, 1.0);
    ASSERT_EQ(result.evidence_items.size(), 2U);
    for (const auto &item : result.evidence_items) {
      EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kSatisfied);
      ASSERT_TRUE(item.evidence);
      const auto &evidence =
          std::get<core::JsonObjectDTO>(item.evidence->value);
      EXPECT_TRUE(
          std::get<bool>(evidence.at("completed_by_natural_completion").value));
    }
    EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
  }
}

TEST(NativeStopQualificationSeamTest,
     RuntimeScaleChangeIsDeferredUntilTheControlledStopFinishes) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe(detail::NativeStopQualificationEvent::kPhysicalDispatch);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));

  auto stopped = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });
  const bool reached_dispatch =
      probe.wait_for(detail::NativeStopQualificationEvent::kPhysicalDispatch);
  if (!reached_dispatch) {
    probe.release();
  }
  ASSERT_TRUE(reached_dispatch);

  backend->set_runtime_velocity_scale(0.25);
  EXPECT_DOUBLE_EQ(backend->runtime_velocity_scale_status().commanded_scale,
                   1.0);
  probe.release();
  ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
  const auto result = stopped.get();

  ASSERT_TRUE(result.accepted);
  EXPECT_TRUE(result.backend_stop_confirmed);
  EXPECT_DOUBLE_EQ(backend->runtime_velocity_scale_status().commanded_scale,
                   0.25);
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     DelayedPublicationCannotRefreshOldStateOrBackdateNewScale) {
  class SampleBarrier final : public detail::INativeStopQualificationProbe {
  public:
    void observe(detail::NativeStopQualificationEvent event,
                 std::string_view) override {
      if (event != detail::NativeStopQualificationEvent::kExecutionSampleReady)
        return;
      std::unique_lock lock(mutex_);
      ++samples_;
      cv_.notify_all();
      cv_.wait(lock, [&] { return finished_ || released_ >= samples_; });
    }
    bool wait(unsigned count) {
      std::unique_lock lock(mutex_);
      return cv_.wait_for(lock, 2s, [&] { return samples_ >= count; });
    }
    void release(bool finish = false) {
      std::lock_guard lock(mutex_);
      ++released_;
      finished_ = finish;
      cv_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned samples_{}, released_{};
    bool finished_{};
  } probe;
  core::FakeClock clock;
  clock.advance(1);
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto release =
      std::shared_ptr<void>(nullptr, [&](void *) { probe.release(true); });
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(probe.wait(1));
  clock.advance(100'000'000);
  backend->set_runtime_velocity_scale(0.2);
  probe.release();
  ASSERT_TRUE(probe.wait(2));
  EXPECT_EQ(backend->get_execution_snapshot().timestamp_ns, 1);
  probe.release();
  ASSERT_TRUE(probe.wait(3));
  // The late target begins only at the second sample boundary. The elapsed
  // 100 ms therefore ran entirely at the original scale, not a backdated ramp.
  EXPECT_NEAR(
      backend->runtime_velocity_scale_status().virtual_trajectory_time_s, 0.1,
      1e-12);
  EXPECT_DOUBLE_EQ(backend->runtime_velocity_scale_status().applied_scale, 1.0);
  release.reset();
  EXPECT_TRUE(backend->stop().accepted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000));
}

TEST(NativeStopQualificationSeamTest,
     CoordinatedWorkersWaitForTheirPeerAndSampleOnePathClock) {
  core::FakeClock clock;
  clock.set(1'000'000'000);
  auto binding = fixture::make_binding();
  auto first_channel = std::make_shared<fixture::StateChannel>(binding);
  auto second_channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe first_probe(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed);
  RecordingProbe second_probe(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed);
  auto first = detail::NativeStopQualificationAccess::construct(
      binding, first_channel, clock, {}, first_probe);
  auto second = detail::NativeStopQualificationAccess::construct(
      binding, second_channel, clock, {}, second_probe);
  auto release = std::shared_ptr<void>(nullptr, [&](void *) {
    first_probe.release();
    second_probe.release();
  });
  const auto trajectory = fixture::trajectory(binding, 5.0);
  const auto bounds = *trajectory.runtime_scale_bounds;
  auto common = std::make_shared<core::detail::CoordinatedExecutionClock>(
      std::vector{core::OperationParticipantId("a"),
                  core::OperationParticipantId("b")},
      core::detail::RuntimeScaleBounds{bounds.max_rate_per_s,
                                       bounds.max_rate_change_per_s2});
  auto first_execution =
      detail::NativeStopQualificationAccess::execute_with_clock(
          *first, trajectory, common, core::OperationParticipantId("a"));
  ASSERT_TRUE(first_probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  first_probe.release();
  clock.advance(1'000'000'000);
  EXPECT_FALSE(common->sample(clock.now()));
  EXPECT_DOUBLE_EQ(first->get_execution_snapshot().progress, 0.0);
  auto second_execution =
      detail::NativeStopQualificationAccess::execute_with_clock(
          *second, trajectory, common, core::OperationParticipantId("b"));
  ASSERT_TRUE(second_probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  second_probe.release();
  ASSERT_TRUE(first_probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionSampleReady));
  ASSERT_TRUE(second_probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionSampleReady));
  const auto origin = common->sample(clock.now());
  ASSERT_TRUE(origin);
  EXPECT_DOUBLE_EQ(origin->trajectory_time_s, 0.0);
  clock.advance(500'000'000);
  ASSERT_TRUE(wait_until_executing(*first, 0.1));
  ASSERT_TRUE(wait_until_executing(*second, 0.1));
  EXPECT_DOUBLE_EQ(
      first->runtime_velocity_scale_status().virtual_trajectory_time_s, 0.5);
  EXPECT_DOUBLE_EQ(
      second->runtime_velocity_scale_status().virtual_trajectory_time_s, 0.5);
  std::uint64_t scale_generation = 0;
  const auto retarget = [&](double scale) {
    return common->retarget(
        scale,
        core::CoordinatedRuntimeScaleCommandDTO{
            core::OperationId("shared.native"), core::OperationRevision(1U),
            core::RuntimeScaleGeneration(++scale_generation)});
  };
  ASSERT_TRUE(retarget(0.25));
  EXPECT_DOUBLE_EQ(first->runtime_velocity_scale_status().commanded_scale,
                   0.25);
  EXPECT_DOUBLE_EQ(second->runtime_velocity_scale_status().commanded_scale,
                   0.25);
  ASSERT_TRUE(first->runtime_velocity_scale_status().coordinated_command);
  ASSERT_TRUE(second->runtime_velocity_scale_status().coordinated_command);
  EXPECT_EQ(
      first->runtime_velocity_scale_status().coordinated_command->generation,
      core::RuntimeScaleGeneration(1U));
  EXPECT_EQ(first->runtime_velocity_scale_status().coordinated_command,
            second->runtime_velocity_scale_status().coordinated_command);
  clock.advance(1'000'000'000);
  const auto scaled = common->sample_now();
  ASSERT_TRUE(scaled);
  ASSERT_TRUE(wait_until_executing(*first, scaled->trajectory_time_s / 5.0));
  ASSERT_TRUE(wait_until_executing(*second, scaled->trajectory_time_s / 5.0));
  EXPECT_DOUBLE_EQ(
      first->runtime_velocity_scale_status().virtual_trajectory_time_s,
      scaled->trajectory_time_s);
  EXPECT_DOUBLE_EQ(
      second->runtime_velocity_scale_status().virtual_trajectory_time_s,
      scaled->trajectory_time_s);
  EXPECT_DOUBLE_EQ(first->runtime_velocity_scale_status().applied_scale,
                   scaled->governor.scale());
  EXPECT_DOUBLE_EQ(second->runtime_velocity_scale_status().applied_scale,
                   scaled->governor.scale());
  ASSERT_TRUE(retarget(0.0));
  clock.advance(2'000'000'000);
  const auto held = common->sample_now();
  ASSERT_TRUE(held);
  ASSERT_DOUBLE_EQ(held->governor.scale(), 0.0);
  ASSERT_TRUE(wait_until_executing(*first, held->trajectory_time_s / 5.0));
  ASSERT_TRUE(wait_until_executing(*second, held->trajectory_time_s / 5.0));
  const auto held_first = first->get_execution_snapshot().joint_positions_rad;
  const auto held_second = second->get_execution_snapshot().joint_positions_rad;
  // A zero shared speed is not completion or a new motion occurrence. Wall
  // time may pass, but restoring speed must not catch up that held interval.
  clock.advance(10'000'000'000);
  const auto still_held = common->sample_now();
  ASSERT_TRUE(still_held);
  EXPECT_DOUBLE_EQ(still_held->trajectory_time_s, held->trajectory_time_s);
  ASSERT_TRUE(retarget(0.5));
  EXPECT_EQ(first->get_execution_snapshot().joint_positions_rad, held_first);
  EXPECT_EQ(second->get_execution_snapshot().joint_positions_rad, held_second);
  clock.advance(500'000'000);
  const auto resumed_scale = common->sample_now();
  ASSERT_TRUE(resumed_scale);
  ASSERT_GT(resumed_scale->trajectory_time_s, held->trajectory_time_s);
  ASSERT_TRUE(
      wait_until_executing(*first, resumed_scale->trajectory_time_s / 5.0));
  ASSERT_TRUE(
      wait_until_executing(*second, resumed_scale->trajectory_time_s / 5.0));
  EXPECT_DOUBLE_EQ(
      first->runtime_velocity_scale_status().virtual_trajectory_time_s,
      resumed_scale->trajectory_time_s);
  EXPECT_DOUBLE_EQ(
      second->runtime_velocity_scale_status().virtual_trajectory_time_s,
      resumed_scale->trajectory_time_s);
  ASSERT_TRUE(retarget(0.75));
  // The FakeClock stays at this retarget boundary: no later worker sample
  // can certify that Stop's captured pose belongs to the new shared law.
  const auto uncaptured =
      first->execute_controlled_stop(fixture::soft_profile(binding, 0.10));
  EXPECT_FALSE(uncaptured.accepted);
  ASSERT_TRUE(uncaptured.error);
  EXPECT_EQ(uncaptured.error->details.at("reason"),
            "coordinated_stop_snapshot_budget_exceeded");
  EXPECT_EQ(first_probe.count(
                detail::NativeStopQualificationEvent::kPhysicalDispatch),
            0U);
  detail::NativeStopQualificationAccess::freeze_runtime_scale(*first);
  EXPECT_FALSE(retarget(0.75));
  detail::NativeStopQualificationAccess::release_runtime_scale(*first);
  // Finishing one Stop must not re-open the stopped occurrence's common law.
  // A fresh prepared dispatch uses a different clock.
  EXPECT_FALSE(retarget(0.75));
  const auto frozen = common->sample_now();
  ASSERT_TRUE(frozen);
  EXPECT_DOUBLE_EQ(frozen->trajectory_time_s, resumed_scale->trajectory_time_s);
  auto waiting_stop = std::async(std::launch::async, [&] {
    return second->execute_controlled_stop(fixture::soft_profile(binding, 0.30));
  });
  ASSERT_TRUE(second_probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalProviderEntry));
  clock.advance(1'000'000'000);
  const auto captured = waiting_stop.get();
  EXPECT_TRUE(captured.accepted);
  EXPECT_TRUE(captured.backend_stop_confirmed);
  EXPECT_FALSE(captured.error);
  EXPECT_TRUE(first->stop().accepted);
  (void)second->stop();
  EXPECT_TRUE(first_execution.wait_for_completion(2'000'000'000));
  EXPECT_TRUE(second_execution.wait_for_completion(2'000'000'000));
  first->set_runtime_velocity_scale(0.4);
  EXPECT_FALSE(first->runtime_velocity_scale_status().active);
  EXPECT_DOUBLE_EQ(first->runtime_velocity_scale_status().commanded_scale, 0.4);
}

TEST(NativeStopQualificationSeamTest,
     StopReleasesAnUnstartedCoordinatedWorkerWithoutWaitingForItsPeer) {
  for (bool controlled_freeze : {false, true}) {
    SCOPED_TRACE(controlled_freeze);
    core::FakeClock clock;
    clock.set(1);
    auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RecordingProbe probe;
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, {}, probe);
    const auto trajectory = fixture::trajectory(binding, 5.0);
    const auto bounds = *trajectory.runtime_scale_bounds;
    auto common = std::make_shared<core::detail::CoordinatedExecutionClock>(
        std::vector{core::OperationParticipantId("a"),
                    core::OperationParticipantId("b")},
        core::detail::RuntimeScaleBounds{bounds.max_rate_per_s,
                                         bounds.max_rate_change_per_s2});
    auto execution = detail::NativeStopQualificationAccess::execute_with_clock(
        *backend, trajectory, common, core::OperationParticipantId("a"));
    ASSERT_TRUE(probe.wait_for(
        detail::NativeStopQualificationEvent::kExecutionLoopArmed));
    if (controlled_freeze)
      detail::NativeStopQualificationAccess::freeze_runtime_scale(*backend);
    else
      EXPECT_TRUE(backend->stop().accepted);
    ASSERT_TRUE(execution.wait_for_completion(2'000'000'000));
    EXPECT_EQ(execution.get_snapshot().state, core::ExecutionState::kStopped);
    EXPECT_FALSE(execution.get_snapshot().error);
    EXPECT_EQ(execution.get_snapshot().joint_positions_rad,
              trajectory.waypoints.front().positions_rad);
    EXPECT_FALSE(common->sample_now());
    if (controlled_freeze)
      detail::NativeStopQualificationAccess::release_runtime_scale(*backend);
  }
}

TEST(NativeStopQualificationSeamTest,
     ReversedClockFailsWithoutAdvancingTheCommandedPose) {
  core::FakeClock clock;
  clock.set(1'000'000'000);
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto release =
      std::shared_ptr<void>(nullptr, [&](void *) { probe.release(); });
  const auto trajectory = fixture::trajectory(binding, 5.0);
  auto execution = backend->execute_trajectory(trajectory);
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  clock.set(999'999'999);
  release.reset();
  ASSERT_TRUE(execution.wait_for_completion(2'000'000'000));
  const auto snapshot = execution.get_snapshot();
  EXPECT_EQ(snapshot.state, core::ExecutionState::kFailed);
  EXPECT_EQ(snapshot.joint_positions_rad,
            trajectory.waypoints.front().positions_rad);
  EXPECT_DOUBLE_EQ(snapshot.progress, 0.0);
  ASSERT_TRUE(snapshot.error);
  EXPECT_EQ(snapshot.error->details.at("reason"),
            "native_runtime_scale_advance_invalid");
}

TEST(NativeStopQualificationSeamTest,
     StopFreezeDoesNotConsumeAnAlreadyPendingScaleTarget) {
  core::FakeClock clock;
  clock.advance(1);
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, {}, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  const bool armed =
      probe.wait_for(detail::NativeStopQualificationEvent::kExecutionLoopArmed);
  if (!armed)
    probe.release();
  ASSERT_TRUE(armed);
  backend->set_runtime_velocity_scale(
      0.2); // Accepted, but the worker is still at its barrier.
  detail::NativeStopQualificationAccess::freeze_runtime_scale(*backend);
  auto freeze = std::shared_ptr<void>(nullptr, [&](void *) {
    detail::NativeStopQualificationAccess::release_runtime_scale(*backend);
  });
  probe.release();
  clock.advance(100'000'000);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (backend->runtime_velocity_scale_status().virtual_trajectory_time_s <
             0.1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto frozen = backend->runtime_velocity_scale_status();
  EXPECT_GE(frozen.virtual_trajectory_time_s, 0.1);
  EXPECT_DOUBLE_EQ(frozen.applied_scale, 1.0);
  EXPECT_DOUBLE_EQ(frozen.commanded_scale, 0.2);
  EXPECT_TRUE(backend->stop().accepted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000));
  freeze.reset();
}

TEST(NativeStopQualificationSeamTest,
     DelayedStopDispatchCannotReplayAnExpiredLeadIn) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe(detail::NativeStopQualificationEvent::kPhysicalDispatch);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  auto stopped = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });
  const bool dispatched =
      probe.wait_for(detail::NativeStopQualificationEvent::kPhysicalDispatch);
  if (!dispatched)
    probe.release();
  ASSERT_TRUE(dispatched);
  EXPECT_EQ(stopped.wait_for(30ms), std::future_status::timeout);
  probe.release();
  ASSERT_EQ(stopped.wait_for(2s), std::future_status::ready);
  const auto result = stopped.get();
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"),
            "native_stop_handoff_budget_exceeded");
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000));
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
}

TEST(NativeStopQualificationSeamTest,
     NonRestingAddressedStateFailsTheControlledStopEvidenceContract) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  channel->force_observed_velocities({0.5, -0.5});
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto stage = controlled_stage_fixture();
  auto &plan = stage.participant_stop_plan;
  auto &parameters = *plan.stage_commands.front().validated_contract_parameters;
  parameters["deceleration_time_s"] = core::JsonValueDTO{0.05};
  plan.stage_commands.front().resolved_stage.default_contract_parameters =
      parameters;
  plan.deadline_budget = core::StopDeadlineBudgetDTO{
      500'000'000,
      50'000'000,
      {{core::StopBudgetPhaseId("command"), 100'000'000},
       {core::StopBudgetPhaseId("terminal"), 250'000'000}}};

  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  detail::NativeStopQualificationAccess::install_prepared_goal_membership(
      *backend, stage, core::ContentDigest("prepared.non-resting"), execution);

  const auto result = backend->TranslateStopStage(
      stage, remaining_budget(stage), std::stop_token{});

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_not_at_rest");
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     ControlledStopStartedAfterHardIssueCannotResurrectTheExecution) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  const auto hard = backend->stop();
  if (!hard.accepted) {
    probe.release();
  }
  ASSERT_TRUE(hard.accepted);

  const auto controlled =
      backend->execute_controlled_stop(fixture::soft_profile(binding, 0.30));
  EXPECT_TRUE(controlled.accepted);
  EXPECT_FALSE(controlled.backend_stop_confirmed);
  ASSERT_TRUE(controlled.error.has_value());
  EXPECT_EQ(controlled.error->details.at("phase"), "controlled_stop_entry");
  EXPECT_FALSE(
      probe.first_index(detail::NativeStopQualificationEvent::kPhysicalDispatch)
          .has_value());
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     HardStopInControlledHandoffIsAcceptedAndPreventsReplacementRearm) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kNominalRearmAttempt);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  auto controlled = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });

  const bool reached_handoff = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalRearmAttempt);
  if (!reached_handoff) {
    probe.release();
  }
  ASSERT_TRUE(reached_handoff);
  EXPECT_FALSE(backend->get_execution_snapshot().state ==
               core::ExecutionState::kExecuting);
  const auto hard = backend->stop();
  EXPECT_TRUE(hard.accepted);
  probe.release();

  ASSERT_EQ(controlled.wait_for(2s), std::future_status::ready);
  const auto result = controlled.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("phase"), "soft_on_path_before_rearm");
  EXPECT_NE(backend->get_execution_snapshot().state,
            core::ExecutionState::kExecuting);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     CancelledNominalWaitDoesNotPublishStoppedBeforeTheWorkerCeases) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kWorkerStopObservedBeforeTerminal);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  auto controlled = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });

  const bool worker_parked = probe.wait_for(
      detail::NativeStopQualificationEvent::kWorkerStopObservedBeforeTerminal);
  const bool caller_waiting = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalResultWaitEnter);
  if (!worker_parked || !caller_waiting) {
    probe.release();
  }
  ASSERT_TRUE(worker_parked);
  ASSERT_TRUE(caller_waiting);
  const auto hard = backend->stop();
  if (!hard.accepted) {
    probe.release();
  }
  ASSERT_TRUE(hard.accepted);
  const auto nominal_status = controlled.wait_for(100ms);
  if (nominal_status != std::future_status::ready) {
    probe.release();
  }
  ASSERT_EQ(nominal_status, std::future_status::ready);
  const auto result = controlled.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  EXPECT_NE(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);

  probe.release();
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
}

TEST(NativeStopQualificationSeamTest,
     RealLateSoftOnPathSelectsSpringBackBeforeDispatchWithoutScopeDrift) {
  // The same short tail must return to the captured trip point when Spring
  // is requested directly; natural completion is never its precision result.
  for (const bool explicit_spring : {false, true}) {
    SCOPED_TRACE(explicit_spring);
    core::FakeClock clock;
    auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RoboPlanExecutionBackendConfig config;
    config.soft_on_path_stop_traj_delta_s = 0.02;
    config.soft_on_path_sample_period_s = 0.01;
    RecordingProbe probe;
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, config, probe);
    const auto original_scope =
        detail::NativeStopQualificationAccess::scope(*backend);
    // At full scale this very short Spring return would exceed the nominal
    // velocity envelope. Use a valid 75% case for substitution ordering; the
    // planner regression separately proves rejection of the full-scale case.
    backend->set_runtime_velocity_scale(0.75);
    auto execution =
        backend->execute_trajectory(fixture::trajectory(binding, 1.0));
    ASSERT_TRUE(wait_until_executing(*backend));
    ASSERT_TRUE(probe.wait_for(
        detail::NativeStopQualificationEvent::kExecutionLoopArmed));
    clock.advance(1'280'000'000);
    ASSERT_TRUE(wait_until_executing(*backend, 0.95));
    const auto trip_positions =
        backend->get_execution_snapshot().joint_positions_rad;
    const auto trip_cursor =
        backend->runtime_velocity_scale_status().virtual_trajectory_time_s;
    auto profile = fixture::soft_profile(binding, 0.15);
    if (explicit_spring) {
      profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
      profile.allow_natural_completion_on_short_path = false;
      profile.recoverable_substitution_mode.reset();
    }

    const auto started = std::chrono::steady_clock::now();
    const auto controlled = backend->execute_controlled_stop(profile);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(controlled.accepted)
        << (controlled.error ? controlled.error->message : "no error");
    EXPECT_TRUE(controlled.backend_stop_confirmed);
    EXPECT_EQ(controlled.executed_stop_mode,
              core::MotionInterruptStopMode::kSpringBackHold);
    EXPECT_FALSE(controlled.completed_by_natural_completion);
    const auto cursor =
        backend->stopped_trajectory_time_s(execution.execution_generation);
    ASSERT_TRUE(cursor);
    EXPECT_DOUBLE_EQ(*cursor, trip_cursor);
    const auto final_positions =
        backend->get_execution_snapshot().joint_positions_rad;
    ASSERT_EQ(final_positions.size(), trip_positions.size());
    for (std::size_t i = 0; i < final_positions.size(); ++i)
      EXPECT_NEAR(final_positions[i], trip_positions[i], 1e-6);
    const auto dispatch = probe.first_index(
        detail::NativeStopQualificationEvent::kPhysicalDispatch);
    ASSERT_TRUE(dispatch.has_value());
    if (explicit_spring) {
      EXPECT_FALSE(controlled.fallback_reason.has_value());
      EXPECT_FALSE(
          probe
              .first_index(
                  detail::NativeStopQualificationEvent::kSubstitutionSelected)
              .has_value());
    } else {
      ASSERT_TRUE(controlled.fallback_reason.has_value());
      EXPECT_EQ(*controlled.fallback_reason,
                "soft_on_path_remaining_time_short");
      EXPECT_EQ(core::interrupt_structured_scalar_text(
                    controlled.diagnostics.at("requested_stop_mode")),
                "soft_on_path");
      EXPECT_EQ(core::interrupt_structured_scalar_text(
                    controlled.diagnostics.at("fallback_reason")),
                *controlled.fallback_reason);
      const auto selection = probe.first_index(
          detail::NativeStopQualificationEvent::kSubstitutionSelected);
      ASSERT_TRUE(selection.has_value());
      EXPECT_LT(*selection, *dispatch);
      EXPECT_EQ(
          probe.first_detail(
              detail::NativeStopQualificationEvent::kSubstitutionSelected),
          "soft_on_path_remaining_time_short");
    }
    EXPECT_EQ(detail::NativeStopQualificationAccess::scope(*backend),
              original_scope);
    EXPECT_LT(elapsed, 500ms);
    EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
  }
}

} // namespace
} // namespace manyforge::roboplan_backend
