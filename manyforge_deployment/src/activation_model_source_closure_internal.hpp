#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment::detail {

struct CapturedModelSourceArtifact final {
  std::string logical_uri;
  std::string media_type;
  core::ContentDigest content_digest;
  std::string bytes;
};

struct CapturedComponentModelSource final {
  std::string component_id;
  CapturedModelSourceArtifact urdf;
  std::optional<CapturedModelSourceArtifact> srdf;
  std::vector<CapturedModelSourceArtifact> resources;
};

/// Decode and verify the exact source closure captured by Composer.
///
/// This boundary owns transfer validation only. It neither reads source paths
/// nor assigns model/profile semantics. Returned bytes are the sole source
/// bytes available to activation preparation.
[[nodiscard]] std::vector<CapturedComponentModelSource>
parse_activation_model_source_closure(const CanonicalValue &value);

/// Decode one gzip/base64 artifact using the same bounded operation-wire
/// contract as the captured source closure.
[[nodiscard]] CapturedModelSourceArtifact
parse_activation_model_source_transfer_artifact(
    const CanonicalValue &value, std::string logical_uri,
    std::uint64_t maximum_bytes, std::string_view label);

} // namespace manyforge::deployment::detail
