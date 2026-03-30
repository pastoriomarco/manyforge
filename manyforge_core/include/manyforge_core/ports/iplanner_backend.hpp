#pragma once

#include <manyforge_core/types/plan_request.hpp>

namespace manyforge::core {

/// Planner backend interface (spec 220 §2)
/// 
/// Responsibilities:
/// - Plan motions from a PlanRequestDTO and return a time-parameterized TrajectoryDTO
/// - Planner-specific knobs live in deployment configuration, not in the program artifact
///
/// Threading:
/// - The kernel MUST NOT issue concurrent Plan() calls to the same IPlannerBackend instance
/// - A Plan() call in progress MUST be cancelable by the kernel
class IPlannerBackend {
 public:
    virtual ~IPlannerBackend() = default;

    /// Plan a trajectory from the current (or specified) state to the goal.
    /// 
    /// @param request The planning request with goal and configuration
    /// @return PlanResultDTO containing the trajectory or error
    virtual PlanResultDTO plan(const PlanRequestDTO& request) = 0;
};

}  // namespace manyforge::core
