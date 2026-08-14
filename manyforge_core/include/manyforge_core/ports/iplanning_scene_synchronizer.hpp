#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::core {

/// Optional non-real-time join to the sole externally owned scene authority.
///
/// A production adapter uses this boundary to latch qualified observation
/// policies immediately before a fresh planning snapshot is retained. The
/// returned revision is the exact post-latch projection that the local native
/// projection must expose. Implementations must be bounded, replay-safe, and
/// fail closed when owner identity, freshness, or projection convergence is
/// uncertain.
class IPlanningSceneSynchronizer {
 public:
  virtual ~IPlanningSceneSynchronizer() = default;

  [[nodiscard]] virtual Result<ProjectionRevisionDTO>
  SynchronizeBeforePlanning() = 0;
};

}  // namespace manyforge::core
