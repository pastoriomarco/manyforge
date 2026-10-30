#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>

#include <gtest/gtest.h>
#include <roboplan/core/scene.hpp>
#include "managed_ros2_component_installation.hpp"
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

/// The maintained provider manifest's catalog anchor
/// (resources/maintained/catalog/provider_manifest/manyforge.provider.ros2_control.json).
constexpr const char *kMaintainedRos2ControlCatalogSemanticDigest =
    "sha256:54f324571f97808d6ed86de3c5d9adf88314dd602e99aab82b022c5e2f3e1ff6";

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
commanded_binding(std::string component_id = "robot_1",
                  const core::StateAuthorityClass authority =
                      core::StateAuthorityClass::kCommanded) {
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
          core::StateAuthorityId(authority == core::StateAuthorityClass::kMeasured
                                     ? "state.measured"
                                     : "state.commanded"),
          authority,
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

/// CP8 step 1: a Real arm's binding (measured authority).
[[nodiscard]] core::RuntimeParticipantBinding measured_binding() {
  return commanded_binding("robot_1", core::StateAuthorityClass::kMeasured);
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

// These fixture rows supply the same strict actuation fact as the maintained
// Lite 6 pneumatic and UR position source rows. Transport/profile strings are
// never the production authority for actuation or output mode.
[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration::
    GripperTargetConfiguration
    fixture_gripper_row(const std::string &component,
                        const std::string &actuation) {
  return {{core::ControllableComponentId(component),
           core::CommandTargetId("gripper.target"),
           "fixture gripper",
           actuation,
           "open",
           "closed",
           0.2,
           {},
           "mount",
           "attach",
           "tcp",
           {}},
          core::GripperOutputMode::kCommanded};
}

// CP9 stream rows every ROS component declares (the maintained values; the
// fixture gripper budget is 0.75 s to tell it apart from code defaults).
void add_component_streams(
    deployment::ros2_control_execution_runtime_contract::Configuration &result) {
  for (const auto &profile : result.controller_profiles) {
    const bool isaac = profile.isaac_profile.has_value();
    result.component_streams.push_back(
        {profile.controllable_component_id, "measured_joints", 250'000'000,
         isaac ? core::StreamSeverityDTO::kWarning : core::StreamSeverityDTO::kFault,
         isaac ? core::StreamRecoveryDTO::kAuto : core::StreamRecoveryDTO::kAck});
  }
  for (const auto &row : result.gripper_target_configurations)
    result.component_streams.push_back(
        {row.controllable_component_id, "gripper_feedback", 750'000'000,
         core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck});
  std::sort(result.component_streams.begin(), result.component_streams.end(),
            [](const auto &left, const auto &right) {
              return std::forward_as_tuple(left.controllable_component_id.value(),
                                           left.stream_id) <
                     std::forward_as_tuple(right.controllable_component_id.value(),
                                           right.stream_id);
            });
}

// Merge 6: an Isaac robot row (`lite6`, `ur10e`) is arm-only; a tool row
// (`lite_gripper`, `robotiq_2f_140`) is its own component with the gripper
// target. The fixture components all select `profile`.
[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration
isaac_runtime_configuration(std::string profile) {
  deployment::ros2_control_execution_runtime_contract::Configuration result;
  const bool tool = profile == "lite_gripper" || profile == "robotiq_2f_140";
  for (const char *component : {"robot_1", "robot_a", "robot_b", "robot.1"}) {
    result.controller_profiles.push_back(
        {core::ControllableComponentId(component), profile, std::nullopt});
    if (tool)
      result.gripper_target_configurations.push_back(fixture_gripper_row(
          component, profile == "lite_gripper" ? "pneumatic" : "position_commanded"));
  }
  add_component_streams(result);
  result.planner_rng_seed = 12345U;
  result.initial_joint_positions_rad = {0.0};
  result.execution_tick_hz = 100U;
  result.collision_monitor_period_ms = 50.0;
  return result;
}

/// An Isaac tool's gripper, composed as the host composes a gripper-only tool
/// owner: its row's transport in its own namespace, then its output row.
[[nodiscard]] RetainedRos2ControlBackendConfig isaac_tool_config(
    const deployment::ros2_control_execution_runtime_contract::Configuration &runtime,
    const core::RuntimeParticipantBinding &tool) {
  const auto *selected = deployment::ros2_control_execution_runtime_contract::
      find_controller_profile(runtime, tool.value().controllable_component_id);
  if (selected == nullptr || !selected->isaac_profile.has_value())
    throw std::logic_error("fixture tool has no Isaac row");
  RetainedRos2ControlBackendConfig config;
  config.node_namespace = retained_isaac_ros_namespace(tool);
  configure_retained_isaac_gripper_action(config, tool, *selected->isaac_profile,
                                          config.node_namespace);
  configure_retained_gripper_output_mode(config, runtime, tool);
  return config;
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
      isaac_runtime_configuration("lite6"), commanded_binding(), std::nullopt);

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
  // Merge 6: the robot row's arm, and its tool row's gripper (own component).
  const auto selected = make_retained_isaac_ros2_control_backend_config(
      isaac_runtime_configuration("lite6"), commanded_binding(), std::nullopt);
  const auto tool = isaac_tool_config(isaac_runtime_configuration("lite_gripper"),
                                      commanded_gripper_binding("robot_a"));

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
  EXPECT_FALSE(adapter.lane_facts.supports_scheduled_replacement);
  EXPECT_TRUE(adapter.lane_facts.has_measured_velocity);
  // Merge 6 arrival: the lane keeps its JTC goal tolerances on every goal.
  EXPECT_EQ(adapter.lane_facts.goal_tolerance_policy,
            core::ExecutionGoalTolerancePolicy::kKept);
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
  ASSERT_TRUE(selected.arm.measured_joint_state_lane.has_value());
  EXPECT_EQ(selected.arm.measured_joint_state_lane->absolute_topic,
            "/isaac_joint_states");
  EXPECT_FALSE(selected.gripper.has_value());
  ASSERT_TRUE(tool.gripper.has_value());
  EXPECT_EQ(tool.gripper->action_name,
            "/manyforge/robot_a/lite_gripper_controller/gripper_cmd");
  EXPECT_EQ(tool.gripper->transport_kind, "isaac_joint_state_effort");
  EXPECT_EQ(tool.gripper->command_joint_names,
            (std::vector<std::string>{"source_gripper_joint"}));
  EXPECT_EQ(tool.gripper->state_joint_name, "source_gripper_joint");
  EXPECT_EQ(tool.gripper->group_id, "gripper");
  EXPECT_EQ(tool.gripper->joint_name, "gripper_joint");
  EXPECT_EQ(tool.gripper->completion_policy, "output_delivery");
}

TEST(RetainedRos2ControlBackendFactoryTest,
     SameTypeIsaacInstancesHaveDisjointControlPlaneEndpoints) {
  // Merge 6: two robots and their two tools, each its own component.
  const auto runtime = isaac_runtime_configuration("ur10e");
  const auto tools = isaac_runtime_configuration("robotiq_2f_140");

  const auto first = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_a"), std::nullopt);
  const auto second = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_b"), std::nullopt);
  const auto first_tool =
      isaac_tool_config(tools, commanded_gripper_binding("robot_a"));
  const auto second_tool =
      isaac_tool_config(tools, commanded_gripper_binding("robot_b"));

  ASSERT_TRUE(first_tool.gripper.has_value());
  ASSERT_TRUE(second_tool.gripper.has_value());
  const std::vector<std::string> first_endpoints{
      first.arm.adapter.action_name, first.arm.adapter.state_topic,
      first.arm.adapter.scale_topic, first.arm.controller.controller_manager,
      first_tool.gripper->action_name};
  const std::vector<std::string> second_endpoints{
      second.arm.adapter.action_name, second.arm.adapter.state_topic,
      second.arm.adapter.scale_topic, second.arm.controller.controller_manager,
      second_tool.gripper->action_name};
  for (std::size_t index = 0; index < first_endpoints.size(); ++index) {
    EXPECT_NE(first_endpoints[index], second_endpoints[index]);
  }
  // Raw Isaac gripper transport wires are lane values, not runtime-owned
  // endpoints.
  EXPECT_EQ(first_tool.gripper->command_topic, second_tool.gripper->command_topic);
  EXPECT_EQ(first_tool.gripper->state_topic, second_tool.gripper->state_topic);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainedIsaacNamespaceRejectsLossyOrCrossedComponentIdentity) {
  const auto runtime = isaac_runtime_configuration("ur10e");

  EXPECT_THROW(
      static_cast<void>(make_retained_isaac_ros2_control_backend_config(
          runtime, commanded_binding("robot.1"), std::nullopt)),
      std::invalid_argument);
  EXPECT_THROW(static_cast<void>(isaac_tool_config(
                   isaac_runtime_configuration("robotiq_2f_140"),
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
          isaac_runtime_configuration("lite6"), commanded_binding(), std::nullopt));
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
  auto config = make_retained_isaac_ros2_control_backend_config(
      isaac_runtime_configuration("lite6"), motion, std::nullopt);
  config.participant.stop.replacement_start_allowance_s.reset();
  config.participant.late_cycle_margin_s = 0.0;
  auto factory = make_retained_ros2_control_backend_factory(std::move(config));
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  try {
    static_cast<void>(factory(motion, std::nullopt, nullptr, nullptr, clock, scheduler));
    FAIL() << "composition without required lane inputs must refuse";
  } catch (const std::invalid_argument &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("replacement_start_allowance_s"), std::string::npos)
        << message;
    EXPECT_NE(message.find("late_cycle_margin_s"), std::string::npos) << message;
  }
}

[[nodiscard]] RetainedRos2ControlBackendConfig
test_factory_config(bool measured_lane) {
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_factory_clock_fixture";
  config.node_namespace = "/manyforge/robot_1";
  config.executor_thread_count = 1U;
  config.arm = test_arm_transport(config.node_namespace);
  config.participant = test_participant_config();
  config.observed_state_authority_profile_id = "commanded_simulation";
  config.defer_ros_start = true;
  if (measured_lane) {
    config.arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
        config.node_namespace + "/joint_states", 250'000'000,
        core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
        std::string(kMeasuredJointStateProfileId), core::StreamSeverityDTO::kFault,
        core::StreamRecoveryDTO::kAck, std::nullopt};
  }
  return config;
}

// The model projection of one-joint fixture bindings (chained in order), each
// registered with its own budget. `passive_finger` adds a model joint no
// target binds.
[[nodiscard]] std::shared_ptr<RetainedKinematicStateProjector>
test_projector(const std::vector<core::RuntimeParticipantBinding> &bindings,
               std::vector<std::int64_t> freshness_budgets_ns,
               const bool passive_finger = false) {
  RetainedKinematicModelMapInput model_map{
      bindings.front().value().retained_bundle_id,
      bindings.front().value().graph_revision,
      {}};
  std::string joints;
  std::string parent = "base";
  for (const auto &binding : bindings) {
    const auto &value = binding.value();
    if (value.resources.size() != 1U || value.model_groups.size() != 1U ||
        !value.control_path.has_value())
      throw std::logic_error("clock fixture requires one addressed model joint");
    const auto &resource = value.resources.front();
    const auto &joint = resource.compiled_resource_name;
    const std::string joint_type =
        resource.coordinate_kind == core::CoordinateKind::kAngular
            ? "revolute"
            : "prismatic";
    const std::string child = joint + "_link";
    joints += "<link name='" + child + "'/><joint name='" + joint +
              "' type='" + joint_type + "'><parent link='" + parent +
              "'/><child link='" + child +
              "'/><axis xyz='0 0 1'/><limit lower='-1' upper='1' "
              "effort='5' velocity='2'/></joint>";
    parent = child;
    model_map.control_bindings.push_back(
        {value.control_path->control_binding_id,
         value.controllable_component_id,
         value.command_target_id,
         {{resource.resource_id,
           {{value.model_groups.front().model_instance_id, joint,
             resource.coordinate_kind}}}}});
  }
  if (passive_finger)
    joints += "<link name='finger'/><joint name='finger_joint' "
              "type='prismatic'><parent link='" + parent +
              "'/><child link='finger'/><axis xyz='1 0 0'/><limit lower='0' "
              "upper='0.01' effort='5' velocity='1'/></joint>";
  const std::string urdf =
      "<robot name='clock_fixture' version='1.0'><link name='base'/>" + joints +
      "</robot>";
  auto scene = std::make_shared<roboplan::Scene>(
      "clock_fixture", roboplan::loadUrdfSceneDescriptionFromXml(urdf, {}));
  return std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create(
          std::move(scene), std::make_shared<std::mutex>(), std::move(model_map),
          bindings, std::move(freshness_budgets_ns)));
}

[[nodiscard]] std::shared_ptr<RetainedKinematicStateProjector>
test_projector(const core::RuntimeParticipantBinding &binding,
               const std::int64_t freshness_budget_ns,
               const bool passive_finger = false) {
  return test_projector(std::vector<core::RuntimeParticipantBinding>{binding},
                        std::vector<std::int64_t>{freshness_budget_ns},
                        passive_finger);
}

[[nodiscard]] std::shared_ptr<RetainedObservedStateChannel>
test_projected_channel(const core::RuntimeParticipantBinding &binding,
                       RetainedObservedStateChannelConfig config,
                       core::IClock &clock) {
  // The factory fixtures bind a retained model coordinate, so their channel
  // needs the matching model projection even though no executor is started.
  auto projector = test_projector(binding, config.freshness_budget_ns);
  return std::make_shared<RetainedObservedStateChannel>(
      binding, std::move(projector), std::move(config), &clock);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredLaneRefusesMissingOrForeignClockBeforeTransportAndBind) {
  const auto motion = commanded_binding();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  for (const bool foreign_owner : {false, true}) {
    auto config = test_factory_config(true);
    // An invalid node name makes premature ROS construction observable.
    config.node_name = "invalid/node/name";
    if (foreign_owner)
      config.measured_receiver_clock = std::make_shared<core::FakeClock>();
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    auto factory = make_retained_ros2_control_backend_factory(
        std::move(config), handle);
    try {
      // Clock ownership must refuse before the missing-channel guard too.
      static_cast<void>(factory(motion, std::nullopt, nullptr, nullptr,
                                clock, scheduler));
      FAIL() << "measured lane requires the exact owning receiver clock";
    } catch (const std::invalid_argument &error) {
      EXPECT_EQ(std::string(error.what()).substr(
                    0U, std::string(error.what()).find(':')),
                foreign_owner ? "measured_receiver_clock_mismatch"
                              : "measured_receiver_clock_missing");
    }
    EXPECT_FALSE(handle->node());
    EXPECT_FALSE(handle->participant());
    EXPECT_FALSE(handle->current_port());
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     LegacyLaneWithoutMeasuredSourceStillAcceptsBorrowedClock) {
  const auto motion = commanded_binding();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = test_factory_config(false);
  const auto motion_channel = test_projected_channel(
      motion, retained_ros2_control_motion_state_config(config), clock);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  auto factory = make_retained_ros2_control_backend_factory(
      std::move(config), handle);
  const auto backend = factory(motion, std::nullopt, motion_channel, nullptr,
                               clock, scheduler);
  ASSERT_TRUE(backend.execution);
  EXPECT_TRUE(handle->OwnsBackend(backend.execution.get(), motion, std::nullopt));
}

[[nodiscard]] RetainedRos2ControlBackendConfig measured_factory_config() {
  auto config = test_factory_config(true);
  config.participant.stop.trip_authority = core::StateAuthorityClass::kMeasured;
  config.participant.progress_freshness_budget_ns = 250'000'000;
  config.participant.start_tolerance_rad = 0.02;
  config.participant.final_tolerance_rad = 0.02;
  config.participant.completion_state_timeout_ns = 1'000'000'000;
  config.observed_state_authority_profile_id =
      std::string(kMeasuredJointStateProfileId);
  return config;
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredAuthorityComposesItsMeasuredLaneAsTheArmState) {
  const auto config = measured_factory_config();
  const auto state = retained_ros2_control_motion_state_config(config);
  EXPECT_EQ(state.source_kind, core::ObservedStateSourceKind::kJointState);
  EXPECT_EQ(state.source_id, "/manyforge/robot_1/joint_states");
  EXPECT_EQ(state.freshness_budget_ns, 250'000'000);
  ASSERT_EQ(state.source_precedence.size(), 1U);
  const auto &source = state.source_precedence.front();
  EXPECT_EQ(source.kind, core::ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(source.source_id, "/manyforge/robot_1/joint_states");
  EXPECT_EQ(source.authority_profile,
            core::ObservedRobotStateAuthorityProfile::kMeasured);
  EXPECT_EQ(source.freshness_budget_ns, 250'000'000);
  EXPECT_EQ(source.authority_profile_id, kMeasuredJointStateProfileId);
  // No measured lane, a non-positive budget, a relative topic or a foreign
  // authority profile refuse: there is no commanded fallback.
  auto missing = config;
  missing.arm.measured_joint_state_lane.reset();
  auto zero = config;
  zero.arm.measured_joint_state_lane->freshness_budget_ns = 0;
  auto relative = config;
  relative.arm.measured_joint_state_lane->absolute_topic = "joint_states";
  auto foreign = config;
  foreign.observed_state_authority_profile_id = "commanded_simulation";
  for (const auto *refused : {&missing, &zero, &relative, &foreign}) {
    EXPECT_THROW(static_cast<void>(
                     retained_ros2_control_motion_state_config(*refused)),
                 std::invalid_argument);
  }
  // Commanded composition is unchanged.
  const auto commanded =
      retained_ros2_control_motion_state_config(test_factory_config(true));
  EXPECT_EQ(commanded.source_kind, core::ObservedStateSourceKind::kCommandedState);
  EXPECT_EQ(commanded.source_id, "commanded_state");
  EXPECT_EQ(commanded.freshness_budget_ns, 0);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredArmStateIsOnlyAFreshCompleteMeasurement) {
  const auto motion = measured_binding();
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1'000'000'000);
  core::ManualScheduler scheduler;
  auto config = measured_factory_config();
  config.measured_receiver_clock = clock;
  const auto motion_channel = test_projected_channel(
      motion, retained_ros2_control_motion_state_config(config), *clock);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  auto factory =
      make_retained_ros2_control_backend_factory(std::move(config), handle);
  const auto backend = factory(motion, std::nullopt, motion_channel, nullptr,
                               *clock, scheduler);
  ASSERT_TRUE(backend.execution);
  EXPECT_EQ(motion_channel->query_context().state_authority_class,
            core::StateAuthorityClass::kMeasured);
  // A measured arm is never seeded: no state before the first measurement.
  EXPECT_FALSE(motion_channel->current_state(clock->now()).ok());

  auto publisher = handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      "/manyforge/robot_1/joint_states", rclcpp::QoS(1));
  handle->StartBackend();
  ASSERT_TRUE(wait_until([&] { return publisher->get_subscription_count() == 1U; }));
  const auto incomplete = [&] {
    const auto metrics =
        motion_channel->publication().numeric_publication().source_metrics();
    return metrics.empty() ? 0U : metrics.front().incomplete_total;
  };
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 7;
  message.name = {"unrelated_joint", "source_joint_1"};
  message.position = {9.0, 0.25};
  message.velocity = {9.0, 0.5};
  publisher->publish(message);
  ASSERT_TRUE(wait_until(
      [&] { return motion_channel->current_state(clock->now()).ok(); }));
  const auto sampled = motion_channel->current_state(clock->now());
  ASSERT_TRUE(sampled.ok());
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(sampled.value().positions.front()).value_rad,
      0.25);
  EXPECT_EQ(sampled.value().source_kind, core::ObservedStateSourceKind::kJointState);
  EXPECT_EQ(sampled.value().state_authority_class, core::StateAuthorityClass::kMeasured);
  EXPECT_EQ(sampled.value().source_timestamp_ns, 7'000'000'000);

  // Stale: one nanosecond beyond the 0.25 s budget is not ready.
  clock->set(1'000'000'000 + 250'000'000);
  EXPECT_TRUE(motion_channel->current_state(clock->now()).ok());
  clock->set(1'000'000'000 + 250'000'001);
  EXPECT_FALSE(motion_channel->current_state(clock->now()).ok());

  // A fresh NaN (or missing-joint) measurement replaces the prior complete
  // one: not ready, and never filled with zero or the previous value.
  for (const bool nan : {true, false}) {
    SCOPED_TRACE(nan);
    clock->set(clock->now() + 10'000'000);
    message.position = {9.0, 0.25};
    publisher->publish(message);
    ASSERT_TRUE(wait_until(
        [&] { return motion_channel->current_state(clock->now()).ok(); }));
    const auto before = incomplete();
    clock->set(clock->now() + 10'000'000);
    if (nan) {
      message.position = {9.0, std::numeric_limits<double>::quiet_NaN()};
    } else {
      message.name = {"unrelated_joint", "other_joint"};
    }
    publisher->publish(message);
    ASSERT_TRUE(wait_until([&] { return incomplete() > before; }));
    EXPECT_FALSE(motion_channel->current_state(clock->now()).ok());
    message.name = {"unrelated_joint", "source_joint_1"};
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     BindRefusesForeignClockOwnerBeforeParticipantConstruction) {
  core::FakeClock clock;
  const auto sink = std::make_shared<NullExecutionStateSink>();
  auto resources = make_test_transport_resources(
      "manyforge_bind_clock_fixture", sink, clock);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  bool participant_constructed = false;
  const auto bound = handle->Bind(
      std::move(resources), clock, test_arm_transport("/manyforge/robot_1"),
      [&](std::shared_ptr<core::IExecutionPort>) {
        participant_constructed = true;
        return std::shared_ptr<core::ExecutionPortParticipant>{};
      },
      nullptr, commanded_binding(), std::nullopt, std::nullopt,
      std::make_shared<core::FakeClock>());
  ASSERT_FALSE(bound.ok());
  EXPECT_EQ(bound.error().details.at("reason"),
            "ros2_control_handle_clock_owner_mismatch");
  EXPECT_FALSE(participant_constructed);
  EXPECT_FALSE(handle->node());
  EXPECT_FALSE(handle->participant());
}

TEST(RetainedRos2ControlBackendFactoryTest,
     ExactMeasuredClockOwnerSurvivesThroughExistingBackendAndLifecycleOwners) {
  // Each independent existing handle owner must preserve the borrowed clock,
  // even after the caller, factory and all other backend owners release it.
  for (const int retained_owner : {0, 1, 2}) {
    SCOPED_TRACE(retained_owner);
    const auto motion = commanded_binding();
    const auto tool = commanded_gripper_binding();
    auto clock = std::make_shared<core::FakeClock>();
    clock->set(123'000'000);
    const std::weak_ptr<core::IClock> weak_clock = clock;
    core::ManualScheduler scheduler;
    auto config = test_factory_config(true);
    config.measured_receiver_clock = clock;
    config.gripper.emplace();
    config.gripper->state_freshness_timeout_s = 1.0;
    config.gripper->action_name =
        config.node_namespace + "/gripper_controller/gripper_cmd";
    config.gripper->group_id = "gripper";
    config.gripper->joint_name = "gripper_joint";
    auto motion_channel = test_projected_channel(
        motion, retained_ros2_control_motion_state_config(config), *clock);
    auto gripper_channel = test_projected_channel(
        tool, retained_ros2_control_gripper_state_config(tool), *clock);
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak_handle = handle;
    auto factory = make_retained_ros2_control_backend_factory(
        std::move(config), handle);
    auto backend = factory(motion, tool, motion_channel, gripper_channel,
                           *clock, scheduler);
    ASSERT_TRUE(backend.execution);
    ASSERT_TRUE(backend.gripper);
    EXPECT_FALSE(backend.execution.owner_before(handle));
    EXPECT_FALSE(handle.owner_before(backend.execution));
    EXPECT_FALSE(backend.gripper.owner_before(handle));
    EXPECT_FALSE(handle.owner_before(backend.gripper));
    std::shared_ptr<core::IExecutionBackend> retained_execution;
    std::shared_ptr<core::IGripperCommandBackend> retained_gripper;
    RetainedExternallyManagedDomainSnapshotSource retained_lifecycle;
    if (retained_owner == 0)
      retained_execution = backend.execution;
    else if (retained_owner == 1)
      retained_gripper = backend.gripper;
    else
      retained_lifecycle = handle->lifecycle_snapshot_source();
    backend = {};
    factory = {};
    motion_channel.reset();
    gripper_channel.reset();
    handle.reset();
    clock.reset();
    EXPECT_FALSE(weak_handle.expired());
    ASSERT_FALSE(weak_clock.expired());
    EXPECT_EQ(weak_clock.lock()->now(), 123'000'000);
    retained_execution.reset();
    retained_gripper.reset();
    retained_lifecycle = {};
    EXPECT_TRUE(weak_handle.expired());
    EXPECT_TRUE(weak_clock.expired());
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
      RetainedRos2ControlRuntimeHandle::Create();
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

  explicit InitialRosInstallationFixture(
      bool with_gripper = true, bool component_local_gripper = false,
      const std::optional<core::SelectedControllerOccurrenceDTO>
          &successor_occurrence = std::nullopt)
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
    gripper_config.state_freshness_timeout_s = 1.0;
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
        include_gripper ? std::optional(tool) : std::nullopt,
        successor_occurrence);
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
          binding == &motion
              ? static_cast<core::IControlDomainExecutionPort *>(handle->participant().get())
              : static_cast<core::IControlDomainExecutionPort *>(gripper_engine.get());
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
     SelectedOccurrenceKeepsArmAndGripperGenerationsIndependent) {
  const core::SelectedControllerOccurrenceDTO initial{
      core::ExecutionBindingEpoch(4U),
      {{core::ControlDomainId("domain.arm"), 7U},
       {core::ControlDomainId("domain.gripper"), 13U}}};
  InitialRosInstallationFixture predecessor(true, false, initial);
  const auto predecessor_port = predecessor.handle->current_port();
  const auto owned = predecessor.handle->ReadOwnedControllerOccurrence();
  EXPECT_EQ(owned, initial);
  const auto next = core::ComputeSelectedControllerSuccessorOccurrence(
      owned,
      {core::ControlDomainId("domain.arm"),
       core::ControlDomainId("domain.gripper")},
      true);
  ASSERT_TRUE(next.ok());
  InitialRosInstallationFixture successor(true, false, next.value());
  EXPECT_EQ(successor.handle->current_port()->ReadReadiness().binding_epoch,
            core::ExecutionBindingEpoch(5U));
  EXPECT_EQ(successor.handle->ReadOwnedControllerOccurrence(), next.value());
  for (const auto &domain : next.value().domains) {
    const auto first = successor.handle->ReadLifecycleSnapshot(
        domain.control_domain_id, successor.clock.now());
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().source_generation, domain.observation_generation);
    const auto repeated = successor.handle->ReadLifecycleSnapshot(
        domain.control_domain_id, successor.clock.now());
    ASSERT_TRUE(repeated.ok());
    EXPECT_EQ(repeated.value().source_generation,
              first.value().source_generation);
  }
  EXPECT_EQ(predecessor.handle->current_port(), predecessor_port);
  EXPECT_EQ(predecessor.handle->ReadOwnedControllerOccurrence(), owned);
  EXPECT_TRUE(predecessor.handle->OwnsBackend(
      predecessor.backend, predecessor.motion, predecessor.tool));
}

TEST(RetainedRos2ControlBackendFactoryTest,
     SelectedOccurrenceUsesExactChangedGripperMembership) {
  InitialRosInstallationFixture arm_only(false);
  const auto added = core::ComputeSelectedControllerSuccessorOccurrence(
      arm_only.handle->ReadOwnedControllerOccurrence(),
      {core::ControlDomainId("domain.arm"),
       core::ControlDomainId("domain.gripper")},
      true);
  ASSERT_TRUE(added.ok());
  InitialRosInstallationFixture with_gripper(true, false, added.value());
  EXPECT_EQ(with_gripper.handle->ReadOwnedControllerOccurrence(),
            added.value());
  const auto gripper = with_gripper.handle->ReadLifecycleSnapshot(
      core::ControlDomainId("domain.gripper"), with_gripper.clock.now());
  ASSERT_TRUE(gripper.ok());
  EXPECT_EQ(gripper.value().source_generation, 1U);
  const auto removed = core::ComputeSelectedControllerSuccessorOccurrence(
      with_gripper.handle->ReadOwnedControllerOccurrence(),
      {core::ControlDomainId("domain.arm")}, true);
  ASSERT_TRUE(removed.ok());
  InitialRosInstallationFixture successor(false, false, removed.value());
  EXPECT_EQ(successor.handle->ReadOwnedControllerOccurrence(), removed.value());
  EXPECT_FALSE(
      successor.handle
          ->ReadLifecycleSnapshot(core::ControlDomainId("domain.gripper"),
                                  successor.clock.now())
          .ok());
  EXPECT_FALSE(successor.handle->OwnsBackend(successor.backend,
                                             successor.motion, successor.tool));
  EXPECT_EQ(arm_only.handle->ReadOwnedControllerOccurrence().domains.size(),
            1U);
  EXPECT_EQ(with_gripper.handle->ReadOwnedControllerOccurrence().domains.size(),
            2U);
}

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

[[nodiscard]] RetainedRos2ControlBackendConfig gripper_only_factory_config(
    const std::string &component = "robot_1") {
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_gripper_only_factory_fixture";
  config.node_namespace = "/manyforge/" + component;
  config.executor_thread_count = 1U;
  config.defer_ros_start = true;
  config.arm.gripper_controller = RetainedRos2ControlArmController{
      config.node_namespace + "/controller_manager", "gripper_controller", "TestSystem"};
  ros2_control_execution::Ros2GripperActionConfig gripper;
  gripper.state_freshness_timeout_s = 1.0;
  gripper.action_name = config.node_namespace + "/gripper_controller/gripper_cmd";
  gripper.group_id = "gripper";
  gripper.joint_name = "gripper_joint";
  config.gripper = std::move(gripper);
  config.arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
      config.node_namespace + "/joint_states", 1'000'000'000,
      core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
      std::string(kMeasuredJointStateProfileId), core::StreamSeverityDTO::kFault,
      core::StreamRecoveryDTO::kAck, std::nullopt};
  return config;
}

struct GripperOnlyInstallationFixture final {
  explicit GripperOnlyInstallationFixture(const std::string &component = "robot_1")
      : tool(commanded_gripper_binding(component)) {}
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  core::RuntimeParticipantBinding tool{commanded_gripper_binding()};
  std::shared_ptr<RetainedObservedStateChannel> channel = test_projected_channel(
      tool, retained_ros2_control_gripper_state_config(tool), clock);
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle =
      RetainedRos2ControlRuntimeHandle::Create();
  std::shared_ptr<core::IGripperCommandBackend> gripper =
      make_retained_ros2_control_gripper_backend(
          gripper_only_factory_config(tool.value().controllable_component_id.value()),
          tool, channel, clock, scheduler, handle);
  core::GripperParticipantEngine engine{*gripper, clock};
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations{
      {tool, std::nullopt, {}, core::MotionInterruptNodeCategory::kGeneric,
       &engine, &engine, std::nullopt, nullptr, std::nullopt, std::nullopt, std::nullopt}};
  const core::ContentDigest identity{std::string("sha256:") + std::string(64, 'a')};
  component_manager::ManagerSelectedRuntimeCompositionRequestDTO request{
      identity, core::ApplyId("apply.gripper-only"), core::ApplyGeneration(1U),
      {tool.value().retained_bundle_id, tool.value().graph_revision,
       {core::SceneId(identity.value()), 1U, identity}},
      core::ActiveBundlePublicationGeneration(1U), identity, identity, 1U,
      core::RuntimeUnitId("runtime.ros2_control"), core::RuntimeEpoch("runtime-epoch.1"),
      core::RuntimeEpoch("manager.1"), core::ConfigurationFenceId("fence.gripper-only"),
      core::ConfigurationFenceGeneration(1U),
      {{tool.value().controllable_component_id},
       {{core::ControlDomainId("domain.gripper"), std::nullopt,
         core::ControlDomainRevision("domain-revision")}},
       {{core::ControlBindingId("binding.gripper"), std::nullopt,
         core::ControlBindingRevision("binding-revision")}},
       {core::ResourceId("robot.gripper.1")}},
      component_manager::ManagerSelectedRuntimeCompositionPhase::kFinalize,
      {{{tool.value().controllable_component_id, deployment::ApplyComponentDisposition::kAdd,
         std::nullopt, identity, {}, {core::ControlDomainId("domain.gripper")}, {}}}, false, {}},
      {core::RetainedBundleId("empty.bundle"), core::GraphRevision("empty.graph"),
       {core::SceneId(identity.value()), 1U, identity}}};

  component_manager::ManagerRuntimeComponentReceiptDTO receipt(
      const RetainedRos2ControlRuntimeHandle *predecessor = nullptr,
      const std::optional<std::string> &process_exit = std::nullopt) const {
    return detail::ReadRos2ComponentReceipt(
        request, tool.value().controllable_component_id, identity, *handle,
        nullptr, gripper.get(), std::nullopt, tool, registrations, nullptr,
        &engine, predecessor, process_exit);
  }
};

TEST(RetainedRos2ControlBackendFactoryTest,
     GripperOnlyInstallReceiptProvesItsActualCommandAndStopOwner) {
  GripperOnlyInstallationFixture fixture;
  ASSERT_TRUE(fixture.request.valid());
  EXPECT_FALSE(fixture.handle->participant());
  EXPECT_FALSE(fixture.handle->current_port());
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper.get(), fixture.tool));
  EXPECT_FALSE(fixture.handle->ReadOwnedControllerOccurrence().arm_binding_epoch);
  ASSERT_EQ(fixture.registrations.size(), 1U);
  EXPECT_EQ(fixture.registrations.front().engine, &fixture.engine);
  EXPECT_EQ(fixture.registrations.front().stop_execution_port,
            static_cast<core::IControlDomainExecutionPort *>(&fixture.engine));
  const auto first = fixture.receipt();
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request, {first}));
  EXPECT_EQ(fixture.receipt(), first);
  EXPECT_FALSE(first.predecessor_runtime_retained);
  EXPECT_FALSE(first.product_binding_resealed);
  auto &row = fixture.request.component_delta.components.front();
  row.disposition = deployment::ApplyComponentDisposition::kRetain;
  row.predecessor_runtime_identity = row.candidate_runtime_identity;
  row.predecessor_domains = row.candidate_domains;
  fixture.request.configuration_fence_scope = {};
  const auto retained = fixture.receipt(fixture.handle.get());
  EXPECT_TRUE(retained.predecessor_runtime_retained);
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request, {retained}));
  EXPECT_THROW(static_cast<void>(fixture.receipt(fixture.handle.get(),
                   "controller launch exited with status 17")), std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     GripperOnlyReceiptRejectsForeignAdapterEngineStopPortAndProduct) {
  GripperOnlyInstallationFixture fixture;
  GripperOnlyInstallationFixture foreign;
  const auto original = fixture.request;
  const auto read = [&](const core::IGripperCommandBackend *gripper,
                        const core::IResolvedParticipantExecutionEngine *engine,
                        const RetainedRos2ControlRuntimeHandle *predecessor = nullptr) {
    return detail::ReadRos2ComponentReceipt(
        fixture.request, fixture.tool.value().controllable_component_id,
        fixture.identity, *fixture.handle, nullptr, gripper, std::nullopt,
        fixture.tool, fixture.registrations, nullptr, engine, predecessor);
  };
  EXPECT_THROW(static_cast<void>(read(foreign.gripper.get(), &fixture.engine)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(read(fixture.gripper.get(), &foreign.engine)),
               std::invalid_argument);
  fixture.registrations.front().stop_execution_port = &foreign.engine;
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.registrations.front().stop_execution_port = &fixture.engine;
  fixture.registrations.push_back(fixture.registrations.front());
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.registrations.pop_back();
  fixture.request.runtime_epoch = core::RuntimeEpoch("foreign.epoch");
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  fixture.request.candidate_projection_revision.graph_revision = core::GraphRevision("foreign.graph");
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  fixture.request.configuration_fence_scope.resources.clear();
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);
  fixture.request = original;
  auto &row = fixture.request.component_delta.components.front();
  row.disposition = deployment::ApplyComponentDisposition::kRetain;
  row.predecessor_runtime_identity = row.candidate_runtime_identity;
  row.predecessor_domains = row.candidate_domains;
  // Only the actual retained owner may reuse resources outside a fresh-install fence.
  fixture.request.configuration_fence_scope = {};
  ASSERT_TRUE(fixture.request.valid());
  EXPECT_THROW(static_cast<void>(read(fixture.gripper.get(), &fixture.engine, nullptr)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(read(fixture.gripper.get(), &fixture.engine, foreign.handle.get())),
               std::invalid_argument);
  EXPECT_TRUE(fixture.handle->OwnsGripper(fixture.gripper.get(), fixture.tool));
  EXPECT_FALSE(fixture.handle->participant());
}

TEST(RetainedRos2ControlBackendFactoryTest,
     GripperOnlyFactoryRefusesForeignClockChannelAndMotionOccurrence) {
  const auto binding = commanded_gripper_binding();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto channel = test_projected_channel(
      binding, retained_ros2_control_gripper_state_config(binding), clock);
  const auto foreign = commanded_gripper_binding("robot_b");
  const auto foreign_channel = test_projected_channel(
      foreign, retained_ros2_control_gripper_state_config(foreign), clock);
  for (const int invalid : {0, 1, 2, 3}) {
    SCOPED_TRACE(invalid);
    auto config = gripper_only_factory_config();
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    if (invalid == 0) config.measured_receiver_clock = std::make_shared<core::FakeClock>();
    if (invalid == 2)
      config.successor_occurrence = core::SelectedControllerOccurrenceDTO{
          core::ExecutionBindingEpoch(2U), {{core::ControlDomainId("domain.gripper"), 2U}}};
    if (invalid == 3)
      config.successor_occurrence = core::SelectedControllerOccurrenceDTO{
          std::nullopt, {{core::ControlDomainId("foreign.domain"), 2U}}};
    EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                     config, binding, invalid == 1 ? foreign_channel : channel,
                     clock, scheduler, handle)), std::invalid_argument);
    EXPECT_FALSE(handle->node());
    EXPECT_FALSE(handle->participant());
    EXPECT_FALSE(handle->current_port());
    EXPECT_TRUE(handle->ReadOwnedControllerOccurrence().domains.empty());
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     GripperOnlyResealPreservesOriginalOwnerAcrossPeerReplacement) {
  GripperOnlyInstallationFixture survivor;
  auto peer = std::make_unique<GripperOnlyInstallationFixture>("robot_b");
  const auto owner = survivor.handle;
  const auto *backend = survivor.gripper.get();
  const auto occurrence = owner->ReadOwnedControllerOccurrence();
  const auto *engine = survivor.registrations.front().engine;
  const auto *stop_port = survivor.registrations.front().stop_execution_port;
  auto retired = std::move(peer);
  peer = std::make_unique<GripperOnlyInstallationFixture>("robot_b");
  EXPECT_NE(peer->handle, retired->handle);
  EXPECT_NE(peer->gripper.get(), retired->gripper.get());
  retired.reset();
  auto value = survivor.tool.value();
  value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  value.graph_revision = core::GraphRevision("candidate.graph");
  value.control_path->active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(2U);
  auto candidate = core::verify_runtime_participant_binding(value);
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  const auto channel = test_projected_channel(
      candidate.value(), retained_ros2_control_gripper_state_config(candidate.value()), survivor.clock);
  ASSERT_TRUE(owner->RebindGripperRoute(candidate.value(), channel).ok());
  EXPECT_TRUE(owner->OwnsGripper(backend, candidate.value()));
  EXPECT_FALSE(owner->OwnsGripper(backend, survivor.tool));
  EXPECT_EQ(owner->ReadOwnedControllerOccurrence(), occurrence);
  peer.reset();
  EXPECT_EQ(survivor.handle, owner);
  EXPECT_EQ(survivor.gripper.get(), backend);
  EXPECT_EQ(survivor.registrations.front().engine, engine);
  EXPECT_EQ(survivor.registrations.front().stop_execution_port, stop_port);
  EXPECT_FALSE(owner->participant());
  EXPECT_FALSE(owner->current_port());
  const std::vector<std::function<void(core::RuntimeParticipantBindingDTO &)>> mutations{
      [](auto &v) { v.control_path->provider_instance_id = core::ProviderInstanceId("foreign.provider"); },
      [](auto &v) { v.control_path->control_binding_id = core::ControlBindingId("foreign.binding"); },
      [](auto &v) { v.resources.front().resource_id = core::ResourceId("foreign.resource"); },
      [](auto &v) { v.control_path->provider_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
                   v.runtime_unit_epochs.front().runtime_epoch = core::RuntimeEpoch("foreign.epoch"); }};
  for (std::size_t i = 0; i < mutations.size(); ++i) {
    SCOPED_TRACE(i);
    auto foreign_value = candidate.value().value();
    mutations[i](foreign_value);
    auto foreign = core::verify_runtime_participant_binding(std::move(foreign_value));
    ASSERT_TRUE(foreign.ok()) << foreign.error().message;
    const auto foreign_channel = test_projected_channel(
        foreign.value(), retained_ros2_control_gripper_state_config(foreign.value()), survivor.clock);
    EXPECT_FALSE(owner->RebindGripperRoute(foreign.value(), foreign_channel).ok());
    EXPECT_TRUE(owner->OwnsGripper(backend, candidate.value()));
    EXPECT_EQ(owner->ReadOwnedControllerOccurrence(), occurrence);
  }
  ASSERT_TRUE(owner->RebindGripperRoute(survivor.tool, survivor.channel).ok());
  EXPECT_TRUE(owner->OwnsGripper(backend, survivor.tool));
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RetainingOnlyTheGripperOnlyBackendRetainsItsActualTransportAndClock) {
  auto clock = std::make_shared<core::FakeClock>();
  core::ManualScheduler scheduler;
  const auto binding = commanded_gripper_binding();
  auto channel = test_projected_channel(
      binding, retained_ros2_control_gripper_state_config(binding), *clock);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  auto config = gripper_only_factory_config();
  config.measured_receiver_clock = clock;
  auto backend = make_retained_ros2_control_gripper_backend(
      config, binding, channel, *clock, scheduler, handle);
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner = handle;
  const std::weak_ptr<core::IClock> clock_lifetime = clock;
  config.measured_receiver_clock.reset();
  clock.reset();
  handle.reset();
  channel.reset();
  EXPECT_FALSE(owner.expired());
  EXPECT_FALSE(clock_lifetime.expired());
  EXPECT_EQ(backend->get_gripper_status(binding.value().command_target_id.value()).state,
            core::GripperCommandState::kIdle);
  backend.reset();
  EXPECT_TRUE(owner.expired());
  EXPECT_TRUE(clock_lifetime.expired());
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
     InitialInstallReceiptRequiresCurrentGenerationForBothOwnedAdapters) {
  InitialRosInstallationFixture fixture;
  ASSERT_TRUE(fixture.request.valid());
  const auto original_motion = fixture.motion;
  const auto original_tool = fixture.tool;
  const auto owner = fixture.handle;
  const auto *const execution = fixture.backend;
  const auto *const gripper = fixture.gripper;
  const auto participant = owner->participant();
  const auto port = owner->current_port();
  const auto occurrence = owner->ReadOwnedControllerOccurrence();
  const auto original_registrations = fixture.registrations;
  ASSERT_EQ(original_registrations.size(), 2U);
  EXPECT_NO_THROW(static_cast<void>(fixture.receipt()));

  // Route adoption changes the accepted configuration generation while these
  // same adapter objects and their physical bindings remain installed.
  auto motion_value = original_motion.value();
  motion_value.control_path->active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(2U);
  auto tool_value = original_tool.value();
  tool_value.control_path->active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(2U);
  auto current_motion =
      core::verify_runtime_participant_binding(std::move(motion_value));
  auto current_tool =
      core::verify_runtime_participant_binding(std::move(tool_value));
  ASSERT_TRUE(current_motion.ok()) << current_motion.error().message;
  ASSERT_TRUE(current_tool.ok()) << current_tool.error().message;
  const auto channel = test_projected_channel(
      current_tool.value(),
      retained_ros2_control_gripper_state_config(current_tool.value()),
      fixture.clock);
  ASSERT_TRUE(owner->RebindMotionRoute(current_motion.value(), {}).ok());
  ASSERT_TRUE(owner->RebindGripperRoute(current_tool.value(), channel).ok());
  EXPECT_FALSE(owner->OwnsBackend(execution, original_motion, original_tool));
  EXPECT_FALSE(owner->OwnsGripper(gripper, original_tool));
  EXPECT_TRUE(owner->OwnsBackend(execution, current_motion.value(),
                                 current_tool.value()));
  EXPECT_TRUE(owner->OwnsGripper(gripper, current_tool.value()));
  EXPECT_THROW(static_cast<void>(fixture.receipt()), std::invalid_argument);

  // Current bindings do not make the still-generation-1 registrations current.
  EXPECT_THROW(
      static_cast<void>(detail::ReadRos2ComponentReceipt(
          fixture.request, original_motion.value().controllable_component_id,
          fixture.identity, *owner, execution, gripper, current_motion.value(),
          current_tool.value(), fixture.registrations, fixture.motion_engine.get(),
          fixture.gripper_engine.get(), nullptr)),
      std::invalid_argument);

  // The registrations retain their original owned engines and Stop ports;
  // only the accepted binding generation follows the same route adoption.
  for (auto &registration : fixture.registrations) {
    if (registration.binding.value() == original_motion.value())
      registration.binding = current_motion.value();
    else {
      ASSERT_EQ(registration.binding.value(), original_tool.value());
      registration.binding = current_tool.value();
    }
  }
  const auto read = [&](const core::RuntimeParticipantBinding &motion,
                        const core::RuntimeParticipantBinding &tool) {
    return detail::ReadRos2ComponentReceipt(
        fixture.request, motion.value().controllable_component_id,
        fixture.identity, *owner, execution, gripper, motion, tool,
        fixture.registrations, fixture.motion_engine.get(),
        fixture.gripper_engine.get(), nullptr);
  };
  EXPECT_THROW(static_cast<void>(read(original_motion, original_tool)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(read(current_motion.value(), original_tool)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(read(original_motion, current_tool.value())),
               std::invalid_argument);
  const auto receipt = read(current_motion.value(), current_tool.value());
  EXPECT_TRUE(receipt.valid());
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(fixture.request,
                                                               {receipt}));
  EXPECT_FALSE(receipt.predecessor_runtime_retained);
  EXPECT_FALSE(receipt.product_binding_resealed);
  EXPECT_EQ(fixture.handle, owner);
  EXPECT_EQ(fixture.backend, execution);
  EXPECT_EQ(fixture.gripper, gripper);
  EXPECT_EQ(owner->participant(), participant);
  EXPECT_EQ(owner->current_port(), port);
  EXPECT_EQ(owner->ReadOwnedControllerOccurrence(), occurrence);
  ASSERT_EQ(fixture.registrations.size(), original_registrations.size());
  for (std::size_t index = 0U; index < fixture.registrations.size(); ++index) {
    EXPECT_EQ(fixture.registrations[index].engine,
              original_registrations[index].engine);
    EXPECT_EQ(fixture.registrations[index].stop_execution_port,
              original_registrations[index].stop_execution_port);
  }
  // This unit proves the exact receipt invariant. The ordinary Manager Apply
  // sequence separately exercises the production caller's current-pair choice.
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
  fixture.request.configuration_fence_scope = {};
  ASSERT_TRUE(fixture.request.valid());
  EXPECT_THROW(static_cast<void>(read(nullptr)), std::invalid_argument);
  InitialRosInstallationFixture foreign;
  EXPECT_THROW(static_cast<void>(read(foreign.handle.get())),
               std::invalid_argument);
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
  const auto actual_retained = read(fixture.handle.get());
  EXPECT_TRUE(actual_retained.predecessor_runtime_retained);
  EXPECT_EQ(actual_retained.disposition,
            deployment::ApplyComponentDisposition::kReplace);
  EXPECT_TRUE(actual_retained.product_binding_resealed);
  EXPECT_TRUE(component_manager::ComponentReceiptsMatchRequest(
      fixture.request, {actual_retained}));
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
     IsaacControllerDescriptionUsesSelectedBackendCommandPose) {
  // The maintained deployment defaults remain equivalent while the initial
  // pose comes from the selected deployment rather than profile constants.
  const std::vector<double> lite6_defaults{0.0, 0.0, 1.57, 0.0, 0.0, 0.0};
  const auto lite6 = retained_isaac_controller_lane("lite6", lite6_defaults);
  EXPECT_EQ(lite6.hardware_component_name, "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(lite6.joint_commands_topic, "/isaac_joint_commands");
  EXPECT_EQ(lite6.joint_states_topic, "/isaac_joint_states");
  EXPECT_EQ(lite6.initial_joint_positions_rad, lite6_defaults);
  const std::vector<double> ur10e_defaults{
      2.63422544, -0.875980752, 1.551074106, -2.29231544, -1.560498884,
      -2.038544566};
  const auto ur10e = retained_isaac_controller_lane("ur10e", ur10e_defaults);
  EXPECT_EQ(ur10e.hardware_component_name, "IsaacUR10eTopicBasedSystem");
  EXPECT_EQ(ur10e.joint_commands_topic, "/isaac_joint_commands");
  EXPECT_EQ(ur10e.joint_states_topic, "/isaac_joint_states");
  EXPECT_EQ(ur10e.initial_joint_positions_rad, ur10e_defaults);
  EXPECT_THROW(
      static_cast<void>(retained_isaac_controller_lane("ur5e", lite6_defaults)),
      std::invalid_argument);

  // One non-default selected pose reaches both the backend-commanded authority
  // and TopicBasedSystem's generated command/state-interface seed.
  const std::vector<double> selected_pose{0.375};
  const auto lane = retained_isaac_controller_lane("lite6", selected_pose);
  const std::string model =
      "<robot name=\"r\"><link name=\"a\"/><link name=\"b\"/>"
      "<joint name=\"joint1\" type=\"revolute\"><parent link=\"a\"/>"
      "<child link=\"b\"/></joint></robot>";
  const auto generated = generate_controller_robot_description(
      ControllerRobotDescriptionRequest{model, {"joint1"}, lane});
  EXPECT_NE(generated.find("<ros2_control name=\"IsaacLite6TopicBasedSystem\" "
                           "type=\"system\">"),
            std::string::npos);
  EXPECT_NE(generated.find("<plugin>topic_based_ros2_control/TopicBasedSystem"
                           "</plugin>"),
            std::string::npos);
  EXPECT_NE(generated.find("<param name=\"initial_value\">0.375</param>"),
            std::string::npos);

  core::FakeClock clock;
  const auto binding = commanded_binding();
  auto channel = test_projected_channel(
      binding,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kCommandedState, "commanded_state", 0,
          1U},
      clock);
  ASSERT_TRUE(channel->publish_commanded_initialization_and_project(
                         selected_pose, std::vector<double>{0.0}, 200)
                  .applied());
  const auto authoritative = channel->current_state(200);
  ASSERT_TRUE(authoritative.ok()) << authoritative.error().message;
  ASSERT_EQ(authoritative.value().positions.size(), 1U);
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(authoritative.value().positions.front())
          .value_rad,
      selected_pose.front());

  const std::vector<double> missing_pose;
  const auto missing = retained_isaac_controller_lane("lite6", missing_pose);
  EXPECT_THROW(static_cast<void>(generate_controller_robot_description(
                   ControllerRobotDescriptionRequest{model, {"joint1"}, missing})),
               ControllerRobotDescriptionError);
}

} // namespace
TEST(RetainedRos2ControlBackendFactoryTest,
     EachComponentUsesItsOwnControllerProfileInTheOneRosUnit) {
  // Q68: a mixed cell (Lite 6 + UR10e, each with its tool) hosts every
  // component in one ROS unit; each one selects its own controller profile.
  deployment::ros2_control_execution_runtime_contract::Configuration runtime;
  runtime.gripper_target_configurations = {
      fixture_gripper_row("tool_a", "pneumatic"),
      fixture_gripper_row("tool_b", "position_commanded")};
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
       std::string("lite6_lite_gripper")},
      {core::ControllableComponentId("tool_a"), std::string("lite_gripper"),
       std::nullopt},
      {core::ControllableComponentId("tool_b"), std::string("robotiq_2f_140"),
       std::nullopt}};
  add_component_streams(runtime);
  const auto lite6 = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_a"), std::nullopt);
  const auto ur10e = make_retained_isaac_ros2_control_backend_config(
      runtime, commanded_binding("robot_b"), std::nullopt);
  const auto lite_gripper =
      isaac_tool_config(runtime, commanded_gripper_binding("tool_a"));
  const auto robotiq = isaac_tool_config(runtime, commanded_gripper_binding("tool_b"));
  EXPECT_EQ(lite6.arm.controller.hardware_component_name, "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(ur10e.arm.controller.hardware_component_name, "IsaacUR10eTopicBasedSystem");
  EXPECT_EQ(lite_gripper.gripper->completion_policy, "output_delivery");
  EXPECT_EQ(robotiq.gripper->completion_policy, "position_reached");
  // The feedback staleness threshold is the component's `gripper_feedback`
  // stream row, not a code default.
  EXPECT_DOUBLE_EQ(lite_gripper.gripper->state_freshness_timeout_s, 0.75);
  EXPECT_DOUBLE_EQ(robotiq.gripper->state_freshness_timeout_s, 0.75);
  // An arm-only robot row carries no gripper: its tool is its own component.
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   runtime, commanded_binding("robot_a"),
                   commanded_gripper_binding("robot_a"))),
               std::invalid_argument);
  // The offline mock lane is built by its own builder (phase 2b); an unknown
  // component has no profile at all.
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   runtime, commanded_binding("robot_c"), std::nullopt)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                   runtime, commanded_binding("robot_d"), std::nullopt)),
               std::invalid_argument);
}

namespace {

[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration
mock_runtime_configuration(const std::string &mock_profile) {
  deployment::ros2_control_execution_runtime_contract::Configuration result;
  result.controller_profiles = {
      {core::ControllableComponentId("robot_1"), std::nullopt, mock_profile}};
  if (mock_profile != "ur5_roboplan")
    result.gripper_target_configurations.push_back(fixture_gripper_row(
        "robot_1", mock_profile == "lite6_lite_gripper" ? "pneumatic"
                                                       : "position_commanded"));
  add_component_streams(result);
  result.planner_rng_seed = 12345U;
  result.initial_joint_positions_rad = {0.0};
  result.execution_tick_hz = 100U;
  result.collision_monitor_period_ms = 50.0;
  return result;
}

const std::map<std::string, double, std::less<>> kMockCancel{
    {"source_joint_1", 12.5}};

[[nodiscard]] core::ActiveControlConfigurationDTO
reconciled_configuration(const core::RuntimeParticipantBinding &binding) {
  const auto &path = *binding.value().control_path;
  return {path.active_control_configuration_id,
          path.control_domain_id,
          path.control_domain_revision,
          {{path.control_binding_id, path.control_binding_revision,
            path.control_route_id}},
          path.activation_set_id,
          path.controller_topology_id,
          path.topology_revision,
          path.provider_runtime_unit_id,
          path.provider_runtime_epoch,
          1U,
          {},
          path.active_control_configuration_generation,
          core::ActiveControlConfigurationStateDTO::kReconciled};
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredLaneUsesSelectedProfileTopicAndExistingPositiveBudget) {
  // CP9: budget/severity/recovery of the measured lane and the gripper
  // feedback stream come from the component's `component_streams` rows.
  const auto motion = commanded_binding();
  const auto tool = commanded_gripper_binding();
  const auto set_rows = [](auto &runtime, std::int64_t measured_budget,
                           core::StreamSeverityDTO severity,
                           core::StreamRecoveryDTO recovery) {
    for (auto &row : runtime.component_streams) {
      if (row.stream_id == "measured_joints") row.budget_ns = measured_budget;
      else row.budget_ns = 500'000'000;
      row.severity = severity;
      row.recovery = recovery;
    }
  };
  // Merge 6: on Isaac the robot row is arm-only and the tool row (its own
  // component) declares the gripper feedback.
  auto isaac_runtime = isaac_runtime_configuration("lite6");
  set_rows(isaac_runtime, 300'000'000, core::StreamSeverityDTO::kWarning,
           core::StreamRecoveryDTO::kAuto);
  auto isaac_tool_runtime = isaac_runtime_configuration("lite_gripper");
  set_rows(isaac_tool_runtime, 300'000'000, core::StreamSeverityDTO::kWarning,
           core::StreamRecoveryDTO::kAuto);
  const auto isaac =
      make_retained_isaac_ros2_control_backend_config(isaac_runtime, motion, std::nullopt);
  const auto isaac_tool = isaac_tool_config(isaac_tool_runtime, tool);
  ASSERT_TRUE(isaac.arm.measured_joint_state_lane);
  EXPECT_EQ(isaac.arm.measured_joint_state_lane->absolute_topic,
            "/isaac_joint_states");
  EXPECT_EQ(isaac.arm.measured_joint_state_lane->freshness_budget_ns, 300'000'000);
  EXPECT_EQ(isaac.arm.measured_joint_state_lane->stream_severity,
            core::StreamSeverityDTO::kWarning);
  EXPECT_EQ(isaac.arm.measured_joint_state_lane->stream_recovery,
            core::StreamRecoveryDTO::kAuto);
  // One value: the in-motion Stop staleness check is the measured_joints
  // row budget.
  EXPECT_EQ(isaac.participant.stop.controller_state_max_age_ns, 300'000'000);
  ASSERT_TRUE(isaac_tool.gripper);
  EXPECT_DOUBLE_EQ(isaac_tool.gripper->state_freshness_timeout_s, 0.5);
  EXPECT_EQ(isaac_tool.gripper->feedback_stream_severity,
            core::StreamSeverityDTO::kWarning);
  EXPECT_EQ(isaac_tool.gripper->feedback_stream_recovery,
            core::StreamRecoveryDTO::kAuto);
  auto mock_runtime = mock_runtime_configuration("lite6_lite_gripper");
  set_rows(mock_runtime, 400'000'000, core::StreamSeverityDTO::kBlocking,
           core::StreamRecoveryDTO::kAck);
  const auto mock = make_retained_mock_ros2_control_backend_config(
      mock_runtime, motion, tool, kMockCancel);
  ASSERT_TRUE(mock.arm.measured_joint_state_lane);
  EXPECT_EQ(mock.arm.measured_joint_state_lane->absolute_topic,
            mock.node_namespace + "/joint_states");
  EXPECT_EQ(mock.arm.measured_joint_state_lane->freshness_budget_ns, 400'000'000);
  EXPECT_EQ(mock.arm.measured_joint_state_lane->stream_severity,
            core::StreamSeverityDTO::kBlocking);
  EXPECT_EQ(mock.arm.measured_joint_state_lane->stream_recovery,
            core::StreamRecoveryDTO::kAck);
  EXPECT_EQ(mock.participant.stop.controller_state_max_age_ns, 400'000'000);
  ASSERT_TRUE(mock.gripper);
  EXPECT_DOUBLE_EQ(mock.gripper->state_freshness_timeout_s, 0.5);
  EXPECT_EQ(mock.gripper->feedback_stream_severity,
            core::StreamSeverityDTO::kBlocking);
  for (const auto *lane : {&*isaac.arm.measured_joint_state_lane,
                           &*mock.arm.measured_joint_state_lane}) {
    EXPECT_EQ(lane->measured_state_authority_id.value(),
              kMeasuredJointStateAuthorityId);
    EXPECT_EQ(lane->measured_source_profile_id, kMeasuredJointStateProfileId);
  }
  // A component that does not declare a required stream is refused. On
  // Isaac the robot declares `measured_joints` and its tool `gripper_feedback`.
  for (const std::string stream : {"measured_joints", "gripper_feedback"}) {
    for (bool isaac_lane : {false, true}) {
      auto runtime =
          !isaac_lane ? mock_runtime_configuration("lite6_lite_gripper")
          : stream == "measured_joints" ? isaac_runtime_configuration("lite6")
                                        : isaac_runtime_configuration("lite_gripper");
      std::erase_if(runtime.component_streams, [&](const auto &row) {
        return row.controllable_component_id == motion.value().controllable_component_id &&
               row.stream_id == stream;
      });
      EXPECT_THROW(static_cast<void>(
                       !isaac_lane
                           ? make_retained_mock_ros2_control_backend_config(
                                 runtime, motion, tool, kMockCancel)
                       : stream == "measured_joints"
                           ? make_retained_isaac_ros2_control_backend_config(
                                 runtime, motion, std::nullopt)
                           : isaac_tool_config(runtime, tool)),
                   std::invalid_argument);
    }
  }
  // The separate measured role never changes the execution/Stop authority.
  EXPECT_EQ(mock.participant.stop.trip_authority,
            core::StateAuthorityClass::kCommanded);
  EXPECT_EQ(isaac.participant.stop.trip_authority,
            core::StateAuthorityClass::kCommanded);
  EXPECT_EQ(retained_ros2_control_motion_state_config(mock).freshness_budget_ns,
            0);
  EXPECT_EQ(
      retained_ros2_control_motion_state_config(isaac).freshness_budget_ns, 0);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     InMotionStalenessCheckIsTheEffectiveMeasuredJointsBudget) {
  // CP9: a Deployment `measured_joints` budget override of this component is
  // the in-motion check; the declared lane keeps the row (the registry applies
  // the override on top). Other components', other streams' and budget-less
  // overrides do not change it.
  const auto motion = commanded_binding();
  const auto tool = commanded_gripper_binding();
  const auto &component = motion.value().controllable_component_id;
  const std::vector<core::StreamParameterOverrideDTO> ignored{
      {core::ControllableComponentId("other_component"), "measured_joints",
       std::int64_t{90'000'000}, std::nullopt, std::nullopt},
      {component, "gripper_feedback", std::int64_t{80'000'000}, std::nullopt,
       std::nullopt},
      {component, "measured_joints", std::nullopt,
       core::StreamSeverityDTO::kBlocking, std::nullopt}};
  auto overrides = ignored;
  overrides.push_back({component, "measured_joints", std::int64_t{700'000'000},
                       std::nullopt, std::nullopt});
  const auto isaac_runtime = isaac_runtime_configuration("lite6");
  const auto mock_runtime = mock_runtime_configuration("lite6_lite_gripper");
  for (const bool isaac_lane : {false, true}) {
    const auto build = [&](const std::vector<core::StreamParameterOverrideDTO> &rows) {
      return isaac_lane
                 ? make_retained_isaac_ros2_control_backend_config(
                       isaac_runtime, motion, std::nullopt, rows)
                 : make_retained_mock_ros2_control_backend_config(
                       mock_runtime, motion, tool, kMockCancel, rows);
    };
    const auto unchanged = build(ignored);
    EXPECT_EQ(unchanged.participant.stop.controller_state_max_age_ns, 250'000'000);
    const auto overridden = build(overrides);
    EXPECT_EQ(overridden.participant.stop.controller_state_max_age_ns, 700'000'000);
    ASSERT_TRUE(overridden.arm.measured_joint_state_lane);
    EXPECT_EQ(overridden.arm.measured_joint_state_lane->freshness_budget_ns,
              250'000'000);
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredDescriptorUsesExactPhysicalTupleAndReconciledConfiguration) {
  const auto motion = commanded_binding();
  const auto tool = commanded_gripper_binding();
  const auto config = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("lite6_lite_gripper"), motion, tool,
      kMockCancel);
  ASSERT_TRUE(config.arm.measured_joint_state_lane);
  for (const auto *binding : {&motion, &tool}) {
    const auto sealed_before = binding->value();
    const auto configuration = reconciled_configuration(*binding);
    const auto constructed =
        make_retained_ros2_control_measured_state_descriptor(
            *binding, configuration, *config.arm.measured_joint_state_lane);
    ASSERT_TRUE(constructed.ok()) << constructed.error().message;
    EXPECT_TRUE(constructed.value().validate().ok());
    auto expected = core::make_observed_state_query_context(
        *binding, config.participant.stop.controller_state_max_age_ns);
    ASSERT_TRUE(expected.ok());
    expected.value().state_authority_id =
        config.arm.measured_joint_state_lane->measured_state_authority_id;
    expected.value().state_authority_class =
        core::StateAuthorityClass::kMeasured;
    EXPECT_EQ(constructed.value().publication.query_context, expected.value());
    EXPECT_EQ(constructed.value().publication.authority_generation,
              configuration.active_control_configuration_generation.value());
    ASSERT_EQ(constructed.value().publication.source_precedence.size(), 1U);
    const auto &source =
        constructed.value().publication.source_precedence.front();
    EXPECT_EQ(source.kind, core::ObservedRobotStateSourceKind::kJointState);
    EXPECT_EQ(source.source_id,
              config.arm.measured_joint_state_lane->absolute_topic);
    EXPECT_EQ(source.freshness_budget_ns,
              config.participant.stop.controller_state_max_age_ns);
    EXPECT_EQ(source.authority_profile,
              core::ObservedRobotStateAuthorityProfile::kMeasured);
    EXPECT_EQ(source.authority_profile_id, kMeasuredJointStateProfileId);
    std::vector<std::string> names;
    for (const auto &resource : binding->value().resources)
      names.push_back(*resource.source_joint_name);
    EXPECT_EQ(constructed.value().source_joint_names, names);
    EXPECT_EQ(binding->value(), sealed_before);
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MeasuredDescriptorRejectsForeignRouteOrUnqualifiedLane) {
  const auto binding = commanded_binding();
  const auto config = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("lite6_lite_gripper"), binding,
      commanded_gripper_binding(), kMockCancel);
  const auto good = reconciled_configuration(binding);
  const auto lane = *config.arm.measured_joint_state_lane;
  auto foreign = good;
  foreign.source_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, foreign, lane)
                   .ok());
  foreign = good;
  foreign.active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(
          good.active_control_configuration_generation.value() + 1U);
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, foreign, lane)
                   .ok());
  foreign = good;
  foreign.binding_routes.front().control_binding_revision =
      core::ControlBindingRevision("foreign.revision");
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, foreign, lane)
                   .ok());
  foreign = good;
  foreign.state =
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, foreign, lane)
                   .ok());
  auto unqualified = lane;
  unqualified.freshness_budget_ns = 0;
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, good, unqualified)
                   .ok());
  unqualified = lane;
  unqualified.measured_state_authority_id =
      core::StateAuthorityId("state.commanded");
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, good, unqualified)
                   .ok());
  unqualified = lane;
  unqualified.measured_source_profile_id = "manyforge.commanded_simulation";
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, good, unqualified)
                   .ok());
  unqualified = lane;
  unqualified.absolute_topic = "relative/joint_states";
  EXPECT_FALSE(make_retained_ros2_control_measured_state_descriptor(
                   binding, good, unqualified)
                   .ok());
  auto missing_mapping = binding.value();
  missing_mapping.resources.front().source_joint_name.reset();
  auto sealed_missing =
      core::verify_runtime_participant_binding(std::move(missing_mapping));
  ASSERT_TRUE(sealed_missing.ok());
  const auto missing = make_retained_ros2_control_measured_state_descriptor(
      sealed_missing.value(), good, lane);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"),
            "measured_source_coordinate_mapping_invalid");
}

class ReplacementAllowanceEnvironment {
public:
  static constexpr const char *name =
      "MANYFORGE_EXPERIMENTAL_REPLACEMENT_START_ALLOWANCE_MS";
  explicit ReplacementAllowanceEnvironment(const char *variable = name)
      : variable_(variable) {
    if (const auto *value = std::getenv(variable_)) previous_ = value;
    set(nullptr);
  }
  ~ReplacementAllowanceEnvironment() {
    set(previous_ ? previous_->c_str() : nullptr);
  }
  void set(const char *value) {
    if (value) static_cast<void>(::setenv(variable_, value, 1));
    else static_cast<void>(::unsetenv(variable_));
  }
private:
  const char *variable_;
  std::optional<std::string> previous_;
};

TEST(RetainedRos2ControlBackendFactoryTest,
     MockReplacementAllowanceDefaultsOverridesAndLogsWithoutTimingFlags) {
  ReplacementAllowanceEnvironment env;
  ReplacementAllowanceEnvironment release_timer("MANYFORGE_EXPERIMENTAL_RELEASE_TIMER");
  ReplacementAllowanceEnvironment midtick_stamp("MANYFORGE_EXPERIMENTAL_MIDTICK_STAMP");
  ReplacementAllowanceEnvironment v0_payload("MANYFORGE_EXPERIMENTAL_V0_PAYLOAD");
  ReplacementAllowanceEnvironment scheduled_spring_back(
      "MANYFORGE_EXPERIMENTAL_SCHEDULED_SPRING_BACK_FAST");
  const auto build = [] {
    return make_retained_mock_ros2_control_backend_config(
        mock_runtime_configuration("lite6_lite_gripper"), commanded_binding(),
        commanded_gripper_binding(), kMockCancel);
  };
  testing::internal::CaptureStderr();
  const auto baseline = build();
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_DOUBLE_EQ(*baseline.participant.stop.replacement_start_allowance_s, 0.020);
  EXPECT_FALSE(baseline.arm.adapter.lane_facts.experiment.any());
  for (const char *value : {"20", "30", "40", "200"}) {
    SCOPED_TRACE(value);
    env.set(value);
    testing::internal::CaptureStderr();
    const auto configured = build();
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_DOUBLE_EQ(*configured.participant.stop.replacement_start_allowance_s,
                     std::stod(value) / 1000.0);
    const auto &flags = configured.arm.adapter.lane_facts.experiment;
    EXPECT_TRUE(flags.any());
    EXPECT_TRUE(flags.start_allowance_override);
    EXPECT_FALSE(flags.release_timer || flags.midtick_stamp || flags.v0_payload ||
                 flags.scheduled_spring_back_fast);
    EXPECT_NE(log.find("manyforge_replacement_start_allowance component=robot_1 allowance_ms=" +
                       std::string(value)), std::string::npos);
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     MockReplacementAllowanceRejectsInvalidInputButDoesNotAffectIsaac) {
  ReplacementAllowanceEnvironment env;
  for (const char *value : {"", "0", "-1", "200.001", "nan", "inf", "-inf",
                            "1e309", "twenty", "20x"}) {
    SCOPED_TRACE(value);
    env.set(value);
    try {
      static_cast<void>(make_retained_mock_ros2_control_backend_config(
          mock_runtime_configuration("lite6_lite_gripper"), commanded_binding(),
          commanded_gripper_binding(), kMockCancel));
      FAIL() << "Invalid allowance was accepted";
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string(error.what()).find(ReplacementAllowanceEnvironment::name),
                std::string::npos);
      EXPECT_NE(std::string(error.what()).find("(0, 200]"), std::string::npos);
    }
  }
  // The same invalid experimental environment is never read by the Isaac lane.
  const auto isaac = make_retained_isaac_ros2_control_backend_config(
      isaac_runtime_configuration("lite6"), commanded_binding(), std::nullopt);
  EXPECT_DOUBLE_EQ(*isaac.participant.stop.replacement_start_allowance_s, 0.020);
  EXPECT_FALSE(isaac.arm.adapter.lane_facts.experiment.any());
}

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
  EXPECT_TRUE(adapter.lane_facts.supports_scheduled_replacement);
  EXPECT_TRUE(adapter.lane_facts.has_measured_velocity);
  EXPECT_EQ(adapter.lane_facts.cancel_semantics,
            core::ExecutionCancelSemantics::kDecelerate);
  // Merge 6 arrival: the lane keeps its JTC goal tolerances on every goal.
  EXPECT_EQ(adapter.lane_facts.goal_tolerance_policy,
            core::ExecutionGoalTolerancePolicy::kKept);
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
  // The fixture's measured_joints row budget.
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
  ASSERT_TRUE(config.arm.gripper_controller.has_value());
  EXPECT_EQ(*config.arm.gripper_controller,
            (RetainedRos2ControlArmController{
                "/manyforge/robot_1/controller_manager",
                "lite_gripper_controller", "MockLite6LiteGripperSystem"}));
  ASSERT_TRUE(config.gripper.has_value());
  EXPECT_EQ(config.gripper->transport_kind, "ros_gripper_command");
  EXPECT_EQ(config.gripper->action_name,
            "/manyforge/robot_1/lite_gripper_controller/gripper_cmd");
  EXPECT_EQ(config.gripper->completion_policy, "output_delivery");
  ASSERT_TRUE(config.gripper->result_timeout_s);
  EXPECT_DOUBLE_EQ(*config.gripper->result_timeout_s, 1.0);
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
    EXPECT_TRUE(config.arm.adapter.lane_facts.supports_scheduled_replacement);
    ASSERT_TRUE(config.participant.stop.replacement_start_allowance_s.has_value());
    EXPECT_DOUBLE_EQ(*config.participant.stop.replacement_start_allowance_s, 0.020);
    EXPECT_DOUBLE_EQ(config.participant.late_cycle_margin_s, 2.0 / 100.0);
    EXPECT_EQ(config.participant.stop.hard_stop_completion_timeout_ns,
              500'000'000)
        << profile;
    ASSERT_TRUE(config.arm.gripper_controller.has_value());
    EXPECT_EQ(config.arm.gripper_controller->controller_manager,
              config.arm.controller.controller_manager);
    EXPECT_EQ(config.arm.gripper_controller->controller_name,
              "robotiq_gripper_controller");
    EXPECT_EQ(config.arm.gripper_controller->hardware_component_name,
              config.arm.controller.hardware_component_name);
    ASSERT_TRUE(config.gripper.has_value());
    EXPECT_EQ(config.gripper->action_name,
              "/manyforge/robot_1/robotiq_gripper_controller/gripper_cmd");
    EXPECT_EQ(config.gripper->completion_policy, "position_reached");
    ASSERT_TRUE(config.gripper->result_timeout_s);
    EXPECT_DOUBLE_EQ(*config.gripper->result_timeout_s, 15.0);
  }
  const auto ur5 = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("ur5_roboplan"), commanded_binding(),
      std::nullopt, kMockCancel);
  EXPECT_FALSE(ur5.gripper.has_value());
  EXPECT_FALSE(ur5.arm.gripper_controller.has_value());
  EXPECT_EQ(ur5.arm.controller.hardware_component_name, "MockUR5System");
  EXPECT_EQ(ur5.participant.stop.hard_stop_completion_timeout_ns, 500'000'000);
}

[[nodiscard]]
deployment::ros2_control_execution_runtime_contract::Configuration
real_runtime_configuration() {
  deployment::ros2_control_execution_runtime_contract::Configuration result;
  deployment::ros2_control_execution_runtime_contract::Configuration::
      ControllerProfile profile{core::ControllableComponentId("robot_1"),
                                std::nullopt, std::nullopt};
  profile.real_profile = "lite6_real";
  profile.device_address_ref = "lite6_main";
  profile.adapter_family = "xarm";
  result.controller_profiles = {profile};
  result.planner_rng_seed = 12345U;
  result.execution_tick_hz = 100U;
  result.collision_monitor_period_ms = 50.0;
  add_component_streams(result);
  return result;
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RealProfileComposesMeasuredAuthorityWithItsFreshnessBudget) {
  const auto config = make_retained_real_ros2_control_backend_config(
      real_runtime_configuration(), measured_binding(), std::nullopt,
      kMockCancel);
  EXPECT_EQ(config.participant.stop.trip_authority,
            core::StateAuthorityClass::kMeasured);
  EXPECT_EQ(config.observed_state_authority_profile_id,
            kMeasuredJointStateProfileId);
  ASSERT_TRUE(config.arm.measured_joint_state_lane.has_value());
  const auto &lane = *config.arm.measured_joint_state_lane;
  EXPECT_EQ(lane.absolute_topic, "/manyforge/robot_1/joint_states");
  // R7 D3: 0.25 s, from the real profile's data row.
  EXPECT_EQ(lane.freshness_budget_ns, 250'000'000);
  EXPECT_EQ(lane.measured_state_authority_id.value(),
            kMeasuredJointStateAuthorityId);
  EXPECT_EQ(lane.measured_source_profile_id, kMeasuredJointStateProfileId);
  EXPECT_EQ(config.participant.progress_freshness_budget_ns, 250'000'000);
  EXPECT_EQ(config.participant.start_tolerance_rad, 0.02);
  // The terminal gate reads the Real row's arrival tolerance, the same value
  // as the controllers' JTC goal; the lane keeps that goal on every goal.
  EXPECT_EQ(config.participant.final_tolerance_rad, 0.01);
  EXPECT_EQ(config.arm.adapter.lane_facts.goal_tolerance_policy,
            core::ExecutionGoalTolerancePolicy::kKept);
  EXPECT_EQ(config.participant.completion_state_timeout_ns, 1'000'000'000);
  EXPECT_EQ(config.arm.adapter.action_name,
            "/manyforge/robot_1/lite6_traj_controller/follow_joint_trajectory");
  EXPECT_EQ(config.arm.controller.controller_manager,
            "/manyforge/robot_1/controller_manager");
  EXPECT_EQ(config.arm.controller.hardware_component_name, "RealLite6System");
  // Mock Lite 6's controller cadence (same template), no scheduled
  // replacement and no gripper.
  const auto mock = make_retained_mock_ros2_control_backend_config(
      mock_runtime_configuration("lite6_lite_gripper"), commanded_binding(),
      commanded_gripper_binding(), kMockCancel);
  EXPECT_EQ(config.arm.adapter.lane_facts.controller_update_rate_hz,
            mock.arm.adapter.lane_facts.controller_update_rate_hz);
  EXPECT_EQ(config.arm.adapter.lane_facts.state_rate_hz,
            mock.arm.adapter.lane_facts.state_rate_hz);
  // M6b JTC20: the action monitor at 20 Hz, as Mock (maintainer).
  EXPECT_DOUBLE_EQ(config.arm.adapter.lane_facts.feedback_rate_hz, 20.0);
  EXPECT_EQ(config.participant.stop.hard_stop_completion_timeout_ns,
            mock.participant.stop.hard_stop_completion_timeout_ns);
  EXPECT_FALSE(config.arm.adapter.lane_facts.supports_scheduled_replacement);
  EXPECT_FALSE(config.gripper.has_value());
  EXPECT_FALSE(config.arm.gripper_controller.has_value());
  EXPECT_EQ(config.participant.stop.max_deceleration_on_cancel_rad_s2,
            (std::map<std::string, double>{{"joint_1", 12.5}}));
  const auto state = retained_ros2_control_motion_state_config(config);
  EXPECT_EQ(state.source_kind, core::ObservedStateSourceKind::kJointState);
  EXPECT_EQ(state.freshness_budget_ns, 250'000'000);
  // The Mock builder is unchanged: commanded authority.
  EXPECT_EQ(mock.participant.stop.trip_authority,
            core::StateAuthorityClass::kCommanded);
  // No Deployment setting: the soft Stop lead-in starts at the command
  // position on both lanes.
  EXPECT_EQ(config.participant.stop.lead_in_anchor, core::StopLeadInAnchor::kCommanded);
  EXPECT_EQ(mock.participant.stop.lead_in_anchor, core::StopLeadInAnchor::kCommanded);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RealComponentStopLeadInAnchorComesFromItsDeploymentSetting) {
  for (const auto anchor : {core::StopLeadInAnchor::kCommanded, core::StopLeadInAnchor::kMeasured}) {
    auto runtime = real_runtime_configuration();
    runtime.controller_profiles.front().stop_lead_in_anchor = anchor;
    const auto config = make_retained_real_ros2_control_backend_config(
        runtime, measured_binding(), std::nullopt, kMockCancel);
    EXPECT_EQ(config.participant.stop.lead_in_anchor, anchor);
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RealProfileComposesStreamlessNoOutputPneumaticGripper) {
  auto runtime = real_runtime_configuration();
  auto row = fixture_gripper_row("robot_1", "pneumatic");
  row.output_mode = core::GripperOutputMode::kNone;
  runtime.gripper_target_configurations.push_back(row);
  const auto config = make_retained_real_ros2_control_backend_config(
      runtime, measured_binding(), commanded_gripper_binding(), kMockCancel);
  EXPECT_EQ(config.participant.stop.trip_authority, core::StateAuthorityClass::kMeasured);
  ASSERT_TRUE(config.gripper.has_value());
  EXPECT_EQ(config.gripper->actuation_kind, core::GripperActuationKind::kPneumatic);
  EXPECT_EQ(config.gripper->output_mode, core::GripperOutputMode::kNone);
  EXPECT_EQ(config.gripper->completion_policy, "no_output");
  EXPECT_TRUE(config.gripper->transport_kind.empty());
  EXPECT_TRUE(config.gripper->action_name.empty());
  EXPECT_TRUE(config.gripper->state_topic.empty());
  EXPECT_EQ(config.gripper->state_freshness_timeout_s, 0.0);
  EXPECT_FALSE(config.gripper->feedback_stream);
  EXPECT_FALSE(config.arm.gripper_controller.has_value());
  EXPECT_EQ(config.gripper->group_id, "gripper");
  EXPECT_EQ(config.gripper->joint_name, "gripper_joint");

  runtime.gripper_target_configurations.front().output_mode =
      core::GripperOutputMode::kCommanded;
  try {
    static_cast<void>(make_retained_real_ros2_control_backend_config(
        runtime, measured_binding(), commanded_gripper_binding(), kMockCancel));
    FAIL() << "Real commanded pneumatic output must wait for I/O routing";
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find("real_pneumatic_io_not_wired"), std::string::npos);
  }
  runtime.gripper_target_configurations.front().actuation_kind = "position_commanded";
  EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                   runtime, measured_binding(), commanded_gripper_binding(), kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                   runtime, measured_binding(), commanded_gripper_binding("other"), kMockCancel)),
               std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RealArmOnlyProjectorRegistersTheMeasuredChannelTarget) {
  // Live 2026-10-27 (Real Lite 6, arm only, passive fingers): the native host
  // registered the projector target with budget 0, the measured arm channel
  // carries its `measured_joints` budget, so the channel refused the target.
  const auto runtime_configuration = real_runtime_configuration();
  const auto motion = measured_binding();
  const auto state = retained_ros2_control_motion_state_config(
      make_retained_real_ros2_control_backend_config(
          runtime_configuration, motion, std::nullopt, kMockCancel));
  const auto budget = retained_ros2_control_motion_state_budget_ns(
      runtime_configuration, motion);
  EXPECT_EQ(budget, state.freshness_budget_ns);
  EXPECT_EQ(budget, 250'000'000);
  // Mock and Isaac keep the non-expiring commanded state.
  EXPECT_EQ(retained_ros2_control_motion_state_budget_ns(
                mock_runtime_configuration("lite6_lite_gripper"),
                commanded_binding()),
            0);
  core::FakeClock clock;
  try {
    static_cast<void>(std::make_shared<RetainedObservedStateChannel>(
        motion, test_projector(motion, 0, true), state, &clock));
    FAIL() << "a budget-0 projector does not register the measured target";
  } catch (const std::invalid_argument &error) {
    EXPECT_STREQ(error.what(), "retained observed-state channel target is not "
                               "registered exactly once");
  }
  const auto channel = std::make_shared<RetainedObservedStateChannel>(
      motion, test_projector(motion, budget, true), state, &clock);
  ASSERT_NE(channel->kinematic_projector(), nullptr);
  EXPECT_EQ(channel->kinematic_projector()->query_contexts(),
            std::vector<core::ObservedStateQueryContextDTO>{
                channel->query_context()});
}

TEST(RetainedRos2ControlBackendFactoryTest,
     RealArmWithNoOutputPneumaticGripperRegistersBothChannelTargets) {
  // Live merge #2 (Real Lite 6 + no-output pneumatic gripper): one projector
  // budget (the arm's measured 250 ms) for both targets, while the commanded
  // gripper channel carries 0, so the gripper channel refused its target.
  auto runtime = real_runtime_configuration();
  auto row = fixture_gripper_row("robot_1", "pneumatic");
  row.output_mode = core::GripperOutputMode::kNone;
  runtime.gripper_target_configurations.push_back(row);
  const auto motion = measured_binding();
  const auto gripper = commanded_gripper_binding();
  const auto motion_state = retained_ros2_control_motion_state_config(
      make_retained_real_ros2_control_backend_config(runtime, motion, gripper,
                                                     kMockCancel));
  const auto gripper_state = retained_ros2_control_gripper_state_config(gripper);
  const auto arm_budget =
      retained_ros2_control_motion_state_budget_ns(runtime, motion);
  EXPECT_EQ(arm_budget, motion_state.freshness_budget_ns);
  EXPECT_EQ(arm_budget, 250'000'000);
  EXPECT_EQ(gripper_state.freshness_budget_ns, 0);
  core::FakeClock clock;
  clock.set(1'000'000'000);
  {
    const auto shared = test_projector({motion, gripper},
                                       {arm_budget, arm_budget}, true);
    static_cast<void>(std::make_shared<RetainedObservedStateChannel>(
        motion, shared, motion_state, &clock));
    try {
      static_cast<void>(std::make_shared<RetainedObservedStateChannel>(
          gripper, shared, gripper_state, &clock));
      FAIL() << "a single arm budget does not register the gripper target";
    } catch (const std::invalid_argument &error) {
      EXPECT_STREQ(error.what(), "retained observed-state channel target is "
                                 "not registered exactly once");
    }
  }
  const auto projector = test_projector(
      {motion, gripper},
      {arm_budget, gripper_state.freshness_budget_ns}, true);
  const auto motion_channel = std::make_shared<RetainedObservedStateChannel>(
      motion, projector, motion_state, &clock);
  const auto gripper_channel = std::make_shared<RetainedObservedStateChannel>(
      gripper, projector, gripper_state, &clock);
  EXPECT_EQ(projector->query_contexts(),
            (std::vector<core::ObservedStateQueryContextDTO>{
                motion_channel->query_context(),
                gripper_channel->query_context()}));
  // As on the host, the commanded gripper seeds from the model projection.
  EXPECT_TRUE(gripper_channel->initialize_from_projection(clock.now()).applied());
}

TEST(RetainedRos2ControlBackendFactoryTest, RealProfileConfigurationFailsClosed) {
  // A gripper binding, a commanded path, another lane's row, or bad cancel
  // bounds refuse.
  EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                   real_runtime_configuration(), measured_binding(),
                   commanded_gripper_binding(), kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                   real_runtime_configuration(), commanded_binding(),
                   std::nullopt, kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                   mock_runtime_configuration("ur5_roboplan"), measured_binding(),
                   std::nullopt, kMockCancel)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(make_retained_mock_ros2_control_backend_config(
                   real_runtime_configuration(), commanded_binding(),
                   std::nullopt, kMockCancel)),
               std::invalid_argument);
  for (const auto &cancel : std::vector<std::map<std::string, double, std::less<>>>{
           {}, {{"source_joint_1", 0.0}}, {{"other_joint", 1.0}}}) {
    EXPECT_THROW(static_cast<void>(make_retained_real_ros2_control_backend_config(
                     real_runtime_configuration(), measured_binding(),
                     std::nullopt, cancel)),
                 std::invalid_argument);
  }
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
                   std::nullopt, kMockCancel)),
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

TEST(
    RetainedRos2ControlBackendFactoryTest,
    PneumaticNoneFactoryCompletesImmediatelyForZeroAndProgramDwellWithoutPhysicalTransport) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto tool = commanded_gripper_binding();
  auto channel = test_projected_channel(
      tool, retained_ros2_control_gripper_state_config(tool), clock);
  // A model-backed no-feedback gripper projects its commanded position, so
  // seed the channel as the retained runtime does.
  ASSERT_TRUE(channel->initialize_from_projection(0).applied());
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  auto config = gripper_only_factory_config();
  config.arm.gripper_controller.reset();
  config.gripper->actuation_kind = core::GripperActuationKind::kPneumatic;
  config.gripper->output_mode = core::GripperOutputMode::kNone;
  config.gripper->action_name.clear();
  config.gripper->transport_kind.clear();
  config.gripper->completion_policy = "no_output";
  auto backend = make_retained_ros2_control_gripper_backend(
      config, tool, channel, clock, scheduler, handle);
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->gripper_actuation_kind(tool.value().command_target_id.value()),
            core::GripperActuationKind::kPneumatic);
  EXPECT_EQ(backend->gripper_output_mode(tool.value().command_target_id.value()),
            core::GripperOutputMode::kNone);
  handle->StartBackend();
  const auto local = handle->gripper_transport_readiness_snapshot();
  ASSERT_TRUE(local.has_value());
  EXPECT_TRUE(local->configured);
  EXPECT_TRUE(local->logical_owner_ready);
  EXPECT_TRUE(local->ready);
  EXPECT_FALSE(local->command_path_ready);
  EXPECT_FALSE(local->feedback_path_ready);
  EXPECT_TRUE(local->transport_kind.empty());
  EXPECT_TRUE(local->state_topic.empty());
  EXPECT_EQ(local->feedback_messages_received, 0U);
  EXPECT_FALSE(handle->participant());
  EXPECT_FALSE(handle->current_port());
  EXPECT_FALSE(handle->ReadOwnedControllerOccurrence().arm_binding_epoch);
  std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner = handle;
  handle.reset();
  EXPECT_FALSE(owner.expired());

  core::GripperCommandRequestDTO request;
  request.gripper_id = tool.value().command_target_id.value();
  request.group_id = "gripper";
  request.joint_name = "gripper_joint";
  request.completion_policy = "no_output";
  request.grasp_confirmation_policy = "not_required";
  for (const double dwell : {0.0, 0.25}) {
    request.duration_s = dwell;
    ASSERT_TRUE(backend->validate_gripper_command(request).ok());
    const auto command = backend->command_gripper(request);
    ASSERT_TRUE(command.accepted);
    const auto completed = backend->get_gripper_status(request.gripper_id);
    EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
    EXPECT_EQ(completed.command_id, command.command_id);
    EXPECT_EQ(completed.generation, command.generation);
    EXPECT_FALSE(completed.error.has_value());
    EXPECT_EQ(completed.completion_policy, "no_output");
    EXPECT_TRUE(completed.transport_kind.empty());
    EXPECT_FALSE(completed.current_position.has_value());
    EXPECT_FALSE(completed.observed_position.has_value());
    EXPECT_EQ(completed.projection_state, core::GripperProjectionState::kApplied);
    EXPECT_NE(std::find(completed.evidence.begin(), completed.evidence.end(),
                        "pneumatic_no_output"), completed.evidence.end());
    for (const char *physical : {"pneumatic_output_delivery_confirmed",
                                 "ros_gripper_command_enqueued",
                                 "isaac_gripper_command_published"})
      EXPECT_EQ(std::find(completed.evidence.begin(), completed.evidence.end(),
                          physical), completed.evidence.end());
  }
  EXPECT_EQ(clock.now(), 0);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
            0U);
  backend.reset();
  EXPECT_TRUE(owner.expired());
}

TEST(RetainedRos2ControlBackendFactoryTest,
     PneumaticNoneFactoryRetainsExactIdentityAndCommandedRequirements) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto tool = commanded_gripper_binding();
  const auto foreign = commanded_gripper_binding("robot_foreign");
  const auto channel = test_projected_channel(
      tool, retained_ros2_control_gripper_state_config(tool), clock);
  const auto foreign_channel = test_projected_channel(
      foreign, retained_ros2_control_gripper_state_config(foreign), clock);
  auto none = gripper_only_factory_config();
  none.arm.gripper_controller.reset();
  none.gripper->actuation_kind = core::GripperActuationKind::kPneumatic;
  none.gripper->output_mode = core::GripperOutputMode::kNone;
  none.gripper->action_name.clear();
  none.gripper->transport_kind.clear();
  none.gripper->completion_policy = "no_output";
  EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                   none, tool, foreign_channel, clock, scheduler,
                   RetainedRos2ControlRuntimeHandle::Create())),
               std::invalid_argument);
  auto physical = none;
  physical.arm.gripper_controller = RetainedRos2ControlArmController{};
  EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                   physical, tool, channel, clock, scheduler,
                   RetainedRos2ControlRuntimeHandle::Create())),
               std::invalid_argument);
  auto commanded = none;
  commanded.gripper->output_mode = core::GripperOutputMode::kCommanded;
  EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                   commanded, tool, channel, clock, scheduler,
                   RetainedRos2ControlRuntimeHandle::Create())),
               std::invalid_argument);
  commanded.arm.gripper_controller = gripper_only_factory_config().arm.gripper_controller;
  EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                   commanded, tool, channel, clock, scheduler,
                   RetainedRos2ControlRuntimeHandle::Create())),
               std::invalid_argument);
}

// Merge 6: a robot whose tool is its own component carries no gripper on
// Isaac; a gripper binding on the robot row is refused.
TEST(RetainedRos2ControlBackendFactoryTest,
     IsaacRobotRowIsArmOnlyAndRefusesAGripper) {
  for (const auto &[robot, hardware, deceleration] :
       {std::tuple{"lite6", "IsaacLite6TopicBasedSystem", 8.0},
        std::tuple{"ur10e", "IsaacUR10eTopicBasedSystem", 10.0}}) {
    SCOPED_TRACE(robot);
    const auto arm_only = make_retained_isaac_ros2_control_backend_config(
        isaac_runtime_configuration(robot), commanded_binding(), std::nullopt);
    EXPECT_FALSE(arm_only.gripper.has_value());
    EXPECT_FALSE(arm_only.arm.gripper_controller.has_value());
    EXPECT_EQ(arm_only.arm.controller.hardware_component_name, hardware);
    EXPECT_EQ(arm_only.arm.adapter.action_name,
              "/manyforge/robot_1/isaac_arm_controller/follow_joint_trajectory");
    EXPECT_EQ(arm_only.participant.stop.max_deceleration_on_cancel_rad_s2,
              (std::map<std::string, double>{{"joint_1", deceleration}}));
    ASSERT_TRUE(arm_only.arm.measured_joint_state_lane.has_value());
    EXPECT_EQ(arm_only.arm.measured_joint_state_lane->absolute_topic,
              "/isaac_joint_states");
    EXPECT_THROW(static_cast<void>(make_retained_isaac_ros2_control_backend_config(
                     isaac_runtime_configuration(robot), commanded_binding(),
                     commanded_gripper_binding())),
                 std::invalid_argument);
  }
}

// Merge 6 (D4): a lane-free pneumatic tool has no controller profile and no
// streams; its I/O-mapped gripper is the no-output logical owner.
TEST(RetainedRos2ControlBackendFactoryTest,
     LaneFreePneumaticToolIsAStreamlessNoOutputOwner) {
  deployment::ros2_control_execution_runtime_contract::Configuration runtime;
  auto row = fixture_gripper_row("tool_1", "pneumatic");
  row.pneumatic_io = deployment::ros2_control_execution_runtime_contract::
      Configuration::PneumaticIo{
          {{core::IoSignalId{core::ControllableComponentId("valve_io"), "valve"}, false, true}},
          {}};
  runtime.gripper_target_configurations.push_back(std::move(row));
  const auto tool = commanded_gripper_binding("tool_1");
  RetainedRos2ControlBackendConfig config;
  config.gripper = ros2_control_execution::Ros2GripperActionConfig{};
  configure_retained_gripper_output_mode(config, runtime, tool);
  ASSERT_TRUE(config.gripper.has_value());
  EXPECT_EQ(config.gripper->output_mode, core::GripperOutputMode::kNone);
  EXPECT_EQ(config.gripper->completion_policy, "no_output");
  EXPECT_FALSE(config.arm.gripper_controller.has_value());
}

// Merge 6: the opt-in stall window comes from the tool's own gripper row;
// without it the backend publishes no grasp evidence (the default).
TEST(RetainedRos2ControlBackendFactoryTest,
     ToolGripperRowCarriesItsOptInGraspStallWindow) {
  using Contract = deployment::ros2_control_execution_runtime_contract::Configuration;
  auto runtime = isaac_runtime_configuration("robotiq_2f_140");
  const auto tool = commanded_gripper_binding("robot_a");
  const auto configured = [&] {
    RetainedRos2ControlBackendConfig config;
    configure_retained_isaac_gripper_action(config, tool, "robotiq_2f_140", "/manyforge/robot_a");
    configure_retained_gripper_output_mode(config, runtime, tool);
    return config;
  };
  EXPECT_FALSE(configured().gripper->grasp_stall_window.has_value());
  for (auto &row : runtime.gripper_target_configurations) {
    if (row.controllable_component_id == core::ControllableComponentId("robot_a"))
      row.grasp_confirmation = Contract::GraspConfirmation{
          true, Contract::GraspStallWindow{0.001, 0.02}};
  }
  const auto config = configured();
  ASSERT_TRUE(config.gripper->grasp_stall_window.has_value());
  EXPECT_DOUBLE_EQ(config.gripper->grasp_stall_window->min_position, 0.001);
  EXPECT_DOUBLE_EQ(config.gripper->grasp_stall_window->max_position, 0.02);
}

// M6 (SMALL3): a pneumatic I/O mapping on an Isaac gripper is refused with its
// catalog code; the message text is unchanged.
TEST(RetainedRos2ControlBackendFactoryTest, IsaacPneumaticIoMappingIsRefusedTyped) {
  using Contract = deployment::ros2_control_execution_runtime_contract::Configuration;
  auto runtime = isaac_runtime_configuration("robotiq_2f_140");
  const auto tool = commanded_gripper_binding("robot_a");
  for (auto &row : runtime.gripper_target_configurations) {
    if (row.controllable_component_id == core::ControllableComponentId("robot_a"))
      row.pneumatic_io = Contract::PneumaticIo{
          {{core::IoSignalId{core::ControllableComponentId("valve_io"), "valve"}, false, true}},
          {}};
  }
  RetainedRos2ControlBackendConfig config;
  configure_retained_isaac_gripper_action(config, tool, "robotiq_2f_140", "/manyforge/robot_a");
  try {
    configure_retained_gripper_output_mode(config, runtime, tool);
    FAIL() << "an Isaac gripper must refuse a pneumatic I/O mapping";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), "pneumatic_io_unsupported_on_isaac");
    EXPECT_STREQ(error.what(),
                 "pneumatic_io_unsupported_on_isaac: Isaac uses its existing gripper transport");
  }
}

// Merge 6: the Isaac tool's transport is the profile's, in the tool's own
// namespace; only an action transport has a bridge for the tool's launch.
TEST(RetainedRos2ControlBackendFactoryTest,
     IsaacToolGripperActionIsTheProfileTransportInTheToolNamespace) {
  const auto tool = commanded_gripper_binding("robot_a");
  RetainedRos2ControlBackendConfig ur10e;
  configure_retained_isaac_gripper_action(ur10e, tool, "robotiq_2f_140", "/manyforge/robot_a");
  ASSERT_TRUE(ur10e.gripper.has_value());
  EXPECT_EQ(ur10e.gripper->transport_kind, "ros_gripper_command");
  EXPECT_EQ(ur10e.gripper->action_name,
            "/manyforge/robot_a/robotiq_gripper_controller/gripper_cmd");
  EXPECT_EQ(ur10e.gripper->state_topic, "/isaac_joint_states");
  EXPECT_EQ(ur10e.gripper->command_joint_names,
            (std::vector<std::string>{"source_gripper_joint"}));
  EXPECT_FALSE(ur10e.arm.gripper_controller.has_value());
  RetainedRos2ControlBackendConfig lite6;
  configure_retained_isaac_gripper_action(lite6, tool, "lite_gripper", "/manyforge/robot_a");
  EXPECT_EQ(lite6.gripper->transport_kind, "isaac_joint_state_effort");
  EXPECT_EQ(lite6.gripper->command_topic, "/isaac_joint_commands_gripper");

  const auto bridge = retained_isaac_gripper_bridge("robotiq_2f_140");
  ASSERT_TRUE(bridge.has_value());
  EXPECT_EQ(bridge->action_name, "robotiq_gripper_controller/gripper_cmd");
  EXPECT_EQ(bridge->command_topic, "/isaac_gripper_state");
  EXPECT_EQ(bridge->state_topic, "/isaac_joint_states");
  EXPECT_EQ(bridge->parameters_file, "isaac_robotiq_2f_140_gripper_bridge.yaml");
  EXPECT_FALSE(retained_isaac_gripper_bridge("lite_gripper").has_value());
  RetainedRos2ControlBackendConfig unknown;
  EXPECT_THROW(configure_retained_isaac_gripper_action(unknown, tool, "ur5e", "/manyforge/robot_a"),
               std::invalid_argument);
}

// Merge 6: an Isaac tool owner binds without a controller manager; its
// lifecycle primitives go to the gripper bridge, as for a robot that carries
// its Isaac gripper, never to a controller switch.
TEST(RetainedRos2ControlBackendFactoryTest,
     IsaacToolGripperOnlyOwnerUsesTheBridgeLifecycleWithoutAControllerManager) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto tool = commanded_gripper_binding("robot_a");
  const auto runtime = isaac_runtime_configuration("robotiq_2f_140");
  const auto channel = test_projected_channel(
      tool, retained_ros2_control_gripper_state_config(tool), clock);
  auto config = gripper_only_factory_config("robot_a");
  config.arm.gripper_controller.reset();
  config.gripper.reset();
  configure_retained_isaac_gripper_action(config, tool, "robotiq_2f_140", config.node_namespace);
  configure_retained_gripper_output_mode(config, runtime, tool);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  const auto backend = make_retained_ros2_control_gripper_backend(
      config, tool, channel, clock, scheduler, handle);
  ASSERT_NE(backend, nullptr);
  EXPECT_TRUE(handle->OwnsGripper(backend.get(), tool));
  EXPECT_FALSE(handle->participant());
  const auto prepared = handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.gripper"),
      core::LifecyclePrimitiveId("ros2_control.controller.prepare"), {}, std::nullopt, {});
  ASSERT_TRUE(prepared.error.has_value());
  // No action server runs in this fixture: the bridge refuses, not a switch.
  EXPECT_EQ(prepared.error->code, "ros2_control_gripper_bridge_not_ready");
  // A half-named controller is neither a controller manager nor a bridge.
  auto partial = config;
  partial.arm.gripper_controller = RetainedRos2ControlArmController{
      config.node_namespace + "/controller_manager", "", ""};
  EXPECT_THROW(static_cast<void>(make_retained_ros2_control_gripper_backend(
                   partial, tool, channel, clock, scheduler,
                   RetainedRos2ControlRuntimeHandle::Create())),
               std::invalid_argument);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     StrictSelectedPneumaticSourceMapsDefaultAndExplicitTimeoutAcrossProfiles) {
  for (bool isaac : {false, true}) {
    for (const std::optional<double> timeout :
         {std::optional<double>{}, std::optional<double>{0.25},
          std::optional<double>{5.0}, std::optional<double>{15.0}}) {
      auto runtime = isaac ? isaac_runtime_configuration("lite_gripper")
                           : mock_runtime_configuration("lite6_lite_gripper");
      auto &selected = runtime.gripper_target_configurations.front();
      selected.delivery_result_timeout_s = timeout;
      auto foreign = fixture_gripper_row("unselected", "pneumatic");
      foreign.delivery_result_timeout_s = 91.0;
      runtime.gripper_target_configurations.push_back(std::move(foreign));
      const auto config =
          isaac ? isaac_tool_config(runtime, commanded_gripper_binding())
                : make_retained_mock_ros2_control_backend_config(
                      runtime, commanded_binding(), commanded_gripper_binding(),
                      kMockCancel);
      ASSERT_TRUE(config.gripper);
      EXPECT_EQ(config.gripper->actuation_kind,
                core::GripperActuationKind::kPneumatic);
      EXPECT_EQ(config.gripper->completion_policy, "output_delivery");
      ASSERT_TRUE(config.gripper->result_timeout_s);
      EXPECT_DOUBLE_EQ(*config.gripper->result_timeout_s,
                       timeout.value_or(1.0));
      EXPECT_EQ(config.gripper->wait_for_action_server_timeout_s, 2.0);
    }
  }
}

TEST(RetainedRos2ControlBackendFactoryTest,
     StrictNoneSourceRetainsPositiveTimeoutMetadataWithoutCommandTransport) {
  auto runtime = mock_runtime_configuration("lite6_lite_gripper");
  auto &selected = runtime.gripper_target_configurations.front();
  selected.output_mode = core::GripperOutputMode::kNone;
  selected.delivery_result_timeout_s = 0.125;
  const auto config = make_retained_mock_ros2_control_backend_config(
      runtime, commanded_binding(), commanded_gripper_binding(), kMockCancel);
  ASSERT_TRUE(config.gripper);
  EXPECT_EQ(config.gripper->completion_policy, "no_output");
  ASSERT_TRUE(config.gripper->result_timeout_s);
  EXPECT_DOUBLE_EQ(*config.gripper->result_timeout_s, 0.125);
  EXPECT_TRUE(config.gripper->transport_kind.empty());
  EXPECT_TRUE(config.gripper->action_name.empty());
  EXPECT_TRUE(config.gripper->command_topic.empty());
  EXPECT_FALSE(config.arm.gripper_controller);
}

TEST(RetainedRos2ControlBackendFactoryTest,
     StrictPositionSourceKeepsExistingLite6FiveAndUrFifteenTimeouts) {
  // The typed row owns actuation. A Position row keeps each lane's old profile
  // timeout even for the Lite 6 profile; no numeric 5-to-1 heuristic is valid.
  for (bool isaac : {false, true}) {
    for (bool lite6 : {false, true}) {
      // Merge 6: on Isaac the tool row (its own component) owns the gripper.
      auto runtime =
          isaac ? isaac_runtime_configuration(lite6 ? "lite_gripper"
                                                   : "robotiq_2f_140")
                : mock_runtime_configuration(lite6 ? "lite6_lite_gripper"
                                                   : "ur10e_robotiq_2f_140");
      runtime.gripper_target_configurations.front().actuation_kind =
          "position_commanded";
      const auto config =
          isaac ? isaac_tool_config(runtime, commanded_gripper_binding())
                : make_retained_mock_ros2_control_backend_config(
                      runtime, commanded_binding(), commanded_gripper_binding(),
                      kMockCancel);
      ASSERT_TRUE(config.gripper);
      ASSERT_TRUE(config.gripper->result_timeout_s);
      EXPECT_DOUBLE_EQ(*config.gripper->result_timeout_s, lite6 ? 5.0 : 15.0);
      EXPECT_EQ(config.gripper->actuation_kind,
                core::GripperActuationKind::kPositionCommanded);
    }
  }
}

} // namespace

} // namespace manyforge::roboplan_backend
