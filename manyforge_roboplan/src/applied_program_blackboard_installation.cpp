#include "applied_program_blackboard_installation.hpp"
#include "applied_global_io_rule_installation.hpp"
#include "applied_program_owner_reader.hpp"

#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
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

// Only the exact actual candidate's immutable original NamedApply provenance
// can qualify this transient. Generation mismatch or a held fence alone cannot.
bool original_named_apply_pending(const Selection &selected,
                                  const std::filesystem::path &root,
                                  const core::CellId &cell) {
  if (!selected.origin || !selected.origin->valid())
    return false;
  const auto status = component_manager::ReadManagerApplyStatusReadOnly(
      root / std::string(component_manager::kManagerApplyJournalFilename), cell,
      *selected.origin);
  if (!status.ok() || status.value().terminal_outcome)
    return false;
  const auto &intent = status.value().intent.intent;
  if (intent.apply_id != selected.origin->apply_id ||
      intent.apply_generation != selected.origin->apply_generation ||
      intent.candidate_retained_bundle_id !=
          selected.bundle->retained_bundle_id() ||
      intent.candidate_graph_revision !=
          selected.bundle->value().cell_graph.graph_revision)
    return false;
  // Corruption or an unrelated head is never an installation-pending hint.
  const auto pointer = component_manager::ReadActiveBundlePublicationReadOnly(
      root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      cell);
  if (!pointer.ok())
    return false;
  const auto &head = pointer.value();
  const auto *published_origin =
      std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &head.request.transaction_ref);
  const bool candidate_head =
      published_origin && *published_origin == *selected.origin &&
      head.publication_generation == selected.publication &&
      head.request.candidate_retained_bundle_id ==
          selected.bundle->retained_bundle_id() &&
      head.request.candidate_graph_revision ==
          selected.bundle->value().cell_graph.graph_revision &&
      (!status.value().publication_receipt ||
       status.value().publication_receipt->publication.publication_receipt ==
           head);
  bool predecessor_head = false;
  if (!status.value().publication_receipt &&
      status.value().runtime_convergence_intent &&
      status.value().fence_acceptance) {
    const auto &record = *status.value().runtime_convergence_intent;
    const auto &convergence = record.runtime_convergence_intent;
    predecessor_head =
        record.intent_digest == status.value().intent.intent_digest &&
        convergence.valid() &&
        convergence.apply_id == selected.origin->apply_id &&
        convergence.apply_generation == selected.origin->apply_generation &&
        convergence.candidate_retained_bundle_id ==
            selected.bundle->retained_bundle_id() &&
        convergence.candidate_graph_revision ==
            selected.bundle->value().cell_graph.graph_revision &&
        convergence.fence_acceptance_digest ==
            status.value().fence_acceptance->acceptance_digest &&
        convergence.predecessor_publication_generation ==
            head.publication_generation &&
        convergence.predecessor_public_pointer_digest ==
            head.durable_public_pointer_content_digest &&
        head.request.candidate_retained_bundle_id ==
            intent.expected_active_projection_revision.retained_bundle_id &&
        head.request.candidate_graph_revision ==
            intent.expected_active_projection_revision.graph_revision &&
        head.publication_generation.value() <
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) &&
        selected.publication.value() ==
            head.publication_generation.value() + 1U;
  }
  if (!candidate_head && !predecessor_head)
    return false;
  try {
    const auto prepared = content_identity::parse_canonical_json(
        intent.prepared_activation.canonical_receipt_json);
    const auto outcome =
        deployment::parse_prepared_activation_program_outcome(prepared);
    if (!outcome.ok() || outcome.value().non_executable ||
        outcome.value().program_binding_id !=
            selected.binding->program_binding_id() ||
        intent.prepared_activation.program_binding_id !=
            selected.binding->program_binding_id())
      return false;
    const auto text_matches = [&](const char *name,
                                  const std::string &expected) {
      const auto *value = prepared.find_object_member(name);
      return value && value->is_string() && value->as_string() == expected;
    };
    return text_matches("program_id",
                        selected.program->value().program_id.value()) &&
           text_matches("program_revision",
                        selected.program->value().program_revision.value()) &&
           text_matches("retained_bundle_id",
                        selected.bundle->retained_bundle_id().value()) &&
           text_matches(
               "graph_revision",
               selected.bundle->value().cell_graph.graph_revision.value());
  } catch (const std::exception &) {
    return false;
  }
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
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
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
        context->request_owner_epoch == kernel.request_owner_epoch() &&
        context->active_publication_generation == selected.publication &&
        context->program_binding_id == selected.binding->program_binding_id() &&
        context->retained_bundle_id == selected.bundle->retained_bundle_id() &&
        context->graph_revision ==
            selected.bundle->value().cell_graph.graph_revision)
      return Reply::Ok();
    // A pending replacement is never a retry hint for a foreign request.
    // Exact Runner/cycle/held-Run proof additionally remains Core-owned.
    if (scope.request_owner_epoch != kernel.request_owner_epoch() ||
        scope.program_binding_id != selected.binding->program_binding_id() ||
        scope.active_publication_generation != selected.publication)
      return Reply::Err(stale());
  }
  const auto observed = detail::ReadAppliedProgramOwnerReadOnly(
      manager_state_root, cell_id, selected.publication);
  if (!observed.ok()) {
    const auto reason = observed.error().details.find("reason");
    const bool expected_pending_read =
        reason != observed.error().details.end() &&
        (reason->second == "program_bb_applied_publication_stale" ||
         reason->second == "program_bb_applied_record_unavailable");
    if (expected_pending_read &&
        original_named_apply_pending(selected, manager_state_root, cell_id)) {
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
    return Reply::Err(observed.error());
  }
  // Succeeded is durable before the Manager's subsequent claim-release RPC.
  // Converge that same exact successful global tuple before strict BB source
  // installation; a failed activation remains an ordinary error, never an ACK
  // or an installation-pending retry hint.
  const auto activated = EnsureAppliedGlobalIoRuleCatalog(
      access, kernel, manager_state_root, cell_id);
  if (!activated.ok())
    return activated;
  auto context = detail::CompileAppliedProgramBlackboardContext(
      observed.value(), kernel.request_owner_epoch(), selected.program,
      selected.binding, *selected.bundle);
  if (!context.ok())
    return Reply::Err(context.error());
  std::shared_lock lock(access.mutex);
  if (!access.product() || !matches(*access.product(), selected))
    return Reply::Err(stale());
  return kernel.InstallAppliedProgramBlackboardContext(
      std::move(context.value()), selected.service);
}
} // namespace manyforge::roboplan_backend::host_detail
