#include <algorithm>
#include <cmath>
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <map>
#include <string>

namespace manyforge::core {
namespace {

ErrorDTO invalid(std::string reason, std::string message,
                 std::map<std::string, std::string> details = {}) {
  details.emplace("reason", std::move(reason));
  return {"invalid_request", std::move(message), std::move(details)};
}

std::optional<ErrorDTO> timing_error(const StopTimingParametersDTO& timing) {
  if (timing.command_latency_allowance_ns <= 0 || timing.settling_allowance_ns < 0 ||
      timing.operational_timeout_ns <= 0 ||
      timing.operational_timeout_ns > kMaxStopEvidencePolicyDurationNs ||
      timing.command_latency_allowance_ns > kMaxStopEvidencePolicyDurationNs ||
      timing.settling_allowance_ns >
          kMaxStopEvidencePolicyDurationNs - timing.command_latency_allowance_ns) {
    return invalid("stop_timing_parameters_invalid",
                   "Stop timing requires positive latency and timeout, nonnegative settling, "
                   "and representable latency plus settling allowances");
  }
  return std::nullopt;
}

bool valid_budget(const StopDeadlineBudgetDTO& budget) {
  if (budget.outer_duration_ns <= 0 ||
      budget.outer_duration_ns > kMaxStopEvidencePolicyDurationNs ||
      budget.kernel_completion_reserve_ns <= 0 ||
      budget.kernel_completion_reserve_ns >= budget.outer_duration_ns || budget.phases.empty() ||
      budget.phases.size() > kMaxStopEvidencePolicyItems)
    return false;
  std::int64_t previous{};
  for (std::size_t index = 0; index < budget.phases.size(); ++index) {
    const auto& phase = budget.phases[index];
    if (phase.phase_id.empty() || phase.deadline_offset_ns <= previous ||
        phase.deadline_offset_ns > budget.outer_duration_ns - budget.kernel_completion_reserve_ns ||
        std::any_of(budget.phases.begin(), budget.phases.begin() + index,
                    [&](const auto& earlier) { return earlier.phase_id == phase.phase_id; }))
      return false;
    previous = phase.deadline_offset_ns;
  }
  return true;
}

}  // namespace

Result<std::int64_t> authored_stop_duration_ns(const double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0 || seconds > kMaxControlledStopDecelerationTimeS)
    return Result<std::int64_t>::Err(
        invalid("stop_profile_duration_out_of_range",
                "Controlled Stop duration is outside its qualified range"));
  const auto duration = std::llround(seconds * 1'000'000'000.0);
  if (duration <= 0)
    return Result<std::int64_t>::Err(
        invalid("stop_profile_duration_out_of_range",
                "Controlled Stop duration must round to at least one nanosecond"));
  return Result<std::int64_t>::Ok(duration);
}

Result<void> validate_controlled_stop_timing(const StopTimingParametersDTO& timing,
                                             const std::int64_t full_profile_duration_ns) {
  if (const auto error = timing_error(timing)) return Result<void>::Err(*error);
  if (full_profile_duration_ns <= 0 ||
      full_profile_duration_ns > kMaxStopEvidencePolicyDurationNs) {
    return Result<void>::Err(
        invalid("stop_profile_timing_out_of_range",
                "Complete Stop profile exceeds the supported timing range"));
  }
  // Item 24: no Apply-time Stop timing refusal. As `main`, the Stop has one
  // deadline and its watchdog issues the hard floor on overrun; a timeout
  // shorter than the profile is not refused in advance.
  return Result<void>::Ok();
}

Result<StopDeadlineBudgetDTO> resolve_stop_timing_budget(
    const StopEvidencePolicyDTO& policy, const std::optional<StopTimingParametersDTO>& timing) {
  if (policy.disposition != StopEvidenceDisposition::kStoppable || !policy.deadline_budget ||
      !valid_budget(*policy.deadline_budget) || policy.escalation_stages.empty() ||
      policy.escalation_stages.size() > kMaxStopEscalationStages ||
      policy.escalation_stages.front().stage_kind != StopEscalationStageKind::kNominal) {
    return Result<StopDeadlineBudgetDTO>::Err(
        invalid("stop_timing_policy_invalid", "Stop timing requires a valid stoppable policy"));
  }
  if (!timing) return Result<StopDeadlineBudgetDTO>::Ok(*policy.deadline_budget);
  if (const auto error = timing_error(*timing)) return Result<StopDeadlineBudgetDTO>::Err(*error);
  // The terminal deadline must follow the command phase; no settling
  // allowance is required (items 1/26: no rest dwell).
  const auto minimum_timeout = timing->command_latency_allowance_ns + 1;
  if (timing->operational_timeout_ns < minimum_timeout) {
    return Result<StopDeadlineBudgetDTO>::Err(
        invalid("stop_timing_parameters_invalid",
                "Stop timeout must be longer than the command latency allowance",
                {{"minimum_timeout_ns", std::to_string(minimum_timeout)}}));
  }
  const auto& nominal = policy.escalation_stages.front();
  auto budget = *policy.deadline_budget;
  const auto phase_index = [&](const StopBudgetPhaseId& id) {
    const auto found = std::find_if(budget.phases.begin(), budget.phases.end(),
                                    [&](const auto& phase) { return phase.phase_id == id; });
    return static_cast<std::size_t>(found - budget.phases.begin());
  };
  const auto command_index = phase_index(nominal.containment_command.command_phase_id);
  std::size_t terminal_index = command_index;
  for (const auto& requirement : nominal.evidence_requirements) {
    const auto index = phase_index(requirement.phase_id);
    if (index == budget.phases.size() || index < command_index) {
      return Result<StopDeadlineBudgetDTO>::Err(invalid(
          "stop_timing_phase_reference_invalid", "Nominal Stop evidence has an invalid phase"));
    }
    terminal_index = std::max(terminal_index, index);
  }
  if (command_index != 0 || terminal_index >= budget.phases.size() ||
      terminal_index == command_index) {
    return Result<StopDeadlineBudgetDTO>::Err(invalid(
        "stop_timing_nominal_phases_invalid",
        "Configurable Stop timing requires distinct initial command and terminal evidence phases"));
  }
  const auto old_terminal = budget.phases[terminal_index].deadline_offset_ns;
  const auto suffix = budget.outer_duration_ns - old_terminal;
  if (timing->operational_timeout_ns > kMaxStopEvidencePolicyDurationNs - suffix) {
    return Result<StopDeadlineBudgetDTO>::Err(
        invalid("stop_timing_completion_envelope_out_of_range",
                "Stop timeout leaves no representable bounded post-fault completion envelope"));
  }
  const auto shift = timing->operational_timeout_ns - old_terminal;
  budget.phases[command_index].deadline_offset_ns = timing->command_latency_allowance_ns;
  for (std::size_t index = terminal_index; index < budget.phases.size(); ++index)
    budget.phases[index].deadline_offset_ns += shift;
  budget.outer_duration_ns += shift;
  if (!valid_budget(budget)) {
    return Result<StopDeadlineBudgetDTO>::Err(
        invalid("configured_stop_phase_order_invalid",
                "Stop timings conflict with the qualified intermediate phase deadlines"));
  }
  return Result<StopDeadlineBudgetDTO>::Ok(std::move(budget));
}

Result<void> harmonize_stop_timing_budgets(const std::span<StopDeadlineBudgetDTO> budgets) {
  std::int64_t outer{}, reserve{}, last_phase{};
  for (const auto& budget : budgets) {
    if (!valid_budget(budget))
      return Result<void>::Err(
          invalid("stop_timing_budget_invalid",
                  "Cannot derive a common cell envelope from an invalid Stop budget"));
    outer = std::max(outer, budget.outer_duration_ns);
    reserve = std::max(reserve, budget.kernel_completion_reserve_ns);
    last_phase = std::max(last_phase, budget.phases.back().deadline_offset_ns);
  }
  if (last_phase > kMaxStopEvidencePolicyDurationNs - reserve)
    return Result<void>::Err(
        invalid("stop_timing_completion_envelope_out_of_range",
                "Common Stop completion envelope exceeds the supported range"));
  outer = std::max(outer, last_phase + reserve);
  for (auto& budget : budgets) {
    budget.outer_duration_ns = outer;
    budget.kernel_completion_reserve_ns = reserve;
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
