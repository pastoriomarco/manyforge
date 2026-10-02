#pragma once

#include <vector>

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_prepared_model_internal.hpp"

namespace manyforge::deployment::detail {

/// Compile one captured authored Deployment into the existing, independently
/// validated runtime Deployment envelope.
///
/// This is the only production authored-intent -> runtime-graph transformer.
/// It consumes exact qualified catalog facts and captured model products; it
/// never selects or reopens a generated Deployment occurrence.
[[nodiscard]] CanonicalValue compile_activation_deployment(
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<core::CatalogRecordRefDTO> &catalog_records,
    const std::vector<QualifiedRuntimeProfile> &runtime_profiles);

} // namespace manyforge::deployment::detail
