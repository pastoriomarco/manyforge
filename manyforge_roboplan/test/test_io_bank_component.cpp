// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

// The `ros_io_bank` component with a fake link: reading and freshness, the
// command's decision in the state (accepted, refused per output, unset), its
// unknown ends (K samples, stale stream, device restart) for every write in
// it, a late effect, the sequence start, outside changes, refusals before
// sending, Stop before and after the send, merging of waiting writes, the
// lifecycle and the family's row check.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/io_bank_component.hpp>

namespace manyforge::roboplan_backend {
namespace {

using content_identity::CanonicalValue;
using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentActivity;
using core::ComponentLifecycle;
using core::IoSignalDirection;
using Source = core::IoSignalSampleSource;

constexpr std::int64_t kMs = 1'000'000;
/// Ten periods at 100 Hz.
constexpr std::int64_t kStale = 100 * kMs;

class FakeLink final : public IIoBankLink {
public:
  bool Send(const IoBankCommandMessage &command) override {
    {
      std::lock_guard lock(mutex);
      commands.push_back(command);
    }
    if (on_send) on_send(command);
    return sends;
  }
  void Record(const IoBankWriteRecord &record) override {
    std::lock_guard lock(mutex);
    records.push_back(record);
  }
  /// The outputs a command sets: index -> value.
  static std::vector<std::pair<int, bool>> Set(const IoBankCommandMessage &command) {
    std::vector<std::pair<int, bool>> set;
    for (std::size_t index = 0U; index < command.values.size(); ++index)
      if (command.values[index] != kIoBankOutputKeep)
        set.emplace_back(static_cast<int>(index), command.values[index] == kIoBankOutputOn);
    return set;
  }

  std::mutex mutex;

  std::function<void(const IoBankCommandMessage &)> on_send;
  bool sends{true};
  std::vector<IoBankCommandMessage> commands;
  std::vector<IoBankWriteRecord> records;
};

struct World final {
  World() {
    clock.set(1'000 * kMs);
    std::vector<IoBankSignal> signals{{"I0", IoSignalDirection::kInput, 0}};
    for (int index = 0; index < 16; ++index)
      signals.push_back({"Q" + std::to_string(index), IoSignalDirection::kOutput, index});
    IoBankComponentConfig config{"plc_io", signals, 100.0,
                                 std::chrono::milliseconds(100),
                                 [this] { clock.advance(20 * kMs); }};
    bank = std::make_shared<IoBankComponent>(std::move(config), clock);
    link = std::make_shared<FakeLink>();
    bank->Attach(link);
  }

  /// One state sample now; `cycle` grows unless given.
  std::vector<core::IoSignalUpdateDTO> Sample(const std::uint64_t outputs,
                                              const std::uint32_t applied_seq,
                                              const std::uint8_t result = kIoBankResultAccepted,
                                              const std::uint16_t code = 0,
                                              std::optional<std::uint32_t> at_cycle = {},
                                              const std::uint64_t inputs = 0U,
                                              const std::uint64_t refused = 0U) {
    cycle = at_cycle.value_or(cycle + 1U);
    return bank->OnState(
        {cycle, applied_seq, result, code, inputs, outputs, refused, clock.now()});
  }

  /// Starts a write on its own thread and waits until it is waiting.
  void Background(const std::string &name, const bool value) {
    const auto before = bank->WaitingWrites();
    threads.emplace_back([this, name, value] {
      auto result = bank->SetOutput(name, value, {});
      std::lock_guard lock(results_mutex);
      results.emplace_back(name, result);
    });
    while (bank->WaitingWrites() == before) std::this_thread::yield();
  }
  void Join() {
    for (auto &thread : threads) thread.join();
    threads.clear();
  }

  void Enable(const std::uint32_t applied_seq = 0U) {
    static_cast<void>(Sample(0U, applied_seq));
    ASSERT_EQ(bank->Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  }

  [[nodiscard]] core::SignalSample Read(const std::string &name) const {
    for (auto &sample : bank->Samples(clock.now()))
      if (sample.signal_id.name == name) return sample;
    throw std::runtime_error("no sample " + name);
  }
  [[nodiscard]] bool Value(const std::string &name) const {
    return std::get<bool>(*Read(name).value);
  }

  core::FakeClock clock;
  std::shared_ptr<IoBankComponent> bank;
  std::shared_ptr<FakeLink> link;
  std::uint32_t cycle{100U};
  std::vector<std::thread> threads;
  std::mutex results_mutex;
  std::vector<std::pair<std::string, roboplan_backend::SetOutputResult>> results;
};

TEST(IoBankComponent, SamplesGiveInputsAndOutputsAndARepeatedCycleIsNotFresh) {
  World world;
  auto updates = world.Sample(1U << 5U, 0U, 0U, 0U, std::nullopt, 1U);
  ASSERT_EQ(updates.size(), 17U);
  EXPECT_EQ(updates[0].value, true);   // I0
  EXPECT_EQ(updates[1].value, false);  // Q0
  EXPECT_EQ(updates[6].value, true);   // Q5
  world.clock.advance(60 * kMs);
  EXPECT_TRUE(world.Sample(0U, 0U, 0U, 0U, world.cycle).empty());  // repeat: not fresh
  world.clock.advance(60 * kMs);
  EXPECT_FALSE(world.Read("Q5").fresh);
  EXPECT_EQ(world.bank->capabilities(),
            (std::vector<core::Capability>{core::Capability::kIoManualWrite,
                                           core::Capability::kIoOutputReadback}));
}

TEST(IoBankComponent, AnAcceptedWriteEndsOnItsSequenceAndIgnoresEarlierSamples) {
  World world;
  world.Enable(41U);
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    // The value shows before the sequence (the device's order): ignored.
    static_cast<void>(world.Sample(1U, 41U));
    EXPECT_FALSE(world.Value("Q0"));
    EXPECT_EQ(world.Read("Q0").source, Source::kDevice);
    world.clock.advance(5 * kMs);
    static_cast<void>(world.Sample(1U, command.seq));
  };
  const auto written = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(world.link->commands.size(), 1U);
  EXPECT_EQ(world.link->commands[0].seq, 42U);  // the first sample's applied_seq + 1
  EXPECT_EQ(FakeLink::Set(world.link->commands[0]),
            (std::vector<std::pair<int, bool>>{{0, true}}));
  EXPECT_TRUE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kManyforgeWrite);
  ASSERT_EQ(world.link->records.size(), 1U);
  EXPECT_EQ(*world.link->records[0].decided_at_ns - world.link->records[0].sent_at_ns, 5 * kMs);
  EXPECT_EQ(world.link->records[0].seq, 42U);
  EXPECT_EQ(world.link->records[0].outputs, 1U);
  EXPECT_EQ(world.link->records[0].outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kIdle);
  // The next write takes the next number.
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(1U | (1U << 5U), command.seq));
  };
  EXPECT_EQ(world.bank->SetOutput("Q5", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.link->commands[1].seq, 43U);
  EXPECT_EQ(FakeLink::Set(world.link->commands[1]),
            (std::vector<std::pair<int, bool>>{{5, true}}));
}

TEST(IoBankComponent, TheSequenceNeverTakesTheDevicesLastAppliedNumber) {
  World world;
  world.Enable(0xFFFFFFFFU);
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(1U, command.seq));
  };
  ASSERT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.link->commands[0].seq, 0U);  // wraps
  // The device applied 0; a restart-free run keeps counting.
  ASSERT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.link->commands[1].seq, 1U);
}

TEST(IoBankComponent, AnAcceptedWriteWhoseSampleShowsAnotherValueIsAnOutsideChange) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(0U, command.seq));
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_FALSE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kOutside);
}

TEST(IoBankComponent, ARefusalFailsWithTheDevicesCode) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(0U, command.seq, kIoBankResultRefused, 7U, std::nullopt, 0U, 1U));
  };
  const auto refused = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(refused.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(refused.error->code, "io_output_source_rejected");
  EXPECT_EQ(refused.error->vendor_code, "device_code=7");
  EXPECT_FALSE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kDevice);
  // Refused, yet a sample in flight showed the value: something else set it.
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(1U, command.seq - 1U, kIoBankResultRefused, 7U, std::nullopt, 0U, 1U));
    static_cast<void>(world.Sample(1U, command.seq, kIoBankResultRefused, 7U, std::nullopt, 0U, 1U));
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kFailed);
  EXPECT_TRUE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kOutside);
}

TEST(IoBankComponent, AnUnsetResultIsUnknown) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(0U, command.seq, kIoBankResultUnset));
  };
  const auto written = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(written.error->code, "io_output_acceptance_unknown");
  EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kUnknown);
}

TEST(IoBankComponent, NoDecisionWithinKSamplesIsUnknownUntilTheNextSample) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &) {
    for (std::uint32_t sample = 1U; sample < kIoBankUnansweredSamples; ++sample)
      static_cast<void>(world.Sample(0U, 0U));
    EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kBusy);
    static_cast<void>(world.Sample(0U, 0U));  // the K-th
  };
  const auto written = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(written.error->code, "io_output_acceptance_unknown");
  EXPECT_FALSE(world.link->records[0].decided_at_ns.has_value());
  EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kUnknown);
  static_cast<void>(world.Sample(0U, 0U));
  EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kIdle);
  EXPECT_FALSE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kDevice);
  EXPECT_EQ(world.link->commands.size(), 1U);  // never resent
}

TEST(IoBankComponent, AStaleStreamEndsTheWriteUnknownAndALateEffectIsItsOwn) {
  World world;
  world.Enable();
  std::uint32_t seq = 0U;
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    seq = command.seq;
    world.clock.advance(kStale + 1);
  };
  const auto written = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(written.error->code, "io_output_acceptance_unknown");
  EXPECT_EQ(world.bank->State(world.clock.now()).fault->code, core::FaultCode::kStreamStale);
  // The stream returns; the device applied it late.
  static_cast<void>(world.Sample(0U, 0U));
  EXPECT_EQ(world.Read("Q0").source, Source::kDevice);
  static_cast<void>(world.Sample(1U, seq));
  EXPECT_TRUE(world.Value("Q0"));
  EXPECT_EQ(world.Read("Q0").source, Source::kManyforgeWrite);
}

TEST(IoBankComponent, ADeviceRestartEndsAPendingWriteUnknownAndRestartsTheSequence) {
  World world;
  world.Enable(10U);
  world.link->on_send = [&](const IoBankCommandMessage &) {
    // The cycle goes back: a new epoch; the values are the device's.
    static_cast<void>(world.Sample(0U, 3U, kIoBankResultUnset, 0U, 1U));
  };
  const auto written = world.bank->SetOutput("Q0", true, {});
  EXPECT_EQ(written.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(world.Read("Q0").source, Source::kDevice);
  EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kIdle);
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    static_cast<void>(world.Sample(1U, command.seq));
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.link->commands.back().seq, 4U);  // the new epoch's applied_seq + 1
}

TEST(IoBankComponent, AnOutputChangedWithoutAWriteIsAnOutsideChange) {
  World world;
  world.Enable();
  static_cast<void>(world.Sample(1U << 5U, 0U));
  EXPECT_TRUE(world.Value("Q5"));
  EXPECT_EQ(world.Read("Q5").source, Source::kOutside);
  EXPECT_FALSE(world.bank->State(world.clock.now()).fault.has_value());
}

TEST(IoBankComponent, WritesAreRefusedBeforeSendingWhenTheyCannotBeSent) {
  World world;
  static_cast<void>(world.Sample(0U, 0U));
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).error->code, "io_output_not_submitted");
  ASSERT_EQ(world.bank->Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.bank->SetOutput("Q99", true, {}).error->code, "io_signal_unknown");
  EXPECT_EQ(world.bank->SetOutput("I0", true, {}).error->code, "io_signal_unknown");
  EXPECT_EQ(world.bank->SetOutput("Q0", 1.0, {}).error->code, "io_signal_type_mismatch");
  std::stop_source stop;
  stop.request_stop();
  EXPECT_EQ(world.bank->SetOutput("Q0", true, stop.get_token()).error->code,
            "component_action_interrupted_by_stop");
  EXPECT_TRUE(world.link->commands.empty());
  world.link->sends = false;
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).error->code, "io_output_not_submitted");
  world.link->sends = true;
  world.clock.advance(kStale + 1);
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).error->code, "io_output_not_submitted");
  EXPECT_EQ(world.link->commands.size(), 1U);  // only the one the link refused
}

TEST(IoBankComponent, AStopAfterTheSendLetsTheWriteComplete) {
  World world;
  world.Enable();
  std::stop_source stop;
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    stop.request_stop();
    static_cast<void>(world.Sample(1U, command.seq));
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, stop.get_token()).outcome,
            ActionOutcome::kSucceeded);
}

TEST(IoBankComponent, WritesWaitingDuringACommandGoOutAsOneCommand) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    if (command.values[0] != kIoBankOutputKeep) {
      // Twelve writes arrive while the first command is in flight: they wait.
      for (int index = 1; index <= 12; ++index) world.Background("Q" + std::to_string(index), true);
      EXPECT_EQ(world.link->commands.size(), 1U);
      EXPECT_EQ(world.bank->State(world.clock.now()).activity, ComponentActivity::kBusy);
      static_cast<void>(world.Sample(1U, command.seq));
    } else {
      static_cast<void>(world.Sample(0x1FFFU, command.seq));
    }
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  world.Join();
  ASSERT_EQ(world.link->commands.size(), 2U);
  EXPECT_EQ(world.link->commands[1].seq, world.link->commands[0].seq + 1U);
  EXPECT_EQ(FakeLink::Set(world.link->commands[1]).size(), 12U);
  ASSERT_EQ(world.results.size(), 12U);
  for (const auto &[name, result] : world.results) {
    EXPECT_EQ(result.outcome, ActionOutcome::kSucceeded) << name;
    EXPECT_TRUE(world.Value(name)) << name;
    EXPECT_EQ(world.Read(name).source, Source::kManyforgeWrite) << name;
  }
  // One record per command.
  ASSERT_EQ(world.link->records.size(), 2U);
  EXPECT_EQ(world.link->records[0].outputs + world.link->records[1].outputs, 13U);
}

TEST(IoBankComponent, TheSameOutputTwiceGoesOutInTwoCommandsInOrder) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    if (command.values[5] != kIoBankOutputKeep) {
      // A pulse on Q0 (on, then off) and Q1 meanwhile.
      world.Background("Q0", true);
      world.Background("Q0", false);
      world.Background("Q1", true);
    }
    const bool q0 = command.values[0] == kIoBankOutputOn;
    static_cast<void>(world.Sample((q0 ? 1U : 0U) | (1U << 5U) | 2U, command.seq));
  };
  EXPECT_EQ(world.bank->SetOutput("Q5", true, {}).outcome, ActionOutcome::kSucceeded);
  world.Join();
  ASSERT_EQ(world.link->commands.size(), 3U);
  EXPECT_EQ(FakeLink::Set(world.link->commands[1]),
            (std::vector<std::pair<int, bool>>{{0, true}, {1, true}}));
  EXPECT_EQ(FakeLink::Set(world.link->commands[2]),
            (std::vector<std::pair<int, bool>>{{0, false}}));
  for (const auto &[name, result] : world.results)
    EXPECT_EQ(result.outcome, ActionOutcome::kSucceeded) << name;
  EXPECT_FALSE(world.Value("Q0"));
}

TEST(IoBankComponent, APartialRefusalFailsOnlyTheRefusedOutputs) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    if (command.values[0] != kIoBankOutputKeep && FakeLink::Set(command).size() == 1U) {
      world.Background("Q1", true);
      world.Background("Q2", true);
      static_cast<void>(world.Sample(1U, command.seq));
    } else {
      // Q2 refused (code 9); Q1 accepted.
      static_cast<void>(world.Sample(1U | 2U, command.seq, kIoBankResultRefused, 9U,
                                     std::nullopt, 0U, 4U));
    }
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  world.Join();
  ASSERT_EQ(world.results.size(), 2U);
  for (const auto &[name, result] : world.results) {
    if (name == "Q1") {
      EXPECT_EQ(result.outcome, ActionOutcome::kSucceeded);
      EXPECT_EQ(world.Read("Q1").source, Source::kManyforgeWrite);
    } else {
      EXPECT_EQ(result.outcome, ActionOutcome::kFailed);
      EXPECT_EQ(result.error->code, "io_output_source_rejected");
      EXPECT_EQ(result.error->vendor_code, "device_code=9");
      EXPECT_FALSE(world.Value("Q2"));
    }
  }
  EXPECT_EQ(world.link->records.back().outcome, ActionOutcome::kFailed);
}

TEST(IoBankComponent, EveryWriteOfAnUnansweredCommandIsUnknown) {
  World world;
  world.Enable();
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    if (FakeLink::Set(command).size() == 1U && command.values[0] != kIoBankOutputKeep) {
      world.Background("Q1", true);
      world.Background("Q2", true);
      static_cast<void>(world.Sample(1U, command.seq));
    } else {
      for (std::uint32_t sample = 0U; sample < kIoBankUnansweredSamples; ++sample)
        static_cast<void>(world.Sample(1U, command.seq - 1U));
    }
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  world.Join();
  ASSERT_EQ(world.results.size(), 2U);
  for (const auto &[name, result] : world.results) {
    EXPECT_EQ(result.outcome, ActionOutcome::kUnknown) << name;
    EXPECT_EQ(result.error->code, "io_output_acceptance_unknown") << name;
  }
  EXPECT_EQ(world.link->commands.size(), 2U);  // never resent
}

TEST(IoBankComponent, AStopBeforeTheSendRemovesOnlyThatWrite) {
  World world;
  world.Enable();
  std::stop_source stop;
  std::optional<roboplan_backend::SetOutputResult> stopped;
  std::thread stopped_writer;
  world.link->on_send = [&](const IoBankCommandMessage &command) {
    if (FakeLink::Set(command).size() == 1U && command.values[0] != kIoBankOutputKeep) {
      world.Background("Q1", true);
      stopped_writer = std::thread([&] { stopped = world.bank->SetOutput("Q2", true, stop.get_token()); });
      while (world.bank->WaitingWrites() < 2U) std::this_thread::yield();
      stop.request_stop();
      while (world.bank->WaitingWrites() > 1U) std::this_thread::yield();
      static_cast<void>(world.Sample(1U, command.seq));
    } else {
      static_cast<void>(world.Sample(3U, command.seq));
    }
  };
  EXPECT_EQ(world.bank->SetOutput("Q0", true, {}).outcome, ActionOutcome::kSucceeded);
  stopped_writer.join();
  world.Join();
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->error->code, "component_action_interrupted_by_stop");
  ASSERT_EQ(world.link->commands.size(), 2U);
  EXPECT_EQ(FakeLink::Set(world.link->commands[1]),
            (std::vector<std::pair<int, bool>>{{1, true}}));
  EXPECT_EQ(world.results.front().second.outcome, ActionOutcome::kSucceeded);
}

TEST(IoBankComponent, LifecycleWaitsForAFreshSampleAndAStaleStreamIsAFault) {
  World world;
  const auto refused = world.bank->Run(ComponentAction::kPrepare, {});
  EXPECT_EQ(refused.outcome, ActionOutcome::kFailed);
  EXPECT_EQ(refused.error->code, "fault_stream_stale");
  EXPECT_FALSE(world.bank->State(world.clock.now()).fault.has_value());  // not prepared
  static_cast<void>(world.Sample(0U, 0U));
  ASSERT_EQ(world.bank->Run(ComponentAction::kPrepare, {}).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(world.bank->State(world.clock.now()).lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(world.bank->State(world.clock.now()).streams[0].freshness,
            core::StreamFreshness::kFresh);
  world.clock.advance(kStale + 1);
  auto state = world.bank->State(world.clock.now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(state.fault->code, core::FaultCode::kStreamStale);
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  EXPECT_FALSE(state.fault->vendor_code.has_value());
  // Samples still arrive, with a frozen cycle: the device's program stopped.
  static_cast<void>(world.Sample(0U, 0U, 0U, 0U, world.cycle));
  state = world.bank->State(world.clock.now());
  EXPECT_EQ(state.fault->vendor_code, "cycle_stopped");
  // Fresh again: the fault ends by itself.
  static_cast<void>(world.Sample(0U, 0U));
  EXPECT_FALSE(world.bank->State(world.clock.now()).fault.has_value());
  EXPECT_EQ(world.bank->Run(ComponentAction::kReboot, {}).error->code,
            "component_action_unsupported");
}

CanonicalValue Row(CanonicalValue::Object fields) { return CanonicalValue(std::move(fields)); }

TEST(IoBankComponent, TheFamilyChecksItsRow) {
  const CanonicalValue::Object base{
      {"adapter", CanonicalValue("ros_io_bank")},
      {"rate_hz", CanonicalValue(200.0)},
      {"inputs", CanonicalValue(CanonicalValue::Array{CanonicalValue("I0")})},
      {"outputs", CanonicalValue(CanonicalValue::Array{CanonicalValue("Q0")})},
      {"indexes", CanonicalValue(CanonicalValue::Object{{"I0", CanonicalValue(0.0)},
                                                        {"Q0", CanonicalValue(63.0)}})}};
  const auto config = io_bank_config_from_row("plc_io", Row(base));
  ASSERT_EQ(config.signals.size(), 2U);
  EXPECT_EQ(config.signals[1].index, 63);
  EXPECT_EQ(config.rate_hz, 200.0);
  const auto with = [&](const std::string &key, CanonicalValue value) {
    auto fields = base;
    for (auto &[name, field] : fields)
      if (name == key) field = value;
    if (std::none_of(fields.begin(), fields.end(),
                     [&](const auto &field) { return field.first == key; }))
      fields.emplace_back(key, value);
    return Row(fields);
  };
  // Index required, whole, 0..63.
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row(
                   "plc_io", with("indexes", CanonicalValue(CanonicalValue::Object{
                                                 {"I0", CanonicalValue(0.0)}})))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row(
                   "plc_io", with("indexes", CanonicalValue(CanonicalValue::Object{
                                                 {"I0", CanonicalValue(0.0)},
                                                 {"Q0", CanonicalValue(64.0)}})))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row(
                   "plc_io", with("indexes", CanonicalValue(CanonicalValue::Object{
                                                 {"I0", CanonicalValue(0.5)},
                                                 {"Q0", CanonicalValue(1.0)}})))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row("plc_io", with("rate_hz", CanonicalValue(0.0)))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row("plc_io", with("adapter", CanonicalValue("xarm")))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(io_bank_config_from_row("plc_io", with("bank", CanonicalValue("tool")))),
               std::invalid_argument);
  // Unique per direction: the same index for an input and an output is fine.
  core::FakeClock clock;
  auto twice = io_bank_config_from_row(
      "plc_io", with("indexes", CanonicalValue(CanonicalValue::Object{
                                    {"I0", CanonicalValue(3.0)}, {"Q0", CanonicalValue(3.0)}})));
  EXPECT_NO_THROW(static_cast<void>(IoBankComponent(twice, clock)));
  twice.signals.push_back({"Q1", IoSignalDirection::kOutput, 3});
  EXPECT_THROW(static_cast<void>(IoBankComponent(twice, clock)), std::invalid_argument);
}

} // namespace
} // namespace manyforge::roboplan_backend
