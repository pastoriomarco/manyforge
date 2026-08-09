#pragma once

#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::headless {

inline constexpr std::string_view kAssistantModeProjectionApiVersion =
    "manyforge.assistant_mode_projection.v1";
inline constexpr std::string_view kAssistantModeIndexApiVersion =
    "manyforge.assistant_mode_index.v1";
inline constexpr std::string_view kAssistantModeCatalogApiVersion =
    "manyforge.assistant_mode_catalog.v1";
inline constexpr std::uint64_t kMaximumAssistantModeProjectionBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumAssistantModeIndexBytes = 8ULL * 1024ULL * 1024ULL;

class AssistantModeProjectionError : public std::runtime_error {
 public:
  AssistantModeProjectionError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}

  [[nodiscard]] const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

/// Build one exact mode projection from an independently verified bundle.
///
/// This publishes descriptors and their retained identities. It grants no
/// callback or runtime availability by itself.
[[nodiscard]] manyforge::content_identity::CanonicalValue ProjectAssistantMode(
    const manyforge::deployment::RetainedBundle& active_bundle, std::string_view mode_id);

/// Build the bounded index of enabled modes from the same independently
/// verified assistant projection used by ProjectAssistantMode.
///
/// The index exists for discovery only. A caller must still fetch the exact
/// selected mode projection before offering or invoking any instrument.
[[nodiscard]] manyforge::content_identity::CanonicalValue ProjectAssistantModeIndex(
    const manyforge::deployment::RetainedBundle& active_bundle);

}  // namespace manyforge::headless
