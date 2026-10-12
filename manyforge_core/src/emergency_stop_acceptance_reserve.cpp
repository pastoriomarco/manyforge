#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <manyforge_core/emergency_stop_acceptance_reserve.hpp>
#include <stdexcept>
#include <string_view>

namespace manyforge::core {
namespace {

template <std::size_t Size>
[[nodiscard]] std::string_view view(const std::array<char, Size>& bytes,
                                    std::size_t used) noexcept {
  return {bytes.data(), used};
}

template <std::size_t Size>
void assign(std::array<char, Size>& destination, std::size_t& used,
            std::string_view source) noexcept {
  std::copy(source.begin(), source.end(), destination.begin());
  used = source.size();
}

[[nodiscard]] bool optional_equal(bool present, std::string_view retained,
                                  std::optional<std::string_view> candidate) noexcept {
  return present == candidate.has_value() && (!present || retained == *candidate);
}

[[nodiscard]] bool known_disposition(StopResolutionDispositionDTO disposition) noexcept {
  switch (disposition) {
    case StopResolutionDispositionDTO::kExact:
    case StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved:
    case StopResolutionDispositionDTO::kAlreadyTerminalNoEffect:
    case StopResolutionDispositionDTO::kPromotedToFull:
      return true;
  }
  return false;
}

[[nodiscard]] bool known_projection_state(
    EmergencyStopProjectionStateDTO projection_state) noexcept {
  switch (projection_state) {
    case EmergencyStopProjectionStateDTO::kPending:
    case EmergencyStopProjectionStateDTO::kRecorded:
    case EmergencyStopProjectionStateDTO::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool bounded_optional(std::optional<std::string_view> value,
                                    std::size_t maximum) noexcept {
  return !value.has_value() || (!value->empty() && value->size() <= maximum &&
                                value->find('\0') == std::string_view::npos);
}

[[nodiscard]] bool bounded_text(std::string_view value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool portable_token(std::string_view value, std::size_t maximum) noexcept {
  return bounded_text(value, maximum) &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '.' ||
                  character == ':' || character == '-';
         });
}

[[nodiscard]] bool sha256_digest(std::string_view value) noexcept {
  constexpr std::string_view prefix = "sha256:";
  return value.size() == kMaximumEmergencyStopDigestBytes &&
         value.substr(0U, prefix.size()) == prefix &&
         std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()), value.end(),
                     [](char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                     });
}

[[nodiscard]] bool attribution_valid(const EmergencyStopAcceptanceCandidate& candidate) noexcept {
  switch (candidate.transport) {
    case RequestTransportDTO::kRos:
    case RequestTransportDTO::kHttp:
    case RequestTransportDTO::kInternal:
      break;
    default:
      return false;
  }
  switch (candidate.assurance) {
    case RequestAssuranceDTO::kUnverified:
      return !candidate.principal_id.has_value();
    case RequestAssuranceDTO::kExternallyAuthenticated:
      return true;
  }
  return false;
}

[[nodiscard]] bool candidate_valid(const EmergencyStopAcceptanceCandidate& candidate) noexcept {
  return portable_token(candidate.request_id, kMaximumEmergencyStopRequestIdBytes) &&
         (!candidate.normalized_request_digest.has_value() ||
          sha256_digest(*candidate.normalized_request_digest)) &&
         bounded_optional(candidate.principal_id, kMaximumEmergencyStopPrincipalBytes) &&
         bounded_optional(candidate.malformed_reason, kMaximumEmergencyStopMalformedReasonBytes) &&
         portable_token(candidate.proposed_stop_id, kMaximumEmergencyStopIdentityBytes) &&
         candidate.proposed_stop_generation.nonzero() && candidate.accepted_at_ns >= 0 &&
         known_disposition(candidate.disposition) &&
         sha256_digest(candidate.captured_scope_digest) &&
         sha256_digest(candidate.full_stop_scope_digest) &&
         (!candidate.overlapping_stop_id.has_value() ||
          portable_token(*candidate.overlapping_stop_id, kMaximumEmergencyStopIdentityBytes)) &&
         attribution_valid(candidate) &&
         (candidate.normalized_request_digest.has_value() ||
          candidate.malformed_reason.has_value()) &&
         (!candidate.malformed_reason.has_value() ||
          (!candidate.normalized_request_digest.has_value() && candidate.full_stop &&
           candidate.disposition == StopResolutionDispositionDTO::kPromotedToFull)) &&
         (candidate.disposition != StopResolutionDispositionDTO::kPromotedToFull ||
          candidate.full_stop) &&
         (!candidate.full_stop || candidate.disposition == StopResolutionDispositionDTO::kExact ||
          candidate.disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved ||
          candidate.disposition == StopResolutionDispositionDTO::kPromotedToFull);
}

}  // namespace

EmergencyStopAcceptanceReserve::EmergencyStopAcceptanceReserve(std::size_t correlation_capacity)
    : correlation_capacity_(correlation_capacity) {
  if (correlation_capacity == 0U ||
      correlation_capacity > kMaximumEmergencyStopCorrelationEntries) {
    throw std::invalid_argument("emergency Stop correlation capacity must be in [1, 64]");
  }
}

EmergencyStopAcceptanceOutcome EmergencyStopAcceptanceReserve::reserve_or_join(
    const EmergencyStopAcceptanceCandidate& candidate) noexcept {
  EmergencyStopAcceptanceOutcome outcome;
  if (!candidate_valid(candidate)) {
    outcome.error = EmergencyStopReserveError::kInvalidCandidate;
    return outcome;
  }

  std::lock_guard lock(mutex_);

  const auto exact_request = [&](const Slot& slot) {
    return view(slot.request_id, slot.request_id_size) == candidate.request_id &&
           optional_equal(slot.normalized_request_digest_present,
                          view(slot.request_digest, slot.request_digest_size),
                          candidate.normalized_request_digest) &&
           slot.transport == candidate.transport && slot.assurance == candidate.assurance &&
           optional_equal(slot.principal_present, view(slot.principal, slot.principal_size),
                          candidate.principal_id) &&
           optional_equal(slot.malformed_reason_present,
                          view(slot.malformed_reason, slot.malformed_reason_size),
                          candidate.malformed_reason);
  };
  const auto same_request_id = [&](const Slot& slot) {
    return slot.active && view(slot.request_id, slot.request_id_size) == candidate.request_id;
  };
  const auto publish = [&](const Slot& slot, StopControlSlotIndex correlation_slot,
                           RequestReplayStatusDTO replay, EmergencyStopAcceptancePathDTO path,
                           bool correlation_retained) {
    EmergencyStopAcceptanceOutcome result;
    result.replay_status = replay;
    result.acceptance_path = path;
    result.disposition = slot.disposition;
    result.stop_generation = slot.stop_generation;
    result.accepted_at_ns = slot.accepted_at_ns;
    result.work_token = slot.work_token;
    result.correlation_slot = correlation_slot;
    result.correlation_retained = correlation_retained;
    result.full_stop = slot.full_stop;
    assign(result.stop_id_bytes_, result.stop_id_size_, view(slot.stop_id, slot.stop_id_size));
    assign(result.scope_digest_bytes_, result.scope_digest_size_,
           view(slot.scope_digest, slot.scope_digest_size));
    return result;
  };
  const auto mint_work_token = [&](StopControlSlotIndex physical_slot) noexcept {
    auto& retained_generation = physical_slot == correlation_capacity_
                                    ? full_stop_work_generation_
                                    : work_generations_[physical_slot];
    const auto proposed_generation = candidate.proposed_stop_generation.value();
    if (proposed_generation > retained_generation) {
      retained_generation = proposed_generation;
    } else if (retained_generation < std::numeric_limits<std::uint64_t>::max()) {
      ++retained_generation;
    } else {
      // Exhausting a physical Stop work generation cannot be converted into a
      // dropped/replayed Stop. This is a process-lifetime invariant breach.
      std::terminate();
    }
    return StopControlWorkToken{physical_slot, retained_generation};
  };
  const auto fill = [&](Slot& slot, StopControlSlotIndex physical_slot, const Slot* joined,
                        bool full_stop, StopResolutionDispositionDTO disposition,
                        EmergencyStopAcceptancePathDTO path, std::string_view scope_digest) {
    slot = Slot{};
    slot.active = true;
    slot.full_stop = full_stop;
    slot.normalized_request_digest_present = candidate.normalized_request_digest.has_value();
    slot.principal_present = candidate.principal_id.has_value();
    slot.malformed_reason_present = candidate.malformed_reason.has_value();
    slot.transport = candidate.transport;
    slot.assurance = candidate.assurance;
    slot.disposition = disposition;
    slot.acceptance_path = path;
    slot.projection_state = EmergencyStopProjectionStateDTO::kPending;
    slot.stop_generation =
        joined != nullptr ? joined->stop_generation : candidate.proposed_stop_generation;
    slot.accepted_at_ns = joined != nullptr ? joined->accepted_at_ns : candidate.accepted_at_ns;
    slot.work_token = joined != nullptr ? joined->work_token : mint_work_token(physical_slot);
    assign(slot.request_id, slot.request_id_size, candidate.request_id);
    if (candidate.normalized_request_digest.has_value()) {
      assign(slot.request_digest, slot.request_digest_size, *candidate.normalized_request_digest);
    }
    if (candidate.principal_id.has_value()) {
      assign(slot.principal, slot.principal_size, *candidate.principal_id);
    }
    if (candidate.malformed_reason.has_value()) {
      assign(slot.malformed_reason, slot.malformed_reason_size, *candidate.malformed_reason);
    }
    assign(slot.stop_id, slot.stop_id_size,
           joined != nullptr ? view(joined->stop_id, joined->stop_id_size)
                             : candidate.proposed_stop_id);
    assign(slot.scope_digest, slot.scope_digest_size, scope_digest);
  };
  const auto coalesce_full = [&]() {
    const auto full_slot = static_cast<StopControlSlotIndex>(correlation_capacity_);
    const bool explicit_full = candidate.full_stop &&
        (candidate.disposition == StopResolutionDispositionDTO::kExact ||
         candidate.disposition == StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved);
    if (!full_stop_slot_.active) {
      fill(full_stop_slot_, full_slot, nullptr, true,
           explicit_full ? candidate.disposition
                         : StopResolutionDispositionDTO::kPromotedToFull,
           explicit_full ? EmergencyStopAcceptancePathDTO::kFreshReserved
                         : EmergencyStopAcceptancePathDTO::kCoalescedFull,
           candidate.full_stop_scope_digest);
      return publish(full_stop_slot_, full_slot, RequestReplayStatusDTO::kAcceptedNew,
                     explicit_full ? EmergencyStopAcceptancePathDTO::kFreshReserved
                                   : EmergencyStopAcceptancePathDTO::kCoalescedFull,
                     true);
    }
    auto coalesced = publish(full_stop_slot_, full_slot, RequestReplayStatusDTO::kAcceptedNew,
                             EmergencyStopAcceptancePathDTO::kCoalescedFull, false);
    coalesced.disposition =
        explicit_full ? candidate.disposition
            : StopResolutionDispositionDTO::kPromotedToFull;
    return coalesced;
  };

  bool request_id_conflict = false;
  for (std::size_t index = 0U; index < correlation_capacity_; ++index) {
    const auto& slot = slots_[index];
    if (!same_request_id(slot)) {
      continue;
    }
    if (exact_request(slot)) {
      return publish(slot, static_cast<StopControlSlotIndex>(index),
                     RequestReplayStatusDTO::kReplayedExact, slot.acceptance_path, true);
    }
    request_id_conflict = true;
  }
  if (same_request_id(full_stop_slot_)) {
    if (exact_request(full_stop_slot_)) {
      return publish(full_stop_slot_, static_cast<StopControlSlotIndex>(correlation_capacity_),
                     RequestReplayStatusDTO::kReplayedExact, full_stop_slot_.acceptance_path, true);
    }
    request_id_conflict = true;
  }
  if (request_id_conflict) {
    return coalesce_full();
  }

  if (candidate.full_stop) {
    return coalesce_full();
  }

  const Slot* joined = nullptr;
  if (candidate.overlapping_stop_id.has_value()) {
    if (full_stop_slot_.active && view(full_stop_slot_.stop_id, full_stop_slot_.stop_id_size) ==
                                      *candidate.overlapping_stop_id) {
      joined = &full_stop_slot_;
    }
    for (std::size_t index = 0U; joined == nullptr && index < correlation_capacity_; ++index) {
      if (slots_[index].active && view(slots_[index].stop_id, slots_[index].stop_id_size) ==
                                      *candidate.overlapping_stop_id) {
        joined = &slots_[index];
      }
    }
    if (joined == nullptr) {
      return coalesce_full();
    }
  }

  const auto available = std::find_if(slots_.begin(), slots_.begin() + correlation_capacity_,
                                      [](const Slot& slot) { return !slot.active; });
  if (available == slots_.begin() + correlation_capacity_) {
    return coalesce_full();
  }

  const auto path = joined == nullptr ? EmergencyStopAcceptancePathDTO::kFreshReserved
                                      : EmergencyStopAcceptancePathDTO::kJoinedResident;
  const auto available_index =
      static_cast<StopControlSlotIndex>(std::distance(slots_.begin(), available));
  fill(*available, available_index, joined, joined != nullptr && joined->full_stop,
       candidate.disposition, path,
       joined != nullptr ? view(joined->scope_digest, joined->scope_digest_size)
                         : candidate.captured_scope_digest);
  return publish(*available, available_index, RequestReplayStatusDTO::kAcceptedNew, path, true);
}

bool EmergencyStopAcceptanceReserve::mark_projection(
    std::string_view stop_id, EmergencyStopProjectionStateDTO projection_state) noexcept {
  if (!portable_token(stop_id, kMaximumEmergencyStopIdentityBytes) ||
      !known_projection_state(projection_state)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  bool found = false;
  const auto update = [&](Slot& slot) {
    if (slot.active && view(slot.stop_id, slot.stop_id_size) == stop_id) {
      slot.projection_state = projection_state;
      found = true;
    }
  };
  for (std::size_t index = 0U; index < correlation_capacity_; ++index) {
    update(slots_[index]);
  }
  update(full_stop_slot_);
  return found;
}

std::optional<EmergencyStopAcceptanceOutcome> EmergencyStopAcceptanceReserve::find(
    std::string_view request_id) const noexcept {
  if (!portable_token(request_id, kMaximumEmergencyStopRequestIdBytes)) {
    return std::nullopt;
  }
  std::lock_guard lock(mutex_);
  const auto project = [](const Slot& slot, StopControlSlotIndex correlation_slot) {
    EmergencyStopAcceptanceOutcome result;
    result.replay_status = RequestReplayStatusDTO::kReplayedExact;
    result.acceptance_path = slot.acceptance_path;
    result.disposition = slot.disposition;
    result.stop_generation = slot.stop_generation;
    result.accepted_at_ns = slot.accepted_at_ns;
    result.work_token = slot.work_token;
    result.correlation_slot = correlation_slot;
    result.correlation_retained = true;
    result.full_stop = slot.full_stop;
    assign(result.stop_id_bytes_, result.stop_id_size_, view(slot.stop_id, slot.stop_id_size));
    assign(result.scope_digest_bytes_, result.scope_digest_size_,
           view(slot.scope_digest, slot.scope_digest_size));
    return result;
  };
  // A conflicting resident request ID may have a newer conservative Full-Stop
  // correlation while the original scoped record remains resident. The Full
  // record is the only safe ID-only projection in that case.
  if (full_stop_slot_.active &&
      view(full_stop_slot_.request_id, full_stop_slot_.request_id_size) == request_id) {
    return project(full_stop_slot_, static_cast<StopControlSlotIndex>(correlation_capacity_));
  }
  for (std::size_t index = 0U; index < correlation_capacity_; ++index) {
    if (slots_[index].active &&
        view(slots_[index].request_id, slots_[index].request_id_size) == request_id) {
      return project(slots_[index], static_cast<StopControlSlotIndex>(index));
    }
  }
  return std::nullopt;
}

std::size_t EmergencyStopAcceptanceReserve::release_after_terminal_handoff(
    std::string_view stop_id) noexcept {
  if (!portable_token(stop_id, kMaximumEmergencyStopIdentityBytes)) {
    return 0U;
  }
  std::lock_guard lock(mutex_);
  std::size_t released = 0U;
  const auto release = [&](Slot& slot) {
    if (slot.active && view(slot.stop_id, slot.stop_id_size) == stop_id) {
      slot = Slot{};
      ++released;
    }
  };
  for (std::size_t index = 0U; index < correlation_capacity_; ++index) {
    release(slots_[index]);
  }
  release(full_stop_slot_);
  return released;
}

std::size_t EmergencyStopAcceptanceReserve::active_correlation_count() const noexcept {
  std::lock_guard lock(mutex_);
  return static_cast<std::size_t>(std::count_if(slots_.begin(),
                                                slots_.begin() + correlation_capacity_,
                                                [](const Slot& slot) { return slot.active; }));
}

bool EmergencyStopAcceptanceReserve::full_stop_active() const noexcept {
  std::lock_guard lock(mutex_);
  return full_stop_slot_.active;
}

}  // namespace manyforge::core
