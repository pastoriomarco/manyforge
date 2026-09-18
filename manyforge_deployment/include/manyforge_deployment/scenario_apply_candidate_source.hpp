#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>

namespace manyforge::deployment {

class ScenarioApplyCandidateError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ScenarioApplyCandidateQuery final {
  manyforge::core::DeploymentArtifactId deployment_artifact_id;
  manyforge::core::DeploymentRevision deployment_revision;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ScenarioApplyCandidateQuery &,
                         const ScenarioApplyCandidateQuery &) = default;
};

struct ScenarioApplyCandidateSelection final {
  ScenarioApplyCandidateQuery query;
  manyforge::core::ScenarioCatalogRevision scenario_catalog_revision;
  manyforge::core::RetainedBundleId retained_bundle_id;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ScenarioApplyCandidateSelection &,
                         const ScenarioApplyCandidateSelection &) = default;
};

struct ScenarioProgramSelection final {
  std::string scenario_id;
  core::ScenarioCatalogRevision scenario_catalog_revision;
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::ProgramBindingId program_binding_id;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::ContentDigest node_environment_digest;
  ScenarioDeploymentCompatibilityDTO deployment_compatibility;
  ScenarioAssistantMode assistant_mode;
  CanonicalValue typed_parameter_overrides;
  /// Exact verified retained assistant product, or JSON null when the
  /// selected bundle has none.  This is carried with the Program selection so
  /// a model-request leaf cannot rediscover or substitute endpoint/template
  /// contracts after admission.
  CanonicalValue normalized_assistant_projection;
  CanonicalValue normalized_program_root;
  CanonicalValue normalized_program_binding_root;

  [[nodiscard]] bool valid() const noexcept;
};

/// Read-only runtime selector for one fixed, sealed scenario catalog.
///
/// The source opens exactly `<catalog_name>.scenario-catalog.yaml` beneath an
/// existing trusted package root, rejects links and concurrent replacement,
/// verifies the complete closed catalog schema and content identity, and
/// resolves one deployment ID/revision to one bundle identity. Multiple
/// Program rows may name that deployment only when they all name the same
/// retained bundle. It never enumerates package contents, selects a latest
/// value, writes a repository, or exposes compiler functionality.
class FilesystemScenarioApplyCandidateSource final {
public:
  FilesystemScenarioApplyCandidateSource(
      std::filesystem::path package_root, std::string catalog_name,
      std::optional<std::filesystem::path> candidate_package_root =
          std::nullopt);

  [[nodiscard]] ScenarioApplyCandidateSelection
  ResolveExact(const ScenarioApplyCandidateQuery &query) const;

  /// Load and independently verify the complete immutable catalog without
  /// selecting an active/current scenario or acquiring publication authority.
  [[nodiscard]] CanonicalValue LoadVerifiedCatalog() const;

  /// Resolve one canonical scenario ID to its independently verified retained
  /// Program closure. Aliases and source paths are deliberately not accepted.
  [[nodiscard]] ScenarioProgramSelection
  LoadExactProgramSelection(std::string_view scenario_id) const;

  /// Load the exact normalized authored Deployment named by a previously
  /// verified Scenario selection. This is an authoring projection only: it
  /// never compiles, publishes, selects, or activates a Deployment.
  [[nodiscard]] CanonicalValue
  LoadExactDeploymentDocument(const ScenarioProgramSelection &selection) const;

  /// Resolve one Scenario from the exact immutable catalog revision named by
  /// an addressed request.  Candidate publication never changes the meaning
  /// of the unqualified lookup above.
  [[nodiscard]] ScenarioProgramSelection LoadExactProgramSelectionForCatalog(
      std::string_view scenario_id,
      const manyforge::core::ScenarioCatalogRevision &scenario_catalog_revision)
      const;

  /// Return the Scenario occurrence whose retained Deployment is the exact
  /// active Manager selection, or no value when this Scenario names another
  /// Deployment in every qualified catalog.
  [[nodiscard]] std::optional<ScenarioProgramSelection>
  TryLoadExactProgramSelectionForActive(
      std::string_view scenario_id,
      const manyforge::core::RetainedBundleId &retained_bundle_id,
      const manyforge::core::GraphRevision &graph_revision) const;

  [[nodiscard]] std::filesystem::path catalog_path() const;

  [[nodiscard]] std::filesystem::path catalog_path_for_revision(
      const manyforge::core::ScenarioCatalogRevision &scenario_catalog_revision)
      const;

private:
  [[nodiscard]] ScenarioProgramSelection LoadExactProgramSelectionFrom(
      std::string_view scenario_id,
      const std::filesystem::path &catalog_path) const;
  [[nodiscard]] std::optional<std::filesystem::path>
  candidate_catalog_path() const;

  std::filesystem::path package_root_;
  std::optional<std::filesystem::path> candidate_package_root_;
  std::string catalog_name_;
};

} // namespace manyforge::deployment
