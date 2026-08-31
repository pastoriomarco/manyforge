#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <span>
#include <string_view>

namespace manyforge::core {

[[nodiscard]] constexpr MotionControlledStopMode provider_contract_stop_mode(
    MotionInterruptStopMode value) noexcept {
  switch (value) {
    case MotionInterruptStopMode::kSoftOnPath:
      return MotionControlledStopMode::kSoftOnPath;
    case MotionInterruptStopMode::kSpringBackHold:
      return MotionControlledStopMode::kSpringBackHold;
    case MotionInterruptStopMode::kHardDeceleration:
      return MotionControlledStopMode::kHardDeceleration;
  }
  return MotionControlledStopMode::kHardDeceleration;
}

/// Pure NRT Prepare-time mapping from one portable source-joint profile to the
/// complete selected provider-contract alternative. `resources` is the exact
/// affected target projection and `retained_default_limits` is the complete
/// deployment-owned settle-threshold table for that participant.
[[nodiscard]] Result<MotionControlledStopContractParametersDTO>
map_motion_interrupt_stop_profile_to_contract_parameters(
    const MotionInterruptStopProfileDTO& profile, MotionInterruptNodeCategory node_category,
    std::span<const RuntimeResourceBindingDTO> resources,
    std::span<const HardStopResourceLimitDTO> retained_default_limits);

/// Reconstruct the exact backend profile selected by a verified prepared
/// provider-contract stage. The deployment contract is resource-addressed;
/// the portable execution contract remains source-joint-addressed, so this is
/// the one shared inverse projection used by every lane adapter.
[[nodiscard]] Result<MotionInterruptStopProfileDTO>
map_motion_controlled_stop_contract_parameters_to_profile(
    const JsonObjectDTO& parameters, std::span<const RuntimeResourceBindingDTO> resources,
    std::string_view profile_id);

/// Project a validated portable source-joint profile into the exact compiled
/// joint namespace used by one retained execution backend. This ephemeral
/// adapter-local value is for trajectory planning/dispatch only; retained
/// profiles and provider contracts remain source/resource addressed.
[[nodiscard]] Result<MotionInterruptStopProfileDTO>
project_motion_interrupt_stop_profile_to_compiled_resources(
    const MotionInterruptStopProfileDTO& profile,
    std::span<const RuntimeResourceBindingDTO> resources);

/// One lane-neutral audit projection for the exact controlled-Stop provider
/// outcome. Backend adapters append this object to their retained evidence
/// item instead of maintaining parallel native/ROS field lists.
[[nodiscard]] JsonValueDTO motion_controlled_stop_evidence(
    const MotionInterruptStopProfileDTO& requested_profile, const ControlledStopResultDTO& result,
    std::string_view stop_evidence_policy_revision, std::string_view parameter_source);

}  // namespace manyforge::core
