#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/io_condition.hpp>
#include <memory>

namespace manyforge::core {

/// Existing applied Program/Runner occurrence and Reset journal counter.
/// Run/claim provenance is absent between Runs; it is not source lifetime.
struct IoProgramBlackboardScopeDTO final {
  RequestOwnerEpoch request_owner_epoch;
  RequestOwnerEpoch program_runner_owner_epoch;
  std::uint64_t cycle_generation;
  std::optional<CellAdmissionClaimGeneration> claim_generation;
  std::optional<ProgramRunId> program_run_id;
  ProgramBindingId program_binding_id;
  /// Existing actual Applied publication occurrence; zero is unqualified.
  ActiveBundlePublicationGeneration active_publication_generation{0U};
  friend bool operator==(const IoProgramBlackboardScopeDTO&,
                         const IoProgramBlackboardScopeDTO&) = default;
};

struct IoBlackboardKeyDescriptorDTO final {
  std::string name;
  IoConditionValueKind value_kind;
  friend bool operator==(const IoBlackboardKeyDescriptorDTO&,
                         const IoBlackboardKeyDescriptorDTO&) = default;
};

/// Trusted host installation from the actual compiled condition-key union,
/// with scalar types resolved against sealed Program declarations. Source IDs
/// and incarnation are server-issued; public frames cannot install/replace them.
/// Validity follows the actual applied binding/Runner occurrence and required
/// health. Reset/new-Run replaces the epoch; held/halted/finished retain values.
/// No sample-age, renewal or caller-provided ProviderValidity capability.
struct IoProgramBlackboardSourceBindingDTO final {
  IoProgramBlackboardScopeDTO scope;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::vector<IoBlackboardKeyDescriptorDTO> keys;
  /// Internal capacity derived from the established host packet/compiled bound,
  /// not a caller knob/default. Limits aggregate retained key/scalar bytes.
  std::size_t maximum_value_bytes;
  friend bool operator==(const IoProgramBlackboardSourceBindingDTO&,
                         const IoProgramBlackboardSourceBindingDTO&) = default;
};

struct IoBlackboardValueDTO final {
  std::string name;
  IoConditionValueKind value_kind;
  /// Null is an honest unavailable key, not false or a publication failure.
  std::optional<IoConditionValueDTO> value;
  friend bool operator==(const IoBlackboardValueDTO&, const IoBlackboardValueDTO&) = default;
};

/// Immutable full typed compiled-key snapshot after one atomically accepted
/// publication. Existing source state and rule evidence share this one value;
/// no per-rule cache copy or independent source lifetime is introduced.
struct IoProgramBlackboardSnapshotDTO final {
  IoProgramBlackboardScopeDTO scope;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t message_sequence;
  TimePoint trusted_received_at_ns;
  std::vector<IoBlackboardValueDTO> values;
  friend bool operator==(const IoProgramBlackboardSnapshotDTO&,
                         const IoProgramBlackboardSnapshotDTO&) = default;
};

inline bool io_blackboard_snapshots_equal(
    const std::shared_ptr<const IoProgramBlackboardSnapshotDTO>& left,
    const std::shared_ptr<const IoProgramBlackboardSnapshotDTO>& right) {
  return left == right || (left && right && *left == *right);
}

/// Compact factual projection of only the checked condition's used keys.
/// Producer pointers alias the one acquired source snapshot; a decoder owns
/// exactly its received entries and never reconstructs absent full-cache data.
/// Values are known, non-null and canonical by unique name at the boundary.
struct IoBlackboardTripEvidenceDTO final {
  IoProgramBlackboardScopeDTO scope;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t message_sequence;
  TimePoint trusted_received_at_ns;
  std::vector<std::shared_ptr<const IoBlackboardValueDTO>> values;
  friend bool operator==(const IoBlackboardTripEvidenceDTO& left,
                         const IoBlackboardTripEvidenceDTO& right) {
    if (left.scope != right.scope || left.source_id != right.source_id ||
        left.incarnation != right.incarnation || left.message_sequence != right.message_sequence ||
        left.trusted_received_at_ns != right.trusted_received_at_ns ||
        left.values.size() != right.values.size())
      return false;
    for (std::size_t index = 0; index < left.values.size(); ++index)
      if (left.values[index] != right.values[index] &&
          (!left.values[index] || !right.values[index] ||
           *left.values[index] != *right.values[index]))
        return false;
    return true;
  }
};

/// Actual unavailable/retired source identity and last accepted frame. No
/// missing key value or synthetic signal trip is manufactured from source loss.
struct IoBlackboardSourceLossEvidenceDTO final {
  IoProgramBlackboardScopeDTO scope;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t last_accepted_message_sequence;
  std::optional<std::string> unavailable_key;
  friend bool operator==(const IoBlackboardSourceLossEvidenceDTO&,
                         const IoBlackboardSourceLossEvidenceDTO&) = default;
};

enum class IoBlackboardPublicationKind { kBaseline, kUpdate };

/// One serialized sender. Baseline supplies the exact installed typed key set;
/// update supplies only changed subscribed keys. Every actual frame advances
/// the same sequence. ACK follows synchronous whole-frame Core admission.
/// trusted_received_at_ns is captured by the in-process host receiver and is
/// evidence only, never an externally asserted freshness/health entitlement.
struct IoProgramBlackboardPublicationDTO final {
  IoProgramBlackboardScopeDTO scope;
  IoBlackboardPublicationKind kind;
  std::uint64_t message_sequence;
  std::uint64_t previous_sequence;
  std::vector<IoBlackboardValueDTO> values;
  TimePoint trusted_received_at_ns;
};

enum class IoProgramBlackboardAcknowledgementStatus { kAccepted, kSuperseded };

/// Returned from exact Kernel/source admission, never synthesized from the
/// caller's packet. Unknown replies create no authority to retry or advance.
struct IoProgramBlackboardAcknowledgementDTO final {
  RequestOwnerEpoch owner_epoch;
  ProgramBindingId program_binding_id;
  std::uint64_t cycle_generation;
  std::optional<ProgramRunId> program_run_id;
  IoSourceIncarnation source_epoch;
  /// Present only for actual accepted admission; superseded has no fake ACK.
  std::optional<std::uint64_t> accepted_sequence;
  ActiveBundlePublicationGeneration active_publication_generation{0U};
  IoProgramBlackboardAcknowledgementStatus status{
      IoProgramBlackboardAcknowledgementStatus::kAccepted};
};

}  // namespace manyforge::core
