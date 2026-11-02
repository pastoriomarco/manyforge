#pragma once

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::deployment {

inline constexpr std::string_view kStructuralModelPreparationApiVersion =
    "manyforge.structural_model_preparation.v1";
inline constexpr std::string_view kStructuralModelPreparationGeneratorId =
    "manyforge.structural_model_preparation";
inline constexpr std::string_view kStructuralModelPreparationGeneratorVersion =
    "1.2.0";
inline constexpr std::string_view
    kStructuralModelPreparationConfigurationIdentityDomain =
        "structural-model-preparation-receipt";

class StructuralModelPreparationError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Build-qualified exact preparation receipt joined to one source-verified
/// structural model.
///
/// The source tool's JSON receipt is evidence, not authority. Construction is
/// sealed behind the verifier below, which fixes the tool build/version,
/// verifies the receipt's canonical content identity, rehashes the original
/// documents, and joins every prepared artifact/rewrite row one-for-one to the
/// independently verified ModelAsset retrieval plan.
///
/// This value proves an exact build/configuration/input/output relationship.
/// It deliberately does not attest that a particular process invocation ran.
class BuildQualifiedStructuralModelPreparation final {
public:
  BuildQualifiedStructuralModelPreparation(
      const BuildQualifiedStructuralModelPreparation &) noexcept = default;
  BuildQualifiedStructuralModelPreparation &operator=(
      const BuildQualifiedStructuralModelPreparation &) noexcept = default;

  [[nodiscard]] const core::RetainedGeneratorRecordDTO &
  generator_record() const noexcept {
    return generator_record_;
  }
  [[nodiscard]] const SourceVerifiedModelAsset &
  verified_model_asset() const noexcept {
    return verified_model_asset_;
  }
  [[nodiscard]] std::string_view canonical_receipt_bytes() const noexcept {
    return canonical_receipt_bytes_;
  }

private:
  BuildQualifiedStructuralModelPreparation(
      core::RetainedGeneratorRecordDTO generator_record,
      SourceVerifiedModelAsset verified_model_asset,
      std::string canonical_receipt_bytes)
      : generator_record_(std::move(generator_record)),
        verified_model_asset_(std::move(verified_model_asset)),
        canonical_receipt_bytes_(std::move(canonical_receipt_bytes)) {}

  core::RetainedGeneratorRecordDTO generator_record_;
  SourceVerifiedModelAsset verified_model_asset_;
  std::string canonical_receipt_bytes_;

  friend BuildQualifiedStructuralModelPreparation
  verify_build_qualified_structural_model_preparation(
      std::string_view, std::string_view, std::optional<std::string_view>,
      const SourceVerifiedModelAsset &);
};

/// Qualifies one exact preparation receipt without executing a resolver.
///
/// `original_urdf_bytes` and optional `original_srdf_bytes` are independently
/// hashed against the receipt. Prepared XML and dependency bytes were already
/// independently verified when `verified_model_asset` was constructed; this
/// operation requires the receipt's complete artifact table to equal that
/// sealed retrieval plan and proof metadata exactly.
[[nodiscard]] BuildQualifiedStructuralModelPreparation
verify_build_qualified_structural_model_preparation(
    std::string_view canonical_receipt_bytes,
    std::string_view original_urdf_bytes,
    std::optional<std::string_view> original_srdf_bytes,
    const SourceVerifiedModelAsset &verified_model_asset);

} // namespace manyforge::deployment
