#pragma once

#include <manyforge_core/types/catalog.hpp>

#include <vector>

namespace manyforge::deployment::detail {

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
canonicalize_compiler_catalog_record_refs(
    std::vector<core::CatalogRecordRefDTO> references);

}  // namespace manyforge::deployment::detail
