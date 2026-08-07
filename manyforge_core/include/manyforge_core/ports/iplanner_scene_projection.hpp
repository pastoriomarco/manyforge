#pragma once

#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Focused native participant in the logical-scene commit protocol.
///
/// N5b establishes transaction acknowledgement and health. Revision-bound
/// leases, rebuild, structural stage/activation, and native query projection
/// are added through this same focused boundary in N5c.
class IPlannerSceneProjection {
 public:
  virtual ~IPlannerSceneProjection() = default;

  [[nodiscard]] virtual ProjectionCommitResultDTO ApplyTransaction(
      const SceneTransactionDTO& transaction) = 0;

  [[nodiscard]] virtual ProjectionHealthDTO ProjectionHealth() const = 0;
};

}  // namespace manyforge::core
