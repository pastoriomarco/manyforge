#pragma once

#include <cstddef>

namespace manyforge::deployment {

/// Compiler-owned operational envelope for one retained cell.
///
/// A named Apply fences the overlap-closed union of one predecessor and one
/// candidate.  The transition constants below are therefore exactly twice
/// these per-cell cardinalities: even two otherwise-disjoint qualified cells
/// must fit the Manager journal, the managed execution-host ledger, and their
/// private packet channel without a late transport-only rejection.
inline constexpr std::size_t
    kMaximumQualifiedApplyFenceControllableComponentsPerCell = 64U;
inline constexpr std::size_t kMaximumQualifiedApplyFenceControlDomainsPerCell =
    32U;
inline constexpr std::size_t kMaximumQualifiedApplyFenceControlBindingsPerCell =
    64U;
inline constexpr std::size_t kMaximumQualifiedApplyFenceResourcesPerCell = 192U;

inline constexpr std::size_t
    kMaximumQualifiedApplyFenceControllableComponentsPerTransition =
        2U * kMaximumQualifiedApplyFenceControllableComponentsPerCell;
inline constexpr std::size_t
    kMaximumQualifiedApplyFenceControlDomainsPerTransition =
        2U * kMaximumQualifiedApplyFenceControlDomainsPerCell;
inline constexpr std::size_t
    kMaximumQualifiedApplyFenceControlBindingsPerTransition =
        2U * kMaximumQualifiedApplyFenceControlBindingsPerCell;
inline constexpr std::size_t kMaximumQualifiedApplyFenceResourcesPerTransition =
    2U * kMaximumQualifiedApplyFenceResourcesPerCell;

} // namespace manyforge::deployment
