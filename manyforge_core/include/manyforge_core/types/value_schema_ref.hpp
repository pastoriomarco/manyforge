#pragma once

#include <manyforge_core/types/strong_id.hpp>
#include <string_view>

namespace manyforge::core {

/// Exact selector for a separately build-qualified
/// `manyforge.closed_schema.v1` descriptor with `schema_purpose: value`.
struct ValueSchemaRefDTO {
  SchemaId schema_id;
  SchemaRevision schema_revision;

  friend bool operator==(const ValueSchemaRefDTO&, const ValueSchemaRefDTO&) = default;
};

/// Allocation-free build-owned projection of a value-schema selector.
struct ValueSchemaRefDescriptor {
  std::string_view schema_id;
  std::string_view schema_revision;

  friend bool operator==(const ValueSchemaRefDescriptor&,
                         const ValueSchemaRefDescriptor&) = default;
};

}  // namespace manyforge::core
