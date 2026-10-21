#include <algorithm>
#include <manyforge_core/io_rule_stop_response.hpp>
#include <set>

namespace manyforge::core {
namespace {
ErrorDTO invalid(const char* reason) {
  return {std::string(to_string(ErrorCode::kInvalidRequest)),
          "Interrupt response does not select its qualified retained Stop chain",
          {{"reason", reason}}};
}

bool permitted_entry(const IoRuleStopEntryRowDTO& stage) {
  const auto action = stage.stop_action_class;
  if (stage.stage_kind == StopEscalationStageKind::kSoftwareHard)
    return action == StopActionClass::kSoftwareHard;
  return (stage.stage_kind == StopEscalationStageKind::kNominal ||
          stage.stage_kind == StopEscalationStageKind::kControlledFallback) &&
         action == StopActionClass::kControlled &&
         (stage.command_kind == StopContainmentCommandKind::kMotionControlledStop ||
          stage.command_kind == StopContainmentCommandKind::kControllerGoalCancel);
}
}  // namespace

Result<std::uint64_t> ResolveIoRuleStopEntryIndex(
    std::span<const IoRuleStopEntryRowDTO> rows,
    const std::optional<StopEscalationStageId>& requested_stage_id) {
  using Reply = Result<std::uint64_t>;
  if (rows.empty() || rows.size() > kMaxStopEscalationStages ||
      (requested_stage_id && requested_stage_id->value().empty()))
    return Reply::Err(invalid("io_rule_stop_entry_invalid"));
  std::set<StopEscalationStageId> ids;
  std::size_t selected = rows.size();
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& stage = rows[index];
    if (stage.stop_escalation_stage_id.value().empty() ||
        !ids.insert(stage.stop_escalation_stage_id).second)
      return Reply::Err(invalid("io_rule_stop_entry_chain_invalid"));
    if ((!requested_stage_id && index == 0U) ||
        (requested_stage_id && stage.stop_escalation_stage_id == *requested_stage_id))
      selected = index;
  }
  if (selected == rows.size() || !permitted_entry(rows[selected]))
    return Reply::Err(invalid("io_rule_stop_entry_not_qualified"));
  const bool floor_reachable =
      std::any_of(rows.begin() + selected, rows.end(), [](const auto& stage) {
        return stage.stage_kind == StopEscalationStageKind::kSoftwareHard &&
               stage.stop_action_class == StopActionClass::kSoftwareHard;
      });
  if (!floor_reachable) return Reply::Err(invalid("io_rule_stop_entry_skips_software_floor"));
  return Reply::Ok(static_cast<std::uint64_t>(selected));
}

Result<std::uint64_t> ResolveIoRuleSuccessThroughIndex(
    std::span<const IoRuleStopEntryRowDTO> rows,
    const std::optional<StopEscalationStageId>& entry_stage_id,
    const std::optional<StopEscalationStageId>& success_through_stage_id) {
  using Reply = Result<std::uint64_t>;
  const auto entry = ResolveIoRuleStopEntryIndex(rows, entry_stage_id);
  if (!entry.ok()) return Reply::Err(entry.error());
  if (!success_through_stage_id) return entry;
  const auto threshold = ResolveIoRuleStopEntryIndex(rows, success_through_stage_id);
  if (!threshold.ok()) return Reply::Err(threshold.error());
  const auto floor = std::find_if(rows.begin() + entry.value(), rows.end(), [](const auto& row) {
    return row.stage_kind == StopEscalationStageKind::kSoftwareHard &&
           row.stop_action_class == StopActionClass::kSoftwareHard;
  });
  if (threshold.value() < entry.value() ||
      threshold.value() > static_cast<std::uint64_t>(floor - rows.begin()))
    return Reply::Err(invalid("io_rule_success_through_outside_suffix"));
  return threshold;
}

Result<IoRulePreparedStopEntryDTO> ResolvePreparedIoRuleStopEntry(
    const PreparedParticipantStopPlanDTO& plan,
    const std::optional<StopEscalationStageId>& requested_stage_id) {
  using Reply = Result<IoRulePreparedStopEntryDTO>;
  if (plan.resolved_stop_base_plan_id.value().empty() || plan.stage_commands.empty() ||
      plan.stage_commands.size() > kMaxStopEscalationStages ||
      (requested_stage_id && requested_stage_id->value().empty()))
    return Reply::Err(invalid("io_rule_stop_entry_invalid"));
  std::vector<IoRuleStopEntryRowDTO> rows;
  rows.reserve(plan.stage_commands.size());
  for (std::size_t index = 0; index < plan.stage_commands.size(); ++index) {
    const auto& stage = plan.stage_commands[index].resolved_stage;
    if (stage.stage_index != index) return Reply::Err(invalid("io_rule_stop_entry_chain_invalid"));
    rows.push_back({stage.stop_escalation_stage_id, stage.stage_kind,
                    stage.containment_command.command_kind,
                    stage.command_qualification.stop_action_class});
  }
  const auto selected = ResolveIoRuleStopEntryIndex(rows, requested_stage_id);
  if (!selected.ok()) return Reply::Err(selected.error());
  const auto& stage = plan.stage_commands[selected.value()].resolved_stage;
  return Reply::Ok(
      {plan.resolved_stop_base_plan_id, stage.stop_escalation_stage_id, stage.stage_index});
}

Result<void> VerifyPreparedIoRuleStopEntry(const PreparedParticipantStopPlanDTO& plan,
                                           const IoRulePreparedStopEntryDTO& entry) {
  const auto resolved = ResolvePreparedIoRuleStopEntry(plan, entry.stop_escalation_stage_id);
  if (!resolved.ok()) return Result<void>::Err(resolved.error());
  if (resolved.value() != entry)
    return Result<void>::Err(invalid("io_rule_stop_entry_binding_mismatch"));
  return Result<void>::Ok();
}

Result<IoRulePreparedStopEntryDTO> ResolvePreparedIoRuleSuccessThrough(
    const PreparedParticipantStopPlanDTO& plan, const IoRulePreparedStopEntryDTO& entry,
    const std::optional<StopEscalationStageId>& success_through_stage_id) {
  using Reply = Result<IoRulePreparedStopEntryDTO>;
  const auto verified = VerifyPreparedIoRuleStopEntry(plan, entry);
  if (!verified.ok()) return Reply::Err(verified.error());
  if (!success_through_stage_id) return Reply::Ok(entry);
  const auto threshold = ResolvePreparedIoRuleStopEntry(plan, success_through_stage_id);
  if (!threshold.ok()) return Reply::Err(threshold.error());
  const auto floor = std::find_if(
      plan.stage_commands.begin() + entry.stage_index, plan.stage_commands.end(),
      [](const auto& command) {
        return command.resolved_stage.stage_kind == StopEscalationStageKind::kSoftwareHard &&
               command.resolved_stage.command_qualification.stop_action_class ==
                   StopActionClass::kSoftwareHard;
      });
  if (threshold.value().stage_index < entry.stage_index ||
      threshold.value().stage_index >
          static_cast<std::uint64_t>(floor - plan.stage_commands.begin()))
    return Reply::Err(invalid("io_rule_success_through_outside_suffix"));
  return threshold;
}

Result<void> VerifyPreparedIoRuleSuccessThrough(const PreparedParticipantStopPlanDTO& plan,
                                                const IoRulePreparedStopEntryDTO& entry,
                                                const IoRulePreparedStopEntryDTO& success_through) {
  const auto resolved =
      ResolvePreparedIoRuleSuccessThrough(plan, entry, success_through.stop_escalation_stage_id);
  if (!resolved.ok()) return Result<void>::Err(resolved.error());
  if (resolved.value() != success_through)
    return Result<void>::Err(invalid("io_rule_success_through_binding_mismatch"));
  return Result<void>::Ok();
}

Result<void> ValidateIoRuleNominalStopMode(const ResolvedStopBasePlanDTO& plan,
                                           const MotionInterruptStopProfileDTO& profile) {
  if (plan.escalation_stages.empty() || !plan.escalation_stages.front().default_contract_parameters)
    return Result<void>::Err(invalid("io_rule_nominal_mode_unqualified"));
  return ValidateIoRuleNominalStopMode(*plan.escalation_stages.front().default_contract_parameters,
                                       profile);
}

Result<void> ValidateIoRuleNominalStopMode(const JsonObjectDTO& parameters,
                                           const MotionInterruptStopProfileDTO& profile) {
  const auto found = parameters.find("stop_mode");
  const auto* retained =
      found == parameters.end() ? nullptr : std::get_if<std::string>(&found->second.value);
  const char* requested = nullptr;
  switch (profile.stop_mode) {
    case MotionInterruptStopMode::kSoftOnPath:
      requested = "soft_on_path";
      break;
    case MotionInterruptStopMode::kSpringBackHold:
      requested = "spring_back_hold";
      break;
    case MotionInterruptStopMode::kHardDeceleration:
      requested = "hard_deceleration";
      break;
    case MotionInterruptStopMode::kSpringBackFast:
      requested = "spring_back_fast";
      break;
  }
  if (!retained || !requested || *retained != requested)
    return Result<void>::Err(invalid("io_rule_nominal_mode_mismatch"));
  return Result<void>::Ok();
}
}  // namespace manyforge::core
