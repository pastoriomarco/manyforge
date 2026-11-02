#pragma once

#include <cstdint>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/motion_preview.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <memory>
#include <stop_token>

namespace manyforge::core {

/// One owner-private, non-effectful planning transaction.
///
/// The session holds the addressed Kernel's submission lane for its lifetime,
/// so no ordinary operation can begin between linked preview legs. Stop and
/// lifecycle control use their independent control path and remain available.
/// Public callers never receive this interface or any value returned by
/// prepare_motion().
class IAddressedMotionPreviewSession {
 public:
  virtual ~IAddressedMotionPreviewSession() = default;

  /// Resolve an arbitrary public participant through the exact active
  /// Program Binding. This is used by a composite owner to bind its tool and
  /// carrier roles; no provider or compiled identity comes from public JSON.
  [[nodiscard]] virtual Result<RuntimeParticipantBindingDTO> resolve_binding(
      const OperationIntentDTO& intent) const = 0;

  /// Resolve one motion intent to the same private request consumed by actual
  /// execution. The result is available only to the C++ owner composing the
  /// candidate search.
  [[nodiscard]] virtual Result<ExecuteRequestDTO> prepare_motion(
      const OperationIntentDTO& intent) = 0;

  /// Read from the state authority owned by the exact selected participant.
  [[nodiscard]] virtual Result<RobotStateDTO> current_state() const = 0;

  /// Resolve one bounded set of caller-portable references through the exact
  /// participant and immutable preview state. Runtime-object bindings are
  /// supplied by the composite owner, never inferred by the transport or
  /// planner adapter.
  [[nodiscard]] virtual Result<std::vector<Pose3DTO>> resolve_reference_poses(
      const OperationIntentDTO& motion_context, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& start_state,
      std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings) = 0;

  /// Plan and validate without dispatch, journaling, scene publication, or
  /// operation-state publication.
  [[nodiscard]] virtual Result<MotionPreviewResultDTO> preview_motion(
      OperationIntentDTO intent,
      CollisionWorldLeaseInventory projection_inventory,
      RobotStateDTO start_state,
      std::uint32_t planning_attempt_index = 0U,
      std::stop_token stop_token = {}) = 0;
};

/// Addressed owner of serialized preview sessions. The public operation
/// surface remains OperationIntentDTO; private execution evidence is minted
/// behind this boundary.
class IAddressedMotionPreviewOwner {
 public:
  virtual ~IAddressedMotionPreviewOwner() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<IAddressedMotionPreviewSession>>
  begin_motion_preview_session(const OperationIntentDTO& primary_motion) = 0;
};

}  // namespace manyforge::core
