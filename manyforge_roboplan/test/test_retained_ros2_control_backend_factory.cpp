#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#define MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#undef MANYFORGE_ROS2_CONTROL_EXECUTION_TESTING

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
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

[[nodiscard]] core::RuntimeParticipantBinding commanded_binding() {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId("robot.1"),
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

} // namespace
} // namespace manyforge::roboplan_backend
