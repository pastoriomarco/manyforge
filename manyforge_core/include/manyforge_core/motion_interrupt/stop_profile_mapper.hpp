#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/stop_control.hpp>

#include <span>

namespace manyforge::core {

/// Pure NRT Prepare-time mapping from one portable source-joint profile to the
/// complete selected provider-contract alternative. `resources` is the exact
/// affected target projection and `retained_default_limits` is the complete
/// deployment-owned settle-threshold table for that participant.
[[nodiscard]] Result<MotionControlledStopContractParametersDTO>
map_motion_interrupt_stop_profile_to_contract_parameters(
    const MotionInterruptStopProfileDTO& profile,
    MotionInterruptNodeCategory node_category,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits);

}  // namespace manyforge::core
