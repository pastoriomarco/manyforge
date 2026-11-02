// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The xArm arm member's action sequences against a fake driver and a fake
// ros2_control lane, and its state overlay.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_xarm_control/xarm_arm_adapter.hpp>

#include "xarm_fakes.hpp"

namespace manyforge::xarm_control {
namespace {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentLifecycle;
using testing::FakeDriver;
using testing::FakeLane;
using testing::FakeStatus;
using testing::report;

struct World final {
  World() {
    clock.set(10'000'000'000LL);
    status->Set(report(clock.now()));
    adapter = std::make_unique<XArmArmAdapter>(
        XArmArmAdapterConfig{6, kXArmJointAgreementToleranceRad, std::chrono::seconds(10)},
        XArmArmAdapterPorts{status, driver, lane, Hardware(), [this] { return hardware_on; },
                            [this] {
                              // One report period: the next report arrives.
                              clock.advance(kXArmReportPeriodNs);
                              if (on_pause) on_pause();
                              if (!reporting) return;
                              auto next = status->observe();
                              next.received_timestamp_ns = clock.now();
                              status->Set(next);
                            },
                            [this] { return measured; }, &clock,
                            [this] { return broadcaster; }});
  }
  [[nodiscard]] core::ActionResult Run(const ComponentAction action) {
    return adapter->Run(action, std::stop_token{});
  }
  /// The hardware switch: records "on"/"off", answers `hardware_outcome`;
  /// switched on, the plugin arms the robot (a ready report, motors on)
  /// when `arms_robot`.
  [[nodiscard]] std::function<core::ActionResult(bool, std::stop_token)> Hardware() {
    return [this](const bool active, std::stop_token) {
      hardware.push_back(active ? "on" : "off");
      core::ActionResult made;
      made.action = active ? ComponentAction::kEnable : ComponentAction::kDisable;
      made.outcome = hardware_outcome;
      if (hardware_outcome != ActionOutcome::kSucceeded) {
        made.error = core::ActionError{"ros2_control_hardware_deactivation_refused",
                                       "refused", {}, {}};
        return made;
      }
      hardware_on = active;
      if (active && arms_robot) status->Set(report(clock.now()));
      if (on_hardware) on_hardware(active);
      return made;
    };
  }

  core::FakeClock clock;
  std::shared_ptr<FakeStatus> status = std::make_shared<FakeStatus>();
  std::shared_ptr<FakeDriver> driver = std::make_shared<FakeDriver>();
  std::shared_ptr<FakeLane> lane = std::make_shared<FakeLane>();
  std::optional<std::vector<double>> measured{std::vector<double>{0.1, -0.2, 0.3, 0.0, 0.5, 0.6}};
  std::vector<std::string> hardware;
  ActionOutcome hardware_outcome{ActionOutcome::kSucceeded};
  /// A fresh process: the launch starts the hardware inactive.
  bool hardware_on{false};
  bool arms_robot{true};
  /// The controller manager's activity shows the broadcaster active.
  bool broadcaster{true};
  /// The controller keeps reporting.
  bool reporting{true};
  /// The plugin's own controller switching, after a hardware switch and on
  /// each pause.
  std::function<void(bool)> on_hardware;
  std::function<void()> on_pause;
  std::unique_ptr<XArmArmAdapter> adapter;
};

/// The plugin's controller cycle after an arming (live 2026-11-02, Real
/// Lite 6): every controller off at once (the broadcaster's samples stop);
/// `reactivates` after `cycle_ns`: all back on, the trajectory controller too.
void CycleControllersAfterTheArming(World &world, const bool reactivates,
                                    const std::int64_t cycle_ns = 2'000'000'000LL) {
  auto until = std::make_shared<std::optional<std::int64_t>>();
  world.on_hardware = [&world, until, cycle_ns](const bool active) {
    if (!active || world.hardware.size() > 2U) return;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                  1'000'000'000LL}};
    *until = world.clock.now() + cycle_ns;
  };
  world.on_pause = [&world, until, reactivates] {
    if (!*until || world.clock.now() < **until) return;
    until->reset();
    if (!reactivates) return;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
    world.lane->state.lifecycle = ComponentLifecycle::kEnabled;
  };
}

using Actions = std::vector<ComponentAction>;
using Ids = std::vector<std::string>;

// XPW-1: Prepare and Recover leave the motors off: the lane's action (no
// hardware switch), the hardware off, a fresh report; no driver call (the
// owner clears errors), never the hardware on (the plugin's motion_enable).
TEST(XArmArmAdapter, PrepareAndRecoverKeepTheMotorsOffAndPowerIsTheMachines) {
  World world;
  // Connected, error cleared, motors off (state 4).
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kNotPrepared);
  const auto prepared = world.Run(ComponentAction::kPrepare);
  EXPECT_EQ(prepared.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(prepared.action, ComponentAction::kPrepare);
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kPrepare});
  EXPECT_EQ(world.hardware, Ids{"off"});
  EXPECT_TRUE(world.driver->Calls().empty());
  EXPECT_FALSE(world.hardware_on);
  // The lane shows the hardware off as not prepared: prepared, motors off.
  auto state = world.adapter->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.activity, core::ComponentActivity::kIdle);
  EXPECT_FALSE(state.fault.has_value());

  world.lane->actions.clear();
  world.hardware.clear();
  const auto recovered = world.Run(ComponentAction::kRecover);
  EXPECT_EQ(recovered.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(recovered.action, ComponentAction::kRecover);
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kRecover});
  EXPECT_EQ(world.hardware, Ids{"off"});
  EXPECT_TRUE(world.driver->Calls().empty());
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);

  EXPECT_TRUE(world.adapter->capabilities().empty());
  EXPECT_EQ(world.Run(ComponentAction::kPowerOff).error->code, "component_action_unsupported");
  EXPECT_EQ(world.Run(ComponentAction::kReboot).error->code, "component_action_unsupported");
}

TEST(XArmArmAdapter, RecoverOfAnEnabledArmSwitchesTheHardwareOff) {
  World world;
  world.hardware_on = true;
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  EXPECT_EQ(world.Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kRecover});
  EXPECT_EQ(world.hardware, Ids{"off"});
  EXPECT_FALSE(world.hardware_on);
}

TEST(XArmArmAdapter, PrepareFailsWithoutAStatusReportOrWithTheHardwareRefused) {
  World world;
  world.reporting = false;
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  const auto started = world.clock.now();
  const auto silent = world.Run(ComponentAction::kPrepare);
  EXPECT_EQ(silent.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(silent.action, ComponentAction::kPrepare);
  EXPECT_EQ(silent.error->code, "xarm_status_unavailable");
  EXPECT_GE(world.clock.now() - started, 10'000'000'000LL);
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kNotPrepared);

  // A refused hardware switch stops the sequence, reported as the action.
  World refused;
  refused.hardware_outcome = ActionOutcome::kFailed;
  const auto failed = refused.Run(ComponentAction::kRecover);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.action, ComponentAction::kRecover);
  EXPECT_EQ(refused.lane->actions, Actions{ComponentAction::kRecover});
}

TEST(XArmArmAdapter, StateIsTheLanesControllerErrorsAreTheOwnersAndAllMotorsOffIsIdle) {
  World world;
  auto state = world.adapter->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.activity, core::ComponentActivity::kBusy);
  EXPECT_FALSE(state.fault.has_value());

  // An e-stop: C2, motors off: the owner's fault; nothing can be executing.
  world.status->Set(report(world.clock.now(), 2, 0, 4, 0));
  state = world.adapter->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_FALSE(state.fault.has_value());
  EXPECT_EQ(state.activity, core::ComponentActivity::kIdle);

  // A stale report is the owner's lost link, not the arm's.
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  state = world.adapter->State(world.clock.now());
  EXPECT_FALSE(state.fault.has_value());
  EXPECT_EQ(state.activity, core::ComponentActivity::kBusy);
}

// M7's rule: armed, settled, error-free, out of joint-servo mode or out of
// the moving and ready states. The owner reports it; the arm's state doesn't.
TEST(XArmArmAdapter, ExternalControlFollowsM7sRuleAndTheOwnerReleasesIt) {
  World world;
  // Prepared, the motors off: not armed, a Studio jog is not external control.
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.clock.advance(kXArmOwnOperationSettleNs + 1);
  auto at = world.clock.now();
  EXPECT_FALSE(world.adapter->ExternalControl(report(at, 0, 0, kXArmConfigChangedState), at));

  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  // While Enable's own changes settle (no armed report after it yet, at
  // most kXArmOwnOperationSettleNs), a changed mode is not external control.
  auto observed = report(world.clock.now() + 1, 0, 0, kXArmConfigChangedState);
  world.status->Set(observed);
  EXPECT_FALSE(world.adapter->ExternalControl(observed, world.clock.now()));
  world.clock.advance(kXArmOwnOperationSettleNs + 1);
  at = world.clock.now();
  EXPECT_TRUE(world.adapter->ExternalControl(report(at, 0, 0, kXArmConfigChangedState), at));
  // A Studio pause, stop or stop request in joint-servo mode.
  for (const int state : {3, 4, 6})
    EXPECT_TRUE(world.adapter->ExternalControl(report(at, 0, kXArmJointServoMode, state), at))
        << state;
  EXPECT_FALSE(world.adapter->ExternalControl(report(at, 0, kXArmJointServoMode, 1), at));
  EXPECT_FALSE(world.adapter->ExternalControl(report(at), at));
  world.status->Set(report(at, 0, 0, kXArmConfigChangedState));
  EXPECT_NE(world.adapter->State(at).lifecycle, ComponentLifecycle::kFault);
  // The owner's Recover releases it: no longer armed until a re-arm.
  world.adapter->ReleaseExternalControl(at);
  EXPECT_FALSE(world.adapter->ExternalControl(report(at, 0, 0, kXArmConfigChangedState), at));

  // An e-stop (C2) explains its pair and disarms the arm: once the owner
  // cleared it, the stopped robot is not external control; Enable re-arms.
  World estop;
  ASSERT_EQ(estop.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  estop.clock.advance(kXArmOwnOperationSettleNs + 1);
  const auto then = estop.clock.now();
  EXPECT_FALSE(estop.adapter->ExternalControl(report(then, 2, kXArmJointServoMode, 4, 0), then));
  EXPECT_FALSE(estop.adapter->ExternalControl(report(then, 0, kXArmJointServoMode, 4, 0), then));
}

// XPW-1: Enable switches the motors on: the arm controller off, the hardware
// on (the plugin's motion_enable, set_mode(1), set_state(0)), fresh joints,
// the gate, then the controller.
TEST(XArmArmAdapter, EnableFromPreparedSwitchesTheHardwareOnThenTheController) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(enabled.action, ComponentAction::kEnable);
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
  EXPECT_EQ(world.hardware, (Ids{"off", "on"}));
  EXPECT_TRUE(world.hardware_on);
  EXPECT_TRUE(world.driver->Calls().empty());
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
}

TEST(XArmArmAdapter, EnableOnARobotNotReadyRunsTheFullArmingSequenceAsM7) {
  World world;
  world.hardware_on = true;
  // Taken over (mode 0, state 5), or C2 cleared by the owner (motors off).
  for (const auto &not_ready : {report(world.clock.now(), 0, 0, kXArmConfigChangedState),
                                report(world.clock.now(), 0, 0, 4, 0)}) {
    world.lane->actions.clear();
    world.hardware.clear();
    world.status->Set(not_ready);
    const auto enabled = world.Run(ComponentAction::kEnable);
    EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
    EXPECT_EQ(enabled.action, ComponentAction::kEnable);
    EXPECT_EQ(world.lane->actions,
              (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
    EXPECT_EQ(world.hardware, (Ids{"off", "on"}));
    EXPECT_TRUE(world.driver->Calls().empty());
  }
}

// As M7: after the arming Enable continues once the joint state broadcaster
// delivers a sample stamped after it, bounded by the lifecycle timeout.
TEST(XArmArmAdapter, EnablesArmingWaitsForJointStatesWithinTheLifecycleTimeout) {
  World world;
  // The broadcaster doesn't deliver after the hardware is switched on.
  world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                100'000'000'000LL}};
  const auto started = world.clock.now();
  const auto failed = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.action, ComponentAction::kEnable);
  EXPECT_EQ(failed.error->code, "xarm_joint_angles_unavailable");
  EXPECT_GE(world.clock.now() - started, 10'000'000'000LL);
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});

  // Samples flowing (stamped after the arming): enabled.
  world.lane->actions.clear();
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
  EXPECT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
}

// Live 2026-11-02 (Real Lite 6): no ManyForge switch overlaps the plugin's
// controller cycle; its reactivation switched the trajectory controller on
// without an Enable: off again, then the Enable.
TEST(XArmArmAdapter, EnableWaitsOutThePluginsControllerCycle) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  CycleControllersAfterTheArming(world, true);
  const auto started = world.clock.now();
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_GE(world.clock.now() - started, 2'000'000'000LL);
  EXPECT_EQ(world.hardware, (Ids{"off", "on"}));
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kDisable,
                                          ComponentAction::kEnable}));
  const auto state = world.adapter->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kFresh);
}

// Live 2026-11-02: the plugin's reactivation failed (its list_controllers
// timed out): the broadcaster is requested once, then the Enable.
TEST(XArmArmAdapter, EnableRequestsTheBroadcasterWhenThePluginsReactivationFailed) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  CycleControllersAfterTheArming(world, false);
  const auto on_hardware = world.on_hardware;
  world.on_hardware = [&world, on_hardware](const bool active) {
    on_hardware(active);
    // The repeated request with the hardware on: the broadcaster on.
    if (active && world.hardware.size() == 3U)
      world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
  };
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.hardware, (Ids{"off", "on", "on"}));
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
}

TEST(XArmArmAdapter, EnableFailsTypedWhenTheRobotNeverReportsReadyAfterTheArming) {
  World world;
  world.arms_robot = false;
  world.status->Set(report(world.clock.now(), 0, 0, kXArmConfigChangedState));
  const auto started = world.clock.now();
  const auto failed = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.action, ComponentAction::kEnable);
  EXPECT_EQ(failed.error->code, "xarm_not_ready");
  EXPECT_EQ(failed.error->vendor_code, "C0 mode=0 state=5");
  EXPECT_GE(world.clock.now() - started, 10'000'000'000LL);
  EXPECT_LT(world.clock.now() - started, 10'000'000'000LL + 2 * kXArmReportPeriodNs);
  EXPECT_EQ(world.hardware, (Ids{"off", "on"}));
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
}

TEST(XArmArmAdapter, DisableSwitchesTheControllerThenTheHardwareOffAndStaysPrepared) {
  World world;
  world.hardware_on = true;
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  const auto disabled = world.Run(ComponentAction::kDisable);
  EXPECT_EQ(disabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(disabled.action, ComponentAction::kDisable);
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
  EXPECT_EQ(world.hardware, Ids{"off"});
  EXPECT_TRUE(world.driver->Calls().empty());
  // The lane shows the hardware off as not prepared; Enable re-arms it.
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  // A failed controller switch leaves the hardware alone.
  world.hardware.clear();
  world.lane->outcome = ActionOutcome::kFailed;
  EXPECT_EQ(world.Run(ComponentAction::kDisable).outcome, ActionOutcome::kFailed);
  EXPECT_TRUE(world.hardware.empty());
}

TEST(XArmArmAdapter, ItsPrepareDoesNotSurviveTheProcess) {
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  ASSERT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  // The process went away: a fresh process isn't prepared.
  world.lane->state.lifecycle = ComponentLifecycle::kOffline;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kOffline);
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kNotPrepared);

  // The same after the lane reported its process exited.
  ASSERT_EQ(world.Run(ComponentAction::kDisable).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  world.lane->state.lifecycle = ComponentLifecycle::kFault;
  world.lane->state.fault = core::Fault{core::FaultCode::kProcessExited, "exited", true,
                                        core::FaultOperatorAction::kRecover, std::nullopt,
                                        std::nullopt, {}, 0};
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
  world.lane->state.fault.reset();
  world.lane->state.lifecycle = ComponentLifecycle::kNotPrepared;
  EXPECT_EQ(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kNotPrepared);
}

TEST(XArmArmAdapter, EnableWithTheHardwareOnAndTheRobotReadyOnlyEnablesTheLane) {
  World world;
  world.hardware_on = true;
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_TRUE(world.driver->Calls().empty());
  EXPECT_TRUE(world.hardware.empty());
  EXPECT_EQ(world.lane->actions, std::vector<ComponentAction>{ComponentAction::kEnable});
}

TEST(XArmArmAdapter, EnableGateRefusesStaleZeroOrDisagreeingJoints) {
  World world;
  world.hardware_on = true;
  world.measured.reset();
  auto refused = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(refused.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(refused.error->code, "xarm_joint_angles_unavailable");
  // The plugin's zero fallback against the true pose.
  world.measured = std::vector<double>(6, 0.0);
  refused = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(refused.error->code, "xarm_measured_joints_disagree");
  EXPECT_TRUE(world.lane->actions.empty());
  // Not ready after its arming: refused.
  world.measured = std::vector<double>{0.1, -0.2, 0.3, 0.0, 0.5, 0.6};
  world.arms_robot = false;
  world.status->Set(report(world.clock.now(), 0, 1, 4, 0));
  refused = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(refused.error->code, "xarm_not_ready");
  EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  world.status->Set(report(world.clock.now() - kXArmStatusFreshnessNs - 1));
  refused = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(refused.error->code, "xarm_status_unavailable");
}

// XPW-1: with the hardware off (prepared, motors off) the driver's own joint
// topic is the measured stream, e-stop included; with it on, display only.
TEST(XArmArmAdapter, VendorJointSamplesLeadWhileTheHardwareIsOffAndNonPosesAreIgnored) {
  World world;
  const auto now = world.clock.now();
  EXPECT_EQ(world.adapter->JointSampleUse(now, std::vector<double>(6, 0.0)),
            XArmJointSampleUse::kIgnore);
  EXPECT_EQ(world.adapter->JointSampleUse(now, {}), XArmJointSampleUse::kIgnore);
  EXPECT_EQ(world.adapter->JointSampleUse(now, {0.1, std::nan(""), 0, 0, 0, 0}),
            XArmJointSampleUse::kIgnore);
  EXPECT_EQ(world.adapter->JointSampleUse(now, {0.1, 0, 0, 0, 0, 0}),
            XArmJointSampleUse::kLeads);
  world.status->Set(report(now, 2, 0, 4, 0));
  EXPECT_EQ(world.adapter->JointSampleUse(now, {0.1, 0, 0, 0, 0, 0}),
            XArmJointSampleUse::kLeads);
  world.hardware_on = true;
  EXPECT_EQ(world.adapter->JointSampleUse(now, {0.1, 0, 0, 0, 0, 0}),
            XArmJointSampleUse::kDisplay);
}

TEST(XArmArmAdapter, ToolOffsetRearmsAndItsOwnStateFiveIsNotExternalControl) {
  World world;
  world.hardware_on = true;
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.clock.advance(kXArmReportPeriodNs * 2);
  // The controller reports ready again after the re-arm (fresh report).
  world.adapter = std::make_unique<XArmArmAdapter>(
      XArmArmAdapterConfig{6, kXArmJointAgreementToleranceRad, std::chrono::seconds(10)},
      XArmArmAdapterPorts{world.status, world.driver, world.lane, world.Hardware(),
                          [&world] { return world.hardware_on; },
                          [&world] {
                            world.clock.advance(kXArmReportPeriodNs);
                            world.status->Set(report(world.clock.now()));
                          },
                          [&world] { return world.measured; }, &world.clock});
  const auto applied = world.adapter->ApplyToolContext(
      core::Pose3DTO{{0, 0, 0}, {0, 0, 0, 1}}, core::ToolContextPointMassDTO{0.5, {0, 0, 0}},
      false);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_TRUE(applied.value());
  EXPECT_EQ(world.driver->Calls(),
            (std::vector<std::string>{"set_tcp_offset", "set_state(0)", "set_tcp_load"}));
  EXPECT_NE(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
}

// Review 2026-11-02: the joint states come back before the controller
// manager's activity reports the plugin's reactivation; the trajectory
// controller state is read only once the activity shows the broadcaster
// active again, so the controller the plugin switched on is switched off.
TEST(XArmArmAdapter, EnableReadsTheTrajectoryControllerOnlyAfterTheActivityShowsTheBroadcaster) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  auto until = std::make_shared<std::optional<std::int64_t>>();
  auto lag = std::make_shared<int>(-1);
  world.on_hardware = [&world, until](const bool active) {
    if (!active) return;
    world.broadcaster = false;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                  1'000'000'000LL}};
    *until = world.clock.now() + 2'000'000'000LL;
  };
  world.on_pause = [&world, until, lag] {
    if (*until && world.clock.now() >= **until) {
      until->reset();
      // The joints flow at once; the activity sample lags two reads.
      world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
      *lag = 2;
      return;
    }
    if (*lag > 0 && --*lag == 0) {
      world.broadcaster = true;
      world.lane->state.lifecycle = ComponentLifecycle::kEnabled;
    }
  };
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kDisable,
                                          ComponentAction::kEnable}));
}

/// An adapter on `world` whose pause is one report period with a fresh
/// ready report carrying `offset_mm` once the driver took `set_tcp_offset`,
/// and the plugin's controller cycle after it: the broadcaster and its
/// joint states off at the first pause after the send, back after `cycle_ns`
/// when `reactivates`.
std::unique_ptr<XArmArmAdapter> OffsetAdapter(World &world, const double offset_mm,
                                              const bool reactivates,
                                              const std::int64_t cycle_ns = 2'000'000'000LL) {
  auto until = std::make_shared<std::optional<std::int64_t>>();
  return std::make_unique<XArmArmAdapter>(
      XArmArmAdapterConfig{6, kXArmJointAgreementToleranceRad, std::chrono::seconds(10)},
      XArmArmAdapterPorts{world.status, world.driver, world.lane, world.Hardware(),
                          [&world] { return world.hardware_on; },
                          [&world, until, offset_mm, reactivates, cycle_ns] {
                            world.clock.advance(kXArmReportPeriodNs);
                            auto next = report(world.clock.now());
                            const auto calls = world.driver->Calls();
                            const bool sent = std::find(calls.begin(), calls.end(),
                                                        "set_tcp_offset") != calls.end();
                            if (sent) next.tcp_offset = {offset_mm, 0, 0, 0, 0, 0};
                            world.status->Set(next);
                            if (!sent) return;
                            if (!*until && world.broadcaster) {
                              world.broadcaster = false;
                              world.lane->state.streams = {{"measured_joints",
                                                            core::StreamFreshness::kStale,
                                                            1'000'000'000LL}};
                              *until = world.clock.now() + cycle_ns;
                            } else if (*until && reactivates && world.clock.now() >= **until) {
                              world.broadcaster = true;
                              world.lane->state.streams = {
                                  {"measured_joints", core::StreamFreshness::kFresh, 0}};
                            }
                          },
                          [&world] { return world.measured; }, &world.clock,
                          [&world] { return world.broadcaster; }});
}

// Review 2026-11-02: at Enable the host switches the trajectory controller
// on right after the hook; a sent TCP offset made the plugin cycle every
// controller off and on, so the hook returns only once that cycle is over.
TEST(XArmArmAdapter, AnOffsetSentAtEnableWaitsOutThePluginsControllerCycle) {
  World world;
  world.hardware_on = true;
  auto adapter = OffsetAdapter(world, 10.0, true);
  const auto started = world.clock.now();
  const auto applied = adapter->ApplyToolContext(core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}},
                                                 core::ToolContextPointMassDTO{0.5, {0, 0, 0}},
                                                 true);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_TRUE(applied.value());
  EXPECT_TRUE(world.broadcaster);
  EXPECT_GE(world.clock.now() - started, 2'000'000'000LL);
  EXPECT_EQ(world.driver->Calls(),
            (std::vector<std::string>{"set_tcp_offset", "set_state(0)", "set_tcp_load"}));

  // Outside Enable the host rebinds after the re-arm itself: no wait.
  World rest;
  rest.hardware_on = true;
  auto at_rest = OffsetAdapter(rest, 10.0, true);
  const auto rest_started = rest.clock.now();
  const auto sent =
      at_rest->ApplyToolContext(core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}}, std::nullopt, false);
  ASSERT_TRUE(sent.ok()) << sent.error().message;
  EXPECT_LT(rest.clock.now() - rest_started, 2'000'000'000LL);

  // No offset to send (the report already shows it): nothing to wait for.
  World same;
  same.hardware_on = true;
  auto unchanged = OffsetAdapter(same, 0.0, false);
  const auto kept = unchanged->ApplyToolContext(core::Pose3DTO{{0, 0, 0}, {0, 0, 0, 1}},
                                                std::nullopt, true);
  ASSERT_TRUE(kept.ok());
  EXPECT_FALSE(kept.value());
}

TEST(XArmArmAdapter, AnOffsetSentAtEnableFailsTypedWhenTheBroadcasterNeverComesBack) {
  World world;
  world.hardware_on = true;
  auto adapter = OffsetAdapter(world, 10.0, false);
  const auto failed = adapter->ApplyToolContext(core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}},
                                                std::nullopt, true);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().details.at("effect"), "observed");
  EXPECT_EQ(failed.error().details.at("reason"), "xarm_reengage_not_ready");

  // The step's own deadline bounds the wait.
  World bounded;
  bounded.hardware_on = true;
  auto in_step = OffsetAdapter(bounded, 10.0, false);
  std::stop_source stop;
  stop.request_stop();
  const auto stopped = in_step->ApplyToolContext(core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}},
                                                 std::nullopt, true,
                                                 XArmToolContextBound{std::nullopt, stop.get_token()});
  ASSERT_FALSE(stopped.ok());
}

} // namespace
} // namespace manyforge::xarm_control
