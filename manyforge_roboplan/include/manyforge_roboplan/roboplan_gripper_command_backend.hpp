#pragma once

#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>

#include <chrono>
#include <memory>

namespace manyforge::roboplan_backend {

/// Backend-owned native gripper lifecycle.
///
/// Commands progress on the shared ControllerCommand scheduler lane. Status
/// reads only copy the latest immutable snapshot; they never advance time or
/// mutate the RoboPlan scene.
class RoboPlanGripperCommandBackend final
    : public core::IGripperCommandBackend {
 public:
  // Opaque callback state. Public only so translation-unit helpers can name
  // the incomplete type; it is not part of the behavioral API.
  struct SharedState;

  RoboPlanGripperCommandBackend(
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      core::IClock& clock,
      core::IScheduler& scheduler,
      core::Duration update_period_ns = 10'000'000);
  ~RoboPlanGripperCommandBackend() override;

  core::GripperCommandResultDTO command_gripper(
      const core::GripperCommandRequestDTO& request) override;
  core::GripperCommandStatusDTO get_gripper_status(
      const std::string& gripper_id) const override;
  core::GripperCommandResultDTO mark_gripper_grasp_lost(
      const std::string& gripper_id,
      std::uint64_t expected_command_id,
      std::uint64_t expected_generation,
      const std::string& evidence_label,
      const std::string& message,
      const std::string& reason_code) override;
  void invalidate_gripper_commands(
      core::GripperInvalidationReason reason) noexcept override;
  void authorize_gripper_commands() noexcept override;

 private:
  std::shared_ptr<SharedState> state_;
};

}  // namespace manyforge::roboplan_backend
