#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/io_rule_stop_response.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
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
        ProviderInstanceId("planner.1"), ProviderManifestId("planner.manifest"),
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
      PlannerOverrideDTO{},
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
          0.5,
          0.5,
          {},
          3.0};
}

[[nodiscard]] PreparedMotionExecutionDTO MovingPayload(const SealedInputs& inputs) {
  auto payload = MotionPayload(inputs);
  auto& trajectory = payload.trajectory;
  trajectory.waypoints.front().velocities_rad_s = {0.1, -0.1};
  trajectory.waypoints.front().accelerations_rad_s2 = {0.02, 0.0};
  trajectory.waypoints.insert(trajectory.waypoints.begin() + 1,
                              TrajectoryWaypointDTO{0.25, {0.05, -0.05}, {0.1, -0.1}, {0.0, 0.0}});
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0};
  payload.start_state.velocities =
      std::vector<CoordinateVelocityDTO>{AngularVelocityDTO{0.1}, AngularVelocityDTO{-0.1}};
  payload.plan_request.start_state = payload.start_state;
  auto suffix = payload.start_state;
  suffix.positions = {AngularPositionDTO{0.05}, AngularPositionDTO{-0.05}};
  suffix.velocities =
      std::vector<CoordinateVelocityDTO>{AngularVelocityDTO{0.0}, AngularVelocityDTO{0.0}};
  auto prefix = trajectory;
  prefix.waypoints.pop_back();
  prefix.runtime_scale_bounds.reset();
  payload.plan_request.moving_replacement =
      MovingReplacementInputDTO{{ControllerGoalId("old.goal"), ControllerGoalGeneration(2U)},
                                ExecutionBindingEpoch(3U),
                                2.0,
                                std::move(prefix),
                                2.25,
                                std::move(suffix),
                                payload.plan_request.base_frame,
                                payload.plan_request.tcp_frame,
                                SpeedProfileDTO{1.0, 1.0, 0.8},
                                {},
                                1.0};
  payload.planning_evidence.planning_result.trajectory = trajectory;
  auto& validated = payload.validation_evidence.noncollision_result;
  validated.trajectory_metrics->peak_tcp_linear_speed_mps = 0.7;
  validated.moving_replacement = MovingReplacementValidationEvidenceDTO{
      TrajectoryValidationMetricsDTO{0.25, {0.3, 0.3}, {0.0, 0.0}, 0.7},
      TrajectoryValidationMetricsDTO{0.75, {0.5, 0.5}, {0.0, 0.0}, 0.1}};
  return payload;
}

TEST(PreparedOperationParticipantCodecTest, SealsAndEncodesBothPayloadTags) {
  const auto base = BasePlan();
  auto command_inputs = Inputs(base, true);
  auto command = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command.ok()) << command.error().details.at("reason");
  const auto command_bytes = EncodePreparedOperationParticipant(
      command.value(), command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_bytes.ok()) << command_bytes.error().message;
  // Current candidates prove deterministic construction; the obsolete pre-typed-Stop aggregate hash is not a contract.
  const auto rebuilt = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(rebuilt.ok());
  EXPECT_EQ(EncodePreparedOperationParticipant(rebuilt.value(), command_inputs.binding,
                                               command_inputs.readiness, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());

  auto motion_inputs = Inputs(base, false);
  auto capped_payload = MotionPayload(motion_inputs);
  capped_payload.requested_tcp_linear_speed_mps = 0.75;
  auto motion = MakePreparedOperationParticipant(
      Candidate(motion_inputs.binding, motion_inputs.readiness, capped_payload),
      motion_inputs.binding, motion_inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion.ok()) << motion.error().details.at("reason");
  const auto motion_bytes = EncodePreparedOperationParticipant(
      motion.value(), motion_inputs.binding, motion_inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_bytes.ok()) << motion_bytes.error().message;
  EXPECT_EQ(motion_bytes.value().find("\"motion_endpoint\""), std::string::npos);
  const auto& sealed_payload = std::get<PreparedMotionExecutionDTO>(motion.value().payload);
  EXPECT_EQ(sealed_payload.move_id.value(), "move.1");
  ASSERT_TRUE(
      sealed_payload.validation_evidence.noncollision_result.trajectory_metrics.has_value());
  const auto& metrics = *sealed_payload.validation_evidence.noncollision_result.trajectory_metrics;
  EXPECT_DOUBLE_EQ(metrics.trajectory_duration_s, 1.0);
  EXPECT_EQ(metrics.peak_abs_joint_velocity_rad_s, (std::vector<double>{0.5, 0.5}));
  EXPECT_EQ(metrics.peak_abs_joint_acceleration_rad_s2, (std::vector<double>{0.0, 0.0}));
  ASSERT_TRUE(metrics.peak_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(*metrics.peak_tcp_linear_speed_mps, 0.1);
  ASSERT_TRUE(sealed_payload.requested_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(*sealed_payload.requested_tcp_linear_speed_mps, 0.75);
  EXPECT_DOUBLE_EQ(sealed_payload.deployment_maximum_tcp_linear_speed_mps, 0.5);
  EXPECT_EQ(sealed_payload.plan_request.speed_profile.max_tcp_linear_speed_mps, 0.5);
}

TEST(PreparedOperationParticipantCodecTest, RejectsCartesianMilestonesForAnotherIntent) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto payload = MotionPayload(inputs);
  payload.trajectory.cartesian_waypoint_times_s = {0.0, 1.0};
  const auto seal = [&] {
    return MakePreparedOperationParticipant(
        Candidate(inputs.binding, inputs.readiness, payload), inputs.binding, inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
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
      inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  auto forged = made.value();
  forged.controller().claimed_command_interfaces.front().interface_name = "velocity";
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
  forged = made.value();
  forged.prepared_contract_digest = ContentDigest(Digest('0'));
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, BackendBindingRejectsCrossedParticipant) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, DefaultLimits(),
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
  crossed.controller().control_route_id = ControlRouteId("route.other");
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
      inputs.readiness, DefaultLimits(),
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

  // mock_ur5_pick_and_place: a second role (the UR5 `flange`) on the compiled frame
  // of an existing role (its `tcp`) still contains the Program participant.
  auto shared_value = physical.value().value();
  ASSERT_FALSE(shared_value.frame_bindings.empty());
  auto shared_frame = shared_value.frame_bindings.back();
  shared_frame.frame_role_id = FrameRoleId("shared_frame_role");
  shared_value.frame_bindings.push_back(std::move(shared_frame));
  auto shared = verify_runtime_participant_binding(std::move(shared_value));
  ASSERT_TRUE(shared.ok()) << shared.error().message;
  EXPECT_FALSE(
      prepared_participant_backend_physical_binding_field_error(
          made.value(), shared.value(), PreparedParticipantPayloadKindDTO::kMotionExecution)
          .has_value());

  auto crossed = made.value();
  crossed.controller().control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepared_participant_backend_physical_binding_field_error(
                crossed, physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution),
            "prepared_participant_backend_control_route_id_mismatch");
}

TEST(PreparedOperationParticipantCodecTest,
     MotionRefusesUnqualifiedScopesAndRequiresDeploymentMaximum) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  const auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));

  auto interrupted = candidate;
  std::get<PreparedMotionExecutionDTO>(interrupted.payload)
      .interrupt_scopes.push_back(
          PreparedMotionInterruptScopeDTO{MotionInterruptScopeId("scope.1"),
                                          InterruptPolicyId("interrupt.contact"),
                                          MotionInterruptActivationMode::kRequireInactiveAtArm,
                                          std::nullopt,
                                          true,
                                          MotionInterruptStopProfileDTO{},
                                          MotionInterruptNodeCategory::kGeneric,
                                          {},
                                          candidate.controller().ordinary_stop_plan});
  auto interrupted_result =
      MakePreparedOperationParticipant(std::move(interrupted), inputs.binding, inputs.readiness,
                                       DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(interrupted_result.ok());
  EXPECT_EQ(interrupted_result.error().details.at("reason"), "motion_interrupt_scope_invalid");

  auto weaker = candidate;
  std::get<PreparedMotionExecutionDTO>(weaker.payload)
      .plan_request.speed_profile.max_tcp_linear_speed_mps = 0.75;
  auto weaker_result =
      MakePreparedOperationParticipant(std::move(weaker), inputs.binding, inputs.readiness,
                                       DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(weaker_result.ok());
  EXPECT_EQ(weaker_result.error().details.at("reason"), "motion_payload_join_invalid");
  auto unexpectedly_lower = candidate;
  std::get<PreparedMotionExecutionDTO>(unexpectedly_lower.payload)
      .plan_request.speed_profile.max_tcp_linear_speed_mps = 0.25;
  EXPECT_FALSE(MakePreparedOperationParticipant(std::move(unexpectedly_lower), inputs.binding,
                                                inputs.readiness, DefaultLimits(),
                                                MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest,
     MotionScopeJoinsExactPreparedStopAndRejectsCrossedOwners) {
  const auto inputs = Inputs(BasePlan(), false);
  auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "search.nominal";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.jerk_limits_enabled = false;
  profile.deceleration_time_s = 1.0;
  profile.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                        {"joint_2", AngularAccelerationDTO{2.0}}};
  PreparedParticipantStopPlanContextDTO context{
      candidate.operation_id,
      candidate.operation_revision,
      candidate.participant_id,
      candidate.program_id,
      candidate.program_revision,
      candidate.program_binding_id,
      candidate.program_run_id,
      profile,
      ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
      std::string("search.scope"),
      MotionInterruptNodeCategory::kGeneric};
  auto stop =
      MakePreparedParticipantStopPlan(inputs.readiness.value().resolved_stop_base_plan, context,
                                      inputs.binding.value().resources, DefaultLimits());
  ASSERT_TRUE(stop.ok()) << stop.error().message;
  std::get<PreparedMotionExecutionDTO>(candidate.payload)
      .interrupt_scopes.push_back({MotionInterruptScopeId("search.scope"),
                                   InterruptPolicyId("search.policy"),
                                   MotionInterruptActivationMode::kFreshEdgeAfterArm,
                                   std::nullopt,
                                   true,
                                   profile,
                                   MotionInterruptNodeCategory::kGeneric,
                                   {},
                                   stop.value()});
  const auto made =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, DefaultLimits(),
                                       MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok()) << made.error().message;
  const auto encoded =
      EncodePreparedOperationParticipant(made.value(), inputs.binding, inputs.readiness,
                                         DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(encoded.ok());
  EXPECT_EQ(encoded.value().find("result_binding_key"), std::string::npos);
  const auto& scope =
      std::get<PreparedMotionExecutionDTO>(made.value().payload).interrupt_scopes.front();
  EXPECT_EQ(scope.stop_plan, stop.value());
  EXPECT_EQ(scope.activation_mode, MotionInterruptActivationMode::kFreshEdgeAfterArm);
  auto crossed_context = context;
  crossed_context.operation_id = OperationId("other.operation");
  const auto crossed_stop = MakePreparedParticipantStopPlan(
      inputs.readiness.value().resolved_stop_base_plan, crossed_context,
      inputs.binding.value().resources, DefaultLimits());
  ASSERT_TRUE(crossed_stop.ok());
  auto crossed = candidate;
  std::get<PreparedMotionExecutionDTO>(crossed.payload).interrupt_scopes.front().stop_plan =
      crossed_stop.value();
  EXPECT_FALSE(MakePreparedOperationParticipant(crossed, inputs.binding, inputs.readiness,
                                                DefaultLimits(),
                                                MotionInterruptNodeCategory::kGeneric)
                   .ok());
  auto mismatched_profile = candidate;
  std::get<PreparedMotionExecutionDTO>(mismatched_profile.payload)
      .interrupt_scopes.front()
      .stop_profile->profile_id = "unbound.profile";
  EXPECT_FALSE(MakePreparedOperationParticipant(mismatched_profile, inputs.binding,
                                                inputs.readiness, DefaultLimits(),
                                                MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest,
     FaultScopeUsesOrdinaryPlanWithoutDesiredProfileAndRejectsCrossedAction) {
  const auto inputs = Inputs(BasePlan(), false);
  auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));
  auto prepared =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, DefaultLimits(),
                                       MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  candidate = prepared.value();
  auto& scopes = std::get<PreparedMotionExecutionDTO>(candidate.payload).interrupt_scopes;
  scopes.push_back({MotionInterruptScopeId("fault.scope"),
                    InterruptPolicyId("fault.policy"),
                    MotionInterruptActivationMode::kRequireInactiveAtArm,
                    std::nullopt,
                    true,
                    std::nullopt,
                    MotionInterruptNodeCategory::kGeneric,
                    {},
                    candidate.controller().ordinary_stop_plan,
                    InterruptPreemptionAction::kPreemptTrajectory});
  const auto sealed =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, DefaultLimits(),
                                       MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;
  candidate = sealed.value();
  const auto bytes =
      EncodePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness,
                                         DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_NE(bytes.value().find("\"preemption_action\":\"preempt_trajectory\""), std::string::npos);
  const auto& scope =
      std::get<PreparedMotionExecutionDTO>(candidate.payload).interrupt_scopes.front();
  EXPECT_FALSE(scope.stop_profile);
  EXPECT_EQ(scope.preemption_action, InterruptPreemptionAction::kPreemptTrajectory);
  EXPECT_EQ(scope.stop_plan, candidate.controller().ordinary_stop_plan);
  auto crossed = candidate;
  std::get<PreparedMotionExecutionDTO>(crossed.payload).interrupt_scopes.front().preemption_action =
      InterruptPreemptionAction::kCompleteMoveOnSignal;
  EXPECT_FALSE(MakePreparedOperationParticipant(crossed, inputs.binding, inputs.readiness,
                                                DefaultLimits(),
                                                MotionInterruptNodeCategory::kGeneric)
                   .ok());
  crossed = candidate;
  std::get<PreparedMotionExecutionDTO>(crossed.payload)
      .interrupt_scopes.front()
      .stop_plan.operation_id = OperationId("other.operation");
  EXPECT_FALSE(MakePreparedOperationParticipant(crossed, inputs.binding, inputs.readiness,
                                                DefaultLimits(),
                                                MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, MovingPrefixKeepsItsCapAndExactPreparedResultIdentity) {
  const auto inputs = Inputs(BasePlan(), false);
  const auto payload = MovingPayload(inputs);
  auto made = MakePreparedOperationParticipant(Candidate(inputs.binding, inputs.readiness, payload),
                                               inputs.binding, inputs.readiness, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok()) << made.error().details.at("reason");
  const auto bytes =
      EncodePreparedOperationParticipant(made.value(), inputs.binding, inputs.readiness,
                                         DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(bytes.ok()) << bytes.error().details.at("reason");
  const auto& restored = std::get<PreparedMotionExecutionDTO>(made.value().payload);
  ASSERT_TRUE(restored.plan_request.moving_replacement);
  ASSERT_TRUE(restored.validation_evidence.noncollision_result.moving_replacement);
  const auto* context = &*restored.plan_request.moving_replacement;
  EXPECT_EQ(
      EncodePreparedTrajectory(restored.trajectory, context).value(),
      EncodePreparedTrajectory(*restored.planning_evidence.planning_result.trajectory, context)
          .value());
  EXPECT_DOUBLE_EQ(*restored.validation_evidence.noncollision_result.trajectory_metrics
                        ->peak_tcp_linear_speed_mps,
                   0.7);
  EXPECT_DOUBLE_EQ(*restored.plan_request.speed_profile.max_tcp_linear_speed_mps, 0.5);
}

TEST(PreparedOperationParticipantCodecTest,
     MovingContextMetricsAndPlanningCannotBeCrossedOrOmitted) {
  const auto inputs = Inputs(BasePlan(), false);
  for (int mutation = 0; mutation < 10; ++mutation) {
    SCOPED_TRACE(mutation);
    auto payload = MovingPayload(inputs);
    auto& result = payload.validation_evidence.noncollision_result;
    if (mutation == 0) result.moving_replacement.reset();
    if (mutation == 1) payload.plan_request.moving_replacement.reset();
    if (mutation == 2) result.moving_replacement->unchanged_prefix.trajectory_duration_s += 0.1;
    if (mutation == 3) result.moving_replacement->changed_future.peak_tcp_linear_speed_mps = 0.6;
    if (mutation == 4) result.moving_replacement->unchanged_prefix.peak_tcp_linear_speed_mps = 0.9;
    if (mutation == 5) result.trajectory_metrics->peak_abs_joint_velocity_rad_s[0] += 0.1;
    if (mutation == 6)
      result.moving_replacement->unchanged_prefix.peak_abs_joint_acceleration_rad_s2.clear();
    if (mutation == 7)
      payload.planning_evidence.planning_result.trajectory->waypoints.back().positions_rad[0] +=
          0.01;
    if (mutation == 8) payload.trajectory.waypoints.front().time_from_start_s = -0.1;
    if (mutation == 9) payload.trajectory.waypoints.front().accelerations_rad_s2[0] += 0.01;
    const auto made = MakePreparedOperationParticipant(
        Candidate(inputs.binding, inputs.readiness, std::move(payload)), inputs.binding,
        inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
    EXPECT_FALSE(made.ok());
  }
}

}  // namespace
TEST(PreparedOperationParticipantCodecTest,
     RuleStopEntryResolvesOriginalSuffixAndRejectsCrossedOrMissingFloor) {
  const auto inputs = Inputs(BasePlan(), false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  const auto& plan = made.value().controller().ordinary_stop_plan;
  const auto first = ResolvePreparedIoRuleStopEntry(plan, {});
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value().stage_index, 0U);
  const auto& floor = plan.stage_commands.back().resolved_stage;
  ASSERT_EQ(floor.stage_kind, StopEscalationStageKind::kSoftwareHard);
  const auto chosen = ResolvePreparedIoRuleStopEntry(plan, floor.stop_escalation_stage_id);
  ASSERT_TRUE(chosen.ok());
  EXPECT_EQ(chosen.value().stage_index, floor.stage_index);
  auto crossed = chosen.value();
  crossed.resolved_stop_base_plan_id = ResolvedStopBasePlanId("other.base.plan");
  EXPECT_FALSE(VerifyPreparedIoRuleStopEntry(plan, crossed).ok());
  crossed = chosen.value();
  crossed.stage_index = 0U;
  EXPECT_FALSE(VerifyPreparedIoRuleStopEntry(plan, crossed).ok());
  EXPECT_FALSE(ResolvePreparedIoRuleStopEntry(plan, StopEscalationStageId("missing")).ok());
  auto no_floor = plan;
  no_floor.stage_commands.pop_back();
  EXPECT_FALSE(ResolvePreparedIoRuleStopEntry(no_floor, {}).ok());
  auto noncontiguous = plan;
  noncontiguous.stage_commands.back().resolved_stage.stage_index += 1U;
  EXPECT_FALSE(ResolvePreparedIoRuleStopEntry(noncontiguous, {}).ok());
}

TEST(PreparedOperationParticipantCodecTest,
     RuleSelectedModeSealsParametersFallbackAndOriginalStageChain) {
  auto base_candidate = BasePlan();
  base_candidate.escalation_stages.front()
      .command_qualification.controlled_substitution_contract->fallback_stop_modes.push_back(
          MotionControlledStopMode::kSpringBackFast);
  const auto remade_base = MakeResolvedStopBasePlan(std::move(base_candidate));
  ASSERT_TRUE(remade_base.ok()) << remade_base.error().message;
  const auto base = remade_base.value();
  const auto inputs = Inputs(base, false);
  auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));
  auto made =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, DefaultLimits(),
                                       MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  candidate = made.value();
  MotionInterruptStopProfileDTO defaults;
  defaults.profile_id = "entry.selected";
  defaults.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  defaults.jerk_limits_enabled = false;
  defaults.deceleration_time_s = 1.0;
  defaults.max_stop_time_s = 1.5;
  defaults.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                         {"joint_2", AngularAccelerationDTO{2.0}}};
  defaults.fallback_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                                  MotionInterruptStopMode::kSpringBackFast};
  const auto selected = SelectIoRuleStopProfile(defaults, MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  const auto profile = selected.value();
  EXPECT_EQ(profile.fallback_stop_modes, (std::vector{MotionInterruptStopMode::kSpringBackFast}));
  PreparedParticipantStopPlanContextDTO context{
      candidate.operation_id,
      candidate.operation_revision,
      candidate.participant_id,
      candidate.program_id,
      candidate.program_revision,
      candidate.program_binding_id,
      candidate.program_run_id,
      profile,
      ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
      std::string("entry.scope"),
      MotionInterruptNodeCategory::kGeneric};
  const auto plan =
      MakePreparedParticipantStopPlan(inputs.readiness.value().resolved_stop_base_plan, context,
                                      inputs.binding.value().resources, DefaultLimits());
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  ASSERT_EQ(plan.value().stage_commands.size(), base.escalation_stages.size());
  for (std::size_t index = 0; index < base.escalation_stages.size(); ++index)
    EXPECT_EQ(plan.value().stage_commands[index].resolved_stage, base.escalation_stages[index]);
  ASSERT_TRUE(plan.value().participant_stop_profile);
  EXPECT_EQ(plan.value().participant_stop_profile->stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(plan.value().participant_stop_profile->fallback_stop_modes,
            (std::vector{MotionInterruptStopMode::kSpringBackFast}));
  ASSERT_TRUE(plan.value().stage_commands.front().validated_contract_parameters);
  EXPECT_EQ(plan.value().stage_commands.front().validated_contract_parameters->at("stop_mode"),
            JsonValueDTO{std::string("spring_back_hold")});
  EXPECT_EQ(plan.value().stage_commands.front().parameter_source,
            StopStageParameterSourceDTO::kParticipantProfileMapping);
  const auto entry = ResolvePreparedIoRuleStopEntry(
      plan.value(), plan.value().stage_commands.back().resolved_stage.stop_escalation_stage_id);
  ASSERT_TRUE(entry.ok());
  std::get<PreparedMotionExecutionDTO>(candidate.payload)
      .interrupt_scopes.push_back({MotionInterruptScopeId("entry.scope"),
                                   InterruptPolicyId("entry.policy"),
                                   MotionInterruptActivationMode::kRequireInactiveAtArm,
                                   {},
                                   true,
                                   profile,
                                   MotionInterruptNodeCategory::kGeneric,
                                   {},
                                   plan.value(),
                                   InterruptPreemptionAction::kCompleteMoveOnSignal,
                                   entry.value(),
                                   entry.value()});
  const auto sealed =
      MakePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness, DefaultLimits(),
                                       MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;
  candidate = sealed.value();
  const auto bytes =
      EncodePreparedOperationParticipant(candidate, inputs.binding, inputs.readiness,
                                         DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  const auto& scope =
      std::get<PreparedMotionExecutionDTO>(candidate.payload).interrupt_scopes.front();
  ASSERT_TRUE(scope.stop_profile);
  EXPECT_EQ(scope.stop_profile->stop_mode, MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(scope.stop_profile->fallback_stop_modes,
            (std::vector{MotionInterruptStopMode::kSpringBackFast}));
  EXPECT_EQ(scope.stop_entry, entry.value());
  EXPECT_EQ(scope.success_through, entry.value());
  EXPECT_EQ(scope.stop_plan.stage_commands, plan.value().stage_commands);
  auto invalid = candidate;
  std::get<PreparedMotionExecutionDTO>(invalid.payload)
      .interrupt_scopes.front()
      .stop_entry->stage_index = 0U;
  EXPECT_FALSE(EncodePreparedOperationParticipant(invalid, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, CapturesQualifiedTcpAndRejectsCrossedPreparedEndpoint) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto value = inputs.binding.value();
  value.motion_endpoint =
      RuntimeMotionEndpointBindingDTO{TargetRoleId("tool"),
                                      CommandTargetId("gripper"),
                                      {FrameRoleId("fingertip"), ModelInstanceId("robot.model"),
                                       ModelRevision("model.revision"), "robot_fingertip"},
                                      {}};
  auto selected = verify_runtime_participant_binding(value);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  inputs.binding = std::move(selected.value());
  auto payload = MotionPayload(inputs);
  payload.plan_request.tcp_frame = ResolvedComponentModelFrameDTO{
      value.controllable_component_id, value.motion_endpoint->frame.model_instance_id,
      value.motion_endpoint->frame.frame_role_id, value.motion_endpoint->frame.compiled_frame_name,
      value.motion_endpoint->frame.model_revision};
  auto prepared = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, payload), inputs.binding, inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(prepared.ok()) << prepared.error().details.at("reason");
  EXPECT_EQ(prepared.value().motion_endpoint, value.motion_endpoint);
  const auto bytes =
      EncodePreparedOperationParticipant(prepared.value(), inputs.binding, inputs.readiness,
                                         DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(bytes.ok()) << bytes.error().message;
  EXPECT_NE(bytes.value().find("\"motion_endpoint\""), std::string::npos);
  auto crossed = prepared.value();
  crossed.motion_endpoint->target_role_id = TargetRoleId("other_tool");
  EXPECT_FALSE(VerifyPreparedOperationParticipant(crossed, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
  auto wrong_frame = payload;
  wrong_frame.plan_request.tcp_frame = ResolvedComponentModelFrameDTO{
      value.controllable_component_id, ModelInstanceId("robot.model"), FrameRoleId("tcp"),
      "robot_tcp", ModelRevision("model.revision")};
  EXPECT_FALSE(MakePreparedOperationParticipant(
                   Candidate(inputs.binding, inputs.readiness, wrong_frame), inputs.binding,
                   inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric)
                   .ok());
  // B7: the TCP frame carries its owner, here a tool mounted on the motion
  // component; the endpoint is compared by its frame key, never the stamp.
  auto tool_owned = payload;
  std::get<ResolvedComponentModelFrameDTO>(tool_owned.plan_request.tcp_frame)
      .controllable_component_id = ControllableComponentId("gripper.component");
  const auto owner_stamped = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, tool_owned), inputs.binding, inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(owner_stamped.ok()) << owner_stamped.error().details.at("reason");
  // Active TCP (M7): the lane's TCP step reads the entry from the stamp.
  EXPECT_EQ(prepared_motion_tcp_entry(owner_stamped.value()),
            std::optional(TcpEntryDTO{ControllableComponentId("gripper.component"),
                                      CommandTargetId("gripper"), FrameRoleId("fingertip")}));
  EXPECT_EQ(prepared_motion_tcp_entry(prepared.value()),
            std::optional(TcpEntryDTO{value.controllable_component_id, CommandTargetId("gripper"),
                                      FrameRoleId("fingertip")}));
}

}  // namespace manyforge::core
