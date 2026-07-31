#pragma once

#include <cstddef>
#include <manyforge_deployment/model_source.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {

inline constexpr std::size_t kMf2JointLocalRankWidth = 6U;
inline constexpr std::size_t kMf2JointMaximumLocalRank = 999999U;
inline constexpr std::string_view kMf2JointHashPreimageDomain =
    "manyforge.occurrence-joint-name.mf2";

class Mf2JointOrderingError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// One deterministic parent-local rank for every joint in an ordinary URDF
/// tree.
///
/// Cross-branch affine-mimic dependencies are projected to the first
/// divergent sibling joints. The result is independent of input row order and
/// contains no planner- or importer-specific state.
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>>
dependency_ordered_joint_local_ranks(const UrdfDocumentIr& urdf);

/// Generates one compiler-owned joint name in the authoritative mf2 namespace.
///
/// The fixed-width rank controls sibling traversal order. The digest excludes
/// that rank deliberately: it remains the stable semantic-identity part of
/// the name when a dependency changes the local traversal position.
[[nodiscard]] std::string make_mf2_generated_joint_name(
    std::size_t parent_local_rank,
    const std::vector<std::pair<std::string_view, std::string_view>>& fields);

}  // namespace manyforge::deployment::detail
