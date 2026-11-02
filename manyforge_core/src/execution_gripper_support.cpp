#include <manyforge_core/execution_kernel.hpp>

namespace manyforge::core {

namespace {

// Gripper commands are executed by the separate gripper participant
// (GripperParticipantEngine). The motion participant implements the shared
// engine interface but owns no gripper.
ErrorDTO gripper_not_configured_error() {
  return ErrorDTO{"gripper_not_configured",
                  "No gripper command backend is configured",
                  {{"error_class", kErrorUnsupportedCapability}}};
}

}  // namespace

GripperCommandResultDTO ExecutionKernel::command_gripper(const GripperCommandRequestDTO&) {
  return GripperCommandResultDTO{false, 0, 0, gripper_not_configured_error()};
}

Result<void> ExecutionKernel::validate_gripper_command(const GripperCommandRequestDTO&) const {
  return Result<void>::Err(gripper_not_configured_error());
}

GripperCommandStatusDTO ExecutionKernel::get_gripper_status(const std::string& gripper_id) const {
  GripperCommandStatusDTO status;
  status.state = GripperCommandState::kFailed;
  status.gripper_id = gripper_id;
  status.error = gripper_not_configured_error();
  return status;
}

Result<std::uint64_t> ExecutionKernel::stop_gripper(const std::string&, std::uint64_t,
                                                    std::uint64_t) {
  return Result<std::uint64_t>::Err(gripper_not_configured_error());
}

Result<void> ExecutionKernel::reset_gripper(const std::string&, std::uint64_t, std::uint64_t) {
  return Result<void>::Err(gripper_not_configured_error());
}

}  // namespace manyforge::core
