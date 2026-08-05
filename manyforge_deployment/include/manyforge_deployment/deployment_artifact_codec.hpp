#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"
#include "manyforge_deployment/deployment_applicability_codec.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"

namespace manyforge::deployment {

class DeploymentArtifactCodecError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Complete, source-preserving `manyforge.deployment.v1` identity proof.
///
/// This trust root closes the complete deployment envelope, verifies every
/// nested graph/provider identity, and verifies the outer DeploymentRevision.
/// It deliberately does not assert catalog-referent qualification,
/// applicability, compiler/IR equality, retained products, or runtime
/// admission. Unsupported optional semantic blocks reject before this value
/// can be produced.
class RevisionVerifiedDeploymentArtifact final {
 public:
  RevisionVerifiedDeploymentArtifact(
      const RevisionVerifiedDeploymentArtifact&) noexcept = default;
  RevisionVerifiedDeploymentArtifact& operator=(
      const RevisionVerifiedDeploymentArtifact&) noexcept = default;

  [[nodiscard]] const core::DeploymentArtifactId& deployment_artifact_id()
      const noexcept {
    return data_->deployment_artifact_id;
  }
  [[nodiscard]] const core::DeploymentRevision& deployment_revision()
      const noexcept {
    return data_->deployment_revision;
  }
  [[nodiscard]] const core::CellId& cell_id() const noexcept {
    return data_->cell_id;
  }
  [[nodiscard]] const std::string& name() const noexcept {
    return data_->name;
  }
  [[nodiscard]] const std::optional<std::string>& description()
      const noexcept {
    return data_->description;
  }
  [[nodiscard]] const std::vector<core::CatalogRecordRefDTO>& catalog_records()
      const noexcept {
    return data_->catalog_records;
  }
  [[nodiscard]] const GraphRevisionVerifiedCellGraph& graph() const noexcept {
    return data_->graph;
  }
  [[nodiscard]] const RevisionVerifiedDeploymentApplicabilityInputs&
  applicability_inputs() const noexcept {
    return data_->applicability_inputs;
  }
  [[nodiscard]] bool assistant_was_present() const noexcept {
    return data_->applicability_inputs.assistant_settings().source_was_present();
  }
  [[nodiscard]] const TypedAssistantSettingsCandidate& assistant_settings()
      const noexcept {
    return data_->applicability_inputs.assistant_settings();
  }

  /// Exact accepted deployment source after schema-owned collection
  /// normalization and identity production/verification.
  [[nodiscard]] const CanonicalValue& normalized_authored_root()
      const noexcept {
    return data_->normalized_authored_root;
  }

  /// Exact normalized authored `graph` member: the six record arrays only.
  [[nodiscard]] const CanonicalValue& normalized_authored_graph()
      const noexcept {
    return *data_->normalized_authored_root.find_object_member("graph");
  }

 private:
  struct Data final {
    Data(
        core::DeploymentArtifactId deployment_artifact_id_in,
        core::DeploymentRevision deployment_revision_in,
        core::CellId cell_id_in, std::string name_in,
        std::optional<std::string> description_in,
        std::vector<core::CatalogRecordRefDTO> catalog_records_in,
        const GraphRevisionVerifiedCellGraph& graph_in,
        const RevisionVerifiedDeploymentApplicabilityInputs&
            applicability_inputs_in,
        CanonicalValue normalized_authored_root_in)
        : deployment_artifact_id(std::move(deployment_artifact_id_in)),
          deployment_revision(std::move(deployment_revision_in)),
          cell_id(std::move(cell_id_in)),
          name(std::move(name_in)),
          description(std::move(description_in)),
          catalog_records(std::move(catalog_records_in)),
          graph(graph_in),
          applicability_inputs(applicability_inputs_in),
          normalized_authored_root(
              std::move(normalized_authored_root_in)) {}

    core::DeploymentArtifactId deployment_artifact_id;
    core::DeploymentRevision deployment_revision;
    core::CellId cell_id;
    std::string name;
    std::optional<std::string> description;
    std::vector<core::CatalogRecordRefDTO> catalog_records;
    GraphRevisionVerifiedCellGraph graph;
    RevisionVerifiedDeploymentApplicabilityInputs applicability_inputs;
    CanonicalValue normalized_authored_root;
  };

  RevisionVerifiedDeploymentArtifact(
      core::DeploymentArtifactId deployment_artifact_id,
      core::DeploymentRevision deployment_revision, core::CellId cell_id,
      std::string name, std::optional<std::string> description,
      std::vector<core::CatalogRecordRefDTO> catalog_records,
      const GraphRevisionVerifiedCellGraph& graph,
      const RevisionVerifiedDeploymentApplicabilityInputs&
          applicability_inputs,
      CanonicalValue normalized_authored_root)
      : data_(std::make_shared<const Data>(
            std::move(deployment_artifact_id),
            std::move(deployment_revision), std::move(cell_id),
            std::move(name), std::move(description),
            std::move(catalog_records), graph, applicability_inputs,
            std::move(normalized_authored_root))) {}

  std::shared_ptr<const Data> data_;

  friend RevisionVerifiedDeploymentArtifact
  compute_deployment_artifact_revision(
      const ParsedArtifactCandidate&,
      std::span<const QualifiedRuntimeProfile>);
  friend RevisionVerifiedDeploymentArtifact
  verify_deployment_artifact_revision(
      const ParsedArtifactCandidate&,
      std::span<const QualifiedRuntimeProfile>);
};

/// Producer path for the currently qualified deployment subset.
///
/// Existing graph-owned nested revisions, provider-binding revisions, and the
/// outer DeploymentRevision are placeholders and are deterministically
/// replaced. The authored graph has exactly its six record arrays; the
/// returned graph proof derives its outer GraphRevision from those arrays and
/// the deployment's `cell_id`.
[[nodiscard]] RevisionVerifiedDeploymentArtifact
compute_deployment_artifact_revision(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles);

/// Verifier path for the currently qualified deployment subset.
///
/// Every declared nested graph/provider revision and the outer
/// DeploymentRevision must already match independently recomputed identities.
/// Nothing is repaired on this path.
[[nodiscard]] RevisionVerifiedDeploymentArtifact
verify_deployment_artifact_revision(
    const ParsedArtifactCandidate& parsed_deployment,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles);

}  // namespace manyforge::deployment
