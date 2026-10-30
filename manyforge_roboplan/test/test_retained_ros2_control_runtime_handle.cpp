// ROS runtime handle and Isaac guard binding against in-process stand-ins of
// the controller manager, the trajectory controller and the Isaac-side guard
// (design R6/§7.5 CCLC activation, §3.1 binding epochs, §4.3/Q41/Q43/Q46).
#include "ros2_control_fake_controller.hpp"
#include "committed_tool_payload_follower.hpp"
#include "../src/program_reset_position_observation.hpp"

#include <gtest/gtest.h>

#include <pal_statistics_msgs/msg/statistics.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>
#include <manyforge_roboplan/retained_vendor_device_status.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>
#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>
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
  static bool TryLockSwitch(RetainedRos2ControlRuntimeHandle &handle) {
    if (!handle.switch_mutex_.try_lock()) return false;
    handle.switch_mutex_.unlock();
    return true;
  }
  static void RefreshEndpointFacts(RetainedRos2ControlRuntimeHandle &handle) {
    handle.RefreshEndpointFacts();
  }
  /// The selected ROS endpoints (CM activity, controller state) are
  /// established in the handle's graph view. Until then every lifecycle step
  /// is refused before effect (`component_instance_graph_unsettled`), so a
  /// test's first step waits for this, not only for a CM observation.
  [[nodiscard]] static bool EndpointsSettled(RetainedRos2ControlRuntimeHandle &handle) {
    handle.RefreshEndpointFacts();
    return !handle.ReadEndpointDiagnostic().has_value();
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
      std::size_t executor_threads = 3U,
      core::Duration measured_freshness_ns = 100)
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

/// The step was sent and answered: a definite handle outcome.
[[nodiscard]] bool Answered(const RetainedExternallyManagedPrimitiveOutcome &outcome) {
  return outcome.outcome != Outcome::kNotSent && outcome.outcome != Outcome::kUncertain;
}

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
        {}, std::nullopt, {});
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
    config.state_freshness_timeout_s = 1.0;
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
        core::LifecyclePrimitiveId(std::string("ros2_control.controller.") + primitive), {},
        std::nullopt, {});
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
  ASSERT_EQ(world.Dispatch("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  ASSERT_EQ(world.Dispatch("activate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_EQ(world.Dispatch("deactivate").outcome,
            Outcome::kSucceeded);
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
  ASSERT_EQ(arm_prepared.outcome, Outcome::kSucceeded)
      << (arm_prepared.error ? arm_prepared.error->message : std::string{"no error detail"});
  ASSERT_TRUE(world.Dispatch("prepare").outcome ==
              Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  EXPECT_TRUE(world.manager.Requests().empty());
  ASSERT_TRUE(world.Dispatch("activate", "domain.arm").outcome ==
              Outcome::kSucceeded);
  ASSERT_TRUE(world.Dispatch("activate").outcome ==
              Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive, "domain.arm"));
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_TRUE(world.Dispatch("deactivate").outcome ==
              Outcome::kSucceeded);
  ASSERT_TRUE(world.Dispatch("deactivate", "domain.arm").outcome ==
              Outcome::kSucceeded);
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
  ASSERT_EQ(prepared.outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  EXPECT_EQ(ArmWorld::Fact(world.Snapshot(), "external.controller_loaded"),
            true);
  EXPECT_EQ(ArmWorld::Fact(world.Snapshot(), "external.transport_ready"), true);
  EXPECT_TRUE(world.manager.Requests().empty());
  ASSERT_EQ(world.Dispatch("activate").outcome,
            Outcome::kSucceeded);
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
  EXPECT_FALSE(world.Dispatch("prepare").outcome ==
               Outcome::kSucceeded);
  EXPECT_TRUE(world.manager.Requests().empty());
  world.manager.SetGripper(true, true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return ArmWorld::Fact(world.Snapshot(), "external.controller_loaded") ==
           true;
  }));
  EXPECT_EQ(world.Snapshot().component_state, State::kDegraded);
  EXPECT_FALSE(world.Dispatch("prepare").outcome ==
               Outcome::kSucceeded);
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RefusedGripperSwitchCannotReportActive) {
  GripperWorld world;
  world.handle->StartBackend();
  ASSERT_TRUE(world.WaitForState(State::kDeclared));
  ASSERT_TRUE(world.Dispatch("prepare").outcome ==
              Outcome::kSucceeded);
  world.manager.Refuse(true);
  const auto refused = world.Dispatch("activate");
  EXPECT_EQ(refused.outcome, Outcome::kFailed);
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
  EXPECT_EQ(foreign.outcome,
            Outcome::kNotSent);
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
    // Declared precedes the first controller-manager observation; Prepare
    // needs the controller observed as loaded on active hardware.
    ASSERT_TRUE(fake::WaitUntil([&] {
      return ArmWorld::Fact(world.Snapshot(domain),
                            "external.controller_loaded") == true;
    }));
    const auto prepared = world.Dispatch("prepare", domain);
    ASSERT_EQ(prepared.outcome, Outcome::kSucceeded)
        << (prepared.error ? prepared.error->code + ": " + prepared.error->message
                           : std::string{"no error detail"});
    world.manager.Refuse(true, message);
    const auto refused = world.Dispatch("activate", domain);
    EXPECT_EQ(refused.outcome, Outcome::kFailed);
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
  // Declared precedes the first controller-manager observation and the
  // graph settling; Prepare is refused before effect until both.
  ASSERT_TRUE(fake::WaitUntil([&] {
    return ArmWorld::Fact(world.Snapshot("domain.arm"),
                          "external.controller_loaded") == true &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
  ASSERT_EQ(world.Dispatch("prepare", "domain.arm").outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(world.Dispatch("activate", "domain.arm").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive, "domain.arm"));
  // The prefix before the truncation marker would split this actual UTF-8
  // character without the existing complete-prefix rule.
  world.manager.Refuse(true, std::string(244U, 'x') + "é" + std::string(80U, 'y'));
  const auto refused = world.Dispatch("deactivate", "domain.arm");
  EXPECT_EQ(refused.outcome, Outcome::kFailed);
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
    ASSERT_TRUE(fake::WaitUntil([&] {
      return world.handle->mock_reset_writer_inactive() &&
             RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
    }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  // A reset refused before effects (not sent) reports no phase at all; stop
  // here rather than read `phases.back()` of an empty vector (SIGSEGV).
  ASSERT_EQ(reset.outcome, Outcome::kUncertain)
      << (reset.error ? reset.error->code + ": " + reset.error->message : std::string{});
  ASSERT_TRUE(reset.error);
  EXPECT_NE(reset.error->message.find("Restart the workbench"),
            std::string::npos);
  ASSERT_FALSE(phases.empty());
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

// SX-291/SX-297: a lifecycle switch's answer is awaited for the CCLC
// transaction's remaining command time; one the controller manager accepts
// but never answers ends as an unknown effect, never a success.
struct UnansweredSwitchWorld final {
  UnansweredSwitchWorld() {
    world.handle->StartBackend();
    if (!fake::WaitUntil([&] {
          const auto activity = world.handle->arm_activity();
          return activity.observed && activity.controller_loaded &&
                 activity.hardware_active;
        }) ||
        !world.handle->node()
             ->create_client<controller_manager_msgs::srv::SwitchController>(
                 world.prefix + "/controller_manager/switch_controller")
             ->wait_for_service(10s))
      throw std::logic_error("no controller manager");
    world.manager.SetSwitchRepliesPaused(true);
  }
  ~UnansweredSwitchWorld() { world.manager.SetSwitchRepliesPaused(false); }

  [[nodiscard]] std::future<RetainedExternallyManagedPrimitiveOutcome>
  Activate(std::optional<core::Duration> remaining) {
    return std::async(std::launch::async, [this, remaining] {
      return world.handle->lifecycle_primitive_dispatcher()(
          core::ControlDomainId("domain.arm"),
          core::LifecyclePrimitiveId("ros2_control.controller.activate"),
          std::stop_token{}, remaining, {});
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
     LifecycleSwitchSlowAnswerWithinALongerBudgetIsAccepted) {
  UnansweredSwitchWorld slow;
  auto pending = slow.Activate(20'000'000'000); // 20 s remaining
  ASSERT_TRUE(fake::WaitUntil([&] { return slow.world.manager.reply_waiting(); }));
  // Beyond the 10 s fallback, inside the transaction's command time.
  std::this_thread::sleep_for(11s);
  EXPECT_EQ(pending.wait_for(0ms), std::future_status::timeout);
  slow.world.manager.SetSwitchRepliesPaused(false);
  ASSERT_EQ(pending.wait_for(5s), std::future_status::ready);
  const auto outcome = pending.get();
  EXPECT_TRUE(Answered(outcome))
      << (outcome.error ? outcome.error->details.at("reason") : std::string());
  EXPECT_EQ(slow.world.manager.switch_requests(), 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     LifecycleSwitchWithoutABudgetEndsUncertainAtTheFallback) {
  UnansweredSwitchWorld unanswered;
  const auto started = std::chrono::steady_clock::now();
  auto pending = unanswered.Activate(std::nullopt);
  EXPECT_TRUE(fake::WaitUntil([&] { return unanswered.world.manager.reply_waiting(); }));
  const bool ended = pending.wait_for(20s) == std::future_status::ready;
  const auto elapsed = std::chrono::steady_clock::now() - started;
  unanswered.world.manager.SetSwitchRepliesPaused(false);
  ASSERT_TRUE(ended);
  const auto outcome = pending.get();
  EXPECT_GE(elapsed, 9s);
  EXPECT_EQ(outcome.outcome, Outcome::kUncertain);
  EXPECT_EQ(unanswered.world.manager.switch_requests(), 1U);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockResetLostSwitchReplyNeverUsesActivityAsSuccess) {
  ArmWorld world("arm_controller", {}, nullptr, ArmBinding(), {}, true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->mock_reset_writer_inactive() &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
                                {}, std::nullopt, {});
  EXPECT_EQ(foreign.outcome, Outcome::kNotSent);
  const auto unknown = dispatch(core::ControlDomainId("domain.arm"),
                                core::LifecyclePrimitiveId("ros2_control.controller.unknown"),
                                {}, std::nullopt, {});
  EXPECT_EQ(unknown.outcome, Outcome::kNotSent);
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->arm_activity().controller_loaded &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->arm_activity().controller_loaded &&
           RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle);
  }));
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
  ASSERT_EQ(world.Dispatch("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  core::GripperCommandRequestDTO command;
  command.gripper_id = "gripper.target";
  command.group_id = "gripper";
  command.joint_name = "gripper_joint";
  command.target_position = 0.02;
  command.duration_s = 1.0;
  EXPECT_FALSE(world.gripper_backend->command_gripper(command).accepted);
  ASSERT_EQ(world.Dispatch("activate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_EQ(world.Dispatch("deactivate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  EXPECT_FALSE(world.gripper_backend->command_gripper(command).accepted);
  ASSERT_EQ(world.Dispatch("activate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  EXPECT_TRUE(world.manager.Requests().empty());
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     IsaacEffortGripperLifecycleIsLocalAndSucceedsWithoutControllerSwitch) {
  GripperWorld world(true, {}, false, std::nullopt, true);
  world.handle->StartBackend();
  // Without its Isaac command subscriber the transport is not ready.
  const auto refused = world.Dispatch("prepare");
  EXPECT_EQ(refused.outcome,
            Outcome::kNotSent);
  ASSERT_TRUE(refused.error);
  EXPECT_EQ(refused.error->code, "ros2_control_gripper_bridge_not_ready");
  const auto isaac = world.manager.node()->create_subscription<sensor_msgs::msg::JointState>(
      world.prefix + "/isaac_joint_commands", rclcpp::SystemDefaultsQoS(),
      [](const sensor_msgs::msg::JointState::ConstSharedPtr) {});
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->gripper_transport_readiness_snapshot()->ready;
  }));
  // As before b9c5798c, the ready transport is the active owner: Prepare and
  // Enable targets are already satisfied and nothing dispatches.
  ASSERT_TRUE(world.WaitForState(State::kActive));
  ASSERT_TRUE(fake::WaitUntil([&] {
    // Observe settles the endpoint facts that the pure health read uses.
    const core::ControlDomainId domain("domain.gripper");
    const auto observed =
        world.handle->ReadLifecycleSnapshot(domain, world.clock.now());
    const auto health =
        world.handle->ReadLifecycleHealth(domain, world.clock.now());
    return observed.ok() && health.ok() && health.value().observed_state == State::kActive &&
           health.value().has_activation_step == false;
  }));
  // A dispatched primitive is a local request, as for the no-output owner.
  ASSERT_EQ(world.Dispatch("deactivate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kDisabled));
  ASSERT_EQ(world.Dispatch("prepare").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kPrepared));
  ASSERT_EQ(world.Dispatch("activate").outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto unknown = world.Dispatch("recover");
  ASSERT_TRUE(unknown.error);
  EXPECT_EQ(unknown.error->code, "ros2_control_lifecycle_primitive_unknown");
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
      {}, clock, RetainedMeasuredJointStateLane{
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
  config.state_freshness_timeout_s = 1.0;
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
          std::string(kMeasuredJointStateProfileId),
          core::StreamSeverityDTO::kFault, core::StreamRecoveryDTO::kAck, std::nullopt}).ok());
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
      domain, core::LifecyclePrimitiveId("ros2_control.controller.prepare"), {},
      std::nullopt, {});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
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
  config.state_freshness_timeout_s = 1.0;
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
        core::LifecyclePrimitiveId("ros2_control.controller." + primitive), {},
        std::nullopt, {});
    ASSERT_EQ(outcome.outcome, Outcome::kSucceeded);
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
        core::LifecyclePrimitiveId("ros2_control.controller.activate"), {},
        std::nullopt, {});
    EXPECT_EQ(foreign.outcome, Outcome::kNotSent);
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
    config.state_freshness_timeout_s = 1.0;
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
    if (!no_output) {
      // Merge 6 tool split (M6 slice 04-TOOLSPLIT): an output gripper with
      // no controller-manager facts is an Isaac transport tool (its
      // lifecycle is the action bridge's), so this bind is accepted. A
      // typed-none gripper with controller facts is still refused (below).
      EXPECT_TRUE(bound.ok()) << bound.error().message;
      continue;
    }
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


// R7 package 3: a deterministic vendor adapter in place of the xArm
// `robot_states` subscription.
class FakeDeviceStatusAdapter final : public IRetainedDeviceStatusAdapter {
public:
  void Start(rclcpp::Node &, core::IClock &) override { ++starts; }
  [[nodiscard]] RetainedDeviceStatus Observe(core::TimePoint) override {
    std::lock_guard lock(mutex);
    ++observe_calls;
    return status;
  }
  [[nodiscard]] RetainedDeviceStatus ReadHealth(core::TimePoint) const override {
    std::lock_guard lock(mutex);
    ++health_calls;
    return status;
  }
  void Set(RetainedDeviceStatus value) {
    std::lock_guard lock(mutex);
    status = std::move(value);
  }
  // R7 package 4: the vendor's own effects, gate and primitives.
  void BeginHardwareTransition(bool activate) override {
    std::lock_guard lock(mutex);
    events.push_back(activate ? "begin:arm" : "begin:disarm");
  }
  void SettleHardwareTransition(bool activate, bool effect_possible,
                                core::TimePoint) override {
    std::lock_guard lock(mutex);
    events.push_back(std::string(activate ? "settle:arm:" : "settle:disarm:") +
                     (effect_possible ? "effect" : "none"));
  }
  [[nodiscard]] core::Result<void>
  CheckActivation(core::TimePoint,
                  const std::vector<double> &measured) const override {
    std::lock_guard lock(mutex);
    gate_inputs.push_back(measured);
    // SX-695: lets a test spend step time inside the Enable gate.
    if (on_check_activation) on_check_activation();
    return gate;
  }
  [[nodiscard]] RetainedRos2ControlSwitchResult
  Dispatch(const core::LifecyclePrimitiveId &primitive,
           std::optional<std::chrono::steady_clock::time_point>, std::stop_token) override {
    std::lock_guard lock(mutex);
    dispatched.push_back(primitive.value());
    return dispatch_result;
  }
  // M6 XADAPT: the report each status query reads: the scripted ones in
  // order (the last repeats), else the current status's facts (none while
  // it is stale: the robot does not answer).
  [[nodiscard]] std::optional<std::vector<core::LifecycleFactDTO>>
  ReadReportFacts(std::optional<core::TimePoint>, std::stop_token) const override {
    std::lock_guard lock(mutex);
    if (on_report_read) on_report_read(report_reads);
    if (!reports.empty()) return reports[std::min(report_reads++, reports.size() - 1U)];
    if (status.cause == core::LifecycleBlockingCauseClassDTO::kAvailability)
      return std::nullopt;
    return status.facts;
  }
  void SetReports(std::vector<std::optional<std::vector<core::LifecycleFactDTO>>> value) {
    std::lock_guard lock(mutex);
    reports = std::move(value);
    report_reads = 0U;
  }
  // Active TCP (M7): every tool-context call the handle makes (Enable, a
  // TCP change or reset, a payload update).
  [[nodiscard]] VendorCallResult
  SendToolContext(const VendorToolContextCommand &command, bool at_enable,
                  std::optional<std::chrono::steady_clock::time_point> step_deadline,
                  std::stop_token) override {
    std::function<void()> effect;
    VendorCallResult result;
    {
      std::lock_guard lock(mutex);
      applied_contexts.emplace_back(command, at_enable);
      applied_deadlines.push_back(step_deadline);
      if (!tool_context_result.ok()) return tool_context_result;
      result.rearmed = tool_context_rearmed;
      effect = on_tool_context;
    }
    // M6 ADAPTERM TOOLCTX: an offset send cycles the plugin's controllers.
    if (result.rearmed && effect) effect();
    return result;
  }
  void SetToolContextRearm(bool rearmed, std::function<void()> effect = {}) {
    std::lock_guard lock(mutex);
    tool_context_rearmed = rearmed;
    on_tool_context = std::move(effect);
  }
  // M6: the vendor joint-state topic and its display gate, when set.
  [[nodiscard]] std::optional<std::string> VendorJointStateTopic() const override {
    return vendor_topic;
  }
  // V-A (A4): like the xArm adapter, the all-zero e-stop signature is no
  // pose; otherwise the scripted stream gate.
  [[nodiscard]] VendorJointSampleGate VendorJointSampleGateFn() const override {
    if (!vendor_topic) return {};
    return [open = vendor_gate_open, calls = vendor_gate_calls](
               core::TimePoint, const std::vector<double> &positions) {
      ++*calls;
      if (std::all_of(positions.begin(), positions.end(),
                      [](const double value) { return value == 0.0; }))
        return VendorJointSampleUse::kIgnore;
      return open->load() ? VendorJointSampleUse::kLeads : VendorJointSampleUse::kDisplay;
    };
  }
  // V-A (A3): plays the xArm plugin (its transient; it reactivates every
  // controller itself after a re-arm, the interface's re-arm contract).
  [[nodiscard]] std::chrono::nanoseconds ActivationTransientBound() const override {
    return 5s;
  }
  // V-A (A5): the xArm rule over the scripted status: no error, fresh.
  [[nodiscard]] core::Result<void> CheckArmable(core::TimePoint now) const override {
    const auto health = ReadHealth(now);
    const auto error =
        std::find_if(health.facts.begin(), health.facts.end(), [](const auto &fact) {
          return fact.lifecycle_fact_id.value() == core::kDeviceErrorPresentFactId;
        });
    if (health.cause != core::LifecycleBlockingCauseClassDTO::kNone ||
        error == health.facts.end() || error->value.boolean_value != false)
      return core::Result<void>::Err(core::ErrorDTO{
          "execution_rejected", "the robot reports an error or no fresh status",
          {{"reason", "ros2_control_device_not_armable"}}});
    return core::Result<void>::Ok();
  }
  // M6 ADAPTERM: the re-engage before an admitted motion; `on_reengage`
  // plays the vendor plugin's reactivation of every controller.
  [[nodiscard]] VendorCallResult ReengageAtRest(std::stop_token) override {
    std::function<void()> effect;
    VendorCallResult result;
    {
      std::lock_guard lock(mutex);
      ++reengage_calls;
      result = reengage_result;
      effect = on_reengage;
    }
    if (result.ok() && result.rearmed && effect) effect();
    return result;
  }
  void EndRearm() override { ++end_rearm_calls; }
  void SetReengage(VendorCallResult value, std::function<void()> effect = {}) {
    std::lock_guard lock(mutex);
    reengage_result = std::move(value);
    on_reengage = std::move(effect);
  }
  void SetGate(core::Result<void> value) {
    std::lock_guard lock(mutex);
    gate = std::move(value);
  }
  void SetToolContextResult(VendorCallResult value) {
    std::lock_guard lock(mutex);
    tool_context_result = std::move(value);
  }
  [[nodiscard]] std::vector<std::optional<std::chrono::steady_clock::time_point>>
  AppliedDeadlines() const {
    std::lock_guard lock(mutex);
    return applied_deadlines;
  }
  [[nodiscard]] std::vector<std::pair<VendorToolContextCommand, bool>>
  AppliedContexts() const {
    std::lock_guard lock(mutex);
    return applied_contexts;
  }
  [[nodiscard]] std::vector<std::string> Events() const {
    std::lock_guard lock(mutex);
    return events;
  }
  [[nodiscard]] std::vector<std::string> Dispatched() const {
    std::lock_guard lock(mutex);
    return dispatched;
  }
  [[nodiscard]] std::vector<std::vector<double>> GateInputs() const {
    std::lock_guard lock(mutex);
    return gate_inputs;
  }

  std::atomic<int> starts{0};
  mutable std::mutex mutex;
  mutable int observe_calls{0};
  mutable int health_calls{0};
  RetainedDeviceStatus status;
  std::vector<std::string> events;
  mutable std::vector<std::vector<double>> gate_inputs;
  core::Result<void> gate = core::Result<void>::Ok();
  std::vector<std::string> dispatched;
  RetainedRos2ControlSwitchResult dispatch_result{
      RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  std::vector<std::optional<std::vector<core::LifecycleFactDTO>>> reports;
  mutable std::size_t report_reads{0U};
  /// Runs at each scripted report read with its index (SX-664: a cancel
  /// during the second status query).
  std::function<void(std::size_t)> on_report_read;
  std::function<void()> on_check_activation;
  VendorCallResult tool_context_result;
  bool tool_context_rearmed{false};
  std::function<void()> on_tool_context;
  std::vector<std::pair<VendorToolContextCommand, bool>> applied_contexts;
  std::vector<std::optional<std::chrono::steady_clock::time_point>> applied_deadlines;
  int reengage_calls{0};
  std::atomic<int> end_rearm_calls{0};
  VendorCallResult reengage_result;
  std::function<void()> on_reengage;
  std::optional<std::string> vendor_topic;
  std::shared_ptr<std::atomic<bool>> vendor_gate_open =
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<int>> vendor_gate_calls =
      std::make_shared<std::atomic<int>>(0);
};

// V-A (A2): a vendor call's typed failure, as an adapter reports it.
[[nodiscard]] VendorCallResult VendorFailure(core::ErrorDTO error, bool effect_possible = false,
                                             bool unresponsive = false) {
  return {false, std::move(error), effect_possible, unresponsive};
}

// Active TCP (M7): the offset z of a `Set` command (none otherwise) and the
// payload mass of a load `Set`.
[[nodiscard]] std::optional<double> OffsetZ(const VendorToolContextCommand &command) {
  const auto *set = std::get_if<VendorToolOffsetSet>(&command.offset);
  if (set == nullptr) return std::nullopt;
  return set->tcp_pose_in_flange.position_m.z_m;
}
[[nodiscard]] std::optional<double> LoadKg(const VendorToolContextCommand &command) {
  const auto *set = std::get_if<VendorToolLoadSet>(&command.load);
  if (set == nullptr) return std::nullopt;
  return set->mass_kg;
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

[[nodiscard]] core::LifecycleFactDTO DeviceBoolean(std::string_view id, bool value) {
  return {core::LifecycleFactId(std::string(id)),
          {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt}};
}

[[nodiscard]] RetainedDeviceStatus DeviceStatus(core::LifecycleBlockingCauseClassDTO cause,
                                                bool error) {
  RetainedDeviceStatus result{
      cause,
      {DeviceBoolean(core::kDeviceAcceptsCommandsFactId, false),
       DeviceBoolean(core::kDeviceErrorPresentFactId, error),
       DeviceBoolean(core::kDeviceMotionEnabledFactId, false),
       DeviceBoolean(core::kDeviceWarningPresentFactId, false)}};
  if (error)
    result.facts.push_back({core::LifecycleFactId("manyforge.vendor_code.error"),
                            {core::LifecycleFactValueKindDTO::kToken, std::nullopt,
                             std::nullopt, std::string("C2")}});
  return result;
}

[[nodiscard]] std::optional<std::string> TokenFact(
    const std::vector<core::LifecycleFactDTO> &facts, std::string_view id) {
  for (const auto &fact : facts)
    if (fact.lifecycle_fact_id.value() == id) return fact.value.token_value;
  return std::nullopt;
}

// The plan predicate holds for this fresh state and these facts.
[[nodiscard]] bool Holds(const core::LifecyclePredicateDTO &predicate, State state,
                         const std::vector<core::LifecycleFactDTO> &facts) {
  if (std::find(predicate.allowed_states.begin(), predicate.allowed_states.end(), state) ==
      predicate.allowed_states.end())
    return false;
  return std::all_of(predicate.fact_predicates.begin(), predicate.fact_predicates.end(),
                     [&](const auto &expected) {
                       return std::any_of(facts.begin(), facts.end(), [&](const auto &fact) {
                         return fact.lifecycle_fact_id == expected.lifecycle_fact_id &&
                                fact.value == expected.expected_value;
                       });
                     });
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmVendorStatusBecomesTheMotionDomainsCauseAndFacts) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  ArmWorld world;
  auto device = std::make_shared<FakeDeviceStatusAdapter>();
  device->Set(DeviceStatus(Cause::kNone, false));
  world.handle->InstallDeviceStatusAdapter(device);
  EXPECT_THROW(world.handle->InstallDeviceStatusAdapter(device), std::logic_error);
  // A Real component's hardware starts inactive (R7 D1): connected, not armed.
  world.manager.SetHardwareActive(false);
  world.handle->StartBackend();
  EXPECT_EQ(device->starts.load(), 1);
  EXPECT_THROW(world.handle->InstallDeviceStatusAdapter(
                   std::make_shared<FakeDeviceStatusAdapter>()),
               std::logic_error);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    (void)world.Snapshot();
    return activity.observed && activity.controller_loaded && !activity.hardware_active &&
           world.handle->ReadLifecycleHealth(core::ControlDomainId("domain.arm"),
                                             world.clock.now()).ok();
  }));

  // Clean vendor status: the generic Declared state, the device facts joined.
  const auto clean = world.Snapshot();
  EXPECT_TRUE(clean.locally_valid());
  EXPECT_EQ(clean.component_state, State::kDeclared);
  EXPECT_EQ(ArmWorld::Cause(clean), "none");
  EXPECT_EQ(ArmWorld::Fact(clean, std::string(core::kDeviceErrorPresentFactId)), false);
  EXPECT_EQ(ArmWorld::Fact(clean, "external.controller_loaded"), false);
  // Prepare is never eligible on inactive hardware: it waits without a request.
  for (const auto &plan : retained_ros2_control_lifecycle_plans()) {
    if (plan.intent != core::ComponentLifecycleIntentKindDTO::kPrepare) continue;
    for (const auto &transition : plan.transitions)
      EXPECT_FALSE(Holds(transition.eligibility_predicate, clean.component_state, clean.facts));
  }

  // An emergency stop (C2) faults the domain with that cause and its code.
  device->Set(DeviceStatus(Cause::kEmergencyStop, true));
  const auto stopped = world.Snapshot();
  EXPECT_TRUE(stopped.locally_valid());
  EXPECT_GT(stopped.source_generation, clean.source_generation);
  EXPECT_EQ(stopped.component_state, State::kFaulted);
  EXPECT_EQ(ArmWorld::Cause(stopped), "emergency_stop");
  EXPECT_EQ(ArmWorld::Fact(stopped, "external.fault_present"), true);
  EXPECT_EQ(ArmWorld::Fact(stopped, std::string(core::kDeviceErrorPresentFactId)), true);
  EXPECT_EQ(TokenFact(stopped.facts, "manyforge.vendor_code.error"), "C2");
  EXPECT_FALSE(stopped.exact_controller_configuration_active);

  // Pure health reads the adapter's pure projection and commits nothing.
  const auto observe_calls = device->observe_calls;
  const auto before = RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle);
  const auto health =
      world.handle->ReadLifecycleHealth(core::ControlDomainId("domain.arm"), world.clock.now());
  ASSERT_TRUE(health.ok()) << health.error().message;
  EXPECT_EQ(health.value().observed_state, State::kFaulted);
  EXPECT_EQ(TokenFact(health.value().facts, core::kLifecycleBlockingCauseFactId),
            "emergency_stop");
  EXPECT_EQ(health.value().facts, stopped.facts);
  EXPECT_EQ(device->observe_calls, observe_calls);
  EXPECT_GE(device->health_calls, 1);
  EXPECT_EQ(RetainedRos2ControlRuntimeHandleTestAccess::Capture(*world.handle), before);

  // Stale vendor status cannot clear the reported intervention (existing rule).
  device->Set(DeviceStatus(Cause::kAvailability, false));
  const auto stale = world.Snapshot();
  EXPECT_EQ(stale.component_state, State::kDegraded);
  EXPECT_EQ(ArmWorld::Cause(stale), "emergency_stop");

  // Fresh clean status clears it; stale clean status is availability.
  device->Set(DeviceStatus(Cause::kNone, false));
  EXPECT_EQ(ArmWorld::Cause(world.Snapshot()), "none");
  EXPECT_EQ(world.Snapshot().component_state, State::kDeclared);
  device->Set(DeviceStatus(Cause::kAvailability, false));
  EXPECT_EQ(ArmWorld::Cause(world.Snapshot()), "availability");
  EXPECT_EQ(world.Snapshot().component_state, State::kDegraded);
  // External control and collisions are vendor intervention causes too.
  device->Set(DeviceStatus(Cause::kExternalControl, false));
  EXPECT_EQ(ArmWorld::Cause(world.Snapshot()), "external_control");
  EXPECT_EQ(world.Snapshot().component_state, State::kFaulted);
  EXPECT_EQ(world.manager.switch_requests(), 0U);
}

// --- R7 package 4: the Real arm's actions --------------------------------

[[nodiscard]] core::LifecycleFactDTO DeviceUnknown(std::string_view id) {
  return {core::LifecycleFactId(std::string(id)),
          {core::LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt,
           std::nullopt}};
}

// Armed, ready and error-free (mode 1, state 1/2, every axis enabled).
[[nodiscard]] RetainedDeviceStatus ReadyDeviceStatus(bool warning = false) {
  return {core::LifecycleBlockingCauseClassDTO::kNone,
          {DeviceBoolean(core::kDeviceAcceptsCommandsFactId, true),
           DeviceBoolean(core::kDeviceErrorPresentFactId, false),
           DeviceBoolean(core::kDeviceMotionEnabledFactId, true),
           DeviceBoolean(core::kDeviceWarningPresentFactId, warning)}};
}

// The adapter's stale or missing report: availability, facts unknown.
[[nodiscard]] RetainedDeviceStatus StaleDeviceStatus() {
  return {core::LifecycleBlockingCauseClassDTO::kAvailability,
          {DeviceUnknown(core::kDeviceAcceptsCommandsFactId),
           DeviceUnknown(core::kDeviceErrorPresentFactId),
           DeviceUnknown(core::kDeviceMotionEnabledFactId),
           DeviceUnknown(core::kDeviceWarningPresentFactId)}};
}

/// The joint state broadcaster's `<ns>/joint_states` at 100 Hz, as a peer.
class FakeJointStates final : public fake::PeerNode {
public:
  explicit FakeJointStates(const std::string &topic)
      : PeerNode("manyforge_fake_joint_states") {
    publisher_ = node()->create_publisher<sensor_msgs::msg::JointState>(
        topic, rclcpp::SensorDataQoS());
    timer_ = node()->create_wall_timer(10ms, [this] {
      if (!publishing_.load()) return;
      sensor_msgs::msg::JointState message;
      message.name = {"source_joint_1"};
      message.position = {position_.load()};
      // M6 ADAPTERM: a velocity and a current stamp, as the broadcaster.
      if (with_velocity_.load()) {
        message.velocity = {velocity_.load()};
        message.header.stamp = node()->now();
      }
      publisher_->publish(message);
    });
    Start();
  }
  ~FakeJointStates() override { Stop(); }
  void Publish(double position) {
    position_ = position;
    publishing_ = true;
  }
  void Pause() { publishing_ = false; }
  /// M6 ADAPTERM: also publish this measured velocity (absent otherwise).
  void SetVelocity(double velocity) {
    velocity_ = velocity;
    with_velocity_ = true;
  }
  /// One message with this source stamp (e.g. an old one delivered late).
  void PublishOnce(double position, const rclcpp::Time &stamp) {
    sensor_msgs::msg::JointState message;
    message.header.stamp = stamp;
    message.name = {"source_joint_1"};
    message.position = {position};
    if (with_velocity_.load()) message.velocity = {velocity_.load()};
    publisher_->publish(message);
  }
  [[nodiscard]] rclcpp::Time Now() const { return node()->now(); }

private:
  std::atomic<double> position_{0.0};
  std::atomic<double> velocity_{0.0};
  std::atomic<bool> with_velocity_{false};
  std::atomic<bool> publishing_{false};
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
};

/// A Real arm on the fake controller manager: the vendor adapter installed,
/// the broadcaster listed, measured joints with a 1 s budget, and the Real
/// xArm catalog evaluated by Core's own plan evaluator on pure health.
struct RealArmWorld final {
  [[nodiscard]] static core::BrandLifecyclePlanCatalogDTO RealCatalog() {
    auto made = core::MakeBrandLifecyclePlanCatalog(
        core::ProviderManifestId(
            std::string(xarm_control::kXArmRos2ControlLifecycleProviderManifestId)),
        // Core requires a sha256-shaped manifest revision.
        core::ProviderManifestRevision(
            "sha256:1111111111111111111111111111111111111111111111111111111111111111"),
        xarm_control::make_xarm_ros2_control_lifecycle_plans());
    if (!made.ok()) throw std::logic_error(made.error().message);
    return made.value();
  }

  explicit RealArmWorld(bool hardware_active = false, bool offset_sync = true)
      : world("arm_controller", std::nullopt, nullptr, ArmBinding(), {}, false,
              {}, {}, 3U, 1'000'000'000),
        catalog(RealCatalog()),
        occurrence{core::ControlDomainId("domain.arm"),
                   core::ControlDomainRevision("domain-revision"),
                   core::ProviderInstanceId("provider.ros2_control"),
                   catalog.provider_manifest_id,
                   catalog.provider_manifest_revision,
                   core::RuntimeUnitId("runtime.ros2_control"),
                   core::RuntimeEpoch("runtime-epoch.1"),
                   catalog.catalog_digest} {
    device->Set(ReadyDeviceStatus());
    world.handle->InstallDeviceStatusAdapter(device);
    // TOOLCTX: a source whose TCP is the frame asked for (the default:
    // "default_tcp") with a 0.5 kg payload.
    world.handle->InstallToolContextSource(
        [this](const std::optional<std::string> &frame)
            -> core::Result<core::ToolContextDTO> {
          std::lock_guard lock(tool_mutex);
          tool_frames.push_back(frame.value_or("default_tcp"));
          if (!tool_source_result.ok())
            return core::Result<core::ToolContextDTO>::Err(tool_source_result.error());
          return core::Result<core::ToolContextDTO>::Ok(
              {{{0.0, 0.0, frame && *frame != "default_tcp" ? 0.2 : 0.1}, {0.0, 0.0, 0.0, 1.0}},
               tool_payload_kg,
               {0.0, 0.0, 0.04}});
        });
    world.handle->InstallCommittedToolContextSource(
        [this](const std::optional<std::string> &,
               const core::LogicalSceneSnapshotDTO &scene)
            -> core::Result<core::ToolContextDTO> {
          std::lock_guard lock(tool_mutex);
          if (!tool_source_result.ok())
            return core::Result<core::ToolContextDTO>::Err(tool_source_result.error());
          return core::Result<core::ToolContextDTO>::Ok(
              {{}, static_cast<double>(scene.revision.revision_id), {0.0, 0.0, 0.04}});
        });
    world.handle->InstallCommittedEnableToolContextSource(
        [this](const std::optional<std::string> &frame,
               const core::LogicalSceneSnapshotDTO &)
            -> core::Result<core::ToolContextDTO> {
          std::lock_guard lock(tool_mutex);
          tool_frames.push_back(frame.value_or("default_tcp"));
          if (!tool_source_result.ok())
            return core::Result<core::ToolContextDTO>::Err(tool_source_result.error());
          return core::Result<core::ToolContextDTO>::Ok(
              {{{0.0, 0.0, frame && *frame != "default_tcp" ? 0.2 : 0.1},
                {0.0, 0.0, 0.0, 1.0}}, tool_payload_kg, {0.0, 0.0, 0.04}});
        });
    committed_scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
        core::LogicalSceneSnapshotDTO{
            {core::SceneId("sha256:" + std::string(64, 'a')), 1U,
             core::ContentDigest("sha256:" + std::string(64, 'b'))},
            core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
    world.handle->InstallCommittedSceneSource([this] {
      // A test may hold one read (the source is installed once, before start).
      std::function<void()> hook;
      {
        std::lock_guard lock(tool_mutex);
        hook = std::exchange(scene_read_hook, {});
      }
      if (hook) hook();
      std::lock_guard lock(tool_mutex);
      return committed_scene;
    });
    // Active TCP (M7): its End-effector TCP `default_tcp` and two others.
    world.handle->InstallActiveTcp(
        kRobot, TcpRows({"default_tcp", "tool_tcp_link", "finger_link"}), offset_sync);
    world.manager.SetHardwareActive(hardware_active);
    world.manager.SetBroadcasterActive(true);
    world.handle->StartBackend();
    if (!WaitForActivity(hardware_active, false, true))
      throw std::logic_error("no controller-manager activity");
    // The handle's own clients share its node's graph.
    const auto node = world.handle->node();
    if (!node->create_client<controller_manager_msgs::srv::SetHardwareComponentState>(
                 world.prefix + "/controller_manager/set_hardware_component_state")
             ->wait_for_service(10s) ||
        !node->create_client<controller_manager_msgs::srv::SwitchController>(
                 world.prefix + "/controller_manager/switch_controller")
             ->wait_for_service(10s) ||
        // M6 XADAPT: the status queries of every Real step.
        !node->create_client<controller_manager_msgs::srv::ListControllers>(
                 world.prefix + "/controller_manager/list_controllers")
             ->wait_for_service(10s) ||
        !node->create_client<controller_manager_msgs::srv::ListHardwareComponents>(
                 world.prefix + "/controller_manager/list_hardware_components")
             ->wait_for_service(10s))
      throw std::logic_error("no controller-manager services");
  }

  [[nodiscard]] bool WaitForActivity(bool hardware, bool controller,
                                     bool broadcaster) {
    return fake::WaitUntil([&] {
      const auto activity = world.handle->arm_activity();
      return activity.observed && activity.controller_loaded &&
             activity.hardware_active == hardware &&
             activity.controller_active == controller &&
             activity.broadcaster_active == broadcaster;
    });
  }

  /// Keep the broadcaster publishing until the handle reports valid joints.
  [[nodiscard]] bool PublishJoints(double position) {
    return fake::WaitUntil([&] {
      joints.Publish(position);
      return ArmWorld::Fact(world.Snapshot(), "external.measured_joints_valid") ==
             true;
    });
  }

  [[nodiscard]] core::LifecycleDomainHealthDTO Health() {
    return HealthAt(world.clock.now());
  }

  /// Pure health against a caller's boundary (a sample newer than it is
  /// never stale by the budget).
  [[nodiscard]] core::LifecycleDomainHealthDTO HealthAt(core::TimePoint boundary) {
    const auto health = world.handle->ReadLifecycleHealth(
        core::ControlDomainId("domain.arm"), boundary);
    if (!health.ok()) throw std::logic_error(health.error().message);
    return health.value();
  }

  [[nodiscard]] static std::optional<bool> MeasuredValid(
      const core::LifecycleDomainHealthDTO &health) {
    for (const auto &row : health.facts)
      if (row.lifecycle_fact_id.value() == "external.measured_joints_valid")
        return row.value.boolean_value;
    return std::nullopt;
  }

  /// The selected plan's transition for `primitive`.
  [[nodiscard]] const core::LifecyclePrimitiveTransitionDTO &Transition(
      core::ComponentLifecycleIntentKindDTO intent, std::string_view primitive) const {
    for (const auto &plan : catalog.plans)
      if (plan.intent == intent)
        for (const auto &transition : plan.transitions)
          if (transition.lifecycle_primitive_id.value() == primitive) return transition;
    throw std::logic_error("no such transition");
  }

  /// Core's decision for the fresh pure health: the primitive to dispatch,
  /// "satisfied", or the refusal's reason.
  [[nodiscard]] std::string Decide(core::ComponentLifecycleIntentKindDTO intent,
                                   State requested) {
    static_cast<void>(world.Snapshot()); // the Observe checkpoint
    const auto decision =
        core::EvaluateLifecycleDomainPlan(intent, requested, catalog, occurrence, Health());
    if (!decision.ok()) return "refused:" + decision.error().details.at("reason");
    if (decision.value().kind == core::LifecycleDomainPlanDecisionKind::kAlreadySatisfied)
      return "satisfied";
    return decision.value().selected_transition->lifecycle_primitive_id.value();
  }

  /// `remaining`: the CCLC step's remaining budget, as CCLC passes it.
  [[nodiscard]] RetainedExternallyManagedPrimitiveOutcome Dispatch(
      std::string_view primitive,
      const std::optional<core::Duration> remaining = std::nullopt) {
    return world.handle->lifecycle_primitive_dispatcher()(
        core::ControlDomainId("domain.arm"),
        core::LifecyclePrimitiveId(std::string(primitive)), std::stop_token{},
        remaining, {});
  }

  [[nodiscard]] std::vector<std::string> ToolFrames() {
    std::lock_guard lock(tool_mutex);
    return tool_frames;
  }

  ArmWorld world;
  std::shared_ptr<FakeDeviceStatusAdapter> device =
      std::make_shared<FakeDeviceStatusAdapter>();
  std::mutex tool_mutex;
  std::vector<std::string> tool_frames;
  core::Result<void> tool_source_result = core::Result<void>::Ok();
  /// Active TCP (M7): the total payload the source reports.
  double tool_payload_kg{0.5};
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> committed_scene;
  std::function<void()> scene_read_hook;
  FakeJointStates joints{world.prefix + "/joint_states"};
  core::BrandLifecyclePlanCatalogDTO catalog;
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
};

using Intent = core::ComponentLifecycleIntentKindDTO;

[[nodiscard]] std::string Reason(const RetainedExternallyManagedPrimitiveOutcome &outcome) {
  return outcome.error ? outcome.error->details.at("reason") : std::string();
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmPrepareArmsTheHardwareAndAbsorbsThePluginsReactivateAll) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDeclared);
  EXPECT_EQ(ArmWorld::Fact(real.world.Snapshot(), "external.controller_spawned"), true);

  // D1: Prepare arms through the stock hardware-state service. §3.3: the
  // plugin's first write() cycles stall the loop and deactivate every
  // controller (the broadcaster too)...
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.hardware.activate");
  real.world.manager.SetHardwareActivateHook([&] {
    real.joints.Pause();
    real.world.manager.SetAllControllersActive(false);
  });
  auto pending = std::async(std::launch::async, [&] {
    return real.Dispatch("ros2_control.hardware.activate");
  });
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  // ...and the activation waits for the transient to end.
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  // The plugin's reactivate-all brings both back; the loop runs again.
  real.world.manager.SetAllControllersActive(true);
  real.joints.Publish(0.25);
  const auto armed = pending.get();
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
  EXPECT_EQ(real.device->Events(),
            (std::vector<std::string>{"begin:arm", "settle:arm:effect"}));
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  // Armed, still unarmed in lifecycle terms until ManyForge marks it.
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDeclared);

  // The controller the plugin turned on is switched off again...
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.controller.deactivate");
  const auto absorbed = real.Dispatch("ros2_control.controller.deactivate");
  ASSERT_TRUE(Answered(absorbed)) << Reason(absorbed);
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
  // ...and only then is the arm marked prepared.
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.controller.prepare");
  const auto prepared = real.Dispatch("ros2_control.controller.prepare");
  ASSERT_TRUE(Answered(prepared)) << Reason(prepared);
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared), "satisfied");
  EXPECT_EQ(real.world.Snapshot().component_state, State::kPrepared);
  // ManyForge never activated a controller during this Prepare.
  for (const auto &[activate, deactivate] : real.world.manager.switch_history())
    EXPECT_TRUE(activate.empty());
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
  // Enable is now the next step, behind its gate.
  EXPECT_EQ(real.Decide(Intent::kEnable, State::kActive),
            "ros2_control.controller.activate");
}

// Live 2026-10-28: the plugin's reactivate-all after the hardware activation
// failed silently and left the broadcaster off; Prepare had already been
// declared on a young sample from before the plugin's stall.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmPrepareReactivatesTheBroadcasterThePluginLeftOff) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto before_activation = real.world.clock.now();
  EXPECT_EQ(RealArmWorld::MeasuredValid(real.HealthAt(before_activation)), true);

  // The broadcaster goes inactive right after the hardware activation, and
  // the plugin never reactivates it.
  real.world.manager.SetHardwareActivateHook([&] {
    real.joints.Pause();
    real.world.manager.SetAllControllersActive(false);
  });
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.hardware.activate");
  const auto started = std::chrono::steady_clock::now();
  const auto armed = real.Dispatch("ros2_control.hardware.activate");
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  // It waited for the plugin's transient, which never brought a sample.
  EXPECT_GE(std::chrono::steady_clock::now() - started, 4s);
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  // A sample from before the activation never counts again, even where the
  // budget alone would still accept it.
  EXPECT_EQ(RealArmWorld::MeasuredValid(real.HealthAt(before_activation)), false);
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDeclared);

  // Prepare switches the broadcaster on alone (one strict request).
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.broadcaster.activate");
  const auto switches = real.world.manager.switch_requests();
  const auto broadcasting = real.Dispatch("ros2_control.broadcaster.activate");
  ASSERT_TRUE(Answered(broadcasting)) << Reason(broadcasting);
  EXPECT_EQ(real.world.manager.switch_requests(), switches + 1U);
  const auto history = real.world.manager.switch_history();
  ASSERT_FALSE(history.empty());
  EXPECT_EQ(history.back().first, std::vector<std::string>{"joint_state_broadcaster"});
  EXPECT_TRUE(history.back().second.empty());
  ASSERT_TRUE(real.WaitForActivity(true, false, true));

  // Not prepared before the broadcaster's first sample after the activation:
  // the step's postcondition keeps CCLC waiting, nothing else is eligible.
  const auto &step =
      real.Transition(Intent::kPrepare, "ros2_control.broadcaster.activate");
  static_cast<void>(real.world.Snapshot());
  EXPECT_FALSE(Holds(step.postcondition_predicate, State::kDeclared, real.Health().facts));
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "refused:no_transition_satisfied");
  ASSERT_TRUE(real.PublishJoints(0.25));
  static_cast<void>(real.world.Snapshot());
  EXPECT_TRUE(Holds(step.postcondition_predicate, State::kDeclared, real.Health().facts));
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.controller.prepare");
  const auto prepared = real.Dispatch("ros2_control.controller.prepare");
  ASSERT_TRUE(Answered(prepared)) << Reason(prepared);
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared), "satisfied");
  EXPECT_EQ(real.world.Snapshot().component_state, State::kPrepared);

  // An active broadcaster is never switched again; the controller never was.
  const auto again = real.Dispatch("ros2_control.broadcaster.activate");
  EXPECT_TRUE(Answered(again)) << Reason(again);
  EXPECT_EQ(real.world.manager.switch_requests(), switches + 1U);
  EXPECT_FALSE(real.world.handle->arm_activity().controller_active);
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
}

// SX-657: the post-activation wait is a per-call limit, min(5 s, the step's
// remaining budget). A broadcaster the plugin left off ends it at 5 s, so a
// 10 s Prepare still has time for the broadcaster step; a shorter remaining
// budget ends it earlier.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmActivationWaitLeavesStepBudgetForTheBroadcasterStep) {
  constexpr core::Duration kStepNs = 10'000'000'000LL;
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareActivateHook([&] {
    real.joints.Pause();
    real.world.manager.SetAllControllersActive(false);
  });
  const auto started = std::chrono::steady_clock::now();
  const auto armed = real.Dispatch("ros2_control.hardware.activate", kStepNs);
  const auto waited = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_GE(waited, 4s);
  EXPECT_LT(waited, 7s);  // the 5 s limit, not the whole 10 s step
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  ASSERT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.broadcaster.activate");
  const auto left = kStepNs - std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
  ASSERT_GT(left, 2'000'000'000LL);
  const auto broadcasting = real.Dispatch("ros2_control.broadcaster.activate", left);
  ASSERT_TRUE(Answered(broadcasting)) << Reason(broadcasting);
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmActivationWaitNeverOutlastsTheStepBudget) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareActivateHook([&] {
    real.joints.Pause();
    real.world.manager.SetAllControllersActive(false);
  });
  const auto started = std::chrono::steady_clock::now();
  const auto armed = real.Dispatch("ros2_control.hardware.activate", 2'000'000'000LL);
  const auto waited = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_LT(waited, 4s);  // 2 s remaining, not the 5 s limit
}

// SX-290: the activation's wait ends only on the measured joints Prepare
// requires, never on an invalid sample merely received after the cutoff.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmActivationWaitIgnoresAnInvalidPostActivationSample) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareActivateHook([&] { real.joints.Pause(); });
  auto pending = std::async(std::launch::async, [&] {
    return real.Dispatch("ros2_control.hardware.activate");
  });
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
  // Only NaN samples after the cutoff (budget 1 s): still waiting.
  real.joints.Publish(std::numeric_limits<double>::quiet_NaN());
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(pending.wait_for(0ms), std::future_status::timeout);
  EXPECT_EQ(ArmWorld::Fact(real.world.Snapshot(), "external.measured_joints_valid"),
            false);
  // The first valid sample ends it; Prepare goes on, never refused.
  real.joints.Publish(0.25);
  const auto armed = pending.get();
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.controller.prepare");
}

// SX-290, documented limit: the cutoff is on receipt time, not the source
// stamp. A pre-activation message delivered after the cutoff is accepted.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmLateDeliveredPreActivationSampleCountsByReceiptTime) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto old_stamp = real.joints.Now();
  real.world.manager.SetHardwareActivateHook([&] { real.joints.Pause(); });
  auto pending = std::async(std::launch::async, [&] {
    return real.Dispatch("ros2_control.hardware.activate");
  });
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
  std::this_thread::sleep_for(1200ms); // past the cutoff (budget 1 s)
  EXPECT_EQ(pending.wait_for(0ms), std::future_status::timeout);
  for (int copy = 0; copy < 3; ++copy) { // best-effort QoS: a few copies
    real.joints.PublishOnce(0.25, old_stamp);
    std::this_thread::sleep_for(10ms);
  }
  const auto armed = pending.get();
  ASSERT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_EQ(ArmWorld::Fact(real.world.Snapshot(), "external.measured_joints_valid"),
            true);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmPrepareRefusesWithVendorErrorOrStaleStatusBeforeAnyEffect) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  // C2 under a pressed emergency stop: Faulted, nothing eligible.
  real.device->Set(DeviceStatus(core::LifecycleBlockingCauseClassDTO::kEmergencyStop, true));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "refused:no_transition_satisfied");
  // A direct request fails before effect too.
  const auto direct = real.Dispatch("ros2_control.hardware.activate");
  EXPECT_EQ(direct.outcome, Outcome::kNotSent);
  EXPECT_EQ(Reason(direct), "ros2_control_device_not_armable");

  // Stale or missing status: the device facts are unknown. M6 XADAPT: the
  // step's status query gets no robot report in time: the robot controller
  // is unresponsive, refused before effect.
  real.device->Set(StaleDeviceStatus());
  EXPECT_NE(real.Decide(Intent::kPrepare, State::kPrepared).rfind("refused:", 0),
            std::string::npos);
  const auto stale = real.Dispatch("ros2_control.hardware.activate");
  EXPECT_EQ(stale.outcome, Outcome::kNotSent);
  ASSERT_TRUE(stale.error);
  EXPECT_EQ(stale.error->code, "robot_controller_unresponsive");

  // An error the operator has not acknowledged, with otherwise clean status.
  auto error = ReadyDeviceStatus();
  error.facts[1] = DeviceBoolean(core::kDeviceErrorPresentFactId, true);
  real.device->Set(error);
  EXPECT_EQ(Reason(real.Dispatch("ros2_control.hardware.activate")),
            "ros2_control_device_not_armable");

  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  EXPECT_TRUE(real.device->Events().empty());
  EXPECT_FALSE(real.world.handle->arm_activity().hardware_active);
}

// M6 XADAPT: a refused Real arm step is decided by the status queried
// before it and after its answer: the controller manager's listed states and
// a robot report received after each query started. Only the vendor facts
// and the hardware, controller and broadcaster states count. A missing one
// (the manager or the robot did not answer) is never definite.
TEST(RetainedRos2ControlRuntimeHandleTest, RefusedStepOutcomeComparesTheQueriedStatus) {
  using Facts = std::vector<core::LifecycleFactDTO>;
  const Facts report = ReadyDeviceStatus().facts;
  Facts changed = report;
  changed[2] = DeviceBoolean(core::kDeviceMotionEnabledFactId, false);
  RetainedRos2ControlArmActivity listed;
  listed.controller_manager_present = true;
  listed.observed = true;
  listed.controller_loaded = true;
  listed.broadcaster_active = true;
  EXPECT_EQ(refused_step_outcome(report, report, listed, listed), Outcome::kFailedNoEffect);
  EXPECT_EQ(refused_step_outcome(report, changed, listed, listed),
            Outcome::kFailedEffectObserved);
  for (const auto change : {&RetainedRos2ControlArmActivity::hardware_active,
                            &RetainedRos2ControlArmActivity::controller_active,
                            &RetainedRos2ControlArmActivity::controller_loaded,
                            &RetainedRos2ControlArmActivity::broadcaster_active}) {
    auto after = listed;
    after.*change = !(after.*change);
    EXPECT_EQ(refused_step_outcome(report, report, listed, after),
              Outcome::kFailedEffectObserved);
  }
  // Unreachable: no report or no listed states, before or after.
  EXPECT_EQ(refused_step_outcome(std::nullopt, report, listed, listed), Outcome::kFailed);
  EXPECT_EQ(refused_step_outcome(report, std::nullopt, listed, listed), Outcome::kFailed);
  EXPECT_EQ(refused_step_outcome(report, report, std::nullopt, listed), Outcome::kFailed);
  EXPECT_EQ(refused_step_outcome(report, report, listed, std::nullopt), Outcome::kFailed);
}

// M6 XADAPT: a hardware activation the controller manager refuses, with the
// status queried before and after it: unchanged is no effect, a changed
// report an observed effect. The robot not answering the second query
// within its bound is `robot_controller_unresponsive`: a known failure whose
// observed effect is the component fault it raises (SX-661: the typed code
// reaches the transaction's result), which the next answered query ends.
// The manager not answering the baseline refuses the step before effect,
// `controller_manager_unresponsive`, a component fault too. Each failure
// carries the queried status.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmRefusedStepIsDefiniteFromQueriesAndUnresponsiveIsAFault) {
  using Facts = std::vector<core::LifecycleFactDTO>;
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareRefused(true);
  const auto report = ReadyDeviceStatus().facts;
  auto changed = report;
  changed[2] = DeviceBoolean(core::kDeviceMotionEnabledFactId, false);
  const auto unresponsive_fault = [&](const std::string &code) {
    const auto snapshot = real.world.Snapshot();
    return snapshot.component_state == State::kFaulted && ArmWorld::Cause(snapshot) == "fault" &&
           ArmWorld::Fact(snapshot, "external.fault_present") == true &&
           snapshot.diagnostic && snapshot.diagnostic->code == code;
  };

  for (const auto &[after, expected] : std::vector<std::pair<std::optional<Facts>, Outcome>>{
           {report, Outcome::kFailedNoEffect}, {changed, Outcome::kFailedEffectObserved}}) {
    real.device->SetReports({report, after});
    const auto lists = real.world.manager.list_requests();
    const auto refused = real.Dispatch("ros2_control.hardware.activate");
    EXPECT_EQ(refused.outcome, expected);
    EXPECT_EQ(Reason(refused), "ros2_control_hardware_activation_refused");
    ASSERT_TRUE(refused.error);
    EXPECT_EQ(refused.error->details.at("vendor_status"), "none");
    EXPECT_EQ(refused.error->details.at("ros2_control_status"),
              "hardware=inactive controller=inactive broadcaster=active");
    // Queried before and after, never read from a cached sample.
    EXPECT_EQ(real.world.manager.list_requests(), lists + 2U);
    EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
  }

  // The robot does not answer after the refusal.
  real.device->SetReports({report, std::nullopt});
  const auto silent = real.Dispatch("ros2_control.hardware.activate");
  EXPECT_EQ(silent.outcome, Outcome::kFailedEffectObserved);
  ASSERT_TRUE(silent.error);
  EXPECT_EQ(silent.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(silent.error->details.at("step_reason"), "ros2_control_hardware_activation_refused");
  EXPECT_EQ(silent.error->details.at("vendor_status"), "unavailable");
  EXPECT_TRUE(unresponsive_fault("robot_controller_unresponsive"));
  // A component fault: nothing is armed over it.
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared), "refused:no_transition_satisfied");
  // The next answered query ends it.
  real.device->SetReports({report});
  static_cast<void>(real.Dispatch("ros2_control.hardware.activate"));
  EXPECT_FALSE(unresponsive_fault("robot_controller_unresponsive"));
  EXPECT_EQ(real.world.manager.hardware_history().size(), 4U);

  // The controller manager receives the baseline query but does not answer
  // it within the time it was allowed (here the step's last 1 s): refused
  // before any request, the connection lost.
  real.world.manager.SetListRepliesPaused(true);
  const auto started = std::chrono::steady_clock::now();
  const auto unanswered = real.world.handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.arm"),
      core::LifecyclePrimitiveId("ros2_control.hardware.activate"), std::stop_token{},
      core::Duration{1'000'000'000}, {});
  real.world.manager.SetListRepliesPaused(false);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_EQ(unanswered.outcome, Outcome::kNotSent);
  ASSERT_TRUE(unanswered.error);
  EXPECT_EQ(unanswered.error->code, "controller_manager_unresponsive");
  EXPECT_EQ(unanswered.error->details.at("ros2_control_status"), "unavailable");
  EXPECT_EQ(real.world.manager.hardware_history().size(), 4U);
  EXPECT_TRUE(unresponsive_fault("controller_manager_unresponsive"));
  EXPECT_FALSE(real.world.handle->arm_activity().hardware_active);
}

// M6 XADAPT (SX-663): a controller manager alive but whose list services
// are not discovered yet is refused before effect, typed
// (`ros2_control_state_query_service_unavailable`), like an undiscovered
// hardware-state or switch service: no fault. Once discovered, the step runs.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmStepWithUndiscoveredQueryServicesIsRefusedWithoutAFault) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.DropListServices();
  const auto probe = real.world.handle->node()->create_client<
      controller_manager_msgs::srv::ListControllers>(real.world.prefix +
                                                     "/controller_manager/list_controllers");
  ASSERT_TRUE(fake::WaitUntil([&] { return !probe->service_is_ready(); }));
  const auto lists = real.world.manager.list_requests();
  const auto refused = real.Dispatch("ros2_control.hardware.activate");
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  EXPECT_EQ(Reason(refused), "ros2_control_state_query_service_unavailable");
  EXPECT_EQ(real.world.manager.list_requests(), lists);
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);

  // Discovered (late): the step runs.
  real.world.manager.ServeListServices();
  ASSERT_TRUE(probe->wait_for_service(10s));
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->node()
        ->create_client<controller_manager_msgs::srv::ListHardwareComponents>(
            real.world.prefix + "/controller_manager/list_hardware_components")
        ->service_is_ready();
  }));
  const auto armed = real.Dispatch("ros2_control.hardware.activate");
  EXPECT_TRUE(Answered(armed)) << Reason(armed);
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
}

// M6 XADAPT (SX-675, D8): the step's budget bounds the baseline. A list
// query the manager does not answer within the time it was allowed (the
// step's last 0.3 s) is the connection lost: refused before any hardware
// command, the arm Faulted. A baseline that is answered but leaves no time
// to send the step is refused with the lifecycle step timeout: nothing
// sent, no fault.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmHungListQueryIsTheConnectionLostAndNoTimeLeftSendsNothing) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto activate = [&](const core::Duration remaining) {
    return real.world.handle->lifecycle_primitive_dispatcher()(
        core::ControlDomainId("domain.arm"),
        core::LifecyclePrimitiveId("ros2_control.hardware.activate"), std::stop_token{},
        remaining, {});
  };

  real.world.manager.SetListRepliesPaused(true);
  auto started = std::chrono::steady_clock::now();
  const auto hung = activate(300'000'000);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 900ms);
  real.world.manager.SetListRepliesPaused(false);
  EXPECT_EQ(hung.outcome, Outcome::kNotSent);
  ASSERT_TRUE(hung.error);
  EXPECT_EQ(hung.error->code, "controller_manager_unresponsive");
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  auto snapshot = real.world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kFaulted);
  ASSERT_TRUE(snapshot.diagnostic);
  EXPECT_EQ(snapshot.diagnostic->code, "controller_manager_unresponsive");

  // The baseline is answered (ending the fault), but the robot's report
  // takes 0.4 s of the 0.3 s left: no time to send the step.
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = [](const std::size_t) {
      std::this_thread::sleep_for(400ms);
    };
  }
  const auto late = activate(300'000'000);
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = {};
  }
  EXPECT_EQ(late.outcome, Outcome::kNotSent);
  ASSERT_TRUE(late.error);
  EXPECT_EQ(late.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(Reason(late), "deadline_exhausted");
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
}

// M6 XADAPT (SX-690): "no time left, nothing sent" holds at the send
// itself. A disarm whose step started with time left waits for the switch
// lock (held by an arming whose hardware-state reply the manager delays);
// when it gets the lock its 0.3 s are gone: refused right before its
// hardware-state request, `component_lifecycle_deadline_expired`, nothing
// sent, no fault. With no time left, its proof query is not sent either
// (SX-691), and the step keeps its outcome.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmBudgetSpentWaitingForTheSwitchLockSendsNothing) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareActivateHook(
      [] { std::this_thread::sleep_for(std::chrono::milliseconds(800)); });
  auto arming = std::async(std::launch::async,
                           [&] { return real.Dispatch("ros2_control.hardware.activate"); });
  // The arming holds the switch lock while its reply is delayed.
  ASSERT_TRUE(fake::WaitUntil([&] { return real.world.manager.hardware_history().size() == 1U; }));
  const auto lists = real.world.manager.list_requests();
  const auto disarm = real.world.handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.arm"),
      core::LifecyclePrimitiveId("ros2_control.hardware.deactivate"), std::stop_token{},
      core::Duration{300'000'000}, {});
  EXPECT_EQ(disarm.outcome, Outcome::kNotSent);
  ASSERT_TRUE(disarm.error);
  EXPECT_EQ(disarm.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(Reason(disarm), "deadline_exhausted");
  // Only the arming's request reached the manager; the baseline was sent
  // (two list queries), the proof query was not.
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
  EXPECT_EQ(real.world.manager.list_requests(), lists + 1U);
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
  real.world.manager.SetHardwareActivateHook({});
  ASSERT_TRUE(real.PublishJoints(0.25));  // ends the arming's transient wait
  EXPECT_TRUE(Answered(arming.get()));
}

// M6 XADAPT (SX-695 1): a status query that could not be sent (its service
// not discovered) is never a non-answer. Here the list services disappear
// after the baseline; the refusal's proof query is not sent, so the
// refusal's effect is unknown: uncertain with the existing
// `ros2_control_state_query_service_unavailable`, and no fault.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmProofQueryWithoutDiscoveredServicesIsUncertainWithoutAFault) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareRefused(true);
  const auto probe = real.world.handle->node()->create_client<
      controller_manager_msgs::srv::ListControllers>(real.world.prefix +
                                                     "/controller_manager/list_controllers");
  bool dropped = false;
  {
    std::lock_guard lock(real.device->mutex);
    // The baseline's report read: the list services go away after it.
    real.device->on_report_read = [&](const std::size_t) {
      if (dropped) return;
      dropped = true;
      real.world.manager.DropListServices();
      static_cast<void>(fake::WaitUntil([&] { return !probe->service_is_ready(); }));
    };
  }
  const auto refused = real.Dispatch("ros2_control.hardware.activate");
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = {};
  }
  ASSERT_TRUE(dropped);
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{true});
  EXPECT_EQ(refused.outcome, Outcome::kUncertain);
  EXPECT_EQ(Reason(refused), "ros2_control_state_query_service_unavailable");
  ASSERT_TRUE(refused.error);
  EXPECT_EQ(refused.error->details.at("step_reason"),
            "ros2_control_hardware_activation_refused");
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
}

// M6 XADAPT (SX-695 3): the Real switch is refused at its send once its
// allowed time is gone: Enable with 0.3 s left spends 0.4 s in its gate;
// `component_lifecycle_deadline_expired`, no switch request, no fault.
TEST(RetainedRos2ControlRuntimeHandleTest, RealArmEnableWithNoTimeLeftAtTheSwitchSendsNothing) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_check_activation = [] { std::this_thread::sleep_for(400ms); };
  }
  const auto switches = real.world.manager.switch_requests();
  const auto enabled = real.world.handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.arm"),
      core::LifecyclePrimitiveId("ros2_control.controller.activate"), std::stop_token{},
      core::Duration{300'000'000}, {});
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_check_activation = {};
  }
  EXPECT_EQ(enabled.outcome, Outcome::kNotSent);
  ASSERT_TRUE(enabled.error);
  EXPECT_EQ(enabled.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(real.world.manager.switch_requests(), switches);
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
}

// M6 XADAPT (SX-664): a refused step whose second status query is cut short
// by cancellation has no evidence: an uncertain outcome (the journal
// accepts it after an observed step), never `{Known, Possible}`, and no
// unresponsive fault. And (SX-664 2) an adapter's own no-effect proof does
// not stand when the queried manager states changed.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmCancelDuringTheProofQueryIsUncertainAndAChangeOverridesNoEffect) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.world.manager.SetHardwareRefused(true);
  const auto report = ReadyDeviceStatus().facts;
  std::stop_source stop;
  // The second report read is where the cancel lands: it returns nothing.
  real.device->SetReports({report, std::nullopt});
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = [&](const std::size_t read) {
      if (read == 1U) stop.request_stop();
    };
  }
  const auto cancelled = real.world.handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.arm"),
      core::LifecyclePrimitiveId("ros2_control.hardware.activate"), stop.get_token(),
      std::nullopt, {});
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = {};
  }
  EXPECT_EQ(cancelled.outcome, Outcome::kUncertain);
  EXPECT_EQ(Reason(cancelled), "ros2_control_status_query_cancelled");
  ASSERT_TRUE(cancelled.error);
  EXPECT_EQ(cancelled.error->details.at("step_reason"),
            "ros2_control_hardware_activation_refused");
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);

  // A clear the adapter proved had no effect on the robot, while the
  // manager's broadcaster went off between the two queries (switched at the
  // baseline's report read, after its states were listed): an observed
  // effect.
  real.device->SetReports({});
  real.device->dispatch_result = {
      RetainedRos2ControlSwitchOutcome::kFailedNoEffect,
      core::ErrorDTO{"execution_rejected", "the emergency stop is still engaged (C2)",
                     {{"reason", "xarm_error_persists_after_clear"}, {"vendor_code", "C2"}}}};
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = [&](const std::size_t) {
      real.world.manager.SetBroadcasterActive(false);
    };
  }
  const auto cleared = real.Dispatch("device.clear_error");
  {
    std::lock_guard lock(real.device->mutex);
    real.device->on_report_read = {};
  }
  EXPECT_EQ(cleared.outcome, Outcome::kFailedEffectObserved);
  EXPECT_EQ(Reason(cleared), "xarm_error_persists_after_clear");
}

// M6 XADAPT (D8): Enable's controller switch, its reply held, with 1 s
// left: the switch is allowed min(the 10 s call cap, the remaining time); no
// answer within it is the connection lost, a known result (SX-661):
// `controller_manager_unresponsive` as a known failure whose observed effect
// is the fault it raises.
TEST(RetainedRos2ControlRuntimeHandleTest, RealArmEnableWithAHungSwitchIsTheConnectionLost) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  real.world.manager.SetSwitchRepliesPaused(true);
  const auto started = std::chrono::steady_clock::now();
  const auto enabled = real.world.handle->lifecycle_primitive_dispatcher()(
      core::ControlDomainId("domain.arm"),
      core::LifecyclePrimitiveId("ros2_control.controller.activate"), std::stop_token{},
      core::Duration{1'000'000'000}, {});
  const auto waited = std::chrono::steady_clock::now() - started;
  real.world.manager.SetSwitchRepliesPaused(false);
  EXPECT_LT(waited, 3s);
  EXPECT_EQ(enabled.outcome, Outcome::kFailedEffectObserved) << Reason(enabled);
  ASSERT_TRUE(enabled.error);
  EXPECT_EQ(enabled.error->code, "controller_manager_unresponsive");
  EXPECT_EQ(enabled.error->details.at("reason"), "ros2_control_switch_timeout");
  const auto snapshot = real.world.Snapshot();
  EXPECT_EQ(snapshot.component_state, State::kFaulted);
  ASSERT_TRUE(snapshot.diagnostic);
  EXPECT_EQ(snapshot.diagnostic->code, "controller_manager_unresponsive");
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmEnableGateRefusesNaNStaleAndDisagreementBeforeAnySwitch) {
  RealArmWorld real(true);
  // An observed setup refusal (kNotSent, e.g. the endpoint graph still
  // settling) sends nothing; retry only that.
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  const auto activate = [&] {
    return real.world.handle->ActivateArmController(std::stop_token{});
  };
  const auto reason = [](const RetainedRos2ControlSwitchResult &result) {
    return result.error ? result.error->details.at("reason") : std::string();
  };

  // No measured sample yet.
  auto refused = activate();
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  EXPECT_EQ(reason(refused), "ros2_control_measured_joints_stale");
  // NaN (no valid hardware read) is never a zero.
  ASSERT_TRUE(fake::WaitUntil([&] {
    real.joints.Publish(std::numeric_limits<double>::quiet_NaN());
    return reason(activate()) == "ros2_control_measured_joints_invalid";
  }));
  EXPECT_EQ(ArmWorld::Fact(real.world.Snapshot(), "external.measured_joints_valid"),
            false);
  EXPECT_EQ(real.Decide(Intent::kEnable, State::kActive),
            "refused:no_transition_satisfied");
  // A stale sample.
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.joints.Pause();
  std::this_thread::sleep_for(1200ms);
  EXPECT_EQ(reason(activate()), "ros2_control_measured_joints_stale");
  EXPECT_TRUE(real.device->GateInputs().empty());

  // Fresh finite joints the vendor finds in disagreement with its angles.
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.device->SetGate(core::Result<void>::Err(core::ErrorDTO{
      "execution_rejected", "disagree", {{"reason", "xarm_measured_joints_disagree"}}}));
  EXPECT_EQ(reason(activate()), "xarm_measured_joints_disagree");
  ASSERT_FALSE(real.device->GateInputs().empty());
  EXPECT_EQ(real.device->GateInputs().back(), std::vector<double>{0.25});
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);

  // Ready and agreeing: the existing activation follows.
  real.device->SetGate(core::Result<void>::Ok());
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto activated = activate();
  ASSERT_EQ(activated.outcome, Outcome::kSucceeded) << SwitchFailureText(activated);
  EXPECT_EQ(real.world.manager.switch_requests(), 1U);
}

// Live 2026-10-27: the broadcaster (150 Hz) delivers a sample between the
// caller taking its boundary and reading health. That sample is newer than
// the boundary, not stale; `measured_joints_valid` must not flicker false,
// or Prepare's `controller.prepare` step is randomly ineligible.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmMeasuredJointsReceivedAfterTheBoundaryAreCurrent) {
  RealArmWorld real;
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto fact = [](const core::LifecycleDomainHealthDTO &health) {
    for (const auto &row : health.facts)
      if (row.lifecycle_fact_id.value() == "external.measured_joints_valid")
        return row.value.boolean_value;
    return std::optional<bool>{};
  };
  // A boundary taken before the latest sample arrived (budget 1 s).
  const auto boundary = real.world.clock.now() - 200'000'000;
  const auto earlier = real.world.handle->ReadLifecycleHealth(
      core::ControlDomainId("domain.arm"), boundary);
  ASSERT_TRUE(earlier.ok()) << earlier.error().message;
  EXPECT_EQ(fact(earlier.value()), true);
  // A sample older than the budget is still stale.
  real.joints.Pause();
  std::this_thread::sleep_for(1200ms);
  EXPECT_EQ(fact(real.Health()), false);
}

// TOOLCTX: at Enable the robot's tool context (default TCP) is applied after
// the gate and before the switch; any failure refuses the Enable with
// nothing switched.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmEnableAppliesTheToolContextBeforeTheSwitchAndRefusesOnFailure) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"execution_rejected", "no answer",
                     {{"reason", "tool_context_not_applied"},
                      {"vendor_reason", "xarm_service_timeout"}}}));
  const auto refused = real.world.handle->ActivateArmController(std::stop_token{});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "tool_context_not_applied");
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  ASSERT_EQ(real.device->AppliedContexts().size(), 1U);
  EXPECT_TRUE(real.device->AppliedContexts().front().second);  // at Enable
  EXPECT_EQ(real.ToolFrames(), (std::vector<std::string>{"default_tcp"}));

  real.device->SetToolContextResult({});
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto activated = real.world.handle->ActivateArmController(std::stop_token{});
  ASSERT_EQ(activated.outcome, Outcome::kSucceeded) << SwitchFailureText(activated);
  EXPECT_EQ(real.world.manager.switch_requests(), 1U);
  const auto applied = real.device->AppliedContexts();
  ASSERT_EQ(applied.size(), 2U);
  // Active TCP (M7): the Enable re-sync at the Active TCP (the End-effector
  // TCP after the start) and the total payload.
  EXPECT_EQ(LoadKg(applied.back().first), std::optional(0.5));
  EXPECT_EQ(OffsetZ(applied.back().first), std::optional(0.1));
}

// V-B2 §2.4: Enable's tool-context re-sync is the nested substep
// `device.tool_context.sync` of `ros2_control.controller.activate`: both its
// events reach the step's sink before the switch, the finished one with its
// mapped outcome and code; a failed re-sync ends the step there.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmEnableReportsTheToolContextSyncAsANestedSubstepBeforeTheSwitch) {
  using Phase = core::LifecycleSubstepEventDTO::Phase;
  using Transition = core::LifecycleTransitionOutcomeDTO;
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  std::vector<core::LifecycleSubstepEventDTO> events;
  std::vector<std::size_t> switches_at_event;
  const core::LifecycleSubstepFeedbackSink feedback =
      [&](const core::LifecycleSubstepEventDTO &event) {
        events.push_back(event);
        switches_at_event.push_back(real.world.manager.switch_requests());
      };
  const core::LifecyclePrimitiveId sync("device.tool_context.sync");
  const core::LifecyclePrimitiveId activate("ros2_control.controller.activate");

  real.device->SetToolContextResult(VendorFailure(core::ErrorDTO{
      "execution_rejected", "no answer", {{"reason", "tool_context_not_applied"}}}));
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto refused =
      real.world.handle->ActivateArmController(std::stop_token{}, std::nullopt, feedback);
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].substep_id, sync);
  EXPECT_EQ(events[0].within, activate);
  EXPECT_EQ(events[0].phase, Phase::kStarted);
  EXPECT_FALSE(events[0].outcome.has_value());
  EXPECT_EQ(events[1].substep_id, sync);
  EXPECT_EQ(events[1].phase, Phase::kFinished);
  EXPECT_EQ(events[1].outcome, Transition::kFailed);
  EXPECT_EQ(events[1].error_code, std::optional<std::string>("execution_rejected"));
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);

  events.clear();
  switches_at_event.clear();
  real.device->SetToolContextResult({});
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto activated =
      real.world.handle->ActivateArmController(std::stop_token{}, std::nullopt, feedback);
  ASSERT_EQ(activated.outcome, Outcome::kSucceeded) << SwitchFailureText(activated);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].phase, Phase::kStarted);
  EXPECT_EQ(events[1].phase, Phase::kFinished);
  EXPECT_EQ(events[1].outcome, Transition::kSucceeded);
  EXPECT_FALSE(events[1].error_code.has_value());
  EXPECT_EQ(switches_at_event, (std::vector<std::size_t>{0U, 0U}));
  EXPECT_EQ(real.world.manager.switch_requests(), 1U);
}

// M6b (SX-727): Enable's tool-context send (the zero offset with the sync
// off) is part of the lifecycle step: the adapter gets the step's deadline
// (a motion's send none). Its outcome follows what was sent: a sent offset
// or re-arm that may still act is uncertain; an applied offset followed by
// a switch refused or not sent is a known failure with an observed effect.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmEnableToolContextSendIsPartOfTheStepAndClassifiedByWhatWasSent) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  // Sent, then no confirmation (effect possible): uncertain, no switch.
  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"execution_rejected",
                     "the robot's set_state after its tcp_offset was not confirmed",
                     {{"reason", "xarm_reengage_not_confirmed"}, {"effect", "possible"}}},
      true));
  const auto uncertain = real.world.handle->ActivateArmController(std::stop_token{}, deadline);
  EXPECT_EQ(uncertain.outcome, Outcome::kUncertain);
  ASSERT_TRUE(uncertain.error.has_value());
  EXPECT_EQ(uncertain.error->details.at("reason"), "xarm_reengage_not_confirmed");
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  ASSERT_EQ(real.device->AppliedDeadlines().size(), 1U);
  EXPECT_TRUE(real.device->AppliedDeadlines().front() == deadline);
  // Not sent (no time left in the step): refused before effect.
  real.device->SetToolContextResult(VendorFailure(core::ErrorDTO{
      "component_lifecycle_deadline_expired", "the lifecycle deadline expired before dispatch",
      {{"reason", "deadline_exhausted"}}}));
  ASSERT_TRUE(real.PublishJoints(0.25));
  EXPECT_EQ(real.world.handle->ActivateArmController(std::stop_token{}, deadline).outcome,
            Outcome::kNotSent);
  EXPECT_EQ(real.device->AppliedDeadlines().size(), 2U);
  // The offset applied, then no time left for the switch: not sent itself,
  // but the step changed the controller.
  real.device->SetToolContextResult({});
  real.device->SetToolContextRearm(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto expired = real.world.handle->ActivateArmController(
      std::stop_token{}, std::chrono::steady_clock::now() - 1ms);
  EXPECT_EQ(expired.outcome, Outcome::kFailedEffectObserved);
  ASSERT_TRUE(expired.error.has_value());
  EXPECT_EQ(expired.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  // The offset applied, then the manager refuses the switch.
  real.world.manager.SetControllerActive(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto refused = real.world.handle->ActivateArmController(std::stop_token{}, deadline);
  EXPECT_EQ(refused.outcome, Outcome::kFailedEffectObserved) << SwitchFailureText(refused);
  EXPECT_EQ(real.world.manager.switch_requests(), 1U);
  // A motion's TCP change is not a lifecycle step: no deadline.
  real.device->SetToolContextRearm(false);
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link")).ok());
  EXPECT_FALSE(real.device->AppliedDeadlines().back().has_value());
}

// TOOLCTX, Active TCP (M7): before a motion whose TCP differs from the
// Active TCP the handle composes the context at the motion's TCP and hands
// it to the vendor adapter (not at Enable), then commits it; a source or
// vendor failure is returned typed with the Active TCP unchanged, and the
// source's failure reaches no vendor. A motion at the Active TCP sends and
// checks nothing (design 3.1).
TEST(RetainedRos2ControlRuntimeHandleTest,
     MotionToolContextUsesTheMotionsTcpAndReturnsFailuresTyped) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link")).ok());
  EXPECT_EQ(real.ToolFrames(), (std::vector<std::string>{"tool_tcp_link"}));
  auto applied = real.device->AppliedContexts();
  ASSERT_EQ(applied.size(), 1U);
  EXPECT_FALSE(applied.front().second);
  // A TCP change: the offset only, no payload.
  EXPECT_EQ(OffsetZ(applied.front().first), std::optional(0.2));
  EXPECT_TRUE(std::holds_alternative<VendorToolLoadKeep>(applied.front().first.load));
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("tool_tcp_link"));
  // The same TCP: no event, no step.
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link")).ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), 1U);
  EXPECT_EQ(real.ToolFrames().size(), 1U);

  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"execution_rejected", "no answer",
                     {{"reason", "tool_context_not_applied"}}}));
  const auto vendor = real.world.handle->ApplyMotionToolContext(TcpEntry("finger_link"));
  ASSERT_FALSE(vendor.ok());
  EXPECT_EQ(vendor.error().details.at("reason"), "tool_context_not_applied");
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("tool_tcp_link"));

  {
    std::lock_guard lock(real.tool_mutex);
    real.tool_source_result = core::Result<void>::Err(
        core::ErrorDTO{"execution_rejected", "not rigid",
                       {{"reason", "tool_context_frame_not_rigid"}}});
  }
  const auto source = real.world.handle->ApplyMotionToolContext(TcpEntry("finger_link"));
  ASSERT_FALSE(source.ok());
  EXPECT_EQ(source.error().details.at("reason"), "tool_context_frame_not_rigid");
  EXPECT_EQ(real.device->AppliedContexts().size(), 2U);
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("tool_tcp_link"));
  // A1-1: an entry outside the list is refused typed before anything.
  const auto unknown = real.world.handle->ApplyMotionToolContext(TcpEntry("elsewhere"));
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "active_tcp_unknown");
}

// T-A1, T-MQ16: no per-move comparison. A move at the Active TCP (a Current
// preempt resolves there) composes nothing and calls no vendor.
TEST(RetainedRos2ControlRuntimeHandleTest, AMoveAtTheActiveTcpNeverAsksTheVendor) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("default_tcp")).ok());
  EXPECT_TRUE(real.device->AppliedContexts().empty());
  EXPECT_TRUE(real.ToolFrames().empty());
}

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

// A2a-1, T-MQ9b, design 3.3: while not enabled Set TCP and a reset commit
// without a vendor call; Enable keeps the value and re-syncs the controller
// at it (offset and total payload). Enabled, Set TCP sends the offset only
// (offset sent recorded); a refusal leaves the value; a reset the controller
// refuses still completes.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmSetTcpCommitsWhileDisabledAndEnableReSyncsAtTheActiveTcp) {
  RealArmWorld real(true);
  std::vector<core::ActiveTcpChangeDTO> events;
  real.world.handle->BindActiveTcpEvents(
      [&](const core::ActiveTcpChangeDTO &change) { events.push_back(change); });
  ASSERT_TRUE(real.world.handle
                  ->SetActiveTcp(TcpEntry("tool_tcp_link"), core::ActiveTcpChangeSource::kProgram)
                  .ok());
  EXPECT_TRUE(real.device->AppliedContexts().empty());
  ASSERT_EQ(events.size(), 1U);
  EXPECT_FALSE(events[0].offset_sent);

  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(real.device->AppliedContexts().size(), 1U);
  const auto enable = real.device->AppliedContexts().front();
  EXPECT_TRUE(enable.second);
  EXPECT_EQ(OffsetZ(enable.first), std::optional(0.2));  // the Active TCP
  EXPECT_EQ(LoadKg(enable.first), std::optional(0.5));
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("tool_tcp_link"));

  real.device->SetToolContextRearm(true, [&] {
    real.world.manager.SetAllControllersActive(false);
    std::this_thread::sleep_for(50ms);
    real.world.manager.SetAllControllersActive(true);
  });
  real.joints.SetVelocity(0.0);
  real.joints.Publish(0.25);
  ASSERT_TRUE(real.world.handle
                  ->SetActiveTcp(TcpEntry("finger_link"), core::ActiveTcpChangeSource::kRobotControl)
                  .ok());
  ASSERT_EQ(real.device->AppliedContexts().size(), 2U);
  EXPECT_FALSE(real.device->AppliedContexts().back().second);
  EXPECT_TRUE(std::holds_alternative<VendorToolLoadKeep>(
      real.device->AppliedContexts().back().first.load));
  ASSERT_EQ(events.size(), 2U);
  EXPECT_TRUE(events[1].offset_sent);

  real.device->SetToolContextRearm(false);
  real.device->SetToolContextResult(VendorFailure(core::ErrorDTO{
      "execution_rejected", "the robot reports an error",
      {{"reason", "tool_context_not_applied"}}}));
  const auto refused = real.world.handle->SetActiveTcp(TcpEntry("default_tcp"),
                                                      core::ActiveTcpChangeSource::kProgram);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("finger_link"));
  EXPECT_EQ(events.size(), 2U);
  // T-MQ9b: the same refusal does not stop a reset.
  ASSERT_TRUE(real.world.handle->ResetActiveTcp().ok());
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("default_tcp"));
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[2].source, core::ActiveTcpChangeSource::kReset);
  EXPECT_FALSE(events[2].offset_sent);
}

// T-N1 (A2b rev 2): a TCP step sent and not confirmed (a post-send
// mismatch) faults the arm until the operator's Recover: an answered status
// query does not end it (unlike D8's lost reply), so motion stays closed; the
// Active TCP keeps its confirmed value, and the Enable after Recover sends it.
TEST(RetainedRos2ControlRuntimeHandleTest,
     AnUnconfirmedTcpSendFaultsTheArmUntilRecoverAndEnableReSyncs) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"execution_rejected", "TCP offset not taken by the controller",
                     {{"reason", "xarm_tcp_offset_not_applied"}}},
      true));
  ASSERT_FALSE(real.world.handle
                   ->SetActiveTcp(TcpEntry("tool_tcp_link"), core::ActiveTcpChangeSource::kProgram)
                   .ok());
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("default_tcp"));
  const auto faulted = [&] {
    const auto snapshot = real.world.Snapshot();
    return snapshot.component_state == State::kFaulted && snapshot.diagnostic &&
           snapshot.diagnostic->details.contains("reason") &&
           snapshot.diagnostic->details.at("reason") == "xarm_tcp_offset_not_applied";
  };
  EXPECT_TRUE(faulted());
  // An answered status query (a Real step's baseline) does not end it.
  real.device->SetToolContextResult({});
  static_cast<void>(real.Dispatch("ros2_control.broadcaster.activate"));
  EXPECT_TRUE(faulted());
  // The operator's Recover ends it; the Enable after it re-syncs the
  // confirmed Active TCP. The Real Recover plan switches the controller off
  // before its status-query step, so the following Enable activates it again.
  ASSERT_TRUE(Answered(real.Dispatch("ros2_control.controller.deactivate")));
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
  EXPECT_TRUE(Answered(real.Dispatch("ros2_control.controller.recover")));
  EXPECT_FALSE(faulted());
  const auto sent = real.device->AppliedContexts().size();
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(real.device->AppliedContexts().size(), sent + 1U);
  EXPECT_TRUE(real.device->AppliedContexts().back().second);
  EXPECT_EQ(OffsetZ(real.device->AppliedContexts().back().first), std::optional(0.1));
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

// T-C2e: with "Send TCP offset to controller" off a TCP change is the
// selection only (no vendor call) and Enable sends a zero offset.
TEST(RetainedRos2ControlRuntimeHandleTest, CheckboxOffChangesOnlyTheSelectionAndEnableSendsZero) {
  RealArmWorld real(true, false);
  EXPECT_FALSE(real.world.handle->ActiveTcpHoldsRobot());
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(real.device->AppliedContexts().size(), 1U);
  EXPECT_TRUE(std::holds_alternative<VendorToolOffsetZero>(
      real.device->AppliedContexts().front().first.offset));
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link")).ok());
  ASSERT_TRUE(real.world.handle
                  ->SetActiveTcp(TcpEntry("finger_link"), core::ActiveTcpChangeSource::kProgram)
                  .ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), 1U);
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("finger_link"));
}

TEST(RetainedRos2ControlRuntimeHandleTest, RetainedRebindKeepsEntryOrResetsWhenRemoved) {
  RealArmWorld real;
  ASSERT_TRUE(real.world.handle->SetActiveTcp(TcpEntry("tool_tcp_link"),
                                               core::ActiveTcpChangeSource::kRobotControl).ok());
  const auto before = real.world.handle->ReadActiveTcp();
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto pose = [](const std::string &) -> std::optional<core::Pose3DTO> { return core::Pose3DTO{}; };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp", "tool_tcp_link"}), std::nullopt,
      source, committed, pose, true).ok());
  const auto kept = real.world.handle->ReadActiveTcp();
  ASSERT_TRUE(kept.value);
  EXPECT_EQ(kept.value->entry, TcpEntry("tool_tcp_link"));
  EXPECT_EQ(kept.registration_generation, before.registration_generation);
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp"}), std::nullopt, source, committed, pose, true).ok());
  const auto reset = real.world.handle->ReadActiveTcp();
  ASSERT_TRUE(reset.value);
  EXPECT_EQ(reset.value->entry, TcpEntry("default_tcp"));
  EXPECT_GT(reset.registration_generation, kept.registration_generation);
}

TEST(RetainedRos2ControlRuntimeHandleTest, RetainedRebindWithoutTcpDoesNotRepeatReset) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  std::vector<core::ActiveTcpChangeDTO> events;
  real.world.handle->BindActiveTcpEvents([&](const auto &event) { events.push_back(event); });
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto pose = [](const std::string &) -> std::optional<core::Pose3DTO> { return core::Pose3DTO{}; };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      {}, std::nullopt, source, committed, pose, true).ok());
  const auto first = real.world.handle->ReadActiveTcp();
  ASSERT_FALSE(first.value);
  const auto sends = real.device->AppliedContexts().size();
  const auto event_count = events.size();
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      {}, std::nullopt, source, committed, pose, true).ok());
  const auto repeated = real.world.handle->ReadActiveTcp();
  EXPECT_FALSE(repeated.value);
  EXPECT_EQ(repeated.registration_generation, first.registration_generation);
  EXPECT_EQ(repeated.sequence, first.sequence);
  EXPECT_EQ(real.device->AppliedContexts().size(), sends);
  EXPECT_EQ(events.size(), event_count);
}

TEST(RetainedRos2ControlRuntimeHandleTest, KeptTcpChangedOffsetPreSendRefusalFaults) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  const auto before = real.world.handle->ReadActiveTcp();
  real.device->SetToolContextResult(VendorFailure(
      {"execution_rejected", "refused before send", {{"reason", "tool_context_not_applied"}}}));
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto pose = [](const std::string &) -> std::optional<core::Pose3DTO> {
    return core::Pose3DTO{{0.0, 0.0, 0.9}, {0.0, 0.0, 0.0, 1.0}};
  };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp", "tool_tcp_link"}), std::nullopt,
      source, committed, pose, true).ok());
  const auto kept = real.world.handle->ReadActiveTcp();
  ASSERT_TRUE(kept.value);
  EXPECT_EQ(kept.value->entry, before.value->entry);
  EXPECT_EQ(kept.registration_generation, before.registration_generation);
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, RetainedResetPreSendRefusalFaultsButKeepsNewTcp) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(real.world.handle->SetActiveTcp(TcpEntry("tool_tcp_link"),
                                               core::ActiveTcpChangeSource::kRobotControl).ok());
  real.device->SetToolContextResult(VendorFailure(
      {"execution_rejected", "refused before send", {{"reason", "tool_context_not_applied"}}}));
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto pose = [](const std::string &) -> std::optional<core::Pose3DTO> { return core::Pose3DTO{}; };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp"}), std::nullopt, source, committed, pose, true).ok());
  ASSERT_TRUE(real.world.handle->ReadActiveTcp().value);
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("default_tcp"));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, RetainedResetThrownSendFaultsAndKeepsNewTcp) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(real.world.handle->SetActiveTcp(TcpEntry("tool_tcp_link"),
                                               core::ActiveTcpChangeSource::kRobotControl).ok());
  real.device->SetToolContextRearm(true, [] { throw std::runtime_error("lost send outcome"); });
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 0.0, {}});
  };
  const auto pose = [](const std::string &) -> std::optional<core::Pose3DTO> { return core::Pose3DTO{}; };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp"}), std::nullopt, source, committed, pose, true).ok());
  ASSERT_TRUE(real.world.handle->ReadActiveTcp().value);
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("default_tcp"));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, CommittedPayloadPreSendRefusalIsPersistentFault) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  real.device->SetToolContextResult(VendorFailure(
      {"execution_rejected", "refused before send", {{"reason", "tool_context_not_applied"}}}));
  const core::SceneRevision revision{core::SceneId("sha256:" + std::string(64, 'a')),
      3U, core::ContentDigest("sha256:" + std::string(64, 'b'))};
  const auto scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{revision, core::CellId("cell"), 0U, 0U, 0U,
                                    core::CollisionPolicySnapshot{}, {}, {}});
  ASSERT_FALSE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

// The local pre-motion check reads the host's current Scene and serializes
// its payload with the same handle path used by the follower.
TEST(RetainedRos2ControlRuntimeHandleTest, LocalCommittedPayloadCheckSendsLatestAndFaultsOnUnconfirmedSend) {
  RealArmWorld real(true);
  const auto scene = [](std::uint64_t revision) {
    return std::make_shared<const core::LogicalSceneSnapshotDTO>(
        core::LogicalSceneSnapshotDTO{
            {core::SceneId("sha256:" + std::string(64, 'a')), revision,
             core::ContentDigest("sha256:" + std::string(64, 'b'))},
            core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
  };
  real.committed_scene = scene(1);
  ASSERT_TRUE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  EXPECT_TRUE(real.device->AppliedContexts().empty());  // not enabled
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(real.device->AppliedContexts().size(), 1U);
  ASSERT_TRUE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), 2U);
  {
    std::lock_guard lock(real.tool_mutex);
    real.committed_scene = scene(2);
  }
  ASSERT_TRUE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  ASSERT_EQ(real.device->AppliedContexts().size(), 3U);
  const auto sent = real.device->AppliedContexts().back();
  EXPECT_FALSE(sent.second);
  EXPECT_TRUE(std::holds_alternative<VendorToolOffsetKeep>(sent.first.offset));
  EXPECT_EQ(LoadKg(sent.first), std::optional(2.0));
  ASSERT_TRUE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), 3U);
  {
    std::lock_guard lock(real.tool_mutex);
    real.committed_scene = scene(3);
  }
  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"robot_controller_unresponsive", "no answer",
                     {{"reason", "tool_context_not_applied"}}},
      true, true));
  ASSERT_FALSE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, FollowerCoalescesToLatestCommittedPayload) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);

  std::promise<void> entered;
  std::promise<void> release;
  std::promise<void> drained;
  const auto released = release.get_future().share();
  auto entered_future = entered.get_future();
  auto drained_future = drained.get_future();
  std::atomic<unsigned> calls{0U};
  std::atomic<bool> all_ok{true};
  CommittedToolPayloadFollower follower([&] {
    const auto call = calls.fetch_add(1U);
    if (call == 0U) {
      entered.set_value();
      released.wait();
    }
    if (!real.world.handle->ApplyLatestCommittedToolPayload().ok()) all_ok = false;
    if (call == 1U) drained.set_value();
  });
  follower.Notify();
  const auto first_started = entered_future.wait_for(5s);
  if (first_started != std::future_status::ready) {
    release.set_value();
    FAIL() << "payload follower did not start";
  }
  for (std::uint64_t revision : {2U, 3U, 4U}) {
    {
      std::lock_guard lock(real.tool_mutex);
      real.committed_scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              {core::SceneId("sha256:" + std::string(64, 'a')), revision,
               core::ContentDigest("sha256:" + std::string(64, 'b'))},
              core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
    }
    follower.Notify();
  }
  release.set_value();
  ASSERT_EQ(drained_future.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(all_ok.load());
  const auto sends = real.device->AppliedContexts();
  ASSERT_EQ(sends.size(), 2U); // Enable and the newest committed payload.
  EXPECT_EQ(LoadKg(sends.back().first), std::optional(4.0));
  EXPECT_FALSE(sends.back().second);
}

TEST(RetainedRos2ControlRuntimeHandleTest, CurrentPayloadUsesValuesAcrossRestoredRevisions) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  const auto scene = [](std::uint64_t n) {
    return std::make_shared<const core::LogicalSceneSnapshotDTO>(
        core::LogicalSceneSnapshotDTO{
            {core::SceneId("sha256:" + std::string(64, 'a')), n,
             core::ContentDigest("sha256:" + std::string(64, 'b'))},
            core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
  };
  const auto newer = scene(5);
  const auto restored = scene(3);
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return newer; }).ok());
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(5.0));
  const auto sends = real.device->AppliedContexts().size();
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return restored; }).ok());
  ASSERT_EQ(real.device->AppliedContexts().size(), sends + 1U);
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(3.0));
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return restored; }).ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), sends + 1U);
  ASSERT_FALSE(real.world.handle->ApplyCommittedToolPayload([] {
    return std::shared_ptr<const core::LogicalSceneSnapshotDTO>{};
  }).ok());
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, FollowerAndMotionSerializeSceneReadsAndSends) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  const auto make_scene = [](std::uint64_t n) {
    return std::make_shared<const core::LogicalSceneSnapshotDTO>(
        core::LogicalSceneSnapshotDTO{
            {core::SceneId("sha256:" + std::string(64, 'a')), n,
             core::ContentDigest("sha256:" + std::string(64, 'b'))},
            core::CellId("cell"), 0U, 0U, 0U, core::CollisionPolicySnapshot{}, {}, {}});
  };
  const auto old_scene = make_scene(3);
  {
    std::lock_guard lock(real.tool_mutex);
    real.committed_scene = make_scene(5);
  }
  std::promise<void> entered;
  std::promise<void> release;
  std::promise<void> motion_read_entered;
  std::promise<void> motion_read_release;
  auto motion_read_entered_future = motion_read_entered.get_future();
  const auto motion_read_released = motion_read_release.get_future().share();
  {
    std::lock_guard lock(real.tool_mutex);
    real.scene_read_hook = [&] {
      motion_read_entered.set_value();
      motion_read_released.wait();
    };
  }
  const auto released = release.get_future().share();
  auto entered_future = entered.get_future();
  auto follower = std::async(std::launch::async, [&] {
    return real.world.handle->ApplyCommittedToolPayload([&] {
      entered.set_value();
      released.wait();
      return old_scene;
    });
  });
  if (entered_future.wait_for(5s) != std::future_status::ready) {
    release.set_value();
    FAIL() << "payload read did not start";
  }
  // The first read is inside the switch lock; the motion thread starts
  // before releasing it and must use the same serialized path.
  EXPECT_FALSE(RetainedRos2ControlRuntimeHandleTestAccess::TryLockSwitch(*real.world.handle));
  std::promise<void> motion_started;
  auto motion_started_future = motion_started.get_future();
  auto motion = std::async(std::launch::async, [&] {
    motion_started.set_value();
    return real.world.handle->ApplyLatestCommittedToolPayload();
  });
  if (motion_started_future.wait_for(5s) != std::future_status::ready) {
    release.set_value();
    motion_read_release.set_value();
    FAIL() << "motion payload check did not start";
  }
  release.set_value();
  if (motion_read_entered_future.wait_for(5s) != std::future_status::ready) {
    motion_read_release.set_value();
    FAIL() << "motion Scene read did not start";
  }
  EXPECT_FALSE(RetainedRos2ControlRuntimeHandleTestAccess::TryLockSwitch(*real.world.handle));
  motion_read_release.set_value();
  ASSERT_TRUE(follower.get().ok());
  ASSERT_TRUE(motion.get().ok());
  const auto sends = real.device->AppliedContexts();
  ASSERT_GE(sends.size(), 3U);
  EXPECT_EQ(LoadKg(sends[sends.size() - 2U].first), std::optional(3.0));
  EXPECT_EQ(LoadKg(sends.back().first), std::optional(5.0));
}

TEST(RetainedRos2ControlRuntimeHandleTest, UnseededCommittedSceneRefusesThenAllowsEnableAndMotionCheck) {
  RealArmWorld real(true);
  const auto seeded = real.committed_scene;
  {
    std::lock_guard lock(real.tool_mutex);
    real.committed_scene.reset();
  }
  const auto unavailable = real.world.handle->ApplyLatestCommittedToolPayload();
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "execution_port_not_ready");
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto refused = real.world.handle->ActivateArmController(std::stop_token{});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "execution_port_not_ready");
  EXPECT_NE(real.world.Snapshot().component_state, State::kFaulted);
  EXPECT_TRUE(real.device->AppliedContexts().empty());
  {
    std::lock_guard lock(real.tool_mutex);
    real.committed_scene = seeded;
  }
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_EQ(real.device->AppliedContexts().size(), 1U);
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(0.5));
  ASSERT_TRUE(real.world.handle->ApplyLatestCommittedToolPayload().ok());
  ASSERT_EQ(real.device->AppliedContexts().size(), 2U);
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(1.0));
}

TEST(RetainedRos2ControlRuntimeHandleTest, RetainedRebindRecomposesSameCommittedSceneRevision) {
  RealArmWorld real(true, false);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome, Outcome::kSucceeded);
  const core::SceneRevision revision{core::SceneId("sha256:" + std::string(64, 'a')),
      3U, core::ContentDigest("sha256:" + std::string(64, 'b'))};
  const auto scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{revision, core::CellId("cell"), 0U, 0U, 0U,
                                    core::CollisionPolicySnapshot{}, {}, {}});
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(3.0));
  const auto sends_before_rebind = real.device->AppliedContexts().size();
  const auto source = [](const std::optional<std::string> &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 4.0, {}});
  };
  const auto committed = [](const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &) {
    return core::Result<core::ToolContextDTO>::Ok({{}, 4.0, {}});
  };
  ASSERT_TRUE(real.world.handle->RebindRetainedToolContext(
      TcpRows({"default_tcp", "tool_tcp_link", "finger_link"}), std::nullopt,
      source, committed, {}, false).ok());
  EXPECT_EQ(real.device->AppliedContexts().size(), sends_before_rebind);
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  ASSERT_EQ(real.device->AppliedContexts().size(), sends_before_rebind + 1U);
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(4.0));
}

TEST(RetainedRos2ControlRuntimeHandleTest, CommittedPayloadCompositionFailureIsPersistentFault) {
  RealArmWorld real(true);
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  const core::SceneRevision revision{core::SceneId("sha256:" + std::string(64, 'a')),
      2U, core::ContentDigest("sha256:" + std::string(64, 'b'))};
  const auto scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{revision, core::CellId("cell"), 0U, 0U, 0U,
                                    core::CollisionPolicySnapshot{}, {}, {}});
  {
    std::lock_guard lock(real.tool_mutex);
    real.tool_source_result = core::Result<void>::Err(
        core::ErrorDTO{"execution_rejected", "frame unavailable", {{"reason", "tool_context_frame_unknown"}}});
  }
  const auto result = real.world.handle->ApplyCommittedToolPayload([&] { return scene; });
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "tool_context_frame_unknown");
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
}

TEST(RetainedRos2ControlRuntimeHandleTest, DisabledNotificationDoesNotMarkSceneApplied) {
  RealArmWorld real(true);
  const core::SceneRevision revision{core::SceneId("sha256:" + std::string(64, 'a')),
      3U, core::ContentDigest("sha256:" + std::string(64, 'b'))};
  const auto scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{revision, core::CellId("cell"), 0U, 0U, 0U,
                                    core::CollisionPolicySnapshot{}, {}, {}});
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  EXPECT_TRUE(real.device->AppliedContexts().empty());
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(3.0));
}

TEST(RetainedRos2ControlRuntimeHandleTest, EnableResendsThenPayloadCheckUsesLastSentValue) {
  RealArmWorld real(true);
  const core::SceneRevision revision{core::SceneId("sha256:" + std::string(64, 'a')),
      3U, core::ContentDigest("sha256:" + std::string(64, 'b'))};
  const auto scene = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{revision, core::CellId("cell"), 0U, 0U, 0U,
                                    core::CollisionPolicySnapshot{}, {}, {}});
  ASSERT_TRUE(fake::WaitUntil([&] {
    return real.world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  ASSERT_EQ(real.world.handle->DeactivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(real.world.WaitForState(State::kDisabled));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.world.handle->ApplyCommittedToolPayload([&] { return scene; }).ok());
  EXPECT_EQ(LoadKg(real.device->AppliedContexts().back().first), std::optional(3.0));
}


// M6 ADAPTERM: an admitted motion after an idle move from the robot's own
// interface. The vendor plugin had switched every controller off; the
// adapter re-engages the robot, the plugin reactivates them, and the handle
// binds the reactivated controller on a new epoch, with no CCLC switch.
TEST(RetainedRos2ControlRuntimeHandleTest,
     IdleExternalMoveIsReengagedBeforeTheMotionWithoutACclcSwitch) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  const auto switches = real.world.manager.switch_requests();
  const auto epoch = real.world.handle->current_port()->ReadReadiness().binding_epoch;
  const auto rearm_ends = real.device->end_rearm_calls.load();  // the Enable's

  // Nothing latched: nothing done, the same epoch.
  ASSERT_TRUE(real.world.handle->ReengageMotionAtRest().ok());
  EXPECT_EQ(real.device->reengage_calls, 1);
  EXPECT_EQ(real.world.handle->current_port()->ReadReadiness().binding_epoch, epoch);

  // UFACTORY Studio moved it: the plugin switched every controller off.
  real.world.manager.SetAllControllersActive(false);
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  EXPECT_FALSE(real.world.handle->current_port()->ReadReadiness().controller_bound);
  real.device->SetReengage(VendorCallResult{true}, [&] {
    real.world.manager.SetAllControllersActive(true);
  });
  real.joints.SetVelocity(0.0);
  real.joints.Publish(0.4);  // the pose after the manual move (10 ms timer)
  const auto reengaged = real.world.handle->ReengageMotionAtRest();
  ASSERT_TRUE(reengaged.ok()) << reengaged.error().message;
  const auto readiness = real.world.handle->current_port()->ReadReadiness();
  EXPECT_TRUE(readiness.controller_active);
  EXPECT_TRUE(readiness.controller_bound);
  EXPECT_GT(readiness.binding_epoch.value(), epoch.value());
  // No CCLC (or handle) controller switch: the plugin reactivated it.
  EXPECT_EQ(real.world.manager.switch_requests(), switches);
  // The gate saw the current (moved) pose, the hold the controller took.
  ASSERT_FALSE(real.device->GateInputs().empty());
  EXPECT_EQ(real.device->GateInputs().back(), std::vector<double>{0.4});
  // The own re-arm ends with the rebind.
  EXPECT_EQ(real.device->end_rearm_calls.load(), rearm_ends + 1);
}

// M6 ADAPTERM: a re-engage the robot does not confirm, or a reactivation that
// does not come, refuses the motion typed before it moves; nothing is bound.
TEST(RetainedRos2ControlRuntimeHandleTest, ReengageFailureRefusesTheMotionTyped) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  real.world.manager.SetAllControllersActive(false);
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  const auto switches = real.world.manager.switch_requests();

  real.device->SetReengage(VendorFailure(
      core::ErrorDTO{"execution_rejected", "the robot's set_mode was not confirmed",
                     {{"reason", "xarm_reengage_not_confirmed"}, {"step", "set_mode"}}},
      true));
  const auto refused = real.world.handle->ReengageMotionAtRest();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "execution_rejected");
  EXPECT_EQ(refused.error().details.at("reason"), "motion_reengage_failed");
  EXPECT_EQ(refused.error().details.at("cause"), "xarm_reengage_not_confirmed");
  EXPECT_EQ(refused.error().details.at("step"), "set_mode");
  EXPECT_FALSE(real.world.handle->current_port()->ReadReadiness().controller_bound);

  // Confirmed, but the plugin never reactivates its controllers: bounded.
  real.device->SetReengage(VendorCallResult{true});
  const auto ends = real.device->end_rearm_calls.load();
  const auto inactive = real.world.handle->ReengageMotionAtRest();
  // SX-584: the own re-arm ends with the failed attempt.
  EXPECT_EQ(real.device->end_rearm_calls.load(), ends + 1);
  ASSERT_FALSE(inactive.ok());
  EXPECT_EQ(inactive.error().details.at("cause"),
            "ros2_control_reengage_controllers_inactive");
  EXPECT_FALSE(real.world.handle->current_port()->ReadReadiness().controller_bound);
  EXPECT_EQ(real.world.manager.switch_requests(), switches);
}

// M6 XADAPT: the motion path's vendor calls follow the lifecycle rule. No
// answer in time (a TCP payload or offset send, the re-engage's
// `set_mode`/`set_state` or its ready wait) refuses the motion with
// `robot_controller_unresponsive`, unwrapped, and the arm reports the same
// component fault as a lifecycle step (Faulted, `fault` cause, that code as
// the diagnostic: an incident). The operator's Recover re-query ends it.
// Other failures keep their typed refusals (see the tests above).
TEST(RetainedRos2ControlRuntimeHandleTest,
     MotionPathVendorCallWithoutAnswerRefusesTheMotionAndFaultsTheArm) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  const auto faulted = [&] {
    const auto snapshot = real.world.Snapshot();
    return snapshot.component_state == State::kFaulted && ArmWorld::Cause(snapshot) == "fault" &&
           snapshot.diagnostic && snapshot.diagnostic->code == "robot_controller_unresponsive";
  };
  const core::ErrorDTO unanswered{"robot_controller_unresponsive",
                                  "the robot did not confirm its tcp_load (xarm_service_timeout)",
                                  {{"reason", "tool_context_not_applied"},
                                   {"vendor_reason", "xarm_service_timeout"},
                                   {"tool_context_field", "tcp_load"}}};
  ASSERT_FALSE(faulted());

  // The TOOLCTX send before a motion.
  real.device->SetToolContextResult(VendorFailure(unanswered, false, true));
  const auto sent = real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link"));
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, "robot_controller_unresponsive");
  EXPECT_TRUE(faulted());
  // The operator's Recover (the status re-query step) ends it.
  real.device->SetToolContextResult({});
  EXPECT_TRUE(Answered(real.Dispatch("ros2_control.controller.recover")));
  EXPECT_FALSE(faulted());

  // The ADAPTERM re-engage: refused with the code itself, not wrapped as
  // `motion_reengage_failed`.
  real.device->SetReengage(VendorFailure(
      core::ErrorDTO{"robot_controller_unresponsive",
                     "the robot controller sent no status report in time",
                     {{"reason", "xarm_reengage_not_ready"}, {"effect", "possible"}}},
      true, true));
  const auto reengaged = real.world.handle->ReengageMotionAtRest();
  ASSERT_FALSE(reengaged.ok());
  EXPECT_EQ(reengaged.error().code, "robot_controller_unresponsive");
  EXPECT_EQ(reengaged.error().details.at("reason"), "xarm_reengage_not_ready");
  EXPECT_TRUE(faulted());
  // Nothing is armed or prepared over the fault.
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared), "refused:no_transition_satisfied");
}

// M6 ADAPTERM (SX-582): a broadcaster message stamped before the re-arm
// started (queued, delivered late, valid and at rest) never ends it: only a
// sample stamped after it lets the handle rebind and end the re-arm (and
// with it REALJOINTS' vendor bridge).
TEST(RetainedRos2ControlRuntimeHandleTest, AQueuedPreRearmSampleNeverEndsTheRearm) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  real.world.manager.SetAllControllersActive(false);
  ASSERT_TRUE(real.WaitForActivity(true, false, false));
  real.joints.Pause();
  real.joints.SetVelocity(0.0);
  const auto queued_stamp = real.joints.Now();  // before the re-arm starts
  const auto ends = real.device->end_rearm_calls.load();
  std::atomic<bool> started{false};
  real.device->SetReengage(VendorCallResult{true}, [&] {
    started = true;
    real.world.manager.SetAllControllersActive(true);
  });
  std::atomic<bool> fresh_sent{false};
  std::thread late([&] {
    while (!started.load()) std::this_thread::sleep_for(5ms);
    // Past the receipt cutoff (one 1 s budget): only its stamp is old.
    std::this_thread::sleep_for(1200ms);
    real.joints.PublishOnce(0.4, queued_stamp);
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(real.device->end_rearm_calls.load(), ends);
    EXPECT_FALSE(real.world.handle->current_port()->ReadReadiness().controller_bound);
    fresh_sent = true;
    real.joints.Publish(0.4);  // stamped now
  });
  const auto reengaged = real.world.handle->ReengageMotionAtRest();
  late.join();
  ASSERT_TRUE(reengaged.ok()) << reengaged.error().message;
  EXPECT_TRUE(fresh_sent.load());
  EXPECT_EQ(real.device->end_rearm_calls.load(), ends + 1);
}

// M6 ADAPTERM (SX-567.1): after the vendor calls the rebind also needs the
// arm at rest: measured velocities within the existing settle threshold and
// the vendor's own report not moving.
TEST(RetainedRos2ControlRuntimeHandleTest, ReengageRebindsOnlyWithTheArmAtRest) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  const auto epoch = real.world.handle->current_port()->ReadReadiness().binding_epoch;
  real.device->SetReengage(VendorCallResult{true});

  // Moving by the measured velocities (above 0.01 rad/s): no rebind.
  real.joints.SetVelocity(0.2);
  real.joints.Publish(0.4);
  auto refused = real.world.handle->ReengageMotionAtRest();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("cause"), "ros2_control_reengage_not_at_rest");
  EXPECT_EQ(real.world.handle->current_port()->ReadReadiness().binding_epoch, epoch);

  // Still, but the vendor reports the moving state: no rebind.
  real.joints.SetVelocity(0.0);
  real.device->SetGate(core::Result<void>::Err(core::ErrorDTO{
      "execution_rejected", "moving", {{"reason", "xarm_in_motion"}}}));
  refused = real.world.handle->ReengageMotionAtRest();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("cause"), "xarm_in_motion");
  EXPECT_EQ(real.world.handle->current_port()->ReadReadiness().binding_epoch, epoch);

  // At rest: bound on a new epoch.
  real.device->SetGate(core::Result<void>::Ok());
  ASSERT_TRUE(real.world.handle->ReengageMotionAtRest().ok());
  EXPECT_GT(real.world.handle->current_port()->ReadReadiness().binding_epoch.value(),
            epoch.value());
}

// M6 ADAPTERM (SX-567.2, SX-569.2): a sent but unconfirmed set_mode/set_state
// (a delayed success, a non-zero reply, a timeout) may still take effect. The
// next motion goes through the fresh-measured wait and binds a new epoch even
// when the latch has meanwhile ended (the adapter has nothing left to do),
// whether or not the controllers' inactive transition was observed.
TEST(RetainedRos2ControlRuntimeHandleTest, AnUnconfirmedReengageForcesTheNextMotionToRebind) {
  for (const std::string kind : {"delayed_success", "nonzero", "timeout"}) {
    for (const bool inactive_seen : {true, false}) {
      SCOPED_TRACE(kind + (inactive_seen ? " seen" : " missed"));
      RealArmWorld real(true);
      ASSERT_TRUE(real.PublishJoints(0.25));
      ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
      ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
                Outcome::kSucceeded);
      ASSERT_TRUE(real.WaitForActivity(true, true, true));
      const auto epoch = real.world.handle->current_port()->ReadReadiness().binding_epoch;
      core::ErrorDTO unconfirmed{
          "execution_rejected", "the robot's set_state was not confirmed",
          {{"reason", "xarm_reengage_not_confirmed"},
           {"step", "set_state"},
           {"effect", "possible"},
           {"vendor_reason", kind == "nonzero" ? "xarm_service_returned_nonzero"
                                               : "xarm_service_timeout"}}};
      if (kind == "nonzero") unconfirmed.details.emplace("vendor_return", "1");
      real.device->SetReengage(VendorFailure(unconfirmed, true));
      const auto first = real.world.handle->ReengageMotionAtRest();
      ASSERT_FALSE(first.ok());
      EXPECT_EQ(first.error().details.at("reason"), "motion_reengage_failed");

      // The late effect: the robot is armed after all and the plugin cycles
      // its controllers (seen, or too fast to be observed).
      if (inactive_seen) {
        real.world.manager.SetAllControllersActive(false);
        ASSERT_TRUE(real.WaitForActivity(true, false, false));
      }
      real.world.manager.SetAllControllersActive(true);
      ASSERT_TRUE(real.WaitForActivity(true, true, true));
      // A later Observe ended the latch: the adapter has nothing to do.
      real.device->SetReengage(VendorCallResult{});
      real.joints.SetVelocity(0.0);
      real.joints.Publish(0.3);
      const auto next = real.world.handle->ReengageMotionAtRest();
      ASSERT_TRUE(next.ok()) << next.error().message;
      const auto readiness = real.world.handle->current_port()->ReadReadiness();
      EXPECT_TRUE(readiness.controller_bound);
      EXPECT_GT(readiness.binding_epoch.value(), epoch.value());
      ASSERT_FALSE(real.device->GateInputs().empty());
      EXPECT_EQ(real.device->GateInputs().back(), std::vector<double>{0.3});
      // Rebound once: the motion after it needs nothing more.
      const auto rebound = readiness.binding_epoch;
      ASSERT_TRUE(real.world.handle->ReengageMotionAtRest().ok());
      EXPECT_EQ(real.world.handle->current_port()->ReadReadiness().binding_epoch, rebound);
    }
  }
}

// Items 23/32 handoff (SX-572, SX-574): a test xArm adapter over the real
// port (scripted status reports) and the production re-engage sequence
// (scripted vendor calls that answer like the robot).
class ScriptedXArmAdapter final : public IRetainedDeviceStatusAdapter {
public:
  struct Transport final : xarm_control::IXArmBrandLifecycleTransport {
    [[nodiscard]] xarm_control::XArmObservedState observe() const override {
      std::lock_guard lock(mutex);
      return report;
    }
    mutable std::mutex mutex;
    xarm_control::XArmObservedState report;
  };

  explicit ScriptedXArmAdapter(std::string vendor_topic)
      : topic_(std::move(vendor_topic)) {}

  /// A fresh status report (5 Hz in production; the test refreshes it).
  void Report(const int state, const int mode, const core::TimePoint at) {
    std::lock_guard lock(transport_->mutex);
    transport_->report = {at, state, mode, 0, 0, 0x1, {0.4}};
  }
  void Refresh(const core::TimePoint at) {
    std::lock_guard lock(transport_->mutex);
    transport_->report.received_timestamp_ns = at;
  }
  xarm_control::XArmBrandLifecyclePort &Port() { return port_; }

  void Start(rclcpp::Node &, core::IClock &clock) override { clock_ = &clock; }
  [[nodiscard]] RetainedDeviceStatus Observe(core::TimePoint now) override {
    auto status = port_.Observe(now);
    return {status.cause, std::move(status.facts)};
  }
  [[nodiscard]] RetainedDeviceStatus ReadHealth(core::TimePoint now) const override {
    auto status = port_.ReadHealth(now);
    return {status.cause, std::move(status.facts)};
  }
  void BeginHardwareTransition(bool) override {}
  void SettleHardwareTransition(bool, bool, core::TimePoint) override {}
  [[nodiscard]] core::Result<void>
  CheckActivation(core::TimePoint, const std::vector<double> &) const override {
    return core::Result<void>::Ok();
  }
  [[nodiscard]] RetainedRos2ControlSwitchResult
  Dispatch(const core::LifecyclePrimitiveId &,
           std::optional<std::chrono::steady_clock::time_point>, std::stop_token) override {
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::nullopt};
  }
  [[nodiscard]] std::optional<std::string> VendorJointStateTopic() const override {
    return topic_;
  }
  [[nodiscard]] VendorJointSampleGate VendorJointSampleGateFn() const override {
    return [this](const core::TimePoint now, const std::vector<double> &positions) {
      return port_.VendorJointSampleUsable(now, positions) ? VendorJointSampleUse::kLeads
                                                           : VendorJointSampleUse::kDisplay;
    };
  }
  [[nodiscard]] std::chrono::nanoseconds ActivationTransientBound() const override {
    return 5s;
  }
  void EndRearm() override {
    ++end_rearm_calls;
    port_.EndRearm();
  }
  [[nodiscard]] VendorCallResult ReengageAtRest(std::stop_token cancellation) override {
    reengage_started = true;
    const xarm_control::XArmReengageCallResult ok{true, true, "", 0};
    const auto result = xarm_control::ReengageAtRest(
        port_,
        {[ok](int) { return ok; },
         [this, ok](int) {
           // The robot answers: joint-servo mode, ready.
           Report(2, 1, clock_->now() + 1);
           return ok;
         },
         [this] { return clock_->now(); },
         [] { std::this_thread::sleep_for(20ms); }},
        cancellation);
    if (result.error)
      return {false, result.error, result.effect_possible,
              result.error->code == xarm_control::kRobotControllerUnresponsiveCode};
    return {result.reengaged};
  }

  std::atomic<bool> reengage_started{false};
  std::atomic<int> end_rearm_calls{0};

private:
  std::string topic_;
  std::shared_ptr<Transport> transport_ = std::make_shared<Transport>();
  xarm_control::XArmBrandLifecyclePort port_{
      transport_, {xarm_control::kXArmStatusFreshnessNs, 1,
                   xarm_control::kXArmJointAgreementToleranceRad,
                   xarm_control::XArmExternalMoveRearm::kAutomatic}};
  core::IClock *clock_{};
};

/// Joint states with a current stamp (the stream's message stamp) on a topic,
/// published every 10 ms while enabled.
class StampedJointStates final {
public:
  StampedJointStates(rclcpp::Node::SharedPtr node, const std::string &topic)
      : node_(std::move(node)),
        publisher_(node_->create_publisher<sensor_msgs::msg::JointState>(
            topic, rclcpp::SensorDataQoS())),
        thread_([this] {
          while (!stopped_.load()) {
            if (enabled_.load()) {
              sensor_msgs::msg::JointState message;
              message.header.stamp = node_->now();
              message.name = {"source_joint_1"};
              message.position = {0.4};
              message.velocity = {0.0};
              publisher_->publish(message);
            }
            std::this_thread::sleep_for(10ms);
          }
        }) {}
  ~StampedJointStates() {
    stopped_ = true;
    thread_.join();
  }
  void Enable(const bool enabled) { enabled_ = enabled; }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr publisher_;
  std::atomic<bool> enabled_{false};
  std::atomic<bool> stopped_{false};
  std::thread thread_;
};

[[nodiscard]] std::shared_ptr<RetainedObservedStateChannel>
ArmDisplayChannel(const core::RuntimeParticipantBinding &binding, core::IClock &clock);

// Items 23/32 (SX-572, SX-574): an idle external-move latch with the
// broadcaster off and only the vendor's joint states arriving; then the
// admitted motion's re-engage, with the broadcaster deliberately late (longer
// than the measured stream's budget). The vendor samples keep this exact
// `measured_joints` stream live from the move through the end of the
// re-engage: no motion hold (the Kernel admits the motion) and no fault loss
// (no new ProviderLost incident), before, during and after the handoff.
TEST(RetainedRos2ControlRuntimeHandleTest,
     VendorSamplesKeepTheMeasuredStreamLiveAcrossTheReengageHandoff) {
  constexpr core::Duration kBudget = 300'000'000;  // the Real 250 ms, rounded up
  auto clock = std::make_shared<WallClock>();
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock, false, {},
                 {}, 3U, kBudget);
  auto device = std::make_shared<ScriptedXArmAdapter>(world.prefix + "/xarm/joint_states");
  world.handle->InstallDeviceStatusAdapter(device);
  // The arm display channel as the backend factory builds it: commanded
  // (the binding's authority) over the retained model. A predicted source,
  // or no model projector for a model-backed binding, is refused at
  // construction.
  const auto display = ArmDisplayChannel(world.binding, world.clock);
  ASSERT_TRUE(world.handle->BindMotionDisplayChannel(display).ok());
  const auto staged = world.handle->StageMeasuredStateSources(
      {MeasuredSourceDescriptor(world.binding, world.prefix, kBudget)}, clock, world.binding,
      std::nullopt);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  ASSERT_TRUE(staged.value());
  staged.value()->EnablePublication();
  world.manager.SetHardwareActive(true);
  world.manager.SetBroadcasterActive(true);
  world.handle->StartBackend();
  const auto component = world.binding.value().controllable_component_id;
  const auto health = host_component_stream_health();
  const auto live = [&] {
    const auto verdict = health->Verdict(component);
    return !verdict.motion_hold.has_value() && !verdict.fault_loss.has_value();
  };

  // Enabled and bound, the broadcaster live.
  StampedJointStates broadcaster(world.handle->node(), world.prefix + "/joint_states");
  StampedJointStates vendor(world.handle->node(), world.prefix + "/xarm/joint_states");
  device->Report(2, 1, clock->now());
  broadcaster.Enable(true);
  vendor.Enable(true);
  std::atomic<bool> reporting{true};
  std::thread reports([&] {
    while (reporting.load()) {
      device->Refresh(clock->now());
      std::this_thread::sleep_for(100ms);
    }
  });
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.handle->PrepareArmController().outcome == Outcome::kSucceeded;
  }));
  ASSERT_EQ(world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  const auto rearm_ends = device->end_rearm_calls.load();  // the Enable's
  static_cast<void>(device->Observe(clock->now()));
  ASSERT_TRUE(fake::WaitUntil(live));

  // UFACTORY Studio: position mode, stopped; the plugin switches every
  // controller off and the broadcaster stops. Only vendor samples arrive.
  device->Report(5, 0, clock->now());
  static_cast<void>(device->Observe(clock->now()));
  ASSERT_EQ(device->Port().MotionReengageDecision(clock->now()).kind,
            xarm_control::XArmMotionReengage::kNeeded);
  broadcaster.Enable(false);
  world.manager.SetAllControllersActive(false);
  for (int sample = 0; sample < 40; ++sample) {  // 4 budgets of vendor-only
    std::this_thread::sleep_for(30ms);
    ASSERT_TRUE(live()) << "hold or loss while latched, sample " << sample;
  }

  // The admitted motion's re-engage; the plugin brings the broadcaster back
  // only 1 s (more than three budgets) after the robot is ready.
  std::thread plugin([&] {
    while (!device->reengage_started.load()) std::this_thread::sleep_for(5ms);
    for (int sample = 0; sample < 33; ++sample) {
      std::this_thread::sleep_for(30ms);
      EXPECT_TRUE(live()) << "hold or loss in the handoff, sample " << sample;
    }
    world.manager.SetAllControllersActive(true);
    broadcaster.Enable(true);
  });
  const auto reengaged = world.handle->ReengageMotionAtRest();
  plugin.join();
  ASSERT_TRUE(reengaged.ok()) << reengaged.error().message;
  EXPECT_TRUE(world.handle->current_port()->ReadReadiness().controller_bound);
  EXPECT_TRUE(live());
  // The rebind ended the own re-arm and the bridge; the latch is over.
  EXPECT_EQ(device->end_rearm_calls.load(), rearm_ends + 1);
  EXPECT_FALSE(device->Port().RearmInProgress());
  EXPECT_FALSE(device->Port().VendorJointSampleUsable(clock->now(), {0.4}));
  EXPECT_EQ(device->Port().MotionReengageDecision(clock->now()).kind,
            xarm_control::XArmMotionReengage::kNotNeeded);
  vendor.Enable(false);
  broadcaster.Enable(false);
  reporting = false;
  reports.join();
}

// M6 ADAPTERM TOOLCTX (live 2026-10-28): a TCP offset send leaves the
// controller's ready state; the adapter re-armed it and the plugin cycled
// its controllers. Before the motion goal the handle waits for the
// reactivated controllers and binds a new epoch, with no CCLC switch; an
// unchanged context (nothing sent) changes nothing.
TEST(RetainedRos2ControlRuntimeHandleTest, ATcpOffsetSendRebindsBeforeTheMotion) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  // Enable sends nothing (item 44; never re-arms).
  ASSERT_FALSE(real.device->AppliedContexts().empty());
  EXPECT_TRUE(real.device->AppliedContexts().back().second);
  const auto switches = real.world.manager.switch_requests();
  const auto epoch = real.world.handle->current_port()->ReadReadiness().binding_epoch;

  // Nothing sent (no re-arm): no rebind.
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("tool_tcp_link")).ok());
  EXPECT_EQ(real.world.handle->current_port()->ReadReadiness().binding_epoch, epoch);

  // The offset was sent: the plugin switched every controller off and on.
  real.device->SetToolContextRearm(true, [&] {
    real.world.manager.SetAllControllersActive(false);
    std::this_thread::sleep_for(50ms);
    real.world.manager.SetAllControllersActive(true);
  });
  real.joints.SetVelocity(0.0);
  real.joints.Publish(0.25);
  const auto applied = real.world.handle->ApplyMotionToolContext(TcpEntry("finger_link"));
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  const auto readiness = real.world.handle->current_port()->ReadReadiness();
  EXPECT_TRUE(readiness.controller_bound);
  EXPECT_GT(readiness.binding_epoch.value(), epoch.value());
  EXPECT_EQ(real.world.manager.switch_requests(), switches);
  EXPECT_GE(real.device->end_rearm_calls.load(), 1);

  // An unconfirmed offset (effect possible): refused typed; the next motion
  // rebinds even though the adapter then sends nothing.
  real.device->SetToolContextRearm(false);
  real.device->SetToolContextResult(VendorFailure(
      core::ErrorDTO{"execution_rejected", "no answer",
                     {{"reason", "tool_context_not_applied"},
                      {"tool_context_field", "tcp_offset"},
                      {"effect", "possible"}}},
      true));
  const auto refused = real.world.handle->ApplyMotionToolContext(TcpEntry("default_tcp"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "tool_context_not_applied");
  // T-N1: sent and not confirmed is the component fault; the value stays.
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);
  EXPECT_EQ(real.world.handle->ReadActiveTcp().value->entry, TcpEntry("finger_link"));
  real.device->SetToolContextResult({});
  EXPECT_TRUE(Answered(real.Dispatch("ros2_control.controller.recover")));
  const auto rebound_epoch = real.world.handle->current_port()->ReadReadiness().binding_epoch;
  ASSERT_TRUE(real.world.handle->ApplyMotionToolContext(TcpEntry("default_tcp")).ok());
  EXPECT_GT(real.world.handle->current_port()->ReadReadiness().binding_epoch.value(),
            rebound_epoch.value());
}

// TOOLCTX: Mock and Isaac have no vendor adapter: nothing to apply, and a
// tool context source cannot be installed without one. Active TCP (M7): a
// TCP change commits at once.
TEST(RetainedRos2ControlRuntimeHandleTest, MockAndIsaacIgnoreTheToolContext) {
  ArmWorld world;
  EXPECT_THROW(world.handle->InstallToolContextSource(
                   [](const std::optional<std::string> &) {
                     return core::Result<core::ToolContextDTO>::Ok({});
                   }),
               std::logic_error);
  world.handle->InstallActiveTcp(kRobot, TcpRows({"tcp", "tool"}), true);
  world.handle->StartBackend();
  EXPECT_TRUE(world.handle->ApplyMotionToolContext(TcpEntry("tool")).ok());
  EXPECT_EQ(world.handle->ReadActiveTcp().value->entry, TcpEntry("tool"));
  EXPECT_TRUE(world.handle->ApplyLatestCommittedToolPayload().ok());
  // M6 ADAPTERM: nothing to re-engage either.
  EXPECT_TRUE(world.handle->ReengageMotionAtRest().ok());
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

TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmDisableSwitchesOffTheControllerThenTheHardware) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(real.world.handle->ActivateArmController(std::stop_token{}).outcome,
            Outcome::kSucceeded);
  ASSERT_TRUE(real.WaitForActivity(true, true, true));
  // The hardware is never disarmed under an active controller.
  const auto early = real.Dispatch("ros2_control.hardware.deactivate");
  EXPECT_EQ(early.outcome, Outcome::kNotSent);
  EXPECT_EQ(Reason(early), "ros2_control_controller_still_active");
  EXPECT_TRUE(real.world.manager.hardware_history().empty());

  // D4 option B: the controller first...
  ASSERT_EQ(real.Decide(Intent::kDisable, State::kDisabled),
            "ros2_control.controller.deactivate");
  ASSERT_TRUE(Answered(real.Dispatch("ros2_control.controller.deactivate")));
  ASSERT_TRUE(real.WaitForActivity(true, false, true));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDisabled);
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  // ...then the hardware: Disabled is not armed.
  ASSERT_EQ(real.Decide(Intent::kDisable, State::kDisabled),
            "ros2_control.hardware.deactivate");
  const auto disarmed = real.Dispatch("ros2_control.hardware.deactivate");
  ASSERT_TRUE(Answered(disarmed)) << Reason(disarmed);
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{false});
  EXPECT_EQ(real.device->Events(),
            (std::vector<std::string>{"begin:disarm", "settle:disarm:effect"}));
  ASSERT_TRUE(real.WaitForActivity(false, false, true));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDisabled);
  EXPECT_EQ(ArmWorld::Cause(real.world.Snapshot()), "none");
  EXPECT_EQ(real.Decide(Intent::kDisable, State::kDisabled), "satisfied");
  // The next Prepare re-arms from Disabled.
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.hardware.activate");
}

// M6 XADAPT: Recover clears the latched error first, with the hardware
// still armed, then disarms, then clears the warning.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmRecoverClearsTheErrorFirstThenDisarmsAndClearsTheWarning) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  ASSERT_EQ(real.world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  // An emergency stop while prepared (the plugin keeps the hardware active).
  real.device->Set(DeviceStatus(core::LifecycleBlockingCauseClassDTO::kEmergencyStop, true));
  EXPECT_EQ(real.world.Snapshot().component_state, State::kFaulted);

  // Each step only while its precondition holds: error, hardware, warning.
  ASSERT_EQ(real.Decide(Intent::kRecover, State::kDisabled), "device.clear_error");
  ASSERT_TRUE(Answered(real.Dispatch("device.clear_error")));
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  real.device->Set(ReadyDeviceStatus(true));
  ASSERT_EQ(real.Decide(Intent::kRecover, State::kDisabled),
            "ros2_control.hardware.deactivate");
  ASSERT_TRUE(Answered(real.Dispatch("ros2_control.hardware.deactivate")));
  ASSERT_TRUE(real.WaitForActivity(false, false, true));
  ASSERT_EQ(real.Decide(Intent::kRecover, State::kDisabled), "xarm.clear_warning");
  // A vendor outcome without an answer is uncertain, reconciled by Observe.
  real.device->dispatch_result = {RetainedRos2ControlSwitchOutcome::kUncertain,
                                  core::ErrorDTO{"execution_rejected", "timeout",
                                                 {{"reason", "xarm_service_timeout"}}}};
  const auto warned = real.Dispatch("xarm.clear_warning");
  EXPECT_EQ(warned.outcome, Outcome::kUncertain);
  EXPECT_EQ(Reason(warned), "xarm_service_timeout");
  real.device->Set(ReadyDeviceStatus(false));
  EXPECT_EQ(real.Decide(Intent::kRecover, State::kDisabled), "satisfied");
  EXPECT_EQ(real.world.Snapshot().component_state, State::kDisabled);
  // One call each, in order; the controller was never switched.
  EXPECT_EQ(real.device->Dispatched(),
            (std::vector<std::string>{"device.clear_error", "xarm.clear_warning"}));
  EXPECT_EQ(real.world.manager.hardware_history(), std::vector<bool>{false});
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  // The target driver then re-drives Prepare (D10).
  EXPECT_EQ(real.Decide(Intent::kPrepare, State::kPrepared),
            "ros2_control.hardware.activate");
}

// M6 XADAPT: with the error still reported, a Recover switches nothing off:
// the clear is the only eligible step (a clear without effect then ends it).
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmRecoverSwitchesNothingOffWhileTheErrorIsReported) {
  RealArmWorld real(true);
  ASSERT_TRUE(real.PublishJoints(0.25));
  real.device->Set(DeviceStatus(core::LifecycleBlockingCauseClassDTO::kEmergencyStop, true));
  ASSERT_EQ(real.Decide(Intent::kRecover, State::kDisabled), "device.clear_error");
  real.device->dispatch_result = {
      RetainedRos2ControlSwitchOutcome::kFailedNoEffect,
      core::ErrorDTO{"execution_rejected", "the emergency stop is still engaged (C2)",
                     {{"reason", "xarm_error_persists_after_clear"},
                      {"vendor_code", "C2"}}}};
  const auto cleared = real.Dispatch("device.clear_error");
  EXPECT_EQ(cleared.outcome, Outcome::kFailedNoEffect);
  // Still the clear only, never the disarm.
  EXPECT_EQ(real.Decide(Intent::kRecover, State::kDisabled), "device.clear_error");
  EXPECT_TRUE(real.world.manager.hardware_history().empty());
  EXPECT_EQ(real.world.manager.switch_requests(), 0U);
  EXPECT_TRUE(real.world.handle->arm_activity().hardware_active);
}

// DX-2967/2969: the wrapper's report stream remains at C2, while the
// adapter's on-demand result is C10 and its own before facts are unavailable.
// RealArmStep must preserve the adapter's uncertain outcome instead of using
// report-only facts to claim no effect, and must not add another query.
// V-A (A2): the adapter says so typed (`adapter_decided`), whatever its
// reason; without it the same uncertain outcome gets the post-step query.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmClearWithUnusableAdapterBaselineDoesNotAddAStatusWait) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  RealArmWorld real;
  real.device->Set(DeviceStatus(Cause::kEmergencyStop, true));
  real.device->dispatch_result = {
      RetainedRos2ControlSwitchOutcome::kUncertain,
      core::ErrorDTO{"execution_rejected", "the clear result is unresolved",
                     {{"reason", "xarm_error_persists_after_clear"},
                      {"vendor_code", "C10"}}},
      true};
  const auto lists = real.world.manager.list_requests();
  const auto unresolved = real.Dispatch("device.clear_error");
  EXPECT_EQ(unresolved.outcome, Outcome::kUncertain);
  EXPECT_EQ(Reason(unresolved), "xarm_error_persists_after_clear");
  ASSERT_TRUE(unresolved.error.has_value());
  EXPECT_EQ(unresolved.error->details.at("vendor_code"), "C10");
  // One baseline consists of the controller and hardware lists; the fake
  // counts its `list_controllers` calls, one per status query. A second one
  // would be RealArmStep's otherwise automatic post-step query.
  EXPECT_EQ(real.world.manager.list_requests(), lists + 1U);
  EXPECT_EQ(ArmWorld::Cause(real.world.Snapshot()), "emergency_stop");

  // The same outcome and strings, not decided by the adapter: queried after.
  real.device->dispatch_result.adapter_decided = false;
  const auto queried_lists = real.world.manager.list_requests();
  const auto queried = real.Dispatch("device.clear_error");
  EXPECT_EQ(queried.outcome, Outcome::kUncertain);
  EXPECT_EQ(real.world.manager.list_requests(), queried_lists + 2U);
}

// m5.5 (REALWEDGE fix 1): a clear the robot proved had no effect is a
// known failure with no effect, not a possible effect: the journal accepts
// it after the Recover's observed disarm, so the transaction fails typed.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmClearWithNoEffectIsAKnownFailureWithoutEffect) {
  RealArmWorld real;
  real.device->dispatch_result = {
      RetainedRos2ControlSwitchOutcome::kFailedNoEffect,
      core::ErrorDTO{"execution_rejected", "the error persists",
                     {{"reason", "xarm_error_persists_after_clear"},
                      {"vendor_code", "C2"}}}};
  const auto cleared = real.Dispatch("device.clear_error");
  EXPECT_EQ(cleared.outcome, Outcome::kFailedNoEffect);
  EXPECT_EQ(Reason(cleared), "xarm_error_persists_after_clear");
  ASSERT_TRUE(cleared.error.has_value());
  EXPECT_EQ(cleared.error->details.at("vendor_code"), "C2");
  // M6 XADAPT: with the current status (the fake reports no vendor codes).
  EXPECT_EQ(cleared.error->details.at("vendor_status"), "none");
  EXPECT_EQ(cleared.error->details.at("ros2_control_status"),
            "hardware=inactive controller=inactive broadcaster=active");

  // M6 XADAPT: a vendor `kFailed` (the adapter could not tell) is decided by
  // the queried status like any refused step: here unchanged, no effect.
  real.device->dispatch_result = {
      RetainedRos2ControlSwitchOutcome::kFailed,
      core::ErrorDTO{"execution_rejected", "refused", {{"reason", "refused"}}}};
  const auto refused = real.Dispatch("device.clear_error");
  EXPECT_EQ(refused.outcome, Outcome::kFailedNoEffect);
}

/// The unmodified xArm driver's `clean_error` service and `robot_states`
/// topic, on the node the vendor adapter starts on.
class FakeXArmDriver final : public fake::PeerNode {
public:
  FakeXArmDriver()
      : PeerNode("fake_xarm_driver"),
        ns("/manyforge_xarm_clear_test_" + std::to_string(std::random_device{}()) +
           "_" + fake::unique_suffix() + "/xarm") {
    clean_error = node()->create_service<xarm_msgs::srv::Call>(
        ns + "/clean_error",
        [this](const xarm_msgs::srv::Call::Request::SharedPtr,
               xarm_msgs::srv::Call::Response::SharedPtr response) {
          read_calls_at_clear = read_calls.load();
          state_read_calls_at_clear = state_read_calls.load();
          ++clean_error_calls;
          response->ret = clean_error_ret.load();
        });
    // M6 XADAPT D11: the on-demand error read (`datas` = [error, warning]),
    // answering the latest reported codes unless scripted; held unanswered
    // while `HoldReads(true)`.
    get_err_warn_code = node()->create_service<xarm_msgs::srv::GetInt16List>(
        ns + "/get_err_warn_code",
        [this](const xarm_msgs::srv::GetInt16List::Request::SharedPtr,
               xarm_msgs::srv::GetInt16List::Response::SharedPtr response) {
          const auto call = ++read_calls;
          std::unique_lock lock(read_mutex);
          read_released.wait(lock, [&] {
            return !reads_held && call < reads_held_from;
          });
          int error = last_error.load();
          int warning = last_warning.load();
          if (!scripted_errors.empty()) {
            error = scripted_errors.front();
            scripted_errors.erase(scripted_errors.begin());
          }
          if (!scripted_warnings.empty()) {
            warning = scripted_warnings.front();
            scripted_warnings.erase(scripted_warnings.begin());
          }
          response->ret = 0;
          response->datas = {static_cast<std::int16_t>(error),
                             static_cast<std::int16_t>(warning)};
          ++completed_read_calls;
        });
    get_state = node()->create_service<xarm_msgs::srv::GetInt16>(
        ns + "/get_state",
        [this](const xarm_msgs::srv::GetInt16::Request::SharedPtr,
               xarm_msgs::srv::GetInt16::Response::SharedPtr response) {
          ++state_read_calls;
          std::unique_lock lock(read_mutex);
          read_released.wait(lock, [&] { return !state_reads_held; });
          response->ret = state_read_ret.load();
          int state = state_value.load();
          if (!scripted_states.empty()) {
            state = scripted_states.front();
            scripted_states.erase(scripted_states.begin());
          }
          response->data = static_cast<std::int16_t>(state);
          ++completed_state_read_calls;
        });
    // M6b (SX-727): `set_tcp_offset`, answered `ret` 0 unless held.
    set_tcp_offset = node()->create_service<xarm_msgs::srv::SetFloat32List>(
        ns + "/set_tcp_offset",
        [this](const xarm_msgs::srv::SetFloat32List::Request::SharedPtr,
               xarm_msgs::srv::SetFloat32List::Response::SharedPtr response) {
          ++offset_calls;
          std::unique_lock lock(read_mutex);
          read_released.wait(lock, [&] { return !offsets_held; });
          response->ret = 0;
          ++completed_offset_calls;
        });
    states = node()->create_publisher<xarm_msgs::msg::RobotMsg>(ns + "/robot_states", 10);
    Start();
  }
  ~FakeXArmDriver() override {
    HoldReads(false);
    HoldStateReads(false);
    HoldOffsets(false);
    Stop();
  }

  void HoldOffsets(const bool held) {
    {
      std::lock_guard lock(read_mutex);
      offsets_held = held;
    }
    read_released.notify_all();
  }

  void HoldReads(const bool held) {
    {
      std::lock_guard lock(read_mutex);
      reads_held = held;
    }
    read_released.notify_all();
  }
  void HoldStateReads(const bool held) {
    {
      std::lock_guard lock(read_mutex);
      state_reads_held = held;
    }
    read_released.notify_all();
  }
  void HoldReadsFrom(const int first_call) {
    {
      std::lock_guard lock(read_mutex);
      reads_held_from = first_call;
    }
    read_released.notify_all();
  }
  [[nodiscard]] bool WaitForClearServices() {
    return node()->create_client<xarm_msgs::srv::Call>(ns + "/clean_error")
               ->wait_for_service(10s) &&
           node()->create_client<xarm_msgs::srv::GetInt16List>(
               ns + "/get_err_warn_code")
               ->wait_for_service(10s) &&
           node()->create_client<xarm_msgs::srv::GetInt16>(ns + "/get_state")
               ->wait_for_service(10s);
  }
  void ScriptReads(std::vector<int> errors, std::vector<int> warnings = {}) {
    std::lock_guard lock(read_mutex);
    scripted_errors = std::move(errors);
    scripted_warnings = std::move(warnings);
  }
  void ScriptStates(std::vector<int> states_to_return) {
    std::lock_guard lock(read_mutex);
    scripted_states = std::move(states_to_return);
  }

  /// One report: the error, warning and axis-enable mask (`mt_able`).
  struct Status final {
    int error = 0;
    int warning = 0;
    int enabled_axes = 0;
    int state = -1;
  };

  void Report(const Status status) {
    last_error = status.error;
    last_warning = status.warning;
    const int reported_state = status.state >= 0
                                   ? status.state
                                   : status.error == 0 ? 2 : 4;
    state_value = reported_state;
    xarm_msgs::msg::RobotMsg message;
    message.state = static_cast<std::int16_t>(reported_state);
    message.mode = 0;
    message.err = status.error;
    message.warn = status.warning;
    message.mt_able = status.enabled_axes;
    message.mt_brake = 0;
    message.angle = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    message.offset = reported_offset;  // float32[6]: a std::array
    states->publish(message);
  }

  const std::string ns;
  rclcpp::Service<xarm_msgs::srv::Call>::SharedPtr clean_error;
  rclcpp::Service<xarm_msgs::srv::GetInt16>::SharedPtr get_state;
  rclcpp::Publisher<xarm_msgs::msg::RobotMsg>::SharedPtr states;
  std::atomic<int> clean_error_calls{0};
  std::atomic<int> read_calls_at_clear{-1};
  std::atomic<int> state_read_calls_at_clear{-1};
  /// The driver's `ret` (M6 XADAPT: API code 1, an error still present).
  std::atomic<int> clean_error_ret{0};
  rclcpp::Service<xarm_msgs::srv::GetInt16List>::SharedPtr get_err_warn_code;
  std::atomic<int> state_read_calls{0};
  std::atomic<int> completed_state_read_calls{0};
  std::atomic<int> state_read_ret{0};
  std::atomic<int> state_value{2};
  std::atomic<int> read_calls{0};
  std::atomic<int> completed_read_calls{0};
  std::atomic<int> last_error{0};
  std::atomic<int> last_warning{0};
  std::mutex read_mutex;
  std::condition_variable read_released;
  bool reads_held{false};
  int reads_held_from{std::numeric_limits<int>::max()};
  bool state_reads_held{false};
  std::vector<int> scripted_errors;
  std::vector<int> scripted_warnings;
  std::vector<int> scripted_states;
  /// The reported TCP offset (`robot_states.offset`: mm, then radians).
  std::array<float, 6> reported_offset{};
  rclcpp::Service<xarm_msgs::srv::SetFloat32List>::SharedPtr set_tcp_offset;
  std::atomic<int> offset_calls{0};
  std::atomic<int> completed_offset_calls{0};
  bool offsets_held{false};
};

// M6b (SX-727): with the TCP offset sync off, Enable's zero-offset send is
// part of the lifecycle step. No time left: nothing is sent
// (`component_lifecycle_deadline_expired`). A held `set_tcp_offset` is
// allowed only the step's remaining time, then the connection is lost
// (effect possible), and it follows the step's cancellation.
TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmEnableZeroOffsetSendFollowsTheStepBudgetAndCancellation) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.node()
                  ->create_client<xarm_msgs::srv::SetFloat32List>(driver.ns + "/set_tcp_offset")
                  ->wait_for_service(10s));
  // A TCP left from Studio (100 mm), in a fresh report.
  driver.reported_offset = {0.0F, 0.0F, 100.0F, 0.0F, 0.0F, 0.0F};
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({});
    return adapter->ReadReportFacts(std::nullopt, {}).has_value();
  }));
  // Active TCP (M7): with the checkbox off the handle's Enable command is
  // the zero offset and the payload (A2b-x's adapter sends them).
  const VendorToolContextCommand context{VendorToolOffsetZero{},
                                         VendorToolLoadSet{0.5, {0.0, 0.0, 0.04}}};

  const auto expired = adapter->SendToolContext(
      context, true, std::chrono::steady_clock::now() - 1ms, std::stop_token{});
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error->code, "component_lifecycle_deadline_expired");
  EXPECT_FALSE(expired.effect_possible);
  EXPECT_EQ(driver.offset_calls.load(), 0);

  driver.HoldOffsets(true);
  const auto started = std::chrono::steady_clock::now();
  const auto unanswered =
      adapter->SendToolContext(context, true, started + 300ms, std::stop_token{});
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  ASSERT_FALSE(unanswered.ok());
  EXPECT_EQ(unanswered.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(unanswered.error->details.at("effect"), "possible");
  // V-A (A2): the same, typed for the handle.
  EXPECT_TRUE(unanswered.effect_possible);
  EXPECT_TRUE(unanswered.unresponsive);
  EXPECT_EQ(driver.offset_calls.load(), 1);
  driver.HoldOffsets(false);
  ASSERT_TRUE(fake::WaitUntil([&] { return driver.completed_offset_calls.load() == 1; }));

  driver.HoldOffsets(true);
  std::stop_source stop;
  auto cancelled = std::async(std::launch::async, [&] {
    return adapter->SendToolContext(context, true, std::nullopt, stop.get_token());
  });
  ASSERT_TRUE(fake::WaitUntil([&] { return driver.offset_calls.load() == 2; }));
  stop.request_stop();
  const auto result = cancelled.get();
  driver.HoldOffsets(false);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error->code, "execution_rejected");
  EXPECT_EQ(result.error->details.at("vendor_reason"), "xarm_service_cancelled");
  EXPECT_EQ(result.error->details.at("effect"), "possible");
  EXPECT_TRUE(result.effect_possible);
  EXPECT_FALSE(result.unresponsive);
}

// m5.5 (REALWEDGE fix 1), M6 XADAPT D12 / DX-2987: the xArm adapter's own
// `clean_error` reads error/warning and state on demand before sending, then
// verifies with the scheduled error/warning reads and an on-demand state read
// whenever the error persists. Only those three values decide no effect.
TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmClearVerifiesTheErrorAgainstOnDemandFacts) {
  using Status = FakeXArmDriver::Status;
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  // The pre-call report: C2 under the pressed emergency stop.
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    for (const auto &fact : adapter->ReadHealth(clock.now()).facts)
      if (fact.lifecycle_fact_id.value() == core::kDeviceErrorPresentFactId)
        return fact.value.boolean_value == true;
    return false;
  }));

  // One clear; once it reaches the driver, the fake services continue to
  // report `after` while the verification offsets elapse.
  const auto clear = [&](const Status after,
                         const std::optional<std::chrono::steady_clock::time_point>
                             step_deadline = std::nullopt) {
    std::stop_source stop;
    const int calls = driver.clean_error_calls.load();
    const int reads_before = driver.read_calls.load();
    const int states_before = driver.state_read_calls.load();
    auto result = std::async(std::launch::async, [&] {
      return adapter->Dispatch(
          core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
          step_deadline, stop.get_token());
    });
    EXPECT_TRUE(fake::WaitUntil([&] { return driver.clean_error_calls.load() > calls; }));
    EXPECT_EQ(driver.read_calls_at_clear.load(), reads_before + 1);
    EXPECT_EQ(driver.state_read_calls_at_clear.load(), states_before + 1);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (result.wait_for(20ms) != std::future_status::ready) {
      if (std::chrono::steady_clock::now() >= deadline) stop.request_stop();
      clock.advance(100'000'000);
      driver.Report(after);
    }
    return result.get();
  };
  const auto expect_persists = [](const RetainedRos2ControlSwitchResult &result,
                                  const RetainedRos2ControlSwitchOutcome outcome,
                                  const std::string &vendor_code) {
    EXPECT_EQ(result.outcome, outcome);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"), "xarm_error_persists_after_clear");
    EXPECT_EQ(result.error->details.at("vendor_code"), vendor_code);
  };

  // C2 before and after, nothing else changed: no effect.
  expect_persists(clear({.error = 2}), RetainedRos2ControlSwitchOutcome::kFailedNoEffect,
                  "C2");
  // A changed axis-enable report does not change the three on-demand facts.
  expect_persists(clear({.error = 2, .enabled_axes = 0x3F}),
                  RetainedRos2ControlSwitchOutcome::kFailedNoEffect, "C2");
  // A warning appeared after the call in the on-demand read.
  expect_persists(clear({.error = 2, .warning = 7, .enabled_axes = 0x3F}),
                  RetainedRos2ControlSwitchOutcome::kFailedEffectObserved, "C2");
  // Another error after the call.
  expect_persists(clear({.error = 3, .warning = 7, .enabled_axes = 0x3F}),
                  RetainedRos2ControlSwitchOutcome::kFailedEffectObserved, "C3");

  const auto cleared = clear({});
  EXPECT_EQ(cleared.outcome, RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_FALSE(cleared.error.has_value());
  EXPECT_EQ(driver.clean_error_calls.load(), 5);

  // M6 XADAPT: API code 1 ("controller has uncleared errors") is an answer,
  // verified with on-demand reads like 0: with the emergency stop still engaged
  // the clear failed with no effect, typed for the operator.
  driver.clean_error_ret = 1;
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    for (const auto &fact : adapter->ReadHealth(clock.now()).facts)
      if (fact.lifecycle_fact_id.value() == core::kDeviceErrorPresentFactId)
        return fact.value.boolean_value == true;
    return false;
  }));
  const auto engaged = clear({.error = 2});
  expect_persists(engaged, RetainedRos2ControlSwitchOutcome::kFailedNoEffect, "C2");
  ASSERT_TRUE(engaged.error.has_value());
  EXPECT_NE(engaged.error->message.find("emergency stop is still engaged (C2)"),
            std::string::npos);
  EXPECT_EQ(driver.clean_error_calls.load(), 6);

  // M6 XADAPT: the vendor facts of the first report after a boundary, none
  // until one arrives (within one freshness window).
  const auto boundary = clock.now();
  EXPECT_FALSE(adapter->ReadReportFacts(boundary, std::stop_token{}).has_value());
  clock.advance(100'000'000);
  std::optional<std::vector<core::LifecycleFactDTO>> next;
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    next = adapter->ReadReportFacts(boundary, std::stop_token{});
    return next.has_value();
  }));
  EXPECT_EQ(next, adapter->ReadReportFacts(std::nullopt, std::stop_token{}));

  // A bounded step with enough remaining time for the final state read still
  // classifies the three last on-demand facts.
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    return adapter->ReadReportFacts(std::nullopt, std::stop_token{}).has_value();
  }));
  const auto budget = clear({.error = 2}, std::chrono::steady_clock::now() + 800ms);
  expect_persists(budget, RetainedRos2ControlSwitchOutcome::kFailedNoEffect, "C2");
  ASSERT_TRUE(budget.error.has_value());
  EXPECT_NE(budget.error->code, "robot_controller_unresponsive");

  // M6 XADAPT (SX-695/699): no time left at the send: nothing reaches the
  // driver, `component_lifecycle_deadline_expired`, no fault.
  const int calls = driver.clean_error_calls.load();
  const auto expired = adapter->Dispatch(
      core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
      std::chrono::steady_clock::now() - 1ms, std::stop_token{});
  EXPECT_EQ(expired.outcome, RetainedRos2ControlSwitchOutcome::kNotSent);
  ASSERT_TRUE(expired.error.has_value());
  EXPECT_EQ(expired.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(driver.clean_error_calls.load(), calls);
}

// M6 XADAPT D12: the clear's on-demand reads. A pre-read precedes each clear;
// post-clear error zero succeeds immediately, while persistence reads state
// and compares all three facts. A silent read is connection loss.
TEST(RetainedRos2ControlRuntimeHandleTest, XArmClearIsVerifiedByShortOnDemandReads) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    return adapter->ReadReportFacts(std::nullopt, std::stop_token{}).has_value();
  }));
  const auto clear = [&](const std::optional<std::chrono::steady_clock::time_point>
                             step_deadline = std::nullopt) {
    const int calls = driver.clean_error_calls.load();
    auto result = std::async(std::launch::async, [&] {
      return adapter->Dispatch(
          core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
          step_deadline, std::stop_token{});
    });
    EXPECT_TRUE(fake::WaitUntil([&] { return driver.clean_error_calls.load() > calls; }));
    const auto watchdog = std::chrono::steady_clock::now() + 5s;
    while (result.wait_for(5ms) != std::future_status::ready) {
      if (std::chrono::steady_clock::now() >= watchdog) break;
      clock.advance(100'000'000);
      driver.Report({.error = 2});
    }
    return result.get();
  };

  // The pre-read and first post-clear read keep C2; the next read clears it.
  driver.ScriptReads({2, 2, 0});
  auto reads = driver.read_calls.load();
  auto state_reads = driver.state_read_calls.load();
  const auto cleared = clear();
  EXPECT_EQ(cleared.outcome, RetainedRos2ControlSwitchOutcome::kSucceeded);
  EXPECT_EQ(driver.read_calls.load(), reads + 3);
  EXPECT_EQ(driver.state_read_calls.load(), state_reads + 1);

  // Persisting through every read (the robot keeps C2, facts unchanged).
  reads = driver.read_calls.load();
  state_reads = driver.state_read_calls.load();
  const auto started = std::chrono::steady_clock::now();
  const auto persisting = clear();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_EQ(driver.read_calls.load(), reads + 4);
  EXPECT_EQ(driver.state_read_calls.load(), state_reads + 2);
  EXPECT_EQ(persisting.outcome, RetainedRos2ControlSwitchOutcome::kFailedNoEffect);
  ASSERT_TRUE(persisting.error.has_value());
  EXPECT_EQ(persisting.error->details.at("reason"), "xarm_error_persists_after_clear");
  EXPECT_EQ(persisting.error->details.at("vendor_code"), "C2");

  // The pre-read answers, but the first post-clear error read is silent past
  // the step's remaining 0.5 s.
  reads = driver.read_calls.load();
  driver.HoldReadsFrom(reads + 2);
  const auto silent = clear(std::chrono::steady_clock::now() + 500ms);
  driver.HoldReadsFrom(std::numeric_limits<int>::max());
  EXPECT_EQ(silent.outcome, RetainedRos2ControlSwitchOutcome::kUncertain);
  ASSERT_TRUE(silent.error.has_value());
  EXPECT_EQ(silent.error->code, "robot_controller_unresponsive");
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmClearCancellationAndOffsetWaitRespectTheStepBudget) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    return adapter->ReadReportFacts(std::nullopt, std::stop_token{}).has_value();
  }));

  // The pre-read and first post-clear error/warning read answer, then
  // cancellation arrives during the wait for the next offset.
  std::stop_source stop;
  const int reads_before_cancel = driver.completed_read_calls.load();
  const int state_reads_before_cancel = driver.state_read_calls.load();
  const int clears_before_cancel = driver.clean_error_calls.load();
  auto cancelled_future = std::async(std::launch::async, [&] {
    return adapter->Dispatch(
        core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
        std::nullopt, stop.get_token());
  });
  ASSERT_TRUE(fake::WaitUntil([&] {
    return driver.clean_error_calls.load() > clears_before_cancel &&
           driver.completed_read_calls.load() > reads_before_cancel + 1;
  }));
  std::this_thread::sleep_for(20ms);
  stop.request_stop();
  const auto cancelled = cancelled_future.get();
  EXPECT_EQ(cancelled.outcome, RetainedRos2ControlSwitchOutcome::kUncertain);
  ASSERT_TRUE(cancelled.error.has_value());
  EXPECT_EQ(cancelled.error->details.at("reason"),
            "xarm_clear_verification_cancelled");
  EXPECT_EQ(driver.read_calls.load(), reads_before_cancel + 2);
  EXPECT_EQ(driver.state_read_calls.load(), state_reads_before_cancel + 1);

  // The pre-read and first two post-clear error/warning reads answer C2. The
  // step expires before the 300 ms offset and leaves no budget for the state
  // needed to classify persistence, so the outcome remains uncertain.
  const int reads_before_budget = driver.read_calls.load();
  const int states_before_budget = driver.state_read_calls.load();
  const int clears_before_budget = driver.clean_error_calls.load();
  const auto started = std::chrono::steady_clock::now();
  const auto step_deadline = started + 220ms;
  auto budget_future = std::async(std::launch::async, [&] {
    return adapter->Dispatch(
        core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
        step_deadline, std::stop_token{});
  });
  ASSERT_TRUE(fake::WaitUntil([&] {
    return driver.clean_error_calls.load() > clears_before_budget;
  }));
  const auto watchdog = started + 2s;
  while (budget_future.wait_for(5ms) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= watchdog) break;
    clock.advance(100'000'000);
    driver.Report({.error = 2});
  }
  const auto budget = budget_future.get();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 280ms);
  EXPECT_EQ(driver.read_calls.load(), reads_before_budget + 3);
  EXPECT_EQ(driver.state_read_calls.load(), states_before_budget + 1);
  EXPECT_EQ(budget.outcome, RetainedRos2ControlSwitchOutcome::kUncertain);
  ASSERT_TRUE(budget.error.has_value());
  EXPECT_EQ(budget.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(budget.error->details.at("reason"), "deadline_exhausted");
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmClearComparesOnlyOnDemandErrorWarningAndState) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  ASSERT_TRUE(fake::WaitUntil([&] {
    driver.Report({.error = 2});
    return adapter->ReadReportFacts(std::nullopt, std::stop_token{}).has_value();
  }));
  const auto clear = [&] {
    return adapter->Dispatch(
        core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
        std::nullopt, std::stop_token{});
  };
  const auto clear_with_reads = [&](std::vector<int> errors,
                                    std::vector<int> warnings,
                                    std::vector<int> states) {
    driver.ScriptReads(std::move(errors), std::move(warnings));
    driver.ScriptStates(std::move(states));
    const int calls = driver.clean_error_calls.load();
    auto result = std::async(std::launch::async, [&] { return clear(); });
    EXPECT_TRUE(fake::WaitUntil([&] { return driver.clean_error_calls.load() > calls; }));
    const auto watchdog = std::chrono::steady_clock::now() + 5s;
    while (result.wait_for(5ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < watchdog) {}
    return result.get();
  };

  // No post-clear robot_states report is needed. The same on-demand facts
  // before and after support a no-effect result.
  const auto unchanged = clear_with_reads({2, 2, 2, 2}, {0, 0, 0, 0}, {4, 4});
  EXPECT_EQ(unchanged.outcome, RetainedRos2ControlSwitchOutcome::kFailedNoEffect);

  // A different on-demand error is an observed effect even though the last
  // robot_states report still shows C2.
  const auto changed_error = clear_with_reads({2, 10, 10, 10}, {0, 0, 0, 0}, {4, 4});
  EXPECT_EQ(changed_error.outcome,
            RetainedRos2ControlSwitchOutcome::kFailedEffectObserved);
  ASSERT_TRUE(changed_error.error.has_value());
  EXPECT_EQ(changed_error.error->details.at("vendor_code"), "C10");

  // Warning changes are compared from the on-demand answers.
  const auto changed_warning = clear_with_reads({2, 2, 2, 2}, {0, 7, 7, 7}, {4, 4});
  EXPECT_EQ(changed_warning.outcome,
            RetainedRos2ControlSwitchOutcome::kFailedEffectObserved);
  ASSERT_TRUE(changed_warning.error.has_value());
  EXPECT_EQ(changed_warning.error->details.at("vendor_code"), "C2");

  // State is the third compared on-demand fact.
  const auto changed_state = clear_with_reads({2, 2, 2, 2}, {0, 0, 0, 0}, {4, 2});
  EXPECT_EQ(changed_state.outcome,
            RetainedRos2ControlSwitchOutcome::kFailedEffectObserved);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmClearBeforeReadAnsweredErrorDoesNotSendClear) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  driver.state_read_ret = 1;
  const auto refused = adapter->Dispatch(
      core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
      std::nullopt, std::stop_token{});
  EXPECT_EQ(refused.outcome, RetainedRos2ControlSwitchOutcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "xarm_service_returned_nonzero");
  EXPECT_EQ(refused.error->details.at("clear_error_sent"), "false");
  EXPECT_EQ(driver.clean_error_calls.load(), 0);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     XArmClearBeforeReadUnansweredDoesNotSendAndReportsConnectionFault) {
  FakeXArmDriver driver;
  core::FakeClock clock;
  clock.set(1'000'000'000);
  const auto adapter = make_retained_vendor_device_status_adapter(
      {.vendor = "xarm",
       .driver_namespace = driver.ns,
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  adapter->Start(*driver.node(), clock);
  ASSERT_TRUE(driver.WaitForClearServices());
  driver.HoldStateReads(true);
  auto result = std::async(std::launch::async, [&] {
    return adapter->Dispatch(
        core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
        std::chrono::steady_clock::now() + 150ms, std::stop_token{});
  });
  ASSERT_TRUE(fake::WaitUntil([&] { return driver.state_read_calls.load() == 1; }));
  const auto unanswered = result.get();
  driver.HoldStateReads(false);
  EXPECT_EQ(unanswered.outcome, RetainedRos2ControlSwitchOutcome::kUncertain);
  ASSERT_TRUE(unanswered.error.has_value());
  EXPECT_EQ(unanswered.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(unanswered.error->details.at("reason"), "xarm_service_timeout");
  EXPECT_EQ(unanswered.error->details.at("clear_error_sent"), "false");
  EXPECT_EQ(driver.clean_error_calls.load(), 0);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     MockArmWithoutVendorAdapterKeepsItsExactFacts) {
  ArmWorld world;
  // A listed broadcaster changes nothing for Mock (R7 package 4).
  world.manager.SetBroadcasterActive(true);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = world.handle->arm_activity();
    (void)world.Snapshot();
    return activity.observed && activity.controller_loaded && activity.hardware_active &&
           world.handle->ReadLifecycleHealth(core::ControlDomainId("domain.arm"),
                                             world.clock.now()).ok();
  }));
  const auto snapshot = world.Snapshot();
  std::vector<std::string> ids;
  for (const auto &fact : snapshot.facts) ids.push_back(fact.lifecycle_fact_id.value());
  EXPECT_EQ(ids, (std::vector<std::string>{
                     "external.controller_configuration_ready",
                     "external.controller_inactive_unrequested",
                     "external.controller_loaded", "external.fault_present",
                     "external.transport_ready", "manyforge.blocking_cause_class"}));
  // Without a vendor adapter the Real primitives stay unknown, as before.
  for (const auto *primitive :
       {"ros2_control.hardware.activate", "ros2_control.hardware.deactivate",
        "ros2_control.broadcaster.activate", "device.clear_error"}) {
    const auto outcome = world.handle->lifecycle_primitive_dispatcher()(
        core::ControlDomainId("domain.arm"), core::LifecyclePrimitiveId(primitive),
        std::stop_token{}, std::nullopt, {});
    EXPECT_EQ(outcome.outcome, Outcome::kNotSent);
    ASSERT_TRUE(outcome.error.has_value());
    EXPECT_EQ(outcome.error->details.at("reason"),
              "ros2_control_lifecycle_primitive_unknown");
  }
  EXPECT_TRUE(world.manager.hardware_history().empty());
  EXPECT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
}

TEST(RetainedRos2ControlRuntimeHandleTest,
     VendorJointDisplaySampleRequiresShapeAndEveryExactName) {
  // M6 (SX-507): a vendor sample is mapped (and may refresh the measured
  // stream) only when it names every bound joint exactly once.
  const auto binding = ArmBinding();
  sensor_msgs::msg::JointState message;
  message.name = {"other", "source_joint_1"};
  message.position = {9.0, 0.25};
  const auto sample = vendor_joint_display_sample(binding, message);
  ASSERT_TRUE(sample.has_value());
  EXPECT_EQ(sample->compiled_names, std::vector<std::string>{"joint_1"});
  EXPECT_EQ(sample->positions, std::vector<double>{0.25});

  auto shape = message;
  shape.position = {0.25};
  EXPECT_FALSE(vendor_joint_display_sample(binding, shape).has_value());
  auto unmatched = message;
  unmatched.name = {"other", "joint_1"};
  EXPECT_FALSE(vendor_joint_display_sample(binding, unmatched).has_value());
  auto duplicated = message;
  duplicated.name = {"source_joint_1", "source_joint_1"};
  EXPECT_FALSE(vendor_joint_display_sample(binding, duplicated).has_value());
  auto nonfinite = message;
  nonfinite.position = {9.0, std::numeric_limits<double>::quiet_NaN()};
  EXPECT_FALSE(vendor_joint_display_sample(binding, nonfinite).has_value());
  EXPECT_FALSE(vendor_joint_display_sample(binding, sensor_msgs::msg::JointState{}).has_value());
  // V-A (A4): every bound joint exactly zero is well-formed here; whether it
  // is a pose (the xArm e-stop signature is not) is the adapter's check.
  auto zeros = message;
  zeros.position = {9.0, 0.0};
  const auto zero_sample = vendor_joint_display_sample(binding, zeros);
  ASSERT_TRUE(zero_sample.has_value());
  EXPECT_EQ(zero_sample->positions, std::vector<double>{0.0});
}

// M6 (approval list 2, item 23): on Real the controller stream is the
// authority, so the arm's display always follows the vendor joint topic,
// idle or moving, whatever the stream gate says. An all-zero sample (the
// e-stop signature) keeps the last valid pose. The addressed state that
// planning and readiness read, and the Prepare/Enable measured joints, are
// unchanged by vendor samples.
TEST(RetainedRos2ControlRuntimeHandleTest,
     RealArmDisplayAlwaysFollowsTheVendorJointTopic) {
  ArmWorld world;
  auto device = std::make_shared<FakeDeviceStatusAdapter>();
  device->Set(ReadyDeviceStatus());
  device->vendor_topic = world.prefix + "/xarm/joint_states";
  world.handle->InstallDeviceStatusAdapter(device);
  // The arm display channel as the backend factory builds it: commanded
  // (the binding's authority) over the retained model. A predicted source,
  // or no model projector for a model-backed binding, is refused at
  // construction.
  const auto display = ArmDisplayChannel(world.binding, world.clock);
  ASSERT_TRUE(world.handle->BindMotionDisplayChannel(display).ok());
  // Before any vendor sample, the addressed state renders as usual.
  const std::vector<double> addressed{0.1}, velocity{0.0};
  ASSERT_TRUE(display->publish_state(addressed, velocity, 100, 100).applied());
  ASSERT_TRUE(display->current_bound_render_state().ok());
  EXPECT_FALSE(display->current_bound_render_state().value().display_only);
  world.manager.SetHardwareActive(true);
  world.handle->StartBackend();
  const auto rendered = [&](const double expected) {
    const auto render = display->current_bound_render_state();
    return render.ok() && render.value().display_only &&
           render.value().positions_si == std::vector<double>{expected};
  };
  FakeJointStates vendor{world.prefix + "/xarm/joint_states"};

  // Idle (the gate refuses: no move from the robot's own interface).
  vendor.Publish(0.3);
  ASSERT_TRUE(fake::WaitUntil([&] { return rendered(0.3); }));
  // A later addressed sample updates the planning state, never the display.
  const std::vector<double> planned{0.5};
  ASSERT_TRUE(display->publish_state(planned, velocity, 101, 101).applied());
  EXPECT_TRUE(rendered(0.3));
  const auto owner = display->current_state(101);
  ASSERT_TRUE(owner.ok());
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(owner.value().positions[0]).value_rad, 0.5);

  // The e-stop signature is ignored: the last valid pose stays.
  vendor.Publish(0.0);
  std::this_thread::sleep_for(200ms);
  EXPECT_TRUE(rendered(0.3));

  // Moving: from the robot's own interface (the gate admits) and from
  // ManyForge (the gate refuses) alike.
  device->vendor_gate_open->store(true);
  vendor.Publish(0.35);
  ASSERT_TRUE(fake::WaitUntil([&] { return rendered(0.35); }));
  vendor.Publish(0.4);
  ASSERT_TRUE(fake::WaitUntil([&] { return rendered(0.4); }));
  device->vendor_gate_open->store(false);
  vendor.Publish(0.45);
  ASSERT_TRUE(fake::WaitUntil([&] { return rendered(0.45); }));
  vendor.Pause();

  // Planning still reads the addressed sample; the Prepare/Enable measured
  // joints never came from the vendor topic (no broadcaster sample).
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(display->current_state(101).value().positions[0])
          .value_rad,
      0.5);
  // Health is read once the selected endpoints are established (before,
  // it is refused as `component_instance_graph_unsettled`).
  ASSERT_TRUE(fake::WaitUntil(
      [&] { return RetainedRos2ControlRuntimeHandleTestAccess::EndpointsSettled(*world.handle); }));
  const auto health = world.handle->ReadLifecycleHealth(core::ControlDomainId("domain.arm"),
                                                        world.clock.now());
  ASSERT_TRUE(health.ok()) << health.error().message;
  EXPECT_NE(RealArmWorld::MeasuredValid(health.value()), std::optional<bool>(true));
}

// The one-joint model of `ArmBinding()`, as the backend factory fixtures
// build it: an arm display channel crosses the retained model.
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

TEST(RetainedRos2ControlRuntimeHandleTest,
     VendorJointSamplesMoveOnlyTheDisplayAndAnOpenGateFeedsTheStream) {
  // M6 (S6): the handle's own subscription joins the adapter gate, the
  // `measured_joints` liveness and the display; the addressed state stays.
  // M6 item 23 (REALDISPLAY): the display follows every valid sample; the
  // gate decides only the stream liveness.
  using Cause = core::LifecycleBlockingCauseClassDTO;
  auto clock = std::make_shared<core::FakeClock>();
  clock->set(1000);
  ArmWorld world("arm_controller", std::nullopt, nullptr, ArmBinding(), clock);
  auto device = std::make_shared<FakeDeviceStatusAdapter>();
  device->Set(DeviceStatus(Cause::kNone, false));
  device->vendor_topic = world.prefix + "/xarm/joint_states";
  world.handle->InstallDeviceStatusAdapter(device);
  const auto descriptor = MeasuredSourceDescriptor(world);
  const auto &query = descriptor.publication.query_context;
  const auto staged = world.handle->StageMeasuredStateSources(
      {descriptor}, clock, world.binding, std::nullopt);
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  const auto reader = staged.value()->readers().front();
  staged.value()->EnablePublication();
  const auto channel = ArmDisplayChannel(world.binding, world.clock);
  const auto bound = world.handle->BindMotionDisplayChannel(channel);
  ASSERT_TRUE(bound.ok()) << bound.error().message;
  const auto measured_stream = [&]() -> std::optional<core::StreamHealthDTO> {
    for (const auto &stream : host_component_stream_health()->Read(
             world.binding.value().controllable_component_id))
      if (stream.stream_id == "measured_joints") return stream;
    return std::nullopt;
  };
  ASSERT_TRUE(measured_stream().has_value());
  EXPECT_FALSE(measured_stream()->age_ns.has_value());

  auto vendor = world.handle->node()->create_publisher<sensor_msgs::msg::JointState>(
      *device->vendor_topic, rclcpp::QoS(1));
  world.manager.SetHardwareActive(false);
  world.handle->StartBackend();
  ASSERT_TRUE(fake::WaitUntil([&] { return vendor->get_subscription_count() == 1U; }));
  sensor_msgs::msg::JointState message;
  message.header.stamp.sec = 2;
  message.name = {"other", "source_joint_1"};
  message.position = {9.0, 0.25};

  // Gate closed: the display follows, the stream is not fed.
  vendor->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] { return device->vendor_gate_calls->load() >= 1; }));
  ASSERT_TRUE(fake::WaitUntil([&] { return channel->current_bound_render_state().ok(); }));
  EXPECT_EQ(channel->current_bound_render_state().value().positions_si,
            std::vector<double>{0.25});
  EXPECT_FALSE(measured_stream()->age_ns.has_value());

  // Gate open: the display follows, marked display-only, and the stream is live.
  device->vendor_gate_open->store(true);
  message.header.stamp.sec = 3;
  message.position = {9.0, 0.5};
  vendor->publish(message);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto render = channel->current_bound_render_state();
    return render.ok() && render.value().positions_si == std::vector<double>{0.5};
  }));
  const auto render = channel->current_bound_render_state().value();
  EXPECT_TRUE(render.display_only);
  EXPECT_EQ(render.compiled_joint_names, std::vector<std::string>{"joint_1"});
  EXPECT_EQ(render.positions_si, std::vector<double>{0.5});
  EXPECT_TRUE(measured_stream()->age_ns.has_value());

  // The addressed state is untouched: no broadcaster sample, no publication.
  EXPECT_FALSE(reader->current_state(query, 1000).ok());
  EXPECT_FALSE(channel->current_state(1000).ok());
}

// M6 (approval list 2, item 34): poses follow the live controller stream.
// While the adapter's gate admits the vendor samples (the robot moved from
// its own interface, or ManyForge's own re-arm is in progress), the latest
// valid one is a jog/Move-to preview's start state. Once the gate refuses
// (armed, broadcaster fresh), previews start from the addressed state again.
// Planning for Execute never reads it.
TEST(RetainedRos2ControlRuntimeHandleTest,
     AMovedArmsPreviewStartsFromTheVendorJointTopic) {
  ArmWorld world;
  auto device = std::make_shared<FakeDeviceStatusAdapter>();
  device->Set(ReadyDeviceStatus());
  device->vendor_topic = world.prefix + "/xarm/joint_states";
  world.handle->InstallDeviceStatusAdapter(device);
  // The arm display channel as the backend factory builds it: commanded
  // (the binding's authority) over the retained model. A predicted source,
  // or no model projector for a model-backed binding, is refused at
  // construction.
  const auto display = ArmDisplayChannel(world.binding, world.clock);
  ASSERT_TRUE(world.handle->BindMotionDisplayChannel(display).ok());
  const std::vector<double> addressed{0.1}, velocity{0.0};
  ASSERT_TRUE(display->publish_state(addressed, velocity, 100, 100).applied());
  world.manager.SetHardwareActive(true);
  world.handle->StartBackend();
  const auto live_at = [&](const double expected) {
    const auto live = display->live_preview_start(101);
    return live.has_value() && live->positions_si == std::vector<double>{expected};
  };
  FakeJointStates vendor{world.prefix + "/xarm/joint_states"};

  // Idle and armed (the gate refuses): a normal preview is unchanged.
  vendor.Publish(0.3);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto render = display->current_bound_render_state();
    return render.ok() && render.value().positions_si == std::vector<double>{0.3};
  }));
  EXPECT_FALSE(display->live_preview_start(101).has_value());

  // Moved from the robot's own interface: the preview starts from the
  // latest valid vendor sample; the all-zero e-stop signature is ignored.
  device->vendor_gate_open->store(true);
  vendor.Publish(0.35);
  ASSERT_TRUE(fake::WaitUntil([&] { return live_at(0.35); }));
  vendor.Publish(0.0);
  std::this_thread::sleep_for(200ms);
  EXPECT_TRUE(live_at(0.35));
  // Items 23/34: the control-state and observation reads serve it too.
  const auto presented = display->current_live_bound_state(101);
  ASSERT_TRUE(presented.ok());
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(presented.value().state.positions[0]).value_rad,
      0.35);

  // Re-armed and rebound (the gate refuses again): the addressed state.
  device->vendor_gate_open->store(false);
  vendor.Publish(0.4);
  ASSERT_TRUE(fake::WaitUntil([&] { return !display->live_preview_start(101).has_value(); }));
  vendor.Pause();
  EXPECT_DOUBLE_EQ(std::get<core::AngularPositionDTO>(
                       display->current_live_bound_state(101).value().state.positions[0])
                       .value_rad,
                   0.1);
  EXPECT_DOUBLE_EQ(
      std::get<core::AngularPositionDTO>(display->current_state(101).value().positions[0])
          .value_rad,
      0.1);
}

} // namespace
} // namespace manyforge::roboplan_backend
