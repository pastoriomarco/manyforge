#pragma once

#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>

#include <memory>
#include <stdexcept>

namespace manyforge::deployment {

class RetentionQualificationError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// One immutable proof that a complete source deployment and the final I2
/// compiler result describe exactly the same semantic cell.
///
/// This proof is the sole legal deployment/compiler input to I3 product
/// materialization. It performs no retrieval, storage, product construction,
/// publication, process, or runtime effect.
class RetentionQualifiedDeployment final {
 public:
  RetentionQualifiedDeployment(
      const RetentionQualifiedDeployment&) noexcept = default;
  RetentionQualifiedDeployment& operator=(
      const RetentionQualifiedDeployment&) noexcept = default;

  [[nodiscard]] const RevisionVerifiedDeploymentArtifact& deployment()
      const noexcept {
    return data_->deployment;
  }

  [[nodiscard]] const CompilerQualifiedCellIr& compiler_ir() const noexcept {
    return data_->compiler_ir;
  }

 private:
  struct Data final {
    Data(const RevisionVerifiedDeploymentArtifact& deployment_in,
         const CompilerQualifiedCellIr& compiler_ir_in)
        : deployment(deployment_in), compiler_ir(compiler_ir_in) {}

    RevisionVerifiedDeploymentArtifact deployment;
    CompilerQualifiedCellIr compiler_ir;
  };

  RetentionQualifiedDeployment(
      const RevisionVerifiedDeploymentArtifact& deployment,
      const CompilerQualifiedCellIr& compiler_ir)
      : data_(std::make_shared<const Data>(deployment, compiler_ir)) {}

  std::shared_ptr<const Data> data_;

  friend RetentionQualifiedDeployment qualify_deployment_for_retention(
      const RevisionVerifiedDeploymentArtifact&,
      const CompilerQualifiedCellIr&);
};

/// Proves complete field-for-field agreement before I3 can perform an effect.
///
/// The comparison covers cell identity, the complete graph and GraphRevision,
/// every applicability table, optional source-presence spelling, selected
/// site-overlay semantics, and the exact catalog-reference union. It never
/// repairs either predecessor.
[[nodiscard]] RetentionQualifiedDeployment
qualify_deployment_for_retention(
    const RevisionVerifiedDeploymentArtifact& deployment,
    const CompilerQualifiedCellIr& compiler_ir);

}  // namespace manyforge::deployment
