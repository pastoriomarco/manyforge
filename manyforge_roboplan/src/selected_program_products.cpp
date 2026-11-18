#include "selected_program_products.hpp"

#include <manyforge_deployment/retained_bundle.hpp>
#include <stdexcept>

namespace manyforge::roboplan_backend::detail {

core::Result<SelectedProgramProducts> LoadSelectedProgramProducts(
    const std::optional<component_manager::HostApplyProgramSelection> &program,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root) {
  return LoadSelectedProgramProducts(program, bundle, &model_map, package_root,
                                     program_artifact_root);
}

core::Result<SelectedProgramProducts> LoadSelectedProgramProducts(
    const std::optional<component_manager::HostApplyProgramSelection> &program,
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
  if (package_root.empty() ||
      (model_map && model_map->query().retained_bundle_id != bundle.retained_bundle_id()))
    return rejected("program_selected_product_mismatch");
  if (!program)
    return Reply::Ok({nullptr, nullptr});
  try {
    const auto &catalog_root = program_artifact_root.empty() ? package_root : program_artifact_root;
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog(catalog_root);
    if (!model_map)
      return rejected("program_selected_products_unavailable");
    const auto &id = program->program_id;
    const auto &revision = program->program_revision;
    SelectedProgramProducts products;
    const auto read = [&](const std::filesystem::path &root) {
      const deployment::FilesystemVerifiedProgramBindingSource bindings(root);
      const deployment::FilesystemVerifiedProgramSource programs(root);
      if (!products.binding)
        products.binding = bindings.get_verified(
            program->program_binding_id, bundle, *model_map, catalog);
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
    return Reply::Ok(std::move(products));
  } catch (const std::exception &) {
    return rejected("program_selected_record_invalid");
  }
}

} // namespace manyforge::roboplan_backend::detail
