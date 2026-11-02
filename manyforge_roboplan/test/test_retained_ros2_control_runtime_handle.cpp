// ROS runtime handle and Isaac guard binding against in-process stand-ins of
// the controller manager, the trajectory controller and the Isaac-side guard
// (design R6/§7.5 activation, §3.1 binding epochs, §4.3/Q41/Q43/Q46).
#include "ros2_control_fake_controller.hpp"
#include "committed_tool_payload_follower.hpp"
#include "../src/program_reset_position_observation.hpp"

#include <gtest/gtest.h>

#include <pal_statistics_msgs/msg/statistics.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/ros2_control_component_adapter.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>
#include <roboplan/core/scene.hpp>
#include <xarm_msgs/msg/robot_msg.hpp>
#include <xarm_msgs/srv/call.hpp>
#include <xarm_msgs/srv/get_int16.hpp>
#include <xarm_msgs/srv/get_int16_list.hpp>
#include <xarm_msgs/srv/set_float32_list.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// Test-only access to the handle's locks and endpoint facts. Transport facts
/// remain supplied by the actual ROS peers.
class RetainedRos2ControlRuntimeHandleTestAccess final {
public:
  static bool TryLockSwitch(RetainedRos2ControlRuntimeHandle &handle) {
    if (!handle.switch_mutex_.try_lock()) return false;
    handle.switch_mutex_.unlock();
    return true;
  }
  static void RefreshEndpointFacts(RetainedRos2ControlRuntimeHandle &handle) {
    handle.RefreshEndpointFacts();
  }
  /// The selected ROS endpoints (CM activity, controller state) each have
  /// exactly one publisher. Until then every lifecycle step is refused before
  /// effect (`component_instance_graph_unsettled`), so a test's first step
  /// waits for this, not only for a CM observation.
  [[nodiscard]] static bool EndpointsSettled(RetainedRos2ControlRuntimeHandle &handle) {
    handle.RefreshEndpointFacts();
    return !handle.ReadEndpointDiagnostic().has_value();
  }
  [[nodiscard]] static bool GripperActive(const RetainedRos2ControlRuntimeHandle &handle) {
    std::lock_guard lock(handle.mutex_);
    return handle.gripper_activity_.observed && handle.gripper_activity_.controller_active;
  }
  /// Holds the handle's state lock: a poll that reads handle state blocks
  /// inside it while holding its handle lease.
  [[nodiscard]] static std::unique_lock<std::mutex>
  LockState(const RetainedRos2ControlRuntimeHandle &handle) {
    return std::unique_lock<std::mutex>(handle.mutex_);
  }
};

namespace {

namespace fake = manyforge::ros2_control_execution::fake_controller;
using namespace std::chrono_literals;

/// A lifecycle command time no test step reaches.
[[nodiscard]] MockResetPositionStepDeadline Soon() {
  return std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

[[nodiscard]] std::string SwitchFailureText(
    const RetainedRos2ControlSwitchResult &result) {
  std::ostringstream text;
  if (result.error) {
    text << result.error->code << ": " << result.error->message;
    for (const auto &[key, value] : result.error->details)
      text << "\n" << key << '=' << value;
  }
  return text.str();
}

class NullSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string &, const std::vector<double> &, core::ExecutionStateUpdateMode) override {
    return {};
  }
  std::optional<double> group_joint_position(const std::string &,
                                             const std::string &) const override {
    return 0.0;
  }
  core::ExecutionStateSinkResult apply_group_joint_position(const std::string &,
                                                            const std::string &, double,
                                                            core::ExecutionStateUpdateMode) override {
    return {};
  }
};

[[nodiscard]] core::RuntimeParticipantBinding ArmBinding() {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId("robot_1"),
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
                                         core::ModelInstanceId("robot.model"), "arm",
                                         {"joint_1"}}},
      {core::ProgramFrameRoleBindingDTO{core::FrameRoleId("tcp"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision"), "tcp"}},
      {core::RuntimeResourceBindingDTO{core::ResourceRoleId("joint_1"),
                                       core::ResourceId("robot.joint.1"), "source_joint_1",
                                       "joint_1", core::CoordinateKind::kAngular}},
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.arm"), core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.arm"), core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface.trajectory"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"), core::ControlRouteId("route.ros2_control"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.ros2_control"),
          core::ProviderManifestId("provider-manifest.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.ros2_control"), core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.commanded"), core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop.ros2_control"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource, std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{core::RuntimeUnitId("runtime.ros2_control"),
                                        core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

/// One handle over the fake controller manager and trajectory controller.
struct ArmWorld final {
  explicit ArmWorld(
      const std::string &action_controller = "arm_controller",
      std::size_t *participant_constructions = nullptr,
      core::RuntimeParticipantBinding initial_binding = ArmBinding(),
      std::shared_ptr<core::IClock> receiver_clock = {},
      bool mock_reset_position = false,
      std::function<std::optional<std::string>()> process_fault = {},
      ros2_control_execution::Ros2TransportResources::ContextProvider context_provider = {},
      std::size_t executor_threads = 3U,
      core::Duration measured_freshness_ns = 100,
      bool command_echo = false)
      : owning_clock(receiver_clock ? std::move(receiver_clock)
                                    : std::make_shared<WallClock>()),
        clock(*owning_clock), binding(std::move(initial_binding)),
        prefix("/manyforge_handle_test_" + fake::unique_suffix()),
        jtc(prefix + "/arm_controller", {"source_joint_1"}),
        manager(prefix + "/controller_manager", "arm_controller", "TestSystem",
                &jtc, mock_reset_position ? "reset_writer" : "") {
    RetainedRos2ControlArmTransport arm;
    arm.adapter.action_name = prefix + "/" + action_controller +
                              "/follow_joint_trajectory";
    arm.adapter.state_topic = prefix + "/arm_controller/controller_state";
    arm.adapter.scale_topic = prefix + "/arm_controller/speed_scaling_input";
    arm.adapter.lane_facts.joint_names = {"source_joint_1"};
    arm.adapter.lane_facts.supports_scale = true;
    arm.adapter.lane_facts.state_rate_hz = 100.0;
    arm.adapter.lane_facts.feedback_rate_hz = 100.0;
    arm.adapter.lane_facts.controller_update_rate_hz = 100.0;
    arm.controller = {prefix + "/controller_manager", "arm_controller", "TestSystem"};
    arm.compiled_joint_names = {"joint_1"};
    if (mock_reset_position)
      arm.mock_reset_position =
          MockResetPositionTarget{"test-runtime-identity",
                                  "reset_writer",
                                  prefix + "/reset_writer/commands",
                                  {"source_joint_1"},
                                  {0.125},
                                  "test-target-digest",
                                  command_echo ? std::optional(prefix + "/joint_commands")
                                               : std::nullopt};
    arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
        prefix + "/joint_states", measured_freshness_ns,
        core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
        std::string(kMeasuredJointStateProfileId),
        core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck, std::nullopt};
    const auto bound = handle->Bind(
        std::make_unique<ros2_control_execution::Ros2TransportResources>(
            "manyforge_handle_test_" + fake::unique_suffix(), "/", executor_threads,
            std::make_shared<NullSink>(), "arm", clock, std::move(context_provider)),
        clock, std::move(arm),
        [this, participant_constructions](
            std::shared_ptr<core::IExecutionPort> port) {
          if (participant_constructions)
            ++*participant_constructions;
          core::ExecutionPortParticipantConfig config;
          config.stop.trip_authority = core::StateAuthorityClass::kCommanded;
          config.stop.stop_traj_delta_s = 0.1;
          config.stop.sample_period_s = 0.01;
          config.stop.replacement_start_allowance_s = 0.02;
          config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
          config.late_cycle_margin_s = 0.5;
          return std::make_shared<core::ExecutionPortParticipant>(
              binding, std::move(port), clock, scheduler,
              [](core::TimePoint, const std::stop_token &) {
                std::this_thread::sleep_for(1ms);
              },
              core::ExecutionPortParticipantStateAuthority{
                  &publication, [] { return core::Result<void>::Ok(); }},
              std::move(config));
        },
        nullptr, binding, std::nullopt);
    if (!bound.ok()) throw std::logic_error(bound.error().message);
    if (mock_reset_position) {
      auto fault_source = process_fault ? std::move(process_fault)
          : std::function<std::optional<std::string>()>(
                [] { return std::optional<std::string>{}; });
      handle->InstallControllerProcessFaultSource(
          fault_source, [fault_source] {
            return core::Result<std::optional<std::string>>::Ok(fault_source());
          });
    }
  }

  /// The component state the ros2_control adapter reports now.
  [[nodiscard]] core::ComponentState Observe() const {
    return ros2_control_component_state(handle->ReadObservation(clock.now()), clock.now());
  }

  [[nodiscard]] bool WaitForLifecycle(core::ComponentLifecycle lifecycle) const {
    return fake::WaitUntil([&] { return Observe().lifecycle == lifecycle; });
  }

  std::shared_ptr<core::IClock> owning_clock;
  core::IClock &clock;
  SimpleScheduler scheduler;
  core::RuntimeParticipantBinding binding;
  core::ObservedRobotStatePublication publication{
      {"joint_1"},
      {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}}};
  std::string prefix;
  fake::FakeJtc jtc;
  fake::FakeControllerManager manager;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle =
      RetainedRos2ControlRuntimeHandle::Create();
};

using Outcome = RetainedRos2ControlSwitchOutcome;

// This peer publishes the same CM activity and strict switch service as the
// existing arm fixture, with a separately configured gripper controller.
class GripperControllerManager final : public fake::PeerNode {
public:
  using Switch = controller_manager_msgs::srv::SwitchController;
  using Gripper = control_msgs::action::GripperCommand;
  explicit GripperControllerManager(const std::string &prefix,
                                    fake::FakeJtc &jtc, bool expose_action)
      : PeerNode("manyforge_gripper_cm"), jtc_(jtc) {
    activity_ =
        node()
            ->create_publisher<
                controller_manager_msgs::msg::ControllerManagerActivity>(
                prefix + "/controller_manager/activity",
                rclcpp::QoS(1).reliable().transient_local());
    switch_ = node()->create_service<Switch>(
        prefix + "/controller_manager/switch_controller",
        [this](const std::shared_ptr<Switch::Request> request,
               std::shared_ptr<Switch::Response> response) {
          std::lock_guard lock(mutex_);
          requests_.push_back(*request);
          const auto &names = request->activate_controllers.empty()
                                  ? request->deactivate_controllers
                                  : request->activate_controllers;
          if (refuse_ || request->strictness != Switch::Request::STRICT ||
              names.size() != 1U ||
              (names.front() != "arm_controller" &&
               names.front() != "gripper_controller")) {
            response->ok = false;
            if (refuse_)
              response->message = refusal_message_;
            return;
          }
          const bool active = !request->activate_controllers.empty();
          if (names.front() == "arm_controller") {
            arm_active_ = active;
            jtc_.SetActive(active);
          } else {
            gripper_active_ = active;
          }
          PublishLocked();
          response->ok = true;
        });
    // Deliberately reachable even when CM reports the controller inactive:
    // action discovery must not become physical lifecycle authority.
    if (expose_action) {
      action_ = rclcpp_action::create_server<Gripper>(
          node(), prefix + "/gripper_controller/gripper_cmd",
          [](const rclcpp_action::GoalUUID &,
             std::shared_ptr<const Gripper::Goal>) {
            return rclcpp_action::GoalResponse::REJECT;
          },
          [](const std::shared_ptr<rclcpp_action::ServerGoalHandle<Gripper>>) {
            return rclcpp_action::CancelResponse::ACCEPT;
          },
          [](const std::shared_ptr<rclcpp_action::ServerGoalHandle<Gripper>>) {
          });
    }
    Start();
    std::lock_guard lock(mutex_);
    PublishLocked();
  }
  ~GripperControllerManager() override { Stop(); }
  void SetGripper(bool loaded, bool active) {
    std::lock_guard lock(mutex_);
    gripper_loaded_ = loaded;
    gripper_active_ = active;
    PublishLocked();
  }
  void Refuse(bool value, std::string message = {}) {
    std::lock_guard lock(mutex_);
    refuse_ = value;
    refusal_message_ = std::move(message);
  }
  std::vector<Switch::Request> Requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

private:
  void PublishLocked() {
    controller_manager_msgs::msg::ControllerManagerActivity message;
    message.header.stamp = node()->now();
    const auto row = [](const std::string &name, bool active) {
      controller_manager_msgs::msg::NamedLifecycleState result;
      result.name = name;
      result.state.id =
          active ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
                 : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
      return result;
    };
    message.controllers.push_back(row("arm_controller", arm_active_));
    if (gripper_loaded_)
      message.controllers.push_back(row("gripper_controller", gripper_active_));
    message.hardware_components.push_back(row("TestSystem", true));
    activity_->publish(message);
  }
  fake::FakeJtc &jtc_;
  mutable std::mutex mutex_;
  bool arm_active_{false}, gripper_active_{false}, gripper_loaded_{true},
      refuse_{false};
  std::string refusal_message_;
  std::vector<Switch::Request> requests_;
  rclcpp::Publisher<controller_manager_msgs::msg::ControllerManagerActivity>::
      SharedPtr activity_;
  rclcpp::Service<Switch>::SharedPtr switch_;
  rclcpp_action::Server<Gripper>::SharedPtr action_;
};

core::RuntimeParticipantBinding GripperBinding() {
  auto value = ArmBinding().value();
  value.target_role_id = core::TargetRoleId("gripper");
  value.command_target_id = core::CommandTargetId("gripper.target");
  value.capability_id = core::CapabilityId("tool.gripper.command");
  value.model_groups = {{core::ModelGroupRoleId("gripper_group"),
                         core::ModelInstanceId("robot.model"),
                         "gripper",
                         {"gripper_joint"}}};
  value.resources = {
      {core::ResourceRoleId("fingers"), core::ResourceId("robot.gripper.1"),
       "source_gripper_joint", "gripper_joint", core::CoordinateKind::kLinear}};
  value.control_path->control_domain_id =
      core::ControlDomainId("domain.gripper");
  value.control_path->control_binding_id =
      core::ControlBindingId("binding.gripper");
  value.control_path->control_interface_contract_id =
      core::ControlInterfaceContractId("interface.gripper");
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok())
    throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

struct GripperWorld final {
  explicit GripperWorld(bool expose_action = true,
                        std::shared_ptr<core::IClock> receiver_clock = {},
                        bool controller_manager_gripper = true,
                        std::optional<core::StreamDeclarationDTO>
                            gripper_feedback_stream = std::nullopt,
                        bool isaac_effort_transport = false)
      : owning_clock(receiver_clock ? std::move(receiver_clock)
                                    : std::make_shared<WallClock>()),
        clock(*owning_clock),
        prefix("/manyforge_gripper_handle_test_" + fake::unique_suffix()),
        jtc(prefix + "/arm_controller", {"source_joint_1"}),
        manager(prefix, jtc, expose_action) {
    auto sink = std::make_shared<NullSink>();
    auto resources =
        std::make_unique<ros2_control_execution::Ros2TransportResources>(
            "manyforge_gripper_handle_" + fake::unique_suffix(), "/", 3U, sink,
            "arm", clock);
    auto gripper =
        std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
            *resources, sink, clock, scheduler, true);
    gripper_backend = gripper.get();
    ros2_control_execution::Ros2GripperActionConfig config;
    config.state_freshness_timeout_s = 1.0;
    config.action_name = prefix + "/gripper_controller/gripper_cmd";
    config.group_id = "gripper";
    config.joint_name = "gripper_joint";
    if (isaac_effort_transport) {
      // The Isaac Lite gripper's direct effort transport: no action server.
      config.transport_kind = "isaac_joint_state_effort";
      config.action_name.clear();
      config.command_topic = prefix + "/isaac_joint_commands";
      config.command_joint_names = {"source_gripper_joint"};
      config.completion_policy = "dwell_elapsed";
      config.open_position = 0.7;
      config.closed_position = 0.0;
      config.open_effort = 5.0;
      config.close_effort = -5.0;
    }
    gripper->configure_gripper_action("gripper.target", config);
    RetainedRos2ControlArmTransport arm;
    arm.adapter.action_name =
        prefix + "/arm_controller/follow_joint_trajectory";
    arm.adapter.state_topic = prefix + "/arm_controller/controller_state";
    arm.adapter.scale_topic = prefix + "/arm_controller/speed_scaling_input";
    arm.adapter.lane_facts.joint_names = {"source_joint_1"};
    arm.adapter.lane_facts.supports_scale = true;
    arm.adapter.lane_facts.state_rate_hz = 100.0;
    arm.adapter.lane_facts.feedback_rate_hz = 100.0;
    arm.adapter.lane_facts.controller_update_rate_hz = 100.0;
    arm.compiled_joint_names = {"joint_1"};
    arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
        prefix + "/joint_states", 100,
        core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
        std::string(kMeasuredJointStateProfileId),
        core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck,
        std::move(gripper_feedback_stream)};
    arm.controller = {prefix + "/controller_manager", "arm_controller",
                      "TestSystem"};
    if (controller_manager_gripper) {
      arm.gripper_controller = RetainedRos2ControlArmController{
          prefix + "/controller_manager", "gripper_controller", "TestSystem"};
    }
    const auto bound = handle->Bind(
        std::move(resources), clock, std::move(arm),
        [this](std::shared_ptr<core::IExecutionPort> port) {
          core::ExecutionPortParticipantConfig config;
          config.stop.trip_authority = core::StateAuthorityClass::kCommanded;
          config.stop.stop_traj_delta_s = 0.1;
          config.stop.sample_period_s = 0.01;
          config.stop.replacement_start_allowance_s = 0.02;
          config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
          config.late_cycle_margin_s = 0.5;
          return std::make_shared<core::ExecutionPortParticipant>(
              motion, std::move(port), clock, scheduler,
              [](core::TimePoint, const std::stop_token &) {
                std::this_thread::sleep_for(1ms);
              },
              core::ExecutionPortParticipantStateAuthority{
                  &publication, [] { return core::Result<void>::Ok(); }},
              std::move(config));
        },
        std::move(gripper), motion, tool);
    if (!bound.ok())
      throw std::logic_error(bound.error().message);
  }
  std::shared_ptr<core::IClock> owning_clock;
  core::IClock &clock;
  SimpleScheduler scheduler;
  core::RuntimeParticipantBinding motion{ArmBinding()}, tool{GripperBinding()};
  core::ObservedRobotStatePublication publication{
      {"joint_1"},
      {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState,
          "commanded_state",
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}}};
  std::string prefix;
  fake::FakeJtc jtc;
  GripperControllerManager manager;
  ros2_control_execution::Ros2GripperCommandBackend *gripper_backend{};
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle =
      RetainedRos2ControlRuntimeHandle::Create();
};

// CONTRACTS §2: an Enable or Disable the gripper controller already
// satisfies succeeds at once; the controller manager would refuse the switch.
TEST(RetainedRos2ControlRuntimeHandleTest, AnAlreadySatisfiedGripperSwitchSendsNothing) {
  using Access = RetainedRos2ControlRuntimeHandleTestAccess;
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return Access::EndpointsSettled(*world.handle); }));
  // Until the switch service is discovered the request is not sent at all.
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->SwitchGripperController(true, {}, Soon()).outcome ==
           Outcome::kSucceeded;
  }));
  ASSERT_TRUE(fake::WaitUntil([&] { return Access::GripperActive(*world.handle); }));
  const auto sent = world.manager.Requests().size();
  EXPECT_EQ(world.handle->SwitchGripperController(true, {}, Soon()).outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(world.manager.Requests().size(), sent);
  // Prepare on the gripper this host enabled is refused, typed, nothing sent.
  const auto prepare_enabled = world.handle->PrepareGripperController({}, Soon());
  EXPECT_EQ(prepare_enabled.outcome, Outcome::kNotSent);
  ASSERT_TRUE(prepare_enabled.error.has_value());
  EXPECT_EQ(prepare_enabled.error->code, "component_enabled");
  EXPECT_EQ(world.manager.Requests().size(), sent);
  EXPECT_TRUE(Access::GripperActive(*world.handle));
  ASSERT_EQ(world.handle->SwitchGripperController(false, {}, Soon()).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil([&] { return !Access::GripperActive(*world.handle); }));
  EXPECT_EQ(world.handle->SwitchGripperController(false, {}, Soon()).outcome,
            Outcome::kSucceeded);
  EXPECT_EQ(world.manager.Requests().size(), sent + 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetPublishesOnceAndRotatesBeforeSuccessorActivation) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_active; }));
  const auto predecessor = world.handle->current_port();
  std::vector<MockResetPositionPhase> phases;
  const auto reset = world.handle->ResetMockPosition(
      {}, [&](const auto &transition, const auto &) {
        phases.push_back(transition.phase);
        EXPECT_EQ(transition.predecessor_epoch,
                  core::ExecutionBindingEpoch(1U));
        if (transition.phase == MockResetPositionPhase::kSwitchingToWriter &&
            transition.switch_acknowledged) {
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.handle->mock_reset_writer_active(); }));
        }
        if (transition.phase == MockResetPositionPhase::kSettingState &&
            transition.target_published) {
          EXPECT_TRUE(world.manager.writer_active());
          EXPECT_FALSE(predecessor->ReadReadiness().controller_bound);
          EXPECT_FALSE(transition.successor_epoch);
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.manager.reset_commands().size() == 1U; }));
          EXPECT_EQ(world.manager.switch_requests(), 2U);
          // Publishing is only buffering; the owner must acknowledge actual
          // consumption before this callback permits writer deactivation.
          core::TrajectoryDTO hold;
          hold.joint_names = {"joint_1"};
          const core::ExecutionGoalKey key{
              core::ControllerGoalId("goal.reset-gated"),
              core::ControllerGoalGeneration(1U)};
          EXPECT_EQ(predecessor->Submit(key, hold).refusal,
                    core::ExecutionPortRefusal::kNotReady);
          EXPECT_EQ(predecessor
                        ->Replace(key,
                                  {core::ControllerGoalId("goal.reset-gated"),
                                   core::ControllerGoalGeneration(2U)},
                                  hold)
                        .refusal,
                    core::ExecutionPortRefusal::kNotReady);
          // Containment cancellation bypasses the pending-motion gate.
          const auto cancelled = predecessor->Cancel(key);
          if (cancelled.error) {
            const auto reason = cancelled.error->details.find("reason");
            EXPECT_TRUE(reason == cancelled.error->details.end() ||
                        reason->second != "mock_reset_position_pending");
          }
        }
        if (transition.phase == MockResetPositionPhase::kSwitchingToJtc) {
          EXPECT_EQ(transition.successor_epoch,
                    core::ExecutionBindingEpoch(2U));
          if (!transition.switch_acknowledged) {
            EXPECT_EQ(world.handle->current_port(),
                      predecessor); // Authorization precedes rotation.
          } else {
            EXPECT_NE(world.handle->current_port(), predecessor);
            EXPECT_TRUE(fake::WaitUntil(
                [&] { return world.handle->mock_reset_successor_ready(); }));
          }
        }
        return core::Result<void>::Ok();
      });
  ASSERT_EQ(reset.outcome, Outcome::kSucceeded) << SwitchFailureText(reset);
  EXPECT_EQ(phases, (std::vector<MockResetPositionPhase>{
                        MockResetPositionPhase::kSwitchingToWriter,
                        MockResetPositionPhase::kSwitchingToWriter,
                        MockResetPositionPhase::kSettingState,
                        MockResetPositionPhase::kSettingState,
                        MockResetPositionPhase::kSwitchingToJtc,
                        MockResetPositionPhase::kSwitchingToJtc}));
  EXPECT_EQ(world.manager.reset_commands(),
            (std::vector<std::vector<double>>{{0.125}}));
  EXPECT_EQ(world.manager.switch_requests(), 3U);
  const auto switches = world.manager.switch_history();
  ASSERT_EQ(switches.size(), 3U);
  EXPECT_EQ(switches[1].first, (std::vector<std::string>{"reset_writer"}));
  EXPECT_EQ(switches[1].second, (std::vector<std::string>{"arm_controller"}));
  EXPECT_EQ(switches[2].first, (std::vector<std::string>{"arm_controller"}));
  EXPECT_EQ(switches[2].second, (std::vector<std::string>{"reset_writer"}));
  EXPECT_TRUE(world.handle->current_port()->ReadReadiness().controller_bound);
}

/// An enabled arm whose writer (with an echo topic, Isaac) is loaded inactive.
void EnableResetArm(ArmWorld &world) {
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.handle->ActivateArmController({}, Soon()).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().controller_active; }));
}

// RP-1b: on Isaac the same writer sequence runs; the write is acknowledged by
// the topic-based hardware sending the target on its joint commands topic
// (this test plays the hardware). A message without the target's joints is
// another robot's; the echo is read by joint name.
TEST(RetainedRos2ControlRuntimeHandleTest,
     IsaacResetAcknowledgesTheWriteOnTheHardwaresCommandEcho) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true, {}, {}, 3U, 100, true);
  EnableResetArm(world);
  const auto hardware = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_commands", rclcpp::QoS(1).reliable());
  const auto send = [&](std::vector<std::string> names, std::vector<double> positions) {
    sensor_msgs::msg::JointState message;
    message.name = std::move(names);
    message.position = std::move(positions);
    hardware->publish(message);
  };
  // The JTC's hold command, as the hardware sends it every cycle.
  ASSERT_TRUE(fake::WaitUntil([&] {
    send({"source_joint_1"}, {0.5});
    return world.handle->reset_command_echo().has_value();
  }));
  EXPECT_EQ(world.handle->reset_command_echo()->positions, (std::vector<double>{0.5}));

  std::vector<std::pair<MockResetPositionPhase, bool>> steps;
  std::uint64_t baseline = 0U;
  const auto reset = world.handle->ResetMockPosition(
      {}, [&](const auto &transition, const auto &) -> core::Result<void> {
        steps.emplace_back(transition.phase, transition.switch_acknowledged);
        if (transition.phase == MockResetPositionPhase::kSettingState) {
          if (!transition.target_published) {
            baseline = world.handle->reset_command_echo()->sequence;
            return core::Result<void>::Ok();
          }
          EXPECT_TRUE(world.manager.writer_active());
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.manager.reset_commands().size() == 1U; }));
          const auto published_at = world.clock.now();
          const std::vector<double> target{0.125};
          const auto acknowledged = [&] {
            const auto echo = world.handle->reset_command_echo();
            return echo && host_detail::ExactResetCommandEcho(*echo, target, baseline,
                                                              published_at);
          };
          send({"other_joint"}, {0.125});
          EXPECT_FALSE(acknowledged());
          EXPECT_TRUE(fake::WaitUntil([&] {
            send({"other_joint", "source_joint_1"}, {9.0, world.manager.reset_commands().front().front()});
            return acknowledged();
          }));
        }
        if (transition.switch_acknowledged) {
          EXPECT_TRUE(fake::WaitUntil([&] {
            return transition.phase == MockResetPositionPhase::kSwitchingToWriter
                       ? world.handle->mock_reset_writer_active()
                       : world.handle->mock_reset_successor_ready();
          }));
        }
        return core::Result<void>::Ok();
      });
  ASSERT_EQ(reset.outcome, Outcome::kSucceeded) << SwitchFailureText(reset);
  using Phase = MockResetPositionPhase;
  EXPECT_EQ(steps, (std::vector<std::pair<Phase, bool>>{{Phase::kSwitchingToWriter, false},
                                                       {Phase::kSwitchingToWriter, true},
                                                       {Phase::kSettingState, false},
                                                       {Phase::kSettingState, false},
                                                       {Phase::kSwitchingToJtc, false},
                                                       {Phase::kSwitchingToJtc, true}}));
  // One publication of the Deployment's initial joints to the writer.
  EXPECT_EQ(world.manager.reset_commands(), (std::vector<std::vector<double>>{{0.125}}));
  const auto switches = world.manager.switch_history();
  ASSERT_EQ(switches.size(), 3U);
  EXPECT_EQ(switches[1].first, (std::vector<std::string>{"reset_writer"}));
  EXPECT_EQ(switches[1].second, (std::vector<std::string>{"arm_controller"}));
  EXPECT_EQ(switches[2].first, (std::vector<std::string>{"arm_controller"}));
  EXPECT_EQ(switches[2].second, (std::vector<std::string>{"reset_writer"}));
  EXPECT_EQ(world.handle->reset_command_echo()->positions, (std::vector<double>{0.125}));
}

// RP-1b: the adapter declares `reset_position` with its writer; a switch the
// controller manager refuses ends the action failed as the component's
// refusal, the writer contained and no target sent.
TEST(Ros2ControlComponentAdapterTest, AResetWhoseSwitchIsRefusedFailsWithTheRefusal) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true, {}, {}, 3U, 100, true);
  EnableResetArm(world);
  Ros2ControlComponentAdapter adapter(
      world.handle, std::chrono::seconds(30),
      [](const auto &, const auto &) { return core::Result<void>::Ok(); });
  EXPECT_EQ(adapter.capabilities(),
            (std::vector<core::Capability>{core::Capability::kResetPosition}));
  world.manager.SetWriterActivationRefused(true);
  const auto failed = adapter.Run(core::ComponentAction::kResetPosition, {});
  EXPECT_EQ(failed.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  // The handle's refusal (reason `ros2_control_activation_refused`) is the
  // component's refusal, with the manager's message.
  EXPECT_EQ(failed.error->code, "component_action_refused");
  EXPECT_NE(failed.error->message.find("refused the controller switch"), std::string::npos);
  EXPECT_TRUE(world.manager.reset_commands().empty());
  const auto switches = world.manager.switch_history();
  ASSERT_EQ(switches.size(), 3U);
  EXPECT_EQ(switches[1].first, (std::vector<std::string>{"reset_writer"}));
  EXPECT_TRUE(switches[2].first.empty());  // containment: the writer off
  EXPECT_EQ(switches[2].second, (std::vector<std::string>{"reset_writer"}));
  EXPECT_FALSE(world.manager.writer_active());
}

// SX-291/SX-297: a lifecycle switch's answer is awaited for the lifecycle
// action's remaining command time; one the controller manager accepts
// but never answers ends as an unknown effect, never a success.
struct UnansweredSwitchWorld final {
  UnansweredSwitchWorld() {
    world.handle->StartBackend();
    if (!fake::WaitUntil([&] {
          const auto activity = world.handle->arm_activity();
          return activity.observed && activity.controller_loaded &&
                 activity.hardware_active &&
                 RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(
                     *world.handle);
        }) ||
        !world.handle->node()
             ->create_client<controller_manager_msgs::srv::SwitchController>(
                 world.prefix + "/controller_manager/switch_controller")
             ->wait_for_service(10s))
      throw std::logic_error("no controller manager");
    world.manager.SetSwitchRepliesPaused(true);
  }
  ~UnansweredSwitchWorld() { world.manager.SetSwitchRepliesPaused(false); }

  [[nodiscard]] std::future<RetainedRos2ControlSwitchResult>
  Activate(const core::Duration remaining) {
    return std::async(std::launch::async, [this, remaining] {
      return world.handle->ActivateArmController(
          std::stop_token{}, std::chrono::steady_clock::now() + std::chrono::nanoseconds(remaining));
    });
  }

  ArmWorld world;
};

TEST(RetainedRos2ControlRuntimeHandleTest,
     LifecycleSwitchWithoutAnswerEndsUncertainAtTheTransactionBudget) {
  UnansweredSwitchWorld unanswered;
  const auto started = std::chrono::steady_clock::now();
  auto pending = unanswered.Activate(2'000'000'000); // 2 s remaining
  EXPECT_TRUE(fake::WaitUntil([&] { return unanswered.world.manager.reply_waiting(); }));
  const bool ended = pending.wait_for(10s) == std::future_status::ready;
  const auto elapsed = std::chrono::steady_clock::now() - started;
  unanswered.world.manager.SetSwitchRepliesPaused(false);
  ASSERT_TRUE(ended);
  const auto outcome = pending.get();
  EXPECT_GE(elapsed, 1900ms);
  EXPECT_LT(elapsed, 8s);
  EXPECT_EQ(outcome.outcome, Outcome::kUncertain);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->details.at("reason"), "ros2_control_switch_timeout");
  // One request, never reported as a success (nothing is retried).
  EXPECT_EQ(unanswered.world.manager.switch_requests(), 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetLostSwitchReplyNeverUsesActivityAsSuccess) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_active; }));
  world.manager.SetSwitchRepliesPaused(true);
  std::stop_source cancellation;
  std::atomic<bool> saw_positive_ack{false};
  auto operation = std::async(std::launch::async, [&] {
    return world.handle->ResetMockPosition(
        cancellation.get_token(),
        [&](const auto &transition, const auto &) {
          if (transition.switch_acknowledged)
            saw_positive_ack = true;
          return core::Result<void>::Ok();
        });
  });
  const bool dispatched =
      fake::WaitUntil([&] { return world.manager.reply_waiting(); });
  EXPECT_TRUE(dispatched);
  EXPECT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_active(); }));
  cancellation.request_stop();
  const bool answered = operation.wait_for(2s) == std::future_status::ready;
  EXPECT_TRUE(answered);
  // Release the fake before any fatal assertion, including failure teardown.
  world.manager.SetSwitchRepliesPaused(false);
  const auto reset = operation.get();
  EXPECT_EQ(reset.outcome, Outcome::kUncertain);
  EXPECT_FALSE(saw_positive_ack.load());
  EXPECT_TRUE(world.manager.reset_commands().empty());
  EXPECT_FALSE(world.handle->current_port()->ReadReadiness().controller_bound);
  ASSERT_TRUE(reset.error);
  EXPECT_NE(reset.error->message.find("Restart the workbench"),
            std::string::npos);
}

TEST(
    RetainedRos2ControlRuntimeHandleTest,
    MockResetLostReplyHasTheStockOneSecondFaultBoundWithoutCallerCancellation) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_active; }));
  world.manager.SetSwitchRepliesPaused(true);
  std::atomic<bool> saw_positive_ack{false};
  auto operation = std::async(std::launch::async, [&] {
    return world.handle->ResetMockPosition(
        {}, [&](const auto &transition, const auto &) {
          if (transition.switch_acknowledged)
            saw_positive_ack = true;
          return core::Result<void>::Ok();
        });
  });
  const bool dispatched =
      fake::WaitUntil([&] { return world.manager.reply_waiting(); });
  EXPECT_TRUE(dispatched);
  const bool bounded = operation.wait_for(3s) == std::future_status::ready;
  world.manager.SetSwitchRepliesPaused(
      false); // Unblock fake teardown even if the assertion fails.
  EXPECT_TRUE(bounded);
  const auto reset = operation.get();
  EXPECT_EQ(reset.outcome, Outcome::kUncertain);
  ASSERT_TRUE(reset.error);
  EXPECT_EQ(reset.error->details.at("reason"), "ros2_control_switch_timeout");
  EXPECT_FALSE(saw_positive_ack.load());
  EXPECT_TRUE(world.manager.reset_commands().empty());
  EXPECT_FALSE(world.handle->current_port()->ReadReadiness().controller_bound);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetRefusesUnsupportedOrUnauthorizedBeforeEffects) {
  ArmWorld unsupported;
  EXPECT_EQ(unsupported.handle
                ->ResetMockPosition({},
                                    [](const auto &, const auto &) {
                                      return core::Result<void>::Ok();
                                    })
                .outcome,
            Outcome::kNotSent);
  EXPECT_EQ(unsupported.manager.switch_requests(), 0U);
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_active; }));
  const auto reset =
      world.handle->ResetMockPosition({}, [](const auto &, const auto &) {
        return core::Result<void>::Err(
            {"execution_rejected", "foreign Reset claim", {}});
      });
  EXPECT_EQ(reset.outcome, Outcome::kNotSent);
  EXPECT_TRUE(world.handle->current_port()->ReadReadiness().controller_bound);
  EXPECT_EQ(world.manager.switch_requests(), 1U);
  EXPECT_TRUE(world.manager.reset_commands().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     SharedUnitPeerReplacementAndAddPreserveTheSurvivorsOriginalOwner) {
  ArmWorld survivor;
  auto peer_value = ArmBinding().value();
  peer_value.controllable_component_id = core::ControllableComponentId("robot_2");
  peer_value.resources.front().resource_id = core::ResourceId("peer.joint.1");
  peer_value.control_path->control_domain_id = core::ControlDomainId("domain.peer");
  peer_value.control_path->control_binding_id = core::ControlBindingId("binding.peer");
  peer_value.control_path->provider_instance_id = core::ProviderInstanceId("provider.peer");
  peer_value.control_path->state_authority_id = core::StateAuthorityId("state.peer");
  auto peer_binding = core::verify_runtime_participant_binding(peer_value);
  ASSERT_TRUE(peer_binding.ok());
  auto peer = std::make_unique<ArmWorld>("arm_controller", nullptr, peer_binding.value());
  const auto port = survivor.handle->current_port();
  const auto participant = survivor.handle->participant();
  const auto readiness = port->ReadReadiness();

  // Replace the peer occurrence inside the same shared runtime unit. This
  // must not rewrite the survivor's component/provider epoch or live port.
  auto retired_peer = std::move(peer);
  const auto replaced_peer_port = retired_peer->handle->current_port();
  peer = std::make_unique<ArmWorld>("arm_controller", nullptr, peer_binding.value());
  EXPECT_NE(peer->handle->current_port(), replaced_peer_port);
  EXPECT_NE(peer->handle, retired_peer->handle);
  auto candidate_value = survivor.binding.value();
  candidate_value.retained_bundle_id = core::RetainedBundleId("replace.bundle");
  candidate_value.graph_revision = core::GraphRevision("replace.graph");
  candidate_value.control_path->active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(3U);
  auto candidate = core::verify_runtime_participant_binding(candidate_value);
  ASSERT_TRUE(candidate.ok());
  // This is an ordinary route publication, not a product-only reseal. The
  // existing strict predicate correctly distinguishes generation 1 -> 3.
  EXPECT_EQ(retained_product_reseal_physical_owner_difference(
                survivor.binding, candidate.value()),
            "control_path.active_control_configuration_generation");
  EXPECT_FALSE(retained_bindings_preserve_physical_owner_after_product_reseal(
      survivor.binding, candidate.value()));
  auto rebound = survivor.handle->RebindMotionRoute(candidate.value(), {});
  ASSERT_TRUE(rebound.ok()) << rebound.error().message;
  EXPECT_TRUE(survivor.handle->OwnsBinding(candidate.value()));
  EXPECT_TRUE(survivor.handle->OwnsBackend(participant.get(), candidate.value(), std::nullopt));
  EXPECT_EQ(survivor.handle->current_port(), port);
  EXPECT_EQ(port->ReadReadiness(), readiness);
  EXPECT_TRUE(peer->handle->OwnsBinding(peer_binding.value()));

  // Remove/Add the peer again. The real candidate route generation is
  // retained, while the survivor still names its original occurrence.
  peer.reset();
  peer = std::make_unique<ArmWorld>("arm_controller", nullptr, peer_binding.value());
  const auto replaced_binding = candidate.value();
  candidate_value.retained_bundle_id = core::RetainedBundleId("add.bundle");
  candidate_value.graph_revision = core::GraphRevision("add.graph");
  candidate_value.control_path->active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(4U);
  candidate = core::verify_runtime_participant_binding(candidate_value);
  ASSERT_TRUE(candidate.ok());
  rebound = survivor.handle->RebindMotionRoute(candidate.value(), {});
  ASSERT_TRUE(rebound.ok()) << rebound.error().message;
  EXPECT_TRUE(survivor.handle->OwnsBinding(candidate.value()));
  EXPECT_FALSE(survivor.handle->OwnsBinding(replaced_binding));
  EXPECT_EQ(survivor.handle->current_port(), port);
  EXPECT_EQ(survivor.handle->participant(), participant);
  EXPECT_EQ(port->ReadReadiness(), readiness);
  EXPECT_EQ(survivor.manager.switch_requests(), 0U);
  EXPECT_EQ(peer->manager.switch_requests(), 0U);

  // Route-generation permission never crosses the survivor's owner epoch,
  // active configuration identity, physical component, or selected route.
  const std::vector<std::pair<std::string, std::function<void(core::RuntimeParticipantBindingDTO &)>>>
      foreign_facts{
          {"control_path.provider_runtime_epoch", [](auto &value) {
             value.control_path->provider_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
             value.runtime_unit_epochs.front().runtime_epoch = core::RuntimeEpoch("foreign.epoch");
           }},
          {"control_path.active_control_configuration_id", [](auto &value) {
             value.control_path->active_control_configuration_id =
                 core::ActiveControlConfigurationId("foreign.configuration");
           }},
          {"controllable_component_id", [](auto &value) {
             value.controllable_component_id = core::ControllableComponentId("robot_2");
           }},
          {"control_path.control_route_id", [](auto &value) {
             value.control_path->control_route_id = core::ControlRouteId("foreign.route");
           }}};
  for (const auto &[field, mutate] : foreign_facts) {
    SCOPED_TRACE(field);
    auto foreign_value = candidate.value().value();
    mutate(foreign_value);
    auto foreign = core::verify_runtime_participant_binding(std::move(foreign_value));
    ASSERT_TRUE(foreign.ok());
    const auto refused = survivor.handle->RebindMotionRoute(foreign.value(), {});
    ASSERT_FALSE(refused.ok());
    EXPECT_NE(refused.error().message.find(field), std::string::npos);
    EXPECT_TRUE(survivor.handle->OwnsBinding(candidate.value()));
    EXPECT_EQ(survivor.handle->current_port(), port);
  }
  // The publication owner's existing rollback also retains the exact old
  // generation; no adapter activation/epoch rotation occurs here.
  EXPECT_TRUE(survivor.handle->RebindMotionRoute(survivor.binding, {}).ok());
  EXPECT_TRUE(survivor.handle->OwnsBinding(survivor.binding));
  EXPECT_EQ(survivor.handle->current_port(), port);
}

// --- Q54: controller statistics monitor (observed statistics only)

[[nodiscard]] pal_statistics_msgs::msg::Statistics
Statistics(const std::vector<std::pair<std::string, double>> &values) {
  pal_statistics_msgs::msg::Statistics message;
  for (const auto &[name, value] : values) {
    pal_statistics_msgs::msg::Statistic statistic;
    statistic.name = name;
    statistic.value = value;
    message.statistics.push_back(statistic);
  }
  return message;
}

TEST(RetainedControllerStatisticsMonitorTest, ReportsSinceActivationWorstPeriodAndThrottlesItsWarning) {
  core::FakeClock clock;
  clock.set(1'000'000'000);
  std::vector<std::string> warnings;
  RetainedControllerStatisticsMonitor monitor(
      {"/cm", "arm_controller", 100.0}, clock,
      [&](const std::string &text) { warnings.push_back(text); });
  auto snapshot = monitor.Read();
  EXPECT_FALSE(snapshot.observed);
  EXPECT_DOUBLE_EQ(snapshot.warning_margin_period_s, 0.02);  // two 10 ms periods
  EXPECT_FALSE(snapshot.receipt_age_ns.has_value());

  // Within the margin: 95 Hz worst since activation (10.5 ms).
  monitor.Observe(Statistics({{"arm_controller.stats/periodicity/min", 95.0},
                              {"arm_controller.stats/periodicity/sample_count", 1200.0},
                              {"other_controller.stats/periodicity/min", 1.0}}),
                  clock.now());
  clock.advance(5'000'000);
  snapshot = monitor.Read();
  EXPECT_TRUE(snapshot.observed);
  EXPECT_DOUBLE_EQ(*snapshot.since_activation_min_hz, 95.0);
  EXPECT_DOUBLE_EQ(*snapshot.since_activation_worst_period_s, 1.0 / 95.0);
  EXPECT_DOUBLE_EQ(*snapshot.since_activation_sample_count, 1200.0);
  EXPECT_EQ(*snapshot.receipt_age_ns, 5'000'000);
  EXPECT_EQ(snapshot.observed_statistics_updates, 1U);
  EXPECT_EQ(snapshot.observed_updates_beyond_margin, 0U);
  EXPECT_TRUE(warnings.empty());

  // Beyond the margin (40 Hz: 25 ms > 20 ms): one throttled warning per second
  // of observed updates, each labelled as observed statistics.
  for (int update = 0; update < 5; ++update) {
    monitor.Observe(Statistics({{"arm_controller.stats/periodicity/min", 40.0}}), clock.now());
    clock.advance(100'000'000);
  }
  ASSERT_EQ(warnings.size(), 1U);
  EXPECT_NE(warnings.front().find("observed controller statistics"), std::string::npos);
  EXPECT_NE(warnings.front().find("not late-cycle counts"), std::string::npos);
  clock.advance(1'000'000'000);
  monitor.Observe(Statistics({{"arm_controller.stats/periodicity/min", 40.0}}), clock.now());
  EXPECT_EQ(warnings.size(), 2U);
  snapshot = monitor.Read();
  EXPECT_EQ(snapshot.observed_statistics_updates, 7U);
  EXPECT_EQ(snapshot.observed_updates_beyond_margin, 6U);
  EXPECT_DOUBLE_EQ(*snapshot.since_activation_worst_period_s, 0.025);

  // After a reactivation the controller manager resets the collector: no
  // cycle yet (NaN) clears the worst period instead of keeping an old one.
  monitor.Observe(Statistics({{"arm_controller.stats/periodicity/min",
                               std::numeric_limits<double>::quiet_NaN()}}),
                  clock.now());
  EXPECT_FALSE(monitor.Read().since_activation_worst_period_s.has_value());
  EXPECT_THROW(RetainedControllerStatisticsMonitor({"/cm", "arm_controller", 0.0}, clock, {}),
               std::invalid_argument);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ControllerStatisticsAreObservedFromTheControllerManagerWithoutTimingAuthority) {
  ArmWorld world;
  EXPECT_FALSE(world.handle->controller_statistics().has_value());
  world.handle->StartBackend();
  // A stand-in for the controller manager's statistics publisher.
  const auto publisher = world.manager.node()->create_publisher<pal_statistics_msgs::msg::Statistics>(
      world.prefix + "/controller_manager/statistics/full", rclcpp::QoS(10));
  ASSERT_TRUE(fake::WaitUntil([&] {
    publisher->publish(Statistics({{"arm_controller.stats/periodicity/min", 40.0},
                                   {"arm_controller.stats/periodicity/sample_count", 300.0}}));
    const auto observed = world.handle->controller_statistics();
    return observed.has_value() && observed->observed_statistics_updates >= 3U;
  }));
  const auto observed = world.handle->controller_statistics();
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->controller_name, "arm_controller");
  EXPECT_DOUBLE_EQ(*observed->since_activation_min_hz, 40.0);
  EXPECT_DOUBLE_EQ(*observed->since_activation_worst_period_s, 0.025);
  EXPECT_DOUBLE_EQ(*observed->since_activation_sample_count, 300.0);
  EXPECT_GE(observed->observed_updates_beyond_margin, 3U);
  ASSERT_TRUE(observed->receipt_age_ns.has_value());
  EXPECT_LT(*observed->receipt_age_ns, 2'000'000'000);
  // Monitor only: the port's controller-period facts stay absent.
  const auto timing = world.handle->current_port()->ReadTiming();
  EXPECT_FALSE(timing.period_mean_s.has_value());
  EXPECT_FALSE(timing.period_min_s.has_value());
  EXPECT_FALSE(timing.period_max_s.has_value());
  EXPECT_FALSE(timing.period_stddev_s.has_value());
}

// Use the production constructor over the fixture's sealed physical binding
// and its explicit test lane budget, rather than invent a second provider.
[[nodiscard]] core::MeasuredStateSourceDescriptor
MeasuredSourceDescriptor(const core::RuntimeParticipantBinding &binding,
                         const std::string &prefix, core::Duration budget_ns = 100) {
  const auto &path = *binding.value().control_path;
  core::ActiveControlConfigurationDTO configuration{
      path.active_control_configuration_id,
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
  const auto descriptor = make_retained_ros2_control_measured_state_descriptor(
      binding, configuration,
      RetainedMeasuredJointStateLane{
          prefix + "/joint_states", budget_ns,
          core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
          std::string(kMeasuredJointStateProfileId),
          core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck, std::nullopt});
  if (!descriptor.ok())
    throw std::logic_error(descriptor.error().message);
  return descriptor.value();
}

[[nodiscard]] core::MeasuredStateSourceDescriptor
MeasuredSourceDescriptor(const ArmWorld &world) {
  return MeasuredSourceDescriptor(world.binding, world.prefix);
}

[[nodiscard]] core::RuntimeParticipantBinding
MeasuredRouteSuccessor(const core::RuntimeParticipantBinding &binding) {
  auto value = binding.value();
  auto &path = *value.control_path;
  path.active_control_configuration_id =
      core::ActiveControlConfigurationId("active.successor");
  path.control_route_id = core::ControlRouteId("route.successor");
  path.control_binding_revision =
      core::ControlBindingRevision("binding-successor-revision");
  path.activation_set_id = core::ActivationSetId("activation.successor");
  path.active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(2U);
  auto candidate = core::verify_runtime_participant_binding(std::move(value));
  if (!candidate.ok())
    throw std::logic_error(candidate.error().message);
  return std::move(candidate.value());
}

TEST(RetainedRos2ControlRuntimeHandle,
     MeasuredSourceCoversActualArmAndToolCoordinates) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  GripperWorld world(true, clock);
  const auto motion = MeasuredSourceDescriptor(world.motion, world.prefix);
  const auto tool = MeasuredSourceDescriptor(world.tool, world.prefix);
  const auto staged = world.handle->StageMeasuredStateSources(
      {motion, tool}, clock, world.motion, world.tool);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  ASSERT_EQ(staged.value()->readers().size(), 2U);
  const auto arm_reader = staged.value()->readers()[0];
  const auto tool_reader = staged.value()->readers()[1];
  staged.value()->EnablePublication();
  auto publisher =
      world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
          world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return publisher->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.name = {"source_gripper_joint", "source_joint_1"};
  message.position = {0.025, 0.25};
  publisher->publish(message);
  const auto &arm_query = motion.publication.query_context;
  const auto &tool_query = tool.publication.query_context;
  ASSERT_TRUE(fake::WaitUntil([&] {
    return arm_reader->current_state(arm_query, 1000).ok() &&
           tool_reader->current_state(tool_query, 1000).ok();
  }));
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(
          arm_reader->current_state(arm_query, 1000).value().positions[0])
          .value_rad,
      0.25);
  EXPECT_DOUBLE_EQ(
      std::get<core::LinearPositionDTO>(
          tool_reader->current_state(tool_query, 1000).value().positions[0])
          .value_m,
      0.025);
  EXPECT_FALSE(arm_reader->current_state(tool_query, 1000).ok());
  EXPECT_FALSE(tool_reader->current_state(arm_query, 1000).ok());
  const auto candidate_motion = MeasuredRouteSuccessor(world.motion);
  const auto candidate_tool = MeasuredRouteSuccessor(world.tool);
  const auto next_motion =
      MeasuredSourceDescriptor(candidate_motion, world.prefix);
  const auto next_tool = MeasuredSourceDescriptor(candidate_tool, world.prefix);
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({next_motion, next_tool}, clock,
                                               candidate_motion, std::nullopt)
                   .ok());
  auto foreign_tool_value = candidate_tool.value();
  foreign_tool_value.control_path->provider_instance_id =
      core::ProviderInstanceId("foreign.tool.provider");
  const auto foreign_tool =
      core::verify_runtime_participant_binding(std::move(foreign_tool_value));
  ASSERT_TRUE(foreign_tool.ok());
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({next_motion, next_tool}, clock,
                                               candidate_motion,
                                               foreign_tool.value())
                   .ok());
  const auto successor = world.handle->StageMeasuredStateSources(
      {next_motion, next_tool}, clock, candidate_motion, candidate_tool);
  ASSERT_TRUE(successor.ok()) << successor.error().message;
  const auto next_arm_reader = successor.value()->readers()[0];
  const auto next_tool_reader = successor.value()->readers()[1];
  EXPECT_TRUE(world.handle->OwnsBinding(world.motion));
  EXPECT_TRUE(world.handle->OwnsBinding(world.tool));
  EXPECT_FALSE(world.handle->OwnsBinding(candidate_motion));
  EXPECT_FALSE(world.handle->OwnsBinding(candidate_tool));
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return publisher->get_subscription_count() == 2U; }));
  clock->set(1050);
  message.name = {"source_joint_1"};
  message.position = {0.5};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto envelope = tool_reader->publication(
        tool_query.control_binding_id, tool_query.control_binding_revision);
    return envelope.ok() && envelope.value().received_timestamp_ns == 1050;
  }));
  EXPECT_TRUE(arm_reader->current_state(arm_query, 1050).ok());
  EXPECT_FALSE(tool_reader->current_state(tool_query, 1050).ok());
  const auto &next_arm_query = next_motion.publication.query_context;
  const auto &next_tool_query = next_tool.publication.query_context;
  EXPECT_FALSE(next_arm_reader->current_state(next_arm_query, 1050).ok());
  EXPECT_FALSE(next_tool_reader->current_state(next_tool_query, 1050).ok());
  successor.value()->EnablePublication();
  EXPECT_FALSE(next_arm_reader->current_state(next_arm_query, 1050).ok());
  EXPECT_FALSE(next_tool_reader->current_state(next_tool_query, 1050).ok());
  clock->set(1075);
  message.name = {"source_gripper_joint", "source_joint_1"};
  message.position = {0.04, 0.75};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return next_arm_reader->current_state(next_arm_query, 1075).ok() &&
           next_tool_reader->current_state(next_tool_query, 1075).ok();
  }));
  EXPECT_FALSE(next_arm_reader->current_state(arm_query, 1075).ok());
  EXPECT_FALSE(next_tool_reader->current_state(tool_query, 1075).ok());
  EXPECT_TRUE(world.handle->OwnsBinding(world.motion));
  EXPECT_TRUE(world.handle->OwnsBinding(world.tool));
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandle,
     MockGripperFeedbackStreamRidesTheMeasuredLane) {
  // CP9: a Mock gripper's `gripper_feedback` stream is declared with the
  // measured lane (its row defaults) and recorded by every JointState that
  // carries the gripper joint.
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  GripperWorld world(true, clock, true,
                     core::StreamDeclarationDTO{
                         "gripper_feedback", 1'500'000'000,
                         core::StreamSeverityDTO::kBlocking,
                         core::StreamRecoveryDTO::kAuto,
                         core::StreamTimestampSourceDTO::kMessageStamp});
  const auto motion = MeasuredSourceDescriptor(world.motion, world.prefix);
  const auto tool = MeasuredSourceDescriptor(world.tool, world.prefix);
  const auto staged = world.handle->StageMeasuredStateSources(
      {motion, tool}, clock, world.motion, world.tool);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  const auto arm_reader = staged.value()->readers()[0];
  const auto tool_reader = staged.value()->readers()[1];
  staged.value()->EnablePublication();
  const auto gripper_stream = [&]() -> std::optional<core::StreamHealthDTO> {
    for (const auto &stream : host_component_stream_health()->Read(
             world.tool.value().controllable_component_id))
      if (stream.stream_id == "gripper_feedback") return stream;
    return std::nullopt;
  };
  const auto declared = gripper_stream();
  ASSERT_TRUE(declared.has_value());
  EXPECT_EQ(declared->budget_ns, 1'500'000'000);
  EXPECT_EQ(declared->severity, core::StreamSeverityDTO::kBlocking);
  EXPECT_EQ(declared->recovery, core::StreamRecoveryDTO::kAuto);
  EXPECT_EQ(declared->timestamp_source,
            core::StreamTimestampSourceDTO::kMessageStamp);
  EXPECT_FALSE(declared->age_ns.has_value());
  auto publisher =
      world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
          world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return publisher->get_subscription_count() == 1U; }));
  const auto &arm_query = motion.publication.query_context;
  const auto &tool_query = tool.publication.query_context;
  // A sample without the gripper joint is no gripper feedback.
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 1;
  message.name = {"source_joint_1"};
  message.position = {0.25};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return arm_reader->current_state(arm_query, 1000).ok(); }));
  EXPECT_FALSE(gripper_stream()->age_ns.has_value());
  message.header.stamp.sec = 2;
  message.name = {"source_gripper_joint", "source_joint_1"};
  message.position = {0.025, 0.25};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return tool_reader->current_state(tool_query, 1000).ok(); }));
  EXPECT_TRUE(gripper_stream()->age_ns.has_value());
}

TEST(RetainedRos2ControlRuntimeHandle,
     GripperOnlyToolStagesItsOwnLaneAndDeclaresBothStreams) {
  // Merge 6 (S3): a gripper-only tool with a lane, a Mock pneumatic tool
  // included, stages its own descriptor. The lane declares and feeds the
  // tool's `measured_joints` and `gripper_feedback` streams, as the combined
  // robot's lane did.
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  core::ManualScheduler scheduler;
  const auto tool = GripperBinding();
  const auto prefix = "/manyforge_tool_streams_" + fake::unique_suffix();
  fake::FakeControllerManager manager(prefix + "/controller_manager",
                                       "gripper_controller", "TestSystem", nullptr);
  auto sink = std::make_shared<NullSink>();
  auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
      "manyforge_tool_streams_" + fake::unique_suffix(), "/", 3U,
      sink, "gripper", *clock);
  auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
      *resources, sink, *clock, scheduler, true);
  ros2_control_execution::Ros2GripperActionConfig config;
  config.state_freshness_timeout_s = 1.0;
  config.action_name = prefix + "/gripper_controller/gripper_cmd";
  config.group_id = "gripper";
  config.joint_name = "gripper_joint";
  backend->configure_gripper_action(tool.value().command_target_id.value(), config);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->BindGripperOnly(std::move(resources), *clock,
      std::move(backend), tool,
      {prefix + "/controller_manager", "gripper_controller", "TestSystem"},
      clock, RetainedMeasuredJointStateLane{
          prefix + "/joint_states", 250'000'000,
          core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
          std::string(kMeasuredJointStateProfileId),
          core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck,
          core::StreamDeclarationDTO{
              "gripper_feedback", 1'000'000'000, core::StreamSeverityDTO::kFault,
              core::StreamRecoveryDTO::kAck,
              core::StreamTimestampSourceDTO::kMessageStamp}}).ok());
  // The descriptor carries the bound lane's own 250 ms budget (the stage
  // refuses a source whose freshness differs from the bound lane).
  const auto descriptor = MeasuredSourceDescriptor(tool, prefix, 250'000'000);
  const auto staged = handle->StageMeasuredStateSources(
      {descriptor}, clock, std::nullopt, tool);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  ASSERT_NE(staged.value(), nullptr);
  staged.value()->EnablePublication();
  const auto stream = [&](std::string_view id) -> std::optional<core::StreamHealthDTO> {
    for (const auto &row : host_component_stream_health()->Read(
             tool.value().controllable_component_id))
      if (row.stream_id == id) return row;
    return std::nullopt;
  };
  ASSERT_TRUE(stream("measured_joints").has_value());
  ASSERT_TRUE(stream("gripper_feedback").has_value());
  EXPECT_EQ(stream("measured_joints")->budget_ns, 250'000'000);
  EXPECT_FALSE(stream("gripper_feedback")->age_ns.has_value());
  auto samples = manager.node()->create_publisher<sensor_msgs::msg::JointState>(
      prefix + "/joint_states", rclcpp::QoS(1));
  handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return samples->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 2;
  message.name = {"source_gripper_joint"};
  message.position = {0.02};
  const auto reader = staged.value()->readers().front();
  const auto &query = descriptor.publication.query_context;
  ASSERT_TRUE(fake::WaitUntil([&] {
    samples->publish(message);
    return reader->current_state(query, clock->now()).ok();
  }));
  EXPECT_TRUE(stream("measured_joints")->age_ns.has_value());
  EXPECT_TRUE(stream("gripper_feedback")->age_ns.has_value());
}

TEST(RetainedRos2ControlRuntimeHandle,
     AReplacementInstanceStartsFromAFreshStreamWhileItsPredecessorIsStale) {
  // CP9: a component instance declares its streams when it binds. A
  // replacement of the same component id (Real -> Mock Load) never inherits
  // its predecessor's stale `measured_joints`, even while the predecessor
  // still exists, and the predecessor's later release keeps the new slot.
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  core::ManualScheduler scheduler;
  const auto tool = GripperBinding();
  const auto &component = tool.value().controllable_component_id;
  const auto measured = [&]() -> std::optional<core::StreamHealthDTO> {
    for (const auto &row : host_component_stream_health()->Read(component))
      if (row.stream_id == "measured_joints") return row;
    return std::nullopt;
  };
  struct Instance final {
    std::string prefix;
    std::unique_ptr<fake::FakeControllerManager> manager;
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  };
  const auto bind = [&] {
    Instance made;
    made.prefix = "/manyforge_tool_replacement_" + fake::unique_suffix();
    made.manager = std::make_unique<fake::FakeControllerManager>(
        made.prefix + "/controller_manager", "gripper_controller", "TestSystem", nullptr);
    auto sink = std::make_shared<NullSink>();
    auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
        "manyforge_tool_replacement_" + fake::unique_suffix(), "/", 3U, sink, "gripper",
        *clock);
    auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
        *resources, sink, *clock, scheduler, true);
    ros2_control_execution::Ros2GripperActionConfig config;
    config.state_freshness_timeout_s = 1.0;
    config.action_name = made.prefix + "/gripper_controller/gripper_cmd";
    config.group_id = "gripper";
    config.joint_name = "gripper_joint";
    backend->configure_gripper_action(tool.value().command_target_id.value(), config);
    made.handle = RetainedRos2ControlRuntimeHandle::Create();
    const auto bound = made.handle->BindGripperOnly(
        std::move(resources), *clock, std::move(backend), tool,
        {made.prefix + "/controller_manager", "gripper_controller", "TestSystem"}, clock,
        RetainedMeasuredJointStateLane{
            made.prefix + "/joint_states", 100'000'000,
            core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
            std::string(kMeasuredJointStateProfileId), core::StreamSeverityDTO::kFault,
            core::StreamRecoveryDTO::kAck, std::nullopt});
    if (!bound.ok()) throw std::logic_error(bound.error().message);
    return made;
  };

  auto predecessor = bind();
  // Declared at bind, before any staging: absent, no sample.
  ASSERT_TRUE(measured().has_value());
  EXPECT_EQ(measured()->state, core::StreamLivenessStateDTO::kAbsent);
  const auto descriptor = MeasuredSourceDescriptor(tool, predecessor.prefix, 100'000'000);
  std::shared_ptr<PreparedMeasuredStateSources> sources;
  {
    const auto staged = predecessor.handle->StageMeasuredStateSources(
        {descriptor}, clock, std::nullopt, tool);
    ASSERT_TRUE(staged.ok()) << staged.error().message;
    sources = staged.value();
  }
  sources->EnablePublication();
  auto samples = predecessor.manager->node()->create_publisher<sensor_msgs::msg::JointState>(
      predecessor.prefix + "/joint_states", rclcpp::QoS(1));
  predecessor.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return samples->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.name = {"source_gripper_joint"};
  message.position = {0.02};
  ASSERT_TRUE(fake::WaitUntil([&] {
    ++message.header.stamp.sec;
    samples->publish(message);
    const auto row = measured();
    return row && row->state == core::StreamLivenessStateDTO::kLive;
  }));
  // The samples stop (an e-stop): the predecessor's stream goes stale.
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto row = measured();
    return row && row->state == core::StreamLivenessStateDTO::kStale;
  }));

  // The replacement binds while the predecessor still exists.
  auto replacement = bind();
  const auto fresh = measured();
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(fresh->state, core::StreamLivenessStateDTO::kAbsent);
  EXPECT_FALSE(fresh->age_ns.has_value());
  EXPECT_FALSE(fresh->error.has_value());

  // The predecessor's release (with its staged sources) keeps the new slot.
  sources.reset();
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> retired = predecessor.handle;
  predecessor.handle.reset();
  EXPECT_TRUE(retired.expired());
  const auto kept = measured();
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept->state, core::StreamLivenessStateDTO::kAbsent);
  EXPECT_FALSE(kept->age_ns.has_value());
}

// CONTRACTS §16 / §6: a machine owner's own launch (arm deactivated) is
// settling until its spawners finished, its process exited or its bound
// passed; the owner's Prepare awaits the driver's first report meanwhile.
TEST(RetainedRos2ControlRuntimeHandle, AProcessOnlyOwnersLaunchIsSettlingUntilItsSpawnersFinish) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  const auto prefix = "/manyforge_owner_settle_" + fake::unique_suffix();
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle
                  ->BindProcessOnly(std::make_unique<ros2_control_execution::Ros2TransportResources>(
                                        "manyforge_owner_settle_" + fake::unique_suffix(), "/", 1U),
                                    *clock, {prefix + "/controller_manager", "", "TestSystem"},
                                    clock)
                  .ok());
  const auto exited = std::make_shared<std::optional<std::string>>();
  handle->InstallControllerProcessFaultSource(
      [exited] { return *exited; },
      [exited] { return core::Result<std::optional<std::string>>::Ok(*exited); });
  EXPECT_FALSE(handle->ControllerLaunchSettling());  // no launch installed
  std::atomic<bool> settled{false};
  handle->InstallControllerLaunchSettle([&settled] { return settled.load(); },
                                        std::chrono::steady_clock::now(),
                                        std::chrono::seconds(30));
  handle->StartBackend();
  EXPECT_TRUE(handle->ControllerLaunchSettling());
  *exited = "the launch exited";
  EXPECT_FALSE(handle->ControllerLaunchSettling());
  exited->reset();
  EXPECT_TRUE(handle->ControllerLaunchSettling());
  settled = true;
  EXPECT_FALSE(handle->ControllerLaunchSettling());
  // A driver restart re-points it to the restarted launch.
  std::atomic<bool> restarted{false};
  handle->ReplaceControllerLaunchSettle([&restarted] { return restarted.load(); },
                                        std::chrono::steady_clock::now());
  EXPECT_TRUE(handle->ControllerLaunchSettling());
  restarted = true;
  EXPECT_FALSE(handle->ControllerLaunchSettling());
}

TEST(RetainedRos2ControlRuntimeHandle, AProcessOnlyBindingObservesTheProcessWithoutAnArm) {
  // CONTRACTS §16: a machine's process with its arm deactivated. The owner's
  // lifecycle comes from the process: the controller manager present (not
  // offline), its exit a fault; there is no controller to load.
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  const auto prefix = "/manyforge_process_only_" + fake::unique_suffix();
  const auto resources = [&] {
    return std::make_unique<ros2_control_execution::Ros2TransportResources>(
        "manyforge_process_only_" + fake::unique_suffix(), "/", 1U);
  };
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  // It names no controller.
  EXPECT_FALSE(handle->BindProcessOnly(resources(), *clock,
                                       {prefix + "/controller_manager", "arm", "TestSystem"},
                                       clock).ok());
  ASSERT_TRUE(handle->BindProcessOnly(resources(), *clock,
                                      {prefix + "/controller_manager", "", "TestSystem"}, clock)
                  .ok());
  EXPECT_FALSE(handle->BindProcessOnly(resources(), *clock,
                                       {prefix + "/controller_manager", "", "TestSystem"},
                                       clock).ok());
  const auto observe = [&] {
    return ros2_control_component_state(handle->ReadObservation(clock->now()), clock->now());
  };
  const auto exited = std::make_shared<std::optional<std::string>>();
  handle->InstallControllerProcessFaultSource(
      [exited] { return *exited; },
      [exited] { return core::Result<std::optional<std::string>>::Ok(*exited); });
  handle->StartBackend();
  EXPECT_EQ(observe().lifecycle, core::ComponentLifecycle::kOffline);
  fake::FakeControllerManager manager(prefix + "/controller_manager", "arm_controller",
                                       "TestSystem", nullptr);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return handle->ReadObservation(clock->now()).controller_manager_present; }));
  const auto running = handle->ReadObservation(clock->now());
  EXPECT_FALSE(running.controller_loaded);
  EXPECT_FALSE(running.process_fault.has_value());
  EXPECT_NE(observe().lifecycle, core::ComponentLifecycle::kOffline);
  *exited = "the launch exited";
  EXPECT_EQ(observe().lifecycle, core::ComponentLifecycle::kFault);
}

TEST(RetainedRos2ControlRuntimeHandle, InactiveJointStateSamplesAreMeasuredAndReadsNeverRefresh) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", nullptr, ArmBinding(), clock);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto &query = descriptor.publication.query_context;
  const auto configured = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  ASSERT_EQ(configured.value()->readers().size(), 1U);
  const auto reader = configured.value()->readers().front();
  configured.value()->EnablePublication();
  EXPECT_FALSE(reader->current_state(query, 1000).ok());
  auto publisher = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return publisher->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 2;
  message.header.stamp.nanosec = 3;
  message.name = {"irrelevant_source", "source_joint_1"};
  message.position = {99.0, 0.25};
  message.velocity = {99.0, 0.5};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] { return reader->current_state(query, 1000).ok(); }));
  const auto sampled = reader->current_state(query, 1000);
  ASSERT_TRUE(sampled.ok());
  EXPECT_EQ(sampled.value().joint_names, query.joint_names);
  EXPECT_EQ(sampled.value().resources, query.resources);
  EXPECT_DOUBLE_EQ(std::get<core::AngularPositionDTO>(sampled.value().positions[0]).value_rad, 0.25);
  ASSERT_TRUE(sampled.value().velocities.has_value());
  EXPECT_DOUBLE_EQ(std::get<core::AngularVelocityDTO>((*sampled.value().velocities)[0]).value_rad_s, 0.5);
  EXPECT_EQ(sampled.value().source_kind, core::ObservedStateSourceKind::kJointState);
  EXPECT_EQ(sampled.value().state_authority_class, core::StateAuthorityClass::kMeasured);
  EXPECT_EQ(sampled.value().source_timestamp_ns, 2'000'000'003);
  EXPECT_EQ(sampled.value().received_timestamp_ns, 1000);
  // CP9: every received sample is pushed to the component's stream.
  const auto streams = host_component_stream_health()->Read(
      world.binding.value().controllable_component_id);
  ASSERT_EQ(streams.size(), 1U);
  EXPECT_EQ(streams.front().stream_id, "measured_joints");
  EXPECT_EQ(streams.front().severity, core::StreamSeverityDTO::kFault);
  EXPECT_EQ(streams.front().recovery, core::StreamRecoveryDTO::kAck);
  EXPECT_TRUE(streams.front().age_ns.has_value());
  clock->set(9'000'000);
  EXPECT_EQ(reader->current_state(query, 1050).value(), sampled.value());
  EXPECT_TRUE(reader->current_state(query, 1100).ok());
  EXPECT_FALSE(reader->current_state(query, 1101).ok());
  EXPECT_FALSE(reader->current_state(query, 1102).ok());
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  EXPECT_FALSE(world.handle->arm_activity().controller_active);
  // A real new malformed sample invalidates completeness instead of leaving
  // the earlier complete sample as the apparent current measurement.
  clock->set(9'000'100);
  message.name = {"source_joint_1", "source_joint_1"};
  message.position = {0.1, 0.2};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto envelope = reader->publication(query.control_binding_id, query.control_binding_revision);
    return envelope.ok() && envelope.value().publication_sequence > sampled.value().sequence &&
           !envelope.value().snapshot.complete && envelope.value().received_timestamp_ns == 9'000'100;
  }));
  EXPECT_FALSE(reader->current_state(query, 9'000'100).ok());
  message.name = {"source_joint_1"};
  message.position = {0.75};
  message.velocity.clear();
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] { return reader->current_state(query, 9'000'100).ok(); }));
  EXPECT_FALSE(reader->current_state(query, 9'000'100).value().velocities.has_value());
}

TEST(RetainedRos2ControlRuntimeHandle, MeasuredRenderWakeAdvancesOnlyForVisibleChanges) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", nullptr, ArmBinding(), clock);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto &query = descriptor.publication.query_context;
  const auto wake = std::make_shared<RetainedObservationWake>();
  const auto configured = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt, wake);
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  const auto reader = configured.value()->readers().front();
  configured.value()->EnablePublication();
  auto publisher = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return publisher->get_subscription_count() == 1U; }));
  std::uint64_t received = 0U;
  const auto publish = [&](std::vector<std::string> names, std::vector<double> positions) {
    sensor_msgs::msg::JointState message;
    message.header.stamp.sec = 2;
    message.header.stamp.nanosec = static_cast<std::uint32_t>(received + 1U);
    message.name = std::move(names);
    message.position = std::move(positions);
    publisher->publish(message);
    ASSERT_TRUE(fake::WaitUntil([&] {
      const auto envelope =
          reader->publication(query.control_binding_id, query.control_binding_revision);
      return envelope.ok() && envelope.value().publication_sequence > received;
    }));
    received = reader->publication(query.control_binding_id, query.control_binding_revision)
                   .value().publication_sequence;
  };
  publish({"source_joint_1"}, {0.25});
  ASSERT_TRUE(fake::WaitUntil([&] { return wake->generation() == 1U; }));
  // An identical freshness sample does not wake; the next real change does.
  publish({"source_joint_1"}, {0.25});
  publish({"source_joint_1"}, {0.5});
  ASSERT_TRUE(fake::WaitUntil([&] { return wake->generation() >= 2U; }));
  // Losing completeness is visible once; regaining it is visible again.
  publish({"source_joint_1", "source_joint_1"}, {0.1, 0.2});
  publish({"source_joint_1", "source_joint_1"}, {0.1, 0.2});
  publish({"source_joint_1"}, {0.5});
  ASSERT_TRUE(fake::WaitUntil([&] { return wake->generation() >= 4U; }));
  EXPECT_EQ(wake->generation(), 4U);
}

TEST(RetainedRos2ControlRuntimeHandle,
     StagedMeasuredSuccessorStartsEmptyAndPreservesPinnedSource) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", nullptr, ArmBinding(), clock);
  const auto original = MeasuredSourceDescriptor(world);
  auto configured = world.handle->StageMeasuredStateSources(
      {original}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(configured.ok());
  configured.value()->EnablePublication();
  const auto old_reader = configured.value()->readers().front();
  auto publisher = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return publisher->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.name = {"source_joint_1"};
  message.position = {0.25};
  publisher->publish(message);
  const auto &old_query = original.publication.query_context;
  ASSERT_TRUE(fake::WaitUntil([&] { return old_reader->current_state(old_query, 1000).ok(); }));
  const auto candidate = MeasuredRouteSuccessor(world.binding);
  const auto successor = MeasuredSourceDescriptor(candidate, world.prefix);
  const auto participant = world.handle->participant();
  const auto port = world.handle->current_port();
  auto replacement = world.handle->StageMeasuredStateSources(
      {successor}, clock, candidate, std::nullopt);
  ASSERT_TRUE(replacement.ok());
  EXPECT_TRUE(world.handle->OwnsBinding(world.binding));
  EXPECT_FALSE(world.handle->OwnsBinding(candidate));
  EXPECT_EQ(world.handle->participant(), participant);
  EXPECT_EQ(world.handle->current_port(), port);
  const auto new_reader = replacement.value()->readers().front();
  const auto &new_query = successor.publication.query_context;
  EXPECT_FALSE(new_reader->current_state(new_query, 1000).ok());
  EXPECT_FALSE(new_reader->current_state(old_query, 1000).ok());
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return publisher->get_subscription_count() == 2U; }));
  clock->set(1050);
  message.position = {0.5};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto sample = old_reader->current_state(old_query, 1050);
    return sample.ok() && sample.value().received_timestamp_ns == 1050;
  }));
  // Candidate preparation does not retire the predecessor or accept a sample
  // before the route owner's positive readback and coherent commit.
  EXPECT_FALSE(new_reader->current_state(new_query, 1050).ok());
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(
          old_reader->current_state(old_query, 1050).value().positions[0])
          .value_rad,
      0.5);
  replacement.value()->EnablePublication();
  EXPECT_TRUE(world.handle->OwnsBinding(world.binding));
  EXPECT_FALSE(world.handle->OwnsBinding(candidate));
  EXPECT_EQ(world.handle->current_port(), port);
  EXPECT_FALSE(new_reader->current_state(new_query, 1050).ok());
  clock->set(1075);
  message.position = {0.75};
  publisher->publish(message);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return new_reader->current_state(new_query, 1075).ok(); }));
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(
          new_reader->current_state(new_query, 1075).value().positions[0])
          .value_rad,
      0.75);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto sample = old_reader->current_state(old_query, 1075);
    return sample.ok() && sample.value().received_timestamp_ns == 1075;
  }));
  EXPECT_FALSE(old_reader->current_state(new_query, 1075).ok());
  EXPECT_FALSE(new_reader->current_state(old_query, 1075).ok());
  configured.value().reset();
  replacement.value().reset();
  // Owning readers independently keep their subscription/node/clock lease.
  EXPECT_EQ(publisher->get_subscription_count(), 2U);
  const auto pinned = old_reader->current_state(old_query, 1075).value();
  publisher.reset();
  world.handle.reset();
  EXPECT_EQ(old_reader->current_state(old_query, 1100).value(), pinned);
  EXPECT_TRUE(new_reader->current_state(new_query, 1100).ok());
  EXPECT_FALSE(old_reader->current_state(old_query, 1176).ok());
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

// The inactive JTC fixture sends no controller samples. After its initial
// activity is drained, the next owner-clock read comes from the actual measured
// subscription receipt. Hold that boundary to exercise concurrent last-owner
// release without relying on a timing window or adding a product test hook.
class BlockingMeasuredReceiptClock final : public core::IClock {
public:
  ~BlockingMeasuredReceiptClock() override { Release(); }

  core::TimePoint now() const override {
    std::unique_lock lock(mutex_);
    ++reads_;
    if (armed_) {
      entered_ = true;
      cv_.notify_all();
      cv_.wait(lock, [&] { return released_; });
    }
    return 1000;
  }
  void Arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
  }
  [[nodiscard]] bool Entered() const {
    std::lock_guard lock(mutex_);
    return entered_;
  }
  void Release() {
    std::lock_guard lock(mutex_);
    armed_ = false;
    released_ = true;
    cv_.notify_all();
  }
  [[nodiscard]] std::size_t Reads() const {
    std::lock_guard lock(mutex_);
    return reads_;
  }
private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  bool armed_{false};
  mutable bool entered_{false};
  bool released_{false};
  mutable std::size_t reads_{};
};

TEST(RetainedRos2ControlRuntimeHandle, MeasuredReceiptDoesNotOwnHandleAndTeardownDrainsBeforeClockRelease) {
  auto clock = std::make_shared<BlockingMeasuredReceiptClock>();
  ArmWorld world("arm_controller", nullptr, ArmBinding(), clock);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto &query = descriptor.publication.query_context;
  const auto configured = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(configured.ok());
  const auto reader = configured.value()->readers().front();
  configured.value()->EnablePublication();
  auto publisher = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return publisher->get_subscription_count() == 1U && world.handle->arm_activity().observed;
  }));
  EXPECT_FALSE(world.handle->arm_activity().controller_active);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak_owner = world.handle;
  clock->Arm();
  sensor_msgs::msg::JointState message;
  message.name = {"source_joint_1"};
  message.position = {0.375};
  publisher->publish(message);
  const bool receipt_entered =
      fake::WaitUntil([&] { return clock->Entered(); });
  if (!receipt_entered)
    clock->Release();
  ASSERT_TRUE(receipt_entered);
  std::atomic<bool> destroyed{false};
  std::jthread destroyer([retained = std::move(world.handle), &destroyed]() mutable {
    retained.reset();
    destroyed.store(true, std::memory_order_release);
  });
  const bool no_callback_owner = fake::WaitUntil([&] { return weak_owner.expired(); });
  // If the regression recurs, rescue the old implementation's last callback
  // owner before unblocking it. The assertion reports the ownership error
  // without deliberately causing executor self-join during the test cleanup.
  auto rescue = no_callback_owner ? std::shared_ptr<RetainedRos2ControlRuntimeHandle>{}
                                 : weak_owner.lock();
  EXPECT_TRUE(no_callback_owner);
  EXPECT_FALSE(destroyed.load(std::memory_order_acquire));
  clock->Release();
  destroyer.join();
  if (rescue) {
    // In the prior implementation join only drained the external reset, not
    // the callback's strong owner. Keep the rescue until that owner positively
    // releases; the inactive fixture has no other continuing handle owners.
    EXPECT_TRUE(fake::WaitUntil([&] { return rescue.use_count() == 1; }));
    // Even after a failed bounded assertion, never unwind borrowed fixture
    // lifetimes while the callback still owns the handle. Actual completion
    // remains required; the existing CTest timeout bounds a stuck callback.
    while (rescue.use_count() != 1) std::this_thread::yield();
  }
  rescue.reset();
  EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
  EXPECT_TRUE(weak_owner.expired());
  const auto frozen = reader->current_state(query, 1000);
  ASSERT_TRUE(frozen.ok());
  EXPECT_DOUBLE_EQ(std::get<core::AngularPositionDTO>(frozen.value().positions[0]).value_rad, 0.375);
  EXPECT_EQ(frozen.value().received_timestamp_ns, 1000);
  const auto clock_reads = clock->Reads();
  EXPECT_EQ(reader->current_state(query, 1050).value(), frozen.value());
  EXPECT_FALSE(reader->current_state(query, 1101).ok());
  EXPECT_TRUE(reader->publication(query.control_binding_id, query.control_binding_revision).ok());
  EXPECT_EQ(clock->Reads(), clock_reads);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandle,
     MeasuredReaderOwnsTheActualClockBeyondHandleAndPreparedLease) {
  std::shared_ptr<const core::IObservedStateProvider> reader;
  std::weak_ptr<core::IClock> clock_lifetime;
  {
    auto clock = std::make_shared<core::FakeClock>();
    clock->set(1000);
    clock_lifetime = clock;
    ArmWorld world("arm_controller", nullptr, ArmBinding(),
                   clock);
    auto prepared = world.handle->StageMeasuredStateSources(
        {MeasuredSourceDescriptor(world)}, clock, world.binding, std::nullopt);
    ASSERT_TRUE(prepared.ok());
    reader = prepared.value()->readers().front();
    prepared.value().reset();
  }
  EXPECT_FALSE(clock_lifetime.expired());
  reader.reset();
  EXPECT_TRUE(clock_lifetime.expired());
}

TEST(RetainedRos2ControlRuntimeHandle,
     SharedContextKeepsSiblingActiveAndRetiredMeasuredReaderFrozen) {
  std::shared_ptr<const core::IObservedStateProvider> retained_reader;
  std::weak_ptr<rclcpp::Context> context_lifetime;
  std::weak_ptr<core::IClock> clock_lifetime;
  {
    auto context = std::make_shared<rclcpp::Context>();
    int argc = 0;
    context->init(argc, nullptr);
    context_lifetime = context;
    auto clock = std::make_shared<core::FakeClock>();
    clock->set(1000);
    clock_lifetime = clock;
    auto provider = [context] { return context; };
    ArmWorld world("arm_controller", nullptr, ArmBinding(),
                   clock, false, {}, provider);
    ros2_control_execution::Ros2TransportResources sibling(
        "shared_context_io_" + fake::unique_suffix(), "", 2U, provider);
    const auto descriptor = MeasuredSourceDescriptor(world);
    const auto &query = descriptor.publication.query_context;
    auto prepared = world.handle->StageMeasuredStateSources(
        {descriptor}, clock, world.binding, std::nullopt);
    ASSERT_TRUE(prepared.ok());
    retained_reader = prepared.value()->readers().front();
    prepared.value()->EnablePublication();
    auto publisher = sibling.node()->create_publisher<sensor_msgs::msg::JointState>(
        world.prefix + "/joint_states", rclcpp::QoS(1));
    auto sibling_receipts = std::make_shared<std::atomic<unsigned>>(0U);
    const auto subscription = sibling.node()->create_subscription<sensor_msgs::msg::JointState>(
        world.prefix + "/joint_states", rclcpp::QoS(1),
        [sibling_receipts](sensor_msgs::msg::JointState::ConstSharedPtr) { ++*sibling_receipts; });
    world.handle->StartBackend();
    sibling.start();
    ASSERT_TRUE(fake::WaitUntil([&] { return publisher->get_subscription_count() == 2U; }));
    sensor_msgs::msg::JointState message;
    message.name = {"source_joint_1"};
    message.position = {0.375};
    publisher->publish(message);
    ASSERT_TRUE(fake::WaitUntil([&] {
      return retained_reader->current_state(query, 1000).ok() && sibling_receipts->load() == 1U;
    }));
    const auto frozen = retained_reader->current_state(query, 1000).value();
    prepared.value().reset();
    world.handle.reset();
    ASSERT_TRUE(context->is_valid());
    clock->set(1050);
    message.position = {0.75};
    publisher->publish(message);
    ASSERT_TRUE(fake::WaitUntil([&] { return sibling_receipts->load() == 2U; }));
    EXPECT_EQ(retained_reader->current_state(query, 1050).value(), frozen);
    EXPECT_FALSE(retained_reader->current_state(query, 1101).ok());
    sibling.quiesce();
    EXPECT_TRUE(context->is_valid());
    EXPECT_EQ(world.manager.switch_requests(), 0U);
  }
  // The frozen reader pins its actual Node/Context and receipt clock even
  // after the host holder, both transports and the prepared lease are gone.
  ASSERT_FALSE(context_lifetime.expired());
  EXPECT_TRUE(context_lifetime.lock()->is_valid());
  EXPECT_FALSE(clock_lifetime.expired());
  retained_reader.reset();
  EXPECT_TRUE(context_lifetime.expired());
  EXPECT_TRUE(clock_lifetime.expired());
}

TEST(RetainedRos2ControlRuntimeHandle,
     InvalidMeasuredCandidateDoesNotAlterExistingSource) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", nullptr, ArmBinding(), clock);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto configured = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(configured.ok());
  configured.value()->EnablePublication();
  auto publisher = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      world.prefix + "/joint_states", rclcpp::QoS(1));
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return publisher->get_subscription_count() == 1U; }));
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources(
                       {descriptor}, std::make_shared<core::FakeClock>(),
                       world.binding, std::nullopt)
                   .ok());
  auto invalid = descriptor;
  invalid.source_joint_names.clear();
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({invalid}, clock, world.binding,
                                               std::nullopt)
                   .ok());
  invalid = descriptor;
  invalid.publication.query_context.runtime_epoch =
      core::RuntimeEpoch("foreign.runtime.epoch");
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({invalid}, clock, world.binding,
                                               std::nullopt)
                   .ok());
  invalid = descriptor;
  invalid.publication.source_precedence.front().source_id =
      "/other/joint_states";
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({invalid}, clock, world.binding,
                                               std::nullopt)
                   .ok());
  invalid = descriptor;
  invalid.publication.query_context.freshness_budget_ns += 1;
  invalid.publication.source_precedence.front().freshness_budget_ns += 1;
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({invalid}, clock, world.binding,
                                               std::nullopt)
                   .ok());
  invalid = descriptor;
  invalid.publication.authority_generation += 1;
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({invalid}, clock, world.binding,
                                               std::nullopt)
                   .ok());
  const std::vector<std::function<void(core::RuntimeParticipantBindingDTO &)>>
      foreign_owners{
          [](auto &value) {
            value.control_path->provider_instance_id =
                core::ProviderInstanceId("foreign.provider");
          },
          [](auto &value) {
            value.control_path->provider_runtime_epoch =
                core::RuntimeEpoch("foreign.epoch");
            value.runtime_unit_epochs.front().runtime_epoch =
                core::RuntimeEpoch("foreign.epoch");
          },
          [](auto &value) {
            value.model_bindings.front().model_revision =
                core::ModelRevision("foreign.model.revision");
          },
          [](auto &value) {
            value.resources.front().source_joint_name = "foreign_source_joint";
          },
          [](auto &value) {
            value.control_path->state_authority_id =
                core::StateAuthorityId("foreign.execution.authority");
          },
          [](auto &value) {
            value.control_path->active_control_configuration_generation =
                core::ActiveControlConfigurationGeneration(2U);
          }};
  for (std::size_t index = 0; index < foreign_owners.size(); ++index) {
    SCOPED_TRACE(index);
    auto value = world.binding.value();
    foreign_owners[index](value);
    auto candidate = core::verify_runtime_participant_binding(std::move(value));
    ASSERT_TRUE(candidate.ok()) << candidate.error().message;
    // Last case is physically valid but the predecessor descriptor is not
    // the complete candidate query. All other cases also provide a matching
    // descriptor so physical validation itself must reject them.
    const auto proposed =
        index + 1 == foreign_owners.size()
            ? descriptor
            : MeasuredSourceDescriptor(candidate.value(), world.prefix);
    EXPECT_FALSE(world.handle
                     ->StageMeasuredStateSources(
                         {proposed}, clock, candidate.value(), std::nullopt)
                     .ok());
    EXPECT_TRUE(world.handle->OwnsBinding(world.binding));
  }
  EXPECT_FALSE(world.handle
                   ->StageMeasuredStateSources({descriptor}, clock,
                                               world.binding, GripperBinding())
                   .ok());
  sensor_msgs::msg::JointState message;
  message.name = {"source_joint_1"};
  message.position = {0.5};
  publisher->publish(message);
  const auto &query = descriptor.publication.query_context;
  ASSERT_TRUE(fake::WaitUntil([&] {
    return configured.value()
        ->readers()
        .front()
        ->current_state(query, 1000)
        .ok();
  }));
  EXPECT_EQ(publisher->get_subscription_count(), 1U);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

// Active TCP (M7): the test robot and an entry of its list, named by the
// compiled frame of its row.
const core::ControllableComponentId kRobot("robot");
[[nodiscard]] core::TcpEntryDTO TcpEntry(const std::string &frame) {
  return {kRobot, core::CommandTargetId("arm"), core::FrameRoleId(frame)};
}
/// Its TCP list: one row per frame; `end_effector` marks the first.
[[nodiscard]] std::vector<RobotTcp> TcpRows(const std::vector<std::string> &frames,
                                            bool end_effector = true) {
  std::vector<RobotTcp> rows;
  for (const auto &frame : frames)
    rows.push_back(RobotTcp{TcpEntry(frame),
                            core::ProgramFrameRoleBindingDTO{
                                core::FrameRoleId(frame), core::ModelInstanceId("model"),
                                core::ModelRevision("revision"), frame, {}},
                            end_effector && rows.empty(), false});
  return rows;
}

// --- R7 package 4: the Real arm's actions --------------------------------

// Active TCP (M7, design 2, 4; T-MQ3, A2a-2): the value starts at the
// End-effector TCP, with a greater registration generation and sequence 0 at
// install; every confirmed change increments the sequence and is one event
// (previous, new, source, offset sent, registration generation, sequence);
// the same entry or an unknown one changes nothing; a reset goes back to the
// End-effector TCP. A reinstall (another runtime) has a greater generation.
// Mock (no adapter): every change commits at once.
TEST(RetainedRos2ControlRuntimeHandleTest,
     ActiveTcpStartsAtTheEndEffectorAndRecordsEachConfirmedChange) {
  ArmWorld world;
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  EXPECT_THROW(world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp"}), true),
               std::logic_error);
  std::vector<core::ActiveTcpChangeDTO> events;
  world.handle->BindActiveTcpEvents(
      [&](const core::ActiveTcpChangeDTO &change) { events.push_back(change); });
  world.handle->StartBackend();
  auto reading = world.handle->ReadActiveTcp();
  EXPECT_EQ(reading.robot, std::optional(kRobot));
  ASSERT_TRUE(reading.value);
  EXPECT_EQ(reading.value->entry, TcpEntry("tcp"));
  const auto installed = reading.sequence;
  EXPECT_EQ(installed, 0U);
  EXPECT_GT(reading.registration_generation, 0U);

  ASSERT_TRUE(world.handle
                  ->SetActiveTcp(TcpEntry("tool"), core::ActiveTcpChangeSource::kRobotControl)
                  .ok());
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].component_id, kRobot);
  EXPECT_EQ(events[0].previous, std::optional(TcpEntry("tcp")));
  EXPECT_EQ(events[0].entry, std::optional(TcpEntry("tool")));
  EXPECT_EQ(events[0].source, core::ActiveTcpChangeSource::kRobotControl);
  EXPECT_FALSE(events[0].offset_sent);
  reading = world.handle->ReadActiveTcp();
  EXPECT_EQ(reading.value->entry, TcpEntry("tool"));
  EXPECT_EQ(reading.sequence, installed + 1U);
  EXPECT_EQ(events[0].sequence, installed + 1U);
  EXPECT_EQ(events[0].registration_generation, reading.registration_generation);
  // The same entry, or one outside the list: no event.
  ASSERT_TRUE(world.handle->SetActiveTcp(TcpEntry("tool"), core::ActiveTcpChangeSource::kProgram)
                  .ok());
  const auto unknown =
      world.handle->SetActiveTcp(TcpEntry("other"), core::ActiveTcpChangeSource::kProgram);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "active_tcp_unknown");
  EXPECT_EQ(events.size(), 1U);
  // A reset: the End-effector TCP, once.
  ASSERT_TRUE(world.handle->ResetActiveTcp().ok());
  ASSERT_TRUE(world.handle->ResetActiveTcp().ok());
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[1].entry, std::optional(TcpEntry("tcp")));
  EXPECT_EQ(events[1].source, core::ActiveTcpChangeSource::kReset);
  EXPECT_EQ(world.handle->ReadActiveTcp().sequence, installed + 2U);
  EXPECT_EQ(events[1].sequence, installed + 2U);
  ArmWorld restarted;
  restarted.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  EXPECT_GT(restarted.handle->ReadActiveTcp().registration_generation,
            reading.registration_generation);
  EXPECT_EQ(restarted.handle->ReadActiveTcp().sequence, 0U);
}

// C-P2, B-run-1: a robot without an End-effector TCP has no Active TCP; a
// reset keeps none, and an explicit entry still sets it.
TEST(RetainedRos2ControlRuntimeHandleTest, NoEndEffectorTcpMeansNoActiveTcpUntilChosen) {
  ArmWorld world;
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}, false), true);
  world.handle->StartBackend();
  EXPECT_FALSE(world.handle->ReadActiveTcp().value);
  ASSERT_TRUE(world.handle->ResetActiveTcp().ok());
  EXPECT_FALSE(world.handle->ReadActiveTcp().value);
  ASSERT_TRUE(world.handle->SetActiveTcp(TcpEntry("tool"), core::ActiveTcpChangeSource::kProgram)
                  .ok());
  EXPECT_EQ(world.handle->ReadActiveTcp().value->entry, TcpEntry("tool"));
}

// Q4: a TCP-changing move under the robot's direct-control binding is Robot
// control's change; any other binding's is a Program's.
TEST(RetainedRos2ControlRuntimeHandleTest, AMoveRecordsRobotControlUnderItsDirectBinding) {
  ArmWorld world;
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true,
                                 core::ProgramBindingId("binding.direct"));
  std::vector<core::ActiveTcpChangeDTO> events;
  world.handle->BindActiveTcpEvents(
      [&](const core::ActiveTcpChangeDTO &change) { events.push_back(change); });
  world.handle->StartBackend();
  ASSERT_TRUE(world.handle
                  ->ApplyMotionToolContext(TcpEntry("tool"), core::ProgramBindingId("binding.direct"))
                  .ok());
  ASSERT_TRUE(world.handle
                  ->ApplyMotionToolContext(TcpEntry("tcp"), core::ProgramBindingId("binding.program"))
                  .ok());
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].source, core::ActiveTcpChangeSource::kRobotControl);
  EXPECT_EQ(events[1].source, core::ActiveTcpChangeSource::kProgram);
}

// TOOLCTX: Mock and Isaac have no tool-context hook: nothing to apply.
// Active TCP (M7): a TCP change commits at once.
TEST(RetainedRos2ControlRuntimeHandleTest, WithoutAToolContextHookATcpChangeCommitsAlone) {
  ArmWorld world;
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  world.handle->StartBackend();
  EXPECT_FALSE(world.handle->HasToolContext());
  EXPECT_TRUE(world.handle->ApplyMotionToolContext(TcpEntry("tool")).ok());
  EXPECT_EQ(world.handle->ReadActiveTcp().value->entry, TcpEntry("tool"));
  EXPECT_TRUE(world.handle->ApplyLatestCommittedToolPayload().ok());
  // Nothing re-armed: nothing to rebind.
  EXPECT_TRUE(world.handle->RebindPendingRearm().ok());
}

// CONTRACTS §6: the vendor family's tool-context hook is the one path of the
// TCP offset; it is installed once, complete, with the tool context sources.
TEST(RetainedRos2ControlRuntimeHandleTest, TheToolContextHookSendsTheMovesTcpOffset) {
  ArmWorld world;
  std::vector<std::optional<core::Pose3DTO>> offsets;
  ToolContextHook hook{[&](const std::optional<core::Pose3DTO> &offset,
                           const std::optional<core::ToolContextPointMassDTO> &,
                           const bool at_enable, const auto &, const std::stop_token &) {
                         EXPECT_FALSE(at_enable);
                         offsets.push_back(offset);
                         return core::Result<bool>::Ok(false);
                       },
                       std::chrono::seconds(1)};
  core::ToolContextDTO tool;
  tool.tcp_pose_in_flange.position_m.z_m = 0.1;
  const auto source = [&](const std::optional<std::string> &frame) {
    return core::Result<core::ToolContextDTO>::Ok(frame ? tool : core::ToolContextDTO{});
  };
  const auto composed = [&](const std::optional<std::string> &frame,
                            const core::LogicalSceneSnapshotDTO &) { return source(frame); };
  EXPECT_THROW(world.handle->InstallToolContext(ToolContextHook{}, source, composed, composed),
               std::logic_error);
  world.handle->InstallToolContext(hook, source, composed, composed);
  EXPECT_TRUE(world.handle->HasToolContext());
  EXPECT_THROW(world.handle->InstallToolContext(hook, source, composed, composed),
               std::logic_error);
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  world.handle->StartBackend();
  ASSERT_TRUE(world.handle->ApplyMotionToolContext(TcpEntry("tool")).ok());
  ASSERT_EQ(offsets.size(), 1U);
  EXPECT_EQ(offsets.front(), std::optional(tool.tcp_pose_in_flange));
  EXPECT_EQ(world.handle->ReadActiveTcp().value->entry, TcpEntry("tool"));
}

// T-N1: a hook call that may still act is the component's tool-context fault
// until Recover; the Active TCP stays unchanged.
TEST(RetainedRos2ControlRuntimeHandleTest, AToolContextCallThatMayStillActFaultsTheComponent) {
  ArmWorld world;
  ToolContextHook hook{[](const std::optional<core::Pose3DTO> &,
                          const std::optional<core::ToolContextPointMassDTO> &, bool,
                          const auto &, const std::stop_token &) {
                         return core::Result<bool>::Err(core::ErrorDTO{
                             "robot_controller_unresponsive", "no answer",
                             {{"reason", "tool_context_not_applied"}, {"effect", "possible"}}});
                       },
                       std::chrono::seconds(1)};
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({});
  };
  const auto composed = [&](const std::optional<std::string> &frame,
                            const core::LogicalSceneSnapshotDTO &) { return source(frame); };
  world.handle->InstallToolContext(hook, source, composed, composed);
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  world.handle->StartBackend();
  const auto refused = world.handle->ApplyMotionToolContext(TcpEntry("tool"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "robot_controller_unresponsive");
  EXPECT_EQ(world.handle->ReadActiveTcp().value->entry, TcpEntry("tcp"));
  EXPECT_EQ(world.handle->ReadObservation(0).tool_context_fault, std::optional<std::string>("no answer"));
}

// A vendor driver may reactivate the trajectory controller itself after the
// Enable's tool-context re-arm: the reported active controller satisfies the
// Enable, bound with no switch (a strict activation would be refused).
TEST(RetainedRos2ControlRuntimeHandleTest,
     AnEnableWhoseHookLeftTheControllerActiveBindsItWithoutASwitch) {
  ArmWorld world;
  ToolContextHook hook{[&](const std::optional<core::Pose3DTO> &,
                           const std::optional<core::ToolContextPointMassDTO> &,
                           const bool at_enable, const auto &, const std::stop_token &) {
                         if (at_enable) {
                           world.manager.SetControllerActive(true);
                           EXPECT_TRUE(fake::WaitUntil(
                               [&] { return world.handle->arm_activity().controller_active; }));
                         }
                         return core::Result<bool>::Ok(at_enable);
                       },
                       std::chrono::seconds(1)};
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({});
  };
  const auto composed = [&](const std::optional<std::string> &frame,
                            const core::LogicalSceneSnapshotDTO &) { return source(frame); };
  world.handle->InstallToolContext(hook, source, composed, composed);
  const auto committed = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          {core::SceneId("sha256:" + std::string(64, 'a')), 1U,
           core::ContentDigest("sha256:" + std::string(64, 'b'))},
          core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
  world.handle->InstallCommittedSceneSource([&] { return committed; });
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kPrepared));
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle); }));
  const auto switches = world.manager.switch_requests();
  const auto enabled = world.handle->ActivateArmController({}, Soon());
  EXPECT_EQ(enabled.outcome, Outcome::kSucceeded);
  EXPECT_EQ(world.manager.switch_requests(), switches);
  EXPECT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kEnabled));
}

TEST(RetainedRos2ControlRuntimeHandleTest, MockMotionCheckWaitsForCommittedScene) {
  ArmWorld world;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> committed;
  world.handle->InstallCommittedSceneSource([&] { return committed; });
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  world.handle->StartBackend();
  const auto unavailable = world.handle->ApplyLatestCommittedToolPayload();
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "execution_port_not_ready");
  committed = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          {core::SceneId("sha256:" + std::string(64, 'a')), 1U,
           core::ContentDigest("sha256:" + std::string(64, 'b'))},
          core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
  EXPECT_TRUE(world.handle->ApplyLatestCommittedToolPayload().ok());
}

/// The arm display channel as the host's execution runtime builds it, over a
/// one-joint retained model.
[[nodiscard]] std::shared_ptr<RetainedObservedStateChannel>
ArmDisplayChannel(const core::RuntimeParticipantBinding &binding, core::IClock &clock) {
  const auto &value = binding.value();
  const auto &resource = value.resources.front();
  RetainedKinematicModelMapInput model_map{value.retained_bundle_id, value.graph_revision, {}};
  model_map.control_bindings.push_back(
      {value.control_path->control_binding_id, value.controllable_component_id,
       value.command_target_id,
       {{resource.resource_id,
         {{value.model_groups.front().model_instance_id, resource.compiled_resource_name,
           resource.coordinate_kind}}}}});
  const std::string urdf =
      "<robot name='display_fixture' version='1.0'><link name='base'/>"
      "<link name='joint_1_link'/><joint name='" + resource.compiled_resource_name +
      "' type='revolute'><parent link='base'/><child link='joint_1_link'/>"
      "<axis xyz='0 0 1'/><limit lower='-1' upper='1' effort='5' velocity='2'/>"
      "</joint></robot>";
  auto scene = std::make_shared<roboplan::Scene>(
      "display_fixture", roboplan::loadUrdfSceneDescriptionFromXml(urdf, {}));
  RetainedObservedStateChannelConfig config{
      core::ObservedStateSourceKind::kCommandedState, "commanded_state", 100'000'000, 1U};
  auto projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create(std::move(scene), std::make_shared<std::mutex>(),
                                              std::move(model_map), {binding},
                                              config.freshness_budget_ns));
  return std::make_shared<RetainedObservedStateChannel>(binding, std::move(projector),
                                                        std::move(config), &clock);
}

// CONTRACTS §6 display joint source: the arm's display follows the vendor's
// own joint topic. A sample the family's check ignores (all zero) or that
// doesn't name the bound joint keeps the last valid pose; a valid one moves
// the display.
TEST(RetainedRos2ControlRuntimeHandleTest, TheDisplayFollowsTheDisplayJointSource) {
  std::atomic<int> checked{0};  // outlives the handle's subscription
  ArmWorld world;
  const auto display = ArmDisplayChannel(world.binding, world.clock);
  const auto topic = world.prefix + "/xarm/joint_states";
  world.handle->InstallDisplayJointSource(
      DisplayJointSource{topic,
                         [&](std::int64_t, const std::vector<double> &positions) {
                           ++checked;
                           return positions == std::vector<double>{0.0}
                                      ? DisplayJointSampleUse::kIgnore
                                      : DisplayJointSampleUse::kDisplay;
                         }},
      display);
  EXPECT_THROW(world.handle->InstallDisplayJointSource(
                   DisplayJointSource{topic, [](std::int64_t, const std::vector<double> &) {
                                        return DisplayJointSampleUse::kDisplay;
                                      }},
                   display),
               std::logic_error);
  world.handle->StartBackend();
  const auto vendor = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      topic, rclcpp::SensorDataQoS());
  const auto publish = [&](const std::string &name, const double position) {
    sensor_msgs::msg::JointState message;
    message.name = {name};
    message.position = {position};
    vendor->publish(message);
  };
  const auto shown = [&](const double position) {
    const auto render = display->current_bound_render_state();
    return render.ok() && render.value().display_only &&
           render.value().positions_si == std::vector<double>{position};
  };
  ASSERT_TRUE(fake::WaitUntil([&] {
    publish("source_joint_1", 0.4);
    return shown(0.4);
  }));
  const auto seen = checked.load();
  // A sample the family ignores reaches its check but not the display. One
  // message per wait: the subscription keeps the latest sample only.
  ASSERT_TRUE(fake::WaitUntil([&] {
    publish("source_joint_1", 0.0);
    return checked.load() > seen;
  }));
  // A sample without the arm's joints never reaches the check.
  const auto ignored = checked.load();
  publish("other_joint", 0.7);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(checked.load(), ignored);
  EXPECT_TRUE(shown(0.4));
}

} // namespace
} // namespace manyforge::roboplan_backend

namespace manyforge::roboplan_backend {
namespace {

// CONTRACTS §6: the ros2_control lane adapter over the fake controller
// manager. The controller's own reports drive the state; the actions are the
// handle's primitives.
TEST(Ros2ControlComponentAdapterTest, ActionsFollowTheControllerManagerReports) {
  ArmWorld world;
  world.handle->StartBackend();
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
  ASSERT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kPrepared));
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle); }));
  EXPECT_EQ(adapter.State(world.clock.now()).activity, core::ComponentActivity::kIdle);
  EXPECT_TRUE(adapter.capabilities().empty());

  const auto enabled = adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(enabled.outcome, core::ActionOutcome::kSucceeded);
  EXPECT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kEnabled));
  // CONTRACTS §2: an already satisfied action succeeds at once, no switch.
  const auto switches = world.manager.switch_requests();
  EXPECT_EQ(adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(world.manager.switch_requests(), switches);
  // Prepare on the arm this host enabled is refused, typed, nothing sent;
  // the arm stays enabled.
  const auto prepare_enabled = adapter.Run(core::ComponentAction::kPrepare, {});
  EXPECT_EQ(prepare_enabled.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(prepare_enabled.error.has_value());
  EXPECT_EQ(prepare_enabled.error->code, "component_enabled");
  EXPECT_EQ(world.manager.switch_requests(), switches);
  EXPECT_EQ(adapter.State(world.clock.now()).lifecycle, core::ComponentLifecycle::kEnabled);

  const auto disabled = adapter.Run(core::ComponentAction::kDisable, {});
  EXPECT_EQ(disabled.outcome, core::ActionOutcome::kSucceeded);
  EXPECT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kPrepared));
  EXPECT_EQ(adapter.Run(core::ComponentAction::kDisable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(world.manager.switch_requests(), switches + 1U);

  const auto refused = adapter.Run(core::ComponentAction::kPowerOff, {});
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "component_action_unsupported");
}

// CONTRACTS §6: a switch's result is the controller manager's activity
// report. A switch answered before its activity sample doesn't end Enable or
// Disable; the sample does, and the adapter's state shows it at once.
TEST(Ros2ControlComponentAdapterTest, EnableAndDisableReturnOnlyOnceTheActivityShowsThem) {
  ArmWorld world;
  world.handle->StartBackend();
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
  ASSERT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kPrepared));
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle); }));
  for (const auto &[action, shown] :
       {std::pair{core::ComponentAction::kEnable, core::ComponentLifecycle::kEnabled},
        std::pair{core::ComponentAction::kDisable, core::ComponentLifecycle::kPrepared}}) {
    SCOPED_TRACE(std::string(core::to_string(action)));
    world.manager.SetActivityPublicationEnabled(false);
    const auto switches = world.manager.switch_requests();
    auto pending = std::async(std::launch::async, [&, action = action] {
      return adapter.Run(action, {});
    });
    ASSERT_TRUE(fake::WaitUntil([&] { return world.manager.switch_requests() == switches + 1U; }));
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout);
    world.manager.SetActivityPublicationEnabled(true);
    const auto result = pending.get();
    EXPECT_EQ(result.outcome, core::ActionOutcome::kSucceeded);
    EXPECT_EQ(adapter.State(world.clock.now()).lifecycle, shown);
  }
}

// A switch whose activity sample never comes within the lifecycle command
// time is the action's typed failure.
TEST(Ros2ControlComponentAdapterTest, ASwitchWhoseActivityIsNeverReportedFailsTyped) {
  ArmWorld world;
  world.handle->StartBackend();
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::milliseconds(500));
  ASSERT_TRUE(world.WaitForLifecycle(core::ComponentLifecycle::kPrepared));
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle); }));
  world.manager.SetActivityPublicationEnabled(false);
  const auto switches = world.manager.switch_requests();
  const auto failed = adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(failed.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "component_action_result_not_observed");
  EXPECT_EQ(world.manager.switch_requests(), switches + 1U);  // never resent
}

// The component's lifecycle command time bounds its prepare, which waits
// for its own endpoints (the controller manager reporting the loaded
// controller) within it instead of refusing at once.
TEST(Ros2ControlComponentAdapterTest, PrepareWaitsForItsEndpointsWithinTheLifecycleCommandTime) {
  {
    ArmWorld world;  // its controller manager is never connected
    Ros2ControlComponentAdapter adapter(world.handle, std::chrono::milliseconds(300));
    const auto started = std::chrono::steady_clock::now();
    const auto failed = adapter.Run(core::ComponentAction::kPrepare, {});
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(300));
    EXPECT_EQ(failed.outcome, core::ActionOutcome::kFailed);
  }
  ArmWorld world;
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
  auto pending = std::async(std::launch::async,
                            [&] { return adapter.Run(core::ComponentAction::kPrepare, {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  world.handle->StartBackend();
  EXPECT_EQ(pending.get().outcome, core::ActionOutcome::kSucceeded);
}

// Live 2026-11-01: an Isaac gripper is its command bridge (an action server
// launched with the component), with no controller manager. Its lifecycle is
// the bridge's, as M7's command-bridge domain: Prepare waits for the action
// server only (its state is a stream, never the lifecycle); Enable and
// Disable send no controller switch.
TEST(Ros2ControlComponentAdapterTest, AnIsaacBridgeGripperNeedsNoControllerManager) {
  const auto bind = [](const std::string &prefix, core::IClock &clock,
                       const std::shared_ptr<core::IClock> &owner,
                       core::ManualScheduler &scheduler) {
    auto sink = std::make_shared<NullSink>();
    auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
        "manyforge_isaac_bridge_" + fake::unique_suffix(), "/", 3U, sink, "gripper", clock);
    auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
        *resources, sink, clock, scheduler, true);
    ros2_control_execution::Ros2GripperActionConfig config;
    config.state_freshness_timeout_s = 1.0;
    config.action_name = prefix + "/gripper_controller/gripper_cmd";
    config.group_id = "gripper";
    config.joint_name = "gripper_joint";
    const auto tool = GripperBinding();
    backend->configure_gripper_action(tool.value().command_target_id.value(), config);
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    EXPECT_TRUE(handle->BindGripperOnly(std::move(resources), clock, std::move(backend), tool,
                                        RetainedRos2ControlArmController{}, owner, std::nullopt)
                    .ok());
    handle->StartBackend();
    return handle;
  };
  auto clock = std::make_shared<WallClock>();
  core::ManualScheduler scheduler;
  {
    // No bridge running: Prepare waits its lifecycle time, then refuses
    // with the bridge's own code; the component is not prepared.
    const auto handle = bind("/manyforge_isaac_bridge_absent_" + fake::unique_suffix(), *clock,
                             clock, scheduler);
    Ros2ControlComponentAdapter adapter(handle, std::chrono::milliseconds(300));
    const auto failed = adapter.Run(core::ComponentAction::kPrepare, {});
    EXPECT_EQ(failed.outcome, core::ActionOutcome::kFailed);
    ASSERT_TRUE(failed.error.has_value());
    EXPECT_EQ(failed.error->code, "ros2_control_gripper_bridge_not_ready");
    EXPECT_EQ(adapter.State(clock->now()).lifecycle, core::ComponentLifecycle::kNotPrepared);
  }
  const auto prefix = "/manyforge_isaac_bridge_" + fake::unique_suffix();
  fake::FakeJtc jtc(prefix + "/arm_controller", {"source_joint_1"});
  GripperControllerManager bridge(prefix, jtc, true);  // only its action server is used
  const auto handle = bind(prefix, *clock, clock, scheduler);
  Ros2ControlComponentAdapter adapter(handle, std::chrono::seconds(30));
  EXPECT_EQ(adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(clock->now()).lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(clock->now()).lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kDisable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(clock->now()).lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_TRUE(bridge.Requests().empty());
}

// Live 2026-11-01: with four launches starting at once on Orin, a gripper's
// controller spawner took 13 s. As M7's bring-up, Prepare first waits for
// the launch to settle within its own bound; then its lifecycle time applies.
TEST(Ros2ControlComponentAdapterTest, PrepareWaitsForItsLaunchToSettleWithinItsBound) {
  {
    ArmWorld world;
    std::atomic<bool> settled{false};
    world.handle->InstallControllerLaunchSettle(
        [&settled] { return settled.load(); }, std::chrono::steady_clock::now(),
        std::chrono::seconds(30));
    world.handle->StartBackend();
    Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
    const auto started = std::chrono::steady_clock::now();
    auto pending = std::async(std::launch::async,
                              [&] { return adapter.Run(core::ComponentAction::kPrepare, {}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    settled = true;
    EXPECT_EQ(pending.get().outcome, core::ActionOutcome::kSucceeded);
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(300));
  }
  ArmWorld world;
  world.handle->InstallControllerLaunchSettle(
      [] { return false; }, std::chrono::steady_clock::now(), std::chrono::milliseconds(300));
  world.handle->StartBackend();
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
  const auto failed = adapter.Run(core::ComponentAction::kPrepare, {});
  EXPECT_EQ(failed.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "component_launch_not_settled");
}

// Live 2026-11-02 (Real Lite 6): a driver restart right after the Apply's
// launch stopped that launch before its spawners finished; Prepare must wait
// for the restarted launch's spawners, within the same span from its start.
TEST(Ros2ControlComponentAdapterTest, PrepareWaitsForTheRestartedLaunchAfterADriverRestart) {
  ArmWorld world;
  world.handle->InstallControllerLaunchSettle(
      [] { return false; },  // the stopped launch never settles
      std::chrono::steady_clock::now() - std::chrono::seconds(10), std::chrono::seconds(10));
  world.handle->StartBackend();
  EXPECT_FALSE(world.handle->ControllerLaunchSettling());  // past its bound
  std::atomic<bool> restarted_settled{false};
  world.handle->ReplaceControllerLaunchSettle(
      [&restarted_settled] { return restarted_settled.load(); },
      std::chrono::steady_clock::now());
  EXPECT_TRUE(world.handle->ControllerLaunchSettling());
  Ros2ControlComponentAdapter adapter(world.handle, std::chrono::seconds(30));
  auto pending = std::async(std::launch::async,
                            [&] { return adapter.Run(core::ComponentAction::kPrepare, {}); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  restarted_settled = true;
  EXPECT_EQ(pending.get().outcome, core::ActionOutcome::kSucceeded);
  EXPECT_FALSE(world.handle->ControllerLaunchSettling());
  // The restarted launch's spawner activated the joint state broadcaster
  // while the hardware was inactive; Enable's hardware activation leaves it
  // active, and the Enable still succeeds (live 2026-11-02: a strict switch
  // was refused, "already active").
  world.manager.SetHardwareActive(false);
  world.manager.SetBroadcasterActive(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && !activity.hardware_active && activity.broadcaster_active;
  }));
  EXPECT_EQ(world.handle->SwitchArmHardware(true, {}).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  // A hardware activation that deactivates the broadcaster: it is switched on.
  EXPECT_EQ(world.handle->DeactivateArmController({}, Soon()).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  world.manager.SetHardwareActive(false);
  world.manager.SetHardwareActivateHook([&] { world.manager.SetBroadcasterActive(false); });
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && !activity.hardware_active && !activity.controller_active;
  }));
  EXPECT_EQ(world.handle->SwitchArmHardware(true, {}).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().broadcaster_active; }));
  // Live 2026-11-02 (Real Lite 6): the driver switched every controller off
  // after the activation and its reactivation failed; the arm adapter's
  // repeated request with the hardware on switches the broadcaster back on.
  world.manager.SetBroadcasterActive(false);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.hardware_active && !activity.broadcaster_active;
  }));
  EXPECT_EQ(world.handle->SwitchArmHardware(true, {}).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().broadcaster_active; }));
  EXPECT_TRUE(world.handle->arm_activity().hardware_active);
  EXPECT_EQ(world.handle->ActivateArmController({}, Soon()).outcome,
            RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_TRUE(world.handle->arm_activity().broadcaster_active);
}

TEST(Ros2ControlComponentAdapterTest, MockResetPositionIsDeclaredWithTheHostPhase) {
  ArmWorld world("arm_controller", nullptr, ArmBinding(), {}, true);
  EXPECT_TRUE(Ros2ControlComponentAdapter(world.handle, std::chrono::seconds(30)).capabilities().empty());
  const Ros2ControlComponentAdapter adapter(
      world.handle, std::chrono::seconds(30), [](const MockResetPositionTransition &, MockResetPositionStepDeadline) {
        return core::Result<void>::Ok();
      });
  EXPECT_EQ(adapter.capabilities(),
            std::vector<core::Capability>{core::Capability::kResetPosition});
}

TEST(Ros2ControlComponentAdapterTest, PrimitiveOutcomesMapToActionResults) {
  using Outcome = RetainedRos2ControlSwitchOutcome;
  const core::ErrorDTO error{"execution_rejected", "refused", {}};
  EXPECT_EQ(ros2_control_action_result(core::ComponentAction::kEnable,
                                       {Outcome::kSucceeded, std::nullopt})
                .outcome,
            core::ActionOutcome::kSucceeded);
  for (const auto failed : {Outcome::kNotSent, Outcome::kFailed, Outcome::kFailedNoEffect,
                            Outcome::kFailedEffectObserved}) {
    const auto result = ros2_control_action_result(core::ComponentAction::kEnable,
                                                   {failed, error});
    EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
    ASSERT_TRUE(result.error.has_value());
    // Not the motion text "Motion refused" (live 2026-11-01).
    EXPECT_EQ(result.error->code, "component_action_refused");
    EXPECT_EQ(result.error->message, "refused");
  }
  const auto bridge = ros2_control_action_result(
      core::ComponentAction::kPrepare,
      {Outcome::kNotSent, core::ErrorDTO{"ros2_control_gripper_bridge_not_ready", "not ready", {}}});
  ASSERT_TRUE(bridge.error.has_value());
  EXPECT_EQ(bridge.error->code, "ros2_control_gripper_bridge_not_ready");
  // Never resent: an unknown effect stays unknown.
  EXPECT_EQ(ros2_control_action_result(core::ComponentAction::kDisable,
                                       {Outcome::kUncertain, error})
                .outcome,
            core::ActionOutcome::kUnknown);
}

} // namespace
} // namespace manyforge::roboplan_backend
