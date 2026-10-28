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

  // Prepare's `xarm.clear_error`; the next report shows no error.
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
// kept (not escalated)". The clear is verified: still reported
// `kXArmClearVerifyNs` after it returned it persists (the step fails typed in
// the adapter); the unarmed error stays facts, never a cause.
TEST(XArmBrandLifecyclePort, ErrorStillReportedAfterOwnClearPersistsWithoutEscalation) {
  Fixture fixture;
  fixture.Report(4, 0, 2, 0, 0);
  ASSERT_EQ(fixture.port.Observe(kNow).cause, Cause::kNone);
  // No report after the clear yet: pending.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, kNow);
  EXPECT_EQ(fixture.port.VerifyOwnClear(kNow), XArmClearVerification::kPending);
  // Within the verification window the report may still lag.
  fixture.Report(4, 0, 2, 0, 0, kNow + 1000 * kMs);
  EXPECT_EQ(fixture.port.VerifyOwnClear(kNow), XArmClearVerification::kPending);
  EXPECT_EQ(fixture.port.Observe(kNow + 1010 * kMs).cause, Cause::kNone);
  // Still C2 at the window: persists, with its code; still not a cause.
  fixture.Report(4, 0, 2, 0, 0, kNow + kXArmClearVerifyNs);
  EXPECT_EQ(fixture.port.VerifyOwnClear(kNow), XArmClearVerification::kPersists);
  EXPECT_EQ(fixture.port.LatestErrorCode(), 2);
  auto status = fixture.port.Observe(kNow + kXArmClearVerifyNs + 10 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(token_fact(status, kVendorCodeErrorFactId), "C2");
  EXPECT_EQ(token_fact(status, kVendorCodeConditionFactId),
            "xarm_error_persists_after_clear");
  expect_sorted_and_valid(status);
  // Released: the retried clear is verified at the first clean report.
  fixture.port.BeginOwnPrimitive(XArmLifecycleCommand::kClearError);
  const auto retried = kNow + 9100 * kMs;
  fixture.port.SettleOwnPrimitive(XArmLifecycleCommand::kClearError, true, retried);
  fixture.Report(4, 0, 0, 0, 0, retried + 200 * kMs);
  EXPECT_EQ(fixture.port.VerifyOwnClear(retried), XArmClearVerification::kCleared);
  status = fixture.port.Observe(retried + 210 * kMs);
  EXPECT_EQ(status.cause, Cause::kNone);
  EXPECT_EQ(boolean_fact(status, core::kDeviceErrorPresentFactId), false);
  EXPECT_EQ(find_fact(status, kVendorCodeConditionFactId), nullptr);
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

TEST(XArmLifecycleProfile, MaintainedGripperManifestUsesTheProductLifecycleProfile) {
  const auto semantic = compute_lite6_gripper_lifecycle_catalog_semantic_digest();
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  const auto manifest_bytes =
      maintained_manifest(kXArmGripperLifecycleProviderManifestId);
  ASSERT_FALSE(manifest_bytes.empty());
  EXPECT_NE(manifest_bytes.find(semantic.value().value()), std::string::npos);
}

TEST(XArmLifecycleProfile, MaintainedArmManifestCarriesOnlyTheClearRecoverPlan) {
  // R7 packages 5 and D12: `xarm_arm_provider`'s catalog is the generator's
  // xArm plans, which this package mirrors; the supervised ready sequence is gone.
  const auto semantic = compute_xarm_arm_lifecycle_catalog_semantic_digest();
  ASSERT_TRUE(semantic.ok()) << semantic.error().message;
  const auto manifest_bytes =
      maintained_manifest(kXArmArmLifecycleProviderManifestId);
  ASSERT_FALSE(manifest_bytes.empty());
  EXPECT_NE(manifest_bytes.find(semantic.value().value()), std::string::npos);
  EXPECT_NE(manifest_bytes.find("\"provider_manifest_id\":\"xarm_arm_provider\""),
            std::string::npos);
  EXPECT_TRUE(maintained_manifest("lite6_arm_provider").empty());
  EXPECT_TRUE(maintained_manifest("lite6_gripper_provider").empty());

  const auto plans = make_xarm_arm_lifecycle_plans();
  ASSERT_EQ(plans.size(), 1U);
  EXPECT_EQ(plans.front().intent, core::ComponentLifecycleIntentKindDTO::kRecover);
  ASSERT_EQ(plans.front().transitions.size(), 2U);
  EXPECT_EQ(plans.front().transitions[0].lifecycle_primitive_id.value(),
            kXArmClearErrorPrimitiveId);
  EXPECT_EQ(plans.front().transitions[1].lifecycle_primitive_id.value(),
            kXArmClearWarningPrimitiveId);
  // The catalog verifies against a manifest anchored by the same digest that
  // offers exactly its intents: observe and recover.
  std::vector<core::ProviderCapabilityOfferDTO> offers;
  for (const std::string_view id :
       {"component.lifecycle.observe", "component.lifecycle.recover"}) {
    const auto *definition = core::find_builtin_capability_definition(id);
    ASSERT_NE(definition, nullptr);
    offers.push_back({core::CapabilityId(std::string(id)),
                      core::CapabilityDefinitionRevision(
                          std::string(definition->capability_definition_revision)),
                      {}});
  }
  core::ProviderManifestDTO manifest{
      core::ProviderManifestId(std::string(kXArmArmLifecycleProviderManifestId)),
      core::ProviderManifestRevision(
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      std::move(offers),
      {},
      core::ProviderLifecyclePlanCatalogDeclarationDTO{
          core::SchemaId(std::string(core::kBrandLifecyclePlanCatalogApiVersion)),
          semantic.value()}};
  const auto catalog = make_xarm_arm_lifecycle_catalog(manifest);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  manifest.provider_manifest_id = core::ProviderManifestId("lite6_arm_provider");
  EXPECT_FALSE(make_xarm_arm_lifecycle_catalog(manifest).ok());
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
            (std::vector<std::string>{std::string(kXArmClearErrorPrimitiveId),
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
  // D10: the two vendor services after the controller and the hardware.
  const auto &recover = plan_for(plans, Intent::kRecover);
  EXPECT_EQ(recover.requested_state, State::kDisabled);
  EXPECT_EQ(primitive_ids(recover),
            (std::vector<std::string>{"ros2_control.controller.deactivate",
                                      "ros2_control.hardware.deactivate",
                                      std::string(kXArmClearErrorPrimitiveId),
                                      std::string(kXArmClearWarningPrimitiveId)}));

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
  // The generator's `xarm_ros2_control_lifecycle_plans` mirrors these plans;
  // the maintained manifest carries their semantic digest.
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
