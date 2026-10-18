#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/provider_manifest.hpp>

namespace manyforge::core {

inline constexpr std::string_view kIoProviderLifecycleManifestId = "manyforge.provider.io_signals";

/// The catalog declares the configured-device limit policy. Admission binds
/// its actual duration from the exact sealed provider domain. Input quality
/// is independent of the driver's resources, connection and enable gate.
[[nodiscard]] Result<std::vector<LifecycleIntentPlanDTO>> MakeIoProviderLifecyclePlans();

[[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> MakeIoProviderLifecycleCatalog(
    const ProviderManifestDTO& manifest);

}  // namespace manyforge::core
