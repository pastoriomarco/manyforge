#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/cell_graph.hpp>

#include <vector>

namespace manyforge::deployment::detail {

void validate_apply_fence_qualification_bound(
    const core::CellGraphDTO& graph);

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
canonicalize_compiler_catalog_record_refs(
    std::vector<core::CatalogRecordRefDTO> references);

}  // namespace manyforge::deployment::detail
