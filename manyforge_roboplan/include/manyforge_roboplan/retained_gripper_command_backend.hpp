#pragma once

#include <chrono>
#include <memory>

#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

namespace manyforge::roboplan_backend {

/// Native scheduler-owned gripper lifecycle for one exact retained target.
///
/// The public addressed Kernel resolves the target before reaching this
/// adapter. This class validates the resulting private wire fields against its
/// sealed binding, owns progression/deadlines/generation fencing, and writes
/// unit-tagged state through IRetainedObservedStateChannel.
class RetainedGripperCommandBackend final
    : public core::IGripperCommandBackend {
 public:
  struct SharedState;

  RetainedGripperCommandBackend(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> state_channel,
      core::IClock& clock, core::IScheduler& scheduler,
      core::Duration update_period_ns = 10'000'000);
  ~RetainedGripperCommandBackend() override;

  RetainedGripperCommandBackend(const RetainedGripperCommandBackend&) = delete;
  RetainedGripperCommandBackend& operator=(
      const RetainedGripperCommandBackend&) = delete;

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
