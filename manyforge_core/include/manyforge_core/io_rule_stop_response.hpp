#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <span>

namespace manyforge::core {

/// Shared pure declaration check used by Apply and runtime preparation. Rows
/// come from the actual qualified catalog/template; this function establishes
/// neither product qualification nor a runtime Stop plan.
[[nodiscard]] Result<std::uint64_t> ResolveIoRuleStopEntryIndex(
    std::span<const IoRuleStopEntryRowDTO> rows,
    const std::optional<StopEscalationStageId>& requested_stage_id);
/// Last acceptable containing stage in that same qualified suffix. Omission
/// means the selected entry itself; stages after the software floor refuse.
[[nodiscard]] Result<std::uint64_t> ResolveIoRuleSuccessThroughIndex(
    std::span<const IoRuleStopEntryRowDTO> rows,
    const std::optional<StopEscalationStageId>& entry_stage_id,
    const std::optional<StopEscalationStageId>& success_through_stage_id);
[[nodiscard]] Result<void> ValidateIoRuleNominalStopMode(
    const JsonObjectDTO& nominal_defaults, const MotionInterruptStopProfileDTO& profile);

/// Resolve only inside an already qualified immutable prepared chain. No stage
/// is removed or renumbered, and the software floor must remain reachable.
[[nodiscard]] Result<IoRulePreparedStopEntryDTO> ResolvePreparedIoRuleStopEntry(
    const PreparedParticipantStopPlanDTO& plan,
    const std::optional<StopEscalationStageId>& requested_stage_id);
[[nodiscard]] Result<void> VerifyPreparedIoRuleStopEntry(const PreparedParticipantStopPlanDTO& plan,
                                                         const IoRulePreparedStopEntryDTO& entry);

[[nodiscard]] Result<IoRulePreparedStopEntryDTO> ResolvePreparedIoRuleSuccessThrough(
    const PreparedParticipantStopPlanDTO& plan, const IoRulePreparedStopEntryDTO& entry,
    const std::optional<StopEscalationStageId>& success_through_stage_id);
[[nodiscard]] Result<void> VerifyPreparedIoRuleSuccessThrough(
    const PreparedParticipantStopPlanDTO& plan, const IoRulePreparedStopEntryDTO& entry,
    const IoRulePreparedStopEntryDTO& success_through);

/// Requested node mode equals the retained nominal row. Existing qualified
/// soft-to-spring substitution remains an internal execution of that same row.
[[nodiscard]] Result<void> ValidateIoRuleNominalStopMode(
    const ResolvedStopBasePlanDTO& plan, const MotionInterruptStopProfileDTO& profile);

}  // namespace manyforge::core
