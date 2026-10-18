#include "applied_program_owner_reader.hpp"

#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>
#include <map>
#include <stdexcept>
#include <string_view>

namespace manyforge::roboplan_backend::detail {

namespace {
template <class Qualify>
core::Result<AppliedOwnerSnapshot> ReadAppliedOwnerProof(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    core::ActiveBundlePublicationGeneration expected_publication_generation,
    Qualify qualify) {
  using Reply = core::Result<AppliedOwnerSnapshot>;
  const auto rejected = [](std::string reason) {
    return Reply::Err(
        {"program_applied_selection_unavailable",
         "The actual Applied Program source context is unavailable",
         {{"reason", std::move(reason)}}});
  };
  if (manager_state_root.empty() || !expected_publication_generation.nonzero())
    return rejected("program_bb_applied_publication_stale");
  try {
    const auto pointer_path =
        manager_state_root /
        std::string(component_manager::kActiveBundlePublicationStoreFilename);
    const auto publication =
        component_manager::ReadActiveBundlePublicationReadOnly(pointer_path,
                                                               cell_id);
    if (!publication.ok() || publication.value().publication_generation !=
                                 expected_publication_generation)
      return rejected("program_bb_applied_publication_stale");
    const auto &receipt = publication.value();
    const auto *apply = std::get_if<core::NamedApplyPublicationTransactionDTO>(
        &receipt.request.transaction_ref);
    if (!apply)
      return rejected("program_bb_applied_program_unbound");
    const auto status = component_manager::ReadManagerApplyStatusReadOnly(
        manager_state_root /
            std::string(component_manager::kManagerApplyJournalFilename),
        cell_id, *apply);
    if (!status.ok() || !status.value().terminal_outcome ||
        !status.value().terminal_outcome->terminal_outcome.succeeded())
      return rejected("program_bb_applied_record_unavailable");
    const auto &record = status.value();
    const auto &intent = record.intent.intent;
    const auto &terminal = record.terminal_outcome->terminal_outcome;
    if (!record.publication_receipt || !record.fence_released ||
        !terminal.valid() || terminal.apply_id != apply->apply_id ||
        terminal.apply_generation != apply->apply_generation ||
        terminal.candidate_projection_revision !=
            receipt.request.candidate_projection_revision ||
        terminal.publication_receipt_digest !=
            record.publication_receipt->publication_receipt_digest ||
        terminal.fence_released_digest !=
            record.fence_released->fence_released_digest ||
        record.publication_receipt->publication.publication_receipt !=
            receipt ||
        intent.candidate_retained_bundle_id !=
            receipt.request.candidate_retained_bundle_id ||
        intent.candidate_graph_revision !=
            receipt.request.candidate_graph_revision)
      return rejected("program_bb_applied_record_invalid");
    const auto prepared = content_identity::parse_canonical_json(
        intent.prepared_activation.canonical_receipt_json);
    const auto text = [&](std::string_view name) -> std::string {
      const auto *value = prepared.find_object_member(name);
      if (!value || !value->is_string() || value->as_string().empty())
        throw std::invalid_argument("Applied receipt field is invalid");
      return value->as_string();
    };
    if (text("schema_version") !=
            deployment::kPreparedActivationInputSchemaVersion ||
        text("retained_bundle_id") !=
            receipt.request.candidate_retained_bundle_id.value() ||
        text("graph_revision") !=
            receipt.request.candidate_graph_revision.value())
      return rejected("program_bb_applied_record_invalid");
    const auto qualified = qualify(prepared, intent.prepared_activation);
    if (!qualified.ok())
      return Reply::Err(qualified.error());
    AppliedOwnerSnapshot result{
        receipt, record.intent.intent_digest,
        record.terminal_outcome->terminal_outcome_digest};
    // A changed pointer is a stale observation. This is not an atomic install
    // fence: callers must still perform the actual owner/currentness join.
    const auto after = component_manager::ReadActiveBundlePublicationReadOnly(
        pointer_path, cell_id);
    if (!after.ok() || after.value() != receipt)
      return rejected("program_bb_applied_publication_stale");
    return Reply::Ok(std::move(result));
  } catch (const std::exception &) {
    return rejected("program_bb_applied_record_invalid");
  }
}
} // namespace

core::Result<AppliedOwnerSnapshot> ReadAppliedOwnerReadOnly(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    core::ActiveBundlePublicationGeneration expected_publication_generation) {
  return ReadAppliedOwnerProof(
      manager_state_root, cell_id, expected_publication_generation,
      [](const auto &, const auto &) { return core::Result<void>::Ok(); });
}

core::Result<AppliedProgramOwnerSnapshot> ReadAppliedProgramOwnerReadOnly(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    core::ActiveBundlePublicationGeneration expected_publication_generation) {
  std::optional<core::ProgramId> program_id;
  std::optional<core::ProgramRevision> program_revision;
  std::optional<core::ProgramBindingId> binding;
  const auto actual = ReadAppliedOwnerProof(
      manager_state_root, cell_id, expected_publication_generation,
      [&](const auto &prepared, const auto &selected) {
        const auto rejected = [](std::string reason) {
          return core::Result<void>::Err(
              {"program_applied_selection_unavailable",
               "The actual Applied Program source context is unavailable",
               {{"reason", std::move(reason)}}});
        };
        const auto outcome =
            deployment::parse_prepared_activation_program_outcome(prepared);
        if (!outcome.ok())
          return rejected("program_bb_applied_record_invalid");
        if (!outcome.value().program_binding_id ||
            outcome.value().non_executable)
          return rejected("program_bb_applied_program_unbound");
        if (outcome.value().program_binding_id != selected.program_binding_id)
          return rejected("program_bb_applied_record_invalid");
        const auto text = [&](std::string_view name) {
          const auto *value = prepared.find_object_member(name);
          if (!value || !value->is_string() || value->as_string().empty())
            throw std::invalid_argument("Applied receipt field is invalid");
          return value->as_string();
        };
        program_id = core::ProgramId(text("program_id"));
        program_revision = core::ProgramRevision(text("program_revision"));
        binding = *outcome.value().program_binding_id;
        return core::Result<void>::Ok();
      });
  if (!actual.ok())
    return core::Result<AppliedProgramOwnerSnapshot>::Err(actual.error());
  return core::Result<AppliedProgramOwnerSnapshot>::Ok(
      {actual.value().publication, actual.value().apply_intent_digest,
       actual.value().terminal_outcome_digest, *program_id, *program_revision,
       *binding});
}

core::Result<core::AppliedProgramBlackboardContextDTO>
CompileAppliedProgramBlackboardContext(
    const AppliedProgramOwnerSnapshot &applied,
    const core::RequestOwnerEpoch &request_owner_epoch,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle) {
  using Reply = core::Result<core::AppliedProgramBlackboardContextDTO>;
  const auto rejected = [](std::string reason) {
    return Reply::Err(
        {"program_applied_selection_unavailable",
         "The actual Applied Program differs from its sealed source products",
         {{"reason", std::move(reason)}}});
  };
  if (request_owner_epoch.empty() || !applied.publication.valid() ||
      !std::holds_alternative<core::NamedApplyPublicationTransactionDTO>(
          applied.publication.request.transaction_ref) ||
      !program || !binding ||
      program->value().program_id != applied.program_id ||
      program->value().program_revision != applied.program_revision ||
      binding->program_binding_id() != applied.program_binding_id ||
      binding->value().program_id != applied.program_id ||
      binding->value().program_revision != applied.program_revision ||
      binding->value().retained_bundle_id != bundle.retained_bundle_id() ||
      binding->value().graph_revision !=
          bundle.value().cell_graph.graph_revision ||
      applied.publication.request.candidate_retained_bundle_id !=
          bundle.retained_bundle_id() ||
      applied.publication.request.candidate_graph_revision !=
          bundle.value().cell_graph.graph_revision)
    return rejected("program_bb_applied_product_stale");
  auto conditions =
      deployment::compile_program_io_conditions(*program, *binding, bundle);
  if (!conditions.ok())
    return Reply::Err(conditions.error());
  auto policies =
      deployment::compile_retained_io_policies(*program, *binding, bundle);
  if (!policies.ok())
    return Reply::Err(policies.error());
  std::map<std::string, core::IoConditionValueKind> keys;
  const auto append = [&](const auto &input) {
    for (const auto &key : input) {
      const auto [found, inserted] = keys.emplace(key.name, key.value_kind);
      if (!inserted && found->second != key.value_kind)
        return false;
    }
    return true;
  };
  if (!append(conditions.value().referenced_program_blackboard_keys) ||
      !append(policies.value().referenced_deployment_blackboard_keys))
    return rejected("program_bb_required_key_type_mismatch");
  std::vector<core::IoBlackboardKeyDescriptorDTO> required;
  required.reserve(keys.size());
  for (const auto &[name, kind] : keys)
    required.push_back({name, kind});
  const auto actual_binding = std::shared_ptr<const core::ProgramBindingDTO>(
      binding, &binding->value());
  const auto declarations = std::shared_ptr<
      const std::vector<core::ProgramBlackboardKeyDeclarationDTO>>(
      program, &program->blackboard_key_declarations());
  return Reply::Ok(
      {request_owner_epoch, applied.publication.publication_generation,
       applied.program_id, applied.program_revision, applied.program_binding_id,
       bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
       actual_binding, declarations,
       std::make_shared<const core::ProgramIoConditionProjectionDTO>(
           std::move(conditions.value())),
       std::move(required), core::kMaximumCellAdmissionJsonRequestBytes});
}

} // namespace manyforge::roboplan_backend::detail
