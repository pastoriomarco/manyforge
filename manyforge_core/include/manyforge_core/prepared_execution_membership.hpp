#pragma once

#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <optional>
#include <string_view>

namespace manyforge::core {

/// One backend's current preparation, ordered by its owning Kernel's existing
/// execution ID. Caller holds the backend's preparation mutex. This is not a
/// request history: retirement keeps only the high-watermark, and cannot revive
/// a retired ID. A backend and its execution-ID owner share a lifetime.
class PreparedExecutionMembership final {
 public:
  [[nodiscard]] std::optional<std::string_view> prepare(
      std::uint64_t execution_id, const PreparedOperationParticipantDTO& participant) {
    if (execution_id == 0U || execution_id < highest_execution_id_ ||
        (execution_id == highest_execution_id_ && !current_)) {
      return "prepared_execution_stale";
    }
    if (execution_id > highest_execution_id_ && current_) {
      return "prepared_execution_not_retired";
    }
    if (current_) {
      if (current_->operation_id != participant.operation_id ||
          current_->participant_id != participant.participant_id ||
          participant.operation_revision.value() < current_->revision.value()) {
        return "prepared_execution_context_mismatch";
      }
      if (participant.operation_revision == current_->revision &&
          participant.prepared_contract_digest != current_->digest) {
        return "prepared_contract_conflict";
      }
    }
    current_ = Entry{participant.operation_id, participant.participant_id,
                     participant.operation_revision, participant.prepared_contract_digest};
    highest_execution_id_ = execution_id;
    return std::nullopt;
  }

  [[nodiscard]] bool matches(const PreparedOperationParticipantDTO& participant) const noexcept {
    return current_ && current_->operation_id == participant.operation_id &&
           current_->participant_id == participant.participant_id &&
           current_->revision == participant.operation_revision &&
           current_->digest == participant.prepared_contract_digest;
  }

  /// The owner must first prove dispatch/Stop/recovery quiescence. An already
  /// retired older ID is an idempotent no-op, never permission to clear a newer
  /// execution. The owner may also close an unprepared/cancelled ID when no
  /// preparation is resident; a delayed Prepare for that ID then rejects.
  [[nodiscard]] bool retire(std::uint64_t execution_id) noexcept {
    if (execution_id == 0U) return false;
    if (execution_id > highest_execution_id_) {
      if (current_) return false;
      highest_execution_id_ = execution_id;
    }
    if (execution_id == highest_execution_id_) current_.reset();
    return true;
  }

  [[nodiscard]] std::optional<std::uint64_t> active_execution_id() const noexcept {
    return current_ ? std::optional(highest_execution_id_) : std::nullopt;
  }

 private:
  struct Entry {
    OperationId operation_id;
    OperationParticipantId participant_id;
    OperationRevision revision;
    ContentDigest digest;
  };
  std::uint64_t highest_execution_id_{};
  std::optional<Entry> current_;
};

}  // namespace manyforge::core
