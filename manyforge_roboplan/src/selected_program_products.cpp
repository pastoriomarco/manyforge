#include "selected_program_products.hpp"

#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <stdexcept>

namespace manyforge::roboplan_backend::detail {

core::Result<SelectedProgramProducts> ReadSelectedProgramProducts(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &origin,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root) {
  return ReadSelectedProgramProducts(manager_state_root, cell_id, origin, bundle,
                                     &model_map, package_root, program_artifact_root);
}

core::Result<SelectedProgramProducts> ReadSelectedProgramProducts(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &origin,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap *model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root) {
  using Reply = core::Result<SelectedProgramProducts>;
  const auto rejected = [](const char *reason) {
    return Reply::Err({"program_applied_selection_unavailable",
                       "The selected Program source products are unavailable",
                       {{"reason", reason}}});
  };
  if (!origin.valid() || manager_state_root.empty() || package_root.empty() ||
      bundle.value().cell_graph.cell_id != cell_id ||
      (model_map && model_map->query().retained_bundle_id != bundle.retained_bundle_id()))
    return rejected("program_selected_product_mismatch");
  const auto observed = component_manager::ReadManagerApplyStatusReadOnly(
      manager_state_root /
          std::string(component_manager::kManagerApplyJournalFilename),
      cell_id, origin);
  if (!observed.ok())
    return Reply::Err(observed.error());
  const auto &intent = observed.value().intent.intent;
  if (intent.cell_id != cell_id || intent.apply_id != origin.apply_id ||
      intent.apply_generation != origin.apply_generation ||
      intent.candidate_retained_bundle_id != bundle.retained_bundle_id() ||
      intent.candidate_graph_revision !=
          bundle.value().cell_graph.graph_revision)
    return rejected("program_selected_product_mismatch");
  try {
    const auto receipt = content_identity::parse_canonical_json(
        intent.prepared_activation.canonical_receipt_json);
    const auto text = [&](const char *key) -> const std::string & {
      const auto *value = receipt.find_object_member(key);
      if (!value || !value->is_string() || value->as_string().empty())
        throw std::invalid_argument("selected Program field is invalid");
      return value->as_string();
    };
    if (text("schema_version") !=
            deployment::kPreparedActivationInputSchemaVersion ||
        text("retained_bundle_id") != bundle.retained_bundle_id().value() ||
        text("graph_revision") !=
            bundle.value().cell_graph.graph_revision.value())
      return rejected("program_selected_record_invalid");
    const auto outcome =
        deployment::parse_prepared_activation_program_outcome(receipt);
    if (!outcome.ok() || outcome.value().program_binding_id !=
                             intent.prepared_activation.program_binding_id)
      return rejected("program_selected_record_invalid");
    if (!outcome.value().program_binding_id)
      return Reply::Ok({nullptr, nullptr, outcome.value()});
    const auto &catalog_root = program_artifact_root.empty() ? package_root : program_artifact_root;
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog(catalog_root);
    if (!model_map)
      return rejected("program_selected_products_unavailable");
    const core::ProgramId id(text("program_id"));
    const core::ProgramRevision revision(text("program_revision"));
    SelectedProgramProducts products;
    const auto read = [&](const std::filesystem::path &root) {
      const deployment::FilesystemVerifiedProgramBindingSource bindings(root);
      const deployment::FilesystemVerifiedProgramSource programs(root);
      if (!products.binding)
        products.binding = bindings.get_verified(
            *outcome.value().program_binding_id, bundle, *model_map, catalog);
      if (!products.program)
        products.program = programs.get_verified(id, revision, bundle);
    };
    if (!program_artifact_root.empty() && program_artifact_root != package_root)
      read(program_artifact_root);
    if (!products.binding || !products.program)
      read(package_root);
    if (!products.binding || !products.program ||
        products.binding->value().program_id != id ||
        products.binding->value().program_revision != revision ||
        products.binding->value().node_environment !=
            products.program->value().node_environment)
      return rejected("program_selected_products_unavailable");
    products.outcome = outcome.value();
    return Reply::Ok(std::move(products));
  } catch (const std::exception &) {
    return rejected("program_selected_record_invalid");
  }
}

} // namespace manyforge::roboplan_backend::detail
