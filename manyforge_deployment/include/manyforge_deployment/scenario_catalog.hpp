#pragma once

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

inline constexpr std::string_view kScenarioCatalogApiVersion =
    "manyforge.scenario_catalog.v1";
inline constexpr std::size_t kMaxScenarioCatalogRows = 4096U;
inline constexpr std::size_t kMaxScenarioAliasesPerRow = 64U;
inline constexpr std::size_t kMaxScenarioPresentationTextCharacters = 4096U;

enum class ScenarioAssistantMode : std::uint8_t {
  kRequired = 0,
  kOptional,
  kForbidden,
};

[[nodiscard]] constexpr std::string_view to_string(
    ScenarioAssistantMode mode) noexcept {
  switch (mode) {
    case ScenarioAssistantMode::kRequired:
      return "required";
    case ScenarioAssistantMode::kOptional:
      return "optional";
    case ScenarioAssistantMode::kForbidden:
      return "forbidden";
  }
  return {};
}

struct ScenarioCatalogRowDTO final {
  std::string scenario_id;
  std::optional<std::string> description;
  std::vector<std::string> aliases;
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::RetainedBundleId retained_bundle_id;
  core::ProgramBindingId program_binding_id;
  ScenarioAssistantMode assistant_mode;
  CanonicalValue typed_parameter_overrides;

  friend bool operator==(const ScenarioCatalogRowDTO&,
                         const ScenarioCatalogRowDTO&) = default;
};

class ScenarioCatalogError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

namespace detail {
class ScenarioCatalogConstructionAuthority;
}

/// Immutable, identity-verified standalone scenario catalog.
///
/// The value is authoring metadata only. It has no active/current deployment,
/// Apply, caller-authority, or operation-dispatch surface.
class ScenarioCatalog final {
 public:
  ~ScenarioCatalog() = default;
  ScenarioCatalog(const ScenarioCatalog&) = delete;
  ScenarioCatalog& operator=(const ScenarioCatalog&) = delete;
  ScenarioCatalog(ScenarioCatalog&&) = delete;
  ScenarioCatalog& operator=(ScenarioCatalog&&) = delete;

  [[nodiscard]] const core::ScenarioCatalogRevision& revision()
      const noexcept {
    return revision_;
  }
  [[nodiscard]] const std::vector<ScenarioCatalogRowDTO>& rows()
      const noexcept {
    return rows_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }
  [[nodiscard]] std::string_view canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

 private:
  ScenarioCatalog(core::ScenarioCatalogRevision revision,
                  std::vector<ScenarioCatalogRowDTO> rows,
                  CanonicalValue normalized_root,
                  std::string canonical_bytes)
      : revision_(std::move(revision)),
        rows_(std::move(rows)),
        normalized_root_(std::move(normalized_root)),
        canonical_bytes_(std::move(canonical_bytes)) {}

  core::ScenarioCatalogRevision revision_;
  std::vector<ScenarioCatalogRowDTO> rows_;
  CanonicalValue normalized_root_;
  std::string canonical_bytes_;

  friend class detail::ScenarioCatalogConstructionAuthority;
};

/// Seal one typed, canonical-sorted catalog and mint its content identity.
[[nodiscard]] std::shared_ptr<const ScenarioCatalog> build_scenario_catalog(
    std::vector<ScenarioCatalogRowDTO> rows);

/// Verify a YAML-or-JSON source artifact and return its canonical sealed value.
[[nodiscard]] std::shared_ptr<const ScenarioCatalog>
verify_scenario_catalog_artifact(std::string_view encoded_bytes);

/// Reload canonical repository bytes, rejecting non-canonical encodings.
[[nodiscard]] std::shared_ptr<const ScenarioCatalog>
reload_verified_scenario_catalog(std::string_view canonical_bytes);

enum class ScenarioCatalogPublishResult : std::uint8_t {
  kPublished = 0,
  kAlreadyCurrent,
  kRevisionConflict,
};

/// One named, bounded, durable scenario-catalog compare-and-set repository.
///
/// `expected_revision == nullopt` means the caller expects no current value.
/// A successful replacement is atomically published only after the new bytes
/// are durable. Reads independently reverify the complete content identity.
class FilesystemScenarioCatalogRepository final {
 public:
  FilesystemScenarioCatalogRepository(std::filesystem::path trusted_root,
                                      std::string catalog_name);
  ~FilesystemScenarioCatalogRepository();

  FilesystemScenarioCatalogRepository(
      const FilesystemScenarioCatalogRepository&) = delete;
  FilesystemScenarioCatalogRepository& operator=(
      const FilesystemScenarioCatalogRepository&) = delete;
  FilesystemScenarioCatalogRepository(
      FilesystemScenarioCatalogRepository&&) = delete;
  FilesystemScenarioCatalogRepository& operator=(
      FilesystemScenarioCatalogRepository&&) = delete;

  [[nodiscard]] ScenarioCatalogPublishResult compare_and_set(
      const std::optional<core::ScenarioCatalogRevision>& expected_revision,
      std::shared_ptr<const ScenarioCatalog> candidate);

  [[nodiscard]] std::shared_ptr<const ScenarioCatalog> get_verified() const;
  [[nodiscard]] const std::filesystem::path& catalog_path() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

/// Exact source/repository coordinates supplied by the bounded Python
/// enumerator to the native semantic closure verifier.
struct ScenarioArtifactClosureQuery final {
  std::string scenario_id;
  core::ScenarioCatalogRevision scenario_catalog_revision;
  std::filesystem::path repository_root;
  std::filesystem::path scenario_catalog_path;
  std::filesystem::path deployment_path;
  std::filesystem::path retained_bundle_path;
  std::filesystem::path program_binding_path;
  std::filesystem::path program_path;
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::RetainedBundleId retained_bundle_id;
  core::ProgramBindingId program_binding_id;
  core::GraphRevision graph_revision;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::ContentDigest node_environment_digest;
  ScenarioAssistantMode assistant_mode;
  std::string typed_parameter_overrides_json;
};

struct VerifiedScenarioArtifactClosure final {
  std::string lane;
  bool assistant_enabled{};

  friend bool operator==(const VerifiedScenarioArtifactClosure&,
                         const VerifiedScenarioArtifactClosure&) = default;
};

/// Compose the qualified deployment, retained-product, Program-contract,
/// Program-Binding, catalog, compatibility, and typed-override owners for one
/// exact scenario. This function performs no publication, Apply, activation,
/// authority selection, network access, or robot effect.
[[nodiscard]] VerifiedScenarioArtifactClosure
verify_scenario_artifact_closure(const ScenarioArtifactClosureQuery& query);

}  // namespace manyforge::deployment
