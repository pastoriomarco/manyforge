#pragma once

#include <manyforge_core/types/gripper.hpp>

#include <string>

namespace manyforge::core {

/// Backend-neutral gripper command authority.
///
/// Implementations own command progression, deadlines, cancellation, and
/// generation fencing. Observer and orchestration layers may admit commands
/// and read immutable status, but may not tick this port to make progress.
class IGripperCommandBackend {
 public:
  virtual ~IGripperCommandBackend() = default;

  virtual GripperCommandResultDTO command_gripper(
      const GripperCommandRequestDTO& request) = 0;

  virtual GripperCommandStatusDTO get_gripper_status(
      const std::string& gripper_id) const = 0;

  virtual GripperCommandResultDTO mark_gripper_grasp_lost(
      const std::string& gripper_id,
      std::uint64_t expected_command_id,
      std::uint64_t expected_generation,
      const std::string& evidence_label,
      const std::string& message,
      const std::string& reason_code) = 0;

  /// Invalidate every admitted generation before reset, replacement, or
  /// teardown can retire its resources. Late callbacks must become no-ops.
  /// This also closes command admission until `authorize_gripper_commands()`
  /// establishes a new lifecycle boundary.
  virtual void invalidate_gripper_commands(
      GripperInvalidationReason reason) noexcept = 0;

  /// Open command admission after a successful reset, resume, or new execute
  /// admission. Implementations must invalidate any preflight work captured
  /// before the preceding close/open boundary.
  virtual void authorize_gripper_commands() noexcept = 0;
};

}  // namespace manyforge::core
