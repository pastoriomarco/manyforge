#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
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
/// `assistant_model_component_unresolved`). Each physical component's graph
/// row carries its default TCP (`compiled_default_tcp`, read from its
/// qualified type in `component_types`); an authored mount's `primary`
/// carries over.
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
        std::nullopt,
    std::span<const QualifiedComponentType> component_types = {});

/// The default TCP compiled into a component's graph row. Its
/// TCPs are `{target, role}` pairs: on each target that declares a default
/// TCP, the roles the type tags as TCPs and the row's custom TCPs (a merely
/// named `tcp` is no TCP). An authored `default_tcp` must be one of
/// them (`component_default_tcp_unknown`). Without one, the single target
/// that declares a default gives it; TCPs on several such targets refuse
/// with `component_default_tcp_required`. None when the component has no TCP.
[[nodiscard]] std::optional<core::ComponentDefaultTcpDTO> compiled_default_tcp(
    const ActivationDeploymentComponent &component,
    const core::ComponentTypeDTO &type);

} // namespace manyforge::deployment::detail
