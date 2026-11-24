// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

// The shared I/O bank tracker: device samples, accepted writes with old
// samples ignored, outside changes, unanswered writes settled by the next
// sample, held writes and a device restart.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <manyforge_roboplan/io_bank_tracker.hpp>

namespace manyforge::roboplan_backend {
namespace {

using core::IoSignalDirection;
using Source = core::IoSignalSampleSource;

constexpr std::int64_t kStale = 100;

IoBankTracker MakeTracker() {
  return IoBankTracker("plc_io",
                       {{"I0", IoSignalDirection::kInput, 0},
                        {"Q0", IoSignalDirection::kOutput, 0},
                        {"Q63", IoSignalDirection::kOutput, 63}},
                       kStale);
}

core::SignalSample Read(const IoBankTracker &tracker, const std::string &name,
                        const std::int64_t now = 0) {
  for (const auto &sample : tracker.Samples(now))
    if (sample.signal_id.name == name) return sample;
  throw std::runtime_error("no sample " + name);
}

bool Value(const IoBankTracker &tracker, const std::string &name) {
  return std::get<bool>(*Read(tracker, name).value);
}

constexpr std::uint64_t kQ63 = std::uint64_t{1} << 63;

TEST(IoBankTracker, SamplesReportEveryBitFromTheDevice) {
  auto tracker = MakeTracker();
  const auto updates = tracker.OnSample(1U, kQ63, 10, 1);
  ASSERT_EQ(updates.size(), 3U);
  EXPECT_EQ(updates[0].signal_id.controllable_component_id.value(), "plc_io");
  EXPECT_EQ(updates[0].value, true);
  EXPECT_EQ(updates[1].value, false);
  EXPECT_EQ(updates[2].value, true);
  EXPECT_EQ(updates[2].source, Source::kDevice);
  EXPECT_TRUE(tracker.Fresh(10 + kStale));
  EXPECT_FALSE(tracker.Fresh(11 + kStale));
  EXPECT_EQ(Read(tracker, "Q63", 30).age_ns, 20);
  EXPECT_THROW(IoBankTracker("x", {{"Q64", IoSignalDirection::kOutput, 64}}, 1),
               std::invalid_argument);
}

TEST(IoBankTracker, AnAcceptedWriteIgnoresOldSamplesUntilItsBound) {
  auto tracker = MakeTracker();
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 1));
  tracker.WriteAccepted(1U, true, 3);
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kManyforgeWrite);
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 3));  // old value, within the bound
  EXPECT_TRUE(Value(tracker, "Q0"));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kManyforgeWrite);
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 4));  // still old after the bound
  EXPECT_FALSE(Value(tracker, "Q0"));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kOutside);
}

TEST(IoBankTracker, TheWrittenValueSeenConfirmsAndALaterChangeIsOutside) {
  auto tracker = MakeTracker();
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 1));
  tracker.WriteAccepted(1U, true, 1);
  static_cast<void>(tracker.OnSample(0U, 1U, 0, 2));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kManyforgeWrite);
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 3));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kOutside);
  EXPECT_FALSE(Value(tracker, "Q0"));
}

TEST(IoBankTracker, AnUnansweredWriteIsUnknownUntilTheNextSample) {
  auto tracker = MakeTracker();
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 1));
  tracker.WriteUnanswered(1U, true);
  EXPECT_TRUE(tracker.AnyUnknown());
  static_cast<void>(tracker.OnSample(0U, 1U, 0, 2));
  EXPECT_FALSE(tracker.AnyUnknown());
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kManyforgeWrite);
  tracker.WriteUnanswered(2U, false);
  static_cast<void>(tracker.OnSample(0U, 1U, 0, 3));  // Q63 unchanged: still the device's
  EXPECT_EQ(Read(tracker, "Q63").source, Source::kDevice);
}

TEST(IoBankTracker, AHeldOutputIgnoresSamplesUntilItsWriteIsDecided) {
  auto tracker = MakeTracker();
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 1));
  tracker.Hold(1U);
  static_cast<void>(tracker.OnSample(0U, 1U, 0, 2));  // the effect before the decision
  EXPECT_FALSE(Value(tracker, "Q0"));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kDevice);
  // Refused, yet the output changed: something else set it.
  tracker.Release(1U);
  static_cast<void>(tracker.OnSample(0U, 1U, 0, 3));
  EXPECT_TRUE(Value(tracker, "Q0"));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kOutside);
}

TEST(IoBankTracker, ANewEpochReportsEveryValueFromTheDevice) {
  auto tracker = MakeTracker();
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 1));
  tracker.WriteAccepted(1U, true, 1);
  tracker.WriteUnanswered(2U, true);
  tracker.NewEpoch();
  EXPECT_FALSE(tracker.AnyUnknown());
  static_cast<void>(tracker.OnSample(0U, 0U, 0, 2));
  EXPECT_FALSE(Value(tracker, "Q0"));
  EXPECT_EQ(Read(tracker, "Q0").source, Source::kDevice);
  EXPECT_EQ(Read(tracker, "Q63").source, Source::kDevice);
}

} // namespace
} // namespace manyforge::roboplan_backend
