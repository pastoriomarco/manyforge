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

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::xarm_control {

/// The status fields of the latest `xarm_msgs/msg/RobotMsg` report of the
/// unmodified vendor driver (`<driver namespace>/robot_states`). The TCP pose
/// is deliberately absent. The controller reports all-zero angles under an
/// emergency stop, so the angles are never a measured state (R7 §3.5):
/// measured joints come only from the joint state broadcaster. The angles
/// serve only the Enable gate's agreement check, with no vendor error (§3.6).
struct XArmObservedState {
  /// Receipt of the latest report on the adapter's clock; absent before the
  /// first report.
  std::optional<std::int64_t> received_timestamp_ns;
  int state = -1;
  int mode = -1;
  int error_code = -1;
  int warning_code = -1;
  /// One bit per axis, set while that axis's servo is enabled (`mt_able`).
  int motion_enable_mask = -1;
  /// `angle`, in axis order (radians); compared with the measured joints only.
  std::vector<double> joint_angles_rad;
  /// M6 ADAPTERM (item 44): the controller's TCP offset (`offset`: x, y, z
  /// in mm, then roll, pitch, yaw in radians), as `set_tcp_offset` takes it.
  std::vector<double> tcp_offset{};
};

/// ManyForge's own vendor operations whose expected effect explains the
/// mode/state change they cause (R7 package 4 dispatches them). Arming and
/// disarming are the hardware component's activation and deactivation: the
/// vendor plugin's `on_activate` arms (clean error/warning, motion enable,
/// servo mode, ready state) and its `on_deactivate` selects position mode.
enum class XArmLifecycleCommand : std::uint8_t {
  kClearError = 1,
  kClearWarning = 2,
  kArm = 3,
  kDisarm = 4,
};

/// M6 XADAPT: the vendor-neutral, user-facing code of a robot controller
/// that did not answer within a call's bound. The arm then reports a
/// component fault (an incident) until a later query is answered.
inline constexpr std::string_view kRobotControllerUnresponsiveCode =
    "robot_controller_unresponsive";

/// M6 XADAPT: a sent vendor call that got no answer within its bound (the
/// driver services' `xarm_service_timeout`, `xarm_service_no_response`), as
/// opposed to an answer that refused or failed.
[[nodiscard]] inline bool XArmCallUnanswered(const std::string_view reason) {
  return reason == "xarm_service_timeout" || reason == "xarm_service_no_response";
}

} // namespace manyforge::xarm_control
