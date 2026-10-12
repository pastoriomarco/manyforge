#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>

#include <gtest/gtest.h>
#include "managed_ros2_component_installation.hpp"
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

/// The maintained provider manifest's catalog anchor
/// (resources/maintained/catalog/provider_manifest/manyforge.provider.ros2_control.json).
constexpr const char *kMaintainedRos2ControlCatalogSemanticDigest =
    "sha256:f6d46812c1a953a09c081e331c92d20473ecfc42d0ac68749e30d724c03cc3b7";

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

[[nodiscard]] std::unique_ptr<ros2_control_execution::Ros2TransportResources>
make_test_transport_resources(
    const std::string &name,
    const std::shared_ptr<core::IExecutionStateSink> &sink,
    core::IClock &clock) {
  return std::make_unique<ros2_control_execution::Ros2TransportResources>(
      name, "/", 1U, sink, "arm", clock);
}

/// Arbitrary explicit timing inputs for the isolated participant fixtures;
/// builder tests below check the production Q55 lane values.
[[nodiscard]] core::ExecutionPortParticipantConfig test_participant_config() {
  core::ExecutionPortParticipantConfig config;
  config.stop.trip_authority = core::StateAuthorityClass::kCommanded;
  config.stop.stop_traj_delta_s = 0.1;
  config.stop.sample_period_s = 0.01;
  config.stop.replacement_start_allowance_s = 0.02;
  config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
  config.late_cycle_margin_s = 0.015;
  return config;
}

[[nodiscard]] RetainedRos2ControlArmTransport
test_arm_transport(const std::string &prefix) {
  RetainedRos2ControlArmTransport arm;
  arm.adapter.action_name = prefix + "/arm_controller/follow_joint_trajectory";
  arm.adapter.state_topic = prefix + "/arm_controller/controller_state";
  arm.adapter.scale_topic = prefix + "/arm_controller/speed_scaling_input";
  arm.adapter.lane_facts.joint_names = {"source_joint_1"};
  arm.adapter.lane_facts.supports_scale = true;
  arm.adapter.lane_facts.state_rate_hz = 100.0;
  arm.adapter.lane_facts.feedback_rate_hz = 20.0;
  arm.adapter.lane_facts.controller_update_rate_hz = 100.0;
  arm.controller = {prefix + "/controller_manager", "arm_controller",
                    "TestSystem"};
  arm.compiled_joint_names = {"joint_1"};
  return arm;
}

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

[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration
isaac_runtime_configuration(std::string profile) {
  deployment::ros2_control_execution_runtime_contract::Configuration result;
  for (const char *component : {"robot_1", "robot_a", "robot_b", "robot.1"}) {
    result.controller_profiles.push_back(
        {core::ControllableComponentId(component), profile, std::nullopt});
  }
  result.planner_rng_seed = 12345U;
  result.initial_joint_positions_rad = {0.0};
  result.execution_tick_hz = 100U;
  result.collision_monitor_period_ms = 50.0;
  return result;
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
  const auto config = make_retained_isaac_ros2_control_backend_config(
      isaac_runtime_configuration("lite6"), commanded_binding(),
      commanded_gripper_binding());

  const auto state = retained_ros2_control_motion_state_config(config);

  EXPECT_EQ(state.source_kind, core::ObservedStateSourceKind::kCommandedState);
  EXPECT_EQ(state.freshness_budget_ns, 0);
  ASSERT_EQ(state.source_precedence.size(), 1U);
  EXPECT_EQ(state.source_precedence.front().authority_profile,
            core::ObservedRobotStateAuthorityProfile::kCommandedSimulation);
  auto measured = config;
  measured.participant.stop.trip_authority = core::StateAuthorityClass::kMeasured;
  EXPECT_THROW(static_cast<void>(retained_ros2_control_motion_state_config(measured)),
               std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedIsaacProfileExpandsToExactControllerAndGripperContract) {
  const auto runtime = isaac_runtime_configuration("lite6");

  const auto selected = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding(), commanded_gripper_binding());

  EXPECT_EQ(selected.node_namespace, "/manyforge/robot_1");
  EXPECT_EQ(selected.executor_thread_count, 3U);
  const auto &adapter = selected.arm.adapter;
  EXPECT_EQ(adapter.action_name,
            "/manyforge/robot_1/isaac_arm_controller/follow_joint_trajectory");
  EXPECT_EQ(adapter.state_topic,
            "/manyforge/robot_1/isaac_arm_controller/controller_state");
  EXPECT_EQ(adapter.scale_topic,
            "/manyforge/robot_1/isaac_arm_controller/speed_scaling_input");
  // The controller's own joint names (the binding's source joints).
  EXPECT_EQ(adapter.lane_facts.joint_names,
            (std::vector<std::string>{"source_joint_1"}));
  EXPECT_TRUE(adapter.lane_facts.supports_scale);
  EXPECT_TRUE(adapter.lane_facts.has_measured_velocity);
  EXPECT_EQ(adapter.lane_facts.goal_tolerance_policy,
            core::ExecutionGoalTolerancePolicy::kErased);
  EXPECT_DOUBLE_EQ(adapter.lane_facts.state_rate_hz, 100.0);
  EXPECT_DOUBLE_EQ(adapter.lane_facts.feedback_rate_hz, 20.0);
  EXPECT_DOUBLE_EQ(adapter.lane_facts.controller_update_rate_hz, 100.0);
  EXPECT_EQ(selected.arm.controller,
            (RetainedRos2ControlArmController{
                "/manyforge/robot_1/controller_manager", "isaac_arm_controller",
                "IsaacLite6TopicBasedSystem"}));
  const auto &participant = selected.participant;
  EXPECT_EQ(participant.stop.trip_authority, core::StateAuthorityClass::kCommanded);
  EXPECT_DOUBLE_EQ(participant.stop.stop_traj_delta_s, 0.1);
  EXPECT_DOUBLE_EQ(participant.stop.sample_period_s, 0.01);
  EXPECT_EQ(participant.stop.max_deceleration_on_cancel_rad_s2,
            (std::map<std::string, double>{{"joint_1", 8.0}}));
  EXPECT_EQ(participant.stop.hard_stop_completion_timeout_ns, 1'500'000'000);
  ASSERT_TRUE(participant.stop.replacement_start_allowance_s.has_value());
  EXPECT_DOUBLE_EQ(*participant.stop.replacement_start_allowance_s, 0.020);
  EXPECT_DOUBLE_EQ(participant.late_cycle_margin_s, 2.0 / 100.0);
  ASSERT_TRUE(participant.tracking.has_value());
  EXPECT_DOUBLE_EQ(participant.tracking->tolerance_rad, 0.05);
  EXPECT_DOUBLE_EQ(participant.tracking->timeout_s, 0.0);
  ASSERT_TRUE(selected.isaac_guard.has_value());
  EXPECT_EQ(selected.isaac_guard->joint_commands_topic, "/isaac_joint_commands");
  EXPECT_EQ(selected.isaac_guard->joint_states_topic, "/isaac_joint_states");
  EXPECT_EQ(selected.isaac_guard->joint_state_max_age_ns, 250'000'000);
  ASSERT_TRUE(selected.gripper.has_value());
  EXPECT_EQ(selected.gripper->action_name,
            "/manyforge/robot_1/lite_gripper_controller/gripper_cmd");
  EXPECT_EQ(selected.gripper->transport_kind, "isaac_joint_state_effort");
  EXPECT_EQ(selected.gripper->command_joint_names,
            (std::vector<std::string>{"source_gripper_joint"}));
  EXPECT_EQ(selected.gripper->state_joint_name, "source_gripper_joint");
  EXPECT_EQ(selected.gripper->group_id, "gripper");
  EXPECT_EQ(selected.gripper->joint_name, "gripper_joint");
  EXPECT_EQ(selected.gripper->completion_policy, "dwell_elapsed");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     SameTypeIsaacInstancesHaveDisjointControlPlaneEndpoints) {
  const auto runtime = isaac_runtime_configuration("ur10e");

  const auto first = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_a"),
      commanded_gripper_binding("robot_a"));
  const auto second = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_b"),
      commanded_gripper_binding("robot_b"));

  ASSERT_TRUE(first.gripper.has_value());
  ASSERT_TRUE(second.gripper.has_value());
  const std::vector<std::string> first_endpoints{
      first.arm.adapter.action_name, first.arm.adapter.state_topic,
      first.arm.adapter.scale_topic, first.arm.controller.controller_manager,
      first.gripper->action_name};
  const std::vector<std::string> second_endpoints{
      second.arm.adapter.action_name, second.arm.adapter.state_topic,
      second.arm.adapter.scale_topic, second.arm.controller.controller_manager,
      second.gripper->action_name};
  for (std::size_t index = 0; index < first_endpoints.size(); ++index) {
    EXPECT_NE(first_endpoints[index], second_endpoints[index]);
  }
  // Raw Isaac transport wires and the guard's command topic are lane values,
  // not runtime-owned endpoints.
  EXPECT_EQ(first.gripper->command_topic, second.gripper->command_topic);
  EXPECT_EQ(first.gripper->state_topic, second.gripper->state_topic);
  EXPECT_EQ(first.isaac_guard->joint_commands_topic,
            second.isaac_guard->joint_commands_topic);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedIsaacNamespaceRejectsLossyOrCrossedComponentIdentity) {
  const auto runtime = isaac_runtime_configuration("ur10e");

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
     LifecycleCatalogIsAnchoredAndCarriesTheControllerSwitchPrimitives) {
  // Design R6 / §7.5: controllers load inactive and CCLC activates them.
  const auto plans = retained_ros2_control_lifecycle_plans();
  ASSERT_EQ(plans.size(), 4U);
  std::vector<std::string> primitives;
  for (const auto &plan : plans) {
    ASSERT_EQ(plan.transitions.size(), 1U);
    primitives.push_back(plan.transitions.front().lifecycle_primitive_id.value());
  }
  EXPECT_EQ(primitives,
            (std::vector<std::string>{"ros2_control.controller.prepare",
                                      "ros2_control.controller.activate",
                                      "ros2_control.controller.deactivate",
                                      "ros2_control.controller.recover"}));
  EXPECT_EQ(plans.front().requested_postcondition.allowed_states,
            (std::vector<core::ComponentLifecycleStateDTO>{
                core::ComponentLifecycleStateDTO::kPrepared,
                core::ComponentLifecycleStateDTO::kActive}));
  EXPECT_EQ(plans.back().requested_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId(
          std::string(kRos2ControlLifecycleProviderManifestId)),
      plans);
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  // The maintained provider manifest anchors exactly these plans.
  EXPECT_EQ(semantic.value().value(), kMaintainedRos2ControlCatalogSemanticDigest);

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
  auto factory = make_retained_ros2_control_backend_factory(
      make_retained_isaac_ros2_control_backend_config(
          isaac_runtime_configuration("lite6"), commanded_binding(),
          commanded_gripper_binding()));
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
     IsaacLaneRefusesMissingRequiredLaneTimingInputs) {
  // The explicit required-input guard still refuses missing timing inputs
  // before any ROS entity exists, even though Q55 supplies the lane values.
  const auto motion = commanded_binding();
  const auto gripper = commanded_gripper_binding();
  auto config = make_retained_isaac_ros2_control_backend_config(
      isaac_runtime_configuration("lite6"), motion, gripper);
  config.participant.stop.replacement_start_allowance_s.reset();
  config.participant.late_cycle_margin_s = 0.0;
  auto factory = make_retained_ros2_control_backend_factory(std::move(config));
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  try {
    static_cast<void>(factory(motion, gripper, nullptr, nullptr, clock, scheduler));
    FAIL() << "composition without required lane inputs must refuse";
  } catch (const std::invalid_argument &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("replacement_start_allowance_s"), std::string::npos)
        << message;
    EXPECT_NE(message.find("late_cycle_margin_s"), std::string::npos) << message;
  }
}

// This fixture owns the real ROS transport, the epoch-1 JTC adapter, the Core
// participant over it and the gripper adapter; no executor or remote
// controller is started by the receipt tests.
struct InitialRosInstallationFixture final {
  bool include_gripper;
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<NullExecutionStateSink> sink =
      std::make_shared<NullExecutionStateSink>();
  core::RuntimeParticipantBinding motion = commanded_binding();
  core::RuntimeParticipantBinding tool = commanded_gripper_binding();
  core::ObservedRobotStatePublication publication{
      {"joint_1"},
      {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}}};
  core::IExecutionBackend *backend = nullptr;
  ros2_control_execution::Ros2GripperCommandBackend *gripper = nullptr;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle =
      std::make_shared<RetainedRos2ControlRuntimeHandle>();
  // The test-only receipt engine borrows the handle's transport resources.
  std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
      receipt_engine_backend;
  std::unique_ptr<core::GripperParticipantEngine> motion_engine;
  std::unique_ptr<core::GripperParticipantEngine> gripper_engine;
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  const core::ContentDigest identity{std::string("sha256:") +
                                     std::string(64, 'a')};
  component_manager::ManagerSelectedRuntimeCompositionRequestDTO request{
      identity,
      core::ApplyId("apply.initial"),
      core::ApplyGeneration(1U),
      {motion.value().retained_bundle_id,
       motion.value().graph_revision,
       {core::SceneId(identity.value()), 1U, identity}},
      core::ActiveBundlePublicationGeneration(1U),
      identity,
      identity,
      1U,
      core::RuntimeUnitId("runtime.ros2_control"),
      core::RuntimeEpoch("runtime-epoch.1"),
      core::RuntimeEpoch("manager.1"),
      core::ConfigurationFenceId("fence.initial"),
      core::ConfigurationFenceGeneration(1U),
      {{motion.value().controllable_component_id},
       {{core::ControlDomainId("domain.arm"), std::nullopt,
         core::ControlDomainRevision("domain-revision")},
        {core::ControlDomainId("domain.gripper"), std::nullopt,
         core::ControlDomainRevision("domain-revision")}},
       {{core::ControlBindingId("binding.arm"), std::nullopt,
         core::ControlBindingRevision("binding-revision")},
        {core::ControlBindingId("binding.gripper"), std::nullopt,
         core::ControlBindingRevision("binding-revision")}},
       {core::ResourceId("robot.gripper.1"),
        core::ResourceId("robot.joint.1")}},
      component_manager::ManagerSelectedRuntimeCompositionPhase::kFinalize,
      {{{motion.value().controllable_component_id,
         deployment::ApplyComponentDisposition::kAdd,
         std::nullopt,
         identity,
         {},
         {core::ControlDomainId("domain.arm"),
          core::ControlDomainId("domain.gripper")},
         {}}},
       false,
       {}},
      {core::RetainedBundleId("empty.bundle"),
       core::GraphRevision("empty.graph"),
       {core::SceneId(identity.value()), 1U, identity}}};
  core::CellGraphDTO predecessor{"manyforge.cell_graph/v1",
                                 core::GraphRevision("empty.graph"),
                                 core::CellId("cell"),
                                 {},
                                 {},
                                 {},
                                 {},
                                 {},
                                 {}};
  core::CellGraphDTO candidate{"manyforge.cell_graph/v1",
                               motion.value().graph_revision,
                               predecessor.cell_id,
                               {},
                               {},
                               {{motion.value().controllable_component_id,
                                 motion.value().component_type_id,
                                 motion.value().component_type_revision,
                                 {},
                                 {request.runtime_unit_id},
                                 std::nullopt}},
                               {},
                               {},
                               {}};

  explicit InitialRosInstallationFixture(bool with_gripper = true,
                                         bool component_local_gripper = false)
      : include_gripper(with_gripper) {
    if (component_local_gripper) {
      auto value = tool.value();
      value.model_groups.clear();
      for (auto &resource : value.resources)
        resource.source_joint_name.reset();
      auto sealed = core::verify_runtime_participant_binding(std::move(value));
      if (!sealed.ok())
        throw std::logic_error(sealed.error().message);
      tool = std::move(sealed.value());
    }
    if (!include_gripper) {
      auto &domains = request.configuration_fence_scope.domains;
      auto &bindings = request.configuration_fence_scope.bindings;
      auto &components = request.component_delta.components;
      if (domains.size() < 2 || bindings.size() < 2 || components.empty() ||
          components.front().candidate_domains.size() < 2) {
        throw std::logic_error("initial installation fixture lacks gripper rows");
      }
      auto &candidate_domains = components.front().candidate_domains;
      domains.erase(domains.begin() + 1, domains.end());
      bindings.erase(bindings.begin() + 1, bindings.end());
      request.configuration_fence_scope.resources = {
          core::ResourceId("robot.joint.1")};
      candidate_domains.erase(candidate_domains.begin() + 1,
                              candidate_domains.end());
    }
    auto resources = make_test_transport_resources(
        "manyforge_initial_install_fixture", sink, clock);
    auto owned_gripper =
        std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
            *resources, sink, clock, scheduler, true);
    ros2_control_execution::Ros2GripperActionConfig gripper_config;
    gripper_config.group_id = "gripper";
    gripper_config.joint_name = "gripper_joint";
    owned_gripper->configure_gripper_action(
        tool.value().command_target_id.value(), gripper_config);
    auto *engine_backend = owned_gripper.get();
    gripper = include_gripper ? engine_backend : nullptr;
    if (!include_gripper)
      receipt_engine_backend = std::move(owned_gripper);
    const auto bound = handle->Bind(
        std::move(resources), clock,
        test_arm_transport("/initial_install_fixture"),
        [this](std::shared_ptr<core::IExecutionPort> port) {
          return std::make_shared<core::ExecutionPortParticipant>(
              motion, std::move(port), clock, scheduler,
              [](core::TimePoint, const std::stop_token &) {},
              core::ExecutionPortParticipantStateAuthority{
                  &publication, [] { return core::Result<void>::Ok(); }},
              test_participant_config());
        },
        std::move(owned_gripper), motion,
        include_gripper ? std::optional(tool) : std::nullopt);
    if (!bound.ok())
      throw std::logic_error("initial ROS fixture binding failed: " +
                             bound.error().message);
    backend = handle->participant().get();
    // This receipt fixture tests exact registration identity, not motion.
    motion_engine = std::make_unique<core::GripperParticipantEngine>(
        *engine_backend, clock);
    gripper_engine = std::make_unique<core::GripperParticipantEngine>(
        *engine_backend, clock);
    std::vector<const core::RuntimeParticipantBinding *> physical{&motion};
    if (include_gripper)
      physical.push_back(&tool);
    for (const auto *binding : physical) {
      core::RuntimeParticipantExecutionRegistration registration{
          *binding,     std::nullopt,
          {},           core::MotionInterruptNodeCategory::kGeneric,
          nullptr,      nullptr,
          std::nullopt, nullptr,
          std::nullopt, std::nullopt,
          std::nullopt};
      registration.engine =
          binding == &motion ? motion_engine.get() : gripper_engine.get();
      registration.stop_execution_port =
          static_cast<core::IControlDomainExecutionPort *>(
              handle->participant().get());
      registrations.push_back(std::move(registration));
    }
  }

  component_manager::ManagerRuntimeComponentReceiptDTO receipt() const {
    return detail::ReadRos2ComponentReceipt(
        request, motion.value().controllable_component_id, identity, *handle,
        backend, gripper, motion,
        include_gripper ? std::optional(tool) : std::nullopt, registrations,
        motion_engine.get(), include_gripper ? gripper_engine.get() : nullptr,
        nullptr);
  }
};

TEST(RetainedRos2ControlBackendFactoryTest,
     GripperlessReceiptRequiresOnlyItsExactMotionParticipant) {
  InitialRosInstallationFixture fixture(false);
  ASSERT_TRUE(fixture.request.valid());
  const auto receipt = fixture.receipt();
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request,
                                                               {receipt}));
  EXPECT_TRUE(fixture.handle->OwnsBackend(fixture.backend, fixture.motion,
                                          std::nullopt));
  EXPECT_FALSE(fixture.handle->OwnsBackend(fixture.backend, fixture.motion,
                                           fixture.tool));
  fixture.registrations.front().stop_execution_port = nullptr;
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     InitialInstallReceiptProvesActualOwnedParticipants) {
  InitialRosInstallationFixture fixture;
  ASSERT_TRUE(fixture.request.valid());
  const auto first = fixture.receipt();
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request,
                                                               {first}));
  EXPECT_EQ(fixture.receipt(), first);
  EXPECT_FALSE(first.predecessor_runtime_retained);
  EXPECT_FALSE(first.product_binding_resealed);
  // Nothing was activated: the epoch-1 port is not ready.
  const auto port = fixture.handle->current_port();
  ASSERT_NE(port, nullptr);
  EXPECT_FALSE(port->ReadReadiness().controller_active);
  EXPECT_EQ(port->ReadReadiness().binding_epoch, core::ExecutionBindingEpoch(1U));
  EXPECT_EQ(port->ReadLaneFacts().joint_names,
            (std::vector<std::string>{"joint_1"}));
  EXPECT_EQ(fixture.receipt(), first);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RuntimeHandleReadsDistinctGripperAndRejectsForeignReceiptIdentity) {
  InitialRosInstallationFixture fixture;
  EXPECT_TRUE(fixture.handle->OwnsBackend(
      fixture.backend, fixture.motion, fixture.tool));
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper, fixture.tool));
  EXPECT_FALSE(fixture.handle->OwnsGripper(nullptr, fixture.tool));
  EXPECT_FALSE(fixture.handle->OwnsGripper(
      fixture.gripper, commanded_gripper_binding("another.robot")));
  const auto readiness = fixture.handle->gripper_transport_readiness_snapshot();
  ASSERT_TRUE(readiness.has_value());
  EXPECT_TRUE(readiness->configured);
  auto *const configured_gripper = fixture.gripper;
  fixture.gripper = nullptr;
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  InitialRosInstallationFixture foreign_fixture;
  fixture.gripper = foreign_fixture.gripper;
  EXPECT_FALSE(fixture.handle->OwnsGripper(fixture.gripper, fixture.tool));
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.gripper = configured_gripper;
  EXPECT_NO_THROW(static_cast<void>(fixture.receipt()));
  const auto rebound = fixture.handle->Bind(
      make_test_transport_resources("manyforge_initial_install_rebind",
                                    fixture.sink, fixture.clock),
      fixture.clock, test_arm_transport("/initial_install_rebind"),
      [](std::shared_ptr<core::IExecutionPort>) {
        return std::shared_ptr<core::ExecutionPortParticipant>{};
      },
      nullptr, fixture.motion, std::nullopt);
  ASSERT_FALSE(rebound.ok());
  EXPECT_EQ(rebound.error().details.at("reason"),
            "ros2_control_handle_already_bound");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     ProductResealPreservesGripperOccurrenceAndChecksCandidateStateAuthority) {
  // This gate test needs an addressed channel, without retained-model geometry.
  // Construct a genuinely component-local gripper before binding its owner.
  InitialRosInstallationFixture fixture(true, true);
  const auto port = fixture.handle->current_port();
  const auto participant = fixture.handle->participant();
  const auto channel_for = [&](const auto &binding) {
    return std::make_shared<RetainedObservedStateChannel>(
        binding, nullptr,
        RetainedObservedStateChannelConfig{
            core::ObservedStateSourceKind::kCommandedState, "gripper.commanded",
            250'000'000, 1U},
        &fixture.clock);
  };
  auto value = fixture.tool.value();
  value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  value.graph_revision = core::GraphRevision("candidate.graph");
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;
  const auto candidate = std::move(sealed.value());
  const auto predecessor_channel = channel_for(fixture.tool);
  const auto candidate_channel = channel_for(candidate);
  // Product seals do not change the physical observed-state query authority.
  ASSERT_EQ(predecessor_channel->query_context(),
            candidate_channel->query_context());
  auto foreign_channel_value = fixture.tool.value();
  foreign_channel_value.control_path->control_binding_id =
      core::ControlBindingId("foreign.state-binding");
  auto foreign_channel_binding = core::verify_runtime_participant_binding(
      std::move(foreign_channel_value));
  ASSERT_TRUE(foreign_channel_binding.ok())
      << foreign_channel_binding.error().message;
  const auto foreign_channel = channel_for(foreign_channel_binding.value());
  ASSERT_NE(foreign_channel->query_context(),
            candidate_channel->query_context());
  EXPECT_FALSE(
      fixture.handle->RebindGripperRoute(candidate, foreign_channel).ok());
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper, fixture.tool));
  const auto retained_channel_rebound =
      fixture.handle->RebindGripperRoute(candidate, predecessor_channel);
  ASSERT_TRUE(retained_channel_rebound.ok())
      << retained_channel_rebound.error().message;
  const auto rebound =
      fixture.handle->RebindGripperRoute(candidate, candidate_channel);
  ASSERT_TRUE(rebound.ok()) << rebound.error().message;
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper, candidate));
  EXPECT_FALSE(fixture.handle->OwnsGripper(fixture.gripper, fixture.tool));
  EXPECT_EQ(fixture.handle->current_port(), port);
  EXPECT_EQ(fixture.handle->participant(), participant);

  for (const bool change_provider : {false, true}) {
    auto foreign_value = candidate.value();
    if (change_provider) {
      foreign_value.control_path->provider_instance_id =
          core::ProviderInstanceId("foreign.provider");
    } else {
      foreign_value.control_path->control_binding_id =
          core::ControlBindingId("foreign.binding");
    }
    auto foreign =
        core::verify_runtime_participant_binding(std::move(foreign_value));
    ASSERT_TRUE(foreign.ok()) << foreign.error().message;
    EXPECT_FALSE(
        fixture.handle
            ->RebindGripperRoute(foreign.value(), channel_for(foreign.value()))
            .ok());
    EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper, candidate));
  }
  ASSERT_TRUE(fixture.handle
                  ->RebindGripperRoute(fixture.tool, channel_for(fixture.tool))
                  .ok());
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper, fixture.tool));
  EXPECT_EQ(fixture.handle->current_port(), port);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainingOnlyTheGripperPortRetainsBothBorrowedResourceUsers) {
  InitialRosInstallationFixture fixture;
  fixture.motion_engine.reset();
  fixture.gripper_engine.reset();
  fixture.registrations.clear();
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner = fixture.handle;
  auto port = std::shared_ptr<core::IGripperCommandBackend>(
      fixture.handle, fixture.gripper);
  fixture.handle.reset();
  EXPECT_FALSE(owner.expired());
  EXPECT_EQ(port->get_gripper_status(
                    fixture.tool.value().command_target_id.value()).state,
            core::GripperCommandState::kIdle);
  port.reset();
  EXPECT_TRUE(owner.expired());
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainingOnlyTheMotionPortRetainsTheSharedResourceOwner) {
  InitialRosInstallationFixture fixture;
  fixture.motion_engine.reset();
  fixture.gripper_engine.reset();
  fixture.registrations.clear();
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner = fixture.handle;
  auto port = std::shared_ptr<core::IExecutionBackend>(fixture.handle,
                                                       fixture.backend);
  fixture.handle.reset();
  EXPECT_FALSE(owner.expired());
  EXPECT_EQ(port->get_execution_snapshot().state, core::ExecutionState::kIdle);
  port.reset();
  EXPECT_TRUE(owner.expired());
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedPreparePreconditionRefusesKnownExitWithoutReplacingOwner) {
  InitialRosInstallationFixture fixture;
  fixture.request.phase =
      component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare;
  auto &row = fixture.request.component_delta.components.front();
  row.disposition = deployment::ApplyComponentDisposition::kRetain;
  row.predecessor_runtime_identity = row.candidate_runtime_identity;
  row.predecessor_domains = row.candidate_domains;
  ASSERT_TRUE(fixture.request.valid());
  const auto owner = fixture.handle;
  const auto *const backend = fixture.backend;
  const auto component = row.controllable_component_id;
  ASSERT_TRUE(owner->OwnsBackend(backend, fixture.motion, fixture.tool));
  // Exercise the production precondition called before the host starts its
  // selected composition builder, separately from the installed receipt.
  EXPECT_NO_THROW(detail::RefuseExitedRos2Retain(component, std::nullopt));
  const std::string exited =
      "controller launch exited with status 17 pid=321 "
      "observed_utc=2026-10-11T19:00:00Z";
  try {
    detail::RefuseExitedRos2Retain(component, exited);
    FAIL() << "a known exited launch passed the Retain prepare precondition";
  } catch (const std::invalid_argument &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(component.value()), std::string::npos);
    EXPECT_NE(message.find(exited), std::string::npos);
    EXPECT_NE(message.find("bring-up required"), std::string::npos);
  }
  EXPECT_EQ(fixture.handle, owner);
  EXPECT_EQ(fixture.backend, backend);
  EXPECT_TRUE(owner->OwnsBackend(backend, fixture.motion, fixture.tool));
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedReceiptRequiresTheExactPredecessorHandle) {
  InitialRosInstallationFixture fixture;
  auto &row = fixture.request.component_delta.components.front();
  row.disposition = deployment::ApplyComponentDisposition::kRetain;
  row.predecessor_runtime_identity = row.candidate_runtime_identity;
  row.predecessor_domains = row.candidate_domains;
  const auto read = [&](const RetainedRos2ControlRuntimeHandle *predecessor,
                        const std::optional<std::string> &process_exit =
                            std::nullopt) {
    return detail::ReadRos2ComponentReceipt(
        fixture.request, fixture.motion.value().controllable_component_id,
        fixture.identity, *fixture.handle, fixture.backend, fixture.gripper,
        fixture.motion, fixture.tool, fixture.registrations,
        fixture.motion_engine.get(), fixture.gripper_engine.get(), predecessor,
        process_exit);
  };
  ASSERT_TRUE(fixture.request.valid());
  EXPECT_THROW(static_cast<void>(read(nullptr)), std::invalid_argument);
  InitialRosInstallationFixture foreign;
  EXPECT_THROW(static_cast<void>(read(foreign.handle.get())),
               std::invalid_argument);
  fixture.request.configuration_fence_scope = {};
  const auto retained = read(fixture.handle.get());
  EXPECT_TRUE(retained.predecessor_runtime_retained);
  EXPECT_TRUE(retained.product_binding_resealed);
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request,
                                                               {retained}));
  // Exact factory-bound adapters still exist after their launch exits. They
  // cannot make that dead occurrence a successful Retain receipt.
  const std::string exited =
      "controller launch was terminated by signal 11 pid=123 "
      "observed_utc=2026-10-11T19:00:00Z";
  try {
    static_cast<void>(read(fixture.handle.get(), exited));
    FAIL() << "an exited launch produced a Retain receipt";
  } catch (const std::invalid_argument &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(
                  fixture.motion.value().controllable_component_id.value()),
              std::string::npos);
    EXPECT_NE(message.find(exited), std::string::npos);
    EXPECT_NE(message.find("bring-up required"), std::string::npos);
  }
  EXPECT_TRUE(fixture.handle->OwnsBackend(fixture.backend, fixture.motion,
                                        fixture.tool));
  fixture.request.configuration_fence_scope =
      foreign.request.configuration_fence_scope;
  row.disposition = deployment::ApplyComponentDisposition::kReplace;
  row.predecessor_runtime_identity =
      core::ContentDigest(std::string("sha256:") + std::string(64, 'b'));
  row.replacement_reasons = {"runtime_units"};
  EXPECT_THROW(static_cast<void>(read(fixture.handle.get())),
               std::invalid_argument);
  EXPECT_FALSE(read(foreign.handle.get()).predecessor_runtime_retained);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     InitialInstallReceiptRejectsStaleForeignAndDuplicateEvidence) {
  InitialRosInstallationFixture fixture;
  const auto original = fixture.request;
  fixture.request.runtime_epoch = core::RuntimeEpoch("stale.epoch");
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  fixture.request.candidate_projection_revision.graph_revision =
      core::GraphRevision("foreign.graph");
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  fixture.request.component_delta.components.front()
      .candidate_runtime_identity =
      core::ContentDigest(std::string("sha256:") + std::string(64, 'b'));
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  fixture.registrations.push_back(fixture.registrations.front());
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.registrations.pop_back();
  fixture.registrations.front().engine = fixture.gripper_engine.get();
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.registrations.front().engine = fixture.motion_engine.get();
  const auto *const stop = fixture.registrations.back().stop_execution_port;
  fixture.registrations.back().stop_execution_port = nullptr;
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.registrations.back().stop_execution_port =
      const_cast<core::IControlDomainExecutionPort *>(stop);
  fixture.request.configuration_fence_scope.resources.pop_back();
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     IsaacControllerDescriptionFactsAreTheLaunchDefaults) {
  // Q69/Q47: the Isaac launch's argument defaults moved unchanged into the
  // profile facts (component name, topics and fixed initial values).
  const auto lite6 = retained_isaac_controller_lane("lite6");
  EXPECT_EQ(lite6.hardware_component_name, "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(lite6.joint_commands_topic, "/isaac_joint_commands");
  EXPECT_EQ(lite6.joint_states_topic, "/isaac_joint_states");
  EXPECT_EQ(lite6.initial_joint_positions_rad,
            (std::vector<double>{0.0, 0.0, 1.57, 0.0, 0.0, 0.0}));
  const auto ur10e = retained_isaac_controller_lane("ur10e");
  EXPECT_EQ(ur10e.hardware_component_name, "IsaacUR10eTopicBasedSystem");
  EXPECT_EQ(ur10e.joint_commands_topic, "/isaac_joint_commands");
  EXPECT_EQ(ur10e.joint_states_topic, "/isaac_joint_states");
  EXPECT_EQ(ur10e.initial_joint_positions_rad,
            (std::vector<double>{2.63422544, -0.875980752, 1.551074106,
                                 -2.29231544, -1.560498884, -2.038544566}));
  EXPECT_THROW(static_cast<void>(retained_isaac_controller_lane("ur5e")),
               std::invalid_argument);

  // The facts generate the Isaac section from a model and binding joints.
  const std::string model =
      "<robot name=\"r\"><link name=\"a\"/><link name=\"b\"/>"
      "<joint name=\"joint1\" type=\"revolute\"><parent link=\"a\"/>"
      "<child link=\"b\"/></joint></robot>";
  auto lane = lite6;
  lane.initial_joint_positions_rad.resize(1U);
  const auto generated = generate_controller_robot_description(
      ControllerRobotDescriptionRequest{model, {"joint1"}, lane});
  EXPECT_NE(generated.find("<ros2_control name=\"IsaacLite6TopicBasedSystem\" "
                           "type=\"system\">"),
            std::string::npos);
  EXPECT_NE(generated.find("<plugin>topic_based_ros2_control/TopicBasedSystem"
                           "</plugin>"),
            std::string::npos);
}

} // namespace
TEST(RetainedRos2ControlBackendFactoryTest,
     EachComponentUsesItsOwnControllerProfileInTheOneRosUnit) {
  // Q68: a mixed cell (Lite 6 + UR10e) hosts both components in one ROS unit;
  // each one selects its own controller profile.
  deployment::ros2_control_execution_runtime_contract::Configuration runtime;
  runtime.planner_rng_seed = 12345U;
  runtime.initial_joint_positions_rad = {0.0, 0.0};
  runtime.execution_tick_hz = 100U;
  runtime.collision_monitor_period_ms = 50.0;
  runtime.controller_profiles = {
      {core::ControllableComponentId("robot_a"), std::string("lite6"),
       std::nullopt},
      {core::ControllableComponentId("robot_b"), std::string("ur10e"),
       std::nullopt},
      {core::ControllableComponentId("robot_c"), std::nullopt,
       std::string("lite6_lite_gripper")}};
  const auto lite6 = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_a"), commanded_gripper_binding("robot_a"));
  const auto ur10e = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_b"), commanded_gripper_binding("robot_b"));
  EXPECT_EQ(lite6.arm.controller.hardware_component_name, "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(ur10e.arm.controller.hardware_component_name, "IsaacUR10eTopicBasedSystem");
  EXPECT_EQ(lite6.gripper->completion_policy, "dwell_elapsed");
  EXPECT_EQ(ur10e.gripper->completion_policy, "position_reached");
  // The offline mock lane is built by its own builder (phase 2b); an unknown
  // component has no profile at all.
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   runtime, commanded_binding("robot_c"),
                   commanded_gripper_binding("robot_c"))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   runtime, commanded_binding("robot_d"),
                   commanded_gripper_binding("robot_d"))),
               std::invalid_argument);
}

namespace {

[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration
mock_runtime_configuration(const std::string &mock_profile) {
  deployment::ros2_control_execution_runtime_contract::Configuration result;
  result.controller_profiles = {
      {core::ControllableComponentId("robot_1"), std::nullopt, mock_profile}};
  result.planner_rng_seed = 12345U;
  result.initial_joint_positions_rad = {0.0};
  result.execution_tick_hz = 100U;
  result.collision_monitor_period_ms = 50.0;
  return result;
}

const std::map<std::string, double, std::less<>> kMockCancel{
    {"source_joint_1", 12.5}};

TEST(RetainedRos2ControlBackendFactoryTest,
     Lite6MockProfileUsesIsaacLaneFactsAndItsOwnControllers) {
  const auto config = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("lite6_lite_gripper"), commanded_binding(),
      commanded_gripper_binding(), kMockCancel);
  EXPECT_EQ(config.node_namespace, "/manyforge/robot_1");
  EXPECT_EQ(config.executor_thread_count, 3U);
  const auto &adapter = config.arm.adapter;
  EXPECT_EQ(adapter.action_name,
            "/manyforge/robot_1/lite6_traj_controller/follow_joint_trajectory");
  EXPECT_EQ(adapter.state_topic,
            "/manyforge/robot_1/lite6_traj_controller/controller_state");
  EXPECT_EQ(adapter.scale_topic,
            "/manyforge/robot_1/lite6_traj_controller/speed_scaling_input");
  EXPECT_EQ(adapter.lane_facts.joint_names,
            (std::vector<std::string>{"source_joint_1"}));
  EXPECT_TRUE(adapter.lane_facts.supports_scale);
  EXPECT_TRUE(adapter.lane_facts.has_measured_velocity);
  EXPECT_EQ(adapter.lane_facts.cancel_semantics,
            core::ExecutionCancelSemantics::kDecelerate);
  EXPECT_EQ(adapter.lane_facts.goal_tolerance_policy,
            core::ExecutionGoalTolerancePolicy::kErased);
  // The UFactory template's cadence (update 150, state 25, monitor 20 Hz).
  EXPECT_DOUBLE_EQ(adapter.lane_facts.controller_update_rate_hz, 150.0);
  EXPECT_DOUBLE_EQ(adapter.lane_facts.state_rate_hz, 25.0);
  EXPECT_DOUBLE_EQ(adapter.lane_facts.feedback_rate_hz, 20.0);
  // Q66: the host's fixed controller manager.
  EXPECT_EQ(config.arm.controller,
            (RetainedRos2ControlArmController{
                "/manyforge/robot_1/controller_manager", "lite6_traj_controller",
                "MockLite6LiteGripperSystem"}));
  const auto &participant = config.participant;
  // Q65: Isaac's lane facts.
  EXPECT_EQ(participant.stop.trip_authority, core::StateAuthorityClass::kCommanded);
  EXPECT_DOUBLE_EQ(participant.stop.stop_traj_delta_s, 0.1);
  EXPECT_DOUBLE_EQ(participant.stop.sample_period_s, 0.01);
  EXPECT_EQ(participant.stop.controller_state_max_age_ns, 250'000'000);
  ASSERT_TRUE(participant.tracking.has_value());
  EXPECT_DOUBLE_EQ(participant.tracking->tolerance_rad, 0.05);
  EXPECT_DOUBLE_EQ(participant.tracking->timeout_s, 0.0);
  // Q48: the generated controllers file's per-joint cancel bound, by
  // compiled joint.
  EXPECT_EQ(participant.stop.max_deceleration_on_cancel_rad_s2,
            (std::map<std::string, double>{{"joint_1", 12.5}}));
  // Q79: Lite 6 family.
  EXPECT_EQ(participant.stop.hard_stop_completion_timeout_ns, 1'500'000'000);
  ASSERT_TRUE(participant.stop.replacement_start_allowance_s.has_value());
  EXPECT_DOUBLE_EQ(*participant.stop.replacement_start_allowance_s, 0.020);
  EXPECT_DOUBLE_EQ(participant.late_cycle_margin_s, 2.0 / 150.0);
  EXPECT_DOUBLE_EQ(participant.initial_speed_factor, 1.0);
  EXPECT_EQ(config.observed_state_authority_profile_id,
            "manyforge.commanded_simulation");
  EXPECT_FALSE(config.isaac_guard.has_value());
  ASSERT_TRUE(config.gripper.has_value());
  EXPECT_EQ(config.gripper->transport_kind, "ros_gripper_command");
  EXPECT_EQ(config.gripper->action_name,
            "/manyforge/robot_1/lite_gripper_controller/gripper_cmd");
  EXPECT_EQ(config.gripper->completion_policy, "position_reached");
  EXPECT_DOUBLE_EQ(config.gripper->result_timeout_s, 5.0);
  EXPECT_EQ(config.gripper->command_joint_names,
            (std::vector<std::string>{"source_gripper_joint"}));
  EXPECT_EQ(config.gripper->state_joint_name, "source_gripper_joint");
  EXPECT_EQ(config.gripper->group_id, "gripper");
  EXPECT_EQ(config.gripper->joint_name, "gripper_joint");
  EXPECT_TRUE(config.gripper->command_topic.empty());
  EXPECT_FALSE(config.gripper->publish_nominal_position);
  const auto state = retained_ros2_control_motion_state_config(config);
  EXPECT_EQ(state.source_kind, core::ObservedStateSourceKind::kCommandedState);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     UrFamilyMockProfilesUseUrValuesAndUr5HasNoGripper) {
  for (const auto *profile : {"ur5e_robotiq_2f_85", "ur10e_robotiq_2f_140"}) {
    const auto config = make_retained_mock_ros2_control_backend_config(
        mock_runtime_configuration(profile), commanded_binding(),
        commanded_gripper_binding(), kMockCancel);
    EXPECT_EQ(config.arm.adapter.action_name,
              "/manyforge/robot_1/joint_trajectory_controller/"
              "follow_joint_trajectory")
        << profile;
    EXPECT_DOUBLE_EQ(config.arm.adapter.lane_facts.controller_update_rate_hz,
                     100.0);
    EXPECT_DOUBLE_EQ(config.arm.adapter.lane_facts.state_rate_hz, 100.0);
    EXPECT_DOUBLE_EQ(config.arm.adapter.lane_facts.feedback_rate_hz, 20.0);
    ASSERT_TRUE(config.participant.stop.replacement_start_allowance_s.has_value());
    EXPECT_DOUBLE_EQ(*config.participant.stop.replacement_start_allowance_s, 0.020);
    EXPECT_DOUBLE_EQ(config.participant.late_cycle_margin_s, 2.0 / 100.0);
    EXPECT_EQ(config.participant.stop.hard_stop_completion_timeout_ns,
              500'000'000)
        << profile;
    ASSERT_TRUE(config.gripper.has_value());
    EXPECT_EQ(config.gripper->action_name,
              "/manyforge/robot_1/robotiq_gripper_controller/gripper_cmd");
    EXPECT_EQ(config.gripper->completion_policy, "position_reached");
    EXPECT_DOUBLE_EQ(config.gripper->result_timeout_s, 15.0);
  }
  const auto ur5 = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("ur5_roboplan"), commanded_binding(),
      std::nullopt, kMockCancel);
  EXPECT_FALSE(ur5.gripper.has_value());
  EXPECT_EQ(ur5.arm.controller.hardware_component_name, "MockUR5System");
  EXPECT_EQ(ur5.participant.stop.hard_stop_completion_timeout_ns, 500'000'000);
}

TEST(RetainedRos2ControlBackendFactoryTest, MockProfileConfigurationFailsClosed) {
  // A gripper the profile does not have, or a missing required one.
  EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                   mock_runtime_configuration("ur5_roboplan"),
                   commanded_binding(), commanded_gripper_binding(),
                   kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                   mock_runtime_configuration("lite6_lite_gripper"),
                   commanded_binding(), std::nullopt, kMockCancel)),
               std::invalid_argument);
  // The cancel bounds must cover exactly the arm joints with positive values.
  const std::vector<std::map<std::string, double, std::less<>>> bad_cancel{
      {},
      {{"source_joint_1", 0.0}},
      {{"other_joint", 1.0}},
      {{"source_joint_1", 1.0}, {"other_joint", 1.0}}};
  for (const auto &cancel : bad_cancel) {
    EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                     mock_runtime_configuration("lite6_lite_gripper"),
                     commanded_binding(), commanded_gripper_binding(), cancel)),
                 std::invalid_argument);
  }
  // An Isaac profile is not built by the mock builder, nor the reverse.
  EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                   isaac_runtime_configuration("lite6"), commanded_binding(),
                   commanded_gripper_binding(), kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   mock_runtime_configuration("lite6_lite_gripper"),
                   commanded_binding(), commanded_gripper_binding())),
               std::invalid_argument);
  // Crossed component identities.
  EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                   mock_runtime_configuration("lite6_lite_gripper"),
                   commanded_binding(), commanded_gripper_binding("robot_b"),
                   kMockCancel)),
               std::invalid_argument);
}

} // namespace

} // namespace manyforge::roboplan_backend
