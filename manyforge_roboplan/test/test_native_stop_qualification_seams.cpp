#include "native_stop_qualification_fixture.hpp"

#include <gtest/gtest.h>

#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/types/prepared_execution.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace fixture = qualification_fixture;

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
      core::StopContainmentCommandKind::kMotionControlledStop, contract,
      std::nullopt};
  command.default_contract_parameters = parameters;
  command.evidence_requirements = {core::ResolvedStopEvidenceRequirementDTO{
      core::StopEvidenceRequirementDTO{
          core::StopEvidenceItemId("controlled.rest"),
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
  return stage;
}

/// The Kernel's per-robot Stop-time values at the Stop request (Q51): a
/// controlled stage carries its own profile deadline, nothing else is shared.
[[nodiscard]] core::RemainingStopBudgetDTO
remaining_budget(const core::ResolvedParticipantStopStageDTO &stage) {
  core::RemainingStopBudgetDTO budget{stage.stop_id, stage.stop_generation, 0,
                                      std::nullopt};
  const auto &command =
      stage.participant_stop_plan.stage_commands.at(stage.stage_index);
  if (command.resolved_stage.containment_command.command_kind ==
          core::StopContainmentCommandKind::kMotionControlledStop &&
      command.validated_contract_parameters.has_value()) {
    budget.remaining_controlled_deadline_ns =
        core::controlled_stop_deadline_from_contract_parameters(
            *command.validated_contract_parameters);
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
      core::SchemaId("manyforge.value_schema.provider_contract.empty.v1"),
      core::SchemaRevision(
          "sha256:"
          "fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270")};
  command.stage_index = 1U;
  command.stop_escalation_stage_id =
      core::StopEscalationStageId("software_hard");
  command.stage_kind = core::StopEscalationStageKind::kSoftwareHard;
  command.containment_command = {
      core::StopContainmentCommandKind::kRegistered, contract,
      core::RegisteredStopCommandKindId("manyforge.stop.software_hard")};
  command.default_contract_parameters = core::JsonObjectDTO{};
  hard.validated_contract_parameters = core::JsonObjectDTO{};
  auto &evidence = command.evidence_requirements.front();
  evidence.requirement.evidence_item_id = core::StopEvidenceItemId("hard.rest");
  evidence.requirement.provider_contract_ref = contract;
  evidence.resolved_source.provider_contract_ref = contract;
  stage.participant_stop_plan.stage_commands.push_back(std::move(hard));
  stage.stage_index = 1U;
  return stage;
}

// Controlled arm stage requiring fresh reference rest and exact goal-terminal
// observation, with an explicit natural-completion policy.
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

// Core unit seam. The port scripts controller facts; it contains no motion,
// braking, scale, or Stop policy. Production Core owns all those decisions.
class CoreStopSeam final {
public:
  using Port = core::testing::FakeExecutionPort;
  CoreStopSeam()
      : binding(fixture::make_binding(true)),
        trajectory(fixture::trajectory(binding, 5.0)),
        port(std::make_shared<Port>()) {
    clock.set(1'000'000'000);
    core::ExecutionLaneFactsDTO facts;
    facts.joint_names = trajectory.joint_names;
    facts.supports_scale = true;
    facts.has_measured_velocity = true;
    facts.state_rate_hz = 100.0;
    facts.feedback_rate_hz = 100.0;
    facts.controller_update_rate_hz = 100.0;
    port->SetLaneFacts(facts);
    config.resources = binding.value().resources;
    config.trip_authority = core::StateAuthorityClass::kCommanded;
    config.stop_traj_delta_s = 0.05;
    config.sample_period_s = 0.01;
    config.replacement_start_allowance_s = 0.02;
    config.hard_stop_completion_timeout_ns = 1'000'000'000;
    for (const auto &name : trajectory.joint_names)
      config.max_deceleration_on_cancel_rad_s2.emplace(name, 4.0);
    helper = std::make_unique<core::ParticipantStopStageHelper>(
        *port, clock,
        [this](core::TimePoint deadline, const std::stop_token &) {
          Tick(deadline);
        },
        [this] { return frozen; }, config);
  }

  void Tick(core::TimePoint deadline) {
    clock.advance(std::max<core::Duration>(
        1, std::min<core::Duration>(10'000'000, deadline - clock.now())));
    ++ticks;
    if (on_tick)
      on_tick();
  }

  static core::ExecutionGoalKey
  Key(const core::ResolvedParticipantStopStageDTO &stage) {
    return {stage.controller_goal_id, stage.controller_goal_generation};
  }

  void Executing(const core::ExecutionGoalKey &key) {
    core::ExecutionGoalObservationDTO observation;
    observation.key = key;
    observation.binding_epoch = core::ExecutionBindingEpoch(1U);
    observation.phase = core::ExecutionGoalPhase::kExecuting;
    observation.mechanism_goal_id = core::MechanismGoalId(
        "core-seam-" + std::to_string(key.controller_goal_generation.value()));
    core::ExecutionGoalFeedbackDTO feedback;
    feedback.source_stamp_ns = clock.now() - 1;
    feedback.local_receipt_stamp_ns = clock.now() - 1;
    observation.latest_feedback = feedback;
    port->SetGoal(std::move(observation));
  }

  void Bind(const core::ResolvedParticipantStopStageDTO &stage) {
    const auto &target = stage.participant_stop_plan.stage_commands.front()
                             .resolved_stage.command_target;
    Executing(Key(stage));
    helper->BindDispatchedGoal(
        {Key(stage),
         trajectory,
         stage.operation_id,
         stage.operation_revision,
         stage.participant_id,
         stage.controllable_component_id,
         stage.command_target_id,
         stage.capability_id,
         stage.control_domain_id,
         stage.control_domain_revision,
         stage.control_binding_id,
         stage.control_binding_revision,
         stage.controller_topology_id,
         stage.topology_revision,
         stage.control_route_id,
         stage.active_control_configuration_id,
         stage.active_control_configuration_generation,
         {target.provider_instance_id, target.provider_manifest_id,
          target.provider_manifest_revision, target.runtime_unit_id,
          target.provider_runtime_epoch},
         stage.resources,
         stage.stop_evidence_policy_id,
         stage.stop_evidence_policy_revision,
         {stage.participant_stop_plan}},
        std::make_shared<core::ExecutionSampleAssociation>(
            core::ExecutionBindingEpoch(1U), port->ReadSample(), clock.now()));
  }

  void
  Sample(double cursor, const std::vector<double> &position,
         const std::vector<double> &reference_velocity,
         std::optional<std::vector<double>> measured_velocity = std::nullopt,
         std::optional<core::TimePoint> source_stamp = std::nullopt) {
    core::ExecutionControllerSampleDTO sample;
    sample.source_stamp_ns = source_stamp.value_or(clock.now());
    sample.local_receipt_stamp_ns = clock.now();
    sample.local_receipt_count = core::ExecutionReceiptCount(++receipts);
    sample.binding_epoch = core::ExecutionBindingEpoch(1U);
    sample.cursor_s = cursor;
    sample.applied_factor = frozen;
    sample.reference_positions_rad = position;
    sample.command_positions_rad = position;
    sample.measured_positions_rad = position;
    sample.reference_velocities_rad_s = reference_velocity;
    sample.reference_accelerations_rad_s2 =
        std::vector<double>(position.size(), 0.0);
    sample.measured_velocities_rad_s =
        measured_velocity.value_or(reference_velocity);
    sample.error_positions_rad = std::vector<double>(position.size(), 0.0);
    port->SetSample(std::move(sample));
  }

  void PathSample(double cursor) {
    const auto sampled = core::sample_trajectory(trajectory, cursor);
    if (!sampled.ok())
      throw std::logic_error(sampled.error().message);
    Sample(cursor, sampled.value().positions_rad,
           sampled.value().velocities_rad_s);
    auto sample = *port->ReadSample();
    sample.reference_accelerations_rad_s2 =
        sampled.value().accelerations_rad_s2;
    port->SetSample(std::move(sample));
  }

  template <class Call> std::vector<Call> Calls() const {
    std::vector<Call> result;
    for (const auto &command : port->commands())
      if (const auto *call = std::get_if<Call>(&command))
        result.push_back(*call);
    return result;
  }

  void CompleteReplacement(std::size_t finish_tick = 100U) {
    auto count = std::make_shared<std::size_t>(0U);
    on_tick = [this, finish_tick, count] {
      const auto calls = Calls<Port::ReplaceCall>();
      if (calls.empty())
        return;
      const auto &call = calls.back();
      ++*count;
      if (*count == 1U) {
        Executing(call.next_key);
        port->FinishGoal(call.active_key,
                         core::ExecutionGoalResult::kPreemptedBy, call.next_key,
                         clock.now());
      }
      const auto &endpoint = call.trajectory.waypoints.back();
      Sample(0.01 * static_cast<double>(*count), endpoint.positions_rad,
             std::vector<double>(endpoint.positions_rad.size(), 0.0));
      if (*count == finish_tick)
        port->FinishGoal(call.next_key, core::ExecutionGoalResult::kSucceeded,
                         std::nullopt, clock.now());
    };
  }

  core::RuntimeParticipantBinding binding;
  core::TrajectoryDTO trajectory;
  std::shared_ptr<Port> port;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  core::StopStageHelperConfig config;
  std::unique_ptr<core::ParticipantStopStageHelper> helper;
  std::function<void()> on_tick;
  double frozen{1.0};
  std::size_t ticks{};
  std::uint64_t receipts{};
};

// Backend-validated prepared DTO for the actual Core participant unit seam.
// This is not compiler/sealed-bundle/ROS-controller qualification.
core::PreparedOperationParticipantDTO
CorePreparedParticipant(const CoreStopSeam &world,
                        const core::ResolvedParticipantStopStageDTO &stage) {
  const auto &binding = world.binding.value();
  const auto &path = *binding.control_path;
  auto request = core::testing::make_test_plan_request(
      "move.core-seam", core::ResolvedJointGoalDTO{},
      world.trajectory.joint_names);
  request.operation_id = stage.operation_id;
  request.participant_id = stage.participant_id;
  request.program_id = binding.program_id;
  request.program_revision = binding.program_revision;
  request.program_binding_id = binding.program_binding_id;
  request.program_run_id = stage.participant_stop_plan.program_run_id;
  request.controllable_component_id = binding.controllable_component_id;
  request.component_type_id = binding.component_type_id;
  request.component_type_revision = binding.component_type_revision;
  request.command_target_id = binding.command_target_id;
  request.capability_id = binding.capability_id;
  request.capability_definition_revision =
      binding.capability_definition_revision;
  request.control_domain_id = path.control_domain_id;
  request.control_domain_revision = path.control_domain_revision;
  request.control_binding_id = path.control_binding_id;
  request.control_binding_revision = path.control_binding_revision;
  request.control_interface_contract_id = path.control_interface_contract_id;
  request.control_interface_revision = path.control_interface_revision;
  request.controller_topology_id = path.controller_topology_id;
  request.topology_revision = path.topology_revision;
  request.control_route_id = path.control_route_id;
  request.activation_set_id = path.activation_set_id;
  request.active_control_configuration_id =
      path.active_control_configuration_id;
  request.active_control_configuration_generation =
      path.active_control_configuration_generation;
  request.provider_instance_id = path.provider_instance_id;
  request.provider_manifest_id = path.provider_manifest_id;
  request.provider_manifest_revision = path.provider_manifest_revision;
  request.provider_runtime_epoch = path.provider_runtime_epoch;
  request.resources = stage.resources;
  const auto &planner = binding.supporting_capability_provider_paths.front();
  request.planner_capability_binding = {
      planner.capability_id,
      planner.capability_definition_revision,
      planner.capability_provider_binding_id,
      planner.capability_provider_binding_revision,
      planner.provider_instance_id,
      planner.provider_manifest_id,
      planner.provider_manifest_revision,
      planner.provider_runtime_unit_id,
      planner.provider_runtime_epoch};
  const fixture::StateChannel initial(world.binding);
  const auto state = initial.current_controlled_state();
  if (!state.ok())
    throw std::logic_error(state.error().message);
  request.start_state = state.value();
  const auto digest = core::ContentDigest("sha256:" + std::string(64U, 'a'));
  core::PreparedMotionExecutionDTO motion{
      core::MoveId("move.core-seam"),
      request,
      world.trajectory,
      state.value(),
      {{world.trajectory, std::nullopt,
        request.projection_binding.projection_revision},
       0U,
       request.planner_capability_binding},
      {core::ValidationRevision("validation.core-seam"),
       core::ValidationResultDTO{true, std::nullopt},
       core::CollisionWorldRevision{1U, digest.value()},
       core::CollisionPolicyRevision{1U, digest.value()},
       core::CollisionDecision{}},
      std::nullopt,
      std::nullopt,
      1.0,
      {},
      10.0};
  std::vector<core::ControllerInterfaceInstanceDTO> interfaces;
  for (const auto &resource : stage.resources)
    interfaces.push_back({resource, "position"});
  return core::PreparedOperationParticipantDTO{
      digest,
      stage.operation_id,
      stage.operation_revision,
      stage.participant_id,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      stage.participant_stop_plan.program_run_id,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.controllable_component_id,
      core::ComponentLifecycleGeneration(1U),
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      binding.model_bindings,
      binding.frame_bindings,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      path.control_interface_contract_id,
      path.control_interface_revision,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      path.activation_set_id,
      {path.provider_instance_id, path.provider_manifest_id,
       path.provider_manifest_revision, path.provider_runtime_unit_id,
       path.provider_runtime_epoch},
      std::nullopt,
      stage.resources,
      core::PreparedParticipantFocusedPortDTO{path.provider_runtime_unit_id},
      interfaces,
      interfaces,
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      binding.runtime_unit_epochs,
      path.stop_evidence_policy_id,
      path.stop_evidence_policy_revision,
      stage.participant_stop_plan,
      std::move(motion),
      std::nullopt};
}

class CoreParticipantSeam final {
public:
  CoreParticipantSeam()
      : publication(
            world.trajectory.joint_names,
            {{core::ObservedRobotStateSourceKind::kCommandedState, "core-seam",
              core::ObservedRobotStateAuthorityProfile::kCommandedSimulation,
              0}}) {
    core::ExecutionPortParticipantConfig config;
    config.stop = world.config;
    config.late_cycle_margin_s = 0.5;
    participant = std::make_unique<core::ExecutionPortParticipant>(
        world.binding, world.port, world.clock, world.scheduler,
        [this](core::TimePoint deadline, const std::stop_token &) {
          world.Tick(deadline);
        },
        core::ExecutionPortParticipantStateAuthority{
            &publication, [] { return core::Result<void>::Ok(); }},
        config);
  }

  core::ExecutionHandle
  Start(const core::ResolvedParticipantStopStageDTO &stage,
        std::uint64_t execution_id = 1U) {
    const auto prepared = CorePreparedParticipant(world, stage);
    const auto result = participant->Prepare(prepared, execution_id);
    if (result.state != core::PrepareParticipantResultStateDTO::kPrepared)
      throw std::logic_error(result.error ? result.error->message
                                          : "Core Prepare rejected");
    auto handle = participant->Dispatch(prepared, stage.controller_goal_id,
                                        stage.controller_goal_generation);
    if (handle.get_snapshot().state == core::ExecutionState::kFailed)
      throw std::logic_error(handle.get_snapshot().error->message);
    world.Executing(CoreStopSeam::Key(stage));
    return handle;
  }

  CoreStopSeam world;
  core::ObservedRobotStatePublication publication;
  std::unique_ptr<core::ExecutionPortParticipant> participant;
};

void ExpectSameSnapshot(const core::ExecutionSnapshot &before,
                        const core::ExecutionSnapshot &after) {
  EXPECT_EQ(after.timestamp_ns, before.timestamp_ns);
  EXPECT_EQ(after.sequence_number, before.sequence_number);
  EXPECT_EQ(after.execution_id, before.execution_id);
  EXPECT_EQ(after.move_id, before.move_id);
  EXPECT_EQ(after.state, before.state);
  EXPECT_EQ(after.progress, before.progress);
  EXPECT_EQ(after.joint_positions_rad, before.joint_positions_rad);
  EXPECT_EQ(after.joint_velocities_rad_s, before.joint_velocities_rad_s);
  EXPECT_EQ(after.state_source, before.state_source);
  EXPECT_EQ(after.source_timestamp_ns, before.source_timestamp_ns);
  EXPECT_EQ(after.source_age_ns, before.source_age_ns);
  EXPECT_EQ(after.error, before.error);
  EXPECT_EQ(after.action_span, before.action_span);
  EXPECT_EQ(after.contact_policy_id, before.contact_policy_id);
}

// Items 1/26: controller-reference containment and exact fresh terminal
// observation; no measured physical-rest claim. Kernel owns Reset (Q27).
TEST(CoreStopQualificationSeamTest,
     TerminalGoalAcceptsExactHardStopWithOneFreshPostCommandSample) {
  // Native direct-Reset and hard-request counters retire with that port.
  // Genuine controller reference rest does not assert measured physical rest.
  for (const double measured_velocity : {0.0, 0.5}) {
    SCOPED_TRACE(measured_velocity);
    CoreParticipantSeam seam;
    auto &world = seam.world;
    const auto stage = software_hard_stage_fixture();
    auto handle = seam.Start(stage);
    world.port->FinishGoal(CoreStopSeam::Key(stage),
                           core::ExecutionGoalResult::kSucceeded, std::nullopt,
                           world.clock.now());
    ASSERT_TRUE(handle.wait_for_completion(0));
    ASSERT_EQ(handle.get_snapshot().state, core::ExecutionState::kCompleted);
    const auto before = seam.participant->get_execution_snapshot();
    const auto terminal = world.port->ReadGoal(CoreStopSeam::Key(stage));
    ASSERT_TRUE(terminal);
    const auto commands = world.port->commands().size();
    world.on_tick = [&] {
      world.Sample(5.0, world.trajectory.waypoints.back().positions_rad,
                   {0.0, 0.0},
                   std::vector<double>{measured_velocity, -measured_velocity});
    };
    const auto result = seam.participant->TranslateStopStage(
        stage, remaining_budget(stage), {});
    ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "no diagnostic");
    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kDispatched);
    EXPECT_EQ(world.ticks, 1U);
    EXPECT_EQ(world.port->commands().size(), commands);
    EXPECT_TRUE(world.Calls<CoreStopSeam::Port::ReplaceCall>().empty());
    EXPECT_TRUE(world.Calls<CoreStopSeam::Port::CancelCall>().empty());
    EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
    const auto &payload = std::get<core::JsonObjectDTO>(
        result.evidence_items.front().evidence->value);
    const auto &sample =
        std::get<core::JsonObjectDTO>(payload.at("post_command_sample").value);
    EXPECT_EQ(std::get<std::string>(sample.at("finish_authority").value),
              "controller_reported");
    EXPECT_GE(std::get<double>(sample.at("sample_after_finish_ns").value), 0.0);
    const auto after_goal = world.port->ReadGoal(CoreStopSeam::Key(stage));
    ASSERT_TRUE(after_goal);
    EXPECT_EQ(*after_goal, *terminal);
    EXPECT_EQ(after_goal->key, terminal->key);
    EXPECT_EQ(after_goal->phase, terminal->phase);
    EXPECT_EQ(after_goal->result, terminal->result);
    EXPECT_EQ(after_goal->terminal_local_receipt_stamp,
              terminal->terminal_local_receipt_stamp);
    EXPECT_EQ(after_goal->mechanism_goal_id, terminal->mechanism_goal_id);
    const auto after = seam.participant->get_execution_snapshot();
    ExpectSameSnapshot(before, after);
    EXPECT_EQ(after.state, before.state);
    EXPECT_EQ(after.timestamp_ns, before.timestamp_ns);
    EXPECT_EQ(after.sequence_number, before.sequence_number);
    EXPECT_EQ(after.joint_positions_rad, before.joint_positions_rad);
    ASSERT_TRUE(seam.participant->RetirePreparedExecution(1U).ok());
    auto newer = stage;
    newer.controller_goal_id = core::ControllerGoalId("goal.newer");
    newer.controller_goal_generation = core::ControllerGoalGeneration(2U);
    auto newer_handle = seam.Start(newer, 2U);
    const auto before_stale = world.port->commands().size();
    world.Bind(newer);
    const auto stale =
        world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
    EXPECT_EQ(stale.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(stale.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(world.port->commands().size(), before_stale);
    EXPECT_EQ(newer_handle.get_snapshot().state,
              core::ExecutionState::kExecuting);
    world.port->FinishGoal(CoreStopSeam::Key(newer),
                           core::ExecutionGoalResult::kSucceeded, std::nullopt,
                           world.clock.now());
    ASSERT_TRUE(newer_handle.wait_for_completion(0));
    EXPECT_EQ(newer_handle.get_snapshot().state,
              core::ExecutionState::kCompleted);
    const auto before_ended_stale = world.port->commands().size();
    const auto ended_stale =
        world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
    EXPECT_EQ(ended_stale.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(ended_stale.outcome,
              core::ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(world.port->commands().size(), before_ended_stale);
  }
}

TEST(CoreStopQualificationSeamTest,
     HardStageObservesAlreadyCompletedGoalWithoutRewritingItsTerminalEvent) {
  // DX728/DX729: only the old-aged native terminal success assertion retires.
  // Core freshness is unchanged in M1. Its correction follows separately.
  CoreParticipantSeam seam;
  auto &world = seam.world;
  auto stage = software_hard_stage_fixture();
  stage.participant_stop_plan.stage_commands.back()
      .resolved_stage.evidence_requirements.push_back(
          direct_stage_fixture()
              .participant_stop_plan.stage_commands.front()
              .resolved_stage.evidence_requirements.front());
  auto handle = seam.Start(stage);
  world.port->FinishGoal(CoreStopSeam::Key(stage),
                         core::ExecutionGoalResult::kSucceeded, std::nullopt,
                         world.clock.now());
  ASSERT_TRUE(handle.wait_for_completion(0));
  ASSERT_EQ(handle.get_snapshot().state, core::ExecutionState::kCompleted);
  const auto before = seam.participant->get_execution_snapshot();
  const auto terminal = world.port->ReadGoal(CoreStopSeam::Key(stage));
  ASSERT_TRUE(terminal);
  const auto commands = world.port->commands().size();
  world.on_tick = [&] {
    world.Sample(5.0, world.trajectory.waypoints.back().positions_rad,
                 {0.0, 0.0});
  };
  const auto result =
      seam.participant->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  ASSERT_EQ(result.evidence_items.size(), 2U);
  EXPECT_EQ(world.port->commands().size(), commands);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::ReplaceCall>().empty());
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::CancelCall>().empty());
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
  const auto after_goal = world.port->ReadGoal(CoreStopSeam::Key(stage));
  ASSERT_TRUE(after_goal);
  EXPECT_EQ(*after_goal, *terminal);
  EXPECT_EQ(after_goal->key, terminal->key);
  EXPECT_EQ(after_goal->phase, terminal->phase);
  EXPECT_EQ(after_goal->result, terminal->result);
  EXPECT_EQ(after_goal->terminal_local_receipt_stamp,
            terminal->terminal_local_receipt_stamp);
  EXPECT_EQ(after_goal->mechanism_goal_id, terminal->mechanism_goal_id);
  const auto after = seam.participant->get_execution_snapshot();
  ExpectSameSnapshot(before, after);
  EXPECT_EQ(after.state, before.state);
  EXPECT_EQ(after.timestamp_ns, before.timestamp_ns);
  EXPECT_EQ(after.sequence_number, before.sequence_number);
  EXPECT_EQ(after.joint_positions_rad, before.joint_positions_rad);
}

TEST(CoreStopQualificationSeamTest,
     ControlledStageAfterRacingNaturalCompletionSettlesAtRestWithoutStop) {
  CoreStopSeam world;
  const auto stage = controlled_stage_with_goal_terminal(true);
  world.Bind(stage);
  world.port->FinishGoal(CoreStopSeam::Key(stage),
                         core::ExecutionGoalResult::kSucceeded, std::nullopt,
                         world.clock.now());
  const auto terminal = world.port->ReadGoal(CoreStopSeam::Key(stage));
  world.on_tick = [&] {
    world.Sample(5.0, world.trajectory.waypoints.back().positions_rad,
                 {0.0, 0.0});
  };
  const auto result =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_FALSE(result.error);
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  ASSERT_EQ(result.evidence_items.size(), 2U);
  EXPECT_TRUE(result.controller_acknowledgement_sequence.has_value());
  EXPECT_EQ(result.controller_acknowledgement_sequence,
            result.evidence_items.back().source_sequence);
  for (const auto &item : result.evidence_items) {
    EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kSatisfied);
    ASSERT_TRUE(item.evidence);
    const auto &evidence = std::get<core::JsonObjectDTO>(item.evidence->value);
    EXPECT_TRUE(
        std::get<bool>(evidence.at("completed_by_natural_completion").value));
    const auto &diagnostics = std::get<core::JsonObjectDTO>(
        evidence.at("provider_diagnostics").value);
    EXPECT_EQ(
        std::get<std::string>(diagnostics.at("natural_completion_basis").value),
        "goal_completed_before_stop_stage");
  }
  const auto after = world.port->ReadGoal(CoreStopSeam::Key(stage));
  ASSERT_TRUE(terminal && after);
  EXPECT_EQ(after->result, terminal->result);
  EXPECT_EQ(after->terminal_local_receipt_stamp,
            terminal->terminal_local_receipt_stamp);
  EXPECT_TRUE(world.port->commands().empty());
}

TEST(CoreStopQualificationSeamTest,
     ControlledStageStillFailsForEveryOtherEndOfThePreparedGoal) {
  // Native snapshot diagnostic strings retire; exact refusal and no effects
  // survive.
  for (const int end : {0, 1, 2, 3}) {
    SCOPED_TRACE(end);
    CoreStopSeam world;
    const auto stage = controlled_stage_with_goal_terminal(end != 0);
    world.Bind(stage);
    const auto ended = end == 1 ? core::ExecutionGoalResult::kCanceled
                       : end == 2
                           ? core::ExecutionGoalResult::kAbortedPathTolerance
                           : core::ExecutionGoalResult::kSucceeded;
    world.port->FinishGoal(CoreStopSeam::Key(stage), ended, std::nullopt,
                           world.clock.now());
    if (end == 3) {
      auto newer = stage;
      newer.controller_goal_generation = core::ControllerGoalGeneration(2U);
      world.Bind(newer);
    }
    const auto before = world.port->ReadGoal(CoreStopSeam::Key(stage));
    const auto result =
        world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->details.at("reason"),
              end == 3 ? "resolved_stop_stage_dispatch_membership_unavailable"
                       : "prepared_goal_not_active");
    for (const auto &item : result.evidence_items)
      EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kMissing);
    EXPECT_TRUE(world.port->commands().empty());
    const auto after = world.port->ReadGoal(CoreStopSeam::Key(stage));
    ASSERT_TRUE(before && after);
    EXPECT_EQ(after->key, before->key);
    EXPECT_EQ(after->result, before->result);
    EXPECT_EQ(after->terminal_local_receipt_stamp,
              before->terminal_local_receipt_stamp);
  }
}

TEST(CoreStopQualificationSeamTest,
     DirectStageCancellationReturnsExactSupersededNoEffectWithoutDispatch) {
  CoreStopSeam world;
  const auto stage = direct_stage_fixture();
  world.Bind(stage);
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto result = world.helper->TranslateStopStage(
      stage, remaining_budget(stage), cancellation.get_token());
  EXPECT_EQ(result.outcome,
            core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect);
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "stop_stage_superseded_before_dispatch");
  for (const auto &item : result.evidence_items)
    EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kInapplicable);
  EXPECT_TRUE(world.port->commands().empty());
}

TEST(CoreStopQualificationSeamTest,
     ZeroOrExpiredRemainingBudgetNeverDispatchesAStopStage) {
  CoreStopSeam world;
  const auto stage = controlled_stage_fixture();
  world.Bind(stage);
  for (const auto remaining : {0LL, -1LL}) {
    auto budget = remaining_budget(stage);
    ASSERT_TRUE(budget.remaining_controlled_deadline_ns);
    budget.remaining_controlled_deadline_ns = remaining;
    const auto result = world.helper->TranslateStopStage(stage, budget, {});
    EXPECT_EQ(result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, "motion_interrupt_backend_stop_failed");
    EXPECT_EQ(result.error->details.at("reason"),
              "resolved_stop_stage_budget_invalid");
    EXPECT_TRUE(world.port->commands().empty());
  }
}

TEST(CoreStopQualificationSeamTest,
     ExactPreparedGoalCancelStopsOnlyItsActiveNativeExecution) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = direct_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  world.on_tick = [&] {
    if (world.Calls<CoreStopSeam::Port::CancelCall>().empty())
      return;
    const auto key = CoreStopSeam::Key(stage);
    world.port->MarkCancelSent(key, world.clock.now() - 1);
    world.port->AcknowledgeCancel(key, world.clock.now());
    world.port->FinishGoal(key, core::ExecutionGoalResult::kCanceled,
                           std::nullopt, world.clock.now());
    world.Sample(0.001, {0.05, -0.05}, {0.0, 0.0});
  };
  const auto result =
      seam.participant->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  ASSERT_EQ(result.evidence_items.size(), 1U);
  EXPECT_EQ(result.evidence_items.front().state,
            core::StopEvidenceItemStateDTO::kSatisfied);
  EXPECT_TRUE(result.controller_acknowledgement_sequence);
  EXPECT_EQ(handle.get_snapshot().state, core::ExecutionState::kStopped);
  const auto cancels = world.Calls<CoreStopSeam::Port::CancelCall>();
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels.front().key, CoreStopSeam::Key(stage));
  ASSERT_TRUE(seam.participant->RetirePreparedExecution(1U).ok());
  auto newer = stage;
  newer.controller_goal_id = core::ControllerGoalId("goal.newer-cancel");
  auto newer_handle = seam.Start(newer, 2U);
  const auto before = world.port->commands().size();
  world.Bind(newer);
  const auto crossed =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  EXPECT_EQ(crossed.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(crossed.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  EXPECT_EQ(world.port->commands().size(), before);
  EXPECT_EQ(newer_handle.get_snapshot().state,
            core::ExecutionState::kExecuting);
  world.port->FinishGoal(CoreStopSeam::Key(newer),
                         core::ExecutionGoalResult::kSucceeded, std::nullopt,
                         world.clock.now());
  ASSERT_TRUE(newer_handle.wait_for_completion(0));
  EXPECT_EQ(newer_handle.get_snapshot().state,
            core::ExecutionState::kCompleted);
}

TEST(CoreStopQualificationSeamTest,
     ExactPreparedArmStageExecutesOneSecondControlledStopExactlyOnce) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = controlled_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  EXPECT_FALSE(
      seam.participant->stopped_trajectory_time_s(handle.execution_generation));
  world.CompleteReplacement();
  const auto started = world.clock.now();
  const auto result =
      seam.participant->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  EXPECT_FALSE(result.recoverable_substitution_reason);
  const auto calls = world.Calls<CoreStopSeam::Port::ReplaceCall>();
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls.front().active_key, CoreStopSeam::Key(stage));
  EXPECT_EQ(calls.front().trajectory.joint_names, world.trajectory.joint_names);
  EXPECT_GE(world.clock.now() - started, 900'000'000);
  EXPECT_LT(world.clock.now() - started, 1'500'000'000);
  EXPECT_EQ(handle.get_snapshot().state, core::ExecutionState::kStopped);
  const auto cursor =
      seam.participant->stopped_trajectory_time_s(handle.execution_generation);
  ASSERT_TRUE(cursor);
  EXPECT_GT(*cursor, 1.0);
  EXPECT_LT(*cursor, 5.0);
  const auto original = core::sample_trajectory(world.trajectory, *cursor);
  ASSERT_TRUE(original.ok());
  const auto snapshot = seam.participant->get_execution_snapshot();
  ASSERT_EQ(snapshot.joint_positions_rad.size(),
            original.value().positions_rad.size());
  for (std::size_t joint = 0; joint < snapshot.joint_positions_rad.size();
       ++joint)
    EXPECT_NEAR(snapshot.joint_positions_rad[joint],
                original.value().positions_rad[joint], 1e-8);
  EXPECT_FALSE(seam.participant->stopped_trajectory_time_s(
      handle.execution_generation + 1U));
  const auto &payload = std::get<core::JsonObjectDTO>(
      result.evidence_items.front().evidence->value);
  EXPECT_EQ(std::get<std::string>(payload.at("profile_id").value),
            "operator_soft_on_path");
  // Authored enabled jerk/deceleration and 4.0/16.0 motion bounds stay
  // positive.
  const auto &parameters = *stage.participant_stop_plan.stage_commands.front()
                                .validated_contract_parameters;
  EXPECT_TRUE(std::get<bool>(parameters.at("jerk_limits_enabled").value));
  for (const auto &row : std::get<core::JsonValueDTO::Array>(
           parameters.at("per_resource_limits").value)) {
    const auto &value = std::get<core::JsonObjectDTO>(row.value);
    EXPECT_DOUBLE_EQ(std::get<double>(std::get<core::JsonObjectDTO>(
                                          value.at("max_deceleration").value)
                                          .at("value_rad_s2")
                                          .value),
                     4.0);
    EXPECT_DOUBLE_EQ(std::get<double>(std::get<core::JsonObjectDTO>(
                                          value.at("max_jerk").value)
                                          .at("value_rad_s3")
                                          .value),
                     1'000'000.0);
  }
  ASSERT_TRUE(world.trajectory.runtime_scale_bounds);
  EXPECT_DOUBLE_EQ(world.trajectory.runtime_scale_bounds->max_rate_per_s, 4.0);
  EXPECT_DOUBLE_EQ(
      world.trajectory.runtime_scale_bounds->max_rate_change_per_s2, 16.0);
}

TEST(CoreStopQualificationSeamTest,
     CancelledReplacementDoesNotRetainItsPlannedPathEndpoint) {
  CoreStopSeam world;
  const auto stage = controlled_stage_fixture();
  world.Bind(stage);
  world.PathSample(1.0);
  std::stop_source cancellation;
  world.on_tick = [&] {
    if (!world.Calls<CoreStopSeam::Port::ReplaceCall>().empty())
      cancellation.request_stop();
  };
  const auto result = world.helper->TranslateStopStage(
      stage, remaining_budget(stage), cancellation.get_token());
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"),
            "stop_stage_cancelled_after_dispatch");
  EXPECT_FALSE(world.helper->OriginalPathCursor(std::nullopt));
  ASSERT_EQ(world.Calls<CoreStopSeam::Port::ReplaceCall>().size(), 1U);
}

TEST(CoreStopQualificationSeamTest,
     PlannerDiagnosticOverflowProjectsOneStableStopStageFailure) {
  CoreStopSeam world;
  const auto stage = controlled_stage_fixture(0.01);
  world.Bind(stage);
  world.PathSample(1.0);
  const auto result =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_LE(result.error->details.size(), 8U);
  EXPECT_EQ(result.error->details.at("reason"),
            "soft_on_path_retimer_not_dispatchable");
  EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
  EXPECT_EQ(result.error->details.at("stop_disposition"),
            "planner_selected_hard_stop");
  EXPECT_EQ(result.error->details.at("details_truncated"), "true");
  EXPECT_GT(std::stod(result.error->details.at("handoff_path_time_s")), 0.0);
  EXPECT_LT(std::stod(result.error->details.at("handoff_path_time_s")), 5.0);
  EXPECT_TRUE(world.port->commands().empty());
}

TEST(CoreStopQualificationSeamTest,
     PreparedOffChoiceIgnoresInactiveJerkValues) {
  for (const bool enabled : {true, false}) {
    SCOPED_TRACE(enabled);
    CoreStopSeam world;
    auto stage = controlled_stage_fixture(0.000001);
    auto &prepared = stage.participant_stop_plan.stage_commands.front();
    (*prepared.validated_contract_parameters)["jerk_limits_enabled"] =
        core::JsonValueDTO{enabled};
    prepared.resolved_stage.default_contract_parameters =
        prepared.validated_contract_parameters;
    world.Bind(stage);
    world.PathSample(1.0);
    world.CompleteReplacement();
    const auto result =
        world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
    if (enabled) {
      EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
      EXPECT_TRUE(world.port->commands().empty());
    } else {
      EXPECT_EQ(result.outcome,
                core::ParticipantStopStageOutcomeDTO::kSucceeded)
          << (result.error ? result.error->message : "no diagnostic");
      ASSERT_EQ(world.Calls<CoreStopSeam::Port::ReplaceCall>().size(), 1U);
    }
  }
}

TEST(CoreStopQualificationSeamTest,
     NearEndpointNaturalCompletionUsesTheOriginalGoalWithoutReplacement) {
  CoreStopSeam world;
  auto stage = controlled_stage_with_goal_terminal(true);
  world.Bind(stage);
  world.PathSample(4.95);
  world.on_tick = [&] {
    if (world.ticks == 5U)
      world.port->FinishGoal(CoreStopSeam::Key(stage),
                             core::ExecutionGoalResult::kSucceeded,
                             std::nullopt, world.clock.now());
    world.PathSample(5.0);
  };
  const auto result =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.actual_provider_contract_mode,
            core::MotionControlledStopMode::kSoftOnPath);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::ReplaceCall>().empty());
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::CancelCall>().empty());
  ASSERT_EQ(result.evidence_items.size(), 2U);
  for (const auto &item : result.evidence_items) {
    EXPECT_EQ(item.state, core::StopEvidenceItemStateDTO::kSatisfied);
    ASSERT_TRUE(item.evidence);
    const auto &evidence = std::get<core::JsonObjectDTO>(item.evidence->value);
    EXPECT_TRUE(
        std::get<bool>(evidence.at("completed_by_natural_completion").value));
  }
  ASSERT_TRUE(world.port->ReadGoal(CoreStopSeam::Key(stage)));
  EXPECT_EQ(world.port->ReadGoal(CoreStopSeam::Key(stage))->result,
            core::ExecutionGoalResult::kSucceeded);
}

TEST(CoreStopQualificationSeamTest,
     RuntimeScaleChangeIsDeferredUntilTheControlledStopFinishes) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = controlled_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  world.CompleteReplacement();
  const auto script = world.on_tick;
  bool retargeted = false;
  world.on_tick = [&] {
    if (!retargeted &&
        !world.Calls<CoreStopSeam::Port::ReplaceCall>().empty()) {
      seam.participant->set_runtime_velocity_scale(0.25);
      EXPECT_DOUBLE_EQ(
          seam.participant->runtime_velocity_scale_status().commanded_scale,
          0.25);
      retargeted = true;
    }
    world.scheduler.advance_to(world.clock.now());
    EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
    script();
  };
  const auto result =
      seam.participant->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  ASSERT_TRUE(retargeted);
  EXPECT_EQ(handle.get_snapshot().state, core::ExecutionState::kStopped);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
  EXPECT_DOUBLE_EQ(
      seam.participant->runtime_velocity_scale_status().commanded_scale, 0.25);
  // Q17: actual surviving participant releases publication at next Dispatch.
  ASSERT_TRUE(seam.participant->RetirePreparedExecution(1U).ok());
  auto next = stage;
  next.controller_goal_id = core::ControllerGoalId("goal.next-scale");
  auto next_handle = seam.Start(next, 2U);
  world.clock.advance(10'000'000);
  world.scheduler.advance_to(world.clock.now());
  const auto scales = world.Calls<CoreStopSeam::Port::SetScaleCall>();
  ASSERT_EQ(scales.size(), 1U);
  EXPECT_NEAR(scales.front().value, 0.96, 1e-12);
  world.port->FinishGoal(CoreStopSeam::Key(next),
                         core::ExecutionGoalResult::kSucceeded, std::nullopt,
                         world.clock.now());
  ASSERT_TRUE(next_handle.wait_for_completion(0));
  EXPECT_EQ(next_handle.get_snapshot().state, core::ExecutionState::kCompleted);
}

TEST(CoreStopQualificationSeamTest,
     DelayedPublicationCannotRefreshOldStateOrBackdateNewScale) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = controlled_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto before = handle.get_snapshot();
  const auto sample = world.port->ReadSample();
  ASSERT_TRUE(sample);
  world.clock.advance(100'000'000);
  const auto scale_accepted_at = world.clock.now();
  seam.participant->set_runtime_velocity_scale(0.2);
  EXPECT_FALSE(handle.wait_for_completion(0));
  const auto delayed = handle.get_snapshot();
  EXPECT_GE(delayed.source_age_ns, 100'000'000);
  EXPECT_EQ(delayed.source_timestamp_ns, before.source_timestamp_ns);
  EXPECT_EQ(delayed.joint_positions_rad, before.joint_positions_rad);
  EXPECT_DOUBLE_EQ(seam.participant->runtime_velocity_scale_status()
                       .virtual_trajectory_time_s,
                   1.0);
  EXPECT_DOUBLE_EQ(
      seam.participant->runtime_velocity_scale_status().applied_scale, 1.0);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
  world.clock.advance(10'000'000);
  const auto scale_published_at = world.clock.now();
  ASSERT_EQ(scale_published_at - scale_accepted_at, 10'000'001);
  world.scheduler.advance_to(scale_published_at);
  const auto scales = world.Calls<CoreStopSeam::Port::SetScaleCall>();
  ASSERT_EQ(scales.size(), 1U);
  // The intervening nonterminal wait advances the fixture clock by 1 ns.
  // Slew starts at acceptance, using the actual publication interval.
  const double expected_scale =
      std::max(0.2, 1.0 - 4.0 * static_cast<double>(scale_published_at -
                                                  scale_accepted_at) /
                             1e9);
  EXPECT_NEAR(scales.front().value, expected_scale, 1e-12);
  EXPECT_EQ(world.port->ReadSample()->local_receipt_stamp_ns,
            sample->local_receipt_stamp_ns);
  world.port->FinishGoal(CoreStopSeam::Key(stage),
                         core::ExecutionGoalResult::kSucceeded, std::nullopt,
                         world.clock.now());
  ASSERT_TRUE(handle.wait_for_completion(0));
  EXPECT_EQ(handle.get_snapshot().state, core::ExecutionState::kCompleted);
}

TEST(CoreStopQualificationSeamTest,
     StopFreezeDoesNotConsumeAnAlreadyPendingScaleTarget) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = controlled_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  seam.participant->set_runtime_velocity_scale(0.2);
  ASSERT_EQ(
      world.scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
      1U);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
  world.CompleteReplacement();
  const auto script = world.on_tick;
  world.on_tick = [&] {
    world.scheduler.advance_to(world.clock.now());
    EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
    const auto status = seam.participant->runtime_velocity_scale_status();
    EXPECT_DOUBLE_EQ(status.commanded_scale, 0.2);
    EXPECT_DOUBLE_EQ(status.applied_scale, 1.0);
    script();
  };
  const auto result =
      seam.participant->TranslateStopStage(stage, remaining_budget(stage), {});
  ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
      << (result.error ? result.error->message : "no diagnostic");
  EXPECT_EQ(handle.get_snapshot().state, core::ExecutionState::kStopped);
  EXPECT_EQ(
      world.scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
      0U);
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::SetScaleCall>().empty());
  EXPECT_DOUBLE_EQ(
      seam.participant->runtime_velocity_scale_status().commanded_scale, 0.2);
  // Native virtual-clock advancement retires with its worker. The actual
  // participant consumes the controller cursor through its sample association.
}

TEST(CoreStopQualificationSeamTest,
     DelayedStopDispatchCannotReplayAnExpiredLeadIn) {
  CoreStopSeam world;
  const auto stage = controlled_stage_fixture();
  world.Bind(stage);
  world.PathSample(1.0);
  // No own feedback/result arrives within the exact lane lead-in.
  const auto result =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"),
            "soft_on_path_stop_delta_budget_exceeded");
  const auto replaces = world.Calls<CoreStopSeam::Port::ReplaceCall>();
  const auto cancels = world.Calls<CoreStopSeam::Port::CancelCall>();
  ASSERT_EQ(replaces.size(), 1U);
  ASSERT_EQ(cancels.size(), 1U);
  EXPECT_EQ(cancels.front().key, replaces.front().next_key);
  EXPECT_FALSE(world.helper->OriginalPathCursor(std::nullopt));
}

TEST(CoreStopQualificationSeamTest,
     ControlledStopStartedAfterHardIssueCannotResurrectTheExecution) {
  CoreStopSeam world;
  const auto stage = controlled_stage_fixture();
  world.Bind(stage);
  world.port->FinishGoal(CoreStopSeam::Key(stage),
                         core::ExecutionGoalResult::kCanceled, std::nullopt,
                         world.clock.now());
  const auto before = world.port->ReadGoal(CoreStopSeam::Key(stage));
  const auto result =
      world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "prepared_goal_not_active");
  EXPECT_TRUE(world.Calls<CoreStopSeam::Port::ReplaceCall>().empty());
  const auto after = world.port->ReadGoal(CoreStopSeam::Key(stage));
  ASSERT_TRUE(before && after);
  EXPECT_EQ(after->phase, core::ExecutionGoalPhase::kTerminal);
  EXPECT_EQ(after->result, before->result);
  EXPECT_EQ(after->terminal_local_receipt_stamp,
            before->terminal_local_receipt_stamp);
}

TEST(CoreStopQualificationSeamTest,
     HardStopInControlledHandoffIsAcceptedAndPreventsReplacementRearm) {
  CoreParticipantSeam seam;
  auto &world = seam.world;
  const auto stage = controlled_stage_fixture();
  auto handle = seam.Start(stage);
  world.PathSample(1.0);
  std::stop_source cancellation;
  bool hard_issued = false;
  world.on_tick = [&] {
    if (hard_issued || world.Calls<CoreStopSeam::Port::ReplaceCall>().empty())
      return;
    const auto hard = seam.participant->stop();
    EXPECT_TRUE(hard.accepted);
    hard_issued = true;
    cancellation.request_stop();
    const auto cancels = world.Calls<CoreStopSeam::Port::CancelCall>();
    for (const auto &call : cancels)
      world.port->FinishGoal(call.key, core::ExecutionGoalResult::kCanceled,
                             std::nullopt, world.clock.now());
  };
  const auto result = seam.participant->TranslateStopStage(
      stage, remaining_budget(stage), cancellation.get_token());
  ASSERT_TRUE(hard_issued);
  EXPECT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"),
            "stop_stage_cancelled_after_dispatch");
  ASSERT_EQ(world.Calls<CoreStopSeam::Port::ReplaceCall>().size(), 1U);
  EXPECT_FALSE(world.Calls<CoreStopSeam::Port::CancelCall>().empty());
  // Native worker state/rearm phases retire. A canceled wait does not prove
  // the replacement physically stopped; it must not publish a stopped cursor.
  EXPECT_FALSE(
      seam.participant->stopped_trajectory_time_s(handle.execution_generation));
  const auto commands = world.port->commands().size();
  (void)handle.get_snapshot();
  EXPECT_EQ(world.port->commands().size(), commands);
}

TEST(CoreStopQualificationSeamTest,
     RealLateSoftOnPathSelectsSpringBackBeforeDispatchWithoutScopeDrift) {
  for (const bool explicit_spring : {false, true}) {
    SCOPED_TRACE(explicit_spring);
    CoreStopSeam world;
    world.trajectory = fixture::trajectory(world.binding, 1.0);
    world.frozen = 0.75;
    auto stage = controlled_stage_fixture();
    auto &prepared = stage.participant_stop_plan.stage_commands.front();
    auto &parameters = *prepared.validated_contract_parameters;
    parameters["deceleration_time_s"] = core::JsonValueDTO{0.15};
    parameters["allow_natural_completion_on_short_path"] =
        core::JsonValueDTO{false};
    if (explicit_spring) {
      parameters["stop_mode"] =
          core::JsonValueDTO{std::string("spring_back_hold")};
    } else {
      parameters["recoverable_substitution_mode"] =
          core::JsonValueDTO{std::string("spring_back_hold")};
    }
    prepared.resolved_stage.default_contract_parameters = parameters;
    const auto scope = stage.resolved_stop_scope_policy_digest;
    const auto resources = stage.resources;
    world.Bind(stage);
    world.PathSample(0.95);
    const auto trip = world.port->ReadSample();
    ASSERT_TRUE(trip);
    world.CompleteReplacement(20U);
    const auto result =
        world.helper->TranslateStopStage(stage, remaining_budget(stage), {});
    ASSERT_EQ(result.outcome, core::ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "no diagnostic");
    EXPECT_EQ(result.actual_provider_contract_mode,
              core::MotionControlledStopMode::kSpringBackHold);
    const auto replaces = world.Calls<CoreStopSeam::Port::ReplaceCall>();
    ASSERT_EQ(replaces.size(), 1U);
    const auto &endpoint = replaces.front().trajectory.waypoints.back();
    for (std::size_t joint = 0; joint < endpoint.positions_rad.size(); ++joint)
      EXPECT_NEAR(endpoint.positions_rad[joint],
                  trip->reference_positions_rad[joint], 1e-6);
    const auto cursor = world.helper->OriginalPathCursor(std::nullopt);
    ASSERT_TRUE(cursor);
    EXPECT_NEAR(*cursor, trip->cursor_s, 1e-9);
    if (explicit_spring)
      EXPECT_FALSE(result.recoverable_substitution_reason);
    else
      EXPECT_EQ(result.recoverable_substitution_reason,
                "soft_on_path_remaining_time_short");
    const auto &evidence = std::get<core::JsonObjectDTO>(
        result.evidence_items.front().evidence->value);
    EXPECT_FALSE(
        std::get<bool>(evidence.at("completed_by_natural_completion").value));
    EXPECT_EQ(stage.resolved_stop_scope_policy_digest, scope);
    EXPECT_EQ(stage.resources, resources);
    EXPECT_EQ(replaces.front().trajectory.resources, resources);
  }
}

} // namespace
} // namespace manyforge::roboplan_backend
