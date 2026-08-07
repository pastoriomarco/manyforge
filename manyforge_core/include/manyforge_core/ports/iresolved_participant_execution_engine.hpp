#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/runtime_scale.hpp>

namespace manyforge::core {

/// Subordinate single-participant execution mechanism used by the addressed
/// Kernel owner. It does not resolve Program roles, select a robot/group, or
/// reserve resources; those responsibilities remain outside this port.
class IResolvedParticipantExecutionEngine {
 public:
  virtual ~IResolvedParticipantExecutionEngine() = default;

  virtual Result<std::uint64_t> execute(const ExecuteRequestDTO& request) = 0;
  virtual StopResultDTO stop() = 0;
  virtual PauseResultDTO pause() = 0;
  virtual ResumeResultDTO resume(
      std::optional<std::uint64_t> expected_epoch = std::nullopt) = 0;
  virtual ResetResultDTO reset(
      std::optional<std::uint64_t> expected_epoch = std::nullopt) = 0;
  virtual AcknowledgeCompletedResultDTO acknowledge_completed(
      std::uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt) = 0;

  /// Private single-participant realization for the addressed owner's exact,
  /// generation-fenced runtime-scale contract. The engine validates and
  /// echoes the owner-resolved binding; it never selects or mints identities.
  virtual Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) = 0;
  virtual Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id,
      const ControlBindingId& control_binding_id) const = 0;

  virtual GripperCommandResultDTO command_gripper(
      const GripperCommandRequestDTO& request) = 0;
  virtual GripperCommandStatusDTO get_gripper_status(
      const std::string& gripper_id) const = 0;

  virtual ExecutionStatus get_status() const = 0;
  virtual SubscriptionHandle subscribe_events(EventCallback callback) = 0;
  virtual void unsubscribe_events(SubscriptionHandle handle) = 0;
};

}  // namespace manyforge::core
