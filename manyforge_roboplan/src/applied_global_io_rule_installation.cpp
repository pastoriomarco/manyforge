#include "applied_global_io_rule_installation.hpp"
#include "applied_program_owner_reader.hpp"
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
  const auto identity = [](const auto &id) {
    return !id.empty() && id.value().find('\0') == std::string::npos;
  };
  if (!identity(context.owner_epoch) ||
      !context.active_publication_generation.nonzero() ||
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
  if (actual.value().owner_epoch != context.owner_epoch ||
      actual.value().catalog_generation == 0U)
    return rejected("io_global_rule_service_context_mismatch");
  std::vector<std::string> keys;
  keys.reserve(policies.value().referenced_deployment_blackboard_keys.size());
  for (const auto &key : policies.value().referenced_deployment_blackboard_keys)
    keys.push_back(key.name);
  auto catalog = std::make_shared<const core::IoActivationRuleCatalogDTO>(
      core::IoActivationRuleCatalogDTO{
          {context.owner_epoch, actual.value().catalog_generation,
           context.active_publication_generation, context.retained_bundle_id,
           context.graph_revision},
          std::move(rules),
          binding ? std::optional(binding->program_binding_id()) : std::nullopt,
          std::move(keys)});
  return Reply::Ok({std::move(catalog), nullptr});
}

namespace {
core::Result<void> CommitAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id, bool restore) {
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
    std::optional<core::NamedApplyPublicationTransactionDTO> origin;
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
        {kernel.request_owner_epoch(), current->publication_generation,
         current->bundle->retained_bundle_id(),
         current->bundle->value().cell_graph.graph_revision},
        current->origin_named_apply});
    if (selected->waiting &&
        (selected->catalog || selected->service || selected->program ||
         selected->binding || selected->waiting->context != selected->context))
      return stale();
    const auto commit = [&] {
      const auto result = restore
          ? kernel.RestoreGlobalIoRuleCatalog(
                selected->context, selected->catalog, selected->service,
                selected->waiting)
          : kernel.ActivateGlobalIoRuleCatalog(selected->context, selected->service);
      if (result.ok() && selected->runtime)
        selected->runtime->RequestRuleEvidenceProcessing();
      return result;
    };
    const auto actual = kernel.ReadInterruptRuleStateBaseline();
    if (actual &&
        ((selected->catalog && actual->catalog == selected->catalog &&
          !selected->waiting && !actual->waiting_for_program &&
          !actual->verified_empty) ||
         (selected->waiting && !selected->catalog && !selected->service &&
          !actual->catalog && !actual->verified_empty &&
          actual->waiting_for_program == selected->waiting &&
          selected->waiting->context == selected->context) ||
         (!selected->catalog && !selected->waiting && !selected->service &&
          !actual->catalog && !actual->waiting_for_program &&
          actual->verified_empty == selected->context)))
      return commit();
  }
  if ((selected->program == nullptr) != (selected->binding == nullptr))
    return stale();
  std::optional<detail::AppliedOwnerSnapshot> proof;
  if (selected->program) {
    const auto actual = detail::ReadAppliedProgramOwnerReadOnly(
        manager_state_root, cell_id,
        selected->context.active_publication_generation);
    if (!actual.ok())
      return Reply::Err(actual.error());
    if (actual.value().program_id != selected->program->value().program_id ||
        actual.value().program_revision !=
            selected->program->value().program_revision ||
        actual.value().program_binding_id !=
            selected->binding->program_binding_id())
      return stale();
    proof.emplace(detail::AppliedOwnerSnapshot{
        actual.value().publication, actual.value().apply_intent_digest,
        actual.value().terminal_outcome_digest});
  } else {
    const auto actual = detail::ReadAppliedOwnerReadOnly(
        manager_state_root, cell_id,
        selected->context.active_publication_generation);
    if (!actual.ok())
      return Reply::Err(actual.error());
    proof.emplace(actual.value());
  }
  const auto *origin = std::get_if<core::NamedApplyPublicationTransactionDTO>(
      &proof->publication.request.transaction_ref);
  if (!origin || selected->origin != *origin ||
      proof->publication.request.candidate_retained_bundle_id !=
          selected->context.retained_bundle_id ||
      proof->publication.request.candidate_graph_revision !=
          selected->context.graph_revision)
    return stale();
  std::shared_lock lock(access.mutex);
  const auto *current = access.product();
  if (!current || current->bundle != selected->bundle ||
      current->program != selected->program ||
      current->program_binding != selected->binding ||
      current->compiled_global_io_rules != selected->catalog ||
      current->compiled_waiting_global_io_rules != selected->waiting ||
      current->io_signal_service != selected->service ||
      current->io_signal_runtime != selected->runtime ||
      current->publication_generation !=
          selected->context.active_publication_generation ||
      current->origin_named_apply != selected->origin)
    return stale();
  const auto result = restore
      ? kernel.RestoreGlobalIoRuleCatalog(
            selected->context, selected->catalog, selected->service,
            selected->waiting)
      : kernel.ActivateGlobalIoRuleCatalog(selected->context, selected->service);
  if (result.ok() && selected->runtime)
    selected->runtime->RequestRuleEvidenceProcessing();
  return result;
}

} // namespace

core::Result<void> EnsureAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id) {
  return CommitAppliedGlobalIoRuleCatalog(access, kernel, manager_state_root,
                                          cell_id, false);
}

core::Result<void> RestoreAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id) {
  return CommitAppliedGlobalIoRuleCatalog(access, kernel, manager_state_root,
                                          cell_id, true);
}

} // namespace manyforge::roboplan_backend::host_detail
