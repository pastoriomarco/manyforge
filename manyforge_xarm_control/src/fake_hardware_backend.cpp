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

#include <manyforge_xarm_control/fake_hardware_backend.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::xarm_control
{
namespace
{

bool valid_command(XArmLifecycleCommand command)
{
  switch (command) {
    case XArmLifecycleCommand::kClearError:
    case XArmLifecycleCommand::kClearWarning:
    case XArmLifecycleCommand::kEnableMotion:
    case XArmLifecycleCommand::kDisableMotion:
    case XArmLifecycleCommand::kSelectJointServoMode:
    case XArmLifecycleCommand::kSetReadyState:
    case XArmLifecycleCommand::kSetStoppedState:
    case XArmLifecycleCommand::kShutdownController:
    case XArmLifecycleCommand::kActivateHardwareComponent:
    case XArmLifecycleCommand::kActivateTrajectoryControllers:
    case XArmLifecycleCommand::kDeactivateTrajectoryControllers:
    case XArmLifecycleCommand::kDeactivateHardwareComponent:
      return true;
  }
  return false;
}

}  // namespace

FakeXArmHardwareBackend::FakeXArmHardwareBackend(
  std::string owner_id,
  std::size_t joint_count,
  std::int64_t observation_lease_ns)
: owner_id_(std::move(owner_id)),
  observation_lease_ns_(observation_lease_ns)
{
  if (owner_id_.empty()) {
    throw std::invalid_argument("fake xArm owner_id is required");
  }
  if (joint_count == 0 || joint_count > 32) {
    throw std::invalid_argument("fake xArm joint_count is invalid");
  }
  if (observation_lease_ns_ <= 0) {
    throw std::invalid_argument("fake xArm observation lease is invalid");
  }
  observation_.transport_owner_component_epoch =
    owner_id_ + ":fake-epoch-1";
  observation_.generation = 1;
  observation_.transport_connected = FactState::kTrue;
  observation_.identity_verified = FactState::kTrue;
  observation_.position_valid = FactState::kTrue;
  observation_.position_ever_initialized = FactState::kTrue;
  observation_.stationary = FactState::kTrue;
  observation_.controller_shutdown_eligible = FactState::kFalse;
  observation_.motion_enabled = FactState::kFalse;
  observation_.vendor_control_ready = FactState::kFalse;
  observation_.hardware_component_active = FactState::kFalse;
  observation_.trajectory_controller_active = FactState::kFalse;
  observation_.fault_present = FactState::kFalse;
  observation_.recovery_required = FactState::kFalse;
  observation_.mode = 0;
  observation_.state = 4;
  observation_.error_code = 0;
  observation_.warning_code = 0;
  joint_state_.generation = 1;
  joint_state_.positions.assign(joint_count, 0.0);
  joint_state_.velocities.assign(joint_count, 0.0);
  last_joint_write_.assign(joint_count, 0.0);
  last_joint_velocity_write_.assign(joint_count, 0.0);
}

std::string_view FakeXArmHardwareBackend::owner_id() const noexcept
{
  return owner_id_;
}

XArmObservedState FakeXArmHardwareBackend::observe(
  std::int64_t now_ns) const
{
  std::scoped_lock lock(mutex_);
  auto result = observation_;
  result.transport_owner_progress_sequence =
    now_ns > 0 ? static_cast<std::uint64_t>(now_ns) : 0;
  result.transport_owner_published_timestamp_ns = now_ns;
  result.transport_owner_received_timestamp_ns = now_ns;
  result.transport_owner_progress_healthy = now_ns > 0;
  result.transport_owner_progress_reason =
    now_ns > 0 ? "" : "fake_owner_clock_invalid";
  result.source_timestamp_ns = now_ns;
  if (now_ns >
    std::numeric_limits<std::int64_t>::max() - observation_lease_ns_)
  {
    result.fresh_until_ns = std::numeric_limits<std::int64_t>::max();
  } else {
    result.fresh_until_ns = now_ns + observation_lease_ns_;
  }
  return result;
}

XArmLifecycleCommandResult
FakeXArmHardwareBackend::execute_lifecycle(
  XArmLifecycleCommand command,
  std::string_view request_id,
  std::uint64_t expected_generation)
{
  std::scoped_lock lock(mutex_);
  XArmLifecycleCommandResult result;
  result.observed_generation = observation_.generation;
  if (request_id.empty() || request_id.size() > 128) {
    result.reason = "invalid_request_id";
    return result;
  }
  if (expected_generation != observation_.generation) {
    result.reason = "observation_generation_mismatch";
    return result;
  }
  if (!valid_command(command)) {
    result.reason = "invalid_lifecycle_command";
    return result;
  }
  result.state = XArmLifecycleResultState::kResponseReceived;
  result.permitted = true;
  if (observation_.transport_connected != FactState::kTrue) {
    result.reason = "transport_not_connected";
    return result;
  }
  result.attempted = true;
  result.return_code = next_return_code_;
  next_return_code_ = 0;
  command_log_.push_back(command);
  if (result.return_code != 0) {
    result.reason = "fake_command_result_ambiguous";
    return result;
  }
  result.reason = "command_succeeded";
  if (command == XArmLifecycleCommand::kShutdownController) {
    result.process_restart_required = true;
  }
  if (auto_apply_commands_) {
    apply_effect(command);
  } else {
    pending_effects_.push_back(command);
  }
  return result;
}

bool FakeXArmHardwareBackend::read_joint_state(JointStateSample & sample)
{
  std::scoped_lock lock(mutex_);
  if (observation_.transport_connected != FactState::kTrue ||
    observation_.position_valid != FactState::kTrue)
  {
    return false;
  }
  sample = joint_state_;
  return true;
}

bool FakeXArmHardwareBackend::write_joint_command(
  const JointCommandView & command)
{
  std::scoped_lock lock(mutex_);
  if (command.positions.size() != joint_state_.positions.size() ||
    command.velocities.size() != joint_state_.velocities.size() ||
    observation_.transport_connected != FactState::kTrue ||
    !command_gate_open_.load(std::memory_order_acquire))
  {
    return false;
  }
  last_joint_write_.assign(
    command.positions.begin(), command.positions.end());
  last_joint_velocity_write_.assign(
    command.velocities.begin(), command.velocities.end());
  joint_state_.positions = last_joint_write_;
  joint_state_.velocities = last_joint_velocity_write_;
  ++joint_state_.generation;
  joint_write_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

XArmCommandGateUpdate FakeXArmHardwareBackend::set_command_gate(
  bool open, std::int64_t valid_until_ns)
{
  const bool accepted = !open || valid_until_ns > 0;
  const bool next_open = accepted && open;
  const bool previous_open = command_gate_open_.exchange(
    next_open, std::memory_order_acq_rel);
  if (previous_open != next_open) {
    command_gate_transition_count_.fetch_add(
      1, std::memory_order_relaxed);
  }
  return {
    accepted ? XArmCommandGateUpdateState::kApplied :
    XArmCommandGateUpdateState::kRejected,
    next_open ? valid_until_ns : 0};
}

void FakeXArmHardwareBackend::set_hardware_component_active(bool active)
{
  std::scoped_lock lock(mutex_);
  observation_.hardware_component_active =
    active ? FactState::kTrue : FactState::kFalse;
  bump_generation();
}

void FakeXArmHardwareBackend::set_trajectory_controller_active(bool active)
{
  std::scoped_lock lock(mutex_);
  observation_.trajectory_controller_active =
    active ? FactState::kTrue : FactState::kFalse;
  bump_generation();
}

void FakeXArmHardwareBackend::set_transport_connected(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.transport_connected = value;
  bump_generation();
}

void FakeXArmHardwareBackend::set_identity_verified(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.identity_verified = value;
  bump_generation();
}

void FakeXArmHardwareBackend::set_position_valid(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.position_valid = value;
  update_vendor_control_ready();
  bump_generation();
}

void FakeXArmHardwareBackend::set_stationary(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.stationary = value;
  bump_generation();
}

void FakeXArmHardwareBackend::set_motion_enabled(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.motion_enabled = value;
  update_vendor_control_ready();
  bump_generation();
}

void FakeXArmHardwareBackend::set_vendor_control_ready(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.vendor_control_ready = value;
  bump_generation();
}

void FakeXArmHardwareBackend::set_vendor_mode_and_state(
  int mode, int state)
{
  std::scoped_lock lock(mutex_);
  observation_.mode = mode;
  observation_.state = state;
  update_vendor_control_ready();
  bump_generation();
}

void FakeXArmHardwareBackend::set_controller_shutdown_eligible(
  FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.controller_shutdown_eligible = value;
  bump_generation();
}

void FakeXArmHardwareBackend::set_fault_present(
  FactState value, int error_code)
{
  std::scoped_lock lock(mutex_);
  observation_.fault_present = value;
  observation_.error_code =
    value == FactState::kFalse ? 0 : error_code;
  update_vendor_control_ready();
  bump_generation();
}

void FakeXArmHardwareBackend::set_recovery_required(FactState value)
{
  std::scoped_lock lock(mutex_);
  observation_.recovery_required = value;
  update_vendor_control_ready();
  bump_generation();
}

void FakeXArmHardwareBackend::set_enable_motion_implies_vendor_ready(
  bool enabled)
{
  std::scoped_lock lock(mutex_);
  enable_motion_implies_vendor_ready_ = enabled;
}

void FakeXArmHardwareBackend::set_auto_apply_commands(bool enabled)
{
  std::scoped_lock lock(mutex_);
  auto_apply_commands_ = enabled;
}

void FakeXArmHardwareBackend::set_next_return_code(int return_code)
{
  std::scoped_lock lock(mutex_);
  next_return_code_ = return_code;
}

void FakeXArmHardwareBackend::apply_pending_effects()
{
  std::scoped_lock lock(mutex_);
  for (const auto & command : pending_effects_) {
    apply_effect(command);
  }
  pending_effects_.clear();
}

std::vector<XArmLifecycleCommand>
FakeXArmHardwareBackend::command_log() const
{
  std::scoped_lock lock(mutex_);
  return command_log_;
}

std::size_t FakeXArmHardwareBackend::joint_write_count() const noexcept
{
  return joint_write_count_.load(std::memory_order_relaxed);
}

std::size_t
FakeXArmHardwareBackend::command_gate_transition_count() const noexcept
{
  return command_gate_transition_count_.load(std::memory_order_relaxed);
}

std::vector<double> FakeXArmHardwareBackend::last_joint_write() const
{
  std::scoped_lock lock(mutex_);
  return last_joint_write_;
}

std::vector<double>
FakeXArmHardwareBackend::last_joint_velocity_write() const
{
  std::scoped_lock lock(mutex_);
  return last_joint_velocity_write_;
}

void FakeXArmHardwareBackend::bump_generation()
{
  ++observation_.generation;
}

void FakeXArmHardwareBackend::update_vendor_control_ready()
{
  const bool facts_known =
    observation_.motion_enabled != FactState::kUnknown &&
    observation_.position_valid != FactState::kUnknown &&
    observation_.fault_present != FactState::kUnknown &&
    observation_.recovery_required != FactState::kUnknown;
  if (!facts_known) {
    observation_.vendor_control_ready = FactState::kUnknown;
    return;
  }
  observation_.vendor_control_ready =
    observation_.motion_enabled == FactState::kTrue &&
    observation_.position_valid == FactState::kTrue &&
    observation_.fault_present == FactState::kFalse &&
    observation_.recovery_required == FactState::kFalse &&
    observation_.warning_code == 0 &&
    observation_.mode == 1 &&
    (observation_.state == 1 || observation_.state == 2) ?
    FactState::kTrue : FactState::kFalse;
}

void FakeXArmHardwareBackend::apply_effect(
  XArmLifecycleCommand command)
{
  switch (command) {
    case XArmLifecycleCommand::kClearError:
      observation_.fault_present = FactState::kFalse;
      observation_.recovery_required = FactState::kFalse;
      observation_.error_code = 0;
      update_vendor_control_ready();
      break;
    case XArmLifecycleCommand::kClearWarning:
      observation_.warning_code = 0;
      update_vendor_control_ready();
      break;
    case XArmLifecycleCommand::kEnableMotion:
      observation_.motion_enabled = FactState::kTrue;
      // The legacy fake action catalog intentionally models enable as the
      // complete test-only ready transition. Production Lite6 semantics are
      // available separately so target-plan tests exercise the real
      // mode/state transition stages.
      if (enable_motion_implies_vendor_ready_) {
        observation_.vendor_control_ready = observation_.motion_enabled;
      } else {
        update_vendor_control_ready();
      }
      break;
    case XArmLifecycleCommand::kDisableMotion:
      observation_.motion_enabled = FactState::kFalse;
      observation_.vendor_control_ready = FactState::kFalse;
      break;
    case XArmLifecycleCommand::kSelectJointServoMode:
      observation_.mode = 1;
      observation_.state = 5;
      update_vendor_control_ready();
      break;
    case XArmLifecycleCommand::kSetReadyState:
      // The restricted primitive sends set_state(0); the fake models the
      // documented feedback postcondition after that request.
      observation_.state = 2;
      update_vendor_control_ready();
      break;
    case XArmLifecycleCommand::kSetStoppedState:
      observation_.state = 4;
      update_vendor_control_ready();
      break;
    case XArmLifecycleCommand::kShutdownController:
      observation_.transport_connected = FactState::kFalse;
      observation_.controller_shutdown_eligible = FactState::kFalse;
      break;
    case XArmLifecycleCommand::kActivateHardwareComponent:
      observation_.hardware_component_active = FactState::kTrue;
      break;
    case XArmLifecycleCommand::kActivateTrajectoryControllers:
      observation_.trajectory_controller_active = FactState::kTrue;
      break;
    case XArmLifecycleCommand::kDeactivateTrajectoryControllers:
      observation_.trajectory_controller_active = FactState::kFalse;
      break;
    case XArmLifecycleCommand::kDeactivateHardwareComponent:
      observation_.hardware_component_active = FactState::kFalse;
      break;
  }
  bump_generation();
}

std::vector<XArmActionPlan> make_fake_xarm_action_plans()
{
  constexpr std::int64_t kFakeTimeoutNs = 1'000'000'000;
  std::vector<XArmActionPlan> plans;
  plans.push_back(
    XArmActionPlan{
      "robot.enable",
      "Enable fake robot",
      ConfirmationClass::kExplicit,
      "Test-only fake motion-enable transition.",
      "The fake backend reports motion enabled.",
      kFakeTimeoutNs,
      {
        {XArmConditionKind::kTransportConnected, 1},
        {XArmConditionKind::kIdentityVerified, 1},
        {XArmConditionKind::kPositionValid, 1},
        {XArmConditionKind::kFaultPresent, 0},
        {XArmConditionKind::kRecoveryRequired, 0},
      },
      {
        XArmLifecycleCommand::kEnableMotion,
      },
      {
        {XArmConditionKind::kMotionEnabled, 1},
      },
    });
  plans.push_back(
    XArmActionPlan{
      "robot.disable",
      "Disable fake robot",
      ConfirmationClass::kExplicit,
      "Test-only fake motion-disable transition.",
      "The fake backend reports motion disabled.",
      kFakeTimeoutNs,
      {
        {XArmConditionKind::kTransportConnected, 1},
        {XArmConditionKind::kIdentityVerified, 1},
      },
      {
        XArmLifecycleCommand::kDisableMotion,
      },
      {
        {XArmConditionKind::kMotionEnabled, 0},
      },
    });
  plans.push_back(
    XArmActionPlan{
      "robot.clear_fault",
      "Clear fake fault",
      ConfirmationClass::kHighConsequence,
      "Test-only fake error-clear transition.",
      "The fake backend reports no fault or recovery requirement.",
      kFakeTimeoutNs,
      {
        {XArmConditionKind::kTransportConnected, 1},
        {XArmConditionKind::kIdentityVerified, 1},
        {XArmConditionKind::kFaultPresent, 1},
      },
      {
        XArmLifecycleCommand::kClearError,
      },
      {
        {XArmConditionKind::kFaultPresent, 0},
        {XArmConditionKind::kRecoveryRequired, 0},
      },
    });
  return plans;
}

}  // namespace manyforge::xarm_control
