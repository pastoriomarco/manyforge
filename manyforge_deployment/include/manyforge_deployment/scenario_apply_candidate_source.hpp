#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>

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
  FilesystemScenarioApplyCandidateSource(std::filesystem::path package_root,
                                         std::string catalog_name);

  [[nodiscard]] ScenarioApplyCandidateSelection
  ResolveExact(const ScenarioApplyCandidateQuery &query) const;

  [[nodiscard]] const std::filesystem::path &catalog_path() const noexcept;

private:
  std::filesystem::path package_root_;
  std::filesystem::path catalog_path_;
};

} // namespace manyforge::deployment
