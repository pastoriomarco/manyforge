#include "applied_global_io_rule_installation.hpp"
#include "ros_host_composition.hpp"
#include "host_io_signal_runtime.hpp"

#include <algorithm>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>

namespace manyforge::roboplan_backend::host_detail {

core::Result<CompiledGlobalIoRuleProduct>
CompileGlobalIoRuleCatalog(
    const core::IoRuleAppliedContextDTO &context,
    const deployment::RetainedBundle &bundle,
    const deployment::ProgramBindingContract *program,
    const deployment::ProgramBinding *binding,
    const std::shared_ptr<core::IIoSignalService> &service) {
  using Reply = core::Result<CompiledGlobalIoRuleProduct>;
  const auto rejected = [](const char *reason) {
    return Reply::Err(
        {"invalid_state_transition",
         "Global interrupt rules differ from the actual source products",
         {{"reason", reason}}});
  };
  if (!context.active_publication_generation.nonzero() ||
      context.active_publication_generation.value() >
          9'007'199'254'740'991ULL ||
      context.retained_bundle_id != bundle.retained_bundle_id() ||
      context.graph_revision != bundle.value().cell_graph.graph_revision)
    return rejected("io_global_rule_candidate_context_mismatch");
  auto policies =
      deployment::compile_retained_io_policies(program, binding, bundle);
  if (!policies.ok())
    return Reply::Err(policies.error());
  auto rules = std::move(policies.value().activation_policies);
  std::sort(rules.begin(), rules.end(),
            [](const auto &left, const auto &right) {
              return left.policy_id < right.policy_id;
            });
  // A no-service product cannot hide Program-only BB demand. With an actual
  // service, its bounded source installation was separately compiled by the
  // existing resource installer; this projection owns only global rules.
  if (!service) {
    const auto signals =
        deployment::ResolveRetainedIoSignalCatalog(bundle.value().cell_graph);
    if (!signals.ok())
      return Reply::Err(signals.error());
    if (!signals.value().empty())
      return rejected("io_global_rule_service_missing");
    if (!policies.value().referenced_deployment_blackboard_keys.empty())
      return rejected("io_global_rule_service_missing");
    if (program) {
      const auto conditions =
          deployment::compile_program_io_conditions(*program, *binding, bundle);
      if (!conditions.ok())
        return Reply::Err(conditions.error());
      if (!conditions.value().referenced_program_blackboard_keys.empty())
        return rejected("io_global_rule_service_missing");
    }
    if (rules.empty())
      return Reply::Ok({});
    if (program || binding ||
        std::any_of(rules.begin(), rules.end(), [](const auto &rule) {
          return rule.condition || rule.waiting_for_program_keys.empty() ||
                 !rule.required_signal_sources.empty();
        }))
      return rejected("io_global_rule_service_missing");
    return Reply::Ok(
        {nullptr,
         std::make_shared<const core::IoRuleWaitingForProgramCatalogDTO>(
             core::IoRuleWaitingForProgramCatalogDTO{context,
                                                    std::move(rules)})});
  }
  const auto actual = service->RuleServiceConfiguration();
  if (!actual.ok())
    return Reply::Err(actual.error());
  if (actual.value().catalog_generation == 0U)
    return rejected("io_global_rule_service_context_mismatch");
  std::vector<std::string> keys;
  keys.reserve(policies.value().referenced_deployment_blackboard_keys.size());
  for (const auto &key : policies.value().referenced_deployment_blackboard_keys)
    keys.push_back(key.name);
  auto catalog = std::make_shared<const core::IoActivationRuleCatalogDTO>(
      core::IoActivationRuleCatalogDTO{
          {actual.value().catalog_generation,
           context.active_publication_generation, context.retained_bundle_id,
           context.graph_revision},
          std::move(rules),
          binding ? std::optional(binding->program_binding_id()) : std::nullopt,
          std::move(keys)});
  return Reply::Ok({std::move(catalog), nullptr});
}

namespace {
/// The committed composition takes the grasp-lost levels: the host
/// store's sink moves to its runtime and the rows of grippers it does not hold
/// drop. Only here, after the terminal publication proof; an Apply candidate
/// never prunes.
void AdoptCarriedObjectLostSignals(const std::shared_ptr<HostIoSignalRuntime> &runtime) {
  if (runtime)
    runtime->AdoptCarriedObjectLostSignals();
  else
    host_carried_object_lost_signals().InstallSink(
        {}, {}, [](const std::vector<core::IoSignalId> &) {});
}

core::Result<void> CommitAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel, bool restore) {
  using Reply = core::Result<void>;
  struct Selection final {
    std::shared_ptr<const deployment::RetainedBundle> bundle;
    std::shared_ptr<const deployment::ProgramBindingContract> program;
    std::shared_ptr<const deployment::ProgramBinding> binding;
    std::shared_ptr<const core::IoActivationRuleCatalogDTO> catalog;
    std::shared_ptr<const core::IoRuleWaitingForProgramCatalogDTO> waiting;
    std::shared_ptr<core::IIoSignalService> service;
    std::shared_ptr<HostIoSignalRuntime> runtime;
    core::IoRuleAppliedContextDTO context;
  };
  const auto stale = [] {
    return Reply::Err({"stale_generation",
                       "The actual global rule product changed",
                       {{"reason", "io_global_rule_candidate_stale"}}});
  };
  std::optional<Selection> selected;
  {
    std::shared_lock lock(access.mutex);
    const auto *current = access.product();
    if (!current || !current->bundle)
      return stale();
    selected.emplace(Selection{
        current->bundle,
        current->program,
        current->program_binding,
        current->compiled_global_io_rules,
        current->compiled_waiting_global_io_rules,
        current->io_signal_service,
        current->io_signal_runtime,
        {current->publication_generation,
         current->bundle->retained_bundle_id(),
         current->bundle->value().cell_graph.graph_revision}});
    if (selected->waiting &&
        (selected->catalog || selected->service || selected->program ||
         selected->binding || selected->waiting->context != selected->context))
      return stale();
    const auto commit = [&] {
      auto result = restore
          ? kernel.RestoreGlobalIoRuleCatalog(
                selected->context, selected->catalog, selected->service,
                selected->waiting)
          : kernel.ActivateGlobalIoRuleCatalog(selected->context, selected->service);
      // After an Apply (Load included) every rule returns
      // to its `active_at_start`.
      if (result.ok() && !restore)
        result = kernel.reset_rule_activation();
      if (result.ok())
        AdoptCarriedObjectLostSignals(selected->runtime);
      if (result.ok() && selected->runtime)
        selected->runtime->RequestRuleEvidenceProcessing();
      return result;
    };
    const auto actual = kernel.ReadInterruptRuleStateBaseline();
    const bool committed =
        actual &&
        ((selected->catalog && actual->catalog == selected->catalog &&
          !selected->waiting && !actual->waiting_for_program &&
          !actual->verified_empty) ||
         (selected->waiting && !selected->catalog && !selected->service &&
          !actual->catalog && !actual->verified_empty &&
          actual->waiting_for_program == selected->waiting &&
          selected->waiting->context == selected->context) ||
         (!selected->catalog && !selected->waiting && !selected->service &&
          !actual->catalog && !actual->waiting_for_program &&
          actual->verified_empty == selected->context));
    if (!committed &&
        (selected->program == nullptr) != (selected->binding == nullptr))
      return stale();
    // The caller acts on the Manager's success of this product's Apply.
    return commit();
  }
}

} // namespace

core::Result<void> EnsureAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel) {
  return CommitAppliedGlobalIoRuleCatalog(access, kernel, false);
}

core::Result<void> RestoreAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel) {
  return CommitAppliedGlobalIoRuleCatalog(access, kernel, true);
}

} // namespace manyforge::roboplan_backend::host_detail
