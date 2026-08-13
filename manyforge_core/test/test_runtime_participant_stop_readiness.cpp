#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ResolvedStopBasePlanDTO BasePlan() {
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
  return std::move(decoded.value());
}

[[nodiscard]] RuntimeParticipantBinding Binding(const ResolvedStopBasePlanDTO& base) {
  const auto& target = base.escalation_stages.front().command_target;
  RuntimeParticipantBindingDTO value{
      ProgramId("program.1"),
      ProgramRevision("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      ProgramBindingId("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
      base.retained_bundle_id,
      base.graph_revision,
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("arm"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability.revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision")}},
      {},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "robot_base"}},
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
      {},
      {RuntimeUnitEpochBindingDTO{base.source_runtime_unit_id, base.source_runtime_epoch},
       RuntimeUnitEpochBindingDTO{target.runtime_unit_id, target.runtime_unit_epoch}},
      {}};
  auto binding = verify_runtime_participant_binding(std::move(value));
  if (!binding.ok()) {
    throw std::logic_error(binding.error().message);
  }
  return std::move(binding.value());
}

[[nodiscard]] ControllerLifecycleExpectedDomain ExpectedDomain(
    const ResolvedStopBasePlanDTO& base, const RuntimeParticipantBinding& binding) {
  const auto& path = *binding.value().control_path;
  const DesiredControlBindingRouteDTO desired_route{
      path.control_binding_id, path.control_binding_revision, path.control_route_id};
  const std::vector<ControllerInterfaceInstanceDTO> interfaces{{ResourceId("joint.1"), "position"},
                                                               {ResourceId("joint.2"), "position"}};
  const ControllerRoleId controller_role("hardware");
  ControllerLifecycleExpectedDomainDTO value{
      base.control_domain_id,
      base.control_domain_revision,
      base.controller_topology_id,
      base.topology_revision,
      ProductId("product.control.arm"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision(
          "sha256:1111111111111111111111111111111111111111111111111111111111111111"),
      ContentDigest("sha256:2222222222222222222222222222222222222222222222222222222222222222"),
      ContentDigest("sha256:3333333333333333333333333333333333333333333333333333333333333333"),
      base.source_runtime_unit_id,
      base.source_runtime_epoch,
      {base.controller_topology_id,
       base.topology_revision,
       {{controller_role,
         {{path.control_interface_contract_id,
           path.control_interface_revision,
           {ControllerClaimDirection::kCommand, ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{base.activation_set_id, {controller_role}, {}}},
       {{base.control_route_id,
         path.control_interface_contract_id,
         path.control_interface_revision,
         {controller_role},
         {base.activation_set_id}}}},
      {base.control_domain_id,
       base.control_domain_revision,
       base.controller_topology_id,
       base.topology_revision,
       base.activation_set_id,
       {desired_route}},
      {{path.control_binding_id, path.control_binding_revision, path.control_interface_contract_id,
        path.control_interface_revision, interfaces, interfaces}},
      {{controller_role,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(
            "sha256:4444444444444444444444444444444444444444444444444444444444444444"),
        "controller_interface.ControllerInterface",
        ContentDigest("sha256:5555555555555555555555555555555555555555555555555555555555555555"),
        interfaces,
        interfaces,
        {},
        {}}},
      {{path.control_binding_id, path.control_binding_revision, path.control_route_id,
        EndpointRoleId("endpoint.command"), "controller.command", true, false}}};
  auto sealed = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] Result<RuntimeParticipantStopReadiness> VerifyReadiness(
    const ResolvedStopBasePlanDTO& base, ComponentLifecycleGeneration generation,
    const RuntimeParticipantBinding& participant,
    const RuntimeParticipantBinding& expected_domain_binding) {
  const auto expected = ExpectedDomain(base, expected_domain_binding);
  return verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO{participant.value().controllable_component_id, generation,
                                         expected.value(), base},
      participant, expected);
}

TEST(RuntimeParticipantStopReadinessTest, SealsExactReconciledBasePlanJoin) {
  auto base = BasePlan();
  const auto binding = Binding(base);
  const auto readiness = VerifyReadiness(base, ComponentLifecycleGeneration(9U), binding, binding);
  ASSERT_TRUE(readiness.ok());
  EXPECT_EQ(readiness.value().value().resolved_stop_base_plan, base);
  EXPECT_EQ(readiness.value().value().component_lifecycle_generation,
            ComponentLifecycleGeneration(9U));
}

TEST(RuntimeParticipantStopReadinessTest, RejectsSelfAttestedControllerDomainSubstitution) {
  const auto base = BasePlan();
  const auto binding = Binding(base);
  const auto expected = ExpectedDomain(base, binding);
  auto substituted = expected.value();
  substituted.expected_bindings.front().claimed_command_interfaces.front().interface_name =
      "velocity";
  ASSERT_TRUE(substituted.locally_valid());

  const auto readiness = verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO{binding.value().controllable_component_id,
                                         ComponentLifecycleGeneration(9U), std::move(substituted),
                                         base},
      binding, expected);
  ASSERT_FALSE(readiness.ok());
  EXPECT_EQ(readiness.error().details.at("reason"),
            "runtime_participant_stop_controller_domain_referent_mismatch");
}

TEST(RuntimeParticipantStopReadinessTest, RejectsStaleConfigurationOrResourceScope) {
  const auto base = BasePlan();
  const auto binding = Binding(base);
  auto stale_value = binding.value();
  stale_value.control_path->active_control_configuration_generation =
      ActiveControlConfigurationGeneration(8U);
  auto stale = verify_runtime_participant_binding(std::move(stale_value));
  ASSERT_TRUE(stale.ok()) << stale.error().message;
  auto result = VerifyReadiness(base, ComponentLifecycleGeneration(9U), stale.value(), binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_base_plan_join_invalid");

  auto widened_value = binding.value();
  widened_value.resources.back().resource_id = ResourceId("joint.3");
  auto widened = verify_runtime_participant_binding(std::move(widened_value));
  ASSERT_TRUE(widened.ok()) << widened.error().message;
  result = VerifyReadiness(base, ComponentLifecycleGeneration(9U), widened.value(), binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_nominal_scope_invalid");
}

TEST(RuntimeParticipantStopReadinessTest, RejectsForgedPlanAndComponentGeneration) {
  auto base = BasePlan();
  const auto binding = Binding(base);
  base.source_sequence += 1U;
  auto result = VerifyReadiness(base, ComponentLifecycleGeneration(9U), binding, binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_base_plan_unverified");

  const auto valid_base = BasePlan();
  result = VerifyReadiness(valid_base, ComponentLifecycleGeneration(0U), binding, binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "runtime_participant_stop_component_generation_invalid");
}

TEST(RuntimeParticipantStopReadinessTest, RejectsSubstitutedProviderOrRuntimeUnitEpoch) {
  const auto base = BasePlan();
  const auto binding = Binding(base);

  auto substituted_provider_value = binding.value();
  substituted_provider_value.control_path->provider_manifest_revision =
      ProviderManifestRevision("sha256:substituted-provider-revision");
  auto substituted_provider =
      verify_runtime_participant_binding(std::move(substituted_provider_value));
  ASSERT_TRUE(substituted_provider.ok()) << substituted_provider.error().message;
  auto result = VerifyReadiness(base, ComponentLifecycleGeneration(9U),
                                substituted_provider.value(), binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_nominal_target_invalid");

  auto substituted_runtime_value = binding.value();
  auto target_runtime =
      std::find_if(substituted_runtime_value.runtime_unit_epochs.begin(),
                   substituted_runtime_value.runtime_unit_epochs.end(), [&](const auto& row) {
                     return row.runtime_unit_id ==
                            substituted_runtime_value.control_path->provider_runtime_unit_id;
                   });
  ASSERT_NE(target_runtime, substituted_runtime_value.runtime_unit_epochs.end());
  target_runtime->runtime_epoch = RuntimeEpoch("runtime.epoch.substituted");
  auto substituted_runtime =
      verify_runtime_participant_binding(std::move(substituted_runtime_value));
  ASSERT_TRUE(substituted_runtime.ok()) << substituted_runtime.error().message;
  result =
      VerifyReadiness(base, ComponentLifecycleGeneration(9U), substituted_runtime.value(), binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_nominal_target_invalid");

  auto substituted_runtime_unit_value = binding.value();
  auto target_runtime_unit =
      std::find_if(substituted_runtime_unit_value.runtime_unit_epochs.begin(),
                   substituted_runtime_unit_value.runtime_unit_epochs.end(), [&](const auto& row) {
                     return row.runtime_unit_id ==
                            substituted_runtime_unit_value.control_path->provider_runtime_unit_id;
                   });
  ASSERT_NE(target_runtime_unit, substituted_runtime_unit_value.runtime_unit_epochs.end());
  substituted_runtime_unit_value.control_path->provider_runtime_unit_id =
      RuntimeUnitId("runtime.substituted");
  target_runtime_unit->runtime_unit_id = RuntimeUnitId("runtime.substituted");
  std::sort(
      substituted_runtime_unit_value.runtime_unit_epochs.begin(),
      substituted_runtime_unit_value.runtime_unit_epochs.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.runtime_unit_id < rhs.runtime_unit_id; });
  auto substituted_runtime_unit =
      verify_runtime_participant_binding(std::move(substituted_runtime_unit_value));
  ASSERT_TRUE(substituted_runtime_unit.ok()) << substituted_runtime_unit.error().message;
  result = VerifyReadiness(base, ComponentLifecycleGeneration(9U), substituted_runtime_unit.value(),
                           binding);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "runtime_participant_stop_nominal_target_invalid");
}

}  // namespace
}  // namespace manyforge::core
