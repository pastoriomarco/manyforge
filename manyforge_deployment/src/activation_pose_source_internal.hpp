#pragma once

#include "activation_io_provider_internal.hpp"

namespace manyforge::deployment::detail {

/// CP9: one model-free pose-source component (runtime kind `pose_source`)
/// becomes a derived component type with the single target `poses` offering
/// `scene.pose.observe`. It reuses the model-free prepared-type record of the
/// I/O pipeline; `normalized_configuration` holds `{pose_sources:[row]}`.
[[nodiscard]] std::vector<ActivationPreparedIoType>
prepare_activation_pose_source_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities);

} // namespace manyforge::deployment::detail
