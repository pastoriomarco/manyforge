#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {
class CompilerQualifiedCellIr;
}

namespace manyforge::deployment::detail {

inline constexpr std::string_view kComposedModelSymbolRoleMapApiVersion =
    "manyforge.composed_model_symbol_role_map.v2";
inline constexpr std::string_view kComposedModelSymbolRoleMapContractId =
    "manyforge.composed_model_symbol_role_map";
inline constexpr std::string_view kComposedModelSymbolRoleMapArtifactRole =
    "composed_model_symbol_role_map";
inline constexpr std::string_view kComposedModelSymbolRoleMapMediaType =
    "application/vnd.manyforge.composed-model-symbol-role-map.v2+json";

// This first qualified map product deliberately uses the common strict
// artifact-codec ceiling. The compiler accepts a larger semantic universe, while retention
// rejects a map that cannot be independently parsed and reverified by every
// current retained-artifact reader.
inline constexpr std::size_t kMaxComposedModelSymbolRoleMapBytes =
    16U * 1024U * 1024U;

/// Emits the closed compiler-owned model namespace and generic semantic maps.
///
/// The input is the non-caller-constructible final compiler value. No source XML is
/// reparsed and no consumer-specific name or type is introduced.
[[nodiscard]] std::string emit_composed_model_symbol_role_map(
    const CompilerQualifiedCellIr& compiler_ir,
    bool allow_empty_model_map = false);

/// Strictly parses and independently verifies one exact map artifact.
///
/// The returned value is its canonical projection. Non-canonical bytes,
/// unknown fields, an incomplete/non-bijective symbol table, broken role or
/// coordinate references, and out-of-bound inputs reject. A map with no
/// symbols and no occurrences rejects unless `allow_empty_model_map` names the
/// Scenario-candidate path of a model-free cell.
[[nodiscard]] CanonicalValue parse_and_verify_composed_model_symbol_role_map(
    std::string_view canonical_bytes, bool allow_empty_model_map = false);

/// Requires exact agreement with the map mechanically derived from the same
/// final compiler value. This is used before retention and after raw-CAS read-back.
void require_composed_model_symbol_role_map_matches(
    std::string_view canonical_bytes,
    const CompilerQualifiedCellIr& compiler_ir,
    bool allow_empty_model_map = false);

}  // namespace manyforge::deployment::detail
