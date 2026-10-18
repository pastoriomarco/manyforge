#pragma once

#include "activation_component_profile_internal.hpp"
#include <manyforge_deployment/component_type_codec.hpp>

namespace manyforge::deployment::detail {

/// Actual closed-schema-qualified catalog artifact derived from one captured
/// model-free configuration. No model or live lifecycle authority is implied.
struct ActivationPreparedIoType final {
  std::string component_id;
  QualifiedComponentType type;
  std::string catalog_bytes;
  CanonicalValue normalized_configuration;
};

[[nodiscard]] std::vector<ActivationPreparedIoType> prepare_activation_io_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities);

} // namespace manyforge::deployment::detail
