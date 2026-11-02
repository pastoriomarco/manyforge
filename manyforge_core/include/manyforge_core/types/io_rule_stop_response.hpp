#pragma once

#include <cstdint>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Declarative metadata of an already-qualified existing chain. This value
/// grants no prepared/runtime authority and contains no live owner facts.
struct IoRuleStopEntryRowDTO final {
  StopEscalationStageId stop_escalation_stage_id;
  StopEscalationStageKind stage_kind;
  StopContainmentCommandKind command_kind;
  StopActionClass stop_action_class;
};

/// Exact resolved member of the existing immutable prepared Stop chain.
/// Starting here retains the entire original suffix, including software floor.
struct IoRulePreparedStopEntryDTO final {
  ResolvedStopBasePlanId resolved_stop_base_plan_id;
  StopEscalationStageId stop_escalation_stage_id;
  std::uint64_t stage_index;
  friend bool operator==(const IoRulePreparedStopEntryDTO&,
                         const IoRulePreparedStopEntryDTO&) = default;
};

}  // namespace manyforge::core
