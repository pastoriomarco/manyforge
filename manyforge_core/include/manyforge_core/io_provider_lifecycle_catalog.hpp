#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/provider_manifest.hpp>

namespace manyforge::core {

inline constexpr std::string_view kIoProviderLifecycleManifestId = "manyforge.provider.io_signals";
/// CP9: model-free pose-source components (runtime kind `pose_source`) use
/// the same four-intent lifecycle plans under their own provider manifest.
inline constexpr std::string_view kPoseSourceLifecycleManifestId = "manyforge.provider.pose_source";
/// Model-free AI model components (runtime kind `ai_model`): the same plans.
inline constexpr std::string_view kAiModelLifecycleManifestId = "manyforge.provider.ai_model";

/// The catalog declares the configured-device limit policy. Admission binds
/// its actual duration from the exact sealed provider domain. Input quality
/// is independent of the driver's resources, connection and enable gate.
[[nodiscard]] Result<std::vector<LifecycleIntentPlanDTO>> MakeIoProviderLifecyclePlans();

/// Accepts the I/O, pose-source and AI model manifests (same plans).
[[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> MakeIoProviderLifecycleCatalog(
    const ProviderManifestDTO& manifest);

}  // namespace manyforge::core
