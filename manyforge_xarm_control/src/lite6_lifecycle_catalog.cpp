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

#include <manyforge_xarm_control/lite6_lifecycle_catalog.hpp>

#include <cstdint>
#include <vector>

namespace manyforge::xarm_control
{
namespace
{

constexpr std::int64_t kLifecycleTimeoutNs = 5'000'000'000;
constexpr std::int64_t kTargetLifecycleTimeoutNs = 30'000'000'000;
constexpr int kExternalJointServoMode = 1;
constexpr int kReadyFeedbackState = 2;
constexpr int kStoppedFeedbackState = 4;
constexpr int kModeChangedFeedbackState = 5;

XArmStateCondition condition(
  XArmConditionKind kind, int expected_value)
{
  return XArmStateCondition{kind, expected_value};
}

}  // namespace

std::vector<XArmActionPlan> make_lite6_xarm_action_plans()
{
  return {
    XArmActionPlan{
      "robot.clear_fault",
      "Clear robot fault",
      ConfirmationClass::kHighConsequence,
      "Calls only the xArm error-clear primitive; it does not enable drives "
      "or start control.",
      "The fault and recovery latch are clear while drives remain disabled "
      "and the robot remains stopped.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kFaultPresent, 1),
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
      {
        XArmLifecycleCommand::kClearError,
      },
      {
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.clear_warning",
      "Clear robot warning",
      ConfirmationClass::kExplicit,
      "Calls only the xArm warning-clear primitive while the robot is stopped "
      "and disabled.",
      "The warning is clear while drives remain disabled and the robot "
      "remains stopped.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kWarningPresent, 1),
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
      {
        XArmLifecycleCommand::kClearWarning,
      },
      {
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.enable",
      "Enable robot drives",
      ConfirmationClass::kHighConsequence,
      "Enables all Lite6 drives without selecting a control mode or starting "
      "trajectory control.",
      "All drive/brake bits are enabled, the two pose sources agree, and the "
      "robot remains stopped.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
      {
        XArmLifecycleCommand::kEnableMotion,
      },
      {
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.select_external_control",
      "Select trajectory control mode",
      ConfirmationClass::kExplicit,
      "Selects xArm joint-servo mode without starting control. The vendor "
      "then enters its MODE_CHANGED interlock.",
      "Joint-servo mode is selected, drives stay enabled, and vendor feedback "
      "is MODE_CHANGED (state 5), which is not motion-ready.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
      {
        XArmLifecycleCommand::kSelectJointServoMode,
      },
      {
        condition(XArmConditionKind::kModeEquals, kExternalJointServoMode),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kVendorControlReady, 0),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(
          XArmConditionKind::kStateEquals, kModeChangedFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.start_control",
      "Start trajectory control",
      ConfirmationClass::kHighConsequence,
      "Requests standby with xArm set_state(0); it does not activate the "
      "hardware/JTC or dispatch a trajectory.",
      "The vendor reports READY feedback (state 2) in enabled, pose-valid "
      "joint-servo mode. Trajectory dispatch remains separately gated.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kModeEquals, kExternalJointServoMode),
        condition(XArmConditionKind::kVendorControlReady, 0),
        condition(XArmConditionKind::kReadyStateRequestEligible, 1),
      },
      {
        XArmLifecycleCommand::kSetReadyState,
      },
      {
        condition(XArmConditionKind::kVendorControlReady, 1),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kModeEquals, kExternalJointServoMode),
        condition(
          XArmConditionKind::kStateEquals, kReadyFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.activate_hardware",
      "Activate robot hardware interface",
      ConfirmationClass::kExplicit,
      "Activates the generated ros2_control hardware component after the "
      "vendor controller is ready. It does not dispatch a trajectory.",
      "The generated hardware component is active while the trajectory "
      "controller remains inactive.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kVendorControlReady, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kHardwareComponentActive, 0),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
      },
      {
        XArmLifecycleCommand::kActivateHardwareComponent,
      },
      {
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
        condition(XArmConditionKind::kVendorControlReady, 1),
      },
    },
    XArmActionPlan{
      "robot.activate_trajectory_controller",
      "Activate trajectory controller",
      ConfirmationClass::kHighConsequence,
      "Activates the generated joint-state broadcaster and trajectory "
      "controller. Admission remains separately fail-closed.",
      "The hardware and generated trajectory controller are active with a "
      "fresh, admitted lifecycle state.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kVendorControlReady, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
      },
      {
        XArmLifecycleCommand::kActivateTrajectoryControllers,
      },
      {
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 1),
        condition(XArmConditionKind::kVendorControlReady, 1),
      },
    },
    XArmActionPlan{
      "robot.deactivate_trajectory_controller",
      "Deactivate trajectory controller",
      ConfirmationClass::kExplicit,
      "Deactivates the generated trajectory controller and joint-state "
      "broadcaster only after stationary state is observed.",
      "The trajectory controller is inactive while the hardware component "
      "remains active.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kStationary, 1),
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 1),
      },
      {
        XArmLifecycleCommand::kDeactivateTrajectoryControllers,
      },
      {
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
        condition(XArmConditionKind::kStationary, 1),
      },
    },
    XArmActionPlan{
      "robot.deactivate_hardware",
      "Deactivate robot hardware interface",
      ConfirmationClass::kExplicit,
      "Deactivates the generated ros2_control hardware component after "
      "trajectory control is inactive and stationary state is observed.",
      "Both the hardware component and trajectory controller are inactive.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kStationary, 1),
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
      },
      {
        XArmLifecycleCommand::kDeactivateHardwareComponent,
      },
      {
        condition(XArmConditionKind::kHardwareComponentActive, 0),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
        condition(XArmConditionKind::kStationary, 1),
      },
    },
    XArmActionPlan{
      "robot.stop_control",
      "Stop trajectory control",
      ConfirmationClass::kNone,
      "Changes only the xArm state to stopped; it does not disable drives.",
      "Vendor control is non-ready and state is stopped while drives remain "
      "enabled.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kMotionEnabled, 1),
      },
      {
        XArmLifecycleCommand::kSetStoppedState,
      },
      {
        condition(XArmConditionKind::kVendorControlReady, 0),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
    },
    XArmActionPlan{
      "robot.disable",
      "Disable robot drives",
      ConfirmationClass::kHighConsequence,
      "Disables all Lite6 drives only after the robot reports stopped.",
      "All drive/brake bits are disabled and vendor control remains stopped.",
      kLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
      {
        XArmLifecycleCommand::kDisableMotion,
      },
      {
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(XArmConditionKind::kVendorControlReady, 0),
        condition(
          XArmConditionKind::kStateEquals, kStoppedFeedbackState),
      },
    },
    XArmActionPlan{
      "shutdown_controller",
      "Shut down physical controller",
      ConfirmationClass::kHighConsequence,
      "Calls exactly xArm system_control(1) after the JTC, hardware command "
      "gate, drives, and motion are independently interlocked. It does not "
      "stop the ROS component and never exposes reboot.",
      "The vendor accepts controller shutdown and the current owner session "
      "is terminally fenced. Electrical power state remains external "
      "evidence; any later startup requires a fresh xarm_ros2 process.",
      kLifecycleTimeoutNs,
      {
        condition(
          XArmConditionKind::kControllerShutdownEligible, 1),
      },
      {
        XArmLifecycleCommand::kShutdownController,
      },
      {
        condition(
          XArmConditionKind::kControllerShutdownEligible, 0),
      },
      true,
    },
  };
}

std::vector<XArmTargetPlan> make_lite6_xarm_target_plans()
{
  return {
    XArmTargetPlan{
      std::string(robot_lifecycle::kPrepareForMotionActionId),
      "Enable robot",
      ConfirmationClass::kHighConsequence,
      "Enables the Lite6 drives, selects external joint-servo control, and "
      "activates the generated hardware and trajectory controller. It never "
      "dispatches a trajectory or clears a fault.",
      "Fresh, fault-free drive, vendor-control, ros2_control hardware, and "
      "trajectory-controller facts are all ready for motion admission.",
      kTargetLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
      },
      {
        XArmTargetStage{
          "robot.enable",
          {
            condition(XArmConditionKind::kMotionEnabled, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
            condition(
              XArmConditionKind::kStateEquals, kStoppedFeedbackState),
          },
        },
        XArmTargetStage{
          "robot.start_control",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
            condition(
              XArmConditionKind::kModeEquals, kExternalJointServoMode),
            condition(XArmConditionKind::kReadyStateRequestEligible, 1),
          },
        },
        XArmTargetStage{
          "robot.select_external_control",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
            condition(
              XArmConditionKind::kStateEquals, kStoppedFeedbackState),
          },
        },
        XArmTargetStage{
          "robot.activate_hardware",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
        XArmTargetStage{
          "robot.deactivate_trajectory_controller",
          {
            condition(XArmConditionKind::kPositionValid, 1),
            condition(XArmConditionKind::kStationary, 1),
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 1),
            condition(XArmConditionKind::kTrajectoryControllerActive, 1),
          },
        },
        XArmTargetStage{
          "robot.deactivate_hardware",
          {
            condition(XArmConditionKind::kPositionValid, 1),
            condition(XArmConditionKind::kStationary, 1),
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 1),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
        XArmTargetStage{
          "robot.activate_trajectory_controller",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 1),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
        // External/manual control may leave drives enabled while changing
        // vendor mode underneath active ROS controllers. Once the controller
        // layers are safely inactive, normalize to the documented stopped
        // state without cycling drive power; the earlier select/start stages
        // then rebuild only the missing command path.
        XArmTargetStage{
          "robot.stop_control",
          {
            condition(XArmConditionKind::kPositionValid, 1),
            condition(XArmConditionKind::kStationary, 1),
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
      },
      {
        condition(XArmConditionKind::kPositionValid, 1),
        condition(XArmConditionKind::kMotionEnabled, 1),
        condition(XArmConditionKind::kVendorControlReady, 1),
        condition(XArmConditionKind::kHardwareComponentActive, 1),
        condition(XArmConditionKind::kTrajectoryControllerActive, 1),
        condition(XArmConditionKind::kFaultPresent, 0),
        condition(XArmConditionKind::kRecoveryRequired, 0),
        condition(XArmConditionKind::kWarningPresent, 0),
      },
    },
    XArmTargetPlan{
      std::string(robot_lifecycle::kMakeNoncommandableActionId),
      "Disable robot",
      ConfirmationClass::kHighConsequence,
      "After ManyForge execution is stopped, deactivates the trajectory "
      "controller and hardware, stops xArm external control, and disables "
      "the drives while retaining observation.",
      "The generated trajectory controller and hardware are inactive, vendor "
      "control is stopped, and all Lite6 drives are disabled.",
      kTargetLifecycleTimeoutNs,
      {
        condition(XArmConditionKind::kTransportConnected, 1),
        condition(XArmConditionKind::kIdentityVerified, 1),
        condition(XArmConditionKind::kStationary, 1),
      },
      {
        XArmTargetStage{
          "robot.deactivate_trajectory_controller",
          {
            condition(XArmConditionKind::kStationary, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 1),
            condition(XArmConditionKind::kTrajectoryControllerActive, 1),
          },
        },
        XArmTargetStage{
          "robot.deactivate_hardware",
          {
            condition(XArmConditionKind::kStationary, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 1),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
        // Stages are priority-ordered: disable wins once stopped state is
        // observed; otherwise stop_control first advances external modes.
        XArmTargetStage{
          "robot.disable",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kVendorControlReady, 0),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
            condition(
              XArmConditionKind::kStateEquals, kStoppedFeedbackState),
          },
        },
        XArmTargetStage{
          "robot.stop_control",
          {
            condition(XArmConditionKind::kMotionEnabled, 1),
            condition(XArmConditionKind::kHardwareComponentActive, 0),
            condition(XArmConditionKind::kTrajectoryControllerActive, 0),
          },
        },
      },
      {
        condition(XArmConditionKind::kMotionEnabled, 0),
        condition(XArmConditionKind::kVendorControlReady, 0),
        condition(XArmConditionKind::kHardwareComponentActive, 0),
        condition(XArmConditionKind::kTrajectoryControllerActive, 0),
      },
    },
  };
}

}  // namespace manyforge::xarm_control
