#pragma once

#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment {

class ProgramReferenceValidationIncomplete : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ProgramReferenceLocation final {
  std::string path;
  std::optional<std::string> node_instance_id;
  std::optional<std::string> parameter;
};

struct ProgramReferenceDiagnostic final {
  std::string code;
  ProgramReferenceLocation location;
  CanonicalValue payload;
};

class ProgramReferenceValidationRefusal final : public std::runtime_error {
 public:
  explicit ProgramReferenceValidationRefusal(std::vector<ProgramReferenceDiagnostic> diagnostics)
      : std::runtime_error("Program references are invalid for the candidate Deployment"),
        diagnostics_(std::move(diagnostics)) {}
  [[nodiscard]] const std::vector<ProgramReferenceDiagnostic>& diagnostics() const noexcept {
    return diagnostics_;
  }
 private:
  std::vector<ProgramReferenceDiagnostic> diagnostics_;
};

/// Validate a parsed v3 Program against one immutable verified candidate.
/// Structural validation and node-catalog validation precede this call.
/// Candidate/catalog inconsistencies throw; they are not missing-reference
/// diagnostics. Draft permits absent reference fields; strict input does not.
[[nodiscard]] std::vector<ProgramReferenceDiagnostic>
validate_program_references(
    const CanonicalValue &program, bool draft,
    const RetainedBundle &retained_bundle,
    const std::optional<VerifiedRetainedModelMap> &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

} // namespace manyforge::deployment
