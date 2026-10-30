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

// R7 package 3: the xArm vendor adapter classifies `robot_states` into the
// common cause classes, the D9 device facts and the vendor codes.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <vector>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>
#include <manyforge_xarm_control/xarm_brand_lifecycle_port.hpp>

namespace manyforge::xarm_control {
namespace {

#ifndef MANYFORGE_XARM_CONTROL_SOURCE_ROOT
#error "the xArm control source root must be build-owned"
#endif

using Cause = core::LifecycleBlockingCauseClassDTO;

constexpr std::int64_t kNow = 100'000'000'000LL;

class FakeTransport final : public IXArmBrandLifecycleTransport {
public:
  [[nodiscard]] XArmObservedState observe() const override { return report; }
  XArmObservedState report;
};

struct Fixture {
  explicit Fixture(XArmExternalMoveRearm policy = XArmExternalMoveRearm::kAutomatic)
      : port(transport, {kXArmStatusFreshnessNs, 6, kXArmJointAgreementToleranceRad,
                         policy}) {
    // The Real hardware starts inactive: observed unarmed.
    port.ObserveHardwareActive(false);
  }

  std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
  XArmBrandLifecyclePort port;

  /// m5.5: ManyForge armed the robot (its hardware activation completed:
  /// servo mode, ready, every axis enabled); no own primitive is in flight.
  void Arm(const std::int64_t at = kNow) {
    Report(4, 0, 0, 0, 0, at - 10'000'000);
    static_cast<void>(port.Observe(at));
    port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
    Report(2, 1, 0, 0, 0x3F, at - 10'000'000);
    static_cast<void>(port.Observe(at));
    port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, at);
    static_cast<void>(port.Observe(at));
  }

  /// A report received at `kNow - 10 ms` (fresh at `kNow`).
  void Report(const int state, const int mode, const int err,
              const int warn = 0, const int mt_able = 0x3F,
              const std::int64_t received = kNow - 10'000'000,
              std::vector<double> angles = {0.1, -0.2, 0.3, 0.0, 0.5, 0.6}) {
    transport->report = {received, state, mode, err, warn, mt_able, std::move(angles)};
  }
};

[[nodiscard]] const core::LifecycleFactDTO *
find_fact(const XArmDeviceStatus &status, const std::string_view id) {
  const auto found = std::find_if(
      status.facts.begin(), status.facts.end(), [&](const auto &fact) {
        return fact.lifecycle_fact_id.value() == id;
      });
  return found == status.facts.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<bool> boolean_fact(const XArmDeviceStatus &status,
                                               const std::string_view id) {
  const auto *fact = find_fact(status, id);
  if (fact == nullptr) {
    throw std::logic_error("missing fact " + std::string(id));
  }
  return fact->value.boolean_value;
}

[[nodiscard]] std::optional<std::string> token_fact(const XArmDeviceStatus &status,
                                                    const std::string_view id) {
  const auto *fact = find_fact(status, id);
  return fact == nullptr ? std::nullopt : fact->value.token_value;
}

[[nodiscard]] std::optional<std::int64_t>
integer_fact(const XArmDeviceStatus &status, const std::string_view id) {
  const auto *fact = find_fact(status, id);
  return fact == nullptr ? std::nullopt : fact->value.integer_value;
}

void expect_sorted_and_valid(const XArmDeviceStatus &status) {
  EXPECT_TRUE(std::is_sorted(status.facts.begin(), status.facts.end(),
                             [](const auto &left, const auto &right) {
                               return left.lifecycle_fact_id <
                                      right.lifecycle_fact_id;
                             }));
  for (const auto &fact : status.facts) {
    EXPECT_TRUE(fact.locally_valid()) << fact.lifecycle_fact_id.value();
  }
}

TEST(XArmBrandLifecyclePort, ArmedCleanReportIsReadyWithDeviceFacts) {
  Fixture fixture;
  fixture.Report(2, 1, 0);
  const auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  expect_sorted_and_valid(status);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), false);
  EXPECT_EQ(boolean_fact(status, core::kDeviceWarningPresentFactId), false);
  EXPECT_EQ(boolean_fact(status, core::kDeviceMotionEnabledFactId), true);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), true);
  EXPECT_EQ(integer_fact(status, kVendorCodeModeFactId), 1);
  EXPECT_EQ(integer_fact(status, kVendorCodeStateFactId), 2);
  EXPECT_EQ(find_fact(status, kVendorCodeErrorFactId), nullptr);
  EXPECT_EQ(find_fact(status, kVendorCodeWarningFactId), nullptr);
  EXPECT_EQ(find_fact(status, kVendorCodePowerCycleFactId), nullptr);

  // Moving in joint-servo mode also accepts commands.
  fixture.Report(1, 1, 0);
  EXPECT_EQ(boolean_fact(fixture.port.Observe(kNow),
                         core::kDeviceAcceptsCommandsFactId),
            true);
}

TEST(XArmBrandLifecyclePort, ConnectedNotArmedIsCleanButDoesNotAcceptCommands) {
  // After Apply the hardware is inactive: the robot is stopped in position
  // mode with its servos disabled. That is clean status, not a cause.
  Fixture fixture;
  fixture.Report(4, 0, 0, 0, 0);
  const auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), false);
  EXPECT_EQ(boolean_fact(status, core::kDeviceMotionEnabledFactId), false);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);

  // Some, not all, axes enabled is not motion enabled; ready state and
  // joint-servo mode alone do not accept commands either.
  fixture.Report(2, 1, 0, 0, 0x1F);
  const auto partial = fixture.port.Observe(kNow);
  EXPECT_EQ(boolean_fact(partial, core::kDeviceMotionEnabledFactId), false);
  EXPECT_EQ(boolean_fact(partial, core::kDeviceAcceptsCommandsFactId), false);
}

TEST(XArmBrandLifecyclePort, EmergencyStopCodesAreEmergencyStopWithTheCode) {
  for (const int code : {1, 2, 3}) {
    Fixture fixture;
    fixture.Arm();
    // The live probe's report under a pressed emergency stop: C2, state 4,
    // mode 0, no axis enabled.
    fixture.Report(4, 0, code, 0, 0);
    const auto status = fixture.port.Observe(kNow);
    EXPECT_EQ(status.cause, Cause::kEmergencyStop) << code;
    expect_sorted_and_valid(status);
    EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId),
              "C" + std::to_string(code));
    EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), true);
    EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);
    EXPECT_EQ(integer_fact(status, kVendorCodeStateFactId), 4);
    EXPECT_EQ(integer_fact(status, kVendorCodeModeFactId), 0);
    EXPECT_EQ(find_fact(status, kVendorCodePowerCycleFactId), nullptr);
  }
}

TEST(XArmBrandLifecyclePort, CollisionAndOtherErrorsAreClassifiedByCode) {
  Fixture collision;
  collision.Arm();
  collision.Report(4, 0, 31);
  EXPECT_EQ(collision.port.Observe(kNow).cause, Cause::kCollision);

  for (const int code : {10, 13, 17}) {
    // Never cleared, so a cause also on the unarmed robot.
    Fixture servo;
    servo.Report(4, 0, code);
    const auto status = servo.port.Observe(kNow);
    EXPECT_EQ(status.cause, Cause::kFault) << code;
    EXPECT_EQ(token_fact(status, kVendorCodePowerCycleFactId),
              std::string(kPowerCycleRequiredToken));
    EXPECT_FALSE(servo.port.ClearErrorPermitted());
  }
  for (const int code : {4, 9, 18, 22, 30, 32, -5}) {
    Fixture other;
    other.Arm();
    other.Report(4, 0, code);
    const auto status = other.port.Observe(kNow);
    EXPECT_EQ(status.cause, Cause::kFault) << code;
    EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId),
              "C" + std::to_string(code));
    EXPECT_EQ(find_fact(status, kVendorCodePowerCycleFactId), nullptr);
    EXPECT_TRUE(other.port.ClearErrorPermitted());
  }
}

TEST(XArmBrandLifecyclePort, WarningIsAFactAndACodeButNotACause) {
  Fixture fixture;
  fixture.Report(2, 1, 0, 11);
  const auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceWarningPresentFactId), true);
  EXPECT_EQ(integer_fact(status, kVendorCodeWarningFactId), 11);
}

TEST(XArmBrandLifecyclePort, StaleOrMissingStatusIsAvailabilityWithUnknownFacts) {
  Fixture fixture;
  // No report yet (the driver is still connecting).
  auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kAvailability);
  ASSERT_EQ(status.facts.size(), 4U);
  for (const auto &fact : status.facts) {
    EXPECT_EQ(fact.value.kind, core::LifecycleFactValueKindDTO::kUnknown);
    EXPECT_NE(fact.lifecycle_fact_id.value().rfind(
                  core::kLifecycleVendorCodeFactPrefix, 0),
              0U);
  }

  // Exactly the freshness budget old is still fresh; one nanosecond more is
  // stale, also for an error report (CCLC retains a reported intervention).
  fixture.Arm();
  fixture.Report(4, 0, 2, 0, 0, kNow - kXArmStatusFreshnessNs);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kEmergencyStop);
  status = fixture.port.Observe(kNow + 1);
  EXPECT_EQ(status.cause, Cause::kAvailability);
  EXPECT_EQ(find_fact(status, kVendorCodeErrorFactId), nullptr);
  EXPECT_FALSE(boolean_fact(status, core::kDeviceErrorPresentFactId).has_value());

}

TEST(XArmBrandLifecyclePort, ErrorNeverYieldsUsableStatusAndNoJointValueIsReported) {
  // R7 §3.5: under an error the controller reports all-zero angles. The
  // adapter never carries joint values; an error is an intervention cause
  // that blocks motion until the operator's Recover.
  Fixture fixture;
  fixture.Arm();
  fixture.Report(4, 0, 2, 0, 0);
  const auto status = fixture.port.Observe(kNow);
  EXPECT_TRUE(core::lifecycle_cause_requires_intervention(status.cause));
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);
  const std::set<std::string> allowed{
      std::string(core::kDeviceAcceptsCommandsFactId),
      std::string(core::kDeviceErrorPresentFactId),
      std::string(core::kDeviceMotionEnabledFactId),
      std::string(core::kDeviceWarningPresentFactId),
      std::string(kVendorCodeConditionFactId),
      std::string(kVendorCodeErrorFactId),
      std::string(kVendorCodeModeFactId),
      std::string(kVendorCodePowerCycleFactId),
      std::string(kVendorCodeStateFactId),
      std::string(kVendorCodeWarningFactId)};
  for (const auto &fact : status.facts) {
    EXPECT_TRUE(allowed.contains(fact.lifecycle_fact_id.value()))
        << fact.lifecycle_fact_id.value();
  }
}

TEST(XArmBrandLifecyclePort, UnrequestedModeOrStateChangeIsStickyExternalControl) {
  // The operator policy: while armed, an unrequested change is the
  // `external_control` intervention cause.
  Fixture fixture(XArmExternalMoveRearm::kOperator);
  fixture.Arm();
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  // A vendor-UI move leaves mode 1 / state 2 without a ManyForge request.
  fixture.Report(5, 0, 0, 0, 0x3F);
  auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kExternalControl);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_mode_changed");
  EXPECT_EQ(find_fact(status, core::kDeviceMovedExternallyFactId), nullptr);
  // Sticky: returning to the old pair, and status loss, do not clear it.
  fixture.Report(2, 1, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kExternalControl);
  EXPECT_EQ(fixture.port.Observe(kNow + kXArmStatusFreshnessNs).cause,
            Cause::kAvailability);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kExternalControl);
  // A vendor error code still takes precedence.
  fixture.Report(4, 0, 2, 0, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kEmergencyStop);
  fixture.Report(4, 0, 0, 0, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kExternalControl);
  // ManyForge's next own primitive (after the operator's acknowledgement)
  // re-baselines from the latest fresh report.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearWarning);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
}

TEST(XArmBrandLifecyclePort, ModesManyForgeNeverSelectsAreExternalControl) {
  Fixture fixture(XArmExternalMoveRearm::kOperator);
  fixture.Arm();
  fixture.Report(2, 2, 0);  // teach (gravity-compensated) mode
  auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kExternalControl);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_manual_mode");
  fixture.Report(2, 7, 0);
  EXPECT_EQ(token_fact(fixture.port.Observe(kNow), kVendorCodeConditionFactId),
            "xarm_mode_external");
  // Unarmed, it is only a condition: Prepare re-arms (servo mode, ready).
  Fixture unarmed(XArmExternalMoveRearm::kOperator);
  unarmed.Report(2, 2, 0);
  status = unarmed.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_manual_mode");
}

TEST(XArmBrandLifecyclePort, ServoModeMovingAndReadyAreOneControlState) {
  Fixture fixture;
  fixture.Report(2, 1, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.Report(1, 1, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.Report(2, 1, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
}

TEST(XArmBrandLifecyclePort, OwnPrimitiveEffectsAreNeverExternalControl) {
  Fixture fixture;
  // An error report explains its pair; clearing it is ManyForge's own effect.
  // Unarmed, the error is facts only (m5.5).
  fixture.Report(4, 0, 2, 0, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  EXPECT_TRUE(fixture.port.ClearErrorPermitted());
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  fixture.Report(4, 0, 0, 0, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, kNow);
  EXPECT_EQ(fixture.port.Observe(kNow + 1).cause, Cause::kNone);

  // A primitive that had no effect explains nothing afterwards.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearWarning);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearWarning, false,
                                  kNow);
  fixture.Report(5, 0, 0, 0, 0);
  const auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_mode_changed");
}

TEST(XArmBrandLifecyclePort, ArmingAndDisarmingAreOwnEffectsNeverExternalControl) {
  // R7 package 4: the hardware activation arms through the plugin's
  // `on_activate` (servo mode, ready); the deactivation selects position mode.
  Fixture fixture;
  fixture.Report(4, 0, 0, 0, 0);
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.Report(5, 1, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.Report(2, 1, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, kNow);
  EXPECT_EQ(fixture.port.Observe(kNow + 1).cause, Cause::kNone);

  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kDisarm);
  fixture.Report(2, 0, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow + 2).cause, Cause::kNone);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kDisarm, true, kNow + 2);
  EXPECT_EQ(fixture.port.Observe(kNow + 3).cause, Cause::kNone);
  // Servo mode afterwards, without an own primitive, is someone else's: a
  // condition on the disarmed robot.
  fixture.Report(2, 1, 0, 0, 0x3F);
  const auto status = fixture.port.Observe(kNow + 4);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_moved_externally");
}

TEST(XArmBrandLifecyclePort, DisarmingNeverExplainsServoModeFromPositionMode) {
  Fixture fixture;
  fixture.Report(4, 0, 0, 0, 0);
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kDisarm);
  fixture.Report(2, 1, 0, 0, 0x3F);
  EXPECT_EQ(token_fact(fixture.port.Observe(kNow), kVendorCodeConditionFactId),
            "xarm_moved_externally");
}

// R7 §3.6: a ready report with these angles, received 10 ms before `kNow`.
void ReportReady(Fixture &fixture, std::vector<double> angles) {
  fixture.Report(2, 1, 0, 0, 0x3F);
  fixture.transport->report.joint_angles_rad = std::move(angles);
}

[[nodiscard]] std::string GateReason(const core::Result<void> &result) {
  if (result.ok()) return "ok";
  EXPECT_EQ(result.error().code, "execution_rejected");
  return result.error().details.at("reason");
}

TEST(XArmBrandLifecyclePort, ActivationGateNeedsReadyFreshAgreeingJoints) {
  Fixture fixture;
  const std::vector<double> pose{0.1, -0.2, 0.3, 0.0, 0.5, 0.6};
  // No report, then a stale one.
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)),
            "xarm_status_unavailable");
  ReportReady(fixture, pose);
  fixture.transport->report.received_timestamp_ns =
      kNow - kXArmStatusFreshnessNs - 1;
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)),
            "xarm_status_unavailable");

  // Under an emergency stop the reported angles are zero: never compared.
  fixture.Report(4, 0, 2, 0, 0);
  fixture.transport->report.joint_angles_rad = {0, 0, 0, 0, 0, 0};
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, {0, 0, 0, 0, 0, 0})),
            "xarm_not_ready");
  // Connected, not armed.
  fixture.Report(4, 0, 0, 0, 0);
  fixture.transport->report.joint_angles_rad = pose;
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)),
            "xarm_not_ready");

  // Ready: agreement within the tolerance passes.
  ReportReady(fixture, pose);
  auto close = pose;
  close[2] += 0.5 * kXArmJointAgreementToleranceRad;
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, close)), "ok");
  // A zero hold target against the real pose is refused.
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, {0, 0, 0, 0, 0, 0})),
            "xarm_measured_joints_disagree");
  auto beyond = pose;
  beyond[5] += 2.0 * kXArmJointAgreementToleranceRad;
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, beyond)),
            "xarm_measured_joints_disagree");
  // NaN or a wrong joint count is never agreement.
  auto nan = pose;
  nan[0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, nan)),
            "xarm_joint_angles_unavailable");
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(
                kNow, std::vector<double>(pose.begin(), pose.end() - 1))),
            "xarm_joint_angles_unavailable");
  ReportReady(fixture, {0.1, -0.2, 0.3});
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)),
            "xarm_joint_angles_unavailable");
  // The tolerance is per robot data.
  XArmBrandLifecyclePort strict(fixture.transport,
                                {kXArmStatusFreshnessNs, 6, 1e-6});
  ReportReady(fixture, pose);
  EXPECT_EQ(GateReason(strict.CheckActivationGate(kNow, close)),
            "xarm_measured_joints_disagree");
  EXPECT_THROW(XArmBrandLifecyclePort(fixture.transport,
                                      {kXArmStatusFreshnessNs, 6, 0.0}),
               std::invalid_argument);
}

// Live 2026-10-27 (same race as the measured joints): a `robot_states`
// report lands between the caller taking its boundary and the read. It is
// newer than the boundary, not stale: the health read keeps its device facts
// and the Enable gate does not refuse with `xarm_status_unavailable`.
TEST(XArmBrandLifecyclePort, RealArmStatusReceivedAfterTheBoundaryIsCurrent) {
  Fixture fixture;
  const std::vector<double> pose{0.1, -0.2, 0.3, 0.0, 0.5, 0.6};
  ReportReady(fixture, pose);
  fixture.transport->report.received_timestamp_ns = kNow + 200'000'000;
  const auto health = fixture.port.ReadHealth(kNow);
  EXPECT_EQ(health.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(health, core::kDeviceAcceptsCommandsFactId), true);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)), "ok");
  // A report older than the budget is still stale, for both readers.
  fixture.transport->report.received_timestamp_ns =
      kNow - kXArmStatusFreshnessNs - 1;
  EXPECT_EQ(fixture.port.ReadHealth(kNow).cause, Cause::kAvailability);
  EXPECT_EQ(GateReason(fixture.port.CheckActivationGate(kNow, pose)),
            "xarm_status_unavailable");
}

TEST(XArmBrandLifecyclePort, PureHealthDoesNotCommitVendorHistory) {
  Fixture fixture(XArmExternalMoveRearm::kOperator);
  fixture.Arm();
  fixture.Report(5, 0, 0, 0, 0x3F);
  const auto pure = fixture.port.ReadHealth(kNow);
  EXPECT_EQ(pure.cause, Cause::kExternalControl);
  // The pure read left the explained pair alone: a return to it is clean,
  // and the committed Observe of the change still detects it.
  fixture.Report(2, 1, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.ReadHealth(kNow).cause, Cause::kNone);
  fixture.Report(5, 0, 0, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow), pure);
}

TEST(XArmBrandLifecyclePort, RefusesMissingTransportAndInvalidAdapterData) {
  EXPECT_THROW(XArmBrandLifecyclePort(nullptr), std::invalid_argument);
  const auto transport = std::make_shared<FakeTransport>();
  EXPECT_THROW(XArmBrandLifecyclePort(transport, {0, 6}), std::invalid_argument);
  EXPECT_THROW(XArmBrandLifecyclePort(transport, {kXArmStatusFreshnessNs, 0}),
               std::invalid_argument);
  // A 7-axis xArm needs all seven bits.
  XArmBrandLifecyclePort seven(transport, {kXArmStatusFreshnessNs, 7});
  transport->report = {kNow, 2, 1, 0, 0, 0x3F, {}};
  EXPECT_EQ(boolean_fact(seven.Observe(kNow), core::kDeviceMotionEnabledFactId),
            false);
  transport->report = {kNow, 2, 1, 0, 0, 0x7F, {}};
  EXPECT_EQ(boolean_fact(seven.Observe(kNow), core::kDeviceMotionEnabledFactId),
            true);
}

// --- m5.5: activation and state handling ----------------------------------

constexpr std::int64_t kMs = 1'000'000LL;

TEST(XArmBrandLifecyclePort, ErrorLeftAfterAnEmergencyStopIsFactsUntilPrepareClearsIt) {
  // Live 2026-10-28: after the e-stop release the controller still reports
  // C2 (mode 0, state 4). Unarmed, that is no incident: Prepare clears it.
  Fixture fixture;
  fixture.Report(4, 0, 2, 0, 0);
  auto status = fixture.port.Observe(kNow);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), true);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);
  EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId), "C2");
  EXPECT_EQ(find_fact(status, kVendorCodeConditionFactId), nullptr);
  EXPECT_TRUE(fixture.port.ClearErrorPermitted());

  // Prepare's `device.clear_error`; the next report shows no error.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, kNow);
  fixture.Report(4, 0, 0, 0, 0, kNow + 200 * kMs);
  status = fixture.port.Observe(kNow + 210 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), false);

  // Then the arming: servo mode, ready, every axis enabled.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.Report(5, 1, 0, 0, 0x3F, kNow + 400 * kMs);
  EXPECT_EQ(fixture.port.Observe(kNow + 410 * kMs).cause, Cause::kNone);
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 600 * kMs);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, kNow + 600 * kMs);
  status = fixture.port.Observe(kNow + 610 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), true);
  // Armed, a new emergency stop is the intervention cause again.
  fixture.Report(4, 0, 2, 0, 0, kNow + 800 * kMs);
  EXPECT_EQ(fixture.port.Observe(kNow + 810 * kMs).cause, Cause::kEmergencyStop);
}

// Maintainer: an e-stop "would fail" the activation "and the error would be
// kept (not escalated)". The clear's own verification is the adapter's (M6
// XADAPT D11: on-demand reads); here the unarmed error stays facts, never a
// cause, and an error still reported 5 s (the own-primitive settle window)
// after the clear is labelled `xarm_error_persists_after_clear`.
TEST(XArmBrandLifecyclePort, ErrorStillReportedAfterOwnClearPersistsWithoutEscalation) {
  constexpr std::int64_t kSettleNs = 5'000'000'000LL;
  Fixture fixture;
  fixture.Report(4, 0, 2, 0, 0);
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, kNow);
  // Within the settle window: not labelled yet, still not a cause.
  fixture.Report(4, 0, 2, 0, 0, kNow + 1000 * kMs);
  auto status = fixture.port.Observe(kNow + 1010 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(find_fact(status, kVendorCodeConditionFactId), nullptr);
  // Still C2 at the window: labelled, with its code; still not a cause.
  fixture.Report(4, 0, 2, 0, 0, kNow + kSettleNs);
  EXPECT_EQ(fixture.port.LatestErrorCode(), 2);
  status = fixture.port.Observe(kNow + kSettleNs + 10 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId), "C2");
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId),
            "xarm_error_persists_after_clear");
  expect_sorted_and_valid(status);
  // Released: the retried clear, a clean report ends the label.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  const auto retried = kNow + 9100 * kMs;
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, retried);
  fixture.Report(4, 0, 0, 0, 0, retried + 200 * kMs);
  status = fixture.port.Observe(retried + 210 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), false);
  EXPECT_EQ(find_fact(status, kVendorCodeConditionFactId), nullptr);
}

// M6 XADAPT: the vendor facts of the latest fresh report, optionally only
// of one received after a boundary (the answer of a refused step).
TEST(XArmBrandLifecyclePort, LatestFreshFactsCanRequireAReportAfterABoundary) {
  Fixture fixture;
  fixture.Report(4, 0, 2, 0, 0, kNow - 10 * kMs);
  const auto latest = fixture.port.LatestFreshFacts(kNow);
  ASSERT_TRUE(latest.has_value());
  // The same report is not one received after `kNow - 10 ms` or later.
  EXPECT_FALSE(fixture.port.LatestFreshFacts(kNow, kNow - 10 * kMs).has_value());
  EXPECT_FALSE(fixture.port.LatestFreshFacts(kNow, kNow).has_value());
  EXPECT_EQ(fixture.port.LatestFreshFacts(kNow, kNow - 20 * kMs), latest);
  // The next report counts; its facts are those `Observe` publishes.
  fixture.Report(4, 0, 2, 0, 0, kNow + 190 * kMs);
  const auto next = fixture.port.LatestFreshFacts(kNow + 200 * kMs, kNow);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, *latest);
  // A stale report never does.
  EXPECT_FALSE(fixture.port.LatestFreshFacts(kNow + 2'000 * kMs, kNow).has_value());
  EXPECT_FALSE(fixture.port.LatestFreshFacts(kNow + 2'000 * kMs).has_value());
}

TEST(XArmBrandLifecyclePort, StudioMoveWhileArmedIsMovedExternallyUntilReArmed) {
  // Live trace 2026-10-28 09:11: state 2 / mode 1 -> state 1 / mode 0 (Studio
  // moves in position mode) -> state 5 / mode 0, err 0, every axis enabled.
  Fixture fixture;
  fixture.Arm();
  fixture.Report(1, 0, 0, 0, 0x3F, kNow + 100 * kMs);
  auto status = fixture.port.Observe(kNow + 110 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceMovedExternallyFactId), true);
  EXPECT_EQ(boolean_fact(status, core::kDeviceInMotionFactId), true);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_moved_externally");
  expect_sorted_and_valid(status);
  // Stopped in state 5: still moved externally, no longer in motion.
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 300 * kMs);
  status = fixture.port.Observe(kNow + 310 * kMs);
  EXPECT_EQ(boolean_fact(status, core::kDeviceMovedExternallyFactId), true);
  EXPECT_EQ(boolean_fact(status, core::kDeviceInMotionFactId), false);
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), "xarm_mode_changed");
  // Moving again.
  fixture.Report(1, 0, 0, 0, 0x3F, kNow + 1100 * kMs);
  EXPECT_EQ(boolean_fact(fixture.port.Observe(kNow + 1110 * kMs), core::kDeviceInMotionFactId),
            true);
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 1900 * kMs);
  EXPECT_EQ(boolean_fact(fixture.port.Observe(kNow + 1910 * kMs), core::kDeviceInMotionFactId),
            false);

  // The driver's Recover disarms (the plugin's deactivation: position mode).
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kDisarm);
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 2000 * kMs);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kDisarm, true, kNow + 2000 * kMs);
  status = fixture.port.Observe(kNow + 2010 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceMovedExternallyFactId), true);
  // Prepare re-arms: servo mode and ready end the condition.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 2400 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 2410 * kMs));
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, kNow + 2410 * kMs);
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 2600 * kMs);
  status = fixture.port.Observe(kNow + 2610 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(find_fact(status, core::kDeviceMovedExternallyFactId), nullptr);
  EXPECT_EQ(find_fact(status, core::kDeviceInMotionFactId), nullptr);
  EXPECT_EQ(find_fact(status, kVendorCodeConditionFactId), nullptr);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), true);
}

TEST(XArmBrandLifecyclePort, UnrequestedStatesThreeToSixAreTypedConditions) {
  const std::vector<std::pair<int, std::string>> states{
      {3, "xarm_paused"}, {4, "xarm_stopped"}, {5, "xarm_mode_changed"},
      {6, "xarm_stop_requested"}};
  for (const auto &[state, token] : states) {
    // Operator policy: the `external_control` cause with its typed reason.
    Fixture operator_policy(XArmExternalMoveRearm::kOperator);
    operator_policy.Arm();
    operator_policy.Report(state, 1, 0, 0, 0x3F);
    auto status = operator_policy.port.Observe(kNow);
    EXPECT_EQ(status.cause, Cause::kExternalControl) << state;
    EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), token) << state;
    EXPECT_EQ(integer_fact(status, kVendorCodeStateFactId), state);
    EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), false);
    // Automatic policy: the moved-externally condition, not in motion (only
    // state 1 is motion).
    Fixture automatic;
    automatic.Arm();
    automatic.Report(state, 1, 0, 0, 0x3F);
    status = automatic.port.Observe(kNow);
    EXPECT_EQ(status.cause, Cause::kNone) << state;
    EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId), token) << state;
    EXPECT_EQ(boolean_fact(status, core::kDeviceMovedExternallyFactId), true);
    EXPECT_EQ(boolean_fact(status, core::kDeviceInMotionFactId), false) << state;
  }
}

// SX-447: a sent deactivation whose outcome is unknown never proves the robot
// unarmed; only the observed hardware state does. A new adapter with no
// hardware observation yet is treated as armed (conservative).
TEST(XArmBrandLifecyclePort, UncertainDisarmStaysArmedUntilTheHardwareIsObservedInactive) {
  Fixture fixture;
  fixture.Arm();
  fixture.port.ObserveHardwareActive(true);
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kDisarm);
  // The hardware deactivation got no answer (uncertain effect).
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kDisarm, true, kNow);
  fixture.Report(4, 0, 2, 0, 0, kNow + 100 * kMs);
  EXPECT_EQ(fixture.port.Observe(kNow + 110 * kMs).cause, Cause::kEmergencyStop);
  // Still listed active: still armed.
  fixture.port.ObserveHardwareActive(true);
  EXPECT_EQ(fixture.port.Observe(kNow + 120 * kMs).cause, Cause::kEmergencyStop);
  // Observed inactive: unarmed, the error is facts until Prepare clears it.
  fixture.port.ObserveHardwareActive(false);
  const auto status = fixture.port.Observe(kNow + 130 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId), "C2");

  // During ManyForge's own arming the list (still inactive) does not
  // override it; afterwards the observation is authoritative again.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.port.ObserveHardwareActive(false);
  fixture.Report(4, 0, 2, 0, 0, kNow + 200 * kMs);
  EXPECT_EQ(fixture.port.Observe(kNow + 210 * kMs).cause, Cause::kEmergencyStop);
}

TEST(XArmBrandLifecyclePort, ANewAdapterWithoutAHardwareObservationIsConservative) {
  const auto transport = std::make_shared<FakeTransport>();
  XArmBrandLifecyclePort port(transport);
  transport->report = {kNow - 10'000'000, 4, 0, 2, 0, 0, {}};
  // Owner or adapter re-created under a possibly active component: armed.
  EXPECT_EQ(port.Observe(kNow).cause, Cause::kEmergencyStop);
  // The controller manager lists the hardware inactive: unarmed.
  port.ObserveHardwareActive(false);
  EXPECT_EQ(port.Observe(kNow).cause, Cause::kNone);
  // Listed active (a retained, armed component): armed.
  port.ObserveHardwareActive(true);
  EXPECT_EQ(port.Observe(kNow).cause, Cause::kEmergencyStop);
}

// M6 ADAPTERM: an admitted motion re-engages the robot only after an
// error-free move from its own interface, at rest, every axis enabled.
TEST(XArmBrandLifecyclePort, MotionReengageOnlyAtRestAfterAnErrorFreeMove) {
  Fixture fixture;
  fixture.Arm();
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 10 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 20 * kMs));
  EXPECT_EQ(fixture.port.MotionReengageDecision(kNow + 20 * kMs).kind,
            XArmMotionReengage::kNotNeeded);

  // Studio moves it: latched; while it still moves, refused typed.
  fixture.Report(1, 0, 0, 0, 0x3F, kNow + 100 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 110 * kMs));
  auto decision = fixture.port.MotionReengageDecision(kNow + 110 * kMs);
  EXPECT_EQ(decision.kind, XArmMotionReengage::kRefused);
  EXPECT_EQ(decision.reason, "xarm_moved_externally_in_motion");

  // Stopped (state 5, position mode), no error, every axis enabled: needed.
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 300 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 310 * kMs));
  EXPECT_EQ(fixture.port.MotionReengageDecision(kNow + 310 * kMs).kind,
            XArmMotionReengage::kNeeded);
  // Never after an error (an emergency stop needs Recover cell), with an
  // axis disabled, or without a fresh report.
  fixture.Report(4, 0, 2, 0, 0, kNow + 400 * kMs);
  EXPECT_EQ(fixture.port.MotionReengageDecision(kNow + 410 * kMs).reason,
            "xarm_error_present");
  fixture.Report(5, 0, 0, 0, 0x1F, kNow + 500 * kMs);
  EXPECT_EQ(fixture.port.MotionReengageDecision(kNow + 510 * kMs).reason,
            "xarm_axes_not_enabled");
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 600 * kMs);
  EXPECT_EQ(fixture.port.MotionReengageDecision(
                kNow + 600 * kMs + kXArmStatusFreshnessNs + kMs).reason,
            "xarm_status_unavailable");

  // The re-engage is ManyForge's own arming: its mode/state changes are
  // never external control, and the ready report ends the latch.
  const auto since = kNow + 700 * kMs;
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.Report(4, 0, 0, 0, 0x3F, since + 10 * kMs);  // set_mode's stop
  EXPECT_EQ(fixture.port.Observe(since + 20 * kMs).cause, Cause::kNone);
  EXPECT_FALSE(fixture.port.ReportsServoReady(since + 20 * kMs, since));
  // SX-567.1: the moving state is never a successful arming.
  fixture.Report(1, 1, 0, 0, 0x3F, since + 50 * kMs);
  EXPECT_FALSE(fixture.port.ReportsServoReady(since + 60 * kMs, since));
  fixture.Report(2, 1, 0, 0, 0x3F, since + 100 * kMs);
  EXPECT_TRUE(fixture.port.ReportsServoReady(since + 110 * kMs, since));
  // A ready report from before the re-engage does not count.
  EXPECT_FALSE(fixture.port.ReportsServoReady(since + 110 * kMs, since + 200 * kMs));
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, since + 110 * kMs);
  const auto status = fixture.port.Observe(since + 120 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(find_fact(status, core::kDeviceMovedExternallyFactId), nullptr);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), true);
  EXPECT_EQ(fixture.port.MotionReengageDecision(since + 120 * kMs).kind,
            XArmMotionReengage::kNotNeeded);
}

// SX-567.1: after the re-engage the gate also needs the arm at rest by its
// own report.
TEST(XArmBrandLifecyclePort, ReengagedGateRefusesTheMovingState) {
  Fixture fixture;
  fixture.Arm();
  const std::vector<double> measured{0.1, -0.2, 0.3, 0.0, 0.5, 0.6};
  fixture.Report(2, 1, 0, 0, 0x3F, kNow - 10 * kMs);
  EXPECT_TRUE(fixture.port.CheckReengagedAtRest(kNow, measured).ok());
  fixture.Report(1, 1, 0, 0, 0x3F, kNow - 10 * kMs);
  EXPECT_TRUE(fixture.port.CheckActivationGate(kNow, measured).ok());
  const auto moving = fixture.port.CheckReengagedAtRest(kNow, measured);
  ASSERT_FALSE(moving.ok());
  EXPECT_EQ(moving.error().details.at("reason"), "xarm_in_motion");
}

/// The re-engage sequence over scripted vendor calls and a manual clock.
struct ReengageScript final {
  explicit ReengageScript(Fixture &fixture) : fixture(fixture) {
    // Latched: moved from Studio, stopped (state 5, position mode).
    fixture.Arm();
    fixture.Report(1, 0, 0, 0, 0x3F, now - 5 * kMs);
    static_cast<void>(fixture.port.Observe(now));
    fixture.Report(5, 0, 0, 0, 0x3F, now - 5 * kMs);
    static_cast<void>(fixture.port.Observe(now));
  }
  [[nodiscard]] XArmReengageCalls Calls() {
    return {[this](const int mode) {
              modes.push_back(mode);
              return mode_result;
            },
            [this](const int state) {
              states.push_back(state);
              if (ready_after_state)
                fixture.Report(2, 1, 0, 0, 0x3F, now + 10 * kMs);
              return state_result;
            },
            [this] { return now; },
            [this] { now += 100 * kMs; }};
  }
  Fixture &fixture;
  std::int64_t now = kNow + 1000 * kMs;
  XArmReengageCallResult mode_result{true, true, "", 0};
  XArmReengageCallResult state_result{true, true, "", 0};
  bool ready_after_state = true;
  std::vector<int> modes;
  std::vector<int> states;
};

TEST(XArmReengage, SetsModeThenStateAndEndsTheLatchOnTheReadyReport) {
  Fixture fixture;
  ReengageScript script(fixture);
  script.now += 20 * kMs;
  const auto result = ReengageAtRest(fixture.port, script.Calls(), std::stop_token{});
  ASSERT_FALSE(result.error.has_value()) << result.error->message;
  EXPECT_TRUE(result.reengaged);
  EXPECT_EQ(script.modes, std::vector<int>{1});
  EXPECT_EQ(script.states, std::vector<int>{0});
  EXPECT_EQ(fixture.port.MotionReengageDecision(script.now).kind,
            XArmMotionReengage::kNotNeeded);
  // ManyForge's own re-arm stays reported until the handle's rebind.
  EXPECT_TRUE(fixture.port.RearmInProgress());
  EXPECT_EQ(boolean_fact(fixture.port.Observe(script.now), core::kDeviceRearmInProgressFactId),
            true);
  fixture.port.EndRearm();
  EXPECT_EQ(find_fact(fixture.port.Observe(script.now), core::kDeviceRearmInProgressFactId),
            nullptr);
  // Nothing latched: no vendor call.
  const auto again = ReengageAtRest(fixture.port, script.Calls(), std::stop_token{});
  EXPECT_FALSE(again.reengaged);
  EXPECT_FALSE(again.error.has_value());
  EXPECT_EQ(script.modes.size(), 1U);
}

// SX-567.2 / SX-569.2: a sent but unconfirmed call is an effect-possible
// failure; a late ready report then ends the latch on its own (which is why
// the handle, not the latch, keeps the rebind required). A call never sent
// has no effect.
TEST(XArmReengage, UnconfirmedCallsReportWhetherAnEffectIsPossible) {
  struct Case {
    std::string name;
    XArmReengageCallResult mode;
    XArmReengageCallResult state;
    std::string step;
    bool effect_possible;
    // M6 XADAPT: no answer in time is the robot controller unresponsive;
    // an answer that refused keeps the typed refusal.
    std::string code;
  };
  const XArmReengageCallResult ok{true, true, "", 0};
  for (const auto &test :
       {Case{"timeout", {true, false, "xarm_service_timeout", std::nullopt}, ok,
             "set_mode", true, "robot_controller_unresponsive"},
        Case{"no_response", ok, {true, false, "xarm_service_no_response", std::nullopt},
             "set_state", true, "robot_controller_unresponsive"},
        Case{"nonzero", ok, {true, false, "xarm_service_returned_nonzero", 1},
             "set_state", true, "execution_rejected"},
        Case{"not_sent", {false, false, "xarm_service_unavailable", std::nullopt}, ok,
             "set_mode", false, "execution_rejected"}}) {
    SCOPED_TRACE(test.name);
    Fixture fixture;
    ReengageScript script(fixture);
    script.mode_result = test.mode;
    script.state_result = test.state;
    script.ready_after_state = false;
    const auto result = ReengageAtRest(fixture.port, script.Calls(), std::stop_token{});
    ASSERT_TRUE(result.error.has_value());
    EXPECT_FALSE(result.reengaged);
    EXPECT_EQ(result.effect_possible, test.effect_possible);
    EXPECT_EQ(result.error->code, test.code);
    EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_confirmed");
    EXPECT_EQ(result.error->details.at("step"), test.step);
    EXPECT_EQ(result.error->details.at("effect"),
              test.effect_possible ? "possible" : "not_started");
    // SX-584: the re-arm fact ends with the attempt, whatever its outcome.
    EXPECT_FALSE(fixture.port.RearmInProgress());
    if (!test.effect_possible) continue;
    // The delayed effect: the robot reports servo mode and ready later, an
    // own effect (never external control) that ends the latch.
    fixture.Report(2, 1, 0, 0, 0x3F, script.now + 200 * kMs);
    const auto status = fixture.port.Observe(script.now + 210 * kMs);
    EXPECT_EQ(status.cause, Cause::kNone);
    EXPECT_EQ(fixture.port.MotionReengageDecision(script.now + 210 * kMs).kind,
              XArmMotionReengage::kNotNeeded);
  }
}

// Items 23/32 (SX-574): the re-engage ends the move latch before the plugin
// has the broadcaster back; vendor samples keep the measured stream live
// through ManyForge's own re-arm until the handle's rebind.
TEST(XArmReengage, VendorSamplesBridgeTheOwnRearmUntilTheRebind) {
  Fixture fixture;
  ReengageScript script(fixture);
  const std::vector<double> pose{0.2, -0.1, 0.4, 0.0, 0.6, 0.7};
  EXPECT_TRUE(fixture.port.VendorJointSampleUsable(script.now, pose));  // latched
  const auto result = ReengageAtRest(fixture.port, script.Calls(), std::stop_token{});
  ASSERT_TRUE(result.reengaged);
  EXPECT_EQ(fixture.port.MotionReengageDecision(script.now).kind, XArmMotionReengage::kNotNeeded);
  EXPECT_TRUE(fixture.port.VendorJointSampleUsable(script.now + 20 * kMs, pose));
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(
      script.now + 20 * kMs, std::vector<double>(6, 0.0)));
  fixture.port.EndRearm();  // the handle's rebind
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(script.now + 20 * kMs, pose));
}

// M6 ADAPTERM TOOLCTX (live 2026-10-28): `set_tcp_offset` from ready drops
// the controller to state 5; `set_state(0)` restores ready. As ManyForge's
// own arming: never external control, no move latch, and
// `device.rearm_in_progress` until the caller's rebind.
TEST(XArmTcpOffset, TheOffsetsStateFiveIsOwnEffectAndTheReArmRestoresReady) {
  Fixture fixture;
  fixture.Arm();
  std::int64_t now = kNow + 1000 * kMs;
  fixture.Report(2, 1, 0, 0, 0x3F, now - 5 * kMs);
  static_cast<void>(fixture.port.Observe(now));
  std::vector<std::string> calls;
  XArmTcpOffsetCalls script{
      [&] {
        calls.emplace_back("tcp_offset");
        fixture.Report(5, 1, 0, 0, 0x3F, now + 5 * kMs);  // dropped out of ready
        // A status commit meanwhile: own effect, never a move or an incident.
        const auto status = fixture.port.Observe(now + 10 * kMs);
        EXPECT_EQ(status.cause, Cause::kNone);
        EXPECT_EQ(find_fact(status, core::kDeviceMovedExternallyFactId), nullptr);
        EXPECT_EQ(boolean_fact(status, core::kDeviceRearmInProgressFactId), true);
        return XArmReengageCallResult{true, true, "", 0};
      },
      [&](const int state) {
        calls.emplace_back("set_state:" + std::to_string(state));
        fixture.Report(2, 1, 0, 0, 0x3F, now + 20 * kMs);
        fixture.transport->report.tcp_offset = {0, 0, 120, 0, 0, 0};
        return XArmReengageCallResult{true, true, "", 0};
      },
      [&] { return now; },
      [&] { now += 100 * kMs; }};
  const auto result = SendTcpOffsetAtRest(fixture.port, script, std::stop_token{});
  ASSERT_FALSE(result.error.has_value()) << result.error->message;
  EXPECT_TRUE(result.reengaged);
  ASSERT_TRUE(result.ready_offset.has_value());
  EXPECT_DOUBLE_EQ((*result.ready_offset)[2], 120.0);
  EXPECT_EQ(calls, (std::vector<std::string>{"tcp_offset", "set_state:0"}));
  const auto status = fixture.port.Observe(now + 30 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(find_fact(status, core::kDeviceMovedExternallyFactId), nullptr);
  EXPECT_EQ(boolean_fact(status, core::kDeviceAcceptsCommandsFactId), true);
  EXPECT_TRUE(fixture.port.RearmInProgress());
  fixture.port.EndRearm();  // the caller's rebind
  EXPECT_FALSE(fixture.port.RearmInProgress());
}

// Item 44: the controller's reported TCP offset, from a fresh report only.
TEST(XArmTcpOffset, TheReportedOffsetComesFromTheLatestFreshReport) {
  Fixture fixture;
  fixture.Report(2, 1, 0, 0, 0x3F, kNow - 10 * kMs);
  EXPECT_FALSE(fixture.port.ReportedTcpOffset(kNow).has_value());  // none reported
  fixture.transport->report.tcp_offset = {0.0, 10.0, 120.0, 0.0, 0.0, 1.5};
  const auto reported = fixture.port.ReportedTcpOffset(kNow);
  ASSERT_TRUE(reported.has_value());
  EXPECT_DOUBLE_EQ((*reported)[2], 120.0);
  EXPECT_DOUBLE_EQ((*reported)[5], 1.5);
  EXPECT_FALSE(fixture.port.ReportedTcpOffset(kNow + kXArmStatusFreshnessNs).has_value());
}

// SX-590: a ready report received before the confirmed `set_state(0)` (here
// during `set_tcp_offset`) never shows the re-arm's result.
TEST(XArmTcpOffset, AReadyReportBeforeTheConfirmedSetStateDoesNotCount) {
  Fixture fixture;
  fixture.Arm();
  std::int64_t now = kNow + 1000 * kMs;
  fixture.Report(2, 1, 0, 0, 0x3F, now - 5 * kMs);
  static_cast<void>(fixture.port.Observe(now));
  const auto result = SendTcpOffsetAtRest(
      fixture.port,
      {[&] {
         fixture.Report(2, 1, 0, 0, 0x3F, now + 5 * kMs);  // ready, but before set_state
         return XArmReengageCallResult{true, true, "", 0};
       },
       [&](int) {
         now += 10 * kMs;  // no new report
         return XArmReengageCallResult{true, true, "", 0};
       },
       [&] { return now; }, [&] { now += 100 * kMs; }},
      std::stop_token{});
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_ready");
  EXPECT_EQ(result.error->details.at("effect"), "possible");
}

// M6b (SX-727): at Enable the ready wait after the offset is part of the
// lifecycle step. Both calls answer but the fresh ready report comes only
// after the step's deadline: the wait ends there (well before
// `kXArmReengageReadyNs`), uncertain with the effect possible, no
// unresponsive fault. Without a deadline the wait keeps its own bound.
TEST(XArmTcpOffset, TheEnableReadyWaitEndsAtTheStepDeadline) {
  Fixture fixture;
  fixture.Arm();
  std::int64_t now = kNow + 1000 * kMs;
  fixture.Report(2, 1, 0, 0, 0x3F, now - 5 * kMs);
  static_cast<void>(fixture.port.Observe(now));
  const auto calls = [&] {
    return XArmTcpOffsetCalls{
        [&] {
          fixture.Report(5, 1, 0, 0, 0x3F, now + 5 * kMs);  // dropped out of ready
          return XArmReengageCallResult{true, true, "", 0};
        },
        [&](int) {
          now += 10 * kMs;  // answered; the ready report is still to come
          return XArmReengageCallResult{true, true, "", 0};
        },
        [&] { return now; },
        [&] {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          now += 10 * kMs;
        }};
  };
  const auto started = now;
  const auto result = SendTcpOffsetAtRest(
      fixture.port, calls(), std::stop_token{},
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_ready");
  EXPECT_EQ(result.error->details.at("effect"), "possible");
  EXPECT_TRUE(result.effect_possible);
  EXPECT_FALSE(result.reengaged);
  EXPECT_LT(now - started, kXArmReengageReadyNs);
  EXPECT_FALSE(fixture.port.RearmInProgress());
  // The pre-motion path (no deadline): the same delayed report arrives
  // within the ready wait and the send succeeds.
  int pauses = 0;
  auto delayed = calls();
  delayed.pause = [&] {
    now += 100 * kMs;
    if (++pauses == 3) fixture.Report(2, 1, 0, 0, 0x3F, now);
  };
  const auto ready = SendTcpOffsetAtRest(fixture.port, delayed, std::stop_token{});
  ASSERT_FALSE(ready.error.has_value()) << ready.error->message;
  EXPECT_TRUE(ready.reengaged);
}

TEST(XArmTcpOffset, FailuresAreTypedWithWhetherAnEffectIsPossible) {
  struct Case {
    std::string name;
    XArmReengageCallResult offset;
    XArmReengageCallResult state;
    std::string reason;
    bool effect_possible;
    std::string code;
  };
  const XArmReengageCallResult ok{true, true, "", 0};
  for (const auto &test :
       {Case{"offset_not_sent", {false, false, "xarm_service_unavailable", std::nullopt}, ok,
             "tool_context_not_applied", false, "execution_rejected"},
        // M6 XADAPT: no answer in time.
        Case{"offset_timeout", {true, false, "xarm_service_timeout", std::nullopt}, ok,
             "tool_context_not_applied", true, "robot_controller_unresponsive"},
        Case{"set_state_timeout", ok, {true, false, "xarm_service_timeout", std::nullopt},
             "xarm_reengage_not_confirmed", true, "robot_controller_unresponsive"},
        Case{"set_state_nonzero", ok, {true, false, "xarm_service_returned_nonzero", 9},
             "xarm_reengage_not_confirmed", true, "execution_rejected"}}) {
    SCOPED_TRACE(test.name);
    Fixture fixture;
    fixture.Arm();
    std::int64_t now = kNow + 1000 * kMs;
    fixture.Report(2, 1, 0, 0, 0x3F, now - 5 * kMs);
    static_cast<void>(fixture.port.Observe(now));
    const auto result = SendTcpOffsetAtRest(
        fixture.port,
        {[&] { return test.offset; }, [&](int) { return test.state; }, [&] { return now; },
         [&] { now += 100 * kMs; }},
        std::stop_token{});
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, test.code);
    EXPECT_EQ(result.error->details.at("reason"), test.reason);
    EXPECT_EQ(result.error->details.at("effect"),
              test.effect_possible ? "possible" : "not_started");
    EXPECT_EQ(result.effect_possible, test.effect_possible);
    // SX-584: the re-arm fact ends with the attempt, whatever its outcome.
    EXPECT_FALSE(fixture.port.RearmInProgress());
  }
}

// SX-590: a ready report received before the confirmed `set_state(0)` (here
// during `set_mode`) never shows the re-engage's result.
TEST(XArmReengage, AReadyReportBeforeTheConfirmedSetStateDoesNotCount) {
  Fixture fixture;
  ReengageScript script(fixture);
  script.ready_after_state = false;
  auto calls = script.Calls();
  calls.set_mode = [&](const int mode) {
    script.modes.push_back(mode);
    fixture.Report(2, 1, 0, 0, 0x3F, script.now + 5 * kMs);  // before set_state
    return XArmReengageCallResult{true, true, "", 0};
  };
  calls.set_state = [&](const int state) {
    script.states.push_back(state);
    script.now += 10 * kMs;  // the call takes time; no new report
    return XArmReengageCallResult{true, true, "", 0};
  };
  const auto result = ReengageAtRest(fixture.port, calls, std::stop_token{});
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_ready");
}

// SX-567.1: a moving report after the calls is never the ready result.
TEST(XArmReengage, AMovingStateAfterTheCallsTimesOutWithAPossibleEffect) {
  Fixture fixture;
  ReengageScript script(fixture);
  script.ready_after_state = false;
  auto calls = script.Calls();
  calls.set_state = [&](const int state) {
    script.states.push_back(state);
    fixture.Report(1, 1, 0, 0, 0x3F, script.now + 10 * kMs);
    return XArmReengageCallResult{true, true, "", 0};
  };
  calls.pause = [&] {
    script.now += 100 * kMs;
    fixture.Report(1, 1, 0, 0, 0x3F, script.now - 10 * kMs);
  };
  const auto result = ReengageAtRest(fixture.port, calls, std::stop_token{});
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_ready");
  // The robot answered (moving): not the unresponsive code.
  EXPECT_EQ(result.error->code, "execution_rejected");
  EXPECT_TRUE(result.effect_possible);
}

// M6 XADAPT: the ready wait ends with no status report received after the
// confirmed `set_state(0)`: the robot controller did not answer in time.
TEST(XArmReengage, NoReportDuringTheReadyWaitIsTheRobotControllerUnresponsive) {
  Fixture fixture;
  ReengageScript script(fixture);
  script.ready_after_state = false;
  const auto result = ReengageAtRest(fixture.port, script.Calls(), std::stop_token{});
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(result.error->details.at("reason"), "xarm_reengage_not_ready");
  EXPECT_TRUE(result.effect_possible);
  EXPECT_FALSE(fixture.port.RearmInProgress());

  // The same for the TCP offset's re-arm.
  Fixture offset_fixture;
  offset_fixture.Arm();
  std::int64_t now = kNow + 1000 * kMs;
  offset_fixture.Report(2, 1, 0, 0, 0x3F, now - 5 * kMs);
  static_cast<void>(offset_fixture.port.Observe(now));
  const XArmReengageCallResult ok{true, true, "", 0};
  const auto sent = SendTcpOffsetAtRest(
      offset_fixture.port,
      {[&] { return ok; }, [&](int) { return ok; }, [&] { return now; },
       [&] { now += 100 * kMs; }},
      std::stop_token{});
  ASSERT_TRUE(sent.error.has_value());
  EXPECT_EQ(sent.error->code, "robot_controller_unresponsive");
  EXPECT_EQ(sent.error->details.at("reason"), "xarm_reengage_not_ready");
}

TEST(XArmBrandLifecyclePort, AnErrorDuringAnExternalMoveKeepsTheErrorClassification) {
  Fixture fixture;
  fixture.Arm();
  fixture.Report(1, 0, 0, 0, 0x3F);
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  fixture.Report(4, 0, 31, 0, 0x3F);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kCollision);
  fixture.Report(4, 0, 2, 0, 0);
  EXPECT_EQ(fixture.port.Observe(kNow).cause, Cause::kEmergencyStop);
}

TEST(XArmBrandLifecyclePort, VendorJointSamplesAreUsableOnlyDuringACleanExternalMove) {
  // M6 option (c): the vendor driver's own joint states stand in for the
  // broadcaster's only while the robot is moved from its own interface.
  const std::vector<double> moved{0.2, -0.1, 0.4, 0.0, 0.6, 0.7};
  Fixture fixture;
  fixture.Arm();
  // Armed and driven by ManyForge: the broadcaster alone.
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 10 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 20 * kMs));
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(kNow + 20 * kMs, moved));

  // Move in progress from the robot's own interface: vendor samples usable.
  fixture.Report(1, 0, 0, 0, 0x3F, kNow + 100 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 110 * kMs));
  EXPECT_TRUE(fixture.port.VendorJointSampleUsable(kNow + 110 * kMs, moved));
  // The e-stop signature (every angle exactly zero) and invalid values never.
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(
      kNow + 110 * kMs, std::vector<double>(6, 0.0)));
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(kNow + 110 * kMs, {}));
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(
      kNow + 110 * kMs, {0.1, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, 0.0}));

  // The driver dies: no fresh report, no vendor sample (the stream goes stale).
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(
      kNow + 110 * kMs + kXArmStatusFreshnessNs + kMs, moved));

  // An error present: vendor samples ignored, whatever the latch.
  fixture.Report(4, 0, 31, 0, 0x3F, kNow + 200 * kMs);
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(kNow + 210 * kMs, moved));
  fixture.Report(5, 0, 0, 0, 0x3F, kNow + 300 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 310 * kMs));
  EXPECT_TRUE(fixture.port.VendorJointSampleUsable(kNow + 310 * kMs, moved));

  // After the re-arm: the broadcaster alone again.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kArm);
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 400 * kMs);
  static_cast<void>(fixture.port.Observe(kNow + 410 * kMs));
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kArm, true, kNow + 410 * kMs);
  fixture.Report(2, 1, 0, 0, 0x3F, kNow + 500 * kMs);
  ASSERT_EQ(find_fact(fixture.port.Observe(kNow + 510 * kMs),
                      core::kDeviceMovedExternallyFactId),
            nullptr);
  EXPECT_FALSE(fixture.port.VendorJointSampleUsable(kNow + 510 * kMs, moved));
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string maintained_manifest(const std::string_view id) {
  const std::filesystem::path root(MANYFORGE_XARM_CONTROL_SOURCE_ROOT);
  return read_file(root / "manyforge_deployment/resources/maintained/catalog/"
                          "provider_manifest" /
                   (std::string(id) + ".json"));
}

// --- R7 package 4: the Real xArm arm's ros2_control catalog -----------------

using Intent = core::ComponentLifecycleIntentKindDTO;
using State = core::ComponentLifecycleStateDTO;

[[nodiscard]] std::vector<std::string>
primitive_ids(const core::LifecycleIntentPlanDTO &plan) {
  std::vector<std::string> result;
  for (const auto &transition : plan.transitions)
    result.push_back(transition.lifecycle_primitive_id.value());
  return result;
}

[[nodiscard]] const core::LifecycleIntentPlanDTO &
plan_for(const std::vector<core::LifecycleIntentPlanDTO> &plans, Intent intent) {
  const auto found = std::find_if(plans.begin(), plans.end(), [&](const auto &plan) {
    return plan.intent == intent;
  });
  if (found == plans.end()) throw std::logic_error("missing plan");
  return *found;
}

// The predicate holds (every fact known and equal, the state allowed).
[[nodiscard]] bool holds(const core::LifecyclePredicateDTO &predicate, State state,
                         const std::map<std::string, bool> &facts) {
  if (std::find(predicate.allowed_states.begin(), predicate.allowed_states.end(),
                state) == predicate.allowed_states.end())
    return false;
  return std::all_of(
      predicate.fact_predicates.begin(), predicate.fact_predicates.end(),
      [&](const auto &expected) {
        const auto found = facts.find(expected.lifecycle_fact_id.value());
        return found != facts.end() &&
               expected.expected_value.boolean_value == found->second;
      });
}

[[nodiscard]] core::ProviderManifestDTO
lifecycle_manifest(const core::ContentDigest &semantic) {
  std::vector<core::ProviderCapabilityOfferDTO> offers;
  for (const std::string_view id :
       {"component.lifecycle.disable", "component.lifecycle.enable",
        "component.lifecycle.observe", "component.lifecycle.prepare",
        "component.lifecycle.recover"}) {
    const auto *definition = core::find_builtin_capability_definition(id);
    if (definition == nullptr) throw std::logic_error("missing definition");
    offers.push_back({core::CapabilityId(std::string(id)),
                      core::CapabilityDefinitionRevision(
                          std::string(definition->capability_definition_revision)),
                      {}});
  }
  return {core::ProviderManifestId(
              std::string(kXArmRos2ControlLifecycleProviderManifestId)),
          core::ProviderManifestRevision(
              "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
          std::move(offers),
          {},
          core::ProviderLifecyclePlanCatalogDeclarationDTO{
              core::SchemaId(std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
              semantic}};
}

TEST(XArmLifecycleProfile, RealRos2ControlPlansArmAtPrepareAndDisarmAtDisable) {
  const auto plans = make_xarm_ros2_control_lifecycle_plans();
  ASSERT_EQ(plans.size(), 4U);
  // D1 + §3.3: arm, absorb the plugin's reactivate-all, switch the
  // broadcaster back on if it stayed off, mark prepared.
  const auto &prepare = plan_for(plans, Intent::kPrepare);
  EXPECT_EQ(prepare.requested_state, State::kPrepared);
  EXPECT_EQ(primitive_ids(prepare),
            (std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId),
                                      "ros2_control.hardware.activate",
                                      "ros2_control.controller.deactivate",
                                      "ros2_control.broadcaster.activate",
                                      "ros2_control.controller.prepare"}));
  // Enable is the existing activation (its gate is in the handle).
  EXPECT_EQ(primitive_ids(plan_for(plans, Intent::kEnable)),
            (std::vector<std::string>{"ros2_control.controller.activate"}));
  // D4 option B: the controller, then the hardware.
  const auto &disable = plan_for(plans, Intent::kDisable);
  EXPECT_EQ(primitive_ids(disable),
            (std::vector<std::string>{"ros2_control.controller.deactivate",
                                      "ros2_control.hardware.deactivate"}));
  EXPECT_TRUE(holds(disable.requested_postcondition, State::kDisabled,
                    {{"external.controller_configuration_ready", false},
                     {"external.controller_active", false},
                     {"external.fault_present", false},
                     {"external.hardware_active", false},
                     {"external.transport_ready", true}}));
  EXPECT_FALSE(holds(disable.requested_postcondition, State::kDisabled,
                     {{"external.controller_configuration_ready", false},
                      {"external.controller_active", false},
                      {"external.fault_present", false},
                      {"external.hardware_active", true},
                      {"external.transport_ready", true}}));
  // D10, M6 XADAPT: the vendor error first, then the controller, the
  // hardware and the warning.
  const auto &recover = plan_for(plans, Intent::kRecover);
  EXPECT_EQ(recover.requested_state, State::kDisabled);
  EXPECT_EQ(primitive_ids(recover),
            (std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId),
                                      "ros2_control.controller.deactivate",
                                      "ros2_control.hardware.deactivate",
                                      std::string(kXArmClearWarningPrimitiveId),
                                      "ros2_control.controller.recover"}));

  // Arming is never eligible over a vendor error, on unknown (stale) status,
  // or without a loaded controller.
  // m5.5: every Real arm step is bounded at 10 s.
  for (const auto &plan : plans)
    EXPECT_EQ(plan.timeout_policy,
              core::LifecyclePlanTimeoutPolicyDTO(std::int64_t{10'000'000'000LL}))
        << static_cast<int>(plan.intent);
  // m5.5: Prepare clears a vendor error left on the unarmed robot first,
  // never over a fault cause, on active hardware or on unknown status.
  const auto &clear = prepare.transitions.front();
  std::map<std::string, bool> unarmed_error{
      {std::string(core::kDeviceErrorPresentFactId), true},
      {"external.controller_active", false},
      {"external.fault_present", false},
      {"external.hardware_active", false},
      {"external.transport_ready", true}};
  EXPECT_TRUE(holds(clear.eligibility_predicate, State::kDeclared, unarmed_error));
  EXPECT_TRUE(holds(clear.eligibility_predicate, State::kDisabled, unarmed_error));
  EXPECT_FALSE(holds(clear.eligibility_predicate, State::kFaulted, unarmed_error));
  for (const auto &[id, value] : unarmed_error) {
    auto changed = unarmed_error;
    changed[id] = !value;
    EXPECT_FALSE(holds(clear.eligibility_predicate, State::kDeclared, changed)) << id;
  }
  auto cleared = unarmed_error;
  EXPECT_FALSE(holds(clear.postcondition_predicate, State::kDeclared, cleared));
  cleared[std::string(core::kDeviceErrorPresentFactId)] = false;
  EXPECT_TRUE(holds(clear.postcondition_predicate, State::kDeclared, cleared));

  const auto &arm = prepare.transitions[1];
  std::map<std::string, bool> unarmed{
      {std::string(core::kDeviceErrorPresentFactId), false},
      {"external.controller_spawned", true},
      {"external.fault_present", false},
      {"external.hardware_active", false},
      {"external.transport_ready", true}};
  EXPECT_TRUE(holds(arm.eligibility_predicate, State::kDeclared, unarmed));
  EXPECT_TRUE(holds(arm.eligibility_predicate, State::kDisabled, unarmed));
  EXPECT_FALSE(holds(arm.eligibility_predicate, State::kFaulted, unarmed));
  auto error = unarmed;
  error[std::string(core::kDeviceErrorPresentFactId)] = true;
  EXPECT_FALSE(holds(arm.eligibility_predicate, State::kDeclared, error));
  auto stale = unarmed;
  stale.erase(std::string(core::kDeviceErrorPresentFactId));
  EXPECT_FALSE(holds(arm.eligibility_predicate, State::kDeclared, stale));
  auto unloaded = unarmed;
  unloaded["external.controller_spawned"] = false;
  EXPECT_FALSE(holds(arm.eligibility_predicate, State::kDeclared, unloaded));
  // Armed does not wait for the broadcaster: the plugin's deactivate-all may
  // have left it off, and the broadcaster step follows.
  const std::map<std::string, bool> armed_off{
      {std::string(core::kDeviceAcceptsCommandsFactId), true},
      {"external.broadcaster_active", false},
      {"external.fault_present", false},
      {"external.hardware_active", true},
      {"external.transport_ready", true}};
  EXPECT_TRUE(holds(arm.postcondition_predicate, State::kDeclared, armed_off));

  // The broadcaster is switched on only on armed, fault-free hardware with
  // the trajectory controller off; done once a measured sample is valid.
  const auto &broadcaster = prepare.transitions[3];
  const std::map<std::string, bool> off{
      {"external.broadcaster_active", false},
      {"external.controller_active", false},
      {"external.fault_present", false},
      {"external.hardware_active", true},
      {"external.transport_ready", true}};
  EXPECT_TRUE(holds(broadcaster.eligibility_predicate, State::kDeclared, off));
  EXPECT_TRUE(holds(broadcaster.eligibility_predicate, State::kDisabled, off));
  EXPECT_FALSE(holds(broadcaster.eligibility_predicate, State::kPrepared, off));
  for (const auto &[id, value] : off) {
    auto changed = off;
    changed[id] = !value;
    EXPECT_FALSE(holds(broadcaster.eligibility_predicate, State::kDeclared, changed))
        << id;
  }
  std::map<std::string, bool> on{{"external.broadcaster_active", true},
                                 {"external.fault_present", false},
                                 {"external.hardware_active", true},
                                 {"external.measured_joints_valid", false},
                                 {"external.transport_ready", true}};
  EXPECT_FALSE(holds(broadcaster.postcondition_predicate, State::kDeclared, on));
  on["external.measured_joints_valid"] = true;
  EXPECT_TRUE(holds(broadcaster.postcondition_predicate, State::kDeclared, on));
  // Prepared needs the robot ready, the broadcaster back, valid measured
  // joints and the controller inactive.
  const std::map<std::string, bool> prepared{
      {std::string(core::kDeviceAcceptsCommandsFactId), true},
      {"external.broadcaster_active", true},
      {"external.controller_active", false},
      {"external.controller_loaded", true},
      {"external.fault_present", false},
      {"external.hardware_active", true},
      {"external.measured_joints_valid", true},
      {"external.transport_ready", true}};
  EXPECT_TRUE(holds(prepare.requested_postcondition, State::kPrepared, prepared));
  for (const auto &[id, value] : prepared) {
    auto changed = prepared;
    changed[id] = !value;
    EXPECT_FALSE(holds(prepare.requested_postcondition, State::kPrepared, changed))
        << id;
  }
}

// M6 XADAPT: Recover clears the latched vendor error (C2 after an emergency
// stop) before any other effect, armed or not; nothing is switched off while
// an error is reported. A clear that does not take effect thus ends the
// Recover with nothing done.
TEST(XArmLifecycleProfile, RealRecoverClearsTheErrorBeforeAnyOtherEffect) {
  const auto plans = make_xarm_ros2_control_lifecycle_plans();
  const auto &recover = plan_for(plans, Intent::kRecover);
  const auto eligible = [&](const State state, const std::map<std::string, bool> &facts) {
    std::vector<std::string> ids;
    for (const auto &transition : recover.transitions)
      if (holds(transition.eligibility_predicate, state, facts))
        ids.push_back(transition.lifecycle_primitive_id.value());
    return ids;
  };
  const std::string error(core::kDeviceErrorPresentFactId);
  const std::string warning(core::kDeviceWarningPresentFactId);
  const std::vector<std::string> clear{std::string(core::kDeviceClearErrorPrimitiveId)};
  // The plugin left the hardware armed under C2 (live 2026-10-28), with or
  // without the trajectory controller: the clear only.
  for (const bool controller : {false, true})
    for (const auto state : {State::kFaulted, State::kDegraded}) {
      const std::map<std::string, bool> armed_error{
          {error, true},
          {warning, false},
          {"external.controller_active", controller},
          {"external.fault_present", true},
          {"external.hardware_active", true},
          {"external.transport_ready", true}};
      EXPECT_EQ(eligible(state, armed_error), clear) << controller;
    }
  // Unarmed with the error: the clear only, as before.
  EXPECT_EQ(eligible(State::kFaulted, {{error, true},
                                       {warning, false},
                                       {"external.controller_active", false},
                                       {"external.fault_present", false},
                                       {"external.hardware_active", false},
                                       {"external.transport_ready", true}}),
            clear);
  // Cleared: the controller, then the hardware, then the warning.
  const auto cleared = [&](const bool controller, const bool hardware, const bool fault) {
    return std::map<std::string, bool>{{error, false},
                                       {warning, true},
                                       {"external.controller_active", controller},
                                       {"external.fault_present", fault},
                                       {"external.hardware_active", hardware},
                                       {"external.transport_ready", true}};
  };
  EXPECT_EQ(eligible(State::kFaulted, cleared(true, true, false)),
            std::vector<std::string>{"ros2_control.controller.deactivate"});
  EXPECT_EQ(eligible(State::kFaulted, cleared(false, true, false)),
            std::vector<std::string>{"ros2_control.hardware.deactivate"});
  EXPECT_EQ(eligible(State::kDisabled, cleared(false, false, false)),
            std::vector<std::string>{std::string(kXArmClearWarningPrimitiveId)});
  // M6 XADAPT: disarmed and error-free with a fault left (the controller
  // manager or the robot did not answer in time): the status re-query.
  EXPECT_EQ(eligible(State::kFaulted, cleared(false, false, true)),
            std::vector<std::string>{"ros2_control.controller.recover"});
  // Without transport nothing is eligible.
  EXPECT_TRUE(eligible(State::kFaulted, {{error, true},
                                         {"external.controller_active", false},
                                         {"external.fault_present", false},
                                         {"external.hardware_active", true},
                                         {"external.transport_ready", false}})
                  .empty());
  // The clear's postcondition is the error gone, armed or not, and never
  // holds on a Faulted component: no Recover step does (a fault ends the
  // step at once).
  const auto &step = recover.transitions.front();
  EXPECT_TRUE(holds(step.postcondition_predicate, State::kDegraded,
                    {{error, false}, {"external.transport_ready", true}}));
  EXPECT_FALSE(holds(step.postcondition_predicate, State::kDegraded,
                     {{error, true}, {"external.transport_ready", true}}));
  for (const auto &transition : recover.transitions)
    EXPECT_FALSE(std::binary_search(transition.postcondition_predicate.allowed_states.begin(),
                                    transition.postcondition_predicate.allowed_states.end(),
                                    State::kFaulted))
        << transition.lifecycle_primitive_id.value();
}

TEST(XArmLifecycleProfile, RealRos2ControlPlansSelectAtMostOneStepPerObservation) {
  // CCLC requires exactly one eligible transition per fresh observation.
  std::set<std::string> ids;
  const auto plans = make_xarm_ros2_control_lifecycle_plans();
  for (const auto &plan : plans)
    for (const auto &transition : plan.transitions)
      for (const auto *predicate :
           {&transition.eligibility_predicate, &transition.postcondition_predicate})
        for (const auto &fact : predicate->fact_predicates)
          ids.insert(fact.lifecycle_fact_id.value());
  const std::vector<std::string> facts(ids.begin(), ids.end());
  ASSERT_LE(facts.size(), 16U);
  for (const auto &plan : plans) {
    for (const auto state :
         {State::kDeclared, State::kPrepared, State::kActive, State::kDegraded,
          State::kFaulted, State::kDisabled}) {
      for (std::uint32_t bits = 0U; bits < (1U << facts.size()); ++bits) {
        std::map<std::string, bool> observed;
        for (std::size_t index = 0U; index < facts.size(); ++index)
          observed[facts[index]] = (bits >> index) & 1U;
        const auto eligible = std::count_if(
            plan.transitions.begin(), plan.transitions.end(),
            [&](const auto &transition) {
              return holds(transition.eligibility_predicate, state, observed);
            });
        ASSERT_LE(eligible, 1) << "plan " << static_cast<int>(plan.intent)
                               << " state " << static_cast<int>(state)
                               << " facts " << bits;
      }
    }
  }
}

TEST(XArmLifecycleProfile, RealRos2ControlCatalogIsAnchoredByItsOwnProvider) {
  const auto semantic =
      compute_xarm_ros2_control_lifecycle_catalog_semantic_digest();
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  auto manifest = lifecycle_manifest(semantic.value());
  const auto catalog = make_xarm_ros2_control_lifecycle_catalog(manifest);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  EXPECT_EQ(catalog.value().plans.size(), 4U);
  // The shared provider and the native xArm provider are not this catalog.
  for (const std::string_view other :
       {"manyforge.provider.ros2_control", "xarm_arm_provider"}) {
    manifest.provider_manifest_id = core::ProviderManifestId(std::string(other));
    EXPECT_FALSE(make_xarm_ros2_control_lifecycle_catalog(manifest).ok()) << other;
  }
}

TEST(XArmLifecycleProfile, MaintainedRealProviderManifestAnchorsTheseRos2ControlPlans) {
  // The generator reads these plans from this package's data file
  // (`resources/xarm_ros2_control_lifecycle_plans.json`, V-A A7); the
  // maintained manifest carries their semantic digest.
  const auto semantic =
      compute_xarm_ros2_control_lifecycle_catalog_semantic_digest();
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  const auto manifest_bytes =
      maintained_manifest(kXArmRos2ControlLifecycleProviderManifestId);
  ASSERT_FALSE(manifest_bytes.empty());
  EXPECT_NE(manifest_bytes.find(semantic.value().value()), std::string::npos);
  // Pretty-printed, like the shared ros2_control manifest.
  EXPECT_NE(manifest_bytes.find(
                "\"provider_manifest_id\": \"manyforge.provider.ros2_control.xarm\""),
            std::string::npos);
}

} // namespace
} // namespace manyforge::xarm_control
