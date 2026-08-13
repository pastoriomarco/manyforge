#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/istop_control_executor.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <mutex>
#include <optional>
#include <string_view>

namespace manyforge::core {

/// Hard implementation bounds for the Kernel-owned emergency Stop reserve.
/// The configured correlation capacity may be smaller, but never larger. The
/// separate Full-Stop slot is not charged to this bound.
inline constexpr std::size_t kMaximumEmergencyStopCorrelationEntries = 64U;
inline constexpr std::size_t kMaximumEmergencyStopRequestIdBytes = 128U;
inline constexpr std::size_t kMaximumEmergencyStopIdentityBytes = 128U;
inline constexpr std::size_t kMaximumEmergencyStopDigestBytes = 71U;
inline constexpr std::size_t kMaximumEmergencyStopPrincipalBytes = 256U;
inline constexpr std::size_t kMaximumEmergencyStopMalformedReasonBytes = 256U;

enum class EmergencyStopAcceptancePathDTO {
  kFreshReserved,
  kJoinedResident,
  kCoalescedFull,
};

enum class EmergencyStopProjectionStateDTO {
  kPending,
  kRecorded,
  kFailed,
};

enum class EmergencyStopReserveError {
  kNone,
  kInvalidCandidate,
};

/// Allocation-free view prepared by the Stop ingress/resolver before it enters
/// the reserve-or-join critical section. The proposed Stop identity is used
/// only for a fresh transaction; exact replay and overlap joins retain the
/// already-resident identity and deadline.
struct EmergencyStopAcceptanceCandidate final {
  std::string_view request_id;
  std::optional<std::string_view> normalized_request_digest;
  RequestTransportDTO transport{RequestTransportDTO::kInternal};
  RequestAssuranceDTO assurance{RequestAssuranceDTO::kUnverified};
  std::optional<std::string_view> principal_id;
  std::optional<std::string_view> malformed_reason;
  std::string_view proposed_stop_id;
  StopGeneration proposed_stop_generation{0U};
  std::int64_t accepted_at_ns{};
  std::int64_t outer_deadline_ns{};
  /// Earliest complete-Full deadline sampled from the new proposal and every
  /// resident predecessor. Used only if this candidate is promoted/coalesced
  /// to the dedicated Full slot.
  std::int64_t full_stop_outer_deadline_ns{};
  StopResolutionDispositionDTO disposition{StopResolutionDispositionDTO::kPromotedToFull};
  std::string_view captured_scope_digest;
  /// Exact current Full-Stop capture. It is prepared for every recognized
  /// request because correlation exhaustion/conflict may conservatively widen
  /// an otherwise exact selector inside the reserve transition.
  std::string_view full_stop_scope_digest;
  std::optional<std::string_view> overlapping_stop_id;
  bool full_stop{};
};

/// Fixed-size return value from reserve_or_join(). It deliberately owns its
/// copied strings so callers never retain a view into a slot that may later be
/// released after terminal handoff.
struct EmergencyStopAcceptanceOutcome final {
  EmergencyStopReserveError error{EmergencyStopReserveError::kNone};
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
  EmergencyStopAcceptancePathDTO acceptance_path{EmergencyStopAcceptancePathDTO::kFreshReserved};
  StopResolutionDispositionDTO disposition{StopResolutionDispositionDTO::kPromotedToFull};
  StopGeneration stop_generation{0U};
  std::int64_t accepted_at_ns{};
  std::int64_t outer_deadline_ns{};
  /// Physical Stop transaction slot/generation handed to StopControl. Joined
  /// request correlations retain the already-resident transaction token.
  StopControlWorkToken work_token;
  /// Exact request-correlation record slot. The dedicated Full-Stop slot is
  /// represented by correlation_capacity(); it may be returned with
  /// correlation_retained=false for an unretained coalesced request.
  StopControlSlotIndex correlation_slot{kInvalidStopControlSlot};
  bool correlation_retained{};
  bool full_stop{};

  [[nodiscard]] bool accepted() const noexcept { return error == EmergencyStopReserveError::kNone; }
  [[nodiscard]] std::string_view stop_id() const noexcept {
    return {stop_id_bytes_.data(), stop_id_size_};
  }
  [[nodiscard]] std::string_view captured_scope_digest() const noexcept {
    return {scope_digest_bytes_.data(), scope_digest_size_};
  }

 private:
  std::array<char, kMaximumEmergencyStopIdentityBytes> stop_id_bytes_{};
  std::size_t stop_id_size_{};
  std::array<char, kMaximumEmergencyStopDigestBytes> scope_digest_bytes_{};
  std::size_t scope_digest_size_{};

  friend class EmergencyStopAcceptanceReserve;
};

/// Kernel-owned fixed emergency acceptance storage. Construction allocates no
/// per-request state. reserve_or_join(), projection updates, and terminal
/// release perform only bounded array/string copies while holding one short
/// internal state lock; they never wait for storage, logging, or a free slot.
///
/// Scope resolution and the dispatch/Stop ordering fence remain Kernel work.
/// The optional overlapping_stop_id is therefore an exact assertion sampled
/// under that same outer ordering boundary, not a request-side hint.
class EmergencyStopAcceptanceReserve final {
 public:
  explicit EmergencyStopAcceptanceReserve(std::size_t correlation_capacity);

  EmergencyStopAcceptanceReserve(const EmergencyStopAcceptanceReserve&) = delete;
  EmergencyStopAcceptanceReserve& operator=(const EmergencyStopAcceptanceReserve&) = delete;

  [[nodiscard]] EmergencyStopAcceptanceOutcome reserve_or_join(
      const EmergencyStopAcceptanceCandidate& candidate) noexcept;

  /// Update the forensic projection state for every resident correlation that
  /// joined the Stop. This does not release or otherwise change containment.
  [[nodiscard]] bool mark_projection(std::string_view stop_id,
                                     EmergencyStopProjectionStateDTO projection_state) noexcept;

  /// Returns the resident acceptance for a retained request ID, if any. This
  /// bounded projection never renews or mutates the slot.
  [[nodiscard]] std::optional<EmergencyStopAcceptanceOutcome> find(
      std::string_view request_id) const noexcept;

  /// Zero every resident record for this Stop only after the Kernel has
  /// committed its immutable terminal certificate/fault/surveillance handoff.
  /// Projection failure deliberately does not prevent reuse.
  [[nodiscard]] std::size_t release_after_terminal_handoff(std::string_view stop_id) noexcept;

  [[nodiscard]] std::size_t correlation_capacity() const noexcept { return correlation_capacity_; }
  [[nodiscard]] std::size_t active_correlation_count() const noexcept;
  [[nodiscard]] bool full_stop_active() const noexcept;

 private:
  struct Slot final {
    bool active{};
    bool full_stop{};
    bool normalized_request_digest_present{};
    bool principal_present{};
    bool malformed_reason_present{};
    RequestTransportDTO transport{RequestTransportDTO::kInternal};
    RequestAssuranceDTO assurance{RequestAssuranceDTO::kUnverified};
    StopResolutionDispositionDTO disposition{StopResolutionDispositionDTO::kPromotedToFull};
    EmergencyStopAcceptancePathDTO acceptance_path{EmergencyStopAcceptancePathDTO::kFreshReserved};
    EmergencyStopProjectionStateDTO projection_state{EmergencyStopProjectionStateDTO::kPending};
    StopGeneration stop_generation{0U};
    std::int64_t accepted_at_ns{};
    std::int64_t outer_deadline_ns{};
    StopControlWorkToken work_token;
    std::array<char, kMaximumEmergencyStopRequestIdBytes> request_id{};
    std::size_t request_id_size{};
    std::array<char, kMaximumEmergencyStopDigestBytes> request_digest{};
    std::size_t request_digest_size{};
    std::array<char, kMaximumEmergencyStopPrincipalBytes> principal{};
    std::size_t principal_size{};
    std::array<char, kMaximumEmergencyStopMalformedReasonBytes> malformed_reason{};
    std::size_t malformed_reason_size{};
    std::array<char, kMaximumEmergencyStopIdentityBytes> stop_id{};
    std::size_t stop_id_size{};
    std::array<char, kMaximumEmergencyStopDigestBytes> scope_digest{};
    std::size_t scope_digest_size{};
  };

  std::size_t correlation_capacity_{};
  mutable std::mutex mutex_;
  std::array<Slot, kMaximumEmergencyStopCorrelationEntries> slots_{};
  Slot full_stop_slot_{};
  /// Reuse generations deliberately survive Slot{} zeroing at terminal
  /// handoff, so a physical executor slot can never mistake new work for an
  /// already-delivered predecessor.
  std::array<std::uint64_t, kMaximumEmergencyStopCorrelationEntries> work_generations_{};
  std::uint64_t full_stop_work_generation_{};
};

}  // namespace manyforge::core
