#include <gtest/gtest.h>

#include <chrono>
#include <manyforge_core/time/fake_clock.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "host_component_runtime.hpp"

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
      }};
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

  // CONTRACTS §7: the output write reaches the adapter itself, not the
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
  // CONTRACTS §6, §15: `open_routes` of a component without compiled routes
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

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
