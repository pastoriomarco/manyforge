#include <manyforge_deployment/cell_compiler.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "cell_compiler_internal.hpp"

namespace manyforge::deployment {
namespace {

template <typename Qualified>
void append_references(std::vector<core::CatalogRecordRefDTO>& references,
                       const std::vector<Qualified>& qualified) {
  references.reserve(references.size() + qualified.size());
  for (const auto& value : qualified) {
    references.emplace_back(value.reference());
  }
}

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
collect_catalog_record_refs(
    const ApplicabilityQualifiedCellIr& applicability) {
  std::vector<core::CatalogRecordRefDTO> references;
  const auto& composition = applicability.composition();
  references.reserve(
      composition.model_assets().size() +
      composition.component_types().size() +
      applicability.runtime_profiles().size() +
      applicability.capability_definitions().size() +
      applicability.control_interface_contracts().size() +
      applicability.controller_topologies().size() +
      applicability.provider_manifests().size() +
      applicability.stop_evidence_policies().size());

  for (const auto& asset : composition.model_assets()) {
    references.emplace_back(asset.qualified_asset().reference());
  }
  append_references(references, composition.component_types());
  append_references(references, applicability.runtime_profiles());
  append_references(references, applicability.capability_definitions());
  append_references(references,
                    applicability.control_interface_contracts());
  append_references(references, applicability.controller_topologies());
  append_references(references, applicability.provider_manifests());
  append_references(references, applicability.stop_evidence_policies());
  return detail::canonicalize_compiler_catalog_record_refs(
      std::move(references));
}

}  // namespace

namespace detail {

std::vector<core::CatalogRecordRefDTO>
canonicalize_compiler_catalog_record_refs(
    std::vector<core::CatalogRecordRefDTO> references) {
  const core::CanonicalCatalogRecordRefLess less;
  std::sort(references.begin(), references.end(), less);
  for (std::size_t index = 1U; index < references.size(); ++index) {
    const auto& previous = references[index - 1U];
    const auto& current = references[index];
    if (less(previous, current) || less(current, previous)) {
      continue;
    }
    const std::string key =
        std::string(core::to_string(core::catalog_record_kind(current))) +
        "/" + core::catalog_record_id_value(current) + "@" +
        core::catalog_record_revision_value(current);
    if (previous == current) {
      throw CellCompilerError(
          "compiler catalog provenance contains a duplicate exact record: " +
          key);
    }
    throw CellCompilerError(
        "compiler catalog provenance contains one record key with unequal "
        "reference metadata: " +
        key);
  }
  return references;
}

}  // namespace detail

CompilerQualifiedCellIr compile_cell_ir(
    const ApplicabilityQualifiedCellIr& applicability) {
  return CompilerQualifiedCellIr(
      applicability, collect_catalog_record_refs(applicability));
}

}  // namespace manyforge::deployment
