#pragma once

#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <string>

namespace manyforge::core {

/// Immutable sealed inputs a `PreparedMotion` was built from, retained for
/// transport (SX-032). A cross-process mechanism transmits these, never
/// derived `PreparedMotion` fields, and the receiver rebuilds the motion
/// through the same Core builder: `DecodePreparedOperationParticipant` with
/// its configured binding, readiness, retained limits and node category,
/// `DecodePreparedTrajectory`, then the builder (all inside
/// `BuildPreparedMotionSlot`, prepared_motion_builder.hpp).
///
/// Produced once at Prepare by `MakeSealedPreparedMotionInputs`
/// (prepared_operation_participant_codec.hpp) from the sealed participant;
/// never reconstructed from a `PreparedMotion`. NRT only: real-time code
/// never reads it.
struct SealedPreparedMotionInputs final {
  /// The participant's `prepared_contract_digest`; equals the built
  /// `PreparedMotion::prepared_contract_digest`.
  ContentDigest participant_digest{std::string{}};
  /// `EncodePreparedOperationParticipant` canonical bytes of exactly that
  /// sealed participant.
  std::string canonical_participant_bytes;
  /// `EncodePreparedTrajectory` canonical bytes of the participant's motion
  /// payload trajectory (the trajectory the motion samples).
  std::string canonical_trajectory_bytes;

  friend bool operator==(const SealedPreparedMotionInputs&,
                         const SealedPreparedMotionInputs&) = default;
};

/// One retained prepared slot: the motion the loop borrows and the sealed
/// inputs it was built from, owned together by one immutable object (the
/// participant execution's prepared session holds it as
/// `std::shared_ptr<const PreparedMotionSlot>`; a receiver owns its rebuilt
/// slot the same way). No second preparation registry exists.
///
/// Invariants: `sealed.participant_digest == motion.prepared_contract_digest`
/// and `motion` was built from `sealed` by the shared builder.
///
/// Lifetime: handed to `IExecutionMechanism::Submit/Replace` by `const&`;
/// the owner keeps it alive and unmodified until mechanism status reports the
/// key's slot unreferenced (`slot_referenced == false`). The mechanism may
/// keep the address for that long; the loop reads only `motion`.
struct PreparedMotionSlot final {
  SealedPreparedMotionInputs sealed;
  PreparedMotion motion;
};

}  // namespace manyforge::core
