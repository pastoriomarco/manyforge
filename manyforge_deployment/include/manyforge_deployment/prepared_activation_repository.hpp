#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

namespace manyforge::deployment {

/// Upper bound of one stored prepared activation, matching the preparer's
/// bounded output (its payload carries raw and catalog artifacts).
inline constexpr std::uint64_t kMaxPreparedActivationBytes =
    128ULL * 1024ULL * 1024ULL;

/// Recompile the prepared Program outcome from the carried Program, authored
/// role assignment, retained bundle, model map and catalog artifacts, and
/// require the exact recorded outcome: the same binding identity, or the
/// identical named non-executable result (H1a). A recorded outcome is thereby
/// proven, not merely consistent with its summary. Throws
/// PreparedActivationInputError on any difference.
void verify_prepared_program_outcome(const PreparedActivationInput &prepared);

/// The verified recompilation itself: as `verify_prepared_program_outcome`,
/// then returns it, including the roles of a non-executable Program that
/// still resolve (used only to place its authored initial Scene).
[[nodiscard]] ProgramBindingOutcome
recompile_prepared_program_outcome(const PreparedActivationInput &prepared);

/// The one prepared-activation artifact row encoding: rows sorted by store
/// key, bytes in bounded base64 chunks. Shared by the preparer and assembler.
[[nodiscard]] CanonicalValue
prepared_activation_artifact_rows(std::vector<PreparedActivationArtifact> artifacts);

/// Scenario-level facts of one prepared activation that are not derived from
/// the retained bundle, Program or role assignment.
struct PreparedActivationAssemblyInput final {
  core::ContentDigest source_document_digest;
  std::string scenario_id;
  core::ContentDigest captured_input_digest;
  /// One of "required", "optional" or "forbidden".
  std::string assistant_mode;
  CanonicalValue typed_parameter_overrides;
};

/// Assemble one complete v3 prepared activation from repository-verified
/// parts: bind the Program to `bundle` (bound or named non-executable), carry
/// exactly the catalog artifacts that binding reads and the bundle's model-map
/// artifact, then parse and verify the result like any stored value. Used by
/// callers that prepare from already-retained artifacts (for example package
/// fixtures); the activation preparer materializes its own closure.
[[nodiscard]] PreparedActivationInput assemble_prepared_activation(
    const PreparedActivationAssemblyInput &input, const RetainedBundle &bundle,
    const VerifiedRetainedModelMap &model_map,
    const ProgramBindingContract &program, const CanonicalValue &role_assignment,
    const ICatalogArtifactSource &catalog, const IRawArtifactStore &raw_artifacts);

/// Assemble for a candidate whose retained product set may contain no model
/// map. Absence is accepted only when the candidate has no controllable
/// components or model instances and the Program outcome is non-executable.
[[nodiscard]] PreparedActivationInput assemble_prepared_activation(
    const PreparedActivationAssemblyInput &input, const RetainedBundle &bundle,
    const std::optional<VerifiedRetainedModelMap> &model_map,
    const ProgramBindingContract &program, const CanonicalValue &role_assignment,
    const ICatalogArtifactSource &catalog, const IRawArtifactStore &raw_artifacts);

/// Immutable content-addressed store of prepared activations under one
/// trusted activation root (`prepared-activations/<hex>.json`).
///
/// The preparer publishes each prepared value it produced; the Manager
/// accepts an Apply only for a stored value whose identity, content and
/// recompiled Program outcome verify, and whose summary equals the request.
class FilesystemPreparedActivationRepository final {
public:
  explicit FilesystemPreparedActivationRepository(
      std::filesystem::path trusted_root);

  /// Publish once; an existing file must hold identical bytes.
  [[nodiscard]] bool put_if_absent(const PreparedActivationInput &prepared) const;

  /// The verified stored value, or null when none is stored under this id.
  [[nodiscard]] std::shared_ptr<const PreparedActivationInput>
  get_verified(const core::ContentDigest &prepared_activation_id) const;

private:
  std::filesystem::path root_;
};

} // namespace manyforge::deployment
