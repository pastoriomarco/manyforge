#pragma once

#include <compare>
#include <string>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Authored references identify Deployment features directly. Frame and body
/// IDs are component-local catalog identities; retained model roles remain
/// separate internal facts after resolution.
struct ProgramTargetReferenceDTO final {
  ControllableComponentId component_id;
  CommandTargetId command_target_id;
  friend auto operator<=>(const ProgramTargetReferenceDTO&,
                          const ProgramTargetReferenceDTO&) = default;
};

struct ProgramSignalReferenceDTO final {
  ControllableComponentId component_id;
  std::string signal_name;
  friend auto operator<=>(const ProgramSignalReferenceDTO&,
                          const ProgramSignalReferenceDTO&) = default;
};

struct ProgramFrameReferenceDTO final {
  ControllableComponentId component_id;
  FrameRoleId frame_id;
  friend auto operator<=>(const ProgramFrameReferenceDTO&,
                          const ProgramFrameReferenceDTO&) = default;
};

struct ProgramCollisionBodyReferenceDTO final {
  ControllableComponentId component_id;
  CollisionBodyRoleId collision_body_id;
  friend auto operator<=>(const ProgramCollisionBodyReferenceDTO&,
                          const ProgramCollisionBodyReferenceDTO&) = default;
};

}  // namespace manyforge::core
