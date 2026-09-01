#pragma once

#include <cstdint>
#include <filesystem>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace manyforge::headless {

inline constexpr std::string_view kSourceModelProjectionApiVersion =
    "manyforge.source_model_projection.v2";
inline constexpr std::uint64_t kMaximumSourceModelProjectionBytes = 64ULL * 1024ULL * 1024ULL;

class SourceModelProjectionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Verified authoring projection of one exact retained model namespace.
///
/// The projection exposes source-local URDF/SRDF semantics and exact
/// occurrence/target selection only. Compiler-generated names, ResourceIds,
/// control routes, and Program-Binding construction never cross this boundary.
class ActiveSourceModelProjection final {
 public:
  explicit ActiveSourceModelProjection(
      std::filesystem::path package_root,
      std::optional<std::filesystem::path> candidate_package_root =
          std::nullopt);

  [[nodiscard]] manyforge::content_identity::CanonicalValue Project(
      const manyforge::deployment::RetainedBundle& active_bundle) const;

 private:
  manyforge::deployment::FilesystemVerifiedRetainedModelMapSource model_maps_;
};

}  // namespace manyforge::headless
