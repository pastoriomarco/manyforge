#include "prepared_activation_materialization_internal.hpp"

#include "activation_model_source_closure_internal.hpp"
#include "activation_prepared_model_closure_internal.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {

std::shared_ptr<const PreparedActivationMaterialization>
build_prepared_activation_candidate(
    const std::filesystem::path& capability_root,
    const CanonicalValue& deployment_value,
    const CanonicalValue& model_source_closure,
    const CanonicalValue& prepared_model_closure,
    const core::CellId& runtime_cell_id,
    bool prepare_canonical_empty) {
  auto captured_sources = parse_activation_model_source_closure(model_source_closure);
  auto deployment_intent = parse_activation_deployment_intent(
      deployment_value, std::move(captured_sources));
  const auto component_profiles =
      resolve_activation_component_profiles(capability_root, deployment_intent);
  expand_activation_machine_members(deployment_intent, component_profiles);
  auto prepared_sources = parse_activation_prepared_model_closure(prepared_model_closure);
  std::vector<ActivationPreparedModel> prepared_models;
  try {
    prepared_models = qualify_activation_prepared_models(
        deployment_intent, component_profiles, std::move(prepared_sources));
  } catch (const std::length_error& error) {
    throw std::runtime_error(
        std::string("prepared-model qualification produced an invalid size: ") + error.what());
  }

  return std::make_shared<const PreparedActivationMaterialization>(materialize_prepared_activation(
      capability_root, std::move(prepared_models), deployment_intent, runtime_cell_id,
      component_profiles, prepare_canonical_empty));
}

}  // namespace manyforge::deployment::detail
