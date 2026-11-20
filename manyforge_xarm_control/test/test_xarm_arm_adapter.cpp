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
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
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
                            [this] { return broadcaster; },
                            [this] {
                              if (on_activity) on_activity();
                              return controllers_inactive;
                            },
                            [this] { return controllers_active; },
                            [this](std::function<void()> answered) {
                              if (!switch_discovered) return false;
                              unanswered.push_back(std::move(answered));
                              return true;
                            }});
  }
  /// The controller manager answers the oldest unanswered deactivation.
  void Answer() {
    ASSERT_FALSE(unanswered.empty());
    auto answered = std::move(unanswered.front());
    unanswered.erase(unanswered.begin());
    answered();
  }
  [[nodiscard]] core::ActionResult Run(const ComponentAction action) {
    return adapter->Run(action, std::stop_token{});
  }
  /// The hardware switch: records "on"/"off" and its step deadline,
  /// answers `hardware_outcome` (with `hardware_error`); switched on, the
  /// plugin arms the robot (a ready report, motors on) when `arms_robot`.
  [[nodiscard]] std::function<core::ActionResult(
      bool, std::optional<std::chrono::steady_clock::time_point>, std::stop_token)>
  Hardware() {
    return [this](const bool active,
                  const std::optional<std::chrono::steady_clock::time_point> step_deadline,
                  std::stop_token) {
      hardware.push_back(active ? "on" : "off");
      hardware_deadlines.push_back(step_deadline);
      core::ActionResult made;
      made.action = active ? ComponentAction::kEnable : ComponentAction::kDisable;
      made.outcome = hardware_outcome;
      if (hardware_outcome != ActionOutcome::kSucceeded) {
        made.error = core::ActionError{hardware_error, "refused", {}, {}};
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
  /// The step deadline each hardware switch carried.
  std::vector<std::optional<std::chrono::steady_clock::time_point>> hardware_deadlines;
  ActionOutcome hardware_outcome{ActionOutcome::kSucceeded};
  std::string hardware_error{"ros2_control_hardware_deactivation_refused"};
  /// A fresh process: the launch starts the hardware inactive.
  bool hardware_on{false};
  bool arms_robot{true};
  /// The controller manager's activity shows the broadcaster active.
  bool broadcaster{true};
  bool controllers_inactive{false};
  std::function<void()> on_activity;
  /// The activity shows the trajectory controller or the broadcaster active.
  bool controllers_active{true};
  /// The controller manager's `switch_controller` is discovered.
  bool switch_discovered{true};
  /// The deactivations sent and not answered yet (their `answered`).
  std::vector<std::function<void()>> unanswered;
  /// The controller keeps reporting.
  bool reporting{true};
  /// Runs after each successful hardware switch, with its direction.
  std::function<void(bool)> on_hardware;
  std::function<void()> on_pause;
  std::unique_ptr<XArmArmAdapter> adapter;
};

using Actions = std::vector<ComponentAction>;
using Ids = std::vector<std::string>;

// Prepare and Recover leave the motors off: the lane's action (no
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

TEST(XArmArmAdapter, EstopFlagOnlyReplacesStaleForKnownInactiveControllers) {
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.adapter->ObserveOwnerEstop();
  world.status->Set(report(world.clock.now()));
  world.lane->state.lifecycle = ComponentLifecycle::kFault;
  world.lane->state.fault = core::Fault{core::FaultCode::kStreamStale,
      "measured stream stopped", true, core::FaultOperatorAction::kRecover,
      {}, {}, {}, world.clock.now()};

  // During the reported e-stop, only the stale stream fault is replaced.
  world.status->Set(report(world.clock.now(), 2));
  const auto during_stop = world.adapter->State(world.clock.now());
  EXPECT_FALSE(during_stop.fault);
  ASSERT_TRUE(during_stop.reason);
  EXPECT_EQ(during_stop.reason->code, "stopped_by_estop");
  world.status->Set(report(world.clock.now()));

  // Unknown or active controller activity does not explain stream silence.
  EXPECT_EQ(world.adapter->State(world.clock.now()).fault->code,
            core::FaultCode::kStreamStale);
  world.controllers_inactive = true;
  const auto stopped = world.adapter->State(world.clock.now());
  EXPECT_FALSE(stopped.fault);
  EXPECT_EQ(stopped.lifecycle, ComponentLifecycle::kPrepared);
  ASSERT_TRUE(stopped.reason);
  EXPECT_EQ(stopped.reason->code, "controllers_stopped_after_estop");

  // A different controller error must not be relabelled as a cleared e-stop.
  world.status->Set(report(world.clock.now(), 31));
  EXPECT_EQ(world.adapter->State(world.clock.now()).fault->code,
            core::FaultCode::kStreamStale);
  world.status->Set(report(world.clock.now()));

  // A new owner cannot inherit the old owner's presentation flag.
  world.adapter->ForgetOwnerEstop();
  EXPECT_EQ(world.adapter->State(world.clock.now()).fault->code,
            core::FaultCode::kStreamStale);
}

// OD-310 (live 2026-11-18, Real Lite 6): its own arming (the hardware
// switched off and on) paused `measured_joints` and the lane's
// `fault_stream_stale` latched the cell. Meanwhile it is `prepared`, reason
// `xarm_arming` (the stream still shows stale); the Enable verifies the
// stream and fails typed when it never returns, and the fault shows again.
TEST(XArmArmAdapter, ItsOwnArmingsStreamPauseIsNotAFault) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.on_hardware = [&world](const bool active) {
    if (!active) return;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                  1'000'000'000LL}};
    world.lane->state.lifecycle = ComponentLifecycle::kFault;
    world.lane->state.fault = core::Fault{core::FaultCode::kStreamStale,
        "The measured_joints stream stopped arriving", true,
        core::FaultOperatorAction::kRecover, {}, {}, {}, world.clock.now()};
  };
  std::optional<core::ComponentState> during;
  bool arming = false;
  world.on_pause = [&world, &during, &arming] {
    if (during || !world.lane->state.fault) return;
    during = world.adapter->State(world.clock.now());
    arming = world.adapter->Arming();
  };
  EXPECT_FALSE(world.adapter->Arming());
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(enabled.error->code, "xarm_joint_angles_unavailable");

  ASSERT_TRUE(during.has_value());
  EXPECT_TRUE(arming);
  EXPECT_FALSE(during->fault.has_value());
  EXPECT_EQ(during->lifecycle, ComponentLifecycle::kPrepared);
  ASSERT_TRUE(during->reason.has_value());
  EXPECT_EQ(during->reason->code, "xarm_arming");
  ASSERT_EQ(during->streams.size(), 1U);
  EXPECT_EQ(during->streams.front().freshness, core::StreamFreshness::kStale);

  // The arming ended without the stream: the lane's fault again.
  EXPECT_FALSE(world.adapter->Arming());
  const auto after = world.adapter->State(world.clock.now());
  ASSERT_TRUE(after.fault.has_value());
  EXPECT_EQ(after.fault->code, core::FaultCode::kStreamStale);
  EXPECT_EQ(after.lifecycle, ComponentLifecycle::kFault);
}

TEST(XArmArmAdapter, ArmActionsClearPriorEstopPresentation) {
  for (const auto action : {ComponentAction::kEnable, ComponentAction::kDisable,
                            ComponentAction::kRecover}) {
    World world;
    ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
    world.adapter->ObserveOwnerEstop();
    ASSERT_EQ(world.Run(action).outcome, ActionOutcome::kSucceeded);
    world.controllers_inactive = true;
    world.lane->state.lifecycle = ComponentLifecycle::kFault;
    world.lane->state.fault = core::Fault{core::FaultCode::kStreamStale,
        "measured stream stopped", true, core::FaultOperatorAction::kRecover,
        {}, {}, {}, world.clock.now()};
    EXPECT_EQ(world.adapter->State(world.clock.now()).fault->code,
              core::FaultCode::kStreamStale);
  }
}

TEST(XArmArmAdapter, OwnerReplacementDuringActivityReadKeepsStaleFault) {
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.adapter->ObserveOwnerEstop();
  world.controllers_inactive = true;
  world.lane->state.lifecycle = ComponentLifecycle::kFault;
  world.lane->state.fault = core::Fault{core::FaultCode::kStreamStale,
      "measured stream stopped", true, core::FaultOperatorAction::kRecover,
      {}, {}, {}, world.clock.now()};
  std::promise<void> entered;
  std::promise<void> resume;
  auto resume_signal = resume.get_future();
  world.on_activity = [&] { entered.set_value(); resume_signal.wait(); };
  auto state = std::async(std::launch::async, [&] {
    return world.adapter->State(world.clock.now());
  });
  entered.get_future().wait();
  world.adapter->ForgetOwnerEstop();
  resume.set_value();
  const auto result = state.get();
  ASSERT_TRUE(result.fault);
  EXPECT_EQ(result.fault->code, core::FaultCode::kStreamStale);
}

TEST(XArmArmAdapter, EstopWhileNotArmedDoesNotMaskStale) {
  World world;
  world.adapter->ObserveOwnerEstop();
  world.controllers_inactive = true;
  world.lane->state.lifecycle = ComponentLifecycle::kFault;
  world.lane->state.fault = core::Fault{core::FaultCode::kStreamStale,
      "measured stream stopped", true, core::FaultOperatorAction::kRecover,
      {}, {}, {}, world.clock.now()};
  EXPECT_EQ(world.adapter->State(world.clock.now()).fault->code,
            core::FaultCode::kStreamStale);
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

// The rule: armed, settled, error-free, out of joint-servo mode or out of
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

// Enable switches the motors on: the arm controller off, the hardware
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

// OD-214: Enable's hardware switch carries the adapter's lifecycle command
// time (the configured `action_timeout`), so a controller manager that never
// answers ends the Enable with the switch's own unknown result; Prepare and
// Disable keep each call's own cap.
TEST(XArmArmAdapter, EnablesHardwareSwitchCarriesTheLifecycleDeadlineAndEndsOnItsTimeout) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(world.hardware_deadlines.size(), 1U);
  EXPECT_FALSE(world.hardware_deadlines[0].has_value());
  world.hardware.clear();
  world.hardware_deadlines.clear();
  const auto before = std::chrono::steady_clock::now();
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  const auto after = std::chrono::steady_clock::now();
  ASSERT_EQ(world.hardware, (Ids{"off", "on"}));
  ASSERT_EQ(world.hardware_deadlines.size(), 2U);
  for (const auto &deadline : world.hardware_deadlines) {
    ASSERT_TRUE(deadline.has_value());
    EXPECT_GE(*deadline, before + std::chrono::seconds(10));
    EXPECT_LE(*deadline, after + std::chrono::seconds(10));
  }
  EXPECT_TRUE(world.hardware_deadlines[0] == world.hardware_deadlines[1]);  // one step deadline
  // Disable: the hardware off with no step deadline, as before.
  world.hardware_deadlines.clear();
  ASSERT_EQ(world.Run(ComponentAction::kDisable).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(world.hardware_deadlines.size(), 1U);
  EXPECT_FALSE(world.hardware_deadlines[0].has_value());
  // The switch unanswered within that deadline: the Enable ends with it.
  world.hardware.clear();
  world.hardware_outcome = ActionOutcome::kUnknown;
  world.hardware_error = "component_action_refused";  // the lane's mapping of the switch timeout
  const auto unanswered = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(unanswered.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(unanswered.action, ComponentAction::kEnable);
  ASSERT_TRUE(unanswered.error.has_value());
  EXPECT_EQ(unanswered.error->code, "component_action_refused");
  EXPECT_EQ(world.hardware, Ids{"off"});  // nothing is resent
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

// After the arming Enable continues once the joint state broadcaster
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

// The vendor plugin's controller switching is answered without effect
// (`XArmPluginControllers`): after the arming Enable waits only for the
// ready report and joint states stamped after it, then switches the
// trajectory controller on once; no cycle bound, no second request.
TEST(XArmArmAdapter, EnableArmsWithoutWaitingForThePluginsControllerCycle) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  // The broadcaster's samples resume two report periods after the hardware
  // is on.
  auto pauses = std::make_shared<int>(-1);
  world.on_hardware = [&world, pauses](const bool active) {
    if (!active) return;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                  1'000'000'000LL}};
    *pauses = 0;
  };
  world.on_pause = [&world, pauses] {
    if (*pauses >= 0 && ++*pauses == 2)
      world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kFresh, 0}};
  };
  const auto started = world.clock.now();
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  // Two report periods for the joints, then one for the gate's report.
  EXPECT_LE(world.clock.now() - started, 3 * kXArmReportPeriodNs);
  EXPECT_EQ(world.hardware, (Ids{"off", "on"}));
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
  EXPECT_TRUE(world.unanswered.empty());
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

// 2026-11-08 (maintainer): Enable is the explicit clear of an e-stop error;
// an e-stop still pressed (C1-C3 after the activation's clear) is refused
// with its own typed reason.
TEST(XArmArmAdapter, EnableIsRefusedTypedWhileTheEstopStaysPressed) {
  World world;
  world.arms_robot = false;
  world.status->Set(report(world.clock.now(), 2, 0, 4));
  const auto failed = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.action, ComponentAction::kEnable);
  EXPECT_EQ(failed.error->code, "robot_estop_active");
  EXPECT_EQ(failed.error->vendor_code, "C2");
  EXPECT_NE(world.adapter->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
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

// With the hardware off (prepared, motors off) the driver's own joint
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

// The joint states come back before the controller manager's activity
// reports the broadcaster active: Enable waits for both, then switches the
// trajectory controller on once.
TEST(XArmArmAdapter, EnableWaitsForTheBroadcasterReportedActive) {
  World world;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.lane->actions.clear();
  world.hardware.clear();
  auto lag = std::make_shared<int>(-1);
  world.on_hardware = [&world, lag](const bool active) {
    if (!active) return;
    world.broadcaster = false;
    *lag = 2;
  };
  bool enabled_early = false;
  world.on_pause = [&world, lag, &enabled_early] {
    enabled_early = enabled_early ||
                    std::find(world.lane->actions.begin(), world.lane->actions.end(),
                              ComponentAction::kEnable) != world.lane->actions.end();
    if (*lag > 0 && --*lag == 0) world.broadcaster = true;
  };
  const auto enabled = world.Run(ComponentAction::kEnable);
  EXPECT_EQ(enabled.outcome, ActionOutcome::kSucceeded);
  EXPECT_FALSE(enabled_early);
  EXPECT_EQ(world.lane->actions, (Actions{ComponentAction::kDisable, ComponentAction::kEnable}));
}

// A Stop ends each wait of the arm's actions with the typed
// `component_action_interrupted_by_stop`.
TEST(XArmArmAdapter, AStopEndsEachWaitTyped) {
  const auto interrupted = [](const core::ActionResult &made) {
    return made.outcome == ActionOutcome::kFailed && made.error &&
           made.error->code == "component_action_interrupted_by_stop";
  };
  {
    // Prepare: the status report.
    World world;
    world.reporting = false;
    world.clock.advance(kXArmStatusFreshnessNs + 1);
    std::stop_source stop;
    world.on_pause = [&stop] { stop.request_stop(); };
    EXPECT_TRUE(interrupted(world.adapter->Run(ComponentAction::kPrepare, stop.get_token())));
  }
  {
    // Enable: the ready report after the arming.
    World world;
    world.arms_robot = false;
    world.status->Set(report(world.clock.now(), 0, 0, kXArmConfigChangedState));
    std::stop_source stop;
    world.on_pause = [&stop] { stop.request_stop(); };
    EXPECT_TRUE(interrupted(world.adapter->Run(ComponentAction::kEnable, stop.get_token())));
    EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
  }
  {
    // Enable: joint states stamped after the ready report.
    World world;
    world.lane->state.streams = {{"measured_joints", core::StreamFreshness::kStale,
                                  1'000'000'000LL}};
    std::stop_source stop;
    world.on_pause = [&world, &stop] {
      // The first pause brings the ready report; the Stop comes after it.
      if (world.clock.now() > 10'000'000'000LL + kXArmReportPeriodNs) stop.request_stop();
    };
    EXPECT_TRUE(interrupted(world.adapter->Run(ComponentAction::kEnable, stop.get_token())));
    EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
  }
  {
    // Enable: the gate's report after the re-arm.
    World world;
    std::stop_source stop;
    int pauses = 0;
    world.on_pause = [&world, &stop, &pauses] {
      // The first pause brings the ready report; the controller then falls
      // silent and a Stop comes.
      if (++pauses < 2) return;
      world.reporting = false;
      stop.request_stop();
    };
    EXPECT_TRUE(interrupted(world.adapter->Run(ComponentAction::kEnable, stop.get_token())));
    EXPECT_EQ(world.lane->actions, Actions{ComponentAction::kDisable});
  }
  {
    // A TCP offset: the ready report after `set_state(0)`.
    World world;
    world.hardware_on = true;
    ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
    world.reporting = false;
    std::stop_source stop;
    world.on_pause = [&stop] { stop.request_stop(); };
    const auto stopped = world.adapter->ApplyToolContext(
        core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}}, std::nullopt, false,
        XArmToolContextBound{std::nullopt, stop.get_token()});
    ASSERT_FALSE(stopped.ok());
    EXPECT_EQ(stopped.error().code, "component_action_interrupted_by_stop");
    EXPECT_EQ(stopped.error().details.at("reason"), "xarm_reengage_not_ready");
  }
}

// Lost readiness as the plugin reads it (an e-stop, a controller error, a
// state above ready, out of joint-servo mode) with the hardware on switches
// the trajectory controller and the broadcaster off: one request, sent again
// only after the controller manager answered and while the activity still
// shows them active.
TEST(XArmArmAdapter, LostReadinessSwitchesTheControllersOffOncePerAnswer) {
  for (const auto &not_ready :
       {report(0, 2, 0, 4, 0), report(0, 31), report(0, 0, 0, kXArmReadyState),
        report(0, 0, kXArmJointServoMode, 4), report(0, 0, kXArmJointServoMode,
                                                     kXArmConfigChangedState)}) {
    World world;
    world.hardware_on = true;
    ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
    // Ready (state 1 or 2, joint-servo mode, no error): nothing.
    world.adapter->OnReport(report(world.clock.now()));
    world.adapter->OnReport(report(world.clock.now(), 0, kXArmJointServoMode, kXArmMovingState));
    EXPECT_TRUE(world.unanswered.empty());
    world.adapter->OnReport(not_ready);
    EXPECT_EQ(world.unanswered.size(), 1U);
    // Unanswered: no second request.
    world.adapter->OnReport(not_ready);
    EXPECT_EQ(world.unanswered.size(), 1U);
    // Answered while the activity still shows them active: sent again.
    world.Answer();
    world.adapter->OnReport(not_ready);
    EXPECT_EQ(world.unanswered.size(), 1U);
    // Answered and reported off: nothing more.
    world.Answer();
    world.controllers_active = false;
    world.adapter->OnReport(not_ready);
    EXPECT_TRUE(world.unanswered.empty());
  }
}

// As the plugin: nothing with the hardware off (its write loop doesn't run)
// or nothing active; an undiscovered service sends nothing and the next
// report tries again.
TEST(XArmArmAdapter, LostReadinessSendsNothingWithTheHardwareOrTheControllersOff) {
  World world;
  const auto estop = report(world.clock.now(), 2, 0, 4, 0);
  world.adapter->OnReport(estop);
  EXPECT_TRUE(world.unanswered.empty());
  world.hardware_on = true;
  world.controllers_active = false;
  world.adapter->OnReport(estop);
  EXPECT_TRUE(world.unanswered.empty());
  world.controllers_active = true;
  world.switch_discovered = false;
  world.adapter->OnReport(estop);
  EXPECT_TRUE(world.unanswered.empty());
  world.switch_discovered = true;
  world.adapter->OnReport(estop);
  EXPECT_EQ(world.unanswered.size(), 1U);
}

// Its own arming (Enable's hardware switch, a TCP offset's re-arm) changes
// mode and state itself: its reports never switch the controllers off.
TEST(XArmArmAdapter, ItsOwnArmingAndTcpOffsetNeverSwitchTheControllersOff) {
  World world;
  world.hardware_on = true;
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  const auto configuring = [&world] {
    return report(world.clock.now(), 0, kXArmJointServoMode, kXArmConfigChangedState);
  };
  // Its arming reports state 5 with the hardware on (from the hardware's
  // activation until the ready report) and so does the offset's re-arm.
  world.on_hardware = [&world, configuring](const bool active) {
    if (active) world.adapter->OnReport(configuring());
  };
  world.on_pause = [&world, configuring] {
    if (world.adapter->Arming()) world.adapter->OnReport(configuring());
  };
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  EXPECT_TRUE(world.unanswered.empty());

  // The offset's own state 5 before its ready report.
  world.on_hardware = nullptr;
  const auto applied = world.adapter->ApplyToolContext(
      core::Pose3DTO{{0, 0, 0}, {0, 0, 0, 1}}, std::nullopt, false);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_TRUE(applied.value());
  EXPECT_TRUE(world.unanswered.empty());

  // The same report outside its own operations: switched off.
  world.on_pause = nullptr;
  world.adapter->OnReport(configuring());
  EXPECT_EQ(world.unanswered.size(), 1U);

  // Enable's arming forgets a deactivation never answered (its controller
  // manager gone): the next one is sent.
  world.status->Set(report(world.clock.now(), 0, 0, 4, 0));
  ASSERT_EQ(world.Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  world.adapter->OnReport(configuring());
  EXPECT_EQ(world.unanswered.size(), 2U);
}

// A TCP offset sent at Enable returns on its ready report: the
// controllers stay as they are (no plugin cycle to wait out).
TEST(XArmArmAdapter, AnOffsetSentAtEnableReturnsOnItsReadyReport) {
  World world;
  world.hardware_on = true;
  // The ready report after `set_state(0)` shows the sent offset.
  world.on_pause = [&world] {
    const auto calls = world.driver->Calls();
    if (std::find(calls.begin(), calls.end(), "set_tcp_offset") == calls.end()) return;
    auto next = world.status->observe();
    next.tcp_offset = {10.0, 0, 0, 0, 0, 0};
    world.status->Set(next);
  };
  const auto started = world.clock.now();
  const auto applied = world.adapter->ApplyToolContext(
      core::Pose3DTO{{0.01, 0, 0}, {0, 0, 0, 1}}, core::ToolContextPointMassDTO{0.5, {0, 0, 0}},
      true);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_TRUE(applied.value());
  EXPECT_LE(world.clock.now() - started, kXArmReportPeriodNs);
  EXPECT_EQ(world.driver->Calls(),
            (std::vector<std::string>{"set_tcp_offset", "set_state(0)", "set_tcp_load"}));
  EXPECT_TRUE(world.hardware.empty());
  EXPECT_TRUE(world.lane->actions.empty());
  EXPECT_TRUE(world.unanswered.empty());

  // No offset to send (the report already shows it): nothing sent.
  World same;
  same.hardware_on = true;
  const auto kept = same.adapter->ApplyToolContext(core::Pose3DTO{{0, 0, 0}, {0, 0, 0, 1}},
                                                   std::nullopt, true);
  ASSERT_TRUE(kept.ok());
  EXPECT_FALSE(kept.value());
  EXPECT_TRUE(same.driver->Calls().empty());
}

} // namespace
} // namespace manyforge::xarm_control
