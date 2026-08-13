#pragma once

#include <cstdint>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Compare one complete fresh observation with its exact expected closure.
/// This operation performs no I/O and never constructs a repair plan.
[[nodiscard]] Result<ControllerLifecycleReconciliationDTO> ReconcileControllerLifecycle(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ControllerLifecycleObservationDTO& observation, std::int64_t now_ns);

/// Build deterministic upstream-first deactivation and hardware-first
/// activation waves for an already accepted route transaction.
/// This operation performs no I/O or mutation.
[[nodiscard]] Result<ControllerLifecycleTransitionPlanDTO> BuildControllerLifecycleTransitionPlan(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ControllerLifecycleObservationDTO& observation, std::int64_t now_ns);

}  // namespace manyforge::core
