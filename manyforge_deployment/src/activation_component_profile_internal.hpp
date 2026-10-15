#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_value.hpp>

#include "activation_deployment_intent_internal.hpp"

namespace manyforge::deployment::detail {

enum class ActivationRuntimeKind { kRos2Control = 1 };

/// Exact repository-owned semantics selected by one authored component.
///
/// The authored intent owns instance configuration.  This value supplies only
/// the stable profile/family facts needed to compile that configuration; it is
/// not a saved Deployment, catalog selector, or compatibility fallback.
struct ActivationComponentProfile final {
  std::string component_id;
  std::string component_profile_id;
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

} // namespace manyforge::deployment::detail
