#pragma once

#include <manyforge_core/types/gripper.hpp>
#include <string>
#include <memory>

namespace manyforge::core {

class RuntimeParticipantBinding;

/// Backend-neutral gripper command authority.
///
/// Implementations own command progression, deadlines, cancellation, and
/// generation fencing. Observer and orchestration layers may admit commands
/// and read immutable status, but may not tick this port to make progress.
class IGripperCommandBackend {
 public:
  virtual ~IGripperCommandBackend() = default;

  /// Exact process-local sessions retained for unresolved old gripper goals.
  virtual std::vector<std::shared_ptr<GripperPreparedSessionPin>>
  tracked_gripper_prepared_sessions() const { return {}; }

  /// Side-effect-free private-wire admission check. Backends with retained
  /// hardware/model limits override this so the addressed owner can reject an
  /// impossible command before journaling an accepted effect.
  virtual Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const {
    return Result<void>::Ok();
  }

  /// Strict selected-target configuration; command coordinate tags are not actuation.
  virtual GripperActuationKind gripper_actuation_kind(const std::string&) const {
    return GripperActuationKind::kPositionCommanded;
  }
  /// Existing engines retain commanded output; only strict authored targets
  /// may expose the explicit pneumatic none choice.
  virtual GripperOutputMode gripper_output_mode(const std::string&) const {
    return GripperOutputMode::kCommanded;
  }

  virtual GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) = 0;

  virtual GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const = 0;

  /// Exact old command whose failed owner has been joined and replaced. The
  /// backend retains the owner identities; no ROS identity crosses this port.
  virtual bool rebuilt_unknown_pending(const std::string&, std::uint64_t, std::uint64_t) const {
    return false;
  }
  /// Pins the exact current replacement owner that may supply recovery proof.
  virtual std::shared_ptr<const void> rebuilt_unknown_owner(
      const std::string&, std::uint64_t, std::uint64_t) const { return {}; }
  /// Retire only the exact previously reported command without asserting a
  /// successful outcome or sending a command. Caller supplies qualified proof.
  virtual Result<void> retire_rebuilt_unknown(const std::string&, std::uint64_t, std::uint64_t,
                                               const std::shared_ptr<const void>&) {
    return Result<void>::Err(ErrorDTO{"recovery_unavailable", "No rebuilt unknown gripper command", {}});
  }

  /// Internal source-adapter boundary. The native host resolves a currently
  /// qualified confirmation binding and its explicit freshness policy before
  /// calling this method. Public callers cannot supply that binding/policy.
  /// Implementations recheck the current target/route and command atomically.
  virtual Result<GripperGraspEvidenceDTO> record_grasp_observation(
      const RuntimeParticipantBinding&, const GripperGraspObservationDTO&, Duration,
      TimePoint /* admission_deadline_ns */) {
    return Result<GripperGraspEvidenceDTO>::Err(
        ErrorDTO{"grasp_confirmation_unavailable",
                 "This gripper backend has no qualified confirmation source",
                 {}});
  }

  /// Retire confirmation when its source becomes unknown. This affects
  /// evidence only: it must not cancel motion or change command admission.
  virtual void invalidate_grasp_evidence() noexcept {}

  virtual GripperCommandResultDTO mark_gripper_grasp_lost(const std::string& gripper_id,
                                                          std::uint64_t expected_command_id,
                                                          std::uint64_t expected_generation,
                                                          const std::string& evidence_label,
                                                          const std::string& message,
                                                          const std::string& reason_code) = 0;

  /// Invalidate every admitted generation before reset, replacement, or
  /// teardown can retire its resources. Late callbacks must become no-ops.
  /// This also closes command admission until `authorize_gripper_commands()`
  /// establishes a new lifecycle boundary.
  virtual void invalidate_gripper_commands(GripperInvalidationReason reason) noexcept = 0;

  /// Open command admission after a successful reset or new execute
  /// admission. Implementations must invalidate any preflight work captured
  /// before the preceding close/open boundary.
  virtual void authorize_gripper_commands() noexcept = 0;

  /// Atomically open command admission, or report why the preceding
  /// invalidation has not settled enough to reopen it. Legacy backends whose
  /// authorization is unconditional retain their existing behavior.
  virtual Result<void> try_authorize_gripper_commands() noexcept {
    authorize_gripper_commands();
    return Result<void>::Ok();
  }
};

}  // namespace manyforge::core
