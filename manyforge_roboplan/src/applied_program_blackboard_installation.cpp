#include "applied_program_blackboard_installation.hpp"
#include "applied_global_io_rule_installation.hpp"
#include "applied_program_owner_reader.hpp"

#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>
#include <map>

namespace manyforge::roboplan_backend::host_detail {
namespace {
core::ErrorDTO stale() {
  return {"stale_generation",
          "The actual Applied Program source changed",
          {{"reason", "program_bb_scope_stale"}}};
}

struct Selection final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> binding;
  std::shared_ptr<core::IIoSignalService> service;
  core::ActiveBundlePublicationGeneration publication{0U};
  std::optional<core::NamedApplyPublicationTransactionDTO> origin;
};

bool matches(const ExecutionProduct &current, const Selection &selected) {
  return current.bundle == selected.bundle &&
         current.program == selected.program &&
         current.program_binding == selected.binding &&
         current.io_signal_service == selected.service &&
         current.publication_generation == selected.publication &&
         current.origin_named_apply == selected.origin;
}

core::ErrorDTO unavailable(std::string reason) {
  return {"program_applied_selection_unavailable",
          "The actual Applied Program source context is unavailable",
          {{"reason", std::move(reason)}}};
}
} // namespace

core::Result<std::size_t>
CompiledProgramBlackboardKeyCapacity(const ExecutionProduct &composition) {
  using Reply = core::Result<std::size_t>;
  if (!composition.program || !composition.program_binding)
    return Reply::Ok(0U);
  auto program = deployment::compile_program_io_conditions(
      *composition.program, *composition.program_binding, *composition.bundle);
  if (!program.ok())
    return Reply::Err(program.error());
  auto policies = deployment::compile_retained_io_policies(
      *composition.program, *composition.program_binding, *composition.bundle);
  if (!policies.ok())
    return Reply::Err(policies.error());
  std::map<std::string, core::IoConditionValueKind> keys;
  const auto append = [&](const auto &rows) {
    for (const auto &row : rows) {
      const auto [found, inserted] = keys.emplace(row.name, row.value_kind);
      if (!inserted && found->second != row.value_kind)
        return false;
    }
    return true;
  };
  if (!append(program.value().referenced_program_blackboard_keys) ||
      !append(policies.value().referenced_deployment_blackboard_keys))
    return Reply::Err({"invalid_request",
                       "Compiled blackboard key types conflict",
                       {{"reason", "program_bb_required_key_type_mismatch"}}});
  return Reply::Ok(keys.size());
}

core::Result<void> EnsureAppliedProgramBlackboardContext(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const ApplyOutcomeLookup &apply_outcome,
    const core::IoProgramBlackboardScopeDTO &scope) {
  using Reply = core::Result<void>;
  Selection selected;
  {
    std::shared_lock lock(access.mutex);
    const auto *current = access.product();
    if (!current || !current->bundle || !current->program ||
        !current->program_binding || !current->io_signal_service)
      return Reply::Err({"stale_generation",
                         "Applied Program source is not installed",
                         {{"reason", "program_bb_source_not_installed"}}});
    selected = {current->bundle,
                current->program,
                current->program_binding,
                current->io_signal_service,
                current->publication_generation,
                current->origin_named_apply};
    const auto context = kernel.applied_program_blackboard_context();
    if (context &&
        context->active_publication_generation == selected.publication &&
        context->program_binding_id == selected.binding->program_binding_id() &&
        context->retained_bundle_id == selected.bundle->retained_bundle_id() &&
        context->graph_revision ==
            selected.bundle->value().cell_graph.graph_revision)
      return Reply::Ok();
    // A pending replacement is never a retry hint for a foreign request.
    // Exact Runner/cycle/held-Run proof additionally remains Core-owned.
    if (scope.program_binding_id != selected.binding->program_binding_id() ||
        scope.active_publication_generation != selected.publication)
      return Reply::Err(stale());
  }
  // The product's Apply succeeded, failed or is pending, as its claim release
  // stated; only a named Apply's product has a Program source.
  if (!selected.origin)
    return Reply::Err(unavailable("program_bb_applied_program_unbound"));
  const auto succeeded = apply_outcome(*selected.origin);
  if (!succeeded) {
    std::shared_lock lock(access.mutex);
    if (!access.product() || !matches(*access.product(), selected))
      return Reply::Err(stale());
    const auto current_scope = kernel.ValidatePendingProgramBlackboardScope(
        scope, selected.binding->value(), selected.publication,
        selected.service);
    if (!current_scope.ok())
      return current_scope;
    return Reply::Err({"bb_source_not_ready",
                       "The current NamedApply is still installing",
                       {{"reason", "program_bb_named_apply_pending"}}});
  }
  if (!*succeeded)
    return Reply::Err(unavailable("program_bb_applied_record_unavailable"));
  // Converge the same exact successful global tuple before strict BB source
  // installation; a failed activation remains an ordinary error, never an ACK
  // or an installation-pending retry hint.
  const auto activated = EnsureAppliedGlobalIoRuleCatalog(access, kernel);
  if (!activated.ok())
    return activated;
  auto context = detail::CompileAppliedProgramBlackboardContext(
      {selected.publication, selected.program->value().program_id,
       selected.program->value().program_revision,
       selected.binding->program_binding_id()},
      selected.program, selected.binding, *selected.bundle);
  if (!context.ok())
    return Reply::Err(context.error());
  std::shared_lock lock(access.mutex);
  if (!access.product() || !matches(*access.product(), selected))
    return Reply::Err(stale());
  return kernel.InstallAppliedProgramBlackboardContext(
      std::move(context.value()), selected.service);
}
} // namespace manyforge::roboplan_backend::host_detail
