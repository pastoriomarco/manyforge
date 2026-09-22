#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

inline constexpr std::string_view kPreparedActivationInputSchemaVersion =
    "manyforge.prepared_activation_input.v2";

class PreparedActivationInputError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

struct PreparedActivationArtifact final {
  core::ContentAddressedStoreKey content_addressed_store_key;
  core::ContentDigest content_digest;
  std::string bytes;
};

struct VerifiedPreparedActivationClosure final {
  std::shared_ptr<const RetainedBundle> retained_bundle;
  VerifiedRetainedModelMap retained_model_map;
  std::shared_ptr<const ProgramBindingContract> program;
  std::shared_ptr<const ProgramBinding> program_binding;
  std::vector<PreparedActivationArtifact> raw_artifacts;
  std::vector<PreparedActivationArtifact> catalog_artifacts;
};

/// Strict, content-addressed in-memory handoff from preparation to activation.
/// It owns the exact verified closure; no catalog selection, mutable lookup or
/// filesystem repository is needed to recover the prepared values.
class PreparedActivationInput final {
public:
  [[nodiscard]] const core::ContentDigest &prepared_activation_id() const
      noexcept {
    return prepared_activation_id_;
  }
  [[nodiscard]] const core::ContentDigest &source_document_digest() const
      noexcept {
    return source_document_digest_;
  }
  [[nodiscard]] const core::ContentDigest &captured_input_digest() const
      noexcept {
    return captured_input_digest_;
  }
  [[nodiscard]] const core::DeploymentArtifactId &deployment_artifact_id()
      const noexcept {
    return deployment_artifact_id_;
  }
  [[nodiscard]] const core::DeploymentRevision &deployment_revision() const
      noexcept {
    return deployment_revision_;
  }
  [[nodiscard]] const core::RetainedBundleId &retained_bundle_id() const
      noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::GraphRevision &graph_revision() const noexcept {
    return graph_revision_;
  }
  [[nodiscard]] const core::ProgramId &program_id() const noexcept {
    return program_id_;
  }
  [[nodiscard]] const core::ProgramRevision &program_revision() const noexcept {
    return program_revision_;
  }
  [[nodiscard]] const core::ProgramBindingId &program_binding_id() const
      noexcept {
    return program_binding_id_;
  }
  [[nodiscard]] const core::ContentDigest &node_environment_digest() const
      noexcept {
    return node_environment_digest_;
  }
  [[nodiscard]] std::string_view assistant_mode() const noexcept {
    return assistant_mode_;
  }
  [[nodiscard]] const CanonicalValue &typed_parameter_overrides() const
      noexcept {
    return typed_parameter_overrides_;
  }
  [[nodiscard]] const CanonicalValue &normalized_root() const noexcept {
    return normalized_root_;
  }
  [[nodiscard]] const VerifiedPreparedActivationClosure &closure() const
      noexcept {
    return closure_;
  }

private:
  PreparedActivationInput(
      core::ContentDigest prepared_activation_id,
      core::ContentDigest source_document_digest,
      core::ContentDigest captured_input_digest,
      core::DeploymentArtifactId deployment_artifact_id,
      core::DeploymentRevision deployment_revision,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision, core::ProgramId program_id,
      core::ProgramRevision program_revision,
      core::ProgramBindingId program_binding_id,
      core::ContentDigest node_environment_digest, std::string assistant_mode,
      CanonicalValue typed_parameter_overrides, CanonicalValue normalized_root,
      VerifiedPreparedActivationClosure closure);

  core::ContentDigest prepared_activation_id_;
  core::ContentDigest source_document_digest_;
  core::ContentDigest captured_input_digest_;
  core::DeploymentArtifactId deployment_artifact_id_;
  core::DeploymentRevision deployment_revision_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  core::ProgramId program_id_;
  core::ProgramRevision program_revision_;
  core::ProgramBindingId program_binding_id_;
  core::ContentDigest node_environment_digest_;
  std::string assistant_mode_;
  CanonicalValue typed_parameter_overrides_;
  CanonicalValue normalized_root_;
  VerifiedPreparedActivationClosure closure_;

  friend PreparedActivationInput
  parse_prepared_activation_input(const CanonicalValue &value);
};

/// Parse the closed prepared-input schema and independently verify its content
/// identity. Unknown, missing, empty, or incorrectly typed fields fail closed.
[[nodiscard]] PreparedActivationInput
parse_prepared_activation_input(const CanonicalValue &value);

} // namespace manyforge::deployment
