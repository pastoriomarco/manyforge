#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/types/prepared_execution.hpp>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] ResolvedStopBasePlanDTO BasePlan() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) bytes.pop_back();
  auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) throw std::logic_error(decoded.error().message);
  return std::move(decoded.value());
}

[[nodiscard]] std::pair<std::string, std::string> ParticipantGolden() {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "golden" /
                    "prepared_operation_participant.v1.sha256";
  std::ifstream stream(path, std::ios::binary);
  std::string canonical_hash;
  std::string contract_digest;
  std::getline(stream, canonical_hash);
  std::getline(stream, contract_digest);
  return {std::move(canonical_hash), std::move(contract_digest)};
}

[[nodiscard]] RuntimeParticipantBinding Binding(const ResolvedStopBasePlanDTO& base, bool command) {
  const auto& target = base.escalation_stages.front().command_target;
  const auto command_schema = builtin_gripper_position_command_schema();
  const auto capability_id = command ? command_schema.compatible_capabilities.front().capability_id
                                     : CapabilityId("execution.trajectory.execute");
  const auto capability_revision =
      command ? command_schema.compatible_capabilities.front().capability_definition_revision
              : CapabilityDefinitionRevision(Digest('c'));
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_providers;
  supporting_providers.reserve(command ? 0U : 1U);
  if (!command) {
    supporting_providers.push_back(RuntimeCapabilityProviderPathDTO{
        CapabilityId("planning.motion.joint_goal"), CapabilityDefinitionRevision(Digest('3')),
        CapabilityProviderBindingId(Digest('4')), CapabilityProviderBindingRevision(Digest('5')),
        ProviderInstanceId("planner.1"), ProviderManifestId(Digest('6')),
        ProviderManifestRevision(Digest('7')), RuntimeUnitId("planner.runtime"),
        RuntimeEpoch("planner.epoch")});
  }
  std::vector<RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  runtime_unit_epochs.reserve(command ? 2U : 3U);
  if (!command) {
    runtime_unit_epochs.push_back(
        {RuntimeUnitId("planner.runtime"), RuntimeEpoch("planner.epoch")});
  }
  runtime_unit_epochs.push_back({base.source_runtime_unit_id, base.source_runtime_epoch});
  runtime_unit_epochs.push_back({target.runtime_unit_id, target.runtime_unit_epoch});
  RuntimeParticipantBindingDTO value{
      ProgramId("program.1"),
      ProgramRevision(Digest('1')),
      ProgramBindingId(Digest('2')),
      base.retained_bundle_id,
      base.graph_revision,
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("arm"),
      capability_id,
      capability_revision,
      command ? std::optional(ProgramCommandSchemaRefDTO{command_schema.command_schema_id,
                                                         command_schema.command_schema_revision})
              : std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision")}},
      {RuntimeModelGroupBindingDTO{ModelGroupRoleId("arm.group"),
                                   ModelInstanceId("robot.model"),
                                   "arm_group",
                                   {"joint_1", "joint_2"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint.1"), ResourceId("joint.1"),
                                 std::string("joint_1"), "joint_1", CoordinateKind::kAngular},
       RuntimeResourceBindingDTO{ResourceRoleId("joint.2"), ResourceId("joint.2"),
                                 std::string("joint_2"), "joint_2", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{base.control_domain_id,
                            base.control_domain_revision,
                            base.control_binding_id,
                            base.control_binding_revision,
                            ControlInterfaceContractId("control.interface"),
                            ControlInterfaceRevision("control.interface.revision"),
                            base.controller_topology_id,
                            base.topology_revision,
                            base.control_route_id,
                            base.activation_set_id,
                            base.active_control_configuration_id,
                            base.active_control_configuration_generation,
                            target.provider_instance_id,
                            target.provider_manifest_id,
                            target.provider_manifest_revision,
                            target.runtime_unit_id,
                            target.provider_runtime_epoch,
                            StateAuthorityId("state.measured"),
                            StateAuthorityClass::kMeasured,
                            base.stop_evidence_policy_id,
                            base.stop_evidence_policy_revision,
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      std::move(supporting_providers),
      std::move(runtime_unit_epochs),
      {}};
  std::sort(
      value.runtime_unit_epochs.begin(), value.runtime_unit_epochs.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.runtime_unit_id < rhs.runtime_unit_id; });
  auto verified = verify_runtime_participant_binding(std::move(value));
  if (!verified.ok()) throw std::logic_error(verified.error().message);
  return std::move(verified.value());
}

[[nodiscard]] ControllerLifecycleExpectedDomain ExpectedDomain(
    const ResolvedStopBasePlanDTO& base, const RuntimeParticipantBinding& binding) {
  const auto& path = *binding.value().control_path;
  const std::vector<ControllerInterfaceInstanceDTO> interfaces{{ResourceId("joint.1"), "position"},
                                                               {ResourceId("joint.2"), "position"}};
  const ControllerRoleId controller("hardware");
  ControllerLifecycleExpectedDomainDTO value{
      base.control_domain_id,
      base.control_domain_revision,
      base.controller_topology_id,
      base.topology_revision,
      ProductId("product.control.arm"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision(Digest('8')),
      ContentDigest(Digest('9')),
      ContentDigest(Digest('a')),
      base.source_runtime_unit_id,
      base.source_runtime_epoch,
      {base.controller_topology_id,
       base.topology_revision,
       {{controller,
         {{path.control_interface_contract_id,
           path.control_interface_revision,
           {ControllerClaimDirection::kCommand, ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{base.activation_set_id, {controller}, {}}},
       {{base.control_route_id,
         path.control_interface_contract_id,
         path.control_interface_revision,
         {controller},
         {base.activation_set_id}}}},
      {base.control_domain_id,
       base.control_domain_revision,
       base.controller_topology_id,
       base.topology_revision,
       base.activation_set_id,
       {{path.control_binding_id, path.control_binding_revision, path.control_route_id}}},
      {{path.control_binding_id, path.control_binding_revision, path.control_interface_contract_id,
        path.control_interface_revision, interfaces, interfaces}},
      {{controller,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(Digest('b')),
        "controller_interface.ControllerInterface",
        ContentDigest(Digest('c')),
        interfaces,
        interfaces,
        {},
        {}}},
      {{path.control_binding_id, path.control_binding_revision, path.control_route_id,
        EndpointRoleId("endpoint.command"), "controller.command", true, false}}};
  auto verified = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!verified.ok()) throw std::logic_error(verified.error().message);
  return std::move(verified.value());
}

struct SealedInputs final {
  RuntimeParticipantBinding binding;
  ControllerLifecycleExpectedDomain expected_domain;
  RuntimeParticipantStopReadiness readiness;
};

[[nodiscard]] SealedInputs Inputs(const ResolvedStopBasePlanDTO& base, bool command) {
  auto binding = Binding(base, command);
  auto domain = ExpectedDomain(base, binding);
  auto readiness = verify_runtime_participant_stop_readiness(
      {binding.value().controllable_component_id, ComponentLifecycleGeneration(9U), domain.value(),
       base},
      binding, domain);
  if (!readiness.ok()) throw std::logic_error(readiness.error().message);
  return {std::move(binding), std::move(domain), std::move(readiness.value())};
}

[[nodiscard]] PreparedCapabilityCommandDTO CommandPayload() {
  const auto schema = builtin_gripper_position_command_schema();
  return {schema.command_schema_id,
          schema.command_schema_revision,
          GripperPositionCommandValueDTO{AngularPositionDTO{0.2}, AngularEffortDTO{1.0}},
          {},
          {},
          {},
          2.0,
          schema.completion_rule,
          schema.confirmation_rule,
          schema.stop_evidence_requirements};
}

[[nodiscard]] PreparedOperationParticipantDTO Candidate(
    const RuntimeParticipantBinding& sealed_binding,
    const RuntimeParticipantStopReadiness& readiness, PreparedParticipantPayloadDTO payload) {
  const auto& binding = sealed_binding.value();
  const auto& path = *binding.control_path;
  std::vector<ResourceId> resources;
  for (const auto& row : binding.resources) resources.push_back(row.resource_id);
  std::sort(resources.begin(), resources.end());
  PreparedParticipantStopPlanContextDTO stop_context{OperationId("operation.1"),
                                                     OperationRevision(1U),
                                                     OperationParticipantId("participant.1"),
                                                     binding.program_id,
                                                     binding.program_revision,
                                                     binding.program_binding_id,
                                                     ProgramRunId("run.1"),
                                                     std::nullopt,
                                                     ParticipantStopProfilePurposeDTO::kOrdinary,
                                                     std::nullopt,
                                                     MotionInterruptNodeCategory::kGeneric};
  auto stop = MakePreparedParticipantStopPlan(readiness.value().resolved_stop_base_plan,
                                              std::move(stop_context), binding.resources, {});
  if (!stop.ok()) throw std::logic_error(stop.error().message);
  const auto& domain = readiness.value().expected_controller_domain;
  const auto& interface_binding = domain.expected_bindings.front();
  PreparedParticipantSelectedRouteDTO route{domain.expected_endpoints};
  return {ContentDigest("pending"),
          OperationId("operation.1"),
          OperationRevision(1U),
          OperationParticipantId("participant.1"),
          binding.program_id,
          binding.program_revision,
          binding.program_binding_id,
          ProgramRunId("run.1"),
          binding.retained_bundle_id,
          binding.graph_revision,
          binding.controllable_component_id,
          readiness.value().component_lifecycle_generation,
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
          {path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
           path.provider_runtime_unit_id, path.provider_runtime_epoch},
          std::nullopt,
          std::move(resources),
          std::move(route),
          interface_binding.claimed_command_interfaces,
          interface_binding.claimed_state_interfaces,
          path.active_control_configuration_id,
          path.active_control_configuration_generation,
          binding.runtime_unit_epochs,
          path.stop_evidence_policy_id,
          path.stop_evidence_policy_revision,
          std::move(stop.value()),
          std::move(payload)};
}

[[nodiscard]] PreparedMotionExecutionDTO MotionPayload(const SealedInputs& inputs) {
  const auto& binding = inputs.binding.value();
  const auto& path = *binding.control_path;
  const auto& planner = binding.supporting_capability_provider_paths.front();
  std::vector<ResourceId> resources;
  std::vector<std::string> joints;
  std::vector<CoordinatePositionDTO> positions;
  for (const auto& row : binding.resources) {
    resources.push_back(row.resource_id);
    joints.push_back(*row.source_joint_name);
    positions.emplace_back(AngularPositionDTO{0.0});
  }
  std::sort(resources.begin(), resources.end());
  ControlledStateDTO state{binding.controllable_component_id,
                           binding.command_target_id,
                           path.control_domain_id,
                           path.control_domain_revision,
                           path.control_binding_id,
                           path.control_binding_revision,
                           path.controller_topology_id,
                           path.topology_revision,
                           path.control_route_id,
                           path.active_control_configuration_id,
                           path.active_control_configuration_generation,
                           resources,
                           joints,
                           positions,
                           std::nullopt};
  PlannerCapabilityBindingDTO planner_binding{planner.capability_id,
                                              planner.capability_definition_revision,
                                              planner.capability_provider_binding_id,
                                              planner.capability_provider_binding_revision,
                                              planner.provider_instance_id,
                                              planner.provider_manifest_id,
                                              planner.provider_manifest_revision,
                                              planner.provider_runtime_unit_id,
                                              planner.provider_runtime_epoch};
  std::vector<PlannerRuntimeUnitBindingDTO> runtime_units;
  for (const auto& row : binding.runtime_unit_epochs)
    runtime_units.push_back({row.runtime_unit_id, row.runtime_epoch});
  ProjectionRevisionDTO projection{
      binding.retained_bundle_id, binding.graph_revision,
      SceneRevision{SceneId(Digest('d')), 1U, ContentDigest(Digest('e'))}};
  PlanRequestDTO request{
      OperationId("operation.1"),
      OperationParticipantId("participant.1"),
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      ProgramRunId("run.1"),
      MoveId("move.1"),
      binding.controllable_component_id,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      planner_binding,
      {{binding.model_bindings.front().model_instance_id,
        binding.model_bindings.front().model_revision}},
      {binding.model_groups.front().model_group_role_id,
       binding.model_groups.front().model_instance_id,
       binding.model_bindings.front().model_revision,
       binding.model_groups.front().compiled_group_name, joints},
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
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_epoch,
      runtime_units,
      resources,
      std::nullopt,
      ResolvedComponentModelFrameDTO{
          binding.controllable_component_id, binding.model_bindings.front().model_instance_id,
          FrameRoleId("base"), "robot_base", binding.model_bindings.front().model_revision},
      ResolvedComponentModelFrameDTO{
          binding.controllable_component_id, binding.model_bindings.front().model_instance_id,
          FrameRoleId("tcp"), "robot_tcp", binding.model_bindings.front().model_revision},
      state,
      ResolvedJointGoalDTO{resources, joints, {0.5, -0.5}},
      TolerancesDTO{0.01, 0.01, 0.01},
      SpeedProfileDTO{0.5, 0.5, std::nullopt},
      PlanningProfileDTO::kFirstFeasible,
      std::nullopt,
      {},
      2.0,
      0U,
      ProjectionLeaseBindingDTO{projection, ProjectionLeaseId("lease.1")},
      {},
      {}};
  TrajectoryDTO trajectory{
      resources, joints, {{0.0, {0.0, 0.0}, {}, {}}, {1.0, {0.5, -0.5}, {0.0, 0.0}, {0.0, 0.0}}}};
  CollisionWorldRevision world{1U, Digest('f')};
  CollisionDecision decision;
  decision.accepted = true;
  decision.severity = CollisionDecisionSeverity::kNone;
  decision.action = CollisionDecisionAction::kDispatch;
  decision.collision_world_revision = world;
  return {MoveId("move.1"),
          request,
          trajectory,
          state,
          {PlanResultDTO{trajectory, std::nullopt, projection}, 0U, planner_binding},
          {ValidationRevision("validation.1"), ValidationResultDTO{true, std::nullopt}, world,
           CollisionPolicyRevision{1U, Digest('1')}, decision},
          std::nullopt,
          {RuntimePolicyId("runtime.policy"), RuntimePolicyRevision(Digest('2'))},
          {},
          3.0};
}

TEST(PreparedOperationParticipantCodecTest, SealsAndRoundTripsBothPayloadTags) {
  const auto base = BasePlan();
  auto command_inputs = Inputs(base, true);
  auto command = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, {}, MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command.ok()) << command.error().details.at("reason");
  const auto command_bytes = EncodePreparedOperationParticipant(
      command.value(), command_inputs.binding, command_inputs.readiness, {},
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_bytes.ok()) << command_bytes.error().message;
  const auto golden = ParticipantGolden();
  EXPECT_EQ(content_identity::sha256_digest(command_bytes.value()), golden.first);
  EXPECT_EQ(command.value().prepared_contract_digest.value(), golden.second);
  const auto command_decoded = DecodePreparedOperationParticipant(
      command_bytes.value(), command_inputs.binding, command_inputs.readiness, {},
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_decoded.ok()) << command_decoded.error().message;
  EXPECT_EQ(EncodePreparedOperationParticipant(command_decoded.value(), command_inputs.binding,
                                               command_inputs.readiness, {},
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());

  auto motion_inputs = Inputs(base, false);
  auto motion = MakePreparedOperationParticipant(
      Candidate(motion_inputs.binding, motion_inputs.readiness, MotionPayload(motion_inputs)),
      motion_inputs.binding, motion_inputs.readiness, {}, MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion.ok()) << motion.error().details.at("reason");
  const auto motion_bytes = EncodePreparedOperationParticipant(
      motion.value(), motion_inputs.binding, motion_inputs.readiness, {},
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_bytes.ok()) << motion_bytes.error().message;
  EXPECT_TRUE(DecodePreparedOperationParticipant(motion_bytes.value(), motion_inputs.binding,
                                                 motion_inputs.readiness, {},
                                                 MotionInterruptNodeCategory::kGeneric)
                  .ok());
}

TEST(PreparedOperationParticipantCodecTest, RejectsSelfAttestedClaimsAndDigestForgery) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, true);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, CommandPayload()), inputs.binding,
      inputs.readiness, {}, MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  auto forged = made.value();
  forged.claimed_command_interfaces.front().interface_name = "velocity";
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness, {},
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
  forged = made.value();
  forged.prepared_contract_digest = ContentDigest(Digest('0'));
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness, {},
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, BackendBindingRejectsCrossedParticipant) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, {}, MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  EXPECT_FALSE(
      prepared_participant_backend_binding_field_error(
          made.value(), inputs.binding, PreparedParticipantPayloadKindDTO::kMotionExecution)
          .has_value());
  EXPECT_EQ(
      prepared_participant_backend_binding_field_error(
          made.value(), inputs.binding, PreparedParticipantPayloadKindDTO::kCapabilityCommand),
      "prepared_participant_backend_shape_invalid");

  auto crossed = made.value();
  crossed.control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepared_participant_backend_binding_field_error(
                crossed, inputs.binding, PreparedParticipantPayloadKindDTO::kMotionExecution),
            "prepared_participant_backend_binding_mismatch");
}

}  // namespace
}  // namespace manyforge::core
