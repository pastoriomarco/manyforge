#include <gtest/gtest.h>

#include <manyforge_roboplan/passive_component_adapter.hpp>

#include <atomic>

namespace manyforge::roboplan_backend {
namespace {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentActivity;
using core::ComponentLifecycle;

// The lifecycle actions succeed at
// once, so a lane-free tool can be enabled; no streams, no capabilities.
TEST(PassiveComponentAdapter, LifecycleActionsSucceedAtOnce) {
  PassiveComponentAdapter adapter;
  EXPECT_EQ(adapter.State(5).lifecycle, ComponentLifecycle::kNotPrepared);
  EXPECT_EQ(adapter.State(5).observed_at_ns, 5);
  EXPECT_EQ(adapter.Run(ComponentAction::kPrepare, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(6).lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(adapter.Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(7).lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_EQ(adapter.Run(ComponentAction::kDisable, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(8).lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(adapter.Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.Run(ComponentAction::kRecover, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(9).lifecycle, ComponentLifecycle::kPrepared);
  const auto state = adapter.State(10);
  EXPECT_TRUE(state.streams.empty());
  EXPECT_FALSE(state.fault.has_value());
  EXPECT_TRUE(adapter.capabilities().empty());
}

TEST(PassiveComponentAdapter, OffersNoOtherAction) {
  PassiveComponentAdapter adapter;
  for (const auto action : {ComponentAction::kResetPosition, ComponentAction::kPowerOff,
                            ComponentAction::kReboot}) {
    const auto result = adapter.Run(action, {});
    EXPECT_EQ(result.outcome, ActionOutcome::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, "component_action_unsupported");
  }
  EXPECT_EQ(adapter.State(1).lifecycle, ComponentLifecycle::kNotPrepared);
}

// Activity follows the component's own actions (e.g. a pneumatic open/close).
TEST(PassiveComponentAdapter, ActivityFollowsItsOwnActions) {
  std::atomic<bool> running{false};
  PassiveComponentAdapter adapter([&] { return running.load(); });
  EXPECT_EQ(adapter.State(1).activity, ComponentActivity::kIdle);
  running = true;
  EXPECT_EQ(adapter.State(2).activity, ComponentActivity::kBusy);
  running = false;
  EXPECT_EQ(adapter.State(3).activity, ComponentActivity::kIdle);
  EXPECT_EQ(PassiveComponentAdapter().State(4).activity, ComponentActivity::kIdle);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
