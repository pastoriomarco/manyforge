#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

inline constexpr std::size_t kMaxArtifactEncodedBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxArtifactNestingDepth = 64U;
inline constexpr std::size_t kMaxArtifactNodeCount = 250000U;
inline constexpr std::size_t kMaxArtifactScalarBytes = 1024U * 1024U;
inline constexpr std::int64_t kMaxArtifactSafeInteger = 9007199254740991LL;

class ArtifactCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Strictly decoded but not yet schema-qualified artifact data.
///
/// The wrapper makes the trust phase explicit: successfully decoding YAML into
/// the JSON data model does not make the payload a valid graph or catalog
/// referent.
class ParsedArtifactCandidate final {
 public:
  [[nodiscard]] const CanonicalValue& root() const noexcept { return root_; }

 private:
  explicit ParsedArtifactCandidate(CanonicalValue root) : root_(std::move(root)) {}

  CanonicalValue root_;

  friend ParsedArtifactCandidate parse_strict_json_model_yaml(std::string_view);
};

/// Decodes exactly one UTF-8 YAML document into the closed I-JSON value model.
///
/// Duplicate keys, non-string keys, aliases, anchors, merge keys, explicit
/// tags, directives, non-JSON/non-finite numeric values, negative zero,
/// unsafe integer literals, invalid UTF-8, non-NFC text, and inputs exceeding
/// the fixed v1 byte/depth/node/scalar bounds are rejected.
[[nodiscard]] ParsedArtifactCandidate parse_strict_json_model_yaml(std::string_view yaml_bytes);

}  // namespace manyforge::deployment
