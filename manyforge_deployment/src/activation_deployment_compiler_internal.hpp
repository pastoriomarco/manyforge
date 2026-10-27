#pragma once

#include <optional>
#include <vector>

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_io_provider_internal.hpp"
#include "activation_pose_source_internal.hpp"
#include "activation_prepared_model_internal.hpp"

namespace manyforge::deployment::detail {

/// Compile one captured authored Deployment into the existing, independently
/// validated runtime Deployment envelope.
///
/// This is the only production authored-intent -> runtime-graph transformer.
/// It consumes exact qualified catalog facts and captured model products; it
/// never selects or reopens a generated Deployment occurrence. A selected
/// assistant profile becomes the `assistant` member; each model alias is
/// served by the AI model component `assistant_model_components` names, else
/// by the only one (typed refusals `assistant_ai_model_component_missing`,
/// `assistant_model_component_unresolved`).
[[nodiscard]] CanonicalValue compile_activation_deployment(
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<core::CatalogRecordRefDTO> &catalog_records,
    const std::vector<QualifiedRuntimeProfile> &runtime_profiles,
    const std::vector<ActivationPreparedIoType> &io_types = {},
    std::span<const QualifiedProviderManifest> provider_manifests = {},
    const std::optional<ActivationAssistantProfile> &assistant_profile =
        std::nullopt);

} // namespace manyforge::deployment::detail
