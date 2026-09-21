#pragma once

#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_core/types/native_projection_handoff.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <vector>

namespace manyforge::component_manager {

/// Joins the active compiler-owned provider/runtime requirements with exact
/// present process epochs directly observed by Component Manager.  A present
/// epoch is sufficient for the scene-owner recovery bootstrap; callers that
/// need admission must still require the enclosing runtime observation to be
/// converged.
[[nodiscard]]
std::vector<manyforge::core::ProjectionNativeRuntimeBindingDTO>
DeriveActiveNativeProjectionRuntimeBindings(
    const manyforge::core::RetainedBundleDTO &active_bundle,
    const RuntimeObservationSnapshot &observations);

/// Adds the scene-owner-issued active revision after a binding-set change.
/// The result is suitable only for the scene owner's active-runtime recovery
/// port; neither helper performs a native projection effect.
[[nodiscard]] manyforge::core::ActiveProjectionRuntimeSetDTO
DeriveActiveNativeProjectionRuntimeSet(
    const manyforge::core::RetainedBundleDTO &active_bundle,
    const manyforge::core::ProjectionRevisionDTO &active_projection_revision,
    const RuntimeObservationSnapshot &observations);

} // namespace manyforge::component_manager
