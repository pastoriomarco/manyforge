#pragma once

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/validation.hpp>

namespace manyforge::core {

class ITrajectoryValidator {
 public:
  virtual ~ITrajectoryValidator() = default;

  virtual ValidationResultDTO validate_non_collision(
      const TrajectoryDTO& trajectory, const ValidationOptionsDTO& options,
      const CollisionCancellationToken& cancel) const = 0;
};

}  // namespace manyforge::core
