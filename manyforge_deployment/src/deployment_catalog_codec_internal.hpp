#pragma once

#include <span>
#include <stdexcept>
#include <vector>

#include "manyforge_core/types/catalog.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment::detail {

class DeploymentCatalogCodecError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Decodes the exact private deployment `catalogs` value:
/// `{records: CatalogRecordRefDTO[]}`.
///
/// The returned table is canonical-sorted by serialized record kind, typed
/// record ID, and typed record revision. This is a value-codec boundary, not
/// semantic qualification of any referenced catalog payload.
[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
decode_deployment_catalogs(const CanonicalValue& catalogs);

/// Validates and deterministically encodes the exact private deployment
/// `catalogs` value. Caller ordering is never retained.
[[nodiscard]] CanonicalValue encode_deployment_catalogs(
    std::span<const core::CatalogRecordRefDTO> records);

}  // namespace manyforge::deployment::detail
