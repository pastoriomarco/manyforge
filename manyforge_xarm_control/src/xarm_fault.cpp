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

#include <manyforge_xarm_control/xarm_fault.hpp>

namespace manyforge::xarm_control {

using core::FaultCode;
using core::FaultOperatorAction;

std::string_view xarm_error_text(const int code) noexcept {
  switch (code) {
  case 1: return "Hardware Emergency STOP effective";
  case 2: return "Emergency IO of Control Box is triggered";
  case 3: return "Emergency Stop of Three-state Switch triggered";
  case 10: return "Servo Motor Error";
  case 11: return "Servo Motor Error of Joint 1";
  case 12: return "Servo Motor Error of Joint 2";
  case 13: return "Servo Motor Error of Joint 3";
  case 14: return "Servo Motor Error of Joint 4";
  case 15: return "Servo Motor Error of Joint 5";
  case 16: return "Servo Motor Error of Joint 6";
  case 17: return "Servo Motor Error of Joint 7";
  case 19: return "End Module Communication Error";
  case 21: return "Kinematic Error";
  case 22: return "Self-collision Error";
  case 23: return "Joint Angle Exceed Limit";
  case 24: return "Speed Exceeds Limit";
  case 25: return "Planning Error";
  case 26: return "System Real Time Error";
  case 27: return "Command Reply Error";
  case 29: return "Other Errors, please contact technical support";
  case 30: return "Feedback Speed Exceeds limit";
  case 31: return "Collision Caused Abnormal Joint Current";
  case 32: return "Circle Calculation Error";
  case 33: return "Controller GPIO Error";
  case 34: return "Trajectory Recording Timeout";
  case 35: return "Exceed Safety Boundary";
  case 36: return "Number of Delayed Command Exceed Limit";
  case 37: return "Abnormal Motion in Manual Mode";
  case 38: return "Abnormal Joint Angle";
  case 39: return "Abnormal Communication Between Master and Slave IC of Power Board";
  case 50: return "Tool Force/Torque Sensor Error";
  case 51: return "Tool Force Torque Sensor Mode Setting Error";
  case 52: return "Tool Force Torque Sensor Zero Setting Error";
  case 53: return "Tool Force Torque Sensor Overload";
  case 60: return "Linear Speed Exceeds Limit";
  case 110: return "Robot Arm Base Board Communication Error";
  case 111: return "Control Box External RS485 Device Communication Error";
  default: return "Abnormal Error Code, please contact support";
  }
}

std::string xarm_vendor_code(const int code) { return "C" + std::to_string(code); }

bool xarm_drive_error(const int code) noexcept { return code >= 10 && code <= 17; }

std::optional<core::Fault> xarm_fault(const int code, const std::int64_t since_ns) {
  if (code <= 0) return std::nullopt;
  FaultCode fault = FaultCode::kVendorFault;
  auto action = FaultOperatorAction::kRecover;
  bool clearable = true;
  switch (code) {
  case 1:
  case 2:
  case 3:
    // Latched after release until cleared: the owner's Recover clears it.
    fault = FaultCode::kEstop;
    break;
  case 31: fault = FaultCode::kProtectiveStop; break;
  case 23:
  case 24:
  case 35:
  case 60: fault = FaultCode::kLimitViolation; break;
  case 22: fault = FaultCode::kSelfCollision; break;
  case 21:
  case 25: fault = FaultCode::kMotionInvalid; break;
  case 19:
  case 33: fault = FaultCode::kCommunication; break;
  default:
    if (xarm_drive_error(code)) {
      fault = FaultCode::kDriveFault;
      action = FaultOperatorAction::kNone;
      clearable = false;
    }
    break;
  }
  const auto text = std::string(xarm_error_text(code));
  return core::Fault{fault, xarm_vendor_code(code) + ": " + text, clearable, action,
                     xarm_vendor_code(code), text, {}, since_ns};
}

XArmFaultScope xarm_fault_scope(const int code) noexcept {
  if (code == 19) return XArmFaultScope::kToolIo;
  if (code == 33) return XArmFaultScope::kControllerIo;
  if (xarm_drive_error(code)) return XArmFaultScope::kArm;
  switch (code) {
  case 21:
  case 22:
  case 23:
  case 24:
  case 25:
  case 31:
  case 35:
  case 60: return XArmFaultScope::kArm;
  default: return XArmFaultScope::kMachine;
  }
}

core::Fault xarm_external_control_fault(const int mode, const int state,
                                        const std::int64_t since_ns) {
  const auto vendor = "mode=" + std::to_string(mode) + " state=" + std::to_string(state);
  return core::Fault{FaultCode::kExternalControl,
                     "the robot was taken over outside ManyForge (" + vendor +
                         "): Recover the machine, then Enable the arm",
                     true,
                     FaultOperatorAction::kRecover,
                     vendor,
                     "mode or state changed outside ManyForge",
                     {},
                     since_ns};
}

} // namespace manyforge::xarm_control
