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

  [[nodiscard]] virtual Result<MotionPreviewResultDTO> preview_motion(
      ExecuteRequestDTO request, CollisionWorldLeaseInventory projection_inventory,
      RobotStateDTO start_state, std::uint32_t planning_attempt_index = 0U,
      std::stop_token stop_token = {}) = 0;
};

}  // namespace manyforge::core
