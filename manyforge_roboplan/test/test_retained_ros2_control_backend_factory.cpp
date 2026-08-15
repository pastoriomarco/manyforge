#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_ros2_control_admission_owner.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

class NullExecutionStateSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult
  apply_group_joint_positions(const std::string &, const std::vector<double> &,
                              core::ExecutionStateUpdateMode) override {
    return {};
  }

  std::optional<double>
  group_joint_position(const std::string &,
                       const std::string &) const override {
    return 0.0;
  }

  core::ExecutionStateSinkResult
  apply_group_joint_position(const std::string &, const std::string &, double,
                             core::ExecutionStateUpdateMode) override {
    return {};
  }
};

[[nodiscard]] core::RuntimeParticipantBinding
commanded_binding(std::string component_id = "robot_1") {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId(std::move(component_id)),
      core::ComponentTypeId("robot.type"),
      core::ComponentTypeRevision("robot-type-revision"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("body"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{core::ModelGroupRoleId("arm_group"),
                                         core::ModelInstanceId("robot.model"),
                                         "arm",
                                         {"joint_1"}}},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("tcp"), core::ModelInstanceId("robot.model"),
          core::ModelRevision("model-revision"), "tcp"}},
      {core::RuntimeResourceBindingDTO{
          core::ResourceRoleId("joint_1"), core::ResourceId("robot.joint.1"),
          "source_joint_1", "joint_1", core::CoordinateKind::kAngular}},
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.arm"),
          core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.arm"),
          core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface.trajectory"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"),
          core::ControlRouteId("route.ros2_control"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.ros2_control"),
          core::ProviderManifestId("provider-manifest.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.commanded"),
          core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop.ros2_control"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::RuntimeParticipantBinding
commanded_gripper_binding(std::string component_id = "robot_1") {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("gripper"),
      core::ControllableComponentId(std::move(component_id)),
      core::ComponentTypeId("robot.type"),
      core::ComponentTypeRevision("robot-type-revision"),
      core::CommandTargetId("gripper.target"),
      core::CapabilityId("tool.gripper.command"),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("body"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{
          core::ModelGroupRoleId("gripper_group"),
          core::ModelInstanceId("robot.model"), "gripper",
          {"gripper_joint"}}},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("tcp"), core::ModelInstanceId("robot.model"),
          core::ModelRevision("model-revision"), "tcp"}},
      {core::RuntimeResourceBindingDTO{
          core::ResourceRoleId("fingers"),
          core::ResourceId("robot.gripper.1"), "source_gripper_joint",
          "gripper_joint", core::CoordinateKind::kLinear}},
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.gripper"),
          core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.gripper"),
          core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface.gripper"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"),
          core::ControlRouteId("route.ros2_control"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.ros2_control"),
          core::ProviderManifestId("provider-manifest.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.commanded"),
          core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop.ros2_control"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::ProviderManifestDTO ros_lifecycle_manifest() {
  const auto plans = retained_ros2_control_lifecycle_plans();
  const core::ProviderManifestId manifest_id{
      std::string(kRos2ControlLifecycleProviderManifestId)};
  const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      manifest_id, plans);
  if (!semantic.ok()) {
    throw std::logic_error(semantic.error().message);
  }
  std::vector<core::ProviderCapabilityOfferDTO> offers;
  for (const char *intent : {"disable", "enable", "observe", "prepare",
                             "recover"}) {
    const core::CapabilityId capability(std::string("component.lifecycle.") +
                                        intent);
    const auto *definition = core::find_builtin_capability_definition(capability);
    if (definition == nullptr) {
      throw std::logic_error("missing built-in lifecycle capability");
    }
    offers.push_back(
        {capability,
         core::CapabilityDefinitionRevision(
             std::string(definition->capability_definition_revision)),
         {}});
  }
  return {manifest_id,
          core::ProviderManifestRevision(
              "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
          std::move(offers),
          {},
          core::ProviderLifecyclePlanCatalogDeclarationDTO{
              core::SchemaId(
                  std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
              semantic.value()}};
}

TEST(RetainedRos2ControlBackendFactoryTest,
     CommandedSimulationSelectsOneNonExpiringAddressedAuthority) {
  ros2_control_execution::Ros2ControlExecutionBackendConfig config;
  config.execution_state_source_mode = "commanded_sim";
  config.required_joints = {"joint_1"};

  const auto state = retained_ros2_control_motion_state_config(config);

  EXPECT_EQ(state.source_kind, core::ObservedStateSourceKind::kCommandedState);
  EXPECT_EQ(state.freshness_budget_ns, 0);
  ASSERT_EQ(state.source_precedence.size(), 1U);
  EXPECT_EQ(state.source_precedence.front().authority_profile,
            core::ObservedRobotStateAuthorityProfile::kCommandedSimulation);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedIsaacProfileExpandsToExactControllerAndGripperContract) {
  deployment::ros2_control_execution_runtime_contract::Configuration runtime{
      "lite6", 12345U, {0.0}, 100U, 50.0};

  const auto selected = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding(), commanded_gripper_binding());

  EXPECT_EQ(selected.execution.required_joints,
            (std::vector<std::string>{"joint_1"}));
  EXPECT_EQ(selected.execution.transport_joint_names,
            (std::vector<std::string>{"source_joint_1"}));
  EXPECT_EQ(selected.execution.node_namespace, "/manyforge/robot_1");
  EXPECT_EQ(selected.execution.action_name,
            "/manyforge/robot_1/isaac_arm_controller/"
            "follow_joint_trajectory");
  EXPECT_EQ(selected.execution.joint_state_topic,
            "/manyforge/robot_1/joint_states");
  EXPECT_EQ(selected.execution.controller_state_topic,
            "/manyforge/robot_1/isaac_arm_controller/controller_state");
  EXPECT_EQ(selected.execution.command_gate_topic,
            "/manyforge/robot_1/manyforge/isaac_arm_command_enable");
  EXPECT_EQ(selected.execution.execution_state_source_mode, "commanded_sim");
  EXPECT_FALSE(selected.execution.enable_jtc_speed_scaling_input);
  EXPECT_TRUE(selected.execution.enable_soft_on_path_path_ramp);
  EXPECT_TRUE(selected.execution.require_execution_admission_snapshot);
  ASSERT_TRUE(selected.execution.controller_binding.has_value());
  EXPECT_EQ(selected.execution.controller_binding->hardware_component_name,
            "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(selected.execution.controller_binding->controller_manager,
            "/manyforge/robot_1/controller_manager");
  EXPECT_EQ(selected.execution.controller_binding->activity_topic,
            "/manyforge/robot_1/controller_manager/activity");
  EXPECT_EQ(selected.execution.controller_binding
                ->trajectory_controller_ros_name,
            "/manyforge/robot_1/isaac_arm_controller");
  EXPECT_EQ(selected.execution.controller_binding->required_joints,
            (std::vector<std::string>{"source_joint_1"}));
  EXPECT_EQ(selected.execution.controller_binding->claimed_command_interfaces,
            (std::vector<std::string>{"source_joint_1/position"}));
  EXPECT_EQ(selected.execution.controller_binding->required_state_interfaces,
            (std::vector<std::string>{"source_joint_1/position",
                                      "source_joint_1/velocity"}));
  ASSERT_TRUE(selected.gripper.has_value());
  EXPECT_EQ(selected.gripper->action_name,
            "/manyforge/robot_1/lite_gripper_controller/gripper_cmd");
  EXPECT_EQ(selected.gripper->transport_kind, "isaac_joint_state_effort");
  EXPECT_EQ(selected.gripper->command_joint_names,
            (std::vector<std::string>{"source_gripper_joint"}));
  EXPECT_EQ(selected.gripper->group_id, "gripper");
  EXPECT_EQ(selected.gripper->joint_name, "gripper_joint");
  EXPECT_EQ(selected.gripper->completion_policy, "dwell_elapsed");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     SameTypeIsaacInstancesHaveDisjointControlPlaneEndpoints) {
  deployment::ros2_control_execution_runtime_contract::Configuration runtime{
      "ur10e", 12345U, {0.0}, 100U, 50.0};

  const auto first = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_a"),
      commanded_gripper_binding("robot_a"));
  const auto second = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_b"),
      commanded_gripper_binding("robot_b"));

  ASSERT_TRUE(first.execution.controller_binding.has_value());
  ASSERT_TRUE(second.execution.controller_binding.has_value());
  ASSERT_TRUE(first.gripper.has_value());
  ASSERT_TRUE(second.gripper.has_value());
  const std::vector<std::string> first_endpoints{
      first.execution.action_name,
      first.execution.joint_state_topic,
      first.execution.controller_state_topic,
      first.execution.command_gate_topic,
      first.execution.controller_binding->controller_manager,
      first.execution.controller_binding->activity_topic,
      first.execution.controller_binding->trajectory_controller_ros_name,
      first.gripper->action_name};
  const std::vector<std::string> second_endpoints{
      second.execution.action_name,
      second.execution.joint_state_topic,
      second.execution.controller_state_topic,
      second.execution.command_gate_topic,
      second.execution.controller_binding->controller_manager,
      second.execution.controller_binding->activity_topic,
      second.execution.controller_binding->trajectory_controller_ros_name,
      second.gripper->action_name};
  ASSERT_EQ(first_endpoints.size(), second_endpoints.size());
  for (std::size_t index = 0; index < first_endpoints.size(); ++index) {
    EXPECT_NE(first_endpoints[index], second_endpoints[index]);
  }
  EXPECT_NE(first.execution.controller_binding->generation_id,
            second.execution.controller_binding->generation_id);

  // These are the configurable raw Isaac transport wires, not runtime-owned
  // admission endpoints. A two-world fixture must give each launch instance
  // distinct raw topics; the retained single-world defaults intentionally
  // stay source-compatible.
  EXPECT_EQ(first.gripper->command_topic, second.gripper->command_topic);
  EXPECT_EQ(first.gripper->state_topic, second.gripper->state_topic);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedIsaacNamespaceRejectsLossyOrCrossedComponentIdentity) {
  deployment::ros2_control_execution_runtime_contract::Configuration runtime{
      "ur10e", 12345U, {0.0}, 100U, 50.0};

  EXPECT_THROW(
      static_cast<void>(make_retained_isaac_ros2_control_backend_config(
          runtime, commanded_binding("robot.1"),
          commanded_gripper_binding("robot.1"))),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(make_retained_isaac_ros2_control_backend_config(
          runtime, commanded_binding("robot_a"),
          commanded_gripper_binding("robot_b"))),
      std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     ExternalLifecycleCatalogIsAnchoredAndHasNoMutationPrimitives) {
  const auto plans = retained_ros2_control_lifecycle_plans();
  ASSERT_EQ(plans.size(), 4U);
  EXPECT_TRUE(std::all_of(plans.begin(), plans.end(), [](const auto &plan) {
    return plan.transitions.empty();
  }));
  EXPECT_EQ(plans.front().intent,
            core::ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(plans.front().requested_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(plans.front().requested_postcondition.allowed_states,
            (std::vector<core::ComponentLifecycleStateDTO>{
                core::ComponentLifecycleStateDTO::kPrepared,
                core::ComponentLifecycleStateDTO::kActive}));
  const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kRos2ControlLifecycleProviderManifestId)),
      plans);
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  EXPECT_EQ(semantic.value().value(),
            "sha256:"
            "b56660c812db2e09af08d4bf7b7850b08a28fbfc2d24f9fb9fcace34e2dbcef2");

  auto catalog =
      make_retained_ros2_control_lifecycle_catalog(ros_lifecycle_manifest());
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  EXPECT_EQ(catalog.value().provider_manifest_id.value(),
            kRos2ControlLifecycleProviderManifestId);
  EXPECT_TRUE(core::VerifyBrandLifecyclePlanCatalog(catalog.value()).ok());

  auto wrong = ros_lifecycle_manifest();
  wrong.provider_manifest_id = core::ProviderManifestId("another.provider");
  const auto rejected = make_retained_ros2_control_lifecycle_catalog(wrong);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "provider_manifest_id_unsupported");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     FactoryIsSingleUseEvenWhenConstructionFailsClosed) {
  RetainedRos2ControlBackendConfig config;
  config.execution.execution_state_source_mode = "commanded_sim";
  config.execution.required_joints = {"joint_1"};
  auto factory = make_retained_ros2_control_backend_factory(std::move(config));
  const auto binding = commanded_binding();
  core::FakeClock clock;
  core::ManualScheduler scheduler;

  EXPECT_THROW(static_cast<void>(factory(binding, std::nullopt, nullptr,
                                         nullptr, clock, scheduler)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(factory(binding, std::nullopt, nullptr,
                                         nullptr, clock, scheduler)),
               std::logic_error);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RuntimeHandleNormalizesBindingChangesIntoMonotonicLifecycleTruth) {
  auto config = ros2_control_execution::Ros2ControlExecutionBackendConfig{};
  config.node_name = "manyforge_runtime_handle_test";
  config.action_name = "/manyforge_runtime_handle_test/follow_joint_trajectory";
  config.joint_state_topic = "/manyforge_runtime_handle_test/joint_states";
  config.required_joints = {"joint_1"};
  config.transport_joint_names = {"source_joint_1"};
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  config.enable_jtc_speed_scaling_input = false;

  core::FakeClock clock;
  clock.set(1'000);
  core::ManualScheduler scheduler;
  auto sink = std::make_shared<NullExecutionStateSink>();
  auto backend =
      std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
          ros2_control_execution::Ros2ControlExecutionBackend::
              TestingNoRosStartupTag{},
          sink, "arm", clock, scheduler, config);
  auto handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
  const auto binding = commanded_binding();
  const auto bound = handle->BindBackend(backend, binding, std::nullopt);
  ASSERT_TRUE(bound.ok()) << bound.error().message;

  const auto unavailable = handle->ReadLifecycleSnapshot(
      core::ControlDomainId("domain.arm"), clock.now());
  ASSERT_TRUE(unavailable.ok()) << unavailable.error().message;
  EXPECT_EQ(unavailable.value().source_generation, 1U);
  EXPECT_EQ(unavailable.value().component_state,
            core::ComponentLifecycleStateDTO::kDegraded);
  EXPECT_FALSE(unavailable.value().exact_controller_configuration_active);

  ros2_control_execution::ControllerBindingSnapshot ready;
  ready.activity_generation = 1U;
  ready.contract_generation_id = "binding.generation";
  ready.observed_timestamp_ns = clock.now();
  ready.received_timestamp_ns = clock.now();
  ready.fresh_until_ns = clock.now() + 1'000;
  ready.controller_active = true;
  ready.hardware_active = true;
  ready.controller_type_matches = true;
  ready.claimed_interfaces_match = true;
  ready.state_interfaces_available = true;
  ready.conflicting_claims_absent = true;
  ready.valid = true;
  backend->testing_publish_controller_binding_snapshot(std::move(ready));

  const auto active = handle->ReadLifecycleSnapshot(
      core::ControlDomainId("domain.arm"), clock.now());
  ASSERT_TRUE(active.ok()) << active.error().message;
  EXPECT_EQ(active.value().source_generation, 2U);
  EXPECT_EQ(active.value().component_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_TRUE(active.value().exact_controller_configuration_active);
  const auto unchanged = handle->ReadLifecycleSnapshot(
      core::ControlDomainId("domain.arm"), clock.now());
  ASSERT_TRUE(unchanged.ok());
  EXPECT_EQ(unchanged.value().source_generation, 2U);

  const auto rebound = handle->BindBackend(backend, binding, std::nullopt);
  ASSERT_FALSE(rebound.ok());
  EXPECT_EQ(rebound.error().details.at("reason"),
            "ros2_control_handle_already_bound");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     AdmissionOwnerFailsClosedWhenLifecycleReadbackIsUnavailable) {
  deployment::ros2_control_execution_runtime_contract::Configuration runtime{
      "lite6", 12345U, {0.0}, 100U, 50.0};
  auto selected = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding(), commanded_gripper_binding());
  ASSERT_TRUE(selected.execution.controller_binding.has_value());
  const auto binding_generation =
      selected.execution.controller_binding->generation_id;

  core::FakeClock clock;
  clock.set(1'000);
  core::ManualScheduler scheduler;
  auto sink = std::make_shared<NullExecutionStateSink>();
  auto backend =
      std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
          ros2_control_execution::Ros2ControlExecutionBackend::
              TestingNoRosStartupTag{},
          sink, "arm", clock, scheduler, std::move(selected.execution));
  auto handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
  const auto binding = commanded_binding();
  ASSERT_TRUE(handle->BindBackend(backend, binding, std::nullopt).ok());

  ros2_control_execution::ControllerBindingSnapshot ready;
  ready.activity_generation = 1U;
  ready.contract_generation_id = binding_generation;
  ready.observed_timestamp_ns = clock.now();
  ready.received_timestamp_ns = clock.now();
  ready.fresh_until_ns = clock.now() + 100'000'000;
  ready.controller_active = true;
  ready.hardware_active = true;
  ready.controller_type_matches = true;
  ready.claimed_interfaces_match = true;
  ready.state_interfaces_available = true;
  ready.conflicting_claims_absent = true;
  ready.valid = true;
  backend->testing_publish_controller_binding_snapshot(std::move(ready));

  RetainedRos2ControlAdmissionOwnerConfig owner_config{
      core::ControllableComponentId("robot_1"),
      core::RuntimeEpoch("runtime-epoch.1"),
      binding_generation,
      "provider-epoch.1",
      "cell-supervisor-epoch.1",
      1'000'000,
      10'000'000};
  auto owner = RetainedRos2ControlAdmissionOwner::Create(
      std::move(owner_config), handle, clock, [] {
        return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
            core::ErrorDTO{
                std::string(core::to_string(
                    core::ErrorCode::kInvariantViolation)),
                "component snapshot intentionally unavailable", {}});
      });
  ASSERT_TRUE(owner.ok()) << owner.error().message;

  for (int attempt = 0;
       attempt < 100 &&
       owner.value()->status().publication_generation == 0U;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto status = owner.value()->status();
  ASSERT_TRUE(status.running);
  ASSERT_TRUE(status.healthy) << status.reason_code;
  ASSERT_GT(status.publication_generation, 0U);

  const auto admission = backend->execution_admission_snapshot();
  ASSERT_TRUE(admission.has_value());
  EXPECT_TRUE(admission->controller_ready);
  EXPECT_FALSE(admission->motion_admitted);
  EXPECT_TRUE(std::binary_search(
      admission->reason_codes.begin(), admission->reason_codes.end(),
      "component_lifecycle_snapshot_unavailable"));
  EXPECT_TRUE(admission->transport_owner_component_epoch.empty());
  EXPECT_TRUE(admission->authority_component_epoch.empty());
}

} // namespace
} // namespace manyforge::roboplan_backend
