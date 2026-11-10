#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <manyforge_core/time/fake_clock.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "host_component_runtime.hpp"
#include "direct_control_target.hpp"

namespace manyforge::roboplan_backend::host_detail {
namespace {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentActivity;
using core::ComponentLifecycle;
using core::ComponentState;

/// Completes every action at once with the configured outcome and moves its
/// lifecycle like a device would.
class InstantAdapter final : public core::IComponentAdapter {
 public:
  ComponentState State(std::int64_t now_ns) const override {
    std::lock_guard lock(mutex_);
    ComponentState state;
    state.lifecycle = lifecycle_;
    state.activity = activity_;
    if (lifecycle_ == ComponentLifecycle::kFault)
      state.fault = core::Fault{core::FaultCode::kEstop, "E-stop", true,
                                core::FaultOperatorAction::kEnable, std::nullopt,
                                std::nullopt, {}, 0};
    state.observed_at_ns = now_ns;
    return state;
  }
  core::ActionResult Run(ComponentAction action, std::stop_token) override {
    std::lock_guard lock(mutex_);
    core::ActionResult result;
    result.outcome = outcome_;
    if (outcome_ == ActionOutcome::kSucceeded)
      lifecycle_ = action == ComponentAction::kEnable ? ComponentLifecycle::kEnabled
                                                      : ComponentLifecycle::kPrepared;
    return result;
  }
  std::vector<core::Capability> capabilities() const override { return {}; }

  void set(ComponentLifecycle lifecycle, ComponentActivity activity) {
    std::lock_guard lock(mutex_);
    lifecycle_ = lifecycle;
    activity_ = activity;
  }

 private:
  mutable std::mutex mutex_;
  ComponentLifecycle lifecycle_{ComponentLifecycle::kNotPrepared};
  ComponentActivity activity_{ComponentActivity::kIdle};
  ActionOutcome outcome_{ActionOutcome::kSucceeded};
};

struct Recorder final {
  std::mutex mutex;
  std::vector<std::vector<ComponentState>> observed;
  std::vector<std::string> opened;
  int published{};
  bool refuse_routes{};
};

HostComponentRuntime::Hooks HooksFor(Recorder &recorder) {
  return {
      [&](std::vector<ComponentState> states) {
        std::lock_guard lock(recorder.mutex);
        recorder.observed.push_back(std::move(states));
      },
      [&](const std::string &component_id) -> core::Result<void> {
        std::lock_guard lock(recorder.mutex);
        recorder.opened.push_back(component_id);
        if (recorder.refuse_routes)
          return core::Result<void>::Err({"route_refused", "routes refused", {}});
        return core::Result<void>::Ok();
      },
      [&] {
        std::lock_guard lock(recorder.mutex);
        ++recorder.published;
      }, {}, {}, {}, {}, {}, {}, {}, {}};
}

void WaitForLastAction(HostComponentRuntime &runtime, const std::string &component_id) {
  for (int attempt = 0; attempt < 2000; ++attempt) {
    runtime.Refresh();
    for (const auto &state : runtime.Read().states)
      if (state.component_id == component_id && state.last_action && !state.action) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "action of " << component_id << " never finished";
}

TEST(HostComponentRuntime, FeedsTheLatchWithEveryComponentOnEveryChange) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  auto io = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}, {"io", io}}));

  robot->set(ComponentLifecycle::kFault, ComponentActivity::kIdle);
  runtime.Refresh();
  runtime.Refresh();  // unchanged: no second publication

  std::lock_guard lock(recorder.mutex);
  ASSERT_EQ(recorder.observed.size(), 2U);
  const auto &latest = recorder.observed.back();
  ASSERT_EQ(latest.size(), 2U);
  EXPECT_EQ(latest[0].component_id, "robot");
  EXPECT_EQ(latest[0].lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(latest[1].component_id, "io");
  EXPECT_EQ(recorder.published, 2);
  EXPECT_EQ(runtime.Read().revision, 2U);
}

TEST(HostComponentRuntime, RemovedComponentsLeaveThePublication) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  auto io = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}, {"io", io}}));
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));

  const auto publication = runtime.Read();
  ASSERT_EQ(publication.states.size(), 1U);
  EXPECT_EQ(publication.states.front().component_id, "robot");
  std::lock_guard lock(recorder.mutex);
  EXPECT_EQ(recorder.observed.back().size(), 1U);
}

TEST(HostComponentRuntime, EnableOpensRoutesBeforeReportingSuccess) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));

  ASSERT_TRUE(runtime.Submit("robot", ComponentAction::kEnable).ok());
  WaitForLastAction(runtime, "robot");

  const auto state = runtime.Read().states.front();
  EXPECT_EQ(state.last_action->outcome, ActionOutcome::kSucceeded);
  std::lock_guard lock(recorder.mutex);
  EXPECT_EQ(recorder.opened, std::vector<std::string>{"robot"});
}

TEST(HostComponentRuntime, RefusedRoutesFailTheEnable) {
  core::FakeClock clock;
  Recorder recorder;
  recorder.refuse_routes = true;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));

  ASSERT_TRUE(runtime.Submit("robot", ComponentAction::kEnable).ok());
  WaitForLastAction(runtime, "robot");

  const auto state = runtime.Read().states.front();
  EXPECT_EQ(state.last_action->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(state.last_action->error.has_value());
  EXPECT_EQ(state.last_action->error->code, "route_refused");
}

TEST(HostComponentRuntime, PrepareOpensNoRoutes) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto io = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"io", io}}));

  ASSERT_TRUE(runtime.Submit("io", ComponentAction::kPrepare).ok());
  WaitForLastAction(runtime, "io");

  std::lock_guard lock(recorder.mutex);
  EXPECT_TRUE(recorder.opened.empty());
}

TEST(HostComponentRuntime, RetainedAdapterKeepsItsLastAction) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));
  ASSERT_TRUE(runtime.Submit("robot", ComponentAction::kPrepare).ok());
  WaitForLastAction(runtime, "robot");

  static_cast<void>(
      runtime.SetComponents({{"robot", robot}, {"io", std::make_shared<InstantAdapter>()}}));

  const auto publication = runtime.Read();
  ASSERT_EQ(publication.states.size(), 2U);
  ASSERT_TRUE(publication.states.front().last_action.has_value());
  EXPECT_EQ(publication.states.front().last_action->action, ComponentAction::kPrepare);
}

TEST(HostComponentRuntime, AnOwnerKeepsItsOwnerFlag) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  static_cast<void>(runtime.SetComponents(
      {{"lite6", std::make_shared<InstantAdapter>(), std::nullopt, std::nullopt, true}}));
  EXPECT_EQ(runtime.Submit("lite6", ComponentAction::kEnable).error().code,
            "component_action_unsupported");
}

TEST(HostComponentRuntime, SharedTransportRecoveryUsesTheRecordedMemberAction) {
  core::FakeClock clock;
  Recorder recorder;
  auto owner = std::make_shared<InstantAdapter>();
  auto member = std::make_shared<InstantAdapter>();
  owner->set(ComponentLifecycle::kFault, ComponentActivity::kUnknown);
  member->set(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  std::atomic<bool> faulted{true};
  std::atomic<int> calls{0};
  auto hooks = HooksFor(recorder);
  hooks.shared_transport_fault = [&](const std::string &id) -> std::optional<core::Fault> {
    if (id != "io" || !faulted.load()) return std::nullopt;
    return core::Fault{core::FaultCode::kProcessExited,
                       "ROS transport executor thread failed: test (owner /manyforge_generic_io)",
                       true, core::FaultOperatorAction::kRecover, std::nullopt,
                       std::nullopt, {}, 0};
  };
  hooks.prepare_shared_transport_recovery = [&](const std::string &id)
      -> core::IComponentAdapter::PreparedRecovery {
    if (id != "io" || !faulted.load()) return {};
    return [&](std::stop_token) {
      ++calls;
      owner->set(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
      faulted.store(false);
      core::ActionResult result;
      result.outcome = ActionOutcome::kSucceeded;
      return result;
    };
  };
  HostComponentRuntime runtime(clock, std::move(hooks));
  static_cast<void>(runtime.SetComponents({{"machine", owner, std::nullopt, std::nullopt, true},
                                           {"io", member, "machine", "io"}}));
  ASSERT_TRUE(runtime.Submit("io", ComponentAction::kRecover).ok());
  WaitForLastAction(runtime, "io");
  const auto states = runtime.Read().states;
  ASSERT_EQ(states.size(), 2U);
  ASSERT_TRUE(states[1].last_action);
  EXPECT_EQ(states[1].last_action->action, ComponentAction::kRecover);
  EXPECT_EQ(states[1].last_action->outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(states[1].lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(calls.load(), 1);
}

TEST(HostComponentRuntime, SharedAdapterRecoveryPinsTheAddressedAdapterAtAcceptance) {
  core::FakeClock clock;
  Recorder recorder;
  auto adapter = std::make_shared<InstantAdapter>();
  adapter->set(ComponentLifecycle::kFault, ComponentActivity::kUnknown);
  std::atomic<bool> captured{false};
  auto hooks = HooksFor(recorder);
  hooks.prepare_adapter_transport_recovery =
      [&](const std::string &id, const std::shared_ptr<core::IComponentAdapter> &pinned)
          -> core::IComponentAdapter::PreparedRecovery {
    if (id != "sensor") return {};
    captured.store(pinned == adapter);
    return [pinned](std::stop_token stop) {
      return pinned->Run(ComponentAction::kRecover, stop);
    };
  };
  HostComponentRuntime runtime(clock, std::move(hooks));
  static_cast<void>(runtime.SetComponents({{"sensor", adapter, std::nullopt, std::nullopt, true}}));
  ASSERT_TRUE(runtime.Submit("sensor", ComponentAction::kRecover).ok());
  WaitForLastAction(runtime, "sensor");
  EXPECT_TRUE(captured.load());
}

TEST(HostComponentRuntime, SharedTransportFaultIsPublishedWithUnknownActivity) {
  core::FakeClock clock;
  Recorder recorder;
  auto hooks = HooksFor(recorder);
  hooks.shared_transport_fault = [](const std::string &id) -> std::optional<core::Fault> {
    if (id != "io") return std::nullopt;
    return core::Fault{core::FaultCode::kProcessExited,
                       "ROS transport executor thread failed: test (owner /manyforge_generic_io)",
                       true, core::FaultOperatorAction::kRecover, std::nullopt,
                       std::nullopt, {}, 0};
  };
  HostComponentRuntime runtime(clock, std::move(hooks));
  auto io = std::make_shared<InstantAdapter>();
  io->set(ComponentLifecycle::kEnabled, ComponentActivity::kIdle);
  static_cast<void>(runtime.SetComponents({{"io", io}}));
  const auto states = runtime.Read().states;
  ASSERT_EQ(states.size(), 1U);
  EXPECT_EQ(states[0].lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(states[0].activity, ComponentActivity::kUnknown);
  ASSERT_TRUE(states[0].fault);
  EXPECT_EQ(states[0].fault->code, core::FaultCode::kProcessExited);
  EXPECT_NE(states[0].fault->message.find("/manyforge_generic_io"), std::string::npos);
}

TEST(HostComponentRuntime, BusyTransportRefusesBeforeOpeningRoutes) {
  core::FakeClock clock;
  Recorder recorder;
  auto hooks = HooksFor(recorder);
  hooks.enter_transport_effect = [](const std::string &, bool)
      -> core::Result<std::shared_ptr<void>> {
    return core::Result<std::shared_ptr<void>>::Err(
        {"component_action_in_progress", "transport recovery in progress", {}});
  };
  HostComponentRuntime runtime(clock, std::move(hooks));
  auto io = std::make_shared<InstantAdapter>();
  io->set(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  static_cast<void>(runtime.SetComponents({{"io", io}}));
  ASSERT_TRUE(runtime.Submit("io", ComponentAction::kEnable).ok());
  WaitForLastAction(runtime, "io");
  const auto states = runtime.Read().states;
  ASSERT_EQ(states.size(), 1U);
  ASSERT_TRUE(states[0].last_action);
  EXPECT_EQ(states[0].last_action->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(states[0].last_action->error);
  EXPECT_EQ(states[0].last_action->error->code, "component_action_in_progress");
  EXPECT_EQ(states[0].lifecycle, ComponentLifecycle::kPrepared);
  std::lock_guard lock(recorder.mutex);
  EXPECT_TRUE(recorder.opened.empty());
}

TEST(HostComponentRuntime, EffectLookupPinsExactAdapterAndItsTransportDependency) {
  core::FakeClock clock;
  Recorder recorder;
  auto hooks = HooksFor(recorder);
  bool uses_transport = true;
  hooks.uses_shared_transport = [&](const std::string &) { return uses_transport; };
  hooks.uses_adapter_transport = [&](const std::string &) { return uses_transport; };
  HostComponentRuntime runtime(clock, std::move(hooks));
  auto first = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"io", first}}));
  auto pinned = runtime.AdapterForEffect("io");
  ASSERT_EQ(pinned.adapter, first);
  EXPECT_TRUE(pinned.uses_shared_transport);
  EXPECT_TRUE(pinned.uses_adapter_transport);
  uses_transport = false;
  auto second = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"io", second}}));
  EXPECT_EQ(pinned.adapter, first);
  EXPECT_TRUE(pinned.uses_shared_transport);
  EXPECT_TRUE(pinned.uses_adapter_transport);
  const auto current = runtime.AdapterForEffect("io");
  EXPECT_EQ(current.adapter, second);
  EXPECT_FALSE(current.uses_shared_transport);
  EXPECT_FALSE(current.uses_adapter_transport);
}

TEST(HostComponentRuntime, RemovedComponentsActionsAreHandedBack) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));
  ASSERT_TRUE(runtime.Submit("robot", ComponentAction::kPrepare).ok());
  // The caller joins them once it released the composition lock.
  auto removed = runtime.SetComponents({});
  EXPECT_EQ(removed.size(), 1U);
  removed.clear();
  EXPECT_TRUE(runtime.Read().states.empty());
}

TEST(HostComponentRuntime, AdapterLookupReturnsTheComponentsOwnAdapter) {
  core::FakeClock clock;
  Recorder recorder;
  HostComponentRuntime runtime(clock, HooksFor(recorder));
  auto robot = std::make_shared<InstantAdapter>();
  auto io = std::make_shared<InstantAdapter>();
  static_cast<void>(runtime.SetComponents({{"robot", robot}, {"io", io}}));

  // The output write reaches the adapter itself, not the
  // route-opening wrapper the coordinator runs.
  EXPECT_EQ(runtime.Adapter("io"), io);
  EXPECT_EQ(runtime.Adapter("robot"), robot);
  EXPECT_EQ(runtime.Adapter("plc"), nullptr);
  static_cast<void>(runtime.SetComponents({{"robot", robot}}));
  EXPECT_EQ(runtime.Adapter("io"), nullptr);
}

/// One control binding of `component` (only its id and component matter).
core::ControlBindingDTO BindingOf(const std::string &binding, const std::string &component) {
  const std::string digest = "sha256:" + std::string(64U, '0');
  return core::ControlBindingDTO{
      core::ControlBindingId{binding},
      core::ControlBindingRevision{digest},
      core::ControllableComponentId{component},
      core::CommandTargetId{"target"},
      core::CapabilityId{"capability"},
      core::CapabilityDefinitionRevision{digest},
      core::ControlDomainId{"domain." + component},
      {},
      core::ControlInterfaceContractId{"interface"},
      core::ControlInterfaceRevision{digest},
      core::ControllerTopologyId{"topology"},
      core::TopologyRevision{digest},
      {},
      {},
      core::ProviderInstanceId{"provider"},
      core::RuntimeUnitId{"unit"},
      core::ProviderManifestId{"manifest"},
      core::ProviderManifestRevision{digest},
      core::StateAuthorityId{"authority"},
      core::StateAuthorityClass::kMeasured,
      core::StopEvidencePolicyId{"policy"},
      core::StopEvidencePolicyRevision{digest},
      core::ContainmentGranularity::kControllerSet,
      {},
  };
}

core::DesiredControlConfigurationDTO CompiledRouteOf(const std::string &binding) {
  const std::string digest = "sha256:" + std::string(64U, '0');
  return core::DesiredControlConfigurationDTO{
      core::ControlDomainId{"domain"},
      core::ControlDomainRevision{digest},
      core::ControllerTopologyId{"topology"},
      core::TopologyRevision{digest},
      core::ActivationSetId{"activation"},
      {{core::ControlBindingId{binding}, core::ControlBindingRevision{digest},
        core::ControlRouteId{"route"}}}};
}

TEST(HostComponentRuntime, OnlyComponentsWithCompiledRoutesHaveRoutesToOpen) {
  // `open_routes` of a component without compiled routes
  // (a gripper-only cell's I/O, a sensor, an owner) has nothing to open; a
  // component with one opens it whether or not the cell has an arm.
  const std::vector bindings{BindingOf("binding.gripper", "gripper"),
                             BindingOf("binding.io", "io")};
  const std::vector routes{CompiledRouteOf("binding.gripper")};
  EXPECT_TRUE(HasCompiledComponentRoutes(routes, bindings, core::ControllableComponentId{"gripper"}));
  EXPECT_FALSE(HasCompiledComponentRoutes(routes, bindings, core::ControllableComponentId{"io"}));
  EXPECT_FALSE(HasCompiledComponentRoutes(routes, bindings, core::ControllableComponentId{"sensor"}));
  EXPECT_FALSE(HasCompiledComponentRoutes({}, bindings, core::ControllableComponentId{"gripper"}));
}

TEST(DirectControlTarget, RetainsMountedToolAndSelectsExactRobot) {
  const core::ProgramTargetReferenceDTO robot{core::ControllableComponentId{"robot"}, core::CommandTargetId{"arm"}};
  const core::ProgramTargetReferenceDTO tool{core::ControllableComponentId{"tool"}, core::CommandTargetId{"gripper"}};
  const core::ProgramCapabilityBindingDTO motion{core::CapabilityId{"execution.trajectory.execute"}, core::CapabilityDefinitionRevision{"r1"}, {}, {}, {}};
  const core::ProgramCapabilityBindingDTO gripper{core::CapabilityId{"tool.gripper.command"}, core::CapabilityDefinitionRevision{"r2"}, {}, {}, {}};
  std::vector<core::ProgramTargetBindingDTO> rows{
      {tool, {}, {}, {}, {}, {gripper}, {}, {}, {}, {}},
      {robot, {}, {}, {}, {}, {motion}, {}, {}, {}, {}}};
  const auto selected = SelectDirectControlTarget(rows, robot, "execution.trajectory.execute");
  ASSERT_EQ(selected.status, DirectControlTargetStatus::kFound);
  EXPECT_EQ(selected.target->target, robot);
  EXPECT_EQ(rows.front().target, tool);
  const auto selected_tool = SelectDirectControlTarget(rows, tool, "tool.gripper.command");
  ASSERT_EQ(selected_tool.status, DirectControlTargetStatus::kFound);
  EXPECT_EQ(selected_tool.target->target, tool);
}

TEST(DirectControlTarget, RefusesMissingOrWrongCapability) {
  const core::ProgramTargetReferenceDTO robot{core::ControllableComponentId{"robot"}, core::CommandTargetId{"arm"}};
  std::vector<core::ProgramTargetBindingDTO> rows{{robot, {}, {}, {}, {}, {}, {}, {}, {}, {}}};
  const core::ProgramTargetReferenceDTO absent{core::ControllableComponentId{"other"}, core::CommandTargetId{"arm"}};
  EXPECT_EQ(SelectDirectControlTarget(rows, absent, "execution.trajectory.execute").status,
            DirectControlTargetStatus::kMissing);
  EXPECT_EQ(SelectDirectControlTarget(rows, robot, "execution.trajectory.execute").status,
            DirectControlTargetStatus::kCapabilityMissing);
}

TEST(DirectControlTarget, RefusesDuplicateExactTarget) {
  const core::ProgramTargetReferenceDTO robot{core::ControllableComponentId{"robot"}, core::CommandTargetId{"arm"}};
  std::vector<core::ProgramTargetBindingDTO> rows{
      {robot, {}, {}, {}, {}, {}, {}, {}, {}, {}},
      {robot, {}, {}, {}, {}, {}, {}, {}, {}, {}}};
  EXPECT_EQ(SelectDirectControlTarget(rows, robot, "execution.trajectory.execute").status,
            DirectControlTargetStatus::kAmbiguous);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
