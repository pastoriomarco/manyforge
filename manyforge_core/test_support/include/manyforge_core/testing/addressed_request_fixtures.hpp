#pragma once

#include <manyforge_core/types/execute_request.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core::testing {

inline ProjectionLeaseBindingDTO make_test_projection_binding() {
  return ProjectionLeaseBindingDTO{
      ProjectionRevisionDTO{RetainedBundleId("bundle:test"), GraphRevision("graph:test"),
                            SceneRevision{SceneId("sha256:" + std::string(64U, '1')), 1U,
                                          ContentDigest("sha256:" + std::string(64U, '2'))}},
      ProjectionLeaseId("projection-lease:test")};
}

inline ResolvedExecutionBindingDTO make_test_execution_binding(
    std::vector<std::string> joint_names = {"joint1", "joint2"},
    std::string program_run_id = "run:test") {
  std::vector<ResourceId> resources;
  resources.reserve(joint_names.size());
  for (const auto& joint_name : joint_names) {
    resources.emplace_back("resource:" + joint_name);
  }

  const ControllableComponentId component_id("component:test");
  const ModelInstanceId model_id("model:test");
  const ModelRevision model_revision("model-revision:test");
  return ResolvedExecutionBindingDTO{
      OperationId("operation:test"),
      OperationParticipantId("participant:test"),
      ControllerGoalId("controller-goal:test"),
      ControllerGoalGeneration(1U),
      ProgramId("program:test"),
      ProgramRevision("program-revision:test"),
      ProgramBindingId("program-binding:test"),
      ProgramRunId(std::move(program_run_id)),
      component_id,
      ComponentTypeId("component-type:test"),
      ComponentTypeRevision("component-type-revision:test"),
      CommandTargetId("target:test"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability-revision:test"),
      PlannerCapabilityBindingDTO{
          CapabilityId("planning.motion.joint_goal"),
          CapabilityDefinitionRevision(
              "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce"),
          CapabilityProviderBindingId("planner-binding:test"),
          CapabilityProviderBindingRevision("planner-binding-revision:test"),
          ProviderInstanceId("planner-provider:test"),
          ProviderManifestId("planner-provider-manifest:test"),
          ProviderManifestRevision("planner-provider-revision:test"),
          RuntimeEpoch("planner-runtime-epoch:test")},
      {PlannerModelBindingDTO{model_id, model_revision}},
      PlannerModelGroupBindingDTO{ModelGroupRoleId("model-group-role:arm"), model_id,
                                  model_revision, "compiled_arm_group", std::move(joint_names)},
      ControlDomainId("control-domain:test"),
      ControlDomainRevision("control-domain-revision:test"),
      ControlBindingId("control-binding:test"),
      ControlBindingRevision("control-binding-revision:test"),
      ControlInterfaceContractId("control-interface:test"),
      ControlInterfaceRevision("control-interface-revision:test"),
      ControllerTopologyId("controller-topology:test"),
      TopologyRevision("topology-revision:test"),
      ControlRouteId("control-route:test"),
      ActivationSetId("activation-set:test"),
      ActiveControlConfigurationId("active-control:test"),
      ActiveControlConfigurationGeneration(1U),
      ProviderInstanceId("provider:test"),
      ProviderManifestId("provider-manifest:test"),
      ProviderManifestRevision("provider-manifest-revision:test"),
      RuntimeEpoch("provider-runtime-epoch:test"),
      {PlannerRuntimeUnitBindingDTO{RuntimeUnitId("runtime-unit:test"),
                                    RuntimeEpoch("runtime-epoch:test")}},
      std::move(resources),
      ResolvedComponentModelFrameDTO{component_id, model_id, FrameRoleId("frame-role:base"), "base",
                                     model_revision},
      ResolvedComponentModelFrameDTO{component_id, model_id, FrameRoleId("frame-role:tcp"), "tcp",
                                     model_revision},
      {},
      {},
      std::nullopt};
}

inline ExecuteRequestDTO make_test_execute_request(
    std::vector<std::string> joint_names = {"joint1", "joint2"}, std::string program_run_id = "") {
  return ExecuteRequestDTO(
      make_test_execution_binding(std::move(joint_names), std::move(program_run_id)));
}

inline ControlledStateDTO make_test_controlled_state(
    const ResolvedExecutionBindingDTO& binding, const std::vector<double>& positions_rad,
    std::optional<std::vector<double>> velocities_rad_s = std::nullopt) {
  std::vector<CoordinatePositionDTO> positions;
  positions.reserve(positions_rad.size());
  for (const double position : positions_rad) {
    positions.emplace_back(AngularPositionDTO{position});
  }
  std::optional<std::vector<CoordinateVelocityDTO>> velocities;
  if (velocities_rad_s.has_value()) {
    velocities.emplace();
    velocities->reserve(velocities_rad_s->size());
    for (const double velocity : *velocities_rad_s) {
      velocities->emplace_back(AngularVelocityDTO{velocity});
    }
  }
  return ControlledStateDTO{binding.controllable_component_id,
                            binding.command_target_id,
                            binding.control_domain_id,
                            binding.control_domain_revision,
                            binding.control_binding_id,
                            binding.control_binding_revision,
                            binding.controller_topology_id,
                            binding.topology_revision,
                            binding.control_route_id,
                            binding.active_control_configuration_id,
                            binding.active_control_configuration_generation,
                            binding.resources,
                            binding.model_group_binding.compiled_joint_names,
                            std::move(positions),
                            std::move(velocities)};
}

inline PlanRequestDTO make_test_plan_request(std::string move_id, ResolvedMotionSpecDTO motion,
                                             std::vector<std::string> joint_names = {"joint1",
                                                                                     "joint2"},
                                             std::vector<double> start_positions_rad = {0.0, 0.0}) {
  auto binding = make_test_execution_binding(std::move(joint_names));
  auto state = make_test_controlled_state(binding, start_positions_rad);
  return PlanRequestDTO{binding.operation_id,
                        binding.participant_id,
                        binding.program_id,
                        binding.program_revision,
                        binding.program_binding_id,
                        binding.program_run_id,
                        MoveId(std::move(move_id)),
                        binding.controllable_component_id,
                        binding.component_type_id,
                        binding.component_type_revision,
                        binding.command_target_id,
                        binding.capability_id,
                        binding.capability_definition_revision,
                        binding.planner_capability_binding,
                        binding.model_bindings,
                        binding.model_group_binding,
                        binding.control_domain_id,
                        binding.control_domain_revision,
                        binding.control_binding_id,
                        binding.control_binding_revision,
                        binding.control_interface_contract_id,
                        binding.control_interface_revision,
                        binding.controller_topology_id,
                        binding.topology_revision,
                        binding.control_route_id,
                        binding.activation_set_id,
                        binding.active_control_configuration_id,
                        binding.active_control_configuration_generation,
                        binding.provider_instance_id,
                        binding.provider_manifest_id,
                        binding.provider_manifest_revision,
                        binding.provider_runtime_epoch,
                        binding.runtime_unit_bindings,
                        binding.resources,
                        std::nullopt,
                        binding.base_frame,
                        binding.tcp_frame,
                        std::move(state),
                        std::move(motion),
                        TolerancesDTO{},
                        SpeedProfileDTO{},
                        PlanningProfileDTO::kFirstFeasible,
                        std::nullopt,
                        {},
                        5.0,
                        0U,
                        make_test_projection_binding(),
                        {},
                        {}};
}

}  // namespace manyforge::core::testing
