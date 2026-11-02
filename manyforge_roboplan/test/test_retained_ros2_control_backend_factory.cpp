#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

#include <gtest/gtest.h>
#include <roboplan/core/scene.hpp>
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
      static_cast<void>(factory(motion, nullptr, clock, scheduler));
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
  const auto backend = factory(motion, motion_channel, clock, scheduler);
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
  const auto backend = factory(motion, motion_channel, *clock, scheduler);
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
      nullptr, commanded_binding(), std::nullopt,
      std::make_shared<core::FakeClock>());
  ASSERT_FALSE(bound.ok());
  EXPECT_EQ(bound.error().details.at("reason"),
            "ros2_control_handle_clock_owner_mismatch");
  EXPECT_FALSE(participant_constructed);
  EXPECT_FALSE(handle->node());
  EXPECT_FALSE(handle->participant());
}

// This fixture owns the real ROS transport, the epoch-1 JTC adapter, the Core
// participant over it and the gripper adapter; no executor or remote
// controller is started by these tests.
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
  // The test-only engine backend borrows the handle's transport resources.
  std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
      engine_backend_owner;
  std::unique_ptr<core::GripperParticipantEngine> motion_engine;
  std::unique_ptr<core::GripperParticipantEngine> gripper_engine;
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;

  explicit InitialRosInstallationFixture(
      bool with_gripper = true, bool component_local_gripper = false)
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
      engine_backend_owner = std::move(owned_gripper);
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
    // The registrations carry exact identity, not motion.
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
};

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

} // namespace

namespace {

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

// CONTRACTS §16: a machine whose arm member is deactivated has no arm
// composition to build its owner, so the host builds that owner on its own
// (running the machine's process where it has one); a machine with an arm
// member keeps its owner on the arm's composition.
TEST(RetainedRos2ControlLaneConfigTest, ArmlessMachinesAreThoseNoArmMemberNames) {
  using Configuration = deployment::ros2_control_execution_runtime_contract::Configuration;
  Configuration configuration{};
  configuration.machines = {
      {core::ControllableComponentId("lite6"), "mock", std::nullopt, std::nullopt},
      {core::ControllableComponentId("uf850"), "isaac", std::nullopt, std::nullopt},
  };
  const Configuration::ControllerProfile arm{
      core::ControllableComponentId("lite6_robot"), "mock", "{}", "ufactory.lite6", "",
      std::nullopt, std::nullopt, true, "lite6", "arm"};
  const Configuration::ControllerProfile ownerless{
      core::ControllableComponentId("ur10e"), "mock", "{}", "universal_robots.ur10e", "",
      std::nullopt, std::nullopt, true, std::nullopt, std::nullopt};
  configuration.controller_profiles = {arm, ownerless};

  const auto armless = retained_ros2_control_armless_machines(configuration);
  ASSERT_EQ(armless.size(), 1U);
  EXPECT_EQ(armless.front().controllable_component_id.value(), "uf850");
  EXPECT_EQ(armless.front().lane, "isaac");
  EXPECT_FALSE(armless.front().machine_lane_row.has_value());

  configuration.controller_profiles = {ownerless};
  EXPECT_EQ(retained_ros2_control_armless_machines(configuration).size(), 2U);
  configuration.machines.clear();
  EXPECT_TRUE(retained_ros2_control_armless_machines(configuration).empty());
}

} // namespace

} // namespace manyforge::roboplan_backend
