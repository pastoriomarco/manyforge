#pragma once

#include <cstdint>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/motion_preview.hpp>
#include <stop_token>

namespace manyforge::core {

/// Narrow owner-private planning surface used by deterministic multi-leg
/// candidate synthesis. Implementations plan and validate against the exact
/// supplied state and projection without dispatching or publishing effects.
class IMotionPreview {
 public:
  virtual ~IMotionPreview() = default;

  /// Capture the authoritative state selected by this exact participant.
  /// Candidate owners use this instead of accepting caller-supplied joint
  /// arrays or reaching around the participant to a second state source.
  [[nodiscard]] virtual Result<RobotStateDTO> current_preview_state(
      std::int64_t now_ns) const = 0;

  /// Resolve one bounded set of portable spatial references against the same
  /// immutable Program, state, and retained-scene evidence used by motion admission.
  /// Composite planners use this only to capture an offset basis before a
  /// linked preview; it performs no planning, dispatch, or publication.
  [[nodiscard]] virtual Result<std::vector<Pose3DTO>> resolve_preview_reference_poses(
      ExecuteRequestDTO request, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& start_state) {
    static_cast<void>(request);
    static_cast<void>(references);
    static_cast<void>(projection_inventory);
    static_cast<void>(start_state);
    return Result<std::vector<Pose3DTO>>::Err(
        ErrorDTO{"missing_reference",
                 "motion preview reference resolution is unavailable",
                 {{"reason", "motion_preview_reference_resolver_unavailable"}}});
  }

  [[nodiscard]] virtual Result<MotionPreviewResultDTO> preview_motion(
      ExecuteRequestDTO request, CollisionWorldLeaseInventory projection_inventory,
      RobotStateDTO start_state, std::uint32_t planning_attempt_index = 0U,
      std::stop_token stop_token = {}) = 0;
};

}  // namespace manyforge::core
