#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/io_signal.hpp>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Scalar subset of the existing Program expression value contract. No pose,
/// records, arithmetic or scripting. Construct string alternatives explicitly
/// with std::string/in_place_type<string>, never an implicitly converted char*.
using IoConditionValueDTO = std::variant<bool, std::int64_t, double, std::string>;

enum class IoConditionValueKind { kBoolean, kInt64, kFloat64, kString };

struct IoConditionBlackboardKeyDTO final {
  std::string name;
  friend auto operator<=>(const IoConditionBlackboardKeyDTO&,
                          const IoConditionBlackboardKeyDTO&) = default;
};

/// Inputs are already resolved by the existing Program/activation binding.
/// There is no parameter alternative and no source lookup/cache in evaluation.
using IoConditionInputSourceDTO = std::variant<IoSignalId, IoConditionBlackboardKeyDTO>;

struct IoConditionInputDTO final {
  IoConditionInputSourceDTO source;
  IoConditionValueKind value_kind;
};

struct IoConditionInputReferenceDTO final {
  std::size_t input_index;
};

using IoConditionOperandDTO = std::variant<IoConditionInputReferenceDTO, IoConditionValueDTO>;

enum class IoConditionComparison {
  kEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
};

struct IoConditionCompareDTO final {
  IoConditionComparison comparison;
  IoConditionOperandDTO left;
  IoConditionOperandDTO right;
};

struct IoConditionAllDTO final {
  std::vector<std::size_t> children;
};

struct IoConditionAnyDTO final {
  std::vector<std::size_t> children;
};

struct IoConditionNotDTO final {
  std::size_t child;
};

using IoConditionNodeDTO =
    std::variant<IoConditionCompareDTO, IoConditionAllDTO, IoConditionAnyDTO, IoConditionNotDTO>;

/// Canonical postorder: every child index is smaller than its parent. All/Any
/// have at least two children; the last node is the root, and every declared
/// input/node is reachable. This bounds validation/evaluation without recursion.
struct IoConditionDefinitionDTO final {
  std::vector<IoConditionInputDTO> inputs;
  std::vector<IoConditionNodeDTO> nodes;
};

/// Explicit caller bounds, with no product defaults or imported script limits.
struct IoConditionLimitsDTO final {
  std::size_t maximum_inputs;
  std::size_t maximum_nodes;
  std::size_t maximum_edges;
  std::size_t maximum_depth;
  std::size_t maximum_identity_bytes;
  std::size_t maximum_string_bytes;
};

}  // namespace manyforge::core
