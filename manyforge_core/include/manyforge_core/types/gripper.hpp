#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/capability_command_receipt.hpp>
#include <manyforge_core/types/addressed_command_identity.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

/// Authored target actuation, separate from angular/linear command coordinates.
enum class GripperActuationKind {
  kPositionCommanded,
  kPneumatic,
};

/// Strict authored output choice. Absence is not a no-output command.
enum class GripperOutputMode {
  kCommanded,
  kNone,
};

enum class GripperCommandState {
  kIdle,
  kExecuting,
  kCompleted,
  kFailed,
};

inline constexpr const char* gripper_command_state_name(GripperCommandState state) noexcept {
  switch (state) {
    case GripperCommandState::kIdle:
      return "idle";
    case GripperCommandState::kExecuting:
      return "executing";
    case GripperCommandState::kCompleted:
      return "completed";
    case GripperCommandState::kFailed:
      return "failed";
  }
  return "failed";
}

enum class GripperProjectionState {
  kNotRequired,
  kPending,
  kApplied,
  kFailed,
};

inline constexpr const char* gripper_projection_state_name(GripperProjectionState state) noexcept {
  switch (state) {
    case GripperProjectionState::kNotRequired:
      return "not_required";
    case GripperProjectionState::kPending:
      return "pending";
    case GripperProjectionState::kApplied:
      return "applied";
    case GripperProjectionState::kFailed:
      return "failed";
  }
  return "failed";
}

enum class GripperInvalidationReason {
  kReset,
  kStop,
  kReplacement,
  kDeploymentSwap,
  kShutdown,
};

/// Exact addressed-operation identity stamped only when a prepared command is
/// released. Direct/manual gripper commands leave this absent. A Stop adapter
/// uses it to reject a stale goal before cancelling a newer command.
using AddressedGripperCommandIdentityDTO = AddressedCommandIdentityDTO;

/// Process-local pin to the exact prepared session; never serialized.
struct GripperPreparedSessionPin {
  virtual ~GripperPreparedSessionPin() = default;
};

struct GripperCommandRequestDTO {
  std::string gripper_id;
  std::string group_id;
  std::string joint_name;
  double target_position = 0.0;
  double duration_s = 0.0;
  /// Complete only when the controller reports done and duration_s has
  /// elapsed since the command started (else on the controller's report).
  bool wait_for_duration = false;
  std::optional<double> max_effort;

  std::string commanded_state;
  std::string actuation_kind;
  std::string state_authority;
  std::string completion_policy;
  std::string grasp_confirmation_policy;
  std::string transport_kind;
  std::vector<std::string> evidence;
  std::string evidence_state;
  /// Owner-resolved terminal deadline for this private command. The native
  /// fixed-duration mechanism rejects a duration that cannot fit this bound.
  double completion_timeout_s = 1.0;
  std::optional<AddressedGripperCommandIdentityDTO> addressed_identity = std::nullopt;
  /// PRIVATE process-local proof of the exact resident prepared session.
  /// The backend compares one atomic load with the captured value at leaf
  /// admission; it is not serialized or retained by a provider.
  std::shared_ptr<const std::atomic<std::uint64_t>> admission_token{};
  std::uint64_t admission_token_expected{};
  std::shared_ptr<GripperLateCancelSlot> late_cancel_slot{};
  std::shared_ptr<GripperPreparedSessionPin> prepared_session_pin{};
  /// Temporary pre-leaf validation while producer/consumer migration lands.
  std::function<bool(const AddressedGripperCommandIdentityDTO&)> committed_owner_is_current;
  std::optional<std::uint64_t> owner_lifecycle_generation;
};

using GripperCommandResultDTO = CapabilityCommandDispatchReceiptDTO;

enum class GraspConfirmationSourceKind {
  kDiscreteInput = 1,
  kPerception = 2,
  kForceCurrent = 3,
};

/// A source adapter's observation, not a Program's assertion that a command
/// succeeded. observed_at_ns is in the receiving owner's clock domain.
struct GripperGraspObservationDTO final {
  std::string gripper_id;
  std::uint64_t command_id{};
  std::uint64_t generation{};
  std::uint64_t expected_status_revision{};
  std::string observation_id;
  bool confirmed{};
  TimePoint observed_at_ns{};
  std::string attribution;

  friend bool operator==(const GripperGraspObservationDTO&,
                         const GripperGraspObservationDTO&) = default;
};

/// One bounded, immutable evidence record for the current gripper command.
/// Its source/time are independent of commanded position and joint samples.
struct GripperGraspEvidenceDTO final {
  // Zero is deliberately not a source kind. Empty evidence is never a
  // discrete-input observation; the actual source adapter supplies its kind.
  GraspConfirmationSourceKind source_kind{};
  ControlBindingId source_id;
  ControlBindingRevision source_revision;
  RuntimeEpoch source_epoch;
  GripperGraspObservationDTO observation;
  TimePoint expires_at_ns{};

  friend bool operator==(const GripperGraspEvidenceDTO&, const GripperGraspEvidenceDTO&) = default;
};

[[nodiscard]] inline bool gripper_grasp_evidence_is_fresh(const GripperGraspEvidenceDTO& evidence,
                                                          TimePoint now_ns) noexcept {
  const bool source_kind_valid =
      evidence.source_kind == GraspConfirmationSourceKind::kDiscreteInput ||
      evidence.source_kind == GraspConfirmationSourceKind::kPerception ||
      evidence.source_kind == GraspConfirmationSourceKind::kForceCurrent;
  return source_kind_valid && now_ns >= evidence.observation.observed_at_ns &&
         now_ns < evidence.expires_at_ns;
}

/// Immutable observer snapshot published by the owning gripper adapter.
/// Reading this DTO must never advance a deadline, mutate command state, or
/// project a joint into a planner scene.
struct GripperCommandStatusDTO {
  GripperCommandState state = GripperCommandState::kIdle;
  std::string gripper_id;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  double progress = 0.0;
  std::optional<double> current_position;
  std::optional<double> target_position;
  std::optional<double> commanded_position;
  std::optional<double> observed_position;
  std::string commanded_state;
  std::string actuation_kind;
  std::string state_authority;
  std::string completion_policy;
  std::string grasp_confirmation_policy;
  std::string transport_kind;
  std::vector<std::string> evidence;
  std::string evidence_state;
  GripperProjectionState projection_state = GripperProjectionState::kNotRequired;
  std::string projection_reason;
  std::optional<ErrorDTO> error;
  std::optional<GripperGraspEvidenceDTO> grasp_evidence;
  /// Local admission fence, independent of an admitted transaction generation.
  std::uint64_t admission_generation{};
  bool admission_closed{};
  /// Destructive lifecycle generation; pneumatic Stop's admission-only fence
  /// does not retire it. Reset/replacement/shutdown still do.
  std::uint64_t lifecycle_generation{};
  /// A Stop/Reset cancel was sent and the controller has not yet reported
  /// that goal's terminal (the Stop does not wait for it).
  bool cancellation_pending{};
};

}  // namespace manyforge::core
