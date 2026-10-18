#pragma once

#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_value.hpp>

#include "activation_model_source_closure_internal.hpp"

namespace manyforge::deployment::detail {

struct ActivationDeploymentComponent final {
  std::string component_id;
  std::string component_profile_id;
  std::optional<CanonicalValue> root_pose_in_cell;
  CanonicalValue target_settings;
  CanonicalValue configuration;
  std::optional<CapturedComponentModelSource> model_source;
  std::string frame_prefix{};
};

struct ActivationDeploymentIntent final {
  std::string deployment_artifact_id;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> assistant_profile_id;
  std::optional<std::string> isaac_world;
  CanonicalValue attachments;
  CanonicalValue policies;
  std::optional<CanonicalValue> compatibility;
  std::vector<ActivationDeploymentComponent> components;
  CanonicalValue normalized_root;
};

/// Parse one normalized authored Deployment intent and join it exactly to the
/// source bytes captured for the same components.
///
/// The caller has no path-based fallback: every physical component must have
/// exactly one captured source row and every captured row must be consumed.
/// Model-free provider components carry neither a model source nor a pose.
[[nodiscard]] ActivationDeploymentIntent parse_activation_deployment_intent(
    const CanonicalValue &value,
    std::vector<CapturedComponentModelSource> captured_sources);

} // namespace manyforge::deployment::detail
