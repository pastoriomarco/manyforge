// ROS runtime handle and Isaac guard binding against in-process stand-ins of
// the controller manager, the trajectory controller and the Isaac-side guard
// (design R6/§7.5 CCLC activation, §3.1 binding epochs, §4.3/Q41/Q43/Q46).
#include "ros2_control_fake_controller.hpp"

#include <gtest/gtest.h>

#include <builtin_interfaces/msg/time.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <diagnostic_msgs/msg/key_value.hpp>
#include <pal_statistics_msgs/msg/statistics.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_isaac_arm_guard_binding.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace fake = manyforge::ros2_control_execution::fake_controller;
using namespace std::chrono_literals;

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
  explicit ArmWorld(const std::string &action_controller = "arm_controller",
                    core::RuntimeParticipantBinding initial_binding = ArmBinding())
      : binding(std::move(initial_binding)),
        prefix("/manyforge_handle_test_" + fake::unique_suffix()),
        jtc(prefix + "/arm_controller", {"source_joint_1"}),
        manager(prefix + "/controller_manager", "arm_controller", "TestSystem", &jtc) {
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
    const auto bound = handle->Bind(
        std::make_unique<ros2_control_execution::Ros2TransportResources>(
            "manyforge_handle_test_" + fake::unique_suffix(), "/", 3U,
            std::make_shared<NullSink>(), "arm", clock),
        clock, std::move(arm),
        [this](std::shared_ptr<core::IExecutionPort> port) {
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

  WallClock clock;
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
      std::make_shared<RetainedRos2ControlRuntimeHandle>();
};

using State = core::ComponentLifecycleStateDTO;
using Outcome = RetainedRos2ControlSwitchOutcome;

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
  auto peer = std::make_unique<ArmWorld>("arm_controller", peer_binding.value());
  const auto port = survivor.handle->current_port();
  const auto participant = survivor.handle->participant();
  const auto readiness = port->ReadReadiness();

  // Replace the peer occurrence inside the same shared runtime unit. This
  // must not rewrite the survivor's component/provider epoch or live port.
  auto retired_peer = std::move(peer);
  const auto replaced_peer_port = retired_peer->handle->current_port();
  peer = std::make_unique<ArmWorld>("arm_controller", peer_binding.value());
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
  peer = std::make_unique<ArmWorld>("arm_controller", peer_binding.value());
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
      R"snapshot({"context":{"bundle_id":"bundle.1","component_id":"component.arm","generator_identity":null,"graph_revision":"graph.1","launch_identity":"sha256:0000000000000000000000000000000000000000000000000000000000000000","request_context":"Apply=apply.7","runtime_epoch":"epoch.1","runtime_unit_id":"robot_cell.ros2_control_execution"},"core_dump_link":null,"exit":{"core_dumped":false,"observed_utc":"2026-10-11T18:00:00Z","signal":11,"status":null},"generated":{"controllers_sha256":"sha256:2222222222222222222222222222222222222222222222222222222222222222","robot_description_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111"},"output":{"dropped_forward_bytes":0,"dropped_forward_lines":0,"invalid_text_bytes":0,"line_count":1,"tail":"controller crashed\n","truncated":false},"readiness":{"arm_controller_active":null,"binding_epoch":null,"captured_utc":null,"controller_bound":null,"controller_loaded":null,"controller_manager_present":null,"controller_observed":null,"hardware_ready":null,"joint_sample_available":null,"known":null,"sample_age_ns":null},"settings":{"argv":["ros2","launch"],"arm_controller_name":"arm","controller_profile":"ur5","controllers_yaml":"arm: effective settings","frame_prefix":"","gripper_controller_name":null,"retained_configuration_json":"{}","runtime_namespace":"/robot"}})snapshot"));
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
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  {
    std::lock_guard lock(mutex);
    exit_reason = "controller_launch_exited: controller launch was terminated by signal 9";
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
        return core::Result<void>::Err({"execution_rejected", "no Isaac joint states",
                                        {{"reason", "isaac_joint_states_not_received"}}});
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
  ASSERT_TRUE(fake::WaitUntil([&] { return world.handle->arm_activity().controller_loaded; }));
  ASSERT_EQ(world.handle->PrepareArmController().outcome, Outcome::kSucceeded);
  // Refused before effect: no switch request reaches the controller manager.
  const auto refused = world.handle->ActivateArmController({});
  EXPECT_EQ(refused.outcome, Outcome::kNotSent);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->details.at("reason"), "isaac_joint_states_not_received");
  EXPECT_EQ(world.manager.switch_requests(), 0U);
  {
    std::lock_guard lock(mutex);
    allow_activation = true;
  }
  ASSERT_EQ(world.handle->ActivateArmController({}).outcome, Outcome::kSucceeded);
  ASSERT_TRUE(world.WaitForState(State::kActive));
  const auto port = world.handle->current_port();
  ASSERT_TRUE(fake::WaitUntil([&] { return port->ReadReadiness().controller_bound; }));

  // Q43: no motion command passes while the guard does not admit it.
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

// --- Isaac guard binding ------------------------------------------------------

/// Scripted arm facts and a record of the binding's effects, in order.
class FakeArmHost final : public IRetainedIsaacArmHost {
public:
  IsaacArmGuardFacts ReadFacts() override {
    std::lock_guard lock(mutex_);
    return facts_;
  }
  std::vector<std::string> ControllerJointNames() override { return {"joint1", "joint2"}; }
  void ReportFault(core::ErrorDTO error) override {
    std::lock_guard lock(mutex_);
    effects_.push_back("fault:" + error.details.at("guard_reason"));
  }
  void DeactivateForFault(std::stop_token) override {
    std::lock_guard lock(mutex_);
    effects_.push_back("deactivate");
  }

  /// JTC active and holding on `epoch`, sample stamped `stamp`.
  void Hold(std::uint64_t epoch, core::TimePoint stamp, bool executing = false) {
    std::lock_guard lock(mutex_);
    facts_.readiness = {true, true, true, core::ExecutionBindingEpoch(epoch)};
    core::ExecutionControllerSampleDTO sample;
    sample.binding_epoch = core::ExecutionBindingEpoch(epoch);
    sample.source_stamp_ns = stamp;
    sample.reference_velocities_rad_s = {0.0, executing ? 0.3 : 0.0};
    facts_.sample = sample;
    facts_.goal_executing = executing;
  }

  [[nodiscard]] std::vector<std::string> effects() const {
    std::lock_guard lock(mutex_);
    return effects_;
  }

private:
  mutable std::mutex mutex_;
  IsaacArmGuardFacts facts_;
  std::vector<std::string> effects_;
};

/// The Isaac-side guard's ROS surface: status publisher (reliable,
/// transient-local), authorization subscriber, joint states publisher.
class FakeGuard final : public fake::PeerNode {
public:
  FakeGuard(const std::string &command_topic, const std::string &joint_states)
      : PeerNode("manyforge_fake_isaac_guard") {
    status_ = node()->create_publisher<diagnostic_msgs::msg::DiagnosticStatus>(
        command_topic + "/guard_status", rclcpp::QoS(1).reliable().transient_local());
    authorization_ = node()->create_subscription<builtin_interfaces::msg::Time>(
        command_topic + "/authorization", rclcpp::QoS(1).reliable(),
        [this](const builtin_interfaces::msg::Time::ConstSharedPtr message) {
          std::lock_guard lock(mutex_);
          authorizations_.push_back(static_cast<core::TimePoint>(message->sec) * 1'000'000'000 +
                                    message->nanosec);
        });
    joint_states_ =
        node()->create_publisher<sensor_msgs::msg::JointState>(joint_states, rclcpp::QoS(10));
    Start();
  }

  void Publish(const std::string &state, std::optional<core::TimePoint> after,
               const std::string &joint_names = R"(["joint1","joint2"])",
               std::optional<std::string> reason = std::nullopt) {
    diagnostic_msgs::msg::DiagnosticStatus message;
    message.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;  // never decisive
    const auto value = [&](std::string key, std::string text) {
      diagnostic_msgs::msg::KeyValue row;
      row.key = std::move(key);
      row.value = std::move(text);
      message.values.push_back(row);
    };
    value("state", state);
    value("joint_names", joint_names);
    if (after) {
      value("authorized_after.sec", std::to_string(*after / 1'000'000'000));
      value("authorized_after.nanosec", std::to_string(*after % 1'000'000'000));
    }
    if (reason) value("reason", *reason);
    status_->publish(message);
  }

  void PublishJointState() { joint_states_->publish(sensor_msgs::msg::JointState{}); }

  [[nodiscard]] std::vector<core::TimePoint> authorizations() const {
    std::lock_guard lock(mutex_);
    return authorizations_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<core::TimePoint> authorizations_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticStatus>::SharedPtr status_;
  rclcpp::Subscription<builtin_interfaces::msg::Time>::SharedPtr authorization_;
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr joint_states_;
};

/// The binding on its own ROS node.
struct GuardWorld final {
  GuardWorld()
      : command_topic("/manyforge_guard_test_" + fake::unique_suffix() + "/isaac_joint_commands"),
        joint_states(command_topic + "_states"), node("manyforge_guard_binding_test") {
    node.Start();
    binding = RetainedIsaacArmGuardBinding::Create(
        {command_topic, joint_states, 250'000'000, 5'000'000}, node.node(), host, clock);
    hooks = binding->hooks();
  }
  ~GuardWorld() {
    hooks = {};
    binding.reset();
    node.Stop();
  }

  [[nodiscard]] bool Admitted() const { return hooks.motion_admitted(); }

  /// The guard's authorization subscription is matched (a volatile
  /// authorization sent before that would be lost).
  [[nodiscard]] bool GuardListening() const {
    return fake::WaitUntil(
        [&] { return node.node()->count_subscribers(command_topic + "/authorization") == 1U; });
  }

  std::string command_topic;
  std::string joint_states;
  WallClock clock;
  std::shared_ptr<FakeArmHost> host = std::make_shared<FakeArmHost>();
  fake::PeerNode node;
  std::shared_ptr<RetainedIsaacArmGuardBinding> binding;
  RetainedRos2ControlArmGuardHooks hooks;
};

TEST(RetainedIsaacArmGuardBindingTest, ParsesTheGuardStatusValuesOnly) {
  diagnostic_msgs::msg::DiagnosticStatus message;
  message.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
  const auto add = [&](std::string key, std::string value) {
    diagnostic_msgs::msg::KeyValue row;
    row.key = std::move(key);
    row.value = std::move(value);
    message.values.push_back(row);
  };
  add("state", "open");
  add("joint_names", R"(["joint1","joint\"2"])");
  add("authorized_after.sec", "12");
  add("authorized_after.nanosec", "345");
  auto parsed = RetainedIsaacArmGuardBinding::ParseStatus(message);
  ASSERT_TRUE(parsed.status.has_value());
  EXPECT_EQ(parsed.status->state, IsaacGuardState::kOpen);
  EXPECT_EQ(parsed.status->authorized_after_ns, std::optional<core::TimePoint>(12'000'000'345));
  ASSERT_TRUE(parsed.joint_names.has_value());
  EXPECT_EQ(*parsed.joint_names, (std::vector<std::string>{"joint1", "joint\"2"}));

  // Half an authorization stamp is malformed; an unknown state too.
  message.values.pop_back();
  EXPECT_FALSE(RetainedIsaacArmGuardBinding::ParseStatus(message).status.has_value());
  message.values.front().value = "maybe";
  EXPECT_FALSE(RetainedIsaacArmGuardBinding::ParseStatus(message).status.has_value());
  // Joint names that are not one compact JSON string array are unparsable.
  for (const char *bad : {"joint1,joint2", R"(["joint1",])", R"(["a"] )", R"(["\u0041"])"}) {
    diagnostic_msgs::msg::DiagnosticStatus names;
    diagnostic_msgs::msg::KeyValue row;
    row.key = "joint_names";
    row.value = bad;
    names.values.push_back(row);
    EXPECT_FALSE(RetainedIsaacArmGuardBinding::ParseStatus(names).joint_names.has_value()) << bad;
  }
}

TEST(RetainedIsaacArmGuardBindingTest,
     AuthorizesTheHoldAndAdmitsMotionOnlyOnTheExactConfirmationWithMatchingJoints) {
  GuardWorld world;
  FakeGuard guard(world.command_topic, world.joint_states);
  ASSERT_TRUE(world.GuardListening());
  world.host->Hold(1U, 1'000'000'000);
  // No activation, authorization or motion before real Isaac joint states.
  EXPECT_FALSE(world.hooks.before_activation().ok());
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(guard.authorizations().empty());
  ASSERT_TRUE(fake::WaitUntil([&] {
    guard.PublishJointState();
    return world.binding->status().activation_allowed;
  }));
  EXPECT_TRUE(world.hooks.before_activation().ok());
  // The hold's own controller stamp is the authorization.
  ASSERT_TRUE(fake::WaitUntil([&] { return !guard.authorizations().empty(); }));
  EXPECT_EQ(guard.authorizations().front(), 1'000'000'000);
  EXPECT_FALSE(world.Admitted());
  // A stale confirmation does not admit; the exact one does.
  guard.Publish("open", 999'000'000);
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(world.Admitted());
  guard.Publish("open", 1'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
  // Joint names in another order block motion until a matching status.
  guard.Publish("open", 1'000'000'000, R"(["joint2","joint1"])");
  ASSERT_TRUE(fake::WaitUntil([&] { return !world.Admitted(); }));
  const auto status = world.binding->status();
  EXPECT_TRUE(status.joint_names_mismatch);
  EXPECT_EQ(status.last_diagnostic,
            std::optional<std::string>("isaac_guard_joint_names_differ_from_controller_order"));
  guard.Publish("open", 1'000'000'000, "not json");
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(world.Admitted());
  guard.Publish("open", 1'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
  EXPECT_EQ(guard.authorizations().size(), 1U);  // one outstanding authorization
}

TEST(RetainedIsaacArmGuardBindingTest, RestartedGuardIsReconnectedBeforeItsStatusIsTrusted) {
  GuardWorld world;
  world.host->Hold(1U, 1'000'000'000);
  {
    FakeGuard first(world.command_topic, world.joint_states);
    ASSERT_TRUE(world.GuardListening());
    ASSERT_TRUE(fake::WaitUntil([&] {
      first.PublishJointState();
      return !first.authorizations().empty();
    }));
    first.Publish("open", first.authorizations().front());
    ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
  }
  ASSERT_TRUE(fake::WaitUntil([&] {
    return world.node.node()->count_subscribers(world.command_topic + "/authorization") == 0U;
  }));
  // A restarted guard (new publisher) replaying the old confirmation is not
  // trusted: the reconnect handling requires a fresh, newer authorization.
  FakeGuard restarted(world.command_topic, world.joint_states);
  ASSERT_TRUE(world.GuardListening());
  restarted.Publish("open", 1'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.binding->status().guard_reconnects == 1U; }));
  EXPECT_FALSE(world.Admitted());
  world.host->Hold(1U, 2'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto values = restarted.authorizations();
    return !values.empty() && values.back() == 2'000'000'000;
  }));
  EXPECT_FALSE(world.Admitted());
  restarted.Publish("open", 2'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
}

TEST(RetainedIsaacArmGuardBindingTest, TripReportsTheFaultThenSwitchesOffAndWaitsForRecover) {
  GuardWorld world;
  FakeGuard guard(world.command_topic, world.joint_states);
  ASSERT_TRUE(world.GuardListening());
  world.host->Hold(3U, 1'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] {
    guard.PublishJointState();
    return !guard.authorizations().empty();
  }));
  guard.Publish("open", 1'000'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
  world.host->Hold(3U, 1'100'000'000, true);  // a goal executes
  std::this_thread::sleep_for(20ms);
  guard.Publish("tripped", 1'000'000'000, R"(["joint1","joint2"])", "no fresh frame");
  ASSERT_TRUE(fake::WaitUntil([&] { return world.host->effects().size() == 2U; }));
  // Item 44 before Q41's switch-off.
  EXPECT_EQ(world.host->effects(),
            (std::vector<std::string>{"fault:no fresh frame", "deactivate"}));
  EXPECT_FALSE(world.Admitted());
  EXPECT_EQ(world.hooks.fault(), std::optional<std::string>("no fresh frame"));
  EXPECT_FALSE(world.hooks.before_activation().ok());
  // A late open status neither clears the incident nor admits motion.
  guard.Publish("open", 1'000'000'000);
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(world.Admitted());
  EXPECT_TRUE(world.binding->status().tripped);

  // The operator's Recover, then CCLC's reactivation on a new epoch: the
  // hold of the old epoch is never authorized, the new epoch's is.
  world.hooks.on_recover();
  EXPECT_FALSE(world.hooks.fault().has_value());
  EXPECT_TRUE(world.hooks.before_activation().ok());
  world.host->Hold(3U, 1'200'000'000);  // still the tripped epoch
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(guard.authorizations().size(), 1U);
  world.host->Hold(4U, 1'300'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto values = guard.authorizations();
    return values.size() == 2U && values.back() == 1'300'000'000;
  }));
  guard.Publish("open", 1'300'000'000);
  ASSERT_TRUE(fake::WaitUntil([&] { return world.Admitted(); }));
}

// --- Q54: controller statistics monitor (observed statistics only) -------------

[[nodiscard]] pal_statistics_msgs::msg::Statistics Statistics(
    const std::vector<std::pair<std::string, double>> &values) {
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

} // namespace
} // namespace manyforge::roboplan_backend
