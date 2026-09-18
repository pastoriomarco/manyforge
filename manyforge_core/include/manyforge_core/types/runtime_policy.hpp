#pragma once

#include <cstddef>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kRuntimePolicyApiVersion =
    "manyforge.runtime_policy.v1";
inline constexpr std::size_t kMaxRuntimePolicyRules = 256U;
inline constexpr std::size_t kMaxRuntimePolicyDescriptionBytes = 4096U;

enum class OpenLoopGraspEnvironmentDTO {
  kSimulation = 0,
  kCommissionedHardware,
};

/// Explicit risk acknowledgement. Only an exact positive row for the current
/// environment may authorize a consumer that otherwise lacks independent
/// grasp confirmation.
struct RuntimeOpenLoopGraspAcknowledgementDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  OpenLoopGraspEnvironmentDTO environment{
      OpenLoopGraspEnvironmentDTO::kSimulation};
  bool allowed{};

  friend bool operator==(const RuntimeOpenLoopGraspAcknowledgementDTO&,
                         const RuntimeOpenLoopGraspAcknowledgementDTO&) =
      default;
};

/// Exact first generic runtime-policy catalog payload.
///
/// The record deliberately restores only the runtime policy facts already
/// required by maintained products: explicit open-loop grasp
/// acknowledgements and the exact interrupt-policy set from
/// which prepared operation scopes may be selected. It is not an extension
/// object and cannot carry lifecycle, route, reset, scene, or admission
/// authority.
struct RuntimePolicyDTO final {
  RuntimePolicyId runtime_policy_id;
  RuntimePolicyRevision runtime_policy_revision;
  std::optional<std::string> description;
  std::vector<InterruptPolicyId> interrupt_policy_ids;
  std::vector<RuntimeOpenLoopGraspAcknowledgementDTO>
      open_loop_grasp_acknowledgements;

  friend bool operator==(const RuntimePolicyDTO&,
                         const RuntimePolicyDTO&) = default;
};

}  // namespace manyforge::core
