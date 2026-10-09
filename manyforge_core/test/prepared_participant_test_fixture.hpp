#pragma once

// Shared sealed-participant fixture for Core tests: the golden resolved Stop
// base plan, a verified binding/readiness pair and Prepare candidates.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::core::prepared_participant_fixture {

[[nodiscard]] inline std::string Digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] inline std::span<const HardStopResourceLimitDTO> DefaultLimits() {
  static const std::array<HardStopResourceLimitDTO, 2> limits{
      {{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
       {ResourceId("joint.2"), AngularVelocityDTO{0.02}}}};
  return limits;
}

/// Variations of the golden base plan's nominal soft-on-path Stop.
struct BasePlanOptions final {
  double max_deceleration_rad_s2{2.0};
  double settle_velocity_threshold_rad_s{0.02};
};

[[nodiscard]] inline ResolvedStopBasePlanDTO BasePlan(const BasePlanOptions& options = {}) {
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
                                 {"value_rad_s2", JsonValueDTO{options.max_deceleration_rad_s2}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                                    {"value_rad_s",
                                     JsonValueDTO{options.settle_velocity_threshold_rad_s}}}}}});
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

[[nodiscard]] inline RuntimeParticipantBinding Binding(
    const ResolvedStopBasePlanDTO& base, bool command,
    StateAuthorityClass state_authority = StateAuthorityClass::kMeasured) {
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
                            state_authority,
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

[[nodiscard]] inline ControllerLifecycleExpectedDomain ExpectedDomain(
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

[[nodiscard]] inline SealedInputs Inputs(
    const ResolvedStopBasePlanDTO& base, bool command,
    StateAuthorityClass state_authority = StateAuthorityClass::kMeasured) {
  auto binding = Binding(base, command, state_authority);
  auto domain = ExpectedDomain(base, binding);
  auto readiness = verify_runtime_participant_stop_readiness(
      {binding.value().controllable_component_id, ComponentLifecycleGeneration(9U), domain.value(),
       base},
      binding, domain);
  if (!readiness.ok()) throw std::logic_error(readiness.error().message);
  return {std::move(binding), std::move(domain), std::move(readiness.value())};
}

[[nodiscard]] inline PreparedCapabilityCommandDTO CommandPayload() {
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

[[nodiscard]] inline PreparedOperationParticipantDTO Candidate(
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

/// Motion payload; `trajectory` replaces the default two-point linear-only
/// trajectory (its duration and per-joint metrics follow it).
[[nodiscard]] inline PreparedMotionExecutionDTO MotionPayload(
    const SealedInputs& inputs, std::optional<TrajectoryDTO> custom = std::nullopt) {
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
  if (custom) trajectory = std::move(*custom);
  const double duration = trajectory.waypoints.back().time_from_start_s;
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
           ValidationResultDTO{
               true, std::nullopt,
               TrajectoryValidationMetricsDTO{duration, std::vector<double>(joints.size(), 0.5),
                                              std::vector<double>(joints.size(), 0.0), 0.1}},
           world, CollisionPolicyRevision{1U, Digest('1')}, decision},
          std::nullopt,
          0.5,
          0.5,
          {},
          3.0};
}

}  // namespace manyforge::core::prepared_participant_fixture
