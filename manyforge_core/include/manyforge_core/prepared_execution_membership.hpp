#pragma once

#include <algorithm>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <optional>
#include <string_view>
#include <type_traits>

namespace manyforge::core {

/// One backend's current preparation, ordered by its owning Kernel's existing
/// execution ID. Caller holds the backend's preparation mutex. This is not a
/// request history: retirement keeps only the high-watermark, and cannot revive
/// a retired ID. A backend and its execution-ID owner share a lifetime.
class PreparedExecutionMembership final {
 public:
  [[nodiscard]] std::optional<std::string_view> prepare(
      std::uint64_t execution_id, const PreparedOperationParticipantDTO& participant) {
    if (pending_) return "prepared_replacement_in_flight";
    if (execution_id == 0U ||
        (execution_id < highest_execution_id_ &&
         (!current_ || current_->execution_id != execution_id)) ||
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
    current_ = Entry{execution_id, participant.operation_id, participant.participant_id,
                     participant.operation_revision, participant.prepared_contract_digest};
    highest_execution_id_ = std::max(highest_execution_id_, execution_id);
    return std::nullopt;
  }

  [[nodiscard]] bool matches(const PreparedOperationParticipantDTO& participant) const noexcept {
    return current_ && current_->operation_id == participant.operation_id &&
           current_->participant_id == participant.participant_id &&
           current_->revision == participant.operation_revision &&
           current_->digest == participant.prepared_contract_digest;
  }

  /// One replacement of the exact resident preparation. The high-watermark
  /// advances without changing the physical current membership.
  [[nodiscard]] std::optional<std::string_view> prepare_replacement(
      std::uint64_t source_id, std::uint64_t candidate_id,
      const PreparedOperationParticipantDTO& participant) {
    if (!current_ || current_->execution_id != source_id || candidate_id <= source_id ||
        candidate_id < highest_execution_id_ ||
        (candidate_id == highest_execution_id_ && !pending_))
      return "prepared_replacement_stale";
    if (pending_ &&
        (pending_->execution_id != candidate_id || !entry_matches(*pending_, participant)))
      return "prepared_replacement_conflict";
    pending_ = Entry{candidate_id, participant.operation_id, participant.participant_id,
                     participant.operation_revision, participant.prepared_contract_digest};
    highest_execution_id_ = candidate_id;
    return std::nullopt;
  }

  [[nodiscard]] bool replacement_matches(
      std::uint64_t candidate_id,
      const PreparedOperationParticipantDTO& participant) const noexcept {
    return pending_ && pending_->execution_id == candidate_id &&
           entry_matches(*pending_, participant);
  }

  [[nodiscard]] bool can_adopt(std::uint64_t source_id, std::uint64_t candidate_id) const noexcept {
    return current_ && current_->execution_id == source_id && pending_ &&
           pending_->execution_id == candidate_id;
  }

  /// Called only after can_adopt and the external ownership CAS under the
  /// same participant mutex. All storage was allocated by preparation.
  void adopt_replacement() noexcept {
    static_assert(std::is_nothrow_swappable_v<Entry>);
    current_.swap(pending_);
    pending_.reset();
  }

  /// The owner must first prove dispatch/Stop/recovery quiescence. An already
  /// retired older ID is an idempotent no-op, never permission to clear a newer
  /// execution. The owner may also close an unprepared/cancelled ID when no
  /// preparation is resident; a delayed Prepare for that ID then rejects.
  [[nodiscard]] bool retire(std::uint64_t execution_id) noexcept {
    if (execution_id == 0U) return false;
    if (pending_ && pending_->execution_id == execution_id) {
      pending_.reset();
      return true;
    }
    if (current_ && current_->execution_id == execution_id && pending_) return false;
    if (execution_id > highest_execution_id_) {
      if (current_) return false;
      highest_execution_id_ = execution_id;
    }
    if (current_ && execution_id == current_->execution_id) current_.reset();
    return true;
  }

  [[nodiscard]] std::optional<std::uint64_t> active_execution_id() const noexcept {
    return current_ ? std::optional(current_->execution_id) : std::nullopt;
  }

 private:
  struct Entry {
    std::uint64_t execution_id;
    OperationId operation_id;
    OperationParticipantId participant_id;
    OperationRevision revision;
    ContentDigest digest;
  };
  [[nodiscard]] static bool entry_matches(
      const Entry& entry, const PreparedOperationParticipantDTO& participant) noexcept {
    return entry.operation_id == participant.operation_id &&
           entry.participant_id == participant.participant_id &&
           entry.revision == participant.operation_revision &&
           entry.digest == participant.prepared_contract_digest;
  }
  std::uint64_t highest_execution_id_{};
  std::optional<Entry> current_;
  std::optional<Entry> pending_;
};

}  // namespace manyforge::core
