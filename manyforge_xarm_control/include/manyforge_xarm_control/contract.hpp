// Copyright 2026 Flexin Group SRL
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
#include <string>
#include <vector>

namespace manyforge::xarm_control {

/// Closed three-valued observation used only at the restricted xArm transport
/// boundary. It carries no caller authority, lease, or admission state.
enum class FactState : std::uint8_t {
  kUnknown,
  kFalse,
  kTrue,
};

struct XArmObservedState {
  /// Fresh startup/configuration epoch generated inside the sole xarm_ros2
  /// transport owner. It is not a deployment identifier.
  std::string transport_owner_component_epoch;
  /// Receiver-local evidence that the xarm_ros2 owner executor progressed.
  std::uint64_t transport_owner_progress_sequence = 0;
  std::int64_t transport_owner_published_timestamp_ns = 0;
  std::int64_t transport_owner_received_timestamp_ns = 0;
  bool transport_owner_progress_healthy = false;
  std::string transport_owner_progress_reason;
  /// Increment whenever any semantic field below changes. Timestamp-only
  /// freshness renewal retains this generation.
  std::uint64_t generation = 0;
  std::int64_t source_timestamp_ns = 0;
  std::int64_t fresh_until_ns = 0;
  FactState transport_connected = FactState::kUnknown;
  FactState identity_verified = FactState::kUnknown;
  FactState position_valid = FactState::kUnknown;
  FactState position_ever_initialized = FactState::kUnknown;
  FactState stationary = FactState::kUnknown;
  FactState controller_shutdown_eligible = FactState::kUnknown;
  FactState motion_enabled = FactState::kUnknown;
  FactState vendor_control_ready = FactState::kUnknown;
  FactState hardware_component_active = FactState::kFalse;
  FactState trajectory_controller_active = FactState::kFalse;
  FactState fault_present = FactState::kUnknown;
  FactState recovery_required = FactState::kUnknown;
  int identity_axis = -1;
  int identity_device_type = -1;
  int mode = -1;
  int state = -1;
  int command_count = -1;
  int brake_mask = -1;
  int servo_enable_mask = -1;
  int error_code = -1;
  int warning_code = -1;
  std::uint64_t report_sample_count = 0;
  std::uint64_t joint_read_attempt_count = 0;
  std::uint64_t joint_read_success_count = 0;
  std::uint64_t joint_write_attempt_count = 0;
  std::uint64_t lifecycle_command_attempt_count = 0;
  std::uint64_t shutdown_controller_attempt_count = 0;
  std::vector<std::string> blocking_reason_codes;
};

/// Semantic operations admitted by the build-owned Lite6 lifecycle catalog.
/// These are never caller-supplied raw SDK mode/state/system-control values.
enum class XArmLifecycleCommand : std::uint8_t {
  kClearError = 1,
  kClearWarning = 2,
  kEnableMotion = 3,
  kDisableMotion = 4,
  kSelectJointServoMode = 5,
  kSetReadyState = 6,
  kSetStoppedState = 7,
  kShutdownController = 8,
  kActivateHardwareComponent = 9,
  kActivateTrajectoryControllers = 10,
  kDeactivateTrajectoryControllers = 11,
  kDeactivateHardwareComponent = 12,
};

enum class XArmLifecycleResultState : std::uint8_t {
  kNotDispatched,
  kResponseReceived,
  kOutcomeUnknownAfterDispatch,
};

struct XArmLifecycleCommandResult {
  XArmLifecycleResultState state = XArmLifecycleResultState::kNotDispatched;
  bool permitted = false;
  bool attempted = false;
  int return_code = -1;
  std::string reason;
  std::uint64_t observed_generation = 0;
  bool process_restart_required = false;
};

} // namespace manyforge::xarm_control
