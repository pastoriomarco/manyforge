#pragma once

#include "activation_io_provider_internal.hpp"

namespace manyforge::deployment::detail {

/// CP9: one model-free pose-source component (runtime kind `pose_source`)
/// becomes a derived component type with the single target `poses` offering
/// `scene.pose.observe`. It reuses the model-free prepared-type record of the
/// I/O pipeline. The authored component (`pose_source.v2`) is joined with the
/// object links (`policies.scene_pose_sources`) naming it and the
/// `component_streams` budget overrides; `normalized_configuration` holds the
/// expanded `{pose_sources:[row]}`. Typed refusals:
/// `scene_pose_source_unknown_component`, `scene_pose_source_mode_missing`,
/// `scene_pose_source_relative_reference_missing`,
/// `scene_pose_source_relative_requires_simulation`.
[[nodiscard]] std::vector<ActivationPreparedIoType>
prepare_activation_pose_source_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities);

/// One model-free AI model component (runtime kind `ai_model`, profile
/// `ai_model_v0`) becomes a derived component type with the single target
/// `endpoint` offering `ai.model.invoke`; `normalized_configuration` holds
/// `{ai_models:[{controllable_component_id, base_url, model, api_key_env?}]}`
/// (a null `api_key_env` is omitted). Typed refusals:
/// `ai_model_configuration_invalid`, `ai_model_api_key_env_invalid` (not
/// `MANYFORGE_AI_[A-Z0-9_]+`).
[[nodiscard]] std::vector<ActivationPreparedIoType>
prepare_activation_ai_model_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities);

} // namespace manyforge::deployment::detail
