#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_value.hpp>

#include "activation_deployment_intent_internal.hpp"

namespace manyforge::deployment::detail {

enum class ActivationRuntimeKind {
  kRos2Control = 1,
  kIoProvider = 2,
  kPoseSource = 3,
  kAiModel = 4
};

/// I/O devices, pose sources and AI models select no model and no physical
/// type.
[[nodiscard]] constexpr bool
is_model_free_runtime_kind(ActivationRuntimeKind kind) noexcept {
  return kind == ActivationRuntimeKind::kIoProvider ||
         kind == ActivationRuntimeKind::kPoseSource ||
         kind == ActivationRuntimeKind::kAiModel;
}

/// Exact repository-owned semantics selected by one authored component.
///
/// The authored intent owns instance configuration.  This value supplies only
/// the stable profile/family facts needed to compile that configuration; it is
/// not a saved Deployment, catalog selector, or compatibility fallback.
struct ActivationComponentProfile final {
  std::string component_id;
  std::string component_profile_id;
  // Model identity fields are empty and model_family is null for model-free
  // kinds (kIoProvider, kPoseSource, kAiModel); they never select a model or physical
  // component type.
  std::string model_family_id;
  std::string model_asset_id;
  std::string accepted_package_uri_prefix;
  ActivationRuntimeKind runtime_kind;
  CanonicalValue model_family;
  CanonicalValue runtime_restoration_profile;
};

/// Resolve every authored component profile from the repository-owned profile
/// description and prove that the authored runtime lane matches that profile.
///
/// Results preserve the canonical component order in `intent`.  Unknown
/// profiles, duplicate repository identities, missing families, and lane
/// mismatches fail before model preparation or runtime-graph construction.
[[nodiscard]] std::vector<ActivationComponentProfile>
resolve_activation_component_profiles(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent);

/// The repository-owned assistant profile row the intent selects by
/// `assistant_profile_id` (`maintained_assistant_profiles.v1.json`), with the
/// shared `mode_profiles` its `mode_profile_id` names; none without an
/// assistant profile. Unknown profiles fail.
struct ActivationAssistantProfile final {
  CanonicalValue profile;
  /// The selected mode profile (mode id -> mode), or an empty object.
  CanonicalValue modes;
};
[[nodiscard]] std::optional<ActivationAssistantProfile>
resolve_activation_assistant_profile(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent);

} // namespace manyforge::deployment::detail
