#pragma once

#include <manyforge_core/types/collision.hpp>

namespace manyforge::core {

class ICollisionDiagnosticComparator {
 public:
  virtual ~ICollisionDiagnosticComparator() = default;

  virtual CollisionResult compare(const CollisionQuery& query) const = 0;
};

}  // namespace manyforge::core
