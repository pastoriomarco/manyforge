#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

inline constexpr std::string_view kPreparedActivationInputSchemaVersion =
    "manyforge.prepared_activation_input.v6";

struct PreparedScenarioOrigin final {
  std::string scenario_id;
  core::ContentDigest source_document_digest;
  friend bool operator==(const PreparedScenarioOrigin &, const PreparedScenarioOrigin &) = default;
};

struct PreparedSealedCanonicalEmptyOrigin final {
  core::CellId cell_id;
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::ContentDigest admission_digest;
  friend bool operator==(const PreparedSealedCanonicalEmptyOrigin &, const PreparedSealedCanonicalEmptyOrigin &) = default;
};

using PreparedActivationOrigin =
    std::variant<PreparedScenarioOrigin, PreparedSealedCanonicalEmptyOrigin>;

[[nodiscard]] PreparedActivationOrigin parse_prepared_activation_origin(const CanonicalValue &value);
[[nodiscard]] CanonicalValue prepared_activation_origin_value(const PreparedActivationOrigin &origin);
[[nodiscard]] core::ContentDigest sealed_canonical_empty_capture_digest(
    const PreparedSealedCanonicalEmptyOrigin &origin);

/// Pure projection of the Program outcome in an immutable Apply receipt.
/// This grants no Applied/currentness authority. Existing Headless ingress
/// error codes are retained so both consumers share the same interpretation.
struct PreparedActivationProgramOutcomeDTO final {
  std::optional<core::ProgramBindingId> program_binding_id;
  std::optional<ProgramNonExecutableOutcomeDTO> non_executable;
};
[[nodiscard]] core::Result<PreparedActivationProgramOutcomeDTO>
parse_prepared_activation_program_outcome(const CanonicalValue &prepared);

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
  /// A candidate with no retained model products has no symbol-role map.
  /// The candidate may have no selected Program, or a non-executable Program.
  std::optional<VerifiedRetainedModelMap> retained_model_map;
  std::shared_ptr<const ProgramBindingContract> program;
  /// Null for no Program or a non-executable Program.
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
  [[nodiscard]] const PreparedActivationOrigin &origin() const noexcept {
    return origin_;
  }
  [[nodiscard]] const PreparedScenarioOrigin *scenario_origin() const noexcept {
    return std::get_if<PreparedScenarioOrigin>(&origin_);
  }
  [[nodiscard]] const PreparedSealedCanonicalEmptyOrigin *sealed_canonical_empty_origin() const noexcept {
    return std::get_if<PreparedSealedCanonicalEmptyOrigin>(&origin_);
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
  [[nodiscard]] const std::optional<core::ProgramId> &program_id() const noexcept {
    return program_id_;
  }
  [[nodiscard]] const std::optional<core::ProgramRevision> &program_revision() const noexcept {
    return program_revision_;
  }
  /// The candidate binding identity; absent exactly when
  /// a non-executable Program or no Program is selected.
  [[nodiscard]] const std::optional<core::ProgramBindingId> &
  program_binding_id() const noexcept {
    return program_binding_id_;
  }
  [[nodiscard]] const std::optional<ProgramNonExecutableOutcomeDTO> &
  program_outcome() const noexcept {
    return program_outcome_;
  }
  /// The authored role assignment the outcome was compiled from, carried so
  /// the outcome can be recompiled on readback (see
  /// `verify_prepared_program_outcome`).
  [[nodiscard]] const core::ProgramRoleAssignmentDTO &
  program_role_assignment() const noexcept {
    return program_role_assignment_;
  }
  [[nodiscard]] const core::ContentDigest &node_environment_digest() const
      noexcept {
    return node_environment_digest_;
  }
  [[nodiscard]] std::string_view assistant_mode() const noexcept {
    return assistant_mode_;
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
      PreparedActivationOrigin origin,
      core::ContentDigest captured_input_digest,
      core::DeploymentArtifactId deployment_artifact_id,
      core::DeploymentRevision deployment_revision,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision, std::optional<core::ProgramId> program_id,
      std::optional<core::ProgramRevision> program_revision,
      std::optional<core::ProgramBindingId> program_binding_id,
      std::optional<ProgramNonExecutableOutcomeDTO> program_outcome,
      core::ProgramRoleAssignmentDTO program_role_assignment,
      core::ContentDigest node_environment_digest, std::string assistant_mode,
      CanonicalValue normalized_root,
      VerifiedPreparedActivationClosure closure);

  core::ContentDigest prepared_activation_id_;
  PreparedActivationOrigin origin_;
  core::ContentDigest captured_input_digest_;
  core::DeploymentArtifactId deployment_artifact_id_;
  core::DeploymentRevision deployment_revision_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  std::optional<core::ProgramId> program_id_;
  std::optional<core::ProgramRevision> program_revision_;
  std::optional<core::ProgramBindingId> program_binding_id_;
  std::optional<ProgramNonExecutableOutcomeDTO> program_outcome_;
  core::ProgramRoleAssignmentDTO program_role_assignment_;
  core::ContentDigest node_environment_digest_;
  std::string assistant_mode_;
  CanonicalValue normalized_root_;
  VerifiedPreparedActivationClosure closure_;

  friend PreparedActivationInput
  parse_prepared_activation_input(const CanonicalValue &value);
};

/// Parse the closed prepared-input schema and independently verify its content
/// identity. Unknown, missing, empty, or incorrectly typed fields fail closed.
///
/// This structural verifier cannot depend on the binding compiler, so a parsed
/// Program outcome is consistent but not yet proven. Admission evidence must
/// come from `verify_prepared_program_outcome` (prepared_activation_repository
/// .hpp), which recompiles the outcome from the carried Program, role
/// assignment and candidate bundle and requires the exact binding or named
/// non-executable result; `FilesystemPreparedActivationRepository::get_verified`
/// always applies it.
[[nodiscard]] PreparedActivationInput
parse_prepared_activation_input(const CanonicalValue &value);

} // namespace manyforge::deployment
