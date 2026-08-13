#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/resolved_stop_base_plan.hpp>

namespace manyforge::core {

/// Immutable content-addressed repository for one fully resolved Stop base
/// plan. This port owns no active/current alias, endpoint discovery, runtime
/// choice, or Stop-time operation.
class IResolvedStopBasePlanRepository {
 public:
  virtual ~IResolvedStopBasePlanRepository() = default;

  /// Validate and normalize the complete DTO, derive its sole content ID,
  /// publish the canonical bytes if absent, and independently read them back.
  [[nodiscard]] virtual Result<ResolvedStopBasePlanId> PutIfAbsent(
      ResolvedStopBasePlanDTO plan) = 0;

  /// Load one complete canonical value by its exact content ID and repeat all
  /// byte, shape, normalization, bound, and identity checks.
  [[nodiscard]] virtual Result<ResolvedStopBasePlanDTO> LoadVerified(
      const ResolvedStopBasePlanId& resolved_stop_base_plan_id) const = 0;
};

}  // namespace manyforge::core
