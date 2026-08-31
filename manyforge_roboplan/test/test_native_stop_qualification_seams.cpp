#include "native_stop_qualification_fixture.hpp"
#include "native_stop_qualification_probe.hpp"

#include <gtest/gtest.h>

#include <manyforge_core/time/fake_clock.hpp>
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

[[nodiscard]] core::ResolvedParticipantStopStageDTO controlled_stage_fixture() {
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
        {"settle_velocity_threshold",
         core::JsonValueDTO{core::JsonObjectDTO{
             {"coordinate_kind", core::JsonValueDTO{"angular_rad_s"}},
             {"value_rad_s", core::JsonValueDTO{0.01}},
         }}},
    });
  }
  core::JsonObjectDTO parameters{
      {"stop_mode", core::JsonValueDTO{"soft_on_path"}},
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

TEST(NativeStopQualificationSeamTest,
     SoftwareHardStopDoesNotWaitForTheRealNominalAdapterMutex) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
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
  auto binding = fixture::make_binding(/*distinct_compiled_joint_names=*/true);
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {
      {"compiled.robot.joint_1", 1'000'000.0},
      {"compiled.robot.joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  const auto stage = controlled_stage_fixture();
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend, 0.05));
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
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     NearEndpointNaturalCompletionUsesTheOriginalGoalWithoutReplacement) {
  core::FakeClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_stop_traj_delta_s = 0.02;
  config.soft_on_path_sample_period_s = 0.01;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto stage = controlled_stage_fixture();
  auto &parameters =
      *stage.participant_stop_plan.stage_commands.front()
           .validated_contract_parameters;
  parameters["deceleration_time_s"] = core::JsonValueDTO{0.15};
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
    return backend->TranslateStopStage(
        stage, remaining_budget(stage), std::stop_token{});
  });
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalResultWaitEnter));
  EXPECT_EQ(probe.first_detail(
                detail::NativeStopQualificationEvent::kNominalResultWaitEnter),
            "natural_completion");
  clock.advance(100'000'000);
  const auto completion_deadline = std::chrono::steady_clock::now() + 2s;
  while (stopped.wait_for(0ms) != std::future_status::ready &&
         std::chrono::steady_clock::now() < completion_deadline) {
    clock.advance(10'000'000);
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_EQ(stopped.wait_for(0ms), std::future_status::ready);
  const auto result = stopped.get();

  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  EXPECT_EQ(
      probe.count(detail::NativeStopQualificationEvent::kPhysicalDispatch), 0U);
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kCompleted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     RuntimeScaleChangeIsDeferredUntilTheControlledStopFinishes) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kPhysicalDispatch);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));

  auto stopped = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });
  const bool reached_dispatch = probe.wait_for(
      detail::NativeStopQualificationEvent::kPhysicalDispatch);
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
     NonRestingAddressedStateFailsTheControlledStopEvidenceContract) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  channel->force_observed_velocities({0.5, -0.5});
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto stage = controlled_stage_fixture();
  auto &plan = stage.participant_stop_plan;
  auto &parameters =
      *plan.stage_commands.front().validated_contract_parameters;
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
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
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
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
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
  core::FakeClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_stop_traj_delta_s = 0.02;
  config.soft_on_path_sample_period_s = 0.01;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  const auto original_scope =
      detail::NativeStopQualificationAccess::scope(*backend);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 1.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  clock.advance(950'000'000);
  ASSERT_TRUE(wait_until_executing(*backend, 0.94));

  const auto started = std::chrono::steady_clock::now();
  const auto controlled =
      backend->execute_controlled_stop(fixture::soft_profile(binding, 0.15));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(controlled.accepted);
  EXPECT_TRUE(controlled.backend_stop_confirmed);
  EXPECT_EQ(controlled.executed_stop_mode,
            core::MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(controlled.fallback_reason.has_value());
  EXPECT_EQ(*controlled.fallback_reason, "soft_on_path_remaining_time_short");
  EXPECT_EQ(core::interrupt_structured_scalar_text(
                controlled.diagnostics.at("requested_stop_mode")),
            "soft_on_path");
  EXPECT_EQ(core::interrupt_structured_scalar_text(
                controlled.diagnostics.at("fallback_reason")),
            *controlled.fallback_reason);
  const auto selection = probe.first_index(
      detail::NativeStopQualificationEvent::kSubstitutionSelected);
  const auto dispatch = probe.first_index(
      detail::NativeStopQualificationEvent::kPhysicalDispatch);
  ASSERT_TRUE(selection.has_value());
  ASSERT_TRUE(dispatch.has_value());
  EXPECT_LT(*selection, *dispatch);
  EXPECT_EQ(probe.first_detail(
                detail::NativeStopQualificationEvent::kSubstitutionSelected),
            "soft_on_path_remaining_time_short");
  EXPECT_EQ(detail::NativeStopQualificationAccess::scope(*backend),
            original_scope);
  EXPECT_LT(elapsed, 500ms);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

} // namespace
} // namespace manyforge::roboplan_backend
