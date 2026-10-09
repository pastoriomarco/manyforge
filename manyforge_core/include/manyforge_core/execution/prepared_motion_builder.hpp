#pragma once

#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/execution/prepared_motion_slot.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <span>

// The one shared `PreparedMotion` builder (SX-032). The host participant
// execution and every cross-process receiver (the controller plugin) call the
// same function on the same sealed inputs; no `PreparedMotion` field is ever
// transported or trusted from transport. Cold path only: it allocates, throws
// internally and reports refusals as values. Never call it from RT code.

namespace manyforge::core {

/// Builds the immutable slot from sealed inputs. Decodes and verifies the
/// sealed participant with the exact binding, readiness, retained default
/// limits and node category (`DecodePreparedOperationParticipant`, which
/// recomputes its digest), requires `sealed.participant_digest` to be that
/// digest, decodes the canonical trajectory and requires it to be exactly the
/// participant's motion trajectory, and binds `key` to the participant's
/// operation and binding identities. The `PreparedMotion` fields are then
/// derived from these inputs only (see the implementation's field notes).
///
/// Refusals (before any transport effect) carry code
/// `prepared_motion_refused` and a precise `reason` detail.
///
/// Deterministic: identical inputs yield identical slots.
[[nodiscard]] Result<PreparedMotionSlot> BuildPreparedMotionSlot(
    const SealedPreparedMotionInputs& sealed, const ExecutionGoalKey& key,
    const RuntimeParticipantBinding& binding, const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category);

}  // namespace manyforge::core
