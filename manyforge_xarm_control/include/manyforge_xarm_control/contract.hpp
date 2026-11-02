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

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/geometry.hpp>

namespace manyforge::xarm_control {

/// The status fields of the latest `xarm_msgs/msg/RobotMsg` report of the
/// driver (`<driver namespace>/robot_states`, 5 Hz with `report_type: rich`).
/// The pose is never read. The joint angles are never a measured state (under
/// an e-stop the controller reports all zeros; measured joints come only from
/// the joint state broadcaster): they serve only the Enable gate's agreement
/// check, with no vendor error.
struct XArmObservedState {
  /// Receipt of the latest report on the adapter's clock; absent before the
  /// first report.
  std::optional<std::int64_t> received_timestamp_ns;
  int state = -1;
  int mode = -1;
  int error_code = -1;
  int warning_code = -1;
  /// One bit per axis, set while that axis's motor is enabled (`mt_able`).
  int motion_enable_mask = -1;
  /// `angle`, in axis order (radians); compared with the measured joints only.
  std::vector<double> joint_angles_rad;
  /// The controller's TCP offset (`offset`: x, y, z in mm, then roll, pitch,
  /// yaw in radians), as `set_tcp_offset` takes it.
  std::vector<double> tcp_offset{};
};

/// Read seam of the driver's status; the production one is the
/// `robot_states` subscription, tests provide a deterministic one.
class IXArmStatusSource {
public:
  virtual ~IXArmStatusSource() = default;
  /// Copy of the latest received report; no wait and no effect.
  [[nodiscard]] virtual XArmObservedState observe() const = 0;
};

/// `robot_states` arrives at 5 Hz (best effort, keep last 1); a report older
/// than five periods is stale.
inline constexpr std::int64_t kXArmStatusFreshnessNs = 1'000'000'000;
/// One `robot_states` period (5 Hz): the pause between reads of a wait.
inline constexpr std::int64_t kXArmReportPeriodNs = 200'000'000;
/// An own operation explains the robot's mode and state changes until a
/// report received after it shows joint-servo mode out of state 5, or for
/// at most this long after it ended (the vendor plugin deactivates and
/// reactivates its controllers after an activation through blocking
/// controller-manager calls; the former `kOwnPrimitiveSettleNs`).
inline constexpr std::int64_t kXArmOwnOperationSettleNs = 5'000'000'000LL;
/// The Enable gate's default largest difference between a measured joint and
/// the controller's reported angle of the same axis while stopped (per robot:
/// `lanes.real.joint_agreement_tolerance_rad`).
inline constexpr double kXArmJointAgreementToleranceRad = 0.02;

/// Controller modes and states (XARM_CONTROLLER_FACTS, "Controller states and
/// modes").
inline constexpr int kXArmJointServoMode = 1;
inline constexpr int kXArmMovingState = 1;
inline constexpr int kXArmReadyState = 2;
inline constexpr int kXArmConfigChangedState = 5;

/// One vendor call is bounded by the shared component call cap.
inline constexpr std::chrono::nanoseconds kXArmServiceResponseTimeout{
    core::kComponentCallResponseCapNs};

enum class XArmServiceCallOutcome : std::uint8_t {
  /// Refused before the request left (service missing, cancelled first).
  kNotSent,
  /// The driver answered `ret == 0`.
  kSucceeded,
  /// Sent, but no answer in time, cancelled, or a non-zero vendor return:
  /// never retried.
  kUnknown,
};

struct XArmServiceCallResult final {
  XArmServiceCallOutcome outcome{XArmServiceCallOutcome::kNotSent};
  /// Typed reason token (`xarm_service_*`); empty on success.
  std::string reason;
  /// The vendor's `ret`, when an answer arrived.
  std::optional<std::int32_t> vendor_return;
};

/// One `get_err_warn_code` read: the call, and the controller's error and
/// warning codes when it answered `ret == 0`.
struct XArmErrWarnRead final {
  XArmServiceCallResult call;
  std::optional<int> error;
  std::optional<int> warning;
};

/// The vendor-neutral code of a robot controller that did not answer a call
/// within its bound.
inline constexpr std::string_view kRobotControllerUnresponsiveCode =
    "robot_controller_unresponsive";

/// A sent vendor call that got no answer within its bound, as opposed to an
/// answer that refused or failed.
[[nodiscard]] inline bool XArmCallUnanswered(const std::string_view reason) {
  return reason == "xarm_service_timeout" || reason == "xarm_service_no_response";
}

/// The two I/O banks of an xArm-family robot: the control box's (CI/DI
/// inputs, CO/DO outputs) and the tool's (TI inputs, TO outputs).
enum class XArmIoBank : std::uint8_t { kController, kTool };

/// `system_control` values (the fork's service, CONTRACTS §13).
inline constexpr int kXArmSystemShutdown = 1;
inline constexpr int kXArmSystemReboot = 2;

/// The driver services the adapters call, each request once, never retried,
/// bounded by `timeout` and following `cancellation`. ROS-free; the
/// production implementation is `XArmDriverServices`.
class IXArmDriver {
public:
  virtual ~IXArmDriver() = default;
  [[nodiscard]] virtual XArmServiceCallResult CleanError(std::chrono::nanoseconds timeout,
                                                         std::stop_token cancellation) const = 0;
  /// `clean_warn`: clears the controller's warning.
  [[nodiscard]] virtual XArmServiceCallResult CleanWarn(std::chrono::nanoseconds timeout,
                                                        std::stop_token cancellation) const = 0;
  [[nodiscard]] virtual XArmErrWarnRead GetErrWarnCode(std::chrono::nanoseconds timeout,
                                                       std::stop_token cancellation) const = 0;
  [[nodiscard]] virtual XArmServiceCallResult SetState(int state, std::chrono::nanoseconds timeout,
                                                       std::stop_token cancellation) const = 0;
  /// `set_tcp_offset`: the TCP in the flange frame, sent as `xarm_tcp_offset`.
  [[nodiscard]] virtual XArmServiceCallResult
  SetTcpOffset(const core::Pose3DTO &tcp_in_flange, std::chrono::nanoseconds timeout,
               std::stop_token cancellation) const = 0;
  /// `set_tcp_load`: kg and the centre of mass in metres in the flange frame.
  [[nodiscard]] virtual XArmServiceCallResult
  SetTcpLoad(double mass_kg, const std::array<double, 3> &center_of_mass_m,
             std::chrono::nanoseconds timeout, std::stop_token cancellation) const = 0;
  /// `set_cgpio_digital` / `set_tgpio_digital` with the driver's defaults
  /// (no `sync=False`, no delay).
  [[nodiscard]] virtual XArmServiceCallResult
  SetDigitalOutput(XArmIoBank bank, int ionum, bool value, std::chrono::nanoseconds timeout,
                   std::stop_token cancellation) const = 0;
  /// The fork's `system_control` (1 = shut down, 2 = reboot).
  [[nodiscard]] virtual XArmServiceCallResult
  SystemControl(int value, std::chrono::nanoseconds timeout,
                std::stop_token cancellation) const = 0;
};

/// The driver's TCP offset of a pose in the flange frame: `[x, y, z]` in
/// millimetres, then `[roll, pitch, yaw]` in radians (the driver's SDK runs
/// with `is_radian`), fixed axes X-Y-Z, so R = Rz(yaw) Ry(pitch) Rx(roll), as
/// xArm Studio shows it.
[[nodiscard]] std::array<double, 6> xarm_tcp_offset(const core::Pose3DTO &pose);

} // namespace manyforge::xarm_control
