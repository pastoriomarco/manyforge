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

#include <span>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_robot_lifecycle/contract.hpp>

namespace manyforge::xarm_control
{

using manyforge::robot_lifecycle::ConfirmationClass;
using manyforge::robot_lifecycle::FactState;

struct XArmObservedState
{
  /// Fresh startup/configuration epoch generated inside the sole xarm_ros2
  /// transport owner. It is not a deployment identifier.
  std::string transport_owner_component_epoch;
  /// Receiver-local evidence that the xarm_ros2 owner executor progressed.
  ///
  /// This is deliberately independent of robot-observation freshness. During
  /// one explicitly bounded lifecycle primitive, owner progress may remain
  /// healthy while motion admission is closed on stale robot state.
  std::uint64_t transport_owner_progress_sequence = 0;
  std::int64_t transport_owner_published_timestamp_ns = 0;
  std::int64_t transport_owner_received_timestamp_ns = 0;
  bool transport_owner_progress_healthy = false;
  std::string transport_owner_progress_reason;
  /// Increment whenever any semantic field below changes. Timestamp-only
  /// freshness renewal retains this generation.
  std::uint64_t generation = 0;
  /// Monotonic timestamps in the coordinator process's steady-clock domain.
  /// A backend must not manufacture a future lease without a fresh report.
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

enum class XArmConditionKind
{
  kTransportConnected,
  kIdentityVerified,
  kPositionValid,
  kMotionEnabled,
  kVendorControlReady,
  kHardwareComponentActive,
  kTrajectoryControllerActive,
  kFaultPresent,
  kRecoveryRequired,
  kWarningPresent,
  kStationary,
  kControllerShutdownEligible,
  kModeEquals,
  kStateEquals,
  kReadyStateRequestEligible,
};

struct XArmStateCondition
{
  XArmConditionKind kind = XArmConditionKind::kTransportConnected;
  /// Boolean facts use 0/1. Mode/state conditions use the exact vendor value.
  int expected_value = 0;
};

/// The complete ROS-facing lifecycle command vocabulary.
///
/// These are semantic operations, not pass-through SDK calls. In particular,
/// callers cannot supply raw mode, state, servo, or system-control values.
enum class XArmLifecycleCommand : std::uint8_t
{
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

/// Whether the NRT transport obtained an authoritative owner result.
///
/// A request that was dispatched but did not return before its deadline must
/// never be treated as a known rejection or automatically sent again. The
/// physical primitive may already have run.
enum class XArmLifecycleResultState : std::uint8_t
{
  kNotDispatched,
  /// Accepted by the adapter's single NRT lifecycle worker. This is an
  /// internal hand-off state, not an authoritative robot result and never an
  /// audit terminal. The matching worker completion must be fenced back into
  /// the same lifecycle transaction before its result can be interpreted.
  kExecutionDeferred,
  kResponseReceived,
  kOutcomeUnknownAfterDispatch,
};

struct XArmLifecycleCommandResult
{
  XArmLifecycleResultState state =
    XArmLifecycleResultState::kNotDispatched;
  bool permitted = false;
  bool attempted = false;
  int return_code = -1;
  std::string reason;
  std::uint64_t observed_generation = 0;
  bool process_restart_required = false;
};

/// One bounded, NRT audit record for an attempted restricted primitive.
///
/// The generic lifecycle transaction remains robot-family neutral. This
/// xArm-specific record preserves the exact adapter-to-owner operation and
/// vendor return without leaking either into manyforge_robot_lifecycle.
struct XArmPrimitiveAuditRecord
{
  std::uint64_t sequence = 0;
  std::int64_t occurred_at_ns = 0;
  std::uint64_t transaction_generation = 0;
  /// Outer generic intent or directly requested diagnostic primitive.
  std::string action_id;
  /// Adapter-owned primitive stage selected within the outer intent.
  std::string stage_action_id;
  std::size_t step_index = 0;
  std::string transport_request_id;
  std::uint64_t expected_observation_generation = 0;
  XArmLifecycleCommand command = XArmLifecycleCommand::kClearError;
  XArmLifecycleCommandResult result;
};

struct XArmActionPlan
{
  std::string action_id;
  std::string label;
  ConfirmationClass confirmation = ConfirmationClass::kExplicit;
  std::string side_effect_summary;
  std::string expected_postcondition;
  std::int64_t timeout_ns = 0;
  std::vector<XArmStateCondition> preconditions;
  std::vector<XArmLifecycleCommand> steps;
  std::vector<XArmStateCondition> postconditions;
  /// The physical-controller shutdown channel disappears after acceptance.
  /// Such a plan succeeds on one authoritative return code rather than a
  /// later observation through the intentionally lost session.
  bool terminal_on_known_success = false;
};

struct XArmTargetStage
{
  /// References one adapter-internal XArmActionPlan.
  std::string action_id;
  /// The stage is eligible only when every condition is currently true.
  std::vector<XArmStateCondition> eligibility;
};

/// One cross-vendor target intent mapped to an xArm-specific resumable plan.
///
/// The coordinator executes at most one newly eligible stage for each fresh
/// observation generation. The generic supervisor retains the outer request,
/// deadline, replay identity, and final transaction result.
struct XArmTargetPlan
{
  std::string action_id;
  std::string label;
  ConfirmationClass confirmation = ConfirmationClass::kExplicit;
  std::string side_effect_summary;
  std::string expected_postcondition;
  std::int64_t timeout_ns = 0;
  std::vector<XArmStateCondition> preconditions;
  std::vector<XArmTargetStage> stages;
  std::vector<XArmStateCondition> postconditions;
};

struct JointStateSample
{
  std::uint64_t generation = 0;
  std::vector<double> positions;
  std::vector<double> velocities;
};

struct JointCommandView
{
  std::span<const double> positions;
  std::span<const double> velocities;
};

enum class XArmCommandGateUpdateState : std::uint8_t
{
  kRejected,
  kPending,
  kApplied,
};

/// Result of one desired command-gate lease update.
///
/// Production may apply the ROS service asynchronously. An open gate is
/// usable only when kApplied carries a still-future effective_valid_until_ns.
/// A pending or rejected update always leaves coordinator admission closed.
struct XArmCommandGateUpdate
{
  XArmCommandGateUpdateState state =
    XArmCommandGateUpdateState::kRejected;
  std::int64_t effective_valid_until_ns = 0;
};

class IXArmHardwareBackend
{
public:
  virtual ~IXArmHardwareBackend() = default;

  IXArmHardwareBackend(const IXArmHardwareBackend &) = delete;
  IXArmHardwareBackend & operator=(const IXArmHardwareBackend &) = delete;

  [[nodiscard]] virtual std::string_view owner_id() const noexcept = 0;
  [[nodiscard]] virtual XArmObservedState observe(
    std::int64_t now_ns) const = 0;
  /// NRT lifecycle and observation calls may run concurrently with the
  /// controller-manager read/write loop. Implementations must synchronize
  /// that boundary. Production read/write implementations must remain
  /// bounded and non-blocking.
  virtual XArmLifecycleCommandResult execute_lifecycle(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) = 0;
  virtual bool read_joint_state(JointStateSample & sample) = 0;
  virtual bool write_joint_command(
    const JointCommandView & command) = 0;
  /// Synchronize the transport command gate with the immutable admission
  /// lease. A false return means the backend remains closed.
  virtual XArmCommandGateUpdate set_command_gate(
    bool open, std::int64_t valid_until_ns) = 0;
  virtual void set_hardware_component_active(bool active) = 0;
  virtual void set_trajectory_controller_active(bool active) = 0;

protected:
  IXArmHardwareBackend() = default;
};

}  // namespace manyforge::xarm_control
