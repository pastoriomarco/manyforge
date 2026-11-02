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

// The xArm I/O members on the `xarm_io_states` stream: writes, their
// confirmation, outside changes, unknown writes settled by the stream,
// controller-owned and declared-only outputs, the confirmation bound and the
// stream's health while the owner is usable.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_xarm_control/xarm_io_component.hpp>

#include "xarm_fakes.hpp"

namespace manyforge::xarm_control {
namespace {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentLifecycle;
using core::IoSignalDirection;
using Source = core::IoSignalSampleSource;
using testing::FakeDriver;

constexpr std::int64_t kMs = 1'000'000;

struct World final {
  explicit World(const XArmIoBank bank = XArmIoBank::kController) {
    clock.set(1'000 * kMs);
    std::vector<XArmIoSignal> signals =
        bank == XArmIoBank::kController
            ? std::vector<XArmIoSignal>{{"CI0", IoSignalDirection::kInput, 0},
                                        {"CO1", IoSignalDirection::kOutput, 1},
                                        {"DO0", IoSignalDirection::kOutput, 8}}
            : std::vector<XArmIoSignal>{{"TO0", IoSignalDirection::kOutput, 0},
                                        {"TO1", IoSignalDirection::kOutput, 1}};
    io = std::make_unique<XArmIoComponent>(
        XArmIoComponentConfig{bank == XArmIoBank::kTool ? "lite6_tool_io" : "lite6_controller_io",
                              bank, signals, XArmIoSettings{}},
        driver, [this](std::int64_t) { return owner_usable; }, clock);
  }

  /// One stream sample now, with the controller words (tool words for tool).
  std::vector<core::IoSignalUpdateDTO> Sample(const std::uint16_t inputs,
                                              const std::uint16_t outputs) {
    return io->OnIoStates({inputs, outputs, inputs, outputs, clock.now()});
  }

  [[nodiscard]] core::SignalSample Read(const std::string &name) const {
    for (auto &sample : io->Samples(clock.now()))
      if (sample.signal_id.name == name) return sample;
    throw std::runtime_error("no sample " + name);
  }

  void Enable() {
    static_cast<void>(Sample(0, 0));
    ASSERT_EQ(io->Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  }

  core::FakeClock clock;
  bool owner_usable{true};
  std::shared_ptr<FakeDriver> driver = std::make_shared<FakeDriver>();
  std::unique_ptr<XArmIoComponent> io;
};

TEST(XArmIoComponent, ConfirmationBoundComesFromTheSettings) {
  EXPECT_EQ(xarm_io_confirmation_bound_ns({100.0, 100.0}), 50 * kMs);
  EXPECT_EQ(xarm_io_confirmation_bound_ns({250.0, 50.0}), 2 * (4 + 20) * kMs + 4 * kMs);
}

TEST(XArmIoComponent, StreamSamplesGiveInputsAndOutputsFromTheDevice) {
  World world;
  const auto updates = world.Sample(0b1, 0b10);
  ASSERT_EQ(updates.size(), 3U);
  EXPECT_EQ(updates[0].signal_id.name, "CI0");
  EXPECT_EQ(updates[0].value, true);
  EXPECT_EQ(updates[1].value, true);  // CO1 is bit 1
  EXPECT_EQ(updates[2].value, false); // DO0 is bit 8
  EXPECT_EQ(updates[1].source, Source::kDevice);
  const auto sample = world.Read("CO1");
  EXPECT_TRUE(sample.fresh);
  EXPECT_EQ(sample.source, Source::kDevice);
  EXPECT_EQ(std::get<bool>(*sample.value), true);
  EXPECT_EQ(world.io->capabilities(),
            (std::vector<core::Capability>{core::Capability::kIoManualWrite,
                                           core::Capability::kIoOutputReadback}));
}

TEST(XArmIoComponent, AnAcceptedWriteIsReportedAtOnceAndStaleSamplesAreIgnored) {
  World world;
  world.Enable();
  const auto written = world.io->SetOutput("CO1", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.driver->Calls(), std::vector<std::string>{"set_cgpio_digital(1,1)"});
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), true);
  // Samples still showing the old value within the bound: ignored.
  world.clock.advance(10 * kMs);
  static_cast<void>(world.Sample(0, 0));
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), true);
  // Seen: confirmed, nothing changes.
  world.clock.advance(10 * kMs);
  static_cast<void>(world.Sample(0, 0b10));
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  // A later change in the stream is an outside change.
  world.clock.advance(10 * kMs);
  static_cast<void>(world.Sample(0, 0));
  EXPECT_EQ(world.Read("CO1").source, Source::kOutside);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), false);
}

TEST(XArmIoComponent, AWriteNotSeenWithinTheBoundIsAnOutsideChange) {
  World world;
  world.Enable();
  ASSERT_EQ(world.io->SetOutput("CO1", true, {}).outcome, ActionOutcome::kSucceeded);
  world.clock.advance(50 * kMs);
  static_cast<void>(world.Sample(0, 0));
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  world.clock.advance(1 * kMs);
  static_cast<void>(world.Sample(0, 0));
  EXPECT_EQ(world.Read("CO1").source, Source::kOutside);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), false);
}

TEST(XArmIoComponent, RetOneIsAnAcceptedWriteAndOtherReturnsFailTyped) {
  World world;
  world.Enable();
  // `ret=1`: a controller error is present and the write takes effect.
  world.driver->set_digital = {XArmServiceCallOutcome::kUnknown, "xarm_service_returned_nonzero",
                               1};
  auto result = world.io->SetOutput("CO1", true, {});
  EXPECT_EQ(result.outcome, ActionOutcome::kSucceeded);
  EXPECT_FALSE(result.error.has_value());
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), true);
  world.driver->set_digital = {XArmServiceCallOutcome::kUnknown, "xarm_service_returned_nonzero",
                               3};
  result = world.io->SetOutput("DO0", true, {});
  EXPECT_EQ(result.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(result.error->code, "io_output_write_failed");
  EXPECT_EQ(result.error->vendor_code, "ret=3");
  EXPECT_EQ(world.Read("DO0").source, Source::kDevice);
}

TEST(XArmIoComponent, AnUnansweredWriteIsUnknownUntilTheStreamReportsTheOutput) {
  World world;
  world.Enable();
  world.driver->set_digital = {XArmServiceCallOutcome::kUnknown, "xarm_service_timeout", {}};
  auto result = world.io->SetOutput("CO1", true, {});
  EXPECT_EQ(result.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(result.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(world.io->State(world.clock.now()).activity, core::ComponentActivity::kUnknown);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), false);
  // The stream shows the write took effect: known again, as our write.
  world.clock.advance(10 * kMs);
  static_cast<void>(world.Sample(0, 0b10));
  EXPECT_EQ(world.io->State(world.clock.now()).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(world.Read("CO1").source, Source::kManyforgeWrite);
  EXPECT_EQ(std::get<bool>(*world.Read("CO1").value), true);

  // It didn't take effect: known again, the value stays.
  result = world.io->SetOutput("DO0", true, {});
  EXPECT_EQ(result.outcome, ActionOutcome::kUnknown);
  world.clock.advance(10 * kMs);
  static_cast<void>(world.Sample(0, 0b10));
  EXPECT_EQ(world.io->State(world.clock.now()).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(world.Read("DO0").source, Source::kDevice);
  EXPECT_EQ(std::get<bool>(*world.Read("DO0").value), false);
  // Never replayed: two writes, two calls.
  EXPECT_EQ(world.driver->Calls().size(), 2U);
}

TEST(XArmIoComponent, ControllerOwnedUndeclaredAndDisabledOutputsAreRefusedBeforeSending) {
  World world;
  world.Enable();
  XArmGpioModuleStatus gpio;
  gpio.output_functions[1] = 2; // CO1 reports "in motion"
  world.io->OnGpioModuleStatus(gpio);
  EXPECT_TRUE(world.io->ControllerOwned("CO1"));
  EXPECT_FALSE(world.io->ControllerOwned("DO0"));
  EXPECT_EQ(world.io->SetOutput("CO1", true, {}).error->code, "io_output_controller_owned");
  EXPECT_EQ(world.io->SetOutput("CO5", true, {}).error->code, "io_signal_unknown");
  EXPECT_EQ(world.io->SetOutput("CI0", true, {}).error->code, "io_signal_unknown");
  // Digital outputs only: a number is refused (the output-writer interface).
  EXPECT_EQ(world.io->SetOutput("DO0", 1.0, {}).error->code, "io_signal_type_mismatch");
  ASSERT_EQ(world.io->Run(ComponentAction::kDisable, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.io->SetOutput("DO0", true, {}).error->code, "io_output_not_submitted");
  EXPECT_TRUE(world.driver->Calls().empty());

  // Tool outputs have no function; only the declared TO0/TO1 are offered.
  World tool(XArmIoBank::kTool);
  tool.Enable();
  tool.io->OnGpioModuleStatus(gpio);
  EXPECT_FALSE(tool.io->ControllerOwned("TO1"));
  EXPECT_EQ(tool.io->SetOutput("TO2", true, {}).error->code, "io_signal_unknown");
  EXPECT_EQ(tool.io->SetOutput("TO1", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(tool.driver->Calls(), std::vector<std::string>{"set_tgpio_digital(1,1)"});
}

// Review 2026-11-02: right after the owner's driver started, the I/O
// stream may not have arrived yet: Prepare, Recover and Enable wait for a
// fresh sample within the lifecycle command time, then fail typed.
TEST(XArmIoComponent, PrepareWaitsForAFreshSampleWithinTheLifecycleTime) {
  for (const bool arrives : {true, false}) {
    core::FakeClock clock;
    clock.set(1'000 * kMs);
    auto driver = std::make_shared<FakeDriver>();
    std::unique_ptr<XArmIoComponent> io;
    int pauses = 0;
    io = std::make_unique<XArmIoComponent>(
        XArmIoComponentConfig{"lite6_controller_io",
                              XArmIoBank::kController,
                              {{"CI0", IoSignalDirection::kInput, 0}},
                              XArmIoSettings{},
                              std::chrono::seconds(2),
                              [&] {
                                clock.advance(20 * kMs);
                                if (arrives && ++pauses == 5)
                                  static_cast<void>(io->OnIoStates({0, 0, 0, 0, clock.now()}));
                              }},
        driver, [](std::int64_t) { return true; }, clock);
    const auto started = clock.now();
    const auto made = io->Run(ComponentAction::kPrepare, {});
    if (arrives) {
      EXPECT_EQ(made.outcome, ActionOutcome::kSucceeded);
      EXPECT_EQ(pauses, 5);
      EXPECT_EQ(io->State(clock.now()).lifecycle, ComponentLifecycle::kPrepared);
    } else {
      EXPECT_EQ(made.outcome, ActionOutcome::kFailed);
      EXPECT_EQ(made.error->code, "fault_stream_stale");
      EXPECT_GE(clock.now() - started, 2'000 * kMs);
    }
  }
}

TEST(XArmIoComponent, AStaleStreamFaultsTheMemberOnlyWhileItsOwnerIsUsable) {
  World world;
  EXPECT_EQ(world.io->Run(ComponentAction::kPrepare, {}).error->code, "fault_stream_stale");
  world.Enable();
  EXPECT_EQ(world.io->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
  // Controller errors and the GPIO module's are the owner's faults.
  world.io->OnGpioModuleStatus({6, 33, {}});
  EXPECT_EQ(world.io->State(world.clock.now()).lifecycle, ComponentLifecycle::kEnabled);
  world.clock.advance(101 * kMs);
  auto state = world.io->State(world.clock.now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(state.fault->code, core::FaultCode::kStreamStale);
  EXPECT_EQ(state.fault->since_ns, world.clock.now());
  EXPECT_EQ(state.streams[0].stream_id, "io_states");
  EXPECT_EQ(state.streams[0].freshness, core::StreamFreshness::kStale);
  EXPECT_FALSE(world.Read("CO1").fresh);
  // The owner isn't usable (shut down, rebooting, lost): the core shows the
  // member offline through it.
  world.owner_usable = false;
  state = world.io->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
}

} // namespace
} // namespace manyforge::xarm_control
