#include <manyforge_deployment/retained_io_policy_compiler.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include "program_io_condition_compiler_internal.hpp"

namespace manyforge::deployment {
namespace {
const CanonicalValue &member(const CanonicalValue &value,
                             std::string_view name) {
  const auto *found = value.find_object_member(name);
  if (!found)
    throw std::invalid_argument("retained_global_rule_invalid");
  return *found;
}

// Follow the condition lowerer's existing local structured-error convention.
// These diagnostics do not change the selection/refusal predicates.
class GroupError final : public std::invalid_argument {
public:
  GroupError(std::string reason, std::string rule_id, std::string group_id)
      : std::invalid_argument(
            "Global interrupt rule " + rule_id +
            (reason == "global_interrupt_program_binding_missing"
                 ? " requires an Applied Program to resolve execution group "
                 : " names an undeclared execution group ") + group_id),
        reason(std::move(reason)), rule_id(std::move(rule_id)),
        group_id(std::move(group_id)) {}
  std::string reason;
  std::string rule_id;
  std::string group_id;
};

std::vector<core::IoActivationMotionSubjectDTO>
group_subjects(const std::vector<std::string> &groups,
               const ProgramBindingContract &program,
               const ProgramBinding &binding, const RetainedBundle &bundle,
               const std::string &rule_id) {
  const auto &table = program.execution_table();
  std::set<std::uint32_t> nodes;
  for (const auto &id : groups) {
    if (id == "all") {
      for (std::size_t index = 0; index < table.nodes.size(); ++index)
        nodes.insert(static_cast<std::uint32_t>(index));
    } else {
      const auto selected = std::find_if(
          table.named_selections.begin(), table.named_selections.end(),
          [&](const auto &row) { return row.group_id == id; });
      if (selected == table.named_selections.end())
        throw GroupError("global_interrupt_group_unbound", rule_id, id);
      nodes.insert(selected->node_indices.begin(),
                   selected->node_indices.end());
    }
  }
  std::set<core::IoActivationMotionSubjectDTO> subjects;
  for (const auto index : nodes) {
    if (index >= table.nodes.size())
      throw std::invalid_argument("global_interrupt_group_invalid");
    for (const auto &effect : table.nodes[index].target_effects) {
      if (!effect.effectful ||
          effect.capability_id.value() != "execution.trajectory.execute")
        continue;
      const auto &targets = binding.value().target_role_bindings;
      const auto target =
          std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
            return row.target_role_id == effect.target_role_id;
          });
      if (target == targets.end())
        throw std::invalid_argument("global_interrupt_group_unbound");
      const auto capability =
          std::find_if(target->capability_bindings.begin(),
                       target->capability_bindings.end(), [&](const auto &row) {
                         return row.capability_id == effect.capability_id;
                       });
      // An optional unavailable capability has no resolved motion participant.
      if (capability == target->capability_bindings.end())
        continue;
      if (!capability->control_binding)
        throw std::invalid_argument("global_interrupt_group_not_motion");
      const auto &ref = *capability->control_binding;
      const auto &controls = bundle.value().cell_graph.control_bindings;
      const auto control =
          std::find_if(controls.begin(), controls.end(), [&](const auto &row) {
            return row.control_binding_id == ref.control_binding_id &&
                   row.control_binding_revision == ref.control_binding_revision;
          });
      if (control == controls.end() ||
          control->controllable_component_id !=
              target->controllable_component_id ||
          control->command_target_id != target->command_target_id)
        throw std::invalid_argument("global_interrupt_group_unbound");
      subjects.insert({control->controllable_component_id,
                       control->command_target_id, control->control_domain_id,
                       control->control_binding_id,
                       control->control_binding_revision});
    }
  }
  return {subjects.begin(), subjects.end()};
}
} // namespace

core::Result<RetainedIoPolicyProjection>
compile_retained_io_policies(const ProgramBindingContract *program,
                             const ProgramBinding *binding,
                             const RetainedBundle &retained_bundle) {
  using Reply = core::Result<RetainedIoPolicyProjection>;
  try {
    if ((program == nullptr) != (binding == nullptr))
      throw std::invalid_argument("global_interrupt_program_binding_missing");
    if (program) {
      const auto &bound = binding->value();
      if (bound.program_id != program->value().program_id ||
          bound.program_revision != program->value().program_revision ||
          bound.node_environment != program->value().node_environment ||
          bound.retained_bundle_id != retained_bundle.retained_bundle_id() ||
          bound.graph_revision !=
              retained_bundle.value().cell_graph.graph_revision)
        throw std::invalid_argument("program_capture_product_mismatch");
    }
    RetainedIoPolicyProjection projection;
    const auto *configuration =
        retained_bundle.normalized_root().find_object_member(
            "interrupt_policy_configuration");
    if (!configuration)
      return Reply::Ok(std::move(projection));
    for (const std::string_view name :
         {"interrupt_sources", "interrupt_policies"})
      if (!member(*configuration, name).as_array().empty())
        return Reply::Err(
            {"unsupported_capability",
             "Legacy interrupt declarations are not installed",
             {{"reason", "legacy_interrupt_policy_runtime_unsupported"}}});
    const auto *rules = configuration->find_object_member("interrupt_rules");
    if (!rules)
      return Reply::Ok(std::move(projection));
    const auto signals =
        ResolveRetainedIoSignalCatalog(retained_bundle.value().cell_graph);
    if (!signals.ok())
      return Reply::Err(signals.error());
    const std::span<const core::ProgramBlackboardKeyDeclarationDTO>
        declarations =
            program
                ? std::span<const core::ProgramBlackboardKeyDeclarationDTO>(
                      program->blackboard_key_declarations())
                : std::span<const core::ProgramBlackboardKeyDeclarationDTO>{};
    std::map<std::string, core::IoConditionValueKind> referenced;
    for (const auto &rule : rules->as_array()) {
      auto condition = detail::compile_authored_deployment_io_condition(
          member(rule, "condition"), declarations, signals.value(),
          program == nullptr);
      if (!condition.ok()) {
        auto failure = condition.error();
        const auto reason = failure.details.find("reason");
        const auto key = failure.details.find("key");
        if (program && reason != failure.details.end() &&
            reason->second == "interrupt_condition_key_refused" &&
            key != failure.details.end()) {
          const auto &name = key->second;
          const bool system =
              name == "node_outcome" || name.starts_with("node_outcome.") ||
              name == "interrupt_state" || name.starts_with("interrupt_state.");
          if (!system) {
            const auto rule_id = member(rule, "rule_id").as_string();
            failure.details.emplace("condition_reason", reason->second);
            failure.details["reason"] = "global_rule_key_undeclared";
            failure.details.emplace("rule_id", rule_id);
            failure.message +=
                " (global rule " + rule_id + ", key " + name + ")";
          }
        }
        return Reply::Err(std::move(failure));
      }
      if (condition.value().condition)
        for (const auto &input : condition.value().condition->inputs())
          if (const auto *key = std::get_if<core::IoConditionBlackboardKeyDTO>(
                  &input.source)) {
            const auto [known, inserted] =
                referenced.emplace(key->name, input.value_kind);
            if (!inserted && known->second != input.value_kind)
              throw std::invalid_argument("global_interrupt_key_type_conflict");
          }
      core::IoActivationRuleScopeSelectionDTO selection =
          core::IoActivationAllMotionScopeDTO{};
      const auto &scope = member(rule, "scope");
      if (scope.is_string()) {
        if (scope.as_string() == "everything")
          selection = core::IoActivationEverythingScopeDTO{};
        else if (scope.as_string() != "all_motion")
          throw std::invalid_argument("global_interrupt_scope_invalid");
      } else {
        std::vector<std::string> groups;
        for (const auto &name : member(scope, "execution_groups").as_array())
          groups.push_back(name.as_string());
        const auto &rule_id = member(rule, "rule_id").as_string();
        if (!program || !binding) {
          if (!groups.empty())
            throw GroupError("global_interrupt_program_binding_missing",
                             rule_id, groups.front());
          throw std::invalid_argument("global_interrupt_program_binding_missing");
        }
        selection = core::IoActivationExecutionGroupsScopeDTO{
            binding->program_binding_id(), groups,
            group_subjects(groups, *program, *binding, retained_bundle, rule_id)};
      }
      // on_trip (sealed by the codec, default gate) selects the existing
      // mechanisms: gate = scoped Stop, always latch while the condition is
      // true (upfront refusal), auto release on clear; full_stop = cell Full
      // Stop, idle per idle_behaviour, operator release.
      bool full_stop = false;
      if (const auto *value = rule.find_object_member("on_trip")) {
        if (value->as_string() == "full_stop")
          full_stop = true;
        else if (value->as_string() != "gate")
          throw std::invalid_argument("global_interrupt_on_trip_invalid");
      }
      std::optional<core::IoRuleIdleBehaviourDTO> idle;
      if (!full_stop)
        idle = core::IoRuleIdleBehaviourDTO::kLatch;
      if (const auto *value = rule.find_object_member("idle_behaviour")) {
        if (!full_stop)
          throw std::invalid_argument("global_interrupt_idle_requires_full_stop");
        if (value->as_string() == "latch")
          idle = core::IoRuleIdleBehaviourDTO::kLatch;
        else if (value->as_string() == "ignore")
          idle = core::IoRuleIdleBehaviourDTO::kIgnore;
        else
          throw std::invalid_argument("global_interrupt_idle_invalid");
      }
      std::optional<core::StopEscalationStageId> entry;
      if (const auto *value = rule.find_object_member("stop_entry_stage_id"))
        entry.emplace(value->as_string());
      const auto latch_release =
          full_stop ? core::InterruptPostLatchDisposition::kAwaitOperatorAck
                    : core::InterruptPostLatchDisposition::kAutoClearOnSignalRelease;
      projection.activation_policies.push_back(
          {core::InterruptPolicyId(member(rule, "rule_id").as_string()),
           std::move(condition.value().condition),
           std::move(condition.value().required_signal_sources),
           std::move(selection), idle,
           static_cast<int>(member(rule, "priority").as_number()), entry,
           latch_release,
           std::move(condition.value().waiting_for_program_keys), full_stop});
    }
    for (const auto &[name, kind] : referenced)
      projection.referenced_deployment_blackboard_keys.push_back({name, kind});
    return Reply::Ok(std::move(projection));
  } catch (const GroupError &failure) {
    return Reply::Err({"invalid_request", failure.what(),
                       {{"reason", failure.reason}, {"rule_id", failure.rule_id},
                        {"group_id", failure.group_id}}});
  } catch (const std::exception &failure) {
    return Reply::Err({"invalid_request",
                       "Retained global interrupt rule is not qualified",
                       {{"reason", failure.what()}}});
  }
}

core::Result<RetainedIoPolicyProjection>
compile_retained_io_policies(const ProgramBindingContract &program,
                             const ProgramBinding &binding,
                             const RetainedBundle &retained_bundle) {
  return compile_retained_io_policies(&program, &binding, retained_bundle);
}
} // namespace manyforge::deployment
