#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_deployment/cell_applicability.hpp>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::deployment {

class CellCompilerError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Immutable, non-caller-constructible result of the complete pure I2
/// semantic compiler.
///
/// The exact applicability-qualified predecessor owns the identity-verified
/// graph, source and occurrence-composition meaning, and every normalized
/// applicability input and logical template. This final wrapper additionally
/// owns the canonical exact catalog-reference union for that semantic closure;
/// it adds no generated product, process, runtime, or retained-artifact claim.
class CompilerQualifiedCellIr final {
 public:
  CompilerQualifiedCellIr(const CompilerQualifiedCellIr&) noexcept = default;
  CompilerQualifiedCellIr& operator=(
      const CompilerQualifiedCellIr&) noexcept = default;

  [[nodiscard]] const ApplicabilityQualifiedCellIr& applicability()
      const noexcept {
    return data_->applicability;
  }
  [[nodiscard]] const std::vector<core::CatalogRecordRefDTO>&
  catalog_record_refs() const noexcept {
    return data_->catalog_record_refs;
  }

 private:
  struct Data final {
    Data(const ApplicabilityQualifiedCellIr& applicability_in,
         std::vector<core::CatalogRecordRefDTO> catalog_record_refs_in)
        : applicability(applicability_in),
          catalog_record_refs(std::move(catalog_record_refs_in)) {}

    ApplicabilityQualifiedCellIr applicability;
    std::vector<core::CatalogRecordRefDTO> catalog_record_refs;
  };

  explicit CompilerQualifiedCellIr(
      const ApplicabilityQualifiedCellIr& applicability,
      std::vector<core::CatalogRecordRefDTO> catalog_record_refs)
      : data_(std::make_shared<const Data>(
            applicability, std::move(catalog_record_refs))) {}

  std::shared_ptr<const Data> data_;

  friend CompilerQualifiedCellIr compile_cell_ir(
      const ApplicabilityQualifiedCellIr&);
};

/// Closes the pure I2 trust boundary over its exact non-forgeable predecessor.
///
/// This is the sole public producer of CompilerQualifiedCellIr. It performs no
/// I/O and accepts no raw graph, structural, composition, or caller-built DTO
/// alternative. Duplicate exact catalog references and equal canonical record
/// keys with unequal reference metadata reject rather than being deduplicated.
[[nodiscard]] CompilerQualifiedCellIr compile_cell_ir(
    const ApplicabilityQualifiedCellIr& applicability);

}  // namespace manyforge::deployment
