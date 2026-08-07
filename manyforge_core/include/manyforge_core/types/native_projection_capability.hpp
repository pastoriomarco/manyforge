#pragma once

#include <array>
#include <manyforge_core/types/capability_definition.hpp>
#include <string_view>

namespace manyforge::core {

/// Exact capability contracts whose provider process must participate in the
/// live logical-scene projection handoff.
///
/// This list is deliberately closed. ID prefixes are presentation taxonomy,
/// not runtime authority: adding another planning/validation/collision
/// capability does not silently make it a native projection participant.
inline constexpr std::array<std::string_view, 12U> kNativeProjectionCapabilityIds{{
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.motion.named_target",
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
    "planning.trajectory.time_parameterized",
    "validation.trajectory.non_collision",
    "collision.query.pre_execution",
    "collision.query.runtime",
    "collision.query.detailed",
}};

/// Returns true only for one exact build-qualified capability ID/revision pair
/// in the closed native-projection set.
[[nodiscard]] inline bool RequiresNativeSceneProjection(
    const CapabilityId& capability_id,
    const CapabilityDefinitionRevision& capability_revision) noexcept {
  const auto* descriptor = find_builtin_capability_definition(capability_id.value());
  if (descriptor == nullptr ||
      descriptor->capability_definition_revision != capability_revision.value()) {
    return false;
  }
  for (const auto candidate : kNativeProjectionCapabilityIds) {
    if (candidate == capability_id.value()) {
      return true;
    }
  }
  return false;
}

}  // namespace manyforge::core
