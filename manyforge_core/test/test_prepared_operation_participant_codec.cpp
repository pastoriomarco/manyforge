#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

[[nodiscard]] std::span<const HardStopResourceLimitDTO> DefaultLimits() {
  static const std::array<HardStopResourceLimitDTO, 2> limits{
      {{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
       {ResourceId("joint.2"), AngularVelocityDTO{0.02}}}};
  return limits;
}

[[nodiscard]] RuntimePolicyCatalogRecordRefDTO PolicyReference(const RuntimePolicyDTO& policy) {
  return {policy.runtime_policy_id,
          policy.runtime_policy_revision,
          SchemaId(std::string(kRuntimePolicyApiVersion)),
          SchemaRevision(Digest('3')),
          ContentDigest(Digest('4')),
          ContentAddressedStoreKey("catalog/runtime-policy/runtime.policy.json")};
}

[[nodiscard]] ResolvedStopBasePlanDTO BasePlan() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) bytes.pop_back();
  auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) throw std::logic_error(decoded.error().message);
  auto base = std::move(decoded.value());
  // The frozen historical codec fixture remains decodable, but its opaque
  // Stop parameters cannot authorize a current operation. Supply the current
  // typed contract and complete defaults before testing Prepare.
  auto& nominal = base.escalation_stages.front();
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("stop.command"),
      SchemaId(std::string(kMotionControlledStopContractSchemaId)),
      SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
  nominal.containment_command.provider_contract_ref = contract;
  nominal.command_qualification.controlled_substitution_contract->provider_contract_ref = contract;
  JsonValueDTO::Array limits;
  for (const auto& resource : {std::string("joint.1"), std::string("joint.2")}) {
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{resource}},
        {"max_deceleration", JsonValueDTO{JsonObjectDTO{
                                 {"coordinate_kind", JsonValueDTO{std::string("angular_rad_s2")}},
                                 {"value_rad_s2", JsonValueDTO{2.0}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                                    {"value_rad_s", JsonValueDTO{0.02}}}}}});
  }
  nominal.default_contract_parameters =
      JsonObjectDTO{{"stop_mode", JsonValueDTO{std::string("soft_on_path")}},
                    {"jerk_limits_enabled", JsonValueDTO{false}},
                    {"deceleration_time_s", JsonValueDTO{1.0}},
                    {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
                    {"per_resource_limits", JsonValueDTO{std::move(limits)}}};
  auto current = MakeResolvedStopBasePlan(std::move(base));
  if (!current.ok()) throw std::logic_error(current.error().message);
  return std::move(current.value());
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
  std::optional<RuntimeParticipantPolicySelection> runtime_policy;
};

[[nodiscard]] SealedInputs Inputs(const ResolvedStopBasePlanDTO& base, bool command) {
  auto binding = Binding(base, command);
  auto domain = ExpectedDomain(base, binding);
  auto readiness = verify_runtime_participant_stop_readiness(
      {binding.value().controllable_component_id, ComponentLifecycleGeneration(9U), domain.value(),
       base},
      binding, domain);
  if (!readiness.ok()) throw std::logic_error(readiness.error().message);
  std::optional<RuntimeParticipantPolicySelection> runtime_policy;
  if (!command) {
    RuntimePolicyDTO policy{RuntimePolicyId("runtime.policy"),
                            RuntimePolicyRevision(Digest('2')),
                            std::nullopt,
                            {},
                            {RuntimeMotionSpeedLimitDTO{RuntimePolicyCellScopeDTO{}, 0.5}},
                            {}};
    const auto policy_reference = PolicyReference(policy);
    auto selected =
        verify_runtime_participant_policy_selection(policy_reference, std::move(policy), binding);
    if (!selected.ok()) throw std::logic_error(selected.error().message);
    runtime_policy = std::move(selected.value());
  }
  return {std::move(binding), std::move(domain), std::move(readiness.value()),
          std::move(runtime_policy)};
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
  auto stop =
      MakePreparedParticipantStopPlan(readiness.value().resolved_stop_base_plan,
                                      std::move(stop_context), binding.resources, DefaultLimits());
  if (!stop.ok()) throw std::logic_error(stop.error().details.at("reason"));
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
          std::move(payload),
          std::nullopt};
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
      SpeedProfileDTO{0.5, 0.5, 0.5},
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
          {ValidationRevision("validation.1"),
           ValidationResultDTO{true, std::nullopt,
                               TrajectoryValidationMetricsDTO{1.0, {0.5, 0.5}, {0.0, 0.0}, 0.1}},
           world, CollisionPolicyRevision{1U, Digest('1')}, decision},
          std::nullopt,
          {inputs.runtime_policy->value().source_artifact,
           inputs.runtime_policy->value().selected_motion_speed_limit},
          {},
          3.0};
}

TEST(PreparedOperationParticipantCodecTest, SealsAndRoundTripsBothPayloadTags) {
  const auto base = BasePlan();
  auto command_inputs = Inputs(base, true);
  auto command = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, nullptr, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command.ok()) << command.error().details.at("reason");
  const auto command_bytes = EncodePreparedOperationParticipant(
      command.value(), command_inputs.binding, command_inputs.readiness, nullptr, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_bytes.ok()) << command_bytes.error().message;
  // Current candidates prove deterministic construction and round-trip
  // integrity; the obsolete pre-typed-Stop aggregate hash is not a contract.
  const auto rebuilt = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, nullptr, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(rebuilt.ok());
  EXPECT_EQ(EncodePreparedOperationParticipant(rebuilt.value(), command_inputs.binding,
                                               command_inputs.readiness, nullptr, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());
  const auto command_decoded = DecodePreparedOperationParticipant(
      command_bytes.value(), command_inputs.binding, command_inputs.readiness, nullptr,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_decoded.ok()) << command_decoded.error().message;
  EXPECT_EQ(EncodePreparedOperationParticipant(command_decoded.value(), command_inputs.binding,
                                               command_inputs.readiness, nullptr, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());

  auto motion_inputs = Inputs(base, false);
  auto motion = MakePreparedOperationParticipant(
      Candidate(motion_inputs.binding, motion_inputs.readiness, MotionPayload(motion_inputs)),
      motion_inputs.binding, motion_inputs.readiness, &*motion_inputs.runtime_policy,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion.ok()) << motion.error().details.at("reason");
  const auto motion_bytes = EncodePreparedOperationParticipant(
      motion.value(), motion_inputs.binding, motion_inputs.readiness,
      &*motion_inputs.runtime_policy, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_bytes.ok()) << motion_bytes.error().message;
  const auto motion_decoded = DecodePreparedOperationParticipant(
      motion_bytes.value(), motion_inputs.binding, motion_inputs.readiness,
      &*motion_inputs.runtime_policy, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_decoded.ok()) << motion_decoded.error().message;
  const auto& decoded_payload =
      std::get<PreparedMotionExecutionDTO>(motion_decoded.value().payload);
  EXPECT_EQ(decoded_payload.move_id.value(), "move.1");
  ASSERT_TRUE(
      decoded_payload.validation_evidence.noncollision_result.trajectory_metrics.has_value());
  const auto& metrics = *decoded_payload.validation_evidence.noncollision_result.trajectory_metrics;
  EXPECT_DOUBLE_EQ(metrics.trajectory_duration_s, 1.0);
  EXPECT_EQ(metrics.peak_abs_joint_velocity_rad_s, (std::vector<double>{0.5, 0.5}));
  EXPECT_EQ(metrics.peak_abs_joint_acceleration_rad_s2, (std::vector<double>{0.0, 0.0}));
  ASSERT_TRUE(metrics.peak_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(*metrics.peak_tcp_linear_speed_mps, 0.1);
  EXPECT_EQ(decoded_payload.runtime_policy.source_artifact.record_id.value(), "runtime.policy");
  EXPECT_EQ(decoded_payload.runtime_policy.source_artifact.record_revision.value(), Digest('2'));
  EXPECT_EQ(decoded_payload.runtime_policy.source_artifact.content_addressed_store_key.value(),
            "catalog/runtime-policy/runtime.policy.json");
  EXPECT_TRUE(std::holds_alternative<RuntimePolicyCellScopeDTO>(
      decoded_payload.runtime_policy.selected_motion_speed_limit.scope));
  EXPECT_DOUBLE_EQ(
      decoded_payload.runtime_policy.selected_motion_speed_limit.maximum_tcp_linear_speed_mps, 0.5);
}

TEST(PreparedOperationParticipantCodecTest, RejectsCartesianMilestonesForAnotherIntent) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto payload = MotionPayload(inputs);
  payload.trajectory.cartesian_waypoint_times_s = {0.0, 1.0};
  const auto seal = [&] {
    return MakePreparedOperationParticipant(
        Candidate(inputs.binding, inputs.readiness, payload), inputs.binding, inputs.readiness,
        &*inputs.runtime_policy, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  };
  auto wrong_kind = seal();
  ASSERT_FALSE(wrong_kind.ok());
  EXPECT_EQ(wrong_kind.error().details.at("reason"),
            "prepared_cartesian_waypoint_milestones_mismatch");
  CartesianPathDTO path;
  path.waypoints.resize(3U);
  payload.plan_request.motion = path;
  auto wrong_count = seal();
  ASSERT_FALSE(wrong_count.ok());
  EXPECT_EQ(wrong_count.error().details.at("reason"),
            "prepared_cartesian_waypoint_milestones_mismatch");
}

TEST(PreparedOperationParticipantCodecTest, RejectsSelfAttestedClaimsAndDigestForgery) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, true);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, CommandPayload()), inputs.binding,
      inputs.readiness, nullptr, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  auto forged = made.value();
  forged.claimed_command_interfaces.front().interface_name = "velocity";
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness, nullptr,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
  forged = made.value();
  forged.prepared_contract_digest = ContentDigest(Digest('0'));
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness, nullptr,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, BackendBindingRejectsCrossedParticipant) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, &*inputs.runtime_policy, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
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
            "prepared_participant_backend_control_route_id_mismatch");
}

TEST(PreparedOperationParticipantCodecTest,
     BackendPhysicalBindingAcceptsContainedProgramAndRejectsCrossedRoute) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, &*inputs.runtime_policy, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());

  auto physical_value = inputs.binding.value();
  physical_value.program_id = ProgramId("direct-control.product-owner");
  physical_value.program_revision = ProgramRevision(Digest('e'));
  physical_value.program_binding_id = ProgramBindingId(Digest('f'));
  physical_value.component_role_id = ComponentRoleId("direct_control_component");
  physical_value.target_role_id = TargetRoleId("direct_control_target");
  auto physical = verify_runtime_participant_binding(std::move(physical_value));
  ASSERT_TRUE(physical.ok()) << physical.error().message;

  EXPECT_EQ(
      prepared_participant_backend_binding_field_error(
          made.value(), physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution),
      "prepared_participant_backend_program_id_mismatch");
  EXPECT_FALSE(
      prepared_participant_backend_physical_binding_field_error(
          made.value(), physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution)
          .has_value());

  auto crossed = made.value();
  crossed.control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepared_participant_backend_physical_binding_field_error(
                crossed, physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution),
            "prepared_participant_backend_control_route_id_mismatch");
}

TEST(PreparedOperationParticipantCodecTest,
     MotionRequiresExactSealedRuntimePolicyReferentAndEffectiveCeiling) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  const auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));

  auto missing =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, nullptr,
                                       DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"), "runtime_policy_selection_required");

  auto foreign = candidate;
  std::get<PreparedMotionExecutionDTO>(foreign.payload)
      .runtime_policy.source_artifact.record_revision = RuntimePolicyRevision(Digest('0'));
  auto foreign_result = MakePreparedOperationParticipant(
      std::move(foreign), inputs.binding, inputs.readiness, &*inputs.runtime_policy,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(foreign_result.ok());
  EXPECT_EQ(foreign_result.error().details.at("reason"), "motion_payload_join_invalid");

  auto wrong_source = candidate;
  std::get<PreparedMotionExecutionDTO>(wrong_source.payload)
      .runtime_policy.source_artifact.content_addressed_store_key =
      ContentAddressedStoreKey("catalog/runtime-policy/other.json");
  auto wrong_source_result = MakePreparedOperationParticipant(
      std::move(wrong_source), inputs.binding, inputs.readiness, &*inputs.runtime_policy,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(wrong_source_result.ok());
  EXPECT_EQ(wrong_source_result.error().details.at("reason"), "motion_payload_join_invalid");

  auto wrong_scope = candidate;
  std::get<PreparedMotionExecutionDTO>(wrong_scope.payload)
      .runtime_policy.selected_motion_speed_limit.scope = RuntimePolicyCommandTargetScopeDTO{
      ControllableComponentId("robot.1"), CommandTargetId("other")};
  auto wrong_scope_result = MakePreparedOperationParticipant(
      std::move(wrong_scope), inputs.binding, inputs.readiness, &*inputs.runtime_policy,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(wrong_scope_result.ok());
  EXPECT_EQ(wrong_scope_result.error().details.at("reason"), "motion_payload_join_invalid");

  auto weaker = candidate;
  std::get<PreparedMotionExecutionDTO>(weaker.payload)
      .plan_request.speed_profile.max_tcp_linear_speed_mps = 0.75;
  auto weaker_result = MakePreparedOperationParticipant(
      std::move(weaker), inputs.binding, inputs.readiness, &*inputs.runtime_policy, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(weaker_result.ok());
  EXPECT_EQ(weaker_result.error().details.at("reason"), "motion_payload_join_invalid");
}

}  // namespace
}  // namespace manyforge::core
