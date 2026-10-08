#include <manyforge_core/execution_kernel.hpp>

namespace manyforge::core {

namespace {

GripperCommandResultDTO gripper_not_configured_result() {
  return GripperCommandResultDTO{false, 0, 0,
                                 ErrorDTO{"gripper_not_configured",
                                          "No gripper command backend is configured",
                                          {{"error_class", kErrorUnsupportedCapability}}}};
}

GripperCommandResultDTO gripper_not_authorized_result(ExecutionState state) {
  return GripperCommandResultDTO{
      false, 0, 0,
      ErrorDTO{"gripper_command_not_authorized",
               "Gripper commands are not authorized in the current execution lifecycle state",
               {{"state", std::string(to_string(state))}}}};
}

}  // namespace

GripperCommandResultDTO ExecutionKernel::command_gripper(const GripperCommandRequestDTO& request) {
  if (gripper_command_backend_ == nullptr) {
    return gripper_not_configured_result();
  }
  const auto state = sm_.status().state;
  switch (state) {
    using enum ExecutionState;
    case kStopping:
    case kStopped:
    case kInterrupted:
    case kFailed:
    case kInterventionRequired:
      return gripper_not_authorized_result(state);
    default:
      break;
  }
  return gripper_command_backend_->command_gripper(request);
}

Result<void> ExecutionKernel::validate_gripper_command(
    const GripperCommandRequestDTO& request) const {
  if (gripper_command_backend_ == nullptr) {
    return Result<void>::Err(*gripper_not_configured_result().error);
  }
  const auto state = sm_.status().state;
  switch (state) {
    using enum ExecutionState;
    case kStopping:
    case kStopped:
    case kInterrupted:
    case kFailed:
    case kInterventionRequired:
      return Result<void>::Err(*gripper_not_authorized_result(state).error);
    default:
      break;
  }
  return gripper_command_backend_->validate_gripper_command(request);
}

GripperCommandStatusDTO ExecutionKernel::get_gripper_status(const std::string& gripper_id) const {
  if (gripper_command_backend_ == nullptr) {
    GripperCommandStatusDTO status;
    status.state = GripperCommandState::kFailed;
    status.gripper_id = gripper_id;
    status.error = gripper_not_configured_result().error;
    return status;
  }
  return gripper_command_backend_->get_gripper_status(gripper_id);
}

Result<std::uint64_t> ExecutionKernel::stop_gripper(const std::string& gripper_id,
                                                    const std::uint64_t expected_command_id,
                                                    const std::uint64_t expected_generation) {
  if (gripper_command_backend_ == nullptr) {
    return Result<std::uint64_t>::Err(*gripper_not_configured_result().error);
  }
  const auto before = gripper_command_backend_->get_gripper_status(gripper_id);
  if (before.gripper_id != gripper_id || before.command_id == 0U ||
      before.command_id != expected_command_id || before.generation != expected_generation ||
      before.state != GripperCommandState::kExecuting) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{"stale_gripper_state",
                 "Gripper identity or lifecycle changed before scoped Stop",
                 {{"reason", "stale_gripper_state"}}});
  }
  invalidate_gripper_authority(GripperInvalidationReason::kStop);
  const auto after = gripper_command_backend_->get_gripper_status(gripper_id);
  if (after.gripper_id != gripper_id || after.command_id != expected_command_id ||
      after.generation == expected_generation || after.state != GripperCommandState::kIdle) {
    return Result<std::uint64_t>::Err(ErrorDTO{"gripper_stop_unconfirmed",
                                               "Gripper Stop did not publish a new idle generation",
                                               {{"reason", "gripper_stop_unconfirmed"}}});
  }
  return Result<std::uint64_t>::Ok(after.generation);
}

Result<void> ExecutionKernel::reset_gripper(const std::string& gripper_id,
                                            const std::uint64_t expected_command_id,
                                            const std::uint64_t expected_generation) {
  if (gripper_command_backend_ == nullptr) {
    return Result<void>::Err(*gripper_not_configured_result().error);
  }
  const auto before = gripper_command_backend_->get_gripper_status(gripper_id);
  if (before.gripper_id != gripper_id || before.command_id == 0U ||
      before.command_id != expected_command_id || before.generation < expected_generation ||
      before.state == GripperCommandState::kExecuting ||
      (before.generation != expected_generation && before.state != GripperCommandState::kIdle)) {
    return Result<void>::Err(ErrorDTO{"stale_gripper_state",
                                      "Gripper identity or lifecycle changed before scoped Reset",
                                      {{"reason", "stale_gripper_state"}}});
  }
  invalidate_gripper_authority(GripperInvalidationReason::kReset);
  authorize_gripper_authority();
  return Result<void>::Ok();
}

GripperCommandResultDTO ExecutionKernel::mark_gripper_grasp_lost(const std::string& gripper_id,
                                                                 std::uint64_t expected_command_id,
                                                                 std::uint64_t expected_generation,
                                                                 const std::string& evidence_label,
                                                                 const std::string& message,
                                                                 const std::string& reason_code) {
  if (gripper_command_backend_ == nullptr) {
    return gripper_not_configured_result();
  }
  return gripper_command_backend_->mark_gripper_grasp_lost(
      gripper_id, expected_command_id, expected_generation, evidence_label, message, reason_code);
}

void ExecutionKernel::invalidate_gripper_authority(GripperInvalidationReason reason) noexcept {
  if (gripper_command_backend_ != nullptr) {
    gripper_command_backend_->invalidate_gripper_commands(reason);
  }
}

void ExecutionKernel::authorize_gripper_authority() noexcept {
  if (gripper_command_backend_ != nullptr) {
    gripper_command_backend_->authorize_gripper_commands();
  }
}

}  // namespace manyforge::core
