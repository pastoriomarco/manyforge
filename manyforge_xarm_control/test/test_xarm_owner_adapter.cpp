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

// The xArm machine owner against a fake driver and lane: its actions, its
// faults with the members they concern, the power actions (the core disables
// the members first), and the I/O stream handed to the I/O members.

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_xarm_control/xarm_owner_adapter.hpp>

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

using Ids = std::vector<std::string>;

struct World final {
  World() {
    clock.set(10'000'000'000LL);
    status->Set(report(clock.now()));
    owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
        status, driver, lane,
        [this](const std::stop_token stop) { return restart(stop); },
        [this] {
          // Down for the first probes, then back.
          ++probes;
          return probes > 2;
        },
        [this](const std::chrono::nanoseconds duration) {
          // Time passes; the controller keeps reporting.
          clock.advance(duration.count());
          waited += duration;
          auto next = status->observe();
          next.received_timestamp_ns = clock.now();
          status->Set(next);
        },
        &clock,
        [this](const std::string &id, const std::vector<core::IoSignalUpdateDTO> &updates,
               std::int64_t) { sunk.emplace_back(id, updates.size()); },
        std::chrono::seconds(10), std::chrono::milliseconds(200), std::chrono::seconds(10), {}});
    owner->AddMember({"arm", "lite6_robot", arm, {}, {}});
    owner->AddMember({"controller_io", "lite6_robot_controller_io", controller_io, controller_io, {}});
    owner->AddMember({"tool_io", "lite6_robot_tool_io", tool_io, tool_io, {}});
  }
  [[nodiscard]] core::ActionResult Run(const ComponentAction action) {
    return owner->Run(action, std::stop_token{});
  }
  [[nodiscard]] std::shared_ptr<XArmIoComponent> Io(const std::string &id, const XArmIoBank bank,
                                                    std::vector<XArmIoSignal> signals) {
    return std::make_shared<XArmIoComponent>(
        XArmIoComponentConfig{id, bank, std::move(signals), XArmIoSettings{}}, driver,
        [this](const std::int64_t now_ns) { return owner->Usable(now_ns); }, clock);
  }

  core::FakeClock clock;
  std::shared_ptr<FakeStatus> status = std::make_shared<FakeStatus>();
  std::shared_ptr<FakeDriver> driver = std::make_shared<FakeDriver>();
  std::shared_ptr<FakeLane> lane = std::make_shared<FakeLane>();
  std::shared_ptr<XArmOwnerAdapter> owner;
  std::shared_ptr<FakeLane> arm = std::make_shared<FakeLane>();
  std::shared_ptr<XArmIoComponent> controller_io =
      Io("lite6_robot_controller_io", XArmIoBank::kController,
         {{"CI0", core::IoSignalDirection::kInput, 0},
          {"CO1", core::IoSignalDirection::kOutput, 1}});
  std::shared_ptr<XArmIoComponent> tool_io =
      Io("lite6_robot_tool_io", XArmIoBank::kTool, {{"TO0", core::IoSignalDirection::kOutput, 0}});
  int restarts{0};
  int probes{0};
  std::chrono::nanoseconds waited{0};
  std::function<core::Result<void>(std::stop_token)> restart = [this](std::stop_token) {
    ++restarts;
    return core::Result<void>::Ok();
  };
  std::vector<std::pair<std::string, std::size_t>> sunk;
};

TEST(XArmOwnerAdapter, PrepareClearsTheErrorRecoverClearsItAndThereIsNoEnable) {
  World world;
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kNotPrepared);
  EXPECT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.restarts, 0);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(world.Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.driver->Calls(), (Ids{"clean_error", "get_err_warn_code", "clean_error",
                                        "get_err_warn_code"}));
  EXPECT_TRUE(world.lane->actions.empty());
  EXPECT_EQ(world.Run(ComponentAction::kEnable).error->code, "component_action_unsupported");
  EXPECT_EQ(world.Run(ComponentAction::kDisable).error->code, "component_action_unsupported");
  EXPECT_EQ(world.owner->capabilities(),
            (std::vector<core::Capability>{core::Capability::kPowerOff,
                                           core::Capability::kReboot}));
}

TEST(XArmOwnerAdapter, PrepareRestartsTheProcessWhenNotConnectedThenWaitsForItsReport) {
  World world;
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  EXPECT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.restarts, 1);
  EXPECT_EQ(world.driver->Calls(), (Ids{"clean_error", "get_err_warn_code"}));
}

// The restarted process's first report is awaited for the machine's
// lifecycle command time (not one freshness window), then the prepare fails.
TEST(XArmOwnerAdapter, PrepareWaitsForTheReportWithinTheLifecycleCommandTime) {
  World world;
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  const auto started = world.clock.now();
  int pauses = 0;
  auto silent = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
      world.status, world.driver, world.lane,
      [](std::stop_token) { return core::Result<void>::Ok(); }, [] { return true; },
      [&](const std::chrono::nanoseconds duration) {
        ++pauses;
        world.clock.advance(duration.count());
      },
      &world.clock, {}, std::chrono::seconds(10), std::chrono::milliseconds(200),
      std::chrono::seconds(2), {}});
  const auto failed = silent->Run(ComponentAction::kPrepare, {});
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.error->code, "xarm_status_unavailable");
  EXPECT_GE(world.clock.now() - started, 2'000'000'000LL);
  EXPECT_LT(world.clock.now() - started, 2'000'000'000LL + kXArmReportPeriodNs + 1);
  EXPECT_GT(pauses, 0);
}

// Live 2026-11-02 (Real Lite 6): Prepare right after the Apply's launch,
// before the new driver's first report, restarted the driver. While the
// launch is still starting, the first report is awaited instead; a driver
// whose launch has settled and that still sends no report is restarted.
TEST(XArmOwnerAdapter, PrepareAwaitsAStartingLaunchsFirstReportAndRestartsOnlyAnOfflineDriver) {
  for (const bool starting : {true, false}) {
    World world;
    world.clock.advance(kXArmStatusFreshnessNs + 1);  // no report yet
    int restarts = 0;
    int pauses = 0;
    auto owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
        world.status, world.driver, world.lane,
        [&](std::stop_token) {
          ++restarts;
          return core::Result<void>::Ok();
        },
        [] { return true; },
        [&](const std::chrono::nanoseconds duration) {
          world.clock.advance(duration.count());
          // The driver's first report arrives on the third pause.
          if (++pauses >= 3) {
            auto next = world.status->observe();
            next.received_timestamp_ns = world.clock.now();
            world.status->Set(next);
          }
        },
        &world.clock, {}, std::chrono::seconds(10), std::chrono::milliseconds(200),
        std::chrono::seconds(10), [&] { return starting; }});
    EXPECT_EQ(owner->Run(ComponentAction::kPrepare, {}).outcome, ActionOutcome::kSucceeded);
    EXPECT_EQ(restarts, starting ? 0 : 1) << "starting=" << starting;
    EXPECT_EQ(world.driver->Calls(), (Ids{"clean_error", "get_err_warn_code"}));
  }
}

TEST(XArmOwnerAdapter, AnEstopStillPressedOrNoAnswerOrAServoErrorFailsTyped) {
  World world;
  world.driver->clean_error = {XArmServiceCallOutcome::kUnknown,
                               "xarm_service_returned_nonzero", 1};
  world.driver->err_warn = {FakeDriver::ok, 2, 0};
  auto failed = world.Run(ComponentAction::kRecover);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.error->code, "robot_error_not_cleared");
  EXPECT_EQ(failed.error->message,
            "Couldn't clear the robot error. If the e-stop is pressed, release it, then "
            "Recover the machine and Enable the arm.");
  EXPECT_EQ(failed.error->vendor_code, "C2");
  // As M7: three reads, 150 ms apart.
  EXPECT_EQ(world.driver->Calls(), (Ids{"clean_error", "get_err_warn_code", "get_err_warn_code",
                                        "get_err_warn_code"}));
  EXPECT_EQ(world.waited, std::chrono::milliseconds(300));

  World unanswered;
  unanswered.driver->clean_error = {XArmServiceCallOutcome::kUnknown, "xarm_service_timeout", {}};
  failed = unanswered.Run(ComponentAction::kPrepare);
  EXPECT_EQ(failed.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(failed.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(unanswered.owner->State(unanswered.clock.now()).lifecycle,
            ComponentLifecycle::kNotPrepared);

  World servo;
  servo.status->Set(report(servo.clock.now(), 12));
  failed = servo.Run(ComponentAction::kRecover);
  EXPECT_EQ(failed.error->code, "fault_drive");
  EXPECT_EQ(failed.error->vendor_code, "C12");
  EXPECT_TRUE(servo.driver->Calls().empty());
}

// Live 2026-11-02 (Real Lite 6, Load): the driver started with the C2 left
// after an e-stop; Prepare's read showed it cleared while `robot_states`
// (5 Hz) still carried C2, so the members' Prepare right after was refused
// `owner_not_prepared`. Prepare and Recover end once the report shows the
// error gone; a report that keeps it until the lifecycle time fails typed.
TEST(XArmOwnerAdapter, PrepareAndRecoverEndOnceTheReportShowsTheErrorGone) {
  for (const auto action : {ComponentAction::kPrepare, ComponentAction::kRecover}) {
    World world;
    world.status->Set(report(world.clock.now(), 2));
    ASSERT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
    int pauses = 0;
    auto owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
        world.status, world.driver, world.lane, world.restart, [] { return true; },
        [&](const std::chrono::nanoseconds duration) {
          world.clock.advance(duration.count());
          // The controller's next report (after the clear) has no error.
          if (duration.count() > 0 && ++pauses >= 2)
            world.status->Set(report(world.clock.now()));
        },
        &world.clock, {}, std::chrono::seconds(10), std::chrono::milliseconds(200),
        std::chrono::seconds(10), {}});
    EXPECT_EQ(owner->Run(action, {}).outcome, ActionOutcome::kSucceeded);
    EXPECT_EQ(pauses, 2);
    EXPECT_NE(owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
    if (action == ComponentAction::kPrepare) {
      EXPECT_EQ(owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
    }
    EXPECT_EQ(world.driver->Calls(), (Ids{"clean_error", "get_err_warn_code"}));
  }

  // The report keeps the error (the read said cleared) for the whole bound.
  World stays;
  stays.status->Set(report(stays.clock.now(), 2));
  const auto started = stays.clock.now();
  const auto failed = stays.Run(ComponentAction::kPrepare);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.error->code, "robot_error_not_cleared");
  EXPECT_EQ(failed.error->vendor_code, "C2");
  EXPECT_GE(stays.clock.now() - started, 10'000'000'000LL);
  EXPECT_EQ(stays.owner->State(stays.clock.now()).lifecycle, ComponentLifecycle::kFault);
}

// Review 2026-11-02: Prepare and Recover end on the owner's own state, not
// the report's error alone: a GPIO module error left after the clear keeps
// them waiting, and fails them typed at the lifecycle time.
TEST(XArmOwnerAdapter, PrepareEndsOnlyOnceTheOwnersStateShowsNoFault) {
  World world;
  world.owner->OnGpioModuleStatus({6, 0, {}});
  const auto started = world.clock.now();
  const auto failed = world.Run(ComponentAction::kPrepare);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.error->code, "fault_communication");
  EXPECT_EQ(failed.error->vendor_code, "gpio state=6 code=0");
  EXPECT_GE(world.clock.now() - started, 10'000'000'000LL);

  // The module recovers during the wait: the Prepare succeeds.
  World recovers;
  recovers.owner->OnGpioModuleStatus({6, 0, {}});
  int pauses = 0;
  std::shared_ptr<XArmOwnerAdapter> owner;
  owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
      recovers.status, recovers.driver, recovers.lane, recovers.restart, [] { return true; },
      [&](const std::chrono::nanoseconds duration) {
        recovers.clock.advance(duration.count());
        auto next = recovers.status->observe();
        next.received_timestamp_ns = recovers.clock.now();
        recovers.status->Set(next);
        if (duration.count() > 0 && ++pauses == 3) owner->OnGpioModuleStatus({0, 0, {}});
      },
      &recovers.clock, {}, std::chrono::seconds(10), std::chrono::milliseconds(200),
      std::chrono::seconds(10), {}});
  owner->OnGpioModuleStatus({6, 0, {}});
  EXPECT_EQ(owner->Run(ComponentAction::kPrepare, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(pauses, 3);
  EXPECT_EQ(owner->State(recovers.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
}

// Review 2026-11-02: 5 Hz best-effort reports keep-last-1: a report is
// fresh for five periods (1 s).
TEST(XArmOwnerAdapter, AReportIsFreshForFivePeriods) {
  EXPECT_EQ(kXArmStatusFreshnessNs, 5 * kXArmReportPeriodNs);
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.status->Set(report(world.clock.now()));
  world.clock.advance(900'000'000);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  world.clock.advance(200'000'000);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
}

TEST(XArmOwnerAdapter, ControllerFaultsAreTheOwnersAndNameTheMembersTheyConcern) {
  World world;
  const auto fault_of = [&world](const int error) {
    world.status->Set(report(world.clock.now(), error));
    const auto state = world.owner->State(world.clock.now());
    EXPECT_EQ(state.lifecycle, ComponentLifecycle::kFault) << error;
    return *state.fault;
  };
  EXPECT_EQ(fault_of(33).concerns, Ids{"lite6_robot_controller_io"});
  EXPECT_EQ(fault_of(19).concerns, Ids{"lite6_robot_tool_io"});
  EXPECT_EQ(fault_of(22).concerns, Ids{"lite6_robot"});
  EXPECT_EQ(fault_of(22).code, core::FaultCode::kSelfCollision);
  const auto estop = fault_of(2);
  EXPECT_EQ(estop.code, core::FaultCode::kEstop);
  EXPECT_EQ(estop.operator_action, core::FaultOperatorAction::kRecover);
  const Ids all{"lite6_robot", "lite6_robot_controller_io", "lite6_robot_tool_io"};
  EXPECT_EQ(estop.concerns, all);
  EXPECT_EQ(fault_of(26).concerns, all);
  world.clock.advance(100'000'000);
  EXPECT_EQ(fault_of(26).since_ns, world.clock.now() - 100'000'000);

  // The GPIO module's error concerns the controller I/O.
  world.status->Set(report(world.clock.now()));
  for (const XArmGpioModuleStatus &gpio : {XArmGpioModuleStatus{6, 0, {}},
                                         XArmGpioModuleStatus{1, 0, {}},
                                         XArmGpioModuleStatus{0, 4, {}}}) {
    world.owner->OnGpioModuleStatus(gpio);
    const auto state = world.owner->State(world.clock.now());
    ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
    EXPECT_EQ(state.fault->code, core::FaultCode::kCommunication);
    EXPECT_EQ(state.fault->concerns, Ids{"lite6_robot_controller_io"});
  }
  world.owner->OnGpioModuleStatus({0, 0, {}});
  auto state = world.owner->State(world.clock.now());

  // A stale report is a lost connection of the whole machine.
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  state = world.owner->State(world.clock.now());
  EXPECT_EQ(state.fault->code, core::FaultCode::kCommunication);
  EXPECT_EQ(state.fault->concerns, all);
  EXPECT_EQ(state.streams[0].freshness, core::StreamFreshness::kStale);
}

TEST(XArmOwnerAdapter, ARebuiltMemberReplacesItsRegistrationAndARemovedOneIsNoConcern) {
  World world;
  auto rebuilt = std::make_shared<FakeLane>();
  world.owner->AddMember({"arm", "lite6_robot", rebuilt, {}, {}});
  world.status->Set(report(world.clock.now(), 2));
  const Ids all{"lite6_robot", "lite6_robot_controller_io", "lite6_robot_tool_io"};
  EXPECT_EQ(world.owner->State(world.clock.now()).fault->concerns, all);
  // The tool I/O left the cell (its adapter is gone): not concerned.
  world.tool_io.reset();
  EXPECT_EQ(world.owner->State(world.clock.now()).fault->concerns,
            (Ids{"lite6_robot", "lite6_robot_controller_io"}));
}

TEST(XArmOwnerAdapter, TheClearReadsBackUntilTheErrorIsGoneAndClearsAWarning) {
  World world;
  world.driver->err_warn_reads = {{FakeDriver::ok, 2, 0}, {FakeDriver::ok, 0, 7}};
  EXPECT_EQ(world.Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.driver->Calls(),
            (Ids{"clean_error", "get_err_warn_code", "get_err_warn_code", "clean_warn"}));
  EXPECT_EQ(world.waited, std::chrono::milliseconds(150));

  // XPW-1: Prepare clears a warning too (the arm's hardware, whose
  // activation also clears it, stays off until Enable); never motion_enable.
  World prepared;
  prepared.driver->err_warn = {FakeDriver::ok, 0, 7};
  EXPECT_EQ(prepared.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(prepared.driver->Calls(), (Ids{"clean_error", "get_err_warn_code", "clean_warn"}));
  // Without a warning left, none.
  World clean;
  EXPECT_EQ(clean.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(clean.driver->Calls(), (Ids{"clean_error", "get_err_warn_code"}));

  // A clean_warn without an answer is unknown, never resent.
  World unanswered;
  unanswered.driver->err_warn = {FakeDriver::ok, 0, 7};
  unanswered.driver->clean_warn = {XArmServiceCallOutcome::kUnknown, "xarm_service_timeout", {}};
  const auto failed = unanswered.Run(ComponentAction::kRecover);
  EXPECT_EQ(failed.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(failed.error->code, "robot_controller_unresponsive");
}

TEST(XArmOwnerAdapter, RecoverReconnectsALostDriverLikePrepare) {
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  EXPECT_EQ(world.owner->State(world.clock.now()).fault->code, core::FaultCode::kCommunication);
  EXPECT_EQ(world.Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.restarts, 1);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);

  // No first report within the component's lifecycle timeout: typed.
  World silent;
  silent.clock.advance(kXArmStatusFreshnessNs + 1);
  silent.restart = [&silent](std::stop_token) {
    silent.status->Set({});
    return core::Result<void>::Ok();
  };
  auto owner = std::make_shared<XArmOwnerAdapter>(XArmOwnerAdapterPorts{
      silent.status, silent.driver, silent.lane, silent.restart, [] { return true; },
      [&silent](const std::chrono::nanoseconds duration) { silent.clock.advance(duration.count()); },
      &silent.clock, {}, std::chrono::seconds(10), std::chrono::milliseconds(200),
      std::chrono::seconds(2), {}});
  const auto started = silent.clock.now();
  const auto failed = owner->Run(ComponentAction::kRecover, {});
  EXPECT_EQ(failed.error->code, "xarm_status_unavailable");
  EXPECT_GE(silent.clock.now() - started, 2'000'000'000LL);
  EXPECT_LT(silent.clock.now() - started, 2'000'000'000LL + kXArmReportPeriodNs + 1);
}

TEST(XArmOwnerAdapter, ExternalControlOfTheArmIsTheOwnersFaultAndRecoverReleasesIt) {
  World world;
  auto arm_lane = std::make_shared<FakeLane>();
  auto arm = std::make_shared<XArmArmAdapter>(
      XArmArmAdapterConfig{6, kXArmJointAgreementToleranceRad, std::chrono::seconds(10)},
      XArmArmAdapterPorts{world.status, world.driver, arm_lane,
                          [](bool, std::stop_token) {
                            core::ActionResult made;
                            made.outcome = ActionOutcome::kSucceeded;
                            return made;
                          },
                          [] { return true; }, [] {},
                          [] {
                            return std::optional<std::vector<double>>(
                                std::vector<double>{0.1, -0.2, 0.3, 0.0, 0.5, 0.6});
                          },
                          &world.clock});
  world.owner->AddMember({"arm", "lite6_robot_arm", arm, {}, arm});
  // XPW-1: only an Enabled arm (motors on) is armed.
  ASSERT_EQ(arm->Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  world.clock.advance(kXArmOwnOperationSettleNs + 1);
  // A Studio pause in joint-servo mode.
  world.status->Set(report(world.clock.now(), 0, kXArmJointServoMode, 3));
  auto state = world.owner->State(world.clock.now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(state.fault->code, core::FaultCode::kExternalControl);
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  EXPECT_EQ(state.fault->concerns, Ids{"lite6_robot_arm"});
  EXPECT_NE(arm->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);

  EXPECT_EQ(world.Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  EXPECT_NE(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kFault);
}

TEST(XArmOwnerAdapter, DetailsShowTheControllersReportForDisplay) {
  World world;
  world.status->Set(report(world.clock.now(), 2, 0, 4, 0x3F));
  auto observed = world.status->observe();
  observed.warning_code = 11;
  world.status->Set(observed);
  const auto state = world.owner->State(world.clock.now());
  EXPECT_EQ(state.details,
            (std::vector<core::ComponentDetail>{
                {"mode", "0"},
                {"state", "4"},
                {"error", "C2: Emergency IO of Control Box is triggered"},
                {"warning", "11"},
                {"motors_enabled", "6"}}));
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  EXPECT_TRUE(world.owner->State(world.clock.now()).details.empty());
}

TEST(XArmOwnerAdapter, PowerOffIsOfflineNotAFault) {
  World world;
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  const auto off = world.Run(ComponentAction::kPowerOff);
  EXPECT_EQ(off.outcome, ActionOutcome::kSucceeded);
  // The core disables the members before the owner's power action.
  EXPECT_TRUE(world.arm->actions.empty());
  EXPECT_EQ(world.driver->Calls(),
            (Ids{"clean_error", "get_err_warn_code", "system_control(1)"}));
  // The driver loses the controller: still offline with the reason.
  world.clock.advance(kXArmStatusFreshnessNs * 4);
  const auto state = world.owner->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kOffline);
  EXPECT_FALSE(state.fault.has_value());
  EXPECT_EQ(state.reason->code, "shutdown_by_operator");
  EXPECT_FALSE(world.owner->Usable(world.clock.now()));

  // Back on the power switch, Prepare restarts the process first.
  EXPECT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.restarts, 1);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
}

TEST(XArmOwnerAdapter, RebootWaitsThenRestartsAndPrepares) {
  World world;
  const auto rebooted = world.Run(ComponentAction::kReboot);
  EXPECT_EQ(rebooted.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(rebooted.action, ComponentAction::kReboot);
  EXPECT_TRUE(world.arm->actions.empty());
  EXPECT_EQ(world.driver->Calls(),
            (Ids{"system_control(2)", "clean_error", "get_err_warn_code"}));
  EXPECT_EQ(world.restarts, 1);
  EXPECT_GE(world.probes, 3);
  EXPECT_EQ(world.owner->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
}

TEST(XArmOwnerAdapter, ARebootThatNeverComesBackFailsAndStaysRebooting) {
  World world;
  world.probes = -1'000'000;
  const auto failed = world.Run(ComponentAction::kReboot);
  EXPECT_EQ(failed.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(failed.error->code, "component_reboot_timeout");
  EXPECT_EQ(world.owner->State(world.clock.now()).reason->code, "component_rebooting");
}

TEST(XArmOwnerAdapter, TheStreamGoesToEachIoMemberAndItsStaleFaultOnlyWhileTheOwnerIsUsable) {
  World world;
  world.owner->OnIoStates({0b1, 0b10, 0, 0b1, world.clock.now()});
  EXPECT_EQ(world.sunk, (std::vector<std::pair<std::string, std::size_t>>{
                            {"lite6_robot_controller_io", 2U}, {"lite6_robot_tool_io", 1U}}));
  ASSERT_EQ(world.tool_io->Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  // Stream stopped while the owner isn't prepared: no fault of its own.
  world.clock.advance(150'000'000);
  world.status->Set(report(world.clock.now()));
  EXPECT_EQ(world.tool_io->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
  // Owner prepared and connected: the stale stream is the member's fault.
  ASSERT_EQ(world.Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  const auto state = world.tool_io->State(world.clock.now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(state.fault->code, core::FaultCode::kStreamStale);
  // The owner loses its report: the owner's fault, not the member's.
  world.clock.advance(kXArmStatusFreshnessNs + 1);
  EXPECT_EQ(world.tool_io->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
}

} // namespace
} // namespace manyforge::xarm_control
