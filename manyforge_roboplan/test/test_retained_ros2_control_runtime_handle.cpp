// ROS runtime handle and Isaac guard binding against in-process stand-ins of
// the controller manager, the trajectory controller and the Isaac-side guard
// (design R6/§7.5 CCLC activation, §3.1 binding epochs, §4.3/Q41/Q43/Q46).
#include "ros2_control_fake_controller.hpp"
#include "../src/program_reset_position_observation.hpp"

#include <gtest/gtest.h>

#include <pal_statistics_msgs/msg/statistics.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// Test-only access to the lifecycle bookkeeping that a health read must not
/// create or advance. Transport facts remain supplied by the actual ROS peers.
class RetainedRos2ControlRuntimeHandleTestAccess final {
public:
  using Bookkeeping =
      std::map<core::ControlDomainId,
               std::tuple<std::uint64_t, bool, std::string,
                          core::LifecycleBlockingCauseClassDTO, bool>>;
  static Bookkeeping Capture(const RetainedRos2ControlRuntimeHandle &handle) {
    std::lock_guard lock(handle.mutex_);
    Bookkeeping result;
    for (const auto &[domain, state] : handle.domain_generations_)
      result.emplace(domain, std::make_tuple(state.publication_generation,
                                             state.first_observation_pending,
                                             state.source_signature,
                                             state.last_intervention_cause,
                                             state.exact_configuration_seen));
    return result;
  }
  static void RefreshEndpointFacts(RetainedRos2ControlRuntimeHandle &handle) {
    handle.RefreshEndpointFacts();
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
      const std::optional<core::SelectedControllerOccurrenceDTO>
          &successor_occurrence = std::nullopt,
      std::size_t *participant_constructions = nullptr,
      core::RuntimeParticipantBinding initial_binding = ArmBinding(),
      std::shared_ptr<core::IClock> receiver_clock = {},
      bool mock_reset_position = false,
      std::function<std::optional<std::string>()> process_fault = {},
      ros2_control_execution::Ros2TransportResources::ContextProvider context_provider = {},
      std::size_t executor_threads = 3U)
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
                                  "test-target-digest"};
    arm.measured_joint_state_lane = RetainedMeasuredJointStateLane{
        prefix + "/joint_states", 100,
        core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
        std::string(kMeasuredJointStateProfileId)};
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
        nullptr, binding, std::nullopt, successor_occurrence);
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

  [[nodiscard]] RetainedExternallyManagedDomainSnapshot Snapshot() {
    const auto read = handle->ReadLifecycleSnapshot(core::ControlDomainId("domain.arm"),
                                                    clock.now());
    if (!read.ok()) throw std::logic_error(read.error().message);
    return read.value();
  }

  [[nodiscard]] static std::optional<bool> Fact(const RetainedExternallyManagedDomainSnapshot &s,
                                                const std::string &id) {
    for (const auto &fact : s.facts) {
      if (fact.lifecycle_fact_id.value() == id) return fact.value.boolean_value;
    }
    return std::nullopt;
  }

  [[nodiscard]] static std::string Cause(const RetainedExternallyManagedDomainSnapshot &s) {
    for (const auto &fact : s.facts) {
      if (fact.lifecycle_fact_id.value() == core::kLifecycleBlockingCauseFactId) {
        return fact.value.token_value.value_or("");
      }
    }
    return {};
  }

  [[nodiscard]] bool WaitForState(core::ComponentLifecycleStateDTO state) {
    return fake::WaitUntil([&] { return Snapshot().component_state == state; });
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

using State = core::ComponentLifecycleStateDTO;
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
                        bool controller_manager_gripper = true)
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
    config.action_name = prefix + "/gripper_controller/gripper_cmd";
    config.group_id = "gripper";
    config.joint_name = "gripper_joint";
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
        std::string(kMeasuredJointStateProfileId)};
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
  RetainedExternallyManagedDomainSnapshot
  Snapshot(const char *domain = "domain.gripper") {
    auto value = handle->ReadLifecycleSnapshot(core::ControlDomainId(domain),
                                               clock.now());
    if (!value.ok())
      throw std::logic_error(value.error().message);
    return value.value();
  }
  bool WaitForState(core::ComponentLifecycleStateDTO state,
                    const char *domain = "domain.gripper") {
    return fake::WaitUntil(
        [&] { return Snapshot(domain).component_state == state; });
  }
  RetainedExternallyManagedPrimitiveOutcome
  Dispatch(const char *primitive, const char *domain = "domain.gripper") {
    return handle->lifecycle_primitive_dispatcher()(
        core::ControlDomainId(domain),
        core::LifecyclePrimitiveId(std::string("ros2_control.controller.") +
                                   primitive),
        {});
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

// The controller-manager peer hosts only the gripper controller. No JTC,
// motion publication or arm participant exists in this fixture.
struct GripperOnlyWorld final {
  explicit GripperOnlyWorld(
      std::optional<core::SelectedControllerOccurrenceDTO> occurrence = {},
      std::shared_ptr<core::IClock> receiver_clock = {})
      : owning_clock(receiver_clock ? std::move(receiver_clock)
                                    : std::make_shared<WallClock>()),
        clock(*owning_clock),
        prefix("/manyforge_gripper_only_" + fake::unique_suffix()),
        manager(prefix + "/controller_manager", "gripper_controller",
                "TestSystem", nullptr) {
    using Gripper = control_msgs::action::GripperCommand;
    action = rclcpp_action::create_server<Gripper>(
        manager.node(), prefix + "/gripper_controller/gripper_cmd",
        [](const rclcpp_action::GoalUUID &, std::shared_ptr<const Gripper::Goal>) {
          return rclcpp_action::GoalResponse::REJECT;
        },
        [](std::shared_ptr<rclcpp_action::ServerGoalHandle<Gripper>>) {
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [](std::shared_ptr<rclcpp_action::ServerGoalHandle<Gripper>>) {});
    auto sink = std::make_shared<NullSink>();
    auto resources =
        std::make_unique<ros2_control_execution::Ros2TransportResources>(
            "manyforge_gripper_only_" + fake::unique_suffix(), "/", 3U,
            sink, "gripper", clock);
    auto owned =
        std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
            *resources, sink, clock, scheduler, true);
    ros2_control_execution::Ros2GripperActionConfig config;
    config.action_name = prefix + "/gripper_controller/gripper_cmd";
    config.group_id = "gripper";
    config.joint_name = "gripper_joint";
    owned->configure_gripper_action(tool.value().command_target_id.value(), config);
    gripper = owned.get();
    const auto bound = handle->BindGripperOnly(
        std::move(resources), clock, std::move(owned), tool,
        {prefix + "/controller_manager", "gripper_controller", "TestSystem"},
        occurrence, owning_clock);
    if (!bound.ok()) throw std::logic_error(bound.error().message);
  }
  RetainedExternallyManagedDomainSnapshot Snapshot() {
    auto result = handle->ReadLifecycleSnapshot(
        tool.value().control_path->control_domain_id, clock.now());
    if (!result.ok()) throw std::logic_error(result.error().message);
    return result.value();
  }
  bool WaitForState(State state) {
    return fake::WaitUntil([&] { return Snapshot().component_state == state; });
  }
  RetainedExternallyManagedPrimitiveOutcome Dispatch(const char *primitive) {
    return handle->lifecycle_primitive_dispatcher()(
        tool.value().control_path->control_domain_id,
        core::LifecyclePrimitiveId(std::string("ros2_control.controller.") + primitive), {});
  }
  std::shared_ptr<core::IClock> owning_clock;
  core::IClock &clock;
  SimpleScheduler scheduler;
  core::RuntimeParticipantBinding tool{GripperBinding()};
  std::string prefix;
  fake::FakeControllerManager manager;
  rclcpp_action::Server<control_msgs::action::GripperCommand>::SharedPtr action;
  core::IGripperCommandBackend *gripper{};
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle =
      RetainedRos2ControlRuntimeHandle::Create();
};

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperOnlyControllerLifecycleHasNoMotionOccurrence) {
  GripperOnlyWorld world;
  EXPECT_FALSE(world.handle->participant());
  EXPECT_FALSE(world.handle->current_port());
  EXPECT_TRUE(world.handle->controller_joint_names().empty());
  EXPECT_FALSE(world.handle->ReadOwnedControllerOccurrence().arm_binding_epoch);
  EXPECT_TRUE(world.handle->OwnsGripper(world.gripper, world.tool));
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->gripper_transport_readiness_snapshot()->ready;
  }));
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  ASSERT_EQ(world.Dispatch("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  ASSERT_EQ(world.Dispatch("activate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_EQ(world.Dispatch("deactivate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  EXPECT_EQ(world.manager.switch_requests(), 2U);
  const auto occurrence = world.handle->ReadOwnedControllerOccurrence();
  EXPECT_FALSE(occurrence.arm_binding_epoch);
  ASSERT_EQ(occurrence.domains.size(), 1U);
  EXPECT_EQ(occurrence.domains.front().control_domain_id,
            world.tool.value().control_path->control_domain_id);
  EXPECT_GT(occurrence.domains.front().observation_generation, 0U);
  EXPECT_FALSE(world.handle->ReadLifecycleSnapshot(core::ControlDomainId("domain.arm"),
                                                  world.clock.now()).ok());
  EXPECT_FALSE(world.handle->participant());
  EXPECT_FALSE(world.handle->current_port());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperOnlySuccessorUsesItsExactDomainFloorWithoutCreatingArmEpoch) {
  const core::SelectedControllerOccurrenceDTO selected{
      std::nullopt, {{core::ControlDomainId("domain.gripper"), 5U}}};
  GripperOnlyWorld world(selected);
  EXPECT_EQ(world.handle->ReadOwnedControllerOccurrence(), selected);
  EXPECT_FALSE(world.handle->participant());
  EXPECT_FALSE(world.handle->current_port());
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(
      core::ControlDomainId("domain.gripper"), world.clock.now());
  ASSERT_TRUE(health.ok()) << health.error().message;
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);
  EXPECT_EQ(world.handle->ReadOwnedControllerOccurrence(), selected);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  const auto observed = world.handle->ReadOwnedControllerOccurrence();
  EXPECT_FALSE(observed.arm_binding_epoch);
  ASSERT_EQ(observed.domains.size(), 1U);
  EXPECT_EQ(observed.domains.front().control_domain_id,
            selected.domains.front().control_domain_id);
  EXPECT_GE(observed.domains.front().observation_generation, 5U);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperOnlyHealthUsesExactProcessCacheWithoutAdvancingOccurrence) {
  GripperOnlyWorld world;
  int observe_calls = 0, health_calls = 0;
  const std::optional<std::string> fault{"controller_launch_exited: gripper owner exited"};
  world.handle->InstallControllerProcessFaultSource(
      [&] { ++observe_calls; return fault; },
      [&] { ++health_calls; return core::Result<std::optional<std::string>>::Ok(fault); });
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(
      world.tool.value().control_path->control_domain_id, world.clock.now());
  ASSERT_TRUE(health.ok()) << health.error().message;
  EXPECT_EQ(health.value().observed_state, State::kFaulted);
  ASSERT_TRUE(health.value().diagnostic);
  EXPECT_EQ(health.value().diagnostic->message, *fault);
  EXPECT_EQ(observe_calls, 0);
  EXPECT_EQ(health_calls, 1);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);
  const auto observed = world.Snapshot();
  EXPECT_EQ(observe_calls, 1);
  EXPECT_EQ(observed.component_state, health.value().observed_state);
  EXPECT_EQ(observed.diagnostic, health.value().diagnostic);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ConfiguredGripperUsesRealInactiveControllerAndExactSwitches) {
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->gripper_transport_readiness_snapshot()->ready;
  }));
  EXPECT_EQ(world.Snapshot().component_state, State::kDeclared);
  EXPECT_EQ(ArmWorld::Fact(world.Snapshot(),
                           "external.controller_configuration_ready"),
            false);
  EXPECT_TRUE(world.manager.Requests().empty());
  ASSERT_TRUE(fake::WaitUntil([&] {
    static_cast<void>(world.Snapshot("domain.arm"));
    const auto health = world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.arm"), world.clock.now());
    return health.ok() && health.value().observed_state == State::kDeclared;
  }));
  const auto arm_prepared = world.Dispatch("prepare", "domain.arm");
  ASSERT_EQ(arm_prepared.known, core::LifecyclePrimitiveKnownResultDTO::kSucceeded)
      << (arm_prepared.error ? arm_prepared.error->message : std::string{"no error detail"});
  ASSERT_TRUE(world.Dispatch("prepare").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  EXPECT_TRUE(world.manager.Requests().empty());
  ASSERT_TRUE(world.Dispatch("activate", "domain.arm").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.Dispatch("activate").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive, "domain.arm"));
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_TRUE(world.Dispatch("deactivate").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.Dispatch("deactivate", "domain.arm").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  ASSERT_TRUE(world.WaitForState(State::kDisabled, "domain.arm"));
  EXPECT_TRUE(world.handle->gripper_transport_readiness_snapshot()->ready);
  EXPECT_FALSE(world.Snapshot().exact_controller_configuration_active);
  const auto requests = world.manager.Requests();
  ASSERT_EQ(requests.size(), 4U);
  EXPECT_EQ(requests[0].activate_controllers,
            std::vector<std::string>{"arm_controller"});
  EXPECT_EQ(requests[1].activate_controllers,
            std::vector<std::string>{"gripper_controller"});
  EXPECT_EQ(requests[2].deactivate_controllers,
            std::vector<std::string>{"gripper_controller"});
  EXPECT_EQ(requests[3].deactivate_controllers,
            std::vector<std::string>{"arm_controller"});
  for (const auto &request : requests) {
    EXPECT_EQ(request.strictness,
              GripperControllerManager::Switch::Request::STRICT);
    EXPECT_FALSE(request.activate_asap);
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     InactiveGripperPreparesWithoutActionServerButEnableNeedsTransport) {
  GripperWorld world(false);
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  EXPECT_FALSE(world.handle->gripper_transport_readiness_snapshot()->ready);
  const auto prepared = world.Dispatch("prepare");
  ASSERT_EQ(prepared.known, core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  EXPECT_EQ(ArmWorld::Fact(world.Snapshot(), "external.controller_loaded"),
            true);
  EXPECT_EQ(ArmWorld::Fact(world.Snapshot(), "external.transport_ready"), true);
  EXPECT_TRUE(world.manager.Requests().empty());
  ASSERT_EQ(world.Dispatch("activate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDegraded));
  EXPECT_FALSE(world.Snapshot().exact_controller_configuration_active);
  EXPECT_EQ(ArmWorld::Cause(world.Snapshot()), "availability");
  const auto requests = world.manager.Requests();
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front().activate_controllers,
            std::vector<std::string>{"gripper_controller"});
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperMissingOrExternallyActiveCannotPrepare) {
  GripperWorld world;
  world.manager.SetGripper(false, false);
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDegraded));
  EXPECT_FALSE(world.Dispatch("prepare").known ==
               core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  EXPECT_TRUE(world.manager.Requests().empty());
  world.manager.SetGripper(true, true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return ArmWorld::Fact(world.Snapshot(), "external.controller_loaded") ==
           true;
  }));
  EXPECT_EQ(world.Snapshot().component_state, State::kDegraded);
  EXPECT_FALSE(world.Dispatch("prepare").known ==
               core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RefusedGripperSwitchCannotReportActive) {
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  ASSERT_TRUE(world.Dispatch("prepare").known ==
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  world.manager.Refuse(true);
  const auto refused = world.Dispatch("activate");
  EXPECT_EQ(refused.dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  EXPECT_EQ(refused.known, core::LifecyclePrimitiveKnownResultDTO::kFailed);
  EXPECT_EQ(refused.effect, core::LifecyclePrimitiveEffectDTO::kPossible);
  ASSERT_TRUE(refused.error);
  EXPECT_EQ(refused.error->code, "execution_rejected");
  EXPECT_EQ(refused.error->message, "the controller manager refused the controller switch");
  EXPECT_EQ(refused.error->details.at("reason"), "ros2_control_activation_refused");
  EXPECT_FALSE(refused.error->details.contains("controller_response_message"));
  EXPECT_EQ(refused.error->details.size(), 4U);
  EXPECT_EQ(refused.error->details.at("controller_name"), "gripper_controller");
  EXPECT_EQ(refused.error->details.at("switch_action"), "activate");
  EXPECT_EQ(refused.error->details.at("controller_service"),
            world.prefix + "/controller_manager/switch_controller");
  EXPECT_EQ(world.Snapshot().component_state, State::kPrepared);
  EXPECT_FALSE(world.Snapshot().exact_controller_configuration_active);
  const auto foreign = world.Dispatch("activate", "domain.foreign");
  EXPECT_EQ(foreign.dispatch,
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  ASSERT_EQ(world.manager.Requests().size(), 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ControllerRefusalResponseIsScopedToActualArmAndGripper) {
  for (const auto* domain : {"domain.arm", "domain.gripper"}) {
    SCOPED_TRACE(domain);
    GripperWorld world;
    const std::string controller = std::string(domain) == "domain.arm"
                                       ? "arm_controller" : "gripper_controller";
    const auto message = "Cannot activate selected controller: " + controller;
    world.handle->StartBackend();
    ASSERT_TRUE(world.WaitForState(State::kDeclared, domain));
    ASSERT_EQ(world.Dispatch("prepare", domain).known,
              core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    world.manager.Refuse(true, message);
    const auto refused = world.Dispatch("activate", domain);
    EXPECT_EQ(refused.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
    EXPECT_EQ(refused.known, core::LifecyclePrimitiveKnownResultDTO::kFailed);
    EXPECT_EQ(refused.effect, core::LifecyclePrimitiveEffectDTO::kPossible);
    ASSERT_TRUE(refused.error);
    EXPECT_EQ(refused.error->code, "execution_rejected");
    EXPECT_EQ(refused.error->message, "the controller manager refused the controller switch");
    EXPECT_EQ(refused.error->details.at("reason"), "ros2_control_activation_refused");
    EXPECT_EQ(refused.error->details.at("controller_response_message"), message);
    EXPECT_EQ(refused.error->details.at("controller_name"), controller);
    EXPECT_EQ(refused.error->details.at("switch_action"), "activate");
    EXPECT_EQ(refused.error->details.at("controller_service"),
              world.prefix + "/controller_manager/switch_controller");
    EXPECT_EQ(refused.error->details.size(), 5U);
    for (const auto& [key, value] : refused.error->details)
      EXPECT_LE(value.size(), 256U) << key;
    EXPECT_EQ(world.Snapshot(domain).component_state, State::kPrepared);
    const auto requests = world.manager.Requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().activate_controllers, std::vector<std::string>{controller});
    EXPECT_TRUE(requests.front().deactivate_controllers.empty());
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RefusedArmDeactivationCarriesUtf8BoundedResponseWithoutChangingOutcome) {
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared, "domain.arm"));
  ASSERT_EQ(world.Dispatch("prepare", "domain.arm").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_EQ(world.Dispatch("activate", "domain.arm").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive, "domain.arm"));
  // The prefix before the truncation marker would split this actual UTF-8
  // character without the existing complete-prefix rule.
  world.manager.Refuse(true, std::string(244U, 'x') + "é" + std::string(80U, 'y'));
  const auto refused = world.Dispatch("deactivate", "domain.arm");
  EXPECT_EQ(refused.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  EXPECT_EQ(refused.known, core::LifecyclePrimitiveKnownResultDTO::kFailed);
  EXPECT_EQ(refused.effect, core::LifecyclePrimitiveEffectDTO::kPossible);
  ASSERT_TRUE(refused.error);
  EXPECT_EQ(refused.error->code, "execution_rejected");
  EXPECT_EQ(refused.error->message, "the controller manager refused the controller switch");
  EXPECT_EQ(refused.error->details.at("reason"), "ros2_control_deactivation_refused");
  const auto& message = refused.error->details.at("controller_response_message");
  EXPECT_EQ(message, std::string(244U, 'x') + "[truncated]");
  EXPECT_LE(message.size(), 256U);
  EXPECT_EQ(refused.error->details.at("controller_name"), "arm_controller");
  EXPECT_EQ(refused.error->details.at("switch_action"), "deactivate");
  EXPECT_EQ(refused.error->details.at("controller_service"),
            world.prefix + "/controller_manager/switch_controller");
  EXPECT_EQ(refused.error->details.size(), 5U);
  for (const auto& [key, value] : refused.error->details)
    EXPECT_LE(value.size(), 256U) << key;
  EXPECT_EQ(world.Snapshot("domain.arm").component_state, State::kActive);
  const auto requests = world.manager.Requests();
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests.back().deactivate_controllers, std::vector<std::string>{"arm_controller"});
  EXPECT_TRUE(requests.back().activate_controllers.empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     SuccessorConsumesOwnedEpochAndFirstGenerationWithoutChangingSurvivors) {
  ArmWorld predecessor;
  ArmWorld survivor;
  const auto old_port = predecessor.handle->current_port();
  const auto survivor_port = survivor.handle->current_port();
  const auto untouched = survivor.handle->ReadOwnedControllerOccurrence();
  auto owned = predecessor.handle->ReadOwnedControllerOccurrence();
  ASSERT_TRUE(owned.locally_valid());
  ASSERT_EQ(owned.domains.size(), 1U);
  EXPECT_EQ(owned.domains.front().observation_generation, 0U);
  EXPECT_EQ(predecessor.Snapshot().source_generation, 1U);
  owned = predecessor.handle->ReadOwnedControllerOccurrence();
  const auto next = core::ComputeSelectedControllerSuccessorOccurrence(
      owned, {core::ControlDomainId("domain.arm")}, true);
  ASSERT_TRUE(next.ok());
  ArmWorld successor("arm_controller", next.value());
  EXPECT_EQ(successor.handle->ReadOwnedControllerOccurrence(), next.value());
  EXPECT_EQ(successor.handle->current_port()->ReadReadiness().binding_epoch,
            core::ExecutionBindingEpoch(2U));
  const auto first = successor.Snapshot();
  EXPECT_EQ(first.source_generation, 2U);
  EXPECT_EQ(successor.Snapshot().source_generation, first.source_generation);
  EXPECT_FALSE(successor.handle->arm_activity().observed);
  EXPECT_FALSE(
      successor.handle->current_port()->ReadReadiness().controller_bound);
  EXPECT_NE(successor.handle->current_port(), old_port);
  EXPECT_EQ(predecessor.handle->current_port(), old_port);
  EXPECT_TRUE(predecessor.handle->OwnsBinding(predecessor.binding));
  EXPECT_EQ(predecessor.handle->ReadOwnedControllerOccurrence(), owned);
  EXPECT_EQ(survivor.handle->current_port(), survivor_port);
  EXPECT_EQ(survivor.handle->ReadOwnedControllerOccurrence(), untouched);
  EXPECT_EQ(predecessor.manager.switch_requests(), 0U);
  EXPECT_EQ(successor.manager.switch_requests(), 0U);
  EXPECT_EQ(survivor.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     InvalidSuccessorRefusesBeforeConstructingTheParticipant) {
  const core::SelectedControllerOccurrenceDTO valid{
      core::ExecutionBindingEpoch(2U),
      {{core::ControlDomainId("domain.arm"), 2U}}};
  auto missing_arm = valid;
  missing_arm.arm_binding_epoch.reset();
  auto zero_generation = valid;
  zero_generation.domains.front().observation_generation = 0U;
  auto foreign_domain = valid;
  foreign_domain.domains.front().control_domain_id =
      core::ControlDomainId("foreign.arm");
  auto duplicate_domain = valid;
  duplicate_domain.domains.push_back(duplicate_domain.domains.front());
  for (const auto &seed :
       {missing_arm, zero_generation, foreign_domain, duplicate_domain}) {
    std::size_t constructed = 0U;
    EXPECT_THROW(ArmWorld("arm_controller", seed, &constructed),
                 std::logic_error);
    EXPECT_EQ(constructed, 0U);
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MaximumOwnedFloorsRefuseSuccessorAndNeverWrapObservation) {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ArmWorld predecessor("arm_controller",
                       core::SelectedControllerOccurrenceDTO{
                           core::ExecutionBindingEpoch(maximum),
                           {{core::ControlDomainId("domain.arm"), maximum}}});
  bool failed = false;
  predecessor.handle->InstallControllerProcessFaultSource([&] {
    return failed ? std::optional<std::string>("exact launch exited")
                  : std::nullopt;
  });
  const auto owned = predecessor.handle->ReadOwnedControllerOccurrence();
  const auto port = predecessor.handle->current_port();
  EXPECT_FALSE(core::ComputeSelectedControllerSuccessorOccurrence(
                   owned, {core::ControlDomainId("domain.arm")}, true)
                   .ok());
  EXPECT_EQ(predecessor.Snapshot().source_generation, maximum);
  failed = true;
  const auto changed = predecessor.handle->ReadLifecycleSnapshot(
      core::ControlDomainId("domain.arm"), predecessor.clock.now());
  ASSERT_FALSE(changed.ok());
  EXPECT_EQ(changed.error().details.at("reason"),
            "ros2_control_lifecycle_generation_exhausted");
  EXPECT_EQ(predecessor.handle->ReadOwnedControllerOccurrence(), owned);
  EXPECT_EQ(predecessor.handle->current_port(), port);
  EXPECT_EQ(predecessor.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MaximumSeedRefusesReactivationBeforeRetiringTheAdapter) {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto successor = core::ComputeSelectedControllerSuccessorOccurrence(
      {core::ExecutionBindingEpoch(maximum - 1U),
       {{core::ControlDomainId("domain.arm"), 0U}}},
      {core::ControlDomainId("domain.arm")}, true);
  ASSERT_TRUE(successor.ok());
  ArmWorld world("arm_controller", successor.value());
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.controller_loaded &&
           activity.hardware_active;
  }));
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_EQ(world.handle->DeactivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  const auto port = world.handle->current_port();
  const auto readiness = port->ReadReadiness();
  ASSERT_TRUE(readiness.hardware_active);
  EXPECT_EQ(readiness.binding_epoch, core::ExecutionBindingEpoch(maximum));
  const auto owned = world.handle->ReadOwnedControllerOccurrence();
  const auto switches = world.manager.switch_requests();
  const auto refused = world.handle->ActivateArmController({});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"),
            "ros2_control_binding_epoch_exhausted");
  EXPECT_EQ(world.handle->current_port(), port);
  EXPECT_EQ(port->ReadReadiness(), readiness);
  EXPECT_EQ(world.handle->ReadOwnedControllerOccurrence(), owned);
  EXPECT_EQ(world.manager.switch_requests(), switches);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetPublishesOnceAndRotatesBeforeSuccessorActivation) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
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

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetSwitchActivityUsesOrdinaryHealthWithoutPhaseEvidence) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  bool saw_delayed_activity = false;
  std::size_t inactive_switch_samples = 0U;
  std::optional<MockResetPositionStepDeadline> writer_deadline;
  const auto check_inactive = [&] {
    world.manager.SetControllerActivity(false, false);
    EXPECT_TRUE(fake::WaitUntil([&] {
      return !world.handle->arm_activity().controller_active &&
             !world.handle->mock_reset_writer_active();
    }));
    const auto snapshot = world.Snapshot();
    EXPECT_EQ(snapshot.component_state, State::kDegraded);
    EXPECT_EQ(ArmWorld::Cause(snapshot), "availability");
    EXPECT_EQ(ArmWorld::Fact(snapshot, "external.controller_loaded"), true);
    EXPECT_EQ(ArmWorld::Fact(snapshot, "external.transport_ready"), true);
    EXPECT_EQ(ArmWorld::Fact(snapshot, "external.fault_present"), false);
    EXPECT_TRUE(std::none_of(snapshot.facts.begin(), snapshot.facts.end(),
                            [](const auto& fact) {
      return fact.lifecycle_fact_id.value() ==
             "external.mock_reset_position_transition";
    }));
    ++inactive_switch_samples;
    world.manager.SetControllerActivity(false, true);
    EXPECT_TRUE(fake::WaitUntil(
        [&] { return world.handle->mock_reset_writer_active(); }));
  };
  const auto reset = world.handle->ResetMockPosition(
      {}, [&](const auto &transition, const auto &deadline) {
        if (transition.phase == MockResetPositionPhase::kSwitchingToWriter) {
          if (!transition.switch_acknowledged) {
            world.manager.SetActivityPublicationEnabled(false);
          } else {
            // A positive service reply does not stand in for CM activity.
            EXPECT_FALSE(world.handle->mock_reset_writer_active());
            saw_delayed_activity = true;
            world.manager.SetActivityPublicationEnabled(true);
            EXPECT_TRUE(fake::WaitUntil(
                [&] { return world.handle->mock_reset_writer_active(); }));
            check_inactive();
          }
        }
        if (transition.phase == MockResetPositionPhase::kSettingState) {
          if (!transition.target_published) writer_deadline = deadline;
          else {
            EXPECT_EQ(writer_deadline, deadline);
            EXPECT_TRUE(fake::WaitUntil(
                [&] { return world.manager.reset_commands().size() == 1U; }));
          }
        }
        if (transition.phase == MockResetPositionPhase::kSwitchingToJtc) {
          if (!transition.switch_acknowledged) check_inactive();
          else EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.handle->mock_reset_successor_ready(); }));
        }
        return core::Result<void>::Ok();
      });
  EXPECT_TRUE(saw_delayed_activity);
  EXPECT_EQ(inactive_switch_samples, 2U);
  EXPECT_EQ(reset.outcome, Outcome::kSucceeded) << SwitchFailureText(reset);
  EXPECT_FALSE(world.manager.writer_active());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetStepEntryDeadlineIncludesValidationBeforeAnySwitch) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto reset = world.handle->ResetMockPosition(
      {}, [&](const auto &transition, const MockResetPositionStepDeadline deadline) {
        EXPECT_EQ(transition.phase, MockResetPositionPhase::kSwitchingToWriter);
        EXPECT_FALSE(transition.switch_acknowledged);
        // Contract142 explicitly includes local validation in the same stock
        // step limit. No fresh one-second response budget starts afterwards.
        std::this_thread::sleep_until(deadline);
        return core::Result<void>::Ok();
      });
  EXPECT_EQ(reset.outcome, Outcome::kNotSent);
  ASSERT_TRUE(reset.error);
  EXPECT_EQ(reset.error->details.at("reason"), "mock_reset_step_timeout");
  EXPECT_EQ(world.manager.switch_requests(), 1U);  // Initial Enable only.
  EXPECT_TRUE(world.manager.reset_commands().empty());
  EXPECT_TRUE(world.handle->current_port()->ReadReadiness().controller_bound);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetDoesNotHideProcessExitTransportLossOrHardwareFailure) {
  for (const int failure : {0, 1, 2}) {
    const bool process_exit = failure == 0;
    const bool hardware_failure = failure == 2;
    std::atomic<bool> exited{false};
    ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true, [&] {
      return exited.load() ? std::optional<std::string>("controller exited")
                           : std::nullopt;
    });
    world.handle->StartBackend();
    ASSERT_TRUE(fake::WaitUntil(
        [&] { return world.handle->mock_reset_writer_inactive(); }));
    ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
              Outcome::kSucceeded);
    ASSERT_TRUE(world.WaitForState(State::kActive));
    bool checked_failure = false;
    bool attempted_switchback = false;
    const auto reset = world.handle->ResetMockPosition(
        {}, [&](const auto &transition, const auto &) {
      if (failure == 1 &&
          transition.phase == MockResetPositionPhase::kSwitchingToJtc &&
          !transition.switch_acknowledged)
        attempted_switchback = true;
      if (transition.phase == MockResetPositionPhase::kSwitchingToWriter &&
          transition.switch_acknowledged) {
        EXPECT_TRUE(fake::WaitUntil(
            [&] { return world.handle->mock_reset_writer_active(); }));
      }
      if (transition.phase != MockResetPositionPhase::kSettingState ||
          !transition.target_published)
        return core::Result<void>::Ok();
      if (process_exit) exited = true;
      else if (hardware_failure) world.manager.SetHardwareActive(false);
      else {
        world.manager.StopAndDestroyEndpoints();
        checked_failure = true;
        // Availability belongs to the accepted Reset step. Let the actual
        // switchback discover the missing service or expire its SAME deadline;
        // do not substitute a synthetic lifecycle error for that outcome.
        return core::Result<void>::Ok();
      }
      EXPECT_TRUE(fake::WaitUntil([&] {
        const auto snapshot = world.Snapshot();
        return process_exit ? snapshot.component_state == State::kFaulted
                            : ArmWorld::Cause(snapshot) == "unknown";
      }));
      const auto snapshot = world.Snapshot();
      if (process_exit) {
        EXPECT_EQ(ArmWorld::Cause(snapshot), "fault");
        EXPECT_EQ(ArmWorld::Fact(snapshot, "external.fault_present"), true);
        EXPECT_EQ(snapshot.component_state, State::kFaulted);
      } else if (hardware_failure) {
        EXPECT_EQ(ArmWorld::Cause(snapshot), "unknown");
        EXPECT_EQ(ArmWorld::Fact(snapshot, "external.controller_loaded"), false);
      }
      checked_failure = true;
      return core::Result<void>::Err(
          {"execution_rejected", "actual source failed during Reset", {}});
    });
    EXPECT_TRUE(checked_failure);
    EXPECT_NE(reset.outcome, Outcome::kSucceeded);
    EXPECT_FALSE(world.handle->current_port()->ReadReadiness().controller_bound);
    if (failure == 1) {
      EXPECT_TRUE(attempted_switchback);
      ASSERT_TRUE(reset.error);
      const auto &reason = reset.error->details.at("reason");
      EXPECT_TRUE(reason == "ros2_control_switch_service_unavailable" ||
                  reason == "ros2_control_switch_timeout") << reason;
      EXPECT_EQ(world.manager.switch_requests(), 2U);
      EXPECT_LE(world.manager.reset_commands().size(), 1U);
    }
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetFailureContainsWriterAndNeverReactivatesOrResends) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_active; }));
  std::vector<MockResetPositionPhase> phases;
  const auto reset = world.handle->ResetMockPosition(
      {}, [&](const auto &transition, const auto &) {
        phases.push_back(transition.phase);
        if (transition.phase == MockResetPositionPhase::kSwitchingToWriter &&
            transition.switch_acknowledged) {
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.handle->mock_reset_writer_active(); }));
        }
        if (transition.phase == MockResetPositionPhase::kSettingState &&
            transition.target_published) {
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.manager.reset_commands().size() == 1U; }));
          return core::Result<void>::Err(
              {"execution_rejected", "fresh measured target unavailable", {}});
        }
        return core::Result<void>::Ok();
      });
  EXPECT_EQ(reset.outcome, Outcome::kUncertain);
  ASSERT_TRUE(reset.error);
  EXPECT_NE(reset.error->message.find("Restart the workbench"),
            std::string::npos);
  EXPECT_EQ(phases.back(), MockResetPositionPhase::kContaining);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  EXPECT_FALSE(world.manager.writer_active());
  EXPECT_FALSE(world.handle->arm_activity().controller_active);
  EXPECT_FALSE(world.handle->current_port()->ReadReadiness().controller_bound);
  EXPECT_EQ(world.handle->ReadOwnedControllerOccurrence().arm_binding_epoch,
            core::ExecutionBindingEpoch(1U));
  EXPECT_EQ(world.manager.reset_commands(),
            (std::vector<std::vector<double>>{{0.125}}));
  EXPECT_EQ(world.manager.switch_requests(), 3U);
  EXPECT_EQ(world.handle
                ->ResetMockPosition({},
                                    [](const auto &, const auto &) {
                                      return core::Result<void>::Ok();
                                    })
                .outcome,
            Outcome::kNotSent);
  EXPECT_EQ(world.manager.switch_requests(), 3U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetLostSwitchReplyNeverUsesActivityAsSuccess) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
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
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
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
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->mock_reset_writer_inactive(); }));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
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
     ActivePostconditionWaitsForItsOwnActionClientDiscovery) {
  // Activity and switch ACK are present before this exact action route exists.
  // The existing fake JTC later supplies it; no proxy client or discovery wait.
  ArmWorld world("delayed_arm_controller");
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.controller_loaded &&
           activity.hardware_active;
  }));
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->arm_activity().controller_active;
  }));
  const auto port = world.handle->current_port();
  const auto pending = world.Snapshot();
  EXPECT_EQ(pending.component_state, State::kDegraded);
  EXPECT_FALSE(pending.exact_controller_configuration_active);
  EXPECT_EQ(ArmWorld::Fact(pending, "external.controller_configuration_ready"),
            std::optional(false));
  EXPECT_TRUE(port->ReadReadiness().controller_bound);
  core::TrajectoryDTO hold;
  hold.joint_names = {"joint_1"};
  hold.waypoints = {{0.0, {0.0}, {0.0}, {0.0}},
                    {1.0, {0.0}, {0.0}, {0.0}}};
  const core::ExecutionGoalKey key{core::ControllerGoalId("goal.discovery"),
                                 core::ControllerGoalGeneration(1U)};
  const auto premature = port->Submit(key, hold);
  EXPECT_EQ(premature.dispatch, core::ExecutionPortDispatch::kNotDispatched);
  ASSERT_TRUE(premature.error);
  EXPECT_EQ(premature.error->message, "FJT action server is not discovered");

  auto discovered = std::make_unique<fake::FakeJtc>(
      world.prefix + "/delayed_arm_controller",
      std::vector<std::string>{"source_joint_1"});
  discovered->SetActive(true);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto ready = world.Snapshot();
  EXPECT_TRUE(ready.exact_controller_configuration_active);
  EXPECT_GT(ready.source_generation, pending.source_generation);
  EXPECT_EQ(ArmWorld::Fact(ready, "external.controller_configuration_ready"),
            std::optional(true));
  EXPECT_TRUE(port->ReadReadiness().controller_bound);
  // The never-dispatched key is still unused: send once on the actual client.
  EXPECT_EQ(port->Submit(key, hold).dispatch,
            core::ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(fake::WaitUntil([&] { return discovered->goals_received() == 1U; }));
  const auto status_topic =
      world.prefix + "/delayed_arm_controller/follow_joint_trajectory/_action/status";
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->node()->count_publishers(status_topic) > 0U;
  }));
  const auto established_readiness = port->ReadReadiness();
  discovered.reset();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->node()->count_publishers(status_topic) == 0U;
  }));
  EXPECT_EQ(port->ReadReadiness(), established_readiness);
  const auto later = world.Snapshot();
  EXPECT_EQ(later.component_state, State::kActive);
  EXPECT_TRUE(later.exact_controller_configuration_active);
  EXPECT_EQ(later.source_generation, ready.source_generation);

  // A fresh adapter epoch must establish its own discovery postcondition.
  ASSERT_EQ(world.handle->DeactivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->arm_activity().controller_active;
  }));
  const auto reactivated = world.Snapshot();
  EXPECT_EQ(reactivated.component_state, State::kDegraded);
  EXPECT_FALSE(reactivated.exact_controller_configuration_active);
  EXPECT_TRUE(world.handle->current_port()->ReadReadiness().controller_bound);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ProductResealPreservesTheLivePortEpochAndLifecycleObservation) {
  ArmWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.controller_loaded &&
           activity.hardware_active;
  }));
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto port = world.handle->current_port();
  const auto participant = world.handle->participant();
  const auto before = world.Snapshot();
  auto value = world.binding.value();
  value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  value.graph_revision = core::GraphRevision("candidate.graph");
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok());
  const auto candidate = std::move(sealed.value());
  const auto rebound = world.handle->RebindMotionRoute(candidate, {});
  ASSERT_TRUE(rebound.ok()) << rebound.error().message;
  EXPECT_EQ(world.handle->current_port(), port);
  EXPECT_EQ(world.handle->participant(), participant);
  EXPECT_TRUE(
      world.handle->OwnsBackend(participant.get(), candidate, std::nullopt));
  EXPECT_FALSE(world.handle->OwnsBinding(world.binding));
  EXPECT_TRUE(world.handle->OwnsBinding(candidate));
  EXPECT_EQ(world.manager.switch_requests(), 1U);
  EXPECT_EQ(world.Snapshot().component_state, before.component_state);
  EXPECT_EQ(world.Snapshot().source_generation, before.source_generation);
  auto foreign_value = candidate.value();
  foreign_value.resources.front().resource_id =
      core::ResourceId("foreign.joint");
  auto foreign =
      core::verify_runtime_participant_binding(std::move(foreign_value));
  ASSERT_TRUE(foreign.ok());
  EXPECT_FALSE(world.handle->RebindMotionRoute(foreign.value(), {}).ok());
  EXPECT_TRUE(world.handle->OwnsBinding(candidate));
  EXPECT_TRUE(world.handle->RebindMotionRoute(world.binding, {}).ok());
  EXPECT_TRUE(world.handle->OwnsBinding(world.binding));
  EXPECT_EQ(world.handle->current_port(), port);
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
  auto peer = std::make_unique<ArmWorld>("arm_controller", std::nullopt, nullptr, peer_binding.value());
  const auto port = survivor.handle->current_port();
  const auto participant = survivor.handle->participant();
  const auto readiness = port->ReadReadiness();

  // Replace the peer occurrence inside the same shared runtime unit. This
  // must not rewrite the survivor's component/provider epoch or live port.
  auto retired_peer = std::move(peer);
  const auto replaced_peer_port = retired_peer->handle->current_port();
  peer = std::make_unique<ArmWorld>("arm_controller", std::nullopt, nullptr, peer_binding.value());
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
  peer = std::make_unique<ArmWorld>("arm_controller", std::nullopt, nullptr, peer_binding.value());
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


TEST(RetainedRos2ControlRuntimeHandleTest,
     ActualRosArmRefusesMotionWhileDeclaredAndPreparedBeforeEnable) {
  ArmWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.controller_loaded &&
           activity.hardware_active && !activity.controller_active;
  }));
  const auto status_topic =
      world.prefix + "/arm_controller/follow_joint_trajectory/_action/status";
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->node()->count_publishers(status_topic) > 0U;
  }));
  const auto initial_goals = world.jtc.goals_received();
  const auto initial_switches = world.manager.switch_requests();
  core::TrajectoryDTO hold;
  hold.joint_names = {"joint_1"};
  hold.waypoints = {{0.0, {0.0}, {0.0}, {0.0}},
                    {1.0, {0.0}, {0.0}, {0.0}}};
  const core::ExecutionGoalKey key{
      core::ControllerGoalId("goal.before-enable"),
      core::ControllerGoalGeneration(1U)};
  const auto expect_closed = [&](State expected) {
    EXPECT_EQ(world.Snapshot().component_state, expected);
    const auto port = world.handle->current_port();
    ASSERT_NE(port, nullptr);
    const auto readiness = port->ReadReadiness();
    EXPECT_FALSE(readiness.controller_active);
    EXPECT_FALSE(readiness.controller_bound);
    EXPECT_TRUE(readiness.hardware_active);
    const auto refused = port->Submit(key, hold);
    EXPECT_EQ(refused.dispatch, core::ExecutionPortDispatch::kNotDispatched);
    EXPECT_EQ(refused.refusal, core::ExecutionPortRefusal::kNotReady);
    EXPECT_EQ(world.jtc.goals_received(), initial_goals);
    EXPECT_EQ(world.manager.switch_requests(), initial_switches);
  };
  expect_closed(State::kDeclared);
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  expect_closed(State::kPrepared);

  // The same valid trajectory/key becomes usable only after this handle's
  // actual activation switch and exact Active postcondition.
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto enabled = world.handle->current_port();
  ASSERT_NE(enabled, nullptr);
  EXPECT_EQ(world.manager.switch_requests(), initial_switches + 1U);
  const auto sent = enabled->Submit(key, hold);
  ASSERT_EQ(sent.dispatch, core::ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.jtc.goals_received() == initial_goals + 1U; }));
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     CclcPrimitivesSwitchTheInactiveLoadedControllerAndReportItsLifecycle) {
  ArmWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    return activity.observed && activity.controller_loaded && activity.hardware_active;
  }));
  // Loaded inactive by its spawner: declared, waiting, nothing to fault.
  auto snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kDeclared);
  EXPECT_EQ(ArmWorld::Cause(snapshot), "none");
  EXPECT_EQ(ArmWorld::Fact(snapshot, "external.controller_loaded"), std::optional(true));
  EXPECT_EQ(ArmWorld::Fact(snapshot, "external.controller_configuration_ready"),
            std::optional(false));
  EXPECT_EQ(world.manager.switch_requests(), 0U);

  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  EXPECT_EQ(world.Snapshot().component_state, State::kPrepared);
  EXPECT_EQ(world.manager.switch_requests(), 0U);  // Prepare switches nothing

  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  EXPECT_EQ(world.manager.switch_requests(), 1U);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  snapshot = world.Snapshot();
  EXPECT_TRUE(snapshot.exact_controller_configuration_active);
  EXPECT_EQ(ArmWorld::Cause(snapshot), "none");
  const auto first = world.handle->current_port();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto ready = first->ReadReadiness();
    return ready.controller_active && ready.controller_bound && ready.hardware_active &&
           ready.binding_epoch == core::ExecutionBindingEpoch(1U);
  }));

  // Disable deactivates; Enable after it is a reactivation: next epoch.
  ASSERT_EQ(world.handle->DeactivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  EXPECT_FALSE(first->ReadReadiness().controller_bound);
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  const auto second = world.handle->current_port();
  EXPECT_NE(second, first);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto ready = second->ReadReadiness();
    return ready.controller_active && ready.controller_bound &&
           ready.binding_epoch == core::ExecutionBindingEpoch(2U);
  }));
  EXPECT_EQ(world.manager.switch_requests(), 3U);

  // An unrequested deactivation after the target was reached: availability
  // (CCLC's provider_lost at the reached target), never a bound binding.
  world.manager.SetControllerActive(false);
  ASSERT_TRUE(world.WaitForState(State::kDegraded));
  snapshot = world.Snapshot();
  EXPECT_EQ(ArmWorld::Cause(snapshot), "availability");
  EXPECT_EQ(ArmWorld::Fact(snapshot, "external.controller_inactive_unrequested"),
            std::optional(true));
  EXPECT_FALSE(second->ReadReadiness().controller_active);
  EXPECT_FALSE(second->ReadReadiness().controller_bound);
  // A later activation by anyone else never rebinds this epoch.
  world.manager.SetControllerActive(true);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().controller_active; }));
  EXPECT_FALSE(second->ReadReadiness().controller_bound);
  EXPECT_EQ(world.Snapshot().component_state, State::kDegraded);

  // Recover re-prepares (switching the stray activation off); the next
  // Enable is epoch 3.
  ASSERT_EQ(world.handle->RecoverArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  const auto third = world.handle->current_port();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return third->ReadReadiness().binding_epoch == core::ExecutionBindingEpoch(3U) &&
           third->ReadReadiness().controller_bound;
  }));

  // Hardware lost after the target was reached is ambiguous: unknown.
  world.manager.SetHardwareActive(false);
  ASSERT_TRUE(fake::WaitUntil([&] { return ArmWorld::Cause(world.Snapshot()) == "unknown"; }));
  EXPECT_EQ(world.Snapshot().component_state, State::kDegraded);

  // The catalog's primitives reach only the arm domain.
  const auto dispatch = world.handle->lifecycle_primitive_dispatcher();
  const auto foreign = dispatch(core::ControlDomainId("domain.gripper"),
                                core::LifecyclePrimitiveId("ros2_control.controller.activate"),
                                {});
  EXPECT_EQ(foreign.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  const auto unknown = dispatch(core::ControlDomainId("domain.arm"),
                                core::LifecyclePrimitiveId("ros2_control.controller.unknown"),
                                {});
  EXPECT_EQ(unknown.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
}

[[nodiscard]] std::shared_ptr<const content_identity::CanonicalValue>
TestLaunchDiagnosticSnapshot() {
  return std::make_shared<
      const content_identity::
          CanonicalValue>(content_identity::parse_canonical_json(
      R"snapshot({"context":{"controllable_component_id":"component.arm","generator_identity":null,"graph_revision":"graph.1","launch_identity":"sha256:0000000000000000000000000000000000000000000000000000000000000000","request_context":"Apply=apply.7","retained_bundle_id":"bundle.1","runtime_epoch":"epoch.1","runtime_unit_id":"robot_cell.ros2_control_execution"},"core_dump_link":null,"exit":{"core_dumped":false,"observed_utc":"2026-10-11T18:00:00Z","signal":11,"status":null},"generated":{"controllers_sha256":"sha256:2222222222222222222222222222222222222222222222222222222222222222","robot_description_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111"},"launch_readiness":{"arm_controller_active":null,"binding_epoch":null,"captured_utc":null,"controller_bound":null,"controller_loaded":null,"controller_manager_present":null,"controller_observed":null,"hardware_ready":null,"joint_sample_available":null,"known":null,"sample_age_ns":null},"output":{"dropped_forward_bytes":0,"dropped_forward_lines":0,"invalid_text_bytes":0,"line_count":1,"tail":"controller crashed\n","truncated":false},"settings":{"argv":["ros2","launch"],"arm_controller_name":"arm","controller_profile":"ur5","controllers_yaml":"arm: effective settings","frame_prefix":"","gripper_controller_name":null,"retained_configuration_json":"{}","runtime_namespace":"/robot"}})snapshot"));
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     AnExitedControllerLaunchFailsTheComponentUntilANewLaunch) {
  // Q80: the Manager-owned controller launch exits while the cell is active.
  ArmWorld world;
  std::mutex mutex;
  std::optional<std::string> exit_reason;
  world.handle->InstallControllerProcessFaultSource([&] {
    std::lock_guard lock(mutex);
    return exit_reason;
  });
  EXPECT_THROW(world.handle->InstallControllerProcessFaultSource(
                   [] { return std::optional<std::string>{}; }),
               std::logic_error);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().controller_loaded; }));
  const auto launch_prepare_result = world.handle->PrepareArmController();
  ASSERT_EQ(launch_prepare_result.outcome, Outcome::kSucceeded)
      << SwitchFailureText(launch_prepare_result);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  {
    std::lock_guard lock(mutex);
    exit_reason = "controller_launch_exited: controller launch was terminated "
                  "by signal 9";
  }
  auto snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kFaulted);
  ASSERT_TRUE(snapshot.diagnostic);
  EXPECT_EQ(snapshot.diagnostic->code, "controller_launch_exited");
  EXPECT_EQ(snapshot.diagnostic->message, *exit_reason);
  EXPECT_FALSE(snapshot.exact_controller_configuration_active);
  EXPECT_EQ(ArmWorld::Cause(snapshot), "fault");
  EXPECT_EQ(ArmWorld::Fact(snapshot, "external.fault_present"), std::optional(true));
  const auto primary_generation = snapshot.source_generation;
  const auto captured = TestLaunchDiagnosticSnapshot();
  EXPECT_FALSE(world.handle->HasControllerProcessIncidentDiagnostic());
  EXPECT_TRUE(world.handle->CacheControllerProcessIncidentDiagnostic(
      "incident.1", captured));
  EXPECT_TRUE(world.handle->HasControllerProcessIncidentDiagnostic());
  snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.diagnostic_snapshot.get(), captured.get());
  EXPECT_GT(snapshot.source_generation, primary_generation);
  const auto enriched_generation = snapshot.source_generation;
  EXPECT_FALSE(world.handle->CacheControllerProcessIncidentDiagnostic(
      "incident.1", TestLaunchDiagnosticSnapshot()));
  snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.diagnostic_snapshot.get(), captured.get());
  EXPECT_EQ(snapshot.source_generation, enriched_generation);
  EXPECT_TRUE(
      world.handle->HasControllerProcessIncidentDiagnostic("incident.1"));
  EXPECT_FALSE(
      world.handle->HasControllerProcessIncidentDiagnostic("incident.other"));
  // The controller manager may still answer (a partial launch failure) or
  // be gone; the process fault wins either way and stays until a new launch.
  world.manager.SetControllerActive(false);
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return !world.handle->arm_activity().controller_active; }));
  snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kFaulted);
  EXPECT_EQ(ArmWorld::Cause(snapshot), "fault");
  {
    std::lock_guard lock(mutex);
    exit_reason =
        std::string(core::kMaxLifecycleErrorMessageBytes - 1U, 'x') + "é";
  }
  snapshot = world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kFaulted);
  ASSERT_TRUE(snapshot.diagnostic);
  EXPECT_EQ(snapshot.diagnostic->message.size(),
            core::kMaxLifecycleErrorMessageBytes - 1U);
  EXPECT_TRUE(snapshot.locally_valid());
  EXPECT_THROW(world.handle->InstallControllerProcessFaultSource(
                   [] { return std::optional<std::string>{}; }),
               std::logic_error);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GuardFaultCanPrecedeSameCauseLaunchExitDiagnostic) {
  ArmWorld world;
  std::mutex mutex;
  std::optional<std::string> guard_fault;
  std::optional<std::string> process_fault;
  world.handle->InstallGuardHooks(
      {[] { return true; }, [] { return core::Result<void>::Ok(); }, [] {},
       [&] {
         std::lock_guard lock(mutex);
         return guard_fault;
       }});
  world.handle->InstallControllerProcessFaultSource([&] {
    std::lock_guard lock(mutex);
    return process_fault;
  });
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return world.handle->arm_activity().controller_loaded; }));
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  {
    std::lock_guard lock(mutex);
    guard_fault = "no fresh frame";
  }
  const auto first = world.Snapshot();
  EXPECT_EQ(ArmWorld::Cause(first), "fault");
  EXPECT_FALSE(first.diagnostic);
  {
    std::lock_guard lock(mutex);
    process_fault =
        "controller_launch_exited component=robot signal=11 tail=crash";
  }
  const auto later = world.Snapshot();
  EXPECT_EQ(ArmWorld::Cause(later), "fault");
  ASSERT_TRUE(later.diagnostic);
  EXPECT_EQ(later.diagnostic->message, *process_fault);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     HealthReadsDoNotAdvanceLifecycleBookkeeping) {
  ArmWorld world;
  std::optional<std::string> guard_fault;
  world.handle->InstallGuardHooks({[] { return true; },
                                   [] { return core::Result<void>::Ok(); },
                                   [] {}, [&] { return guard_fault; }});
  world.handle->StartBackend();
  ASSERT_TRUE(
      fake::WaitUntil([&] { return world.handle->arm_activity().observed; }));
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    return world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.arm"), world.clock.now()).ok();
  }));
  const auto before =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto domain = core::ControlDomainId("domain.arm");
  const auto read_settled_health = [&] {
    core::Result<core::LifecycleDomainHealthDTO> result;
    const bool settled = fake::WaitUntil([&] {
      RetainedRos2ControlRuntimeHandleTestAccess::RefreshEndpointFacts(*world.handle);
      const auto bookkeeping =
          RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
      result = world.handle->ReadLifecycleHealth(domain, world.clock.now());
      EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
                bookkeeping);
      return result.ok() || result.error().code != "component_instance_graph_unsettled";
    });
    if (!settled || !result.ok()) {
      std::ostringstream diagnostic;
      diagnostic << result.error().code << ": " << result.error().message;
      for (const auto &[key, value] : result.error().details)
        diagnostic << '\n' << key << '=' << value;
      ADD_FAILURE() << diagnostic.str();
    }
    return result;
  };
  for (int read = 0; read < 3; ++read) {
    const auto health = read_settled_health();
    ASSERT_TRUE(health.ok()) << health.error().message;
    EXPECT_EQ(health.value().observed_state, State::kDeclared);
    EXPECT_TRUE(health.value().locally_valid());
  }
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            before);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  const auto first = world.Snapshot();
  const auto same = read_settled_health();
  ASSERT_TRUE(same.ok());
  EXPECT_EQ(same.value().observed_state, first.component_state);
  EXPECT_EQ(same.value().facts, first.facts);
  const auto observed =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  guard_fault = "actual cached guard fault";
  const auto fault = read_settled_health();
  ASSERT_TRUE(fault.ok());
  EXPECT_EQ(fault.value().observed_state, State::kFaulted);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            observed);
  const auto committed_fault = world.Snapshot();
  EXPECT_EQ(committed_fault.component_state, fault.value().observed_state);
  EXPECT_EQ(committed_fault.facts, fault.value().facts);
  EXPECT_GT(committed_fault.source_generation, first.source_generation);
  guard_fault.reset();
  world.manager.StopAndDestroyEndpoints();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return !world.handle->arm_activity().controller_manager_present;
  })) << "remaining activity publishers="
      << world.handle->node()->count_publishers(
             world.prefix + "/controller_manager/activity");
  const auto retained =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto unavailable =
      world.handle->ReadLifecycleHealth(domain, world.clock.now());
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().code, "component_instance_graph_unsettled");
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            retained);
  const auto committed_unavailable = world.Snapshot();
  EXPECT_EQ(committed_unavailable.component_state, State::kDegraded);
  EXPECT_EQ(ArmWorld::Cause(committed_unavailable), "fault");
  const auto final =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  EXPECT_FALSE(world.handle
                   ->ReadLifecycleHealth(core::ControlDomainId("foreign"),
                                         world.clock.now())
                   .ok());
  EXPECT_FALSE(world.handle->ReadLifecycleHealth(domain, -1).ok());
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            final);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ConflictingActivityEndpointsRefuseActivationWithoutMutatingPureHealth) {
  ArmWorld world;
  world.handle->StartBackend();
  const auto domain = core::ControlDomainId("domain.arm");
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    return world.handle->arm_activity().observed &&
           world.handle->ReadLifecycleHealth(domain, world.clock.now()).ok();
  }));
  const auto settled_state = world.Snapshot().component_state;
  auto conflicting = world.manager.node()->create_publisher<
      controller_manager_msgs::msg::ControllerManagerActivity>(
      world.prefix + "/controller_manager/activity",
      rclcpp::QoS(1).reliable().transient_local());
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    const auto health = world.handle->ReadLifecycleHealth(domain, world.clock.now());
    return !health.ok() && health.error().code == "duplicate_component_instance";
  }));
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(domain, world.clock.now());
  ASSERT_FALSE(health.ok());
  EXPECT_EQ(health.error().details.at("topic"),
            world.prefix + "/controller_manager/activity");
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);
  for (const auto &result : {world.handle->PrepareArmController(),
                            world.handle->ActivateArmController({})}) {
    EXPECT_EQ(result.outcome, Outcome::kNotSent);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, "duplicate_component_instance");
  }
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  // Ordinary Observe remains available; Close can still read/disable the lane.
  EXPECT_EQ(world.Snapshot().component_state, settled_state);
  const std::weak_ptr<rclcpp::Publisher<
      controller_manager_msgs::msg::ControllerManagerActivity>> removed = conflicting;
  conflicting.reset();
  // Wake the same peer's existing waitset so SDK-held publisher entities can
  // retire even while this otherwise silent controller manager is idle.
  world.manager.node()->get_node_base_interface()
      ->get_default_callback_group()->trigger_notify_guard_condition();
  std::optional<core::ErrorDTO> last_health_error;
  auto last_state = settled_state;
  const bool recovered = fake::WaitUntil([&] {
    last_state = world.Snapshot().component_state;
    const auto current = world.handle->ReadLifecycleHealth(domain, world.clock.now());
    last_health_error = current.ok() ? std::nullopt
                                    : std::optional<core::ErrorDTO>(current.error());
    return current.ok();
  });
  // Failure-only NRT evidence: no additional graph read occurs during the
  // original recovery wait, and no cache/SDK state is changed by this report.
  if (!recovered) {
    std::ostringstream diagnostic;
    diagnostic << "removed_publisher_expired=" << removed.expired()
               << " remaining_owners=" << removed.use_count()
               << " settled_state=" << static_cast<unsigned>(settled_state)
               << " last_state=" << static_cast<unsigned>(last_state);
    if (last_health_error) {
      diagnostic << "\nlast_health_error=" << last_health_error->code
                 << " message=" << last_health_error->message;
      for (const auto &[key, value] : last_health_error->details)
        diagnostic << "\nhealth_detail[" << key << "]=" << value;
    }
    const auto activity = world.handle->arm_activity();
    diagnostic << "\nactivity present=" << activity.controller_manager_present
               << " observed=" << activity.observed
               << " loaded=" << activity.controller_loaded
               << " active=" << activity.controller_active
               << " hardware_active=" << activity.hardware_active;
    for (const auto &topic : {
             world.prefix + "/controller_manager/activity",
             world.prefix + "/arm_controller/controller_state"}) {
      try {
        const auto endpoints = world.handle->node()->get_publishers_info_by_topic(topic);
        diagnostic << "\ngraph topic=" << topic << " endpoint_count=" << endpoints.size();
        for (const auto &endpoint : endpoints) {
          diagnostic << "\nendpoint=" << endpoint.node_namespace() << '/'
                     << endpoint.node_name() << " type=" << endpoint.topic_type() << " gid=";
          for (const auto byte : endpoint.endpoint_gid())
            diagnostic << static_cast<unsigned>(byte) << ',';
        }
      } catch (const std::exception &error) {
        diagnostic << "\ngraph topic=" << topic << " read_error=" << error.what();
      }
    }
    RecordProperty("endpoint_removal_diagnostic", diagnostic.str());
    ADD_FAILURE() << diagnostic.str();
  }
  ASSERT_TRUE(recovered);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ConflictingJtcStateDoesNotBlockDisableOrBecomeProcessIdentity) {
  ArmWorld world;
  world.handle->StartBackend();
  const auto domain = core::ControlDomainId("domain.arm");
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    const auto activity = world.handle->arm_activity();
    return activity.controller_manager_present && activity.observed &&
           activity.controller_loaded && activity.hardware_active &&
           !activity.controller_active &&
           world.handle->ReadLifecycleHealth(domain, world.clock.now()).ok();
  }));
  const auto prepared = world.handle->PrepareArmController();
  ASSERT_EQ(prepared.outcome, Outcome::kSucceeded) << SwitchFailureText(prepared);
  const auto activated = world.handle->ActivateArmController({});
  ASSERT_EQ(activated.outcome, Outcome::kSucceeded) << SwitchFailureText(activated);
  auto second = world.jtc.node()->create_publisher<
      control_msgs::msg::JointTrajectoryControllerState>(
      world.prefix + "/arm_controller/controller_state", rclcpp::QoS(10));
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    const auto health = world.handle->ReadLifecycleHealth(domain, world.clock.now());
    return !health.ok() && health.error().code == "duplicate_component_instance";
  }));
  const auto health = world.handle->ReadLifecycleHealth(domain, world.clock.now());
  ASSERT_FALSE(health.ok());
  EXPECT_EQ(health.error().details.at("topic"),
            world.prefix + "/arm_controller/controller_state");
  const auto switches = world.manager.switch_requests();
  const auto disabled = world.handle->DeactivateArmController({});
  EXPECT_EQ(disabled.outcome, Outcome::kSucceeded) << SwitchFailureText(disabled);
  EXPECT_EQ(world.manager.switch_requests(), switches + 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     HealthUsesPureProcessCacheAndObserveKeepsAdvancement) {
  ArmWorld world;
  int observation_calls = 0;
  int health_calls = 0;
  std::optional<std::string> fault =
      "controller_launch_exited: exact owner process exited";
  world.handle->InstallControllerProcessFaultSource(
      [&] {
        ++observation_calls;
        return fault;
      },
      [&] {
        ++health_calls;
        return core::Result<std::optional<std::string>>::Ok(fault);
      });
  const auto before =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto domain = core::ControlDomainId("domain.arm");
  const auto health =
      world.handle->ReadLifecycleHealth(domain, world.clock.now());
  ASSERT_TRUE(health.ok());
  EXPECT_EQ(health.value().observed_state, State::kFaulted);
  ASSERT_TRUE(health.value().diagnostic);
  EXPECT_EQ(health.value().diagnostic->message, *fault);
  EXPECT_EQ(observation_calls, 0);
  EXPECT_EQ(health_calls, 1);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            before);
  const auto observed = world.Snapshot();
  EXPECT_EQ(observation_calls, 1);
  EXPECT_EQ(observed.component_state, health.value().observed_state);
  EXPECT_EQ(observed.facts, health.value().facts);
  EXPECT_EQ(observed.diagnostic, health.value().diagnostic);
  // The original process callback can also run for an unknown Observe domain;
  // a process fault must not make that domain appear bound.
  EXPECT_FALSE(world.handle
                   ->ReadLifecycleSnapshot(core::ControlDomainId("foreign"),
                                           world.clock.now())
                   .ok());
  EXPECT_EQ(observation_calls, 2);
  EXPECT_EQ(health_calls, 1);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     HealthRequiresExactPureProcessProjection) {
  ArmWorld world;
  int observation_calls = 0;
  world.handle->InstallControllerProcessFaultSource([&] {
    ++observation_calls;
    return std::optional<std::string>{};
  });
  const auto before =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(
      core::ControlDomainId("domain.arm"), world.clock.now());
  ASSERT_FALSE(health.ok());
  EXPECT_EQ(health.error().code,
            core::to_string(core::ErrorCode::kInvariantViolation));
  ASSERT_TRUE(health.error().details.contains("reason"));
  EXPECT_EQ(health.error().details.at("reason"),
            "ros2_control_process_health_unavailable");
  EXPECT_EQ(observation_calls, 0);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            before);
  EXPECT_TRUE(world.handle
                  ->ReadLifecycleSnapshot(core::ControlDomainId("domain.arm"),
                                          world.clock.now())
                  .ok());
  EXPECT_EQ(observation_calls, 1);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     PureProcessCacheFailureIsNotObserveFallback) {
  ArmWorld world;
  int observation_calls = 0;
  const core::ErrorDTO unavailable{"exact_process_cache_unavailable",
                                   "Exact process record is unavailable",
                                   {}};
  world.handle->InstallControllerProcessFaultSource(
      [&] {
        ++observation_calls;
        return std::optional<std::string>{};
      },
      [&] {
        return core::Result<std::optional<std::string>>::Err(unavailable);
      });
  const auto before =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(
      core::ControlDomainId("domain.arm"), world.clock.now());
  ASSERT_FALSE(health.ok());
  EXPECT_EQ(health.error(), unavailable);
  EXPECT_EQ(observation_calls, 0);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            before);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperHealthReadsTheSameFactsWithoutLifecycleCommit) {
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(
      fake::WaitUntil([&] { return world.handle->arm_activity().observed; }));
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    return world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.gripper"), world.clock.now()).ok();
  }));
  const auto before =
      RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health = world.handle->ReadLifecycleHealth(
      core::ControlDomainId("domain.gripper"), world.clock.now());
  ASSERT_TRUE(health.ok()) << health.error().message;
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle),
            before);
  const auto observed = world.Snapshot();
  EXPECT_EQ(observed.component_state, health.value().observed_state);
  EXPECT_EQ(observed.facts, health.value().facts);
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest, GripperActivationStepUsesExactSelectedTransport) {
  for (const bool controller_manager_gripper : {false, true}) {
    GripperWorld world(true, {}, controller_manager_gripper);
    const auto before =
        RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
    const auto health = world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.gripper"), world.clock.now());
    ASSERT_TRUE(health.ok()) << health.error().message;
    EXPECT_TRUE(health.value().has_activation_step);
    EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);
    const auto observed = world.Snapshot();
    EXPECT_EQ(observed.component_state, health.value().observed_state);
    EXPECT_EQ(observed.facts, health.value().facts);
    const auto arm = world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.arm"), world.clock.now());
    ASSERT_TRUE(arm.ok()) << arm.error().message;
    EXPECT_FALSE(arm.value().has_activation_step.has_value());
    EXPECT_TRUE(world.manager.Requests().empty());
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     CommandBridgeLifecycleClosesAdmissionAndReactivatesWithoutControllerSwitch) {
  GripperWorld world(true, {}, false);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->gripper_transport_readiness_snapshot()->ready;
  }));
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  ASSERT_EQ(world.Dispatch("prepare").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  core::GripperCommandRequestDTO command;
  command.gripper_id = "gripper.target";
  command.group_id = "gripper";
  command.joint_name = "gripper_joint";
  command.target_position = 0.02;
  command.duration_s = 1.0;
  EXPECT_FALSE(world.gripper_backend->command_gripper(command).accepted);
  ASSERT_EQ(world.Dispatch("activate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_EQ(world.Dispatch("deactivate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  EXPECT_FALSE(world.gripper_backend->command_gripper(command).accepted);
  ASSERT_EQ(world.Dispatch("activate").known,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest, LaneGuardHooksGateActivationMotionAndRecover) {
  ArmWorld world;
  std::mutex mutex;
  bool allow_activation = false;
  bool admitted = false;
  std::optional<std::string> fault;
  int recovers = 0;
  world.handle->InstallGuardHooks(RetainedRos2ControlArmGuardHooks{
      [&] {
        std::lock_guard lock(mutex);
        return admitted;
      },
      [&]() -> core::Result<void> {
        std::lock_guard lock(mutex);
        if (allow_activation) return core::Result<void>::Ok();
        return core::Result<void>::Err(
            {"execution_rejected", "external mechanism guard is not ready",
             {{"reason", "external_mechanism_guard_not_ready"}}});
      },
      [&] {
        std::lock_guard lock(mutex);
        ++recovers;
        fault.reset();
      },
      [&] {
        std::lock_guard lock(mutex);
        return fault;
      }});
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    (void)world.Snapshot();
    const auto activity = world.handle->arm_activity();
    return activity.controller_manager_present && activity.observed &&
           activity.controller_loaded && activity.hardware_active &&
           !activity.controller_active &&
           world.handle->ReadLifecycleHealth(
               core::ControlDomainId("domain.arm"), world.clock.now()).ok();
  }));
  const auto prepared = world.handle->PrepareArmController();
  ASSERT_EQ(prepared.outcome, Outcome::kSucceeded) << SwitchFailureText(prepared);
  // Refused before effect: no switch request reaches the controller manager.
  const auto refused = world.handle->ActivateArmController({});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"),
            "external_mechanism_guard_not_ready");
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  {
    std::lock_guard lock(mutex);
    allow_activation = true;
  }
  const auto activated = world.handle->ActivateArmController({});
  ASSERT_EQ(activated.outcome, Outcome::kSucceeded) << SwitchFailureText(activated);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto port = world.handle->current_port();
  ASSERT_TRUE(fake::WaitUntil([&] { return port->ReadReadiness().controller_bound; }));

  // An installed external mechanism guard gates every motion path.
  core::TrajectoryDTO hold;
  hold.joint_names = {"joint_1"};
  hold.waypoints = {{0.0, {0.0}, {0.0}, {0.0}}, {1.0, {0.0}, {0.0}, {0.0}}};
  const core::ExecutionGoalKey key{core::ControllerGoalId("goal.gated"),
                                   core::ControllerGoalGeneration(1U)};
  const auto gated = port->Submit(key, hold);
  EXPECT_EQ(gated.dispatch, core::ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(gated.refusal, core::ExecutionPortRefusal::kNotReady);
  EXPECT_EQ(port->Replace(key, {core::ControllerGoalId("goal.gated"),
                                core::ControllerGoalGeneration(2U)},
                          hold)
                .refusal,
            core::ExecutionPortRefusal::kNotReady);
  EXPECT_EQ(world.jtc.goals_received(), 0U);
  {
    std::lock_guard lock(mutex);
    admitted = true;
  }
  const auto readiness = port->ReadReadiness();
  const auto submitted = port->Submit(key, hold);
  testing::Message diagnostic;
  diagnostic << "refusal="
             << (submitted.refusal ? static_cast<int>(*submitted.refusal) : -1)
             << " controller_active=" << readiness.controller_active
             << " controller_bound=" << readiness.controller_bound
             << " hardware_active=" << readiness.hardware_active;
  if (submitted.error) {
    diagnostic << " error.code=" << submitted.error->code
               << " error.message=" << submitted.error->message;
    for (const auto &[name, value] : submitted.error->details) {
      diagnostic << " error.detail[" << name << "]=" << value;
    }
  }
  EXPECT_NE(submitted.dispatch, core::ExecutionPortDispatch::kNotDispatched)
      << diagnostic;
  ASSERT_TRUE(fake::WaitUntil([&] { return world.jtc.goals_received() == 1U; }));
  // The controller receives its own joint names.
  EXPECT_EQ(world.jtc.trajectories().front().joint_names,
            (std::vector<std::string>{"source_joint_1"}));

  // A guard fault is a `fault` cause until the operator's Recover.
  {
    std::lock_guard lock(mutex);
    fault = "no fresh frame";
  }
  ASSERT_TRUE(world.WaitForState(State::kFaulted));
  EXPECT_EQ(ArmWorld::Cause(world.Snapshot()), "fault");
  EXPECT_EQ(world.handle->DeactivateArmControllerForFault({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(fake::WaitUntil([&] { return !world.handle->arm_activity().controller_active; }));
  EXPECT_EQ(world.Snapshot().component_state, State::kFaulted);
  ASSERT_EQ(world.handle->RecoverArmController({}).outcome, Outcome::kSucceeded);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(recovers, 1);
  }
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
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
                         const std::string &prefix) {
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
          prefix + "/joint_states", 100,
          core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
          std::string(kMeasuredJointStateProfileId)});
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

TEST(RetainedRos2ControlRuntimeHandle, InactiveJointStateSamplesAreMeasuredAndReadsNeverRefresh) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
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
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
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
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
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
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
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
    ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(),
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
    ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(),
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
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
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

TEST(RetainedRos2ControlRuntimeHandle,
     MeasuredSubscriptionFailureIsUnavailableWithoutChangingPhysicalOwner) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto node = world.handle->node();
  ASSERT_NE(node, nullptr);
  // Transport resources own a private context. Closing only that context
  // makes real subscription creation fail without mutating global ROS state.
  const auto context = node->get_node_base_interface()->get_context();
  ASSERT_TRUE(context->shutdown("isolated measured subscription failure"));
  const auto staged = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  EXPECT_EQ(staged.value(), nullptr);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     ScalarMeasuredObservationRefreshesLateActivityGraphAndRetainsDuplicateRefusal) {
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  core::ManualScheduler scheduler;
  const auto tool = GripperBinding();
  const auto domain = tool.value().control_path->control_domain_id;
  const auto prefix = "/manyforge_scalar_observer_" + fake::unique_suffix();
  fake::FakeControllerManager manager(prefix + "/controller_manager",
                                       "gripper_controller", "TestSystem", nullptr);
  auto sink = std::make_shared<NullSink>();
  auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
      "manyforge_scalar_observer_" + fake::unique_suffix(), "/", 3U,
      sink, "gripper", *clock);
  auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
      *resources, sink, *clock, scheduler, true);
  ros2_control_execution::Ros2GripperActionConfig config;
  config.action_name = prefix + "/gripper_controller/gripper_cmd";
  config.group_id = "gripper";
  config.joint_name = "gripper_joint";
  backend->configure_gripper_action(tool.value().command_target_id.value(), config);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->BindGripperOnly(std::move(resources), *clock,
      std::move(backend), tool,
      {prefix + "/controller_manager", "gripper_controller", "TestSystem"},
      {}, clock, RetainedMeasuredJointStateLane{
          prefix + "/joint_states", 100,
          core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
          std::string(kMeasuredJointStateProfileId)}).ok());
  // Early facts ownership must not change the unstarted health classification.
  ASSERT_TRUE(handle->ReadLifecycleHealth(domain, clock->now()).ok());
  const auto descriptor = MeasuredSourceDescriptor(tool, prefix);
  const auto staged = handle->StageMeasuredStateSources(
      {descriptor}, clock, std::nullopt, tool);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  ASSERT_NE(staged.value(), nullptr);
  staged.value()->EnablePublication();
  const auto reader = staged.value()->readers().front();
  const auto query = descriptor.publication.query_context;
  auto samples = manager.node()->create_publisher<sensor_msgs::msg::JointState>(
      prefix + "/joint_states", rclcpp::QoS(1));
  handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return samples->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 2;
  message.name = {"source_gripper_joint"};
  message.position = {0.02};
  // Settle initial discovery through existing lifecycle Observe, before any
  // measured sample is sent. No queued measured callback can consume the
  // deliberately later graph edge tested below.
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto observed = handle->ReadLifecycleSnapshot(domain, clock->now());
    const auto health = handle->ReadLifecycleHealth(domain, clock->now());
    return observed.ok() && health.ok() &&
           health.value().observed_state == State::kDeclared;
  }));
  EXPECT_FALSE(reader->current_state(query, clock->now()).ok());
  EXPECT_FALSE(handle->participant());
  EXPECT_FALSE(handle->current_port());
  EXPECT_FALSE(handle->ReadOwnedControllerOccurrence().arm_binding_epoch);
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle);

  // A real unrelated endpoint appears AFTER the activity matched callback.
  // The existing CM activity is latched and is not republished for this edge.
  auto unrelated = manager.node()->create_publisher<sensor_msgs::msg::JointState>(
      prefix + "/late_unrelated", rclcpp::QoS(1));
  ASSERT_TRUE(fake::WaitUntil([&] {
    if (handle->node()->count_publishers(prefix + "/late_unrelated") != 1U)
      return false;
    // Discovery and this subscription's asynchronous graph-event delivery
    // are distinct; wait for the actual cached negative fact before testing
    // that pure reads preserve it. No measured sample has been sent yet.
    const auto health = handle->ReadLifecycleHealth(domain, clock->now());
    return !health.ok() && health.error().code == "component_instance_graph_unsettled" &&
           health.error().details.at("graph_reason") == "selected_endpoint_graph_changed";
  }));
  for (int read = 0; read < 3; ++read) {
    const auto health = handle->ReadLifecycleHealth(domain, clock->now());
    ASSERT_FALSE(health.ok());
    EXPECT_EQ(health.error().code, "component_instance_graph_unsettled");
    EXPECT_EQ(health.error().details.at("graph_reason"), "selected_endpoint_graph_changed");
  }
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle), before);
  EXPECT_FALSE(reader->current_state(query, clock->now()).ok());
  // The lifecycle Observe checkpoint performs the required following stable
  // endpoint capture. It settles unrelated graph churn without waiting for a
  // measured sample and without issuing a controller effect.
  const auto settled = handle->ReadLifecycleSnapshot(domain, clock->now());
  ASSERT_TRUE(settled.ok()) << settled.error().message;
  ASSERT_TRUE(handle->ReadLifecycleHealth(domain, clock->now()).ok());
  EXPECT_EQ(manager.switch_requests(), 0U);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle), before);
  ASSERT_TRUE(fake::WaitUntil([&] {
    samples->publish(message);
    const auto sample = reader->current_state(query, clock->now());
    const auto health = handle->ReadLifecycleHealth(domain, clock->now());
    return sample.ok() && sample.value().sequence > 0U && health.ok() &&
           health.value().observed_state == State::kDeclared;
  }));
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle), before);
  EXPECT_EQ(manager.switch_requests(), 0U);

  auto conflicting = manager.node()->create_publisher<
      controller_manager_msgs::msg::ControllerManagerActivity>(
      prefix + "/controller_manager/activity",
      rclcpp::QoS(1).reliable().transient_local());
  ASSERT_TRUE(fake::WaitUntil([&] {
    samples->publish(message);
    const auto health = handle->ReadLifecycleHealth(domain, clock->now());
    return !health.ok() && health.error().code == "duplicate_component_instance";
  }));
  const auto refused = handle->lifecycle_primitive_dispatcher()(
      domain, core::LifecyclePrimitiveId("ros2_control.controller.prepare"), {});
  EXPECT_EQ(refused.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(refused.effect, core::LifecyclePrimitiveEffectDTO::kNone);
  ASSERT_TRUE(refused.error);
  EXPECT_EQ(refused.error->code, "duplicate_component_instance");
  EXPECT_EQ(manager.switch_requests(), 0U);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle), before);
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> released = handle;
  handle.reset();
  EXPECT_TRUE(released.expired());
  EXPECT_TRUE(reader->current_state(query, clock->now()).ok());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     PneumaticNoneLocalLifecycleUsesExactDomainWithoutPhysicalController) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto tool = GripperBinding();
  auto sink = std::make_shared<NullSink>();
  auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
      "manyforge_none_lifecycle_" + fake::unique_suffix(), "/", 1U,
      sink, "gripper", clock);
  auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
      *resources, sink, clock, scheduler, true);
  ros2_control_execution::Ros2GripperActionConfig config;
  config.actuation_kind = core::GripperActuationKind::kPneumatic;
  config.output_mode = core::GripperOutputMode::kNone;
  config.action_name.clear();
  config.transport_kind.clear();
  config.completion_policy = "no_output";
  config.group_id = "gripper";
  config.joint_name = "gripper_joint";
  backend->configure_gripper_action(tool.value().command_target_id.value(), config);
  auto *const gripper = backend.get();
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->BindGripperOnly(std::move(resources), clock,
                                    std::move(backend), tool, {}, {}, {},
                                    std::nullopt).ok());
  handle->StartBackend();
  EXPECT_TRUE(handle->OwnsGripper(gripper, tool));
  EXPECT_EQ(gripper->gripper_actuation_kind(tool.value().command_target_id.value()),
            core::GripperActuationKind::kPneumatic);
  EXPECT_EQ(gripper->gripper_output_mode(tool.value().command_target_id.value()),
            core::GripperOutputMode::kNone);
  EXPECT_FALSE(handle->participant());
  EXPECT_FALSE(handle->current_port());
  EXPECT_TRUE(handle->controller_joint_names().empty());
  EXPECT_FALSE(handle->ReadOwnedControllerOccurrence().arm_binding_epoch);
  EXPECT_FALSE(handle->arm_activity().controller_manager_present);
  const auto domain = tool.value().control_path->control_domain_id;
  const auto initial = handle->ReadLifecycleSnapshot(domain, clock.now());
  ASSERT_TRUE(initial.ok());
  EXPECT_EQ(initial.value().component_state, State::kActive);
  const auto command_before = gripper->get_gripper_status(tool.value().command_target_id.value());
  core::GripperCommandRequestDTO candidate;
  candidate.gripper_id = tool.value().command_target_id.value();
  candidate.group_id = config.group_id;
  candidate.joint_name = config.joint_name;
  candidate.target_position = config.open_position;
  candidate.completion_policy = "no_output";
  candidate.grasp_confirmation_policy = "not_required";
  EXPECT_TRUE(gripper->validate_gripper_command(candidate).ok());
  for (const auto *stale : {"dwell_elapsed", "output_delivery"}) {
    candidate.completion_policy = stale;
    EXPECT_FALSE(gripper->validate_gripper_command(candidate).ok());
  }
  const auto dispatcher = handle->lifecycle_primitive_dispatcher();
  for (const auto &[primitive, state] :
       std::vector<std::pair<std::string, State>>{{"deactivate", State::kDisabled},
                                                {"prepare", State::kPrepared},
                                                {"activate", State::kActive}}) {
    const auto outcome = dispatcher(domain,
        core::LifecyclePrimitiveId("ros2_control.controller." + primitive), {});
    ASSERT_EQ(outcome.known, core::LifecyclePrimitiveKnownResultDTO::kSucceeded);
    const auto snapshot = handle->ReadLifecycleSnapshot(domain, clock.now());
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().component_state, state);
    EXPECT_EQ(ArmWorld::Fact(snapshot.value(), "external.controller_configuration_ready"),
              state == State::kActive);
    EXPECT_GT(snapshot.value().source_generation, initial.value().source_generation);
    const auto bookkeeping = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle);
    const auto health = handle->ReadLifecycleHealth(domain, clock.now());
    ASSERT_TRUE(health.ok());
    EXPECT_EQ(health.value().observed_state, state);
    EXPECT_EQ(health.value().facts, snapshot.value().facts);
    EXPECT_EQ(health.value().has_activation_step, false);
    EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*handle), bookkeeping);
    const auto local = handle->gripper_transport_readiness_snapshot();
    ASSERT_TRUE(local.has_value());
    EXPECT_TRUE(local->configured);
    EXPECT_TRUE(local->logical_owner_ready);
    EXPECT_EQ(local->output_mode, core::GripperOutputMode::kNone);
    EXPECT_FALSE(local->command_path_ready);
    EXPECT_FALSE(local->feedback_path_ready);
    EXPECT_TRUE(local->transport_kind.empty());
    EXPECT_TRUE(local->state_topic.empty());
    EXPECT_EQ(local->feedback_messages_received, 0U);
    const auto foreign = dispatcher(core::ControlDomainId("domain.foreign"),
        core::LifecyclePrimitiveId("ros2_control.controller.activate"), {});
    EXPECT_EQ(foreign.dispatch, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
    EXPECT_EQ(foreign.effect, core::LifecyclePrimitiveEffectDTO::kNone);
    EXPECT_TRUE(foreign.error.has_value());
    EXPECT_FALSE(handle->ReadLifecycleSnapshot(core::ControlDomainId("domain.foreign"),
                                              clock.now()).ok());
    const auto unchanged = handle->ReadLifecycleSnapshot(domain, clock.now());
    ASSERT_TRUE(unchanged.ok());
    EXPECT_EQ(unchanged.value(), snapshot.value());
    const auto command_after = gripper->get_gripper_status(tool.value().command_target_id.value());
    EXPECT_EQ(command_after.state, core::GripperCommandState::kIdle);
    EXPECT_EQ(command_after.command_id, command_before.command_id);
    EXPECT_EQ(command_after.generation, command_before.generation);
    EXPECT_EQ(command_after.revision, command_before.revision);
    EXPECT_EQ(command_after.admission_generation, command_before.admission_generation);
    EXPECT_EQ(command_after.lifecycle_generation, command_before.lifecycle_generation);
    EXPECT_EQ(command_after.admission_closed, command_before.admission_closed);
  }
  EXPECT_EQ(clock.now(), 0);
  EXPECT_FALSE(handle->arm_activity().controller_manager_present);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     GripperOnlyBindingRequiresTypedNoneForEmptyControllerFacts) {
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto tool = GripperBinding();
  auto sink = std::make_shared<NullSink>();
  for (const bool no_output : {true, false}) {
    auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
        "manyforge_none_binding_" + fake::unique_suffix(), "/", 1U,
        sink, "gripper", clock);
    auto backend = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
        *resources, sink, clock, scheduler, true);
    ros2_control_execution::Ros2GripperActionConfig config;
    config.actuation_kind = core::GripperActuationKind::kPneumatic;
    config.completion_policy = "output_delivery";
    if (no_output) {
      config.output_mode = core::GripperOutputMode::kNone;
      config.action_name.clear();
      config.transport_kind.clear();
      config.completion_policy = "no_output";
    }
    backend->configure_gripper_action(tool.value().command_target_id.value(), config);
    core::GripperCommandRequestDTO candidate;
    candidate.gripper_id = tool.value().command_target_id.value();
    candidate.target_position = config.open_position;
    candidate.completion_policy = no_output ? "no_output" : "output_delivery";
    candidate.grasp_confirmation_policy = "not_required";
    EXPECT_TRUE(backend->validate_gripper_command(candidate).ok());
    candidate.completion_policy = no_output ? "dwell_elapsed" : "position_reached";
    EXPECT_FALSE(backend->validate_gripper_command(candidate).ok());
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    const RetainedRos2ControlArmController controller = no_output
        ? RetainedRos2ControlArmController{"/foreign/controller_manager", "gripper", "hardware"}
        : RetainedRos2ControlArmController{};
    const auto bound = handle->BindGripperOnly(std::move(resources), clock,
        std::move(backend), tool, controller, {}, {}, std::nullopt);
    EXPECT_FALSE(bound.ok());
    EXPECT_FALSE(handle->gripper_transport_readiness_snapshot().has_value());
    EXPECT_TRUE(handle->ReadOwnedControllerOccurrence().domains.empty());
    EXPECT_FALSE(handle->participant());
    EXPECT_FALSE(handle->current_port());
  }
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetEndpointChangesWaitForCurrentSuccessorWithinExistingDeadline) {
  for (const bool restore_endpoint : {true, false}) {
    SCOPED_TRACE(restore_endpoint);
    ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
    // Production installs this observer before start; its existing sample
    // timer refreshes endpoints and wakes Reset on new controller samples.
    world.handle->InstallSampleObserver(
        [](const core::ExecutionControllerSampleDTO &) {});
    world.handle->StartBackend();
    ASSERT_TRUE(fake::WaitUntil([&] {
      static_cast<void>(world.Snapshot());
      const auto health = world.handle->ReadLifecycleHealth(
          core::ControlDomainId("domain.arm"), world.clock.now());
      return health.ok() && health.value().observed_state == State::kDeclared;
    }));
    ASSERT_TRUE(fake::WaitUntil(
        [&] { return world.handle->mock_reset_writer_inactive(); }));
    const auto activated = world.handle->ActivateArmController({});
    ASSERT_EQ(activated.outcome, Outcome::kSucceeded)
        << (activated.error ? activated.error->message : std::string{"no error detail"});
    ASSERT_TRUE(world.WaitForState(State::kActive));
    rclcpp::Publisher<control_msgs::msg::JointTrajectoryControllerState>::SharedPtr conflict;
    bool checked_availability = false;
    bool checked_successor = false;
    const auto reset = world.handle->ResetMockPosition(
        {}, [&](const auto &transition, const auto &deadline) -> core::Result<void> {
      if (transition.phase == MockResetPositionPhase::kSwitchingToWriter &&
          transition.switch_acknowledged) {
        EXPECT_TRUE(fake::WaitUntil(
            [&] { return world.handle->mock_reset_writer_active(); }));
      }
      if (transition.phase == MockResetPositionPhase::kSettingState) {
        if (!transition.target_published) {
          conflict = world.jtc.node()->create_publisher<
              control_msgs::msg::JointTrajectoryControllerState>(
              world.prefix + "/arm_controller/controller_state", rclcpp::QoS(10));
          EXPECT_TRUE(fake::WaitUntil([&] {
            (void)world.Snapshot();
            const auto health = world.handle->ReadLifecycleHealth(
                core::ControlDomainId("domain.arm"), world.clock.now());
            return !health.ok() &&
                   health.error().code == "duplicate_component_instance";
          }));
          // Reset owns this availability interval; SDK graph observation is
          // not synthesized into a second lifecycle transport-loss condition.
          const auto snapshot = world.Snapshot();
          EXPECT_EQ(ArmWorld::Cause(snapshot), "availability");
          EXPECT_EQ(ArmWorld::Fact(snapshot, "external.transport_ready"), true);
          EXPECT_EQ(ArmWorld::Fact(snapshot, "external.fault_present"), false);
          checked_availability = true;
        } else {
          EXPECT_TRUE(fake::WaitUntil(
              [&] { return world.manager.reset_commands().size() == 1U; }));
        }
      }
      if (transition.phase == MockResetPositionPhase::kSwitchingToJtc &&
          transition.switch_acknowledged) {
        checked_successor = true;
        EXPECT_FALSE(world.handle->mock_reset_successor_ready());
        const std::weak_ptr<rclcpp::Publisher<
            control_msgs::msg::JointTrajectoryControllerState>> removed = conflict;
        if (restore_endpoint) {
          conflict.reset();
          // As in the existing endpoint-removal fixture, release the peer
          // executor's selected SDK owners before claiming endpoint removal.
          world.jtc.node()->get_node_base_interface()
              ->get_default_callback_group()->trigger_notify_guard_condition();
        }
        const auto wake = world.handle->mock_reset_observation_wake();
        if (!wake)
          return core::Result<void>::Err(
              {"fixture_observation_missing", "Reset observation wake is absent", {}});
        // Use the actual SDK deadline, exactly as the host does. A positive
        // CM switch ACK cannot make a conflicting successor ready.
        const auto observed = host_detail::WaitForMockResetObservation(
            *wake, {}, deadline,
            [&] { return core::Result<bool>::Ok(
                world.handle->mock_reset_successor_ready()); },
            [] { return core::Result<void>::Ok(); });
        if (restore_endpoint) {
          EXPECT_TRUE(removed.expired());
        }
        return observed;
      }
      return core::Result<void>::Ok();
    });
    EXPECT_TRUE(checked_availability);
    EXPECT_TRUE(checked_successor);
    EXPECT_EQ(reset.outcome, restore_endpoint ? Outcome::kSucceeded : Outcome::kFailed)
        << SwitchFailureText(reset);
    if (!restore_endpoint) {
      ASSERT_TRUE(reset.error);
      EXPECT_EQ(reset.error->details.at("reason"),
                "reset_position_acknowledgment_timeout");
      EXPECT_FALSE(world.handle->current_port()->ReadReadiness().controller_bound);
    }
    EXPECT_EQ(world.manager.reset_commands().size(), 1U);
  }
}

// stock-jtc-01 crash: a runtime weak callback can be in flight when the last
// external owner releases. Its locked lease must never become the last owner:
// the destructor would then run teardown on its own executor (spin thread:
// join throws std::system_error inside noexcept quiesce, abort; a worker
// thread was observed to complete, still under live callbacks). The releaser
// must cancel, drain and join, then release every child before reset() returns.
// One executor thread runs the callback on the spin thread; three threads
// also put it on the executor's worker threads.
TEST(RetainedRos2ControlRuntimeHandleTest,
     LastExternalReleaseDuringOwnCallbackJoinsOnTheReleasingThread) {
  for (const std::size_t executor_threads : {std::size_t{1}, std::size_t{3}}) {
    for (int round = 0; round < (executor_threads == 1U ? 1 : 6); ++round) {
      SCOPED_TRACE(testing::Message() << "threads=" << executor_threads
                                      << " round=" << round);
      // Declared before the world: the observer may run until teardown.
      std::atomic<bool> armed{false};
      std::promise<void> entered;
      auto entered_future = entered.get_future();
      std::weak_ptr<RetainedRos2ControlRuntimeHandle> external;
      std::atomic<bool> lease_outlived_external{false};
      ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), {},
                     false, {}, {}, executor_threads);
      external = world.handle;
      world.handle->InstallSampleObserver(
          [&](const core::ExecutionControllerSampleDTO &) {
            if (!armed.exchange(false))
              return;
            entered.set_value();
            // The sample timer holds its locked lease across this call. Keep
            // it until the external owner is gone; bounded, so the old
            // ownership (this lease is then the last owner) still unwinds.
            lease_outlived_external.store(
                !fake::WaitUntil([&] { return external.expired(); }, 1s));
          });
      world.handle->StartBackend();
      ASSERT_TRUE(fake::WaitUntil([&] {
        const auto activity = world.handle->arm_activity();
        return activity.observed && activity.controller_loaded &&
               activity.hardware_active;
      }));
      // An observed setup refusal (kNotSent) sends nothing; retry only that.
      ASSERT_TRUE(fake::WaitUntil([&] {
        return world.handle->PrepareArmController().outcome ==
               Outcome::kSucceeded;
      }));
      std::optional<RetainedRos2ControlSwitchResult> activated;
      ASSERT_TRUE(fake::WaitUntil([&] {
        activated = world.handle->ActivateArmController({});
        return activated->outcome != Outcome::kNotSent;
      }));
      ASSERT_EQ(activated->outcome, Outcome::kSucceeded)
          << SwitchFailureText(*activated);
      ASSERT_TRUE(world.WaitForState(State::kActive));
      const std::weak_ptr<RetainedRos2ControlRuntimeHandle> internal =
          world.handle->weak_from_this();
      const std::weak_ptr<core::ExecutionPortParticipant> participant =
          world.handle->participant();
      const std::weak_ptr<rclcpp::Node> node = world.handle->node();
      armed.store(true);
      ASSERT_EQ(entered_future.wait_for(10s), std::future_status::ready);
      world.handle.reset(); // the last external owner, callback in flight
      EXPECT_FALSE(lease_outlived_external.load());
      EXPECT_TRUE(internal.expired());
      EXPECT_TRUE(participant.expired());
      EXPECT_TRUE(node.expired());
      // Never unwind the fixture's borrowed lifetimes under a live handle.
      ASSERT_TRUE(fake::WaitUntil([&] { return internal.expired(); }));
    }
  }
}


} // namespace
} // namespace manyforge::roboplan_backend
