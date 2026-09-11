#pragma once

#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/stop_timing.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <map>
#include <span>

namespace manyforge::deployment::detail {

using BindingStopTimingBudgets =
    std::map<core::ControlBindingId, core::StopDeadlineBudgetDTO>;

/// Shared cold derivation for compiler, retained verifier and bootstrap. All
/// binding policies are independently qualified before entering this function.
inline core::Result<BindingStopTimingBudgets> resolve_binding_stop_timing(
    std::span<const core::ControlBindingDTO> bindings,
    std::span<const QualifiedStopEvidencePolicy> policies,
    std::span<const core::StopTimingSelectionDTO> selections) {
  const auto invalid = [](std::string message) {
    return core::Result<BindingStopTimingBudgets>::Err(
        {"invalid_request",
         std::move(message),
         {{"reason", "stop_timing_binding_selection_invalid"}}});
  };
  if (bindings.size() > core::kMaxDeploymentApplicabilityRows ||
      selections.size() > core::kMaxDeploymentApplicabilityRows)
    return invalid("Stop timing inventory exceeds the deployment bound");
  using PolicyKey =
      std::pair<core::StopEvidencePolicyId, core::StopEvidencePolicyRevision>;
  std::map<PolicyKey, const core::StopEvidencePolicyDTO *> indexed_policies;
  for (const auto &policy : policies) {
    const auto &value = policy.value();
    if (!indexed_policies
             .emplace(PolicyKey{value.stop_evidence_policy_id,
                                value.stop_evidence_policy_revision},
                      &value)
             .second)
      return invalid("Stop timing policy inventory contains duplicates");
  }
  std::map<core::ControlBindingId, const core::StopTimingSelectionDTO *>
      indexed_selections;
  for (const auto &selection : selections)
    if (!indexed_selections.emplace(selection.control_binding_id, &selection)
             .second)
      return invalid("Stop timing selection repeats a binding");
  std::vector<core::StopDeadlineBudgetDTO> budgets;
  BindingStopTimingBudgets result;
  for (const auto &binding : bindings) {
    const auto policy =
        indexed_policies.find({binding.stop_evidence_policy_id,
                               binding.stop_evidence_policy_revision});
    if (policy == indexed_policies.end())
      return invalid("Stop timing lost an exact binding policy");
    std::optional<core::StopTimingParametersDTO> timing;
    const auto selected = indexed_selections.find(binding.control_binding_id);
    if (selected != indexed_selections.end()) {
      if (selected->second->control_binding_revision !=
              binding.control_binding_revision ||
          policy->second->disposition !=
              core::StopEvidenceDisposition::kStoppable)
        return invalid("Stop timing selects a stale or non-stoppable binding");
      timing = selected->second->timing;
      indexed_selections.erase(selected);
    }
    if (policy->second->disposition ==
        core::StopEvidenceDisposition::kNotStoppable)
      continue;
    auto resolved = core::resolve_stop_timing_budget(*policy->second, timing);
    if (!resolved.ok())
      return core::Result<BindingStopTimingBudgets>::Err(resolved.error());
    if (!result.emplace(binding.control_binding_id, std::move(resolved.value()))
             .second)
      return invalid("Stop timing inventory repeats a binding");
  }
  if (!indexed_selections.empty())
    return invalid("Stop timing selects an unknown binding");
  for (const auto &[id, budget] : result)
    budgets.push_back(budget);
  const auto harmonized = core::harmonize_stop_timing_budgets(budgets);
  if (!harmonized.ok())
    return core::Result<BindingStopTimingBudgets>::Err(harmonized.error());
  std::size_t index = 0;
  for (auto &[id, budget] : result)
    budget = std::move(budgets[index++]);
  return core::Result<BindingStopTimingBudgets>::Ok(std::move(result));
}

} // namespace manyforge::deployment::detail
