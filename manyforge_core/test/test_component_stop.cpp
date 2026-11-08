#include <gtest/gtest.h>
#include <algorithm>
#include <manyforge_core/types/component_state.hpp>

namespace manyforge::core {
namespace {
ComponentState Owner() {
  ComponentState owner;
  owner.component_id = "machine";
  owner.lifecycle = ComponentLifecycle::kPrepared;
  owner.activity = ComponentActivity::kIdle;
  return owner;
}
ComponentStop Estop() {
  return {StopCode::kEstop, "E-stop pressed or not yet cleared (C2)",
          "C2", "Emergency stop", 42};
}
ComponentState Member() {
  ComponentState member;
  member.component_id = "arm";
  member.owner_id = "machine";
  member.role = "arm";
  member.lifecycle = ComponentLifecycle::kEnabled;
  member.activity = ComponentActivity::kBusy;
  return member;
}
TEST(ComponentStopTest, StopIsUnhealthyAndOwnerIsNotUsable) {
  auto owner = Owner();
  ASSERT_TRUE(component_healthy(owner));
  ASSERT_TRUE(owner_usable(owner));
  owner.stop = Estop();
  EXPECT_EQ(owner.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_FALSE(component_healthy(owner));
  EXPECT_FALSE(owner_usable(owner));
  EXPECT_EQ(to_string(owner.stop->code), "estop");
  EXPECT_EQ(parse_stop_code("estop"), StopCode::kEstop);
  EXPECT_FALSE(parse_stop_code("fault_estop"));
}
TEST(ComponentStopTest, StoppedOwnerProjectsMemberWithoutInventingIdle) {
  auto owner = Owner();
  owner.stop = Estop();
  const auto member = presented_member_state(Member(), owner);
  EXPECT_EQ(member.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(member.activity, ComponentActivity::kBusy);
  ASSERT_TRUE(member.reason);
  EXPECT_EQ(member.reason->code, kStoppedByEstopReason);
  EXPECT_FALSE(member.fault);
  EXPECT_FALSE(member.stop);
  owner.lifecycle = ComponentLifecycle::kNotPrepared;
  const auto before_prepare = presented_member_state(Member(), owner);
  EXPECT_EQ(before_prepare.lifecycle, ComponentLifecycle::kPrepared);
  ASSERT_TRUE(before_prepare.reason);
  EXPECT_EQ(before_prepare.reason->code, kStoppedByEstopReason);
}
TEST(ComponentStopTest, StoppedOwnerDoesNotEraseIndependentFailures) {
  auto owner = Owner();
  owner.stop = Estop();
  auto member = Member();
  member.lifecycle = ComponentLifecycle::kFault;
  member.fault = Fault{FaultCode::kDriveFault, "power cycle", false,
                       FaultOperatorAction::kNone, {}, {}, {}, 1};
  EXPECT_EQ(presented_member_state(member, owner), member);
  owner.lifecycle = ComponentLifecycle::kOffline;
  EXPECT_EQ(presented_member_state(member, owner), member);
  member = Member();
  const auto projected = presented_member_state(member, owner);
  EXPECT_EQ(projected.lifecycle, ComponentLifecycle::kOffline);
  ASSERT_TRUE(projected.reason);
  EXPECT_EQ(projected.reason->code, kOwnerUnavailableReason);
}
TEST(ComponentStopTest, LatchRetainsStopAndIndependentFault) {
  auto owner = Owner();
  owner.stop = Estop();
  auto member = Member();
  member.lifecycle = ComponentLifecycle::kFault;
  member.fault = Fault{FaultCode::kDriveFault, "power cycle", false,
                       FaultOperatorAction::kNone, {}, {}, {}, 1};
  const auto causes = cell_latch_causes({member, owner});
  ASSERT_EQ(causes.size(), 2U);
  const auto stop = std::find_if(causes.begin(), causes.end(), [](const auto& cause) {
    return cause.kind == CellLatchCauseKind::kStop;
  });
  ASSERT_NE(stop, causes.end());
  EXPECT_EQ(stop->component_id, "machine");
  ASSERT_TRUE(stop->stop);
  EXPECT_EQ(*stop->stop, *owner.stop);
  EXPECT_FALSE(stop->fault);
  EXPECT_EQ(to_string(stop->kind), "stop");
  EXPECT_EQ(parse_cell_latch_cause_kind("stop"), CellLatchCauseKind::kStop);
  EXPECT_TRUE(std::any_of(causes.begin(), causes.end(), [](const auto& cause) {
    return cause.component_id == "arm" && cause.kind == CellLatchCauseKind::kFault;
  }));
  owner.stop.reset();
  EXPECT_TRUE(cell_latch_causes({owner}).empty());
}
TEST(ComponentStopTest, SimulatorRemainsExcludedFromLatch) {
  auto simulator = Owner();
  simulator.capabilities.push_back(Capability::kSimReset);
  simulator.stop = Estop();
  EXPECT_TRUE(latch_exempt(simulator, {simulator}));
  EXPECT_TRUE(cell_latch_causes({simulator}).empty());
}
TEST(ComponentStopTest, StopDoesNotHideUnknownActivityOrIndependentOwnerFault) {
  auto owner = Owner();
  owner.stop = Estop();
  owner.activity = ComponentActivity::kUnknown;
  auto causes = cell_latch_causes({owner});
  ASSERT_EQ(causes.size(), 2U);
  EXPECT_EQ(causes[0].kind, CellLatchCauseKind::kStop);
  EXPECT_EQ(causes[1].kind, CellLatchCauseKind::kActivityUnknown);
  owner.lifecycle = ComponentLifecycle::kFault;
  owner.fault = Fault{FaultCode::kDriveFault, "power cycle", false,
                      FaultOperatorAction::kNone, {}, {}, {}, 1};
  causes = cell_latch_causes({owner});
  ASSERT_EQ(causes.size(), 2U);
  EXPECT_EQ(causes[0].kind, CellLatchCauseKind::kStop);
  EXPECT_EQ(causes[1].kind, CellLatchCauseKind::kFault);
}
}  // namespace
}  // namespace manyforge::core
