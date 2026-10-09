#pragma once

#include <cstdint>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <string_view>

namespace manyforge::core {

/// Last established phase of one command (dispatch, scale or Stop/cancel)
/// for one `ExecutionGoalKey`. Each phase has exactly one source:
/// - kAccepted: Kernel acceptance DTOs;
/// - kSubmitted: the mechanism's Submit/Post result, never a Kernel transition;
/// - kConsumed: the loop's consumed-generation status, never a successful
///   write or action response;
/// - kRealized: the adapter's command-realization contract, never loop
///   consumption or a suppressed/coalesced write.
/// Phase says how far the command provably got; it says nothing about
/// effects. Delivery downstream, position reached and stationary are separate
/// facts, never inferred from this phase.
enum class ExecutionCommandPhase : std::uint8_t {
  kAccepted = 0,
  kSubmitted,
  kConsumed,
  kRealized,
};

[[nodiscard]] constexpr std::string_view to_string(ExecutionCommandPhase value) noexcept {
  switch (value) {
    case ExecutionCommandPhase::kAccepted:
      return "accepted";
    case ExecutionCommandPhase::kSubmitted:
      return "submitted";
    case ExecutionCommandPhase::kConsumed:
      return "consumed";
    case ExecutionCommandPhase::kRealized:
      return "realized";
  }
  return {};
}

/// Phase and effect knowledge as two independent facts. A command can fail
/// after producing an effect; a timeout after submission leaves the effect
/// `kEffectUncertain` (outcome unknown). Effect knowledge reuses the CCLC
/// effect enum: kNone (nothing known yet), kKnownNoEffect, kEffectObserved,
/// kPartialEffectObserved (e.g. partial interface write), kEffectUncertain.
/// Uncertainty never triggers a resend or replacement by itself; containment
/// follows the existing policy for it.
struct ExecutionCommandKnowledge final {
  ExecutionCommandPhase phase{ExecutionCommandPhase::kAccepted};
  ComponentLifecycleDomainEffectDTO effect{ComponentLifecycleDomainEffectDTO::kNone};

  friend constexpr bool operator==(const ExecutionCommandKnowledge&,
                                   const ExecutionCommandKnowledge&) = default;
};

}  // namespace manyforge::core
