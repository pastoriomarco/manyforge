#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/scenario_program_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "activation_prepared_model_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_component_profile_internal.hpp"

namespace manyforge::deployment {
class RetainedBundle;

namespace detail {

struct PreparedActivationArtifact final {
  core::ContentAddressedStoreKey content_addressed_store_key;
  core::ContentDigest content_digest;
  std::string bytes;
};

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

[[nodiscard]] PreparedActivationMaterialization materialize_prepared_activation(
    const std::filesystem::path &capability_root,
    std::vector<ActivationPreparedModel> prepared_models,
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    bool prepare_canonical_empty = false);

int run_maintained_example_qualification(int argc, char **argv);

} // namespace detail
} // namespace manyforge::deployment
