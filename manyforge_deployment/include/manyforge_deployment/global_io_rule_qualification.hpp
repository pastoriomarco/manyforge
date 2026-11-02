#pragma once

#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>
#include <span>

namespace manyforge::deployment {

/// Raw, sealed static-chain dependencies for actual global entry selections.
/// No runtime route/currentness or Stop permission is inferred.
[[nodiscard]] core::Result<std::vector<core::RetainedArtifactRefDTO>>
required_global_io_stop_artifacts(const RetainedIoPolicyProjection &rules,
                                  const RetainedBundle &bundle);

/// Final Program-dependent selection proof before emitting or independently
/// reloading actual Prepared Apply. Reuses the same qualified chain reader and
/// Core entry/floor checker as node rules; no desired-trip outcome is added.
[[nodiscard]] core::Result<void> validate_global_io_stop_entries(
    const RetainedIoPolicyProjection &rules, const RetainedBundle &bundle,
    const VerifiedRetainedModelMap *model_map,
    std::span<const PreparedActivationArtifact> raw_artifacts);

} // namespace manyforge::deployment
