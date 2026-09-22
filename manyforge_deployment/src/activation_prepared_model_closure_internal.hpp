#pragma once

#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_value.hpp>

#include "activation_model_source_closure_internal.hpp"

namespace manyforge::deployment::detail {

struct PreparedComponentModelSource final {
  std::string component_id;
  std::string model_asset_id;
  std::string accepted_package_uri_prefix;
  CanonicalValue preparation_receipt;
  CapturedModelSourceArtifact urdf;
  std::optional<CapturedModelSourceArtifact> srdf;
};

/// Decode the deterministic Python preparation result carried in the same
/// bounded operation request as its original captured source closure.
[[nodiscard]] std::vector<PreparedComponentModelSource>
parse_activation_prepared_model_closure(const CanonicalValue &value);

} // namespace manyforge::deployment::detail
