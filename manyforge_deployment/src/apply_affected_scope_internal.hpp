#pragma once

#include <span>

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/retained_product.hpp>

namespace manyforge::deployment::detail {

/// Internal algorithm boundary. Inputs are assumed to be the complete typed
/// values of independently verified retained bundles. Production callers use
/// the sealed public overload; focused tests exercise overlap closure here.
[[nodiscard]] core::ConfigurationFenceScopeDTO
derive_apply_configuration_fence_scope_verified_values(
    const core::RetainedBundleDTO &predecessor,
    const core::RetainedBundleDTO &candidate,
    std::span<const core::ControllableComponentId> actual_restart_components =
        {},
    std::span<const core::RetainedBundleDTO *const> actual_terminal_products =
        {});

} // namespace manyforge::deployment::detail
