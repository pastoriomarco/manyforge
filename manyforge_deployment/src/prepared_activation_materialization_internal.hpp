#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/scenario_program_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "activation_prepared_model_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_component_profile_internal.hpp"

namespace manyforge::deployment {
class RetainedBundle;

namespace detail {

using PreparedActivationArtifact = ::manyforge::deployment::PreparedActivationArtifact;

struct PreparedActivationMaterialization final {
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  ScenarioDeploymentCompatibilityDTO deployment_compatibility;
  std::shared_ptr<const RetainedBundle> bundle;
  std::optional<VerifiedRetainedModelMap> model_map;
  std::shared_ptr<const RetainedBundle> canonical_empty_bundle;
  std::vector<PreparedActivationArtifact> raw_artifacts;
  std::vector<PreparedActivationArtifact> catalog_artifacts;
};

[[nodiscard]] std::shared_ptr<const PreparedActivationMaterialization>
build_prepared_activation_candidate(
    const std::filesystem::path &capability_root,
    const CanonicalValue &deployment_value,
    const CanonicalValue &model_source_closure,
    const CanonicalValue &prepared_model_closure,
    const core::CellId &runtime_cell_id,
    bool prepare_canonical_empty = false);

[[nodiscard]] PreparedActivationMaterialization materialize_prepared_activation(
    const std::filesystem::path &capability_root,
    std::vector<ActivationPreparedModel> prepared_models,
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    bool prepare_canonical_empty = false);


} // namespace detail
} // namespace manyforge::deployment
