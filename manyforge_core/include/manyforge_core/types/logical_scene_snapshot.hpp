#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/scene_edit.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

inline constexpr std::string_view kLogicalSceneApiVersion =
    "manyforge.logical_scene.v1";
inline constexpr std::string_view kLogicalSceneIdIdentityDomain = "scene-id";
inline constexpr std::string_view kLogicalSceneContentIdentityDomain =
    "logical-scene-content";
inline constexpr std::string_view kCollisionPolicyApiVersion =
    "manyforge.collision_policy.v1";
inline constexpr std::string_view kCollisionPolicyContentIdentityDomain =
    "collision-policy-content";
inline constexpr std::string_view
    kCarriedObjectAttachmentRevisionIdentityDomain =
        "carried-object-attachment-revision";
inline constexpr std::string_view kCanonicalEmptyCollisionPolicyDigest =
    "sha256:59e1687796336598d7256d010902f760beb3c238adba73fe3e5ccd6674d797ac";

/// Complete owner input from which the initial logical-scene lineage is
/// minted after canonical-empty bootstrap acceptance.
///
/// This value intentionally performs no hashing. The logical scene owner must
/// feed its exact normalized value through the one qualified content-identity
/// codec under kLogicalSceneIdIdentityDomain/kLogicalSceneApiVersion.
struct LogicalSceneBootstrapIdentityInputDTO final {
  CellId cell_id;
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !cell_id.empty() && !bootstrap_id.empty() &&
           bootstrap_generation.nonzero();
  }

  friend bool operator==(const LogicalSceneBootstrapIdentityInputDTO&,
                         const LogicalSceneBootstrapIdentityInputDTO&) =
      default;
};

/// Exact canonical fail-closed policy required by initial empty bootstrap.
/// Representation checking is local; the digest is the externally qualified
/// content-identity result, not recomputed in this DTO header.
[[nodiscard]] bool IsCanonicalEmptyCollisionPolicy(
    const CollisionPolicySnapshot& policy) noexcept;

/// Portable cell-frame scene reference. Generated frame names never enter the
/// logical scene contract.
struct CellSceneFrameRefDTO final {
  CellId cell_id;

  [[nodiscard]] bool valid() const noexcept { return !cell_id.empty(); }
  friend bool operator==(const CellSceneFrameRefDTO&,
                         const CellSceneFrameRefDTO&) = default;
};

/// Portable component-owned model-frame reference.
struct ComponentSceneFrameRefDTO final {
  ControllableComponentId controllable_component_id;
  ModelInstanceId model_instance_id;
  FrameRoleId frame_role_id;

  [[nodiscard]] bool valid() const noexcept {
    return !controllable_component_id.empty() && !model_instance_id.empty() &&
           !frame_role_id.empty();
  }
  friend bool operator==(const ComponentSceneFrameRefDTO&,
                         const ComponentSceneFrameRefDTO&) = default;
};

/// Portable unowned passive-model frame reference.
struct PassiveModelSceneFrameRefDTO final {
  ModelInstanceId model_instance_id;
  FrameRoleId frame_role_id;

  [[nodiscard]] bool valid() const noexcept {
    return !model_instance_id.empty() && !frame_role_id.empty();
  }
  friend bool operator==(const PassiveModelSceneFrameRefDTO&,
                         const PassiveModelSceneFrameRefDTO&) = default;
};

/// Closed portable frame-reference union from spec 210 section 1.2.
using SceneFrameRefDTO =
    std::variant<CellSceneFrameRefDTO, ComponentSceneFrameRefDTO,
                 PassiveModelSceneFrameRefDTO>;

struct CellFixedSceneObjectPlacementDTO final {
  CellId cell_id;
  Pose3DTO pose_in_cell;

  friend bool operator==(const CellFixedSceneObjectPlacementDTO&,
                         const CellFixedSceneObjectPlacementDTO&) = default;
};

struct CarriedSceneObjectPlacementDTO final {
  CarriedObjectAttachmentId carried_object_attachment_id;
  CarriedObjectAttachmentRevision carried_object_attachment_revision;

  friend bool operator==(const CarriedSceneObjectPlacementDTO&,
                         const CarriedSceneObjectPlacementDTO&) = default;
};

using SceneObjectPlacementDTO = std::variant<CellFixedSceneObjectPlacementDTO,
                                             CarriedSceneObjectPlacementDTO>;

/// Identity of the provider that owns one provider-backed object snapshot.
/// Presence as one optional record prevents partial provider identity from
/// being represented by the enclosing object. Observation sequence, times and
/// freshness live in the scene runtime, not in the logical scene.
struct SceneObjectProviderEvidenceDTO final {
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  std::string provider_source_epoch;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const SceneObjectProviderEvidenceDTO&,
                         const SceneObjectProviderEvidenceDTO&) = default;
};

/// One portable logical-scene resource row.
struct SceneObjectSnapshotDTO final {
  SceneResourceId scene_resource_id;
  std::vector<CollisionShapeDTO> collision_shapes;
  SceneObjectPlacementDTO placement;
  std::optional<SceneObjectProviderEvidenceDTO> provider_evidence;
  /// Portable authored support relationships retained independently of any
  /// planner's generated collision-body ids.
  std::vector<SupportContactDTO> support_contacts{};

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const SceneObjectSnapshotDTO&,
                         const SceneObjectSnapshotDTO&) = default;
};

/// Grasp-lost bound of a carried object whose offset is measured: a measured
/// offset farther than either bound from the first measured offset of the
/// attachment means the object is no longer held (`carried_object_lost`).
struct CarriedObjectGraspLostBoundDTO final {
  double translation_m{};
  double rotation_rad{};

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(translation_m) && translation_m > 0.0 &&
           std::isfinite(rotation_rad) && rotation_rad > 0.0;
  }
  friend bool operator==(const CarriedObjectGraspLostBoundDTO&,
                         const CarriedObjectGraspLostBoundDTO&) = default;
};

/// What a loss of the attachment does: the attach node's `on_grasp_lost`.
/// The defaults are the CP9 behaviour (alarm, Stop, detach). The
/// `carried_object_lost.<command_target_id>` system signal of the gripper is
/// published on every loss, whatever the flags.
struct GraspLostResponseDTO final {
  /// Raise `scene.carried_object_lost`; requires `stop_motion`.
  bool raise_alarm{true};
  /// Request the carrier's operational Stop.
  bool stop_motion{true};
  /// Blackboard key the Program Runner mirrors the signal into; the scene only
  /// carries it.
  std::optional<std::string> set_key{};
  /// Detach the object at its observed pose.
  bool detach{true};

  /// Refused: an alarm without the Stop, or an empty key name.
  [[nodiscard]] bool valid() const noexcept {
    return (!raise_alarm || stop_motion) &&
           (!set_key ||
            (!set_key->empty() && set_key->find('\0') == std::string::npos));
  }
  [[nodiscard]] bool is_default() const noexcept {
    return *this == GraspLostResponseDTO{};
  }
  friend bool operator==(const GraspLostResponseDTO&,
                         const GraspLostResponseDTO&) = default;
};

/// The attach node's grasp-lost bound and response.
struct CarriedObjectGraspLostCheckDTO final {
  CarriedObjectGraspLostBoundDTO bound;
  /// Absent means the defaults; never present and equal to them, so older
  /// attachments keep their identity.
  std::optional<GraspLostResponseDTO> on_grasp_lost{};

  [[nodiscard]] GraspLostResponseDTO response() const {
    return on_grasp_lost.value_or(GraspLostResponseDTO{});
  }
  friend bool operator==(const CarriedObjectGraspLostCheckDTO&,
                         const CarriedObjectGraspLostCheckDTO&) = default;
};

/// One canonical scene-owned carried-object attachment row.
struct CarriedObjectAttachmentDTO final {
  CarriedObjectAttachmentId carried_object_attachment_id;
  CarriedObjectAttachmentRevision carried_object_attachment_revision;
  SceneResourceId scene_resource_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  SceneFrameRefDTO carrier_frame_ref;
  Pose3DTO pose_in_carrier_frame;
  std::vector<CollisionBodyRoleId> touch_body_roles;
  /// Present when the attach request carried a grasp-lost bound.
  std::optional<CarriedObjectGraspLostCheckDTO> grasp_lost_check{};

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CarriedObjectAttachmentDTO&,
                         const CarriedObjectAttachmentDTO&) = default;
};

/// Exact scene identity carried by an immutable logical snapshot.
///
/// This record does not mint either field. In particular, the canonical-empty
/// SceneId/content-digest preimage is intentionally left to the scene-owner
/// identity contract rather than guessed by this ROS-free value layer.
struct LogicalSceneSnapshotIdentityDTO final {
  SceneRevision revision;
  CellId cell_id;

  [[nodiscard]] bool valid() const noexcept {
    return revision.valid() && !cell_id.empty();
  }
  friend bool operator==(const LogicalSceneSnapshotIdentityDTO&,
                         const LogicalSceneSnapshotIdentityDTO&) = default;
};

/// Frozen portable logical-scene value selected with one exact retained graph.
///
/// Component roots and structural attachment transforms are deliberately
/// absent. They belong to the retained bundle, not to scene state.
struct LogicalSceneSnapshotDTO final {
  SceneRevision revision;
  CellId cell_id;
  std::uint64_t transaction_sequence;
  std::uint64_t topology_epoch;
  std::uint64_t policy_epoch;
  CollisionPolicySnapshot configured_collision_policy;
  std::vector<SceneObjectSnapshotDTO> objects;
  std::vector<CarriedObjectAttachmentDTO> carried_object_attachments;

  [[nodiscard]] LogicalSceneSnapshotIdentityDTO identity() const {
    return {revision, cell_id};
  }

  /// Validates locally decidable shape, ordering, and complete attachment-table
  /// invariants. Content-hash recomputation remains the scene owner's job.
  [[nodiscard]] bool locally_valid() const noexcept;

  [[nodiscard]] bool has_exact_identity(
      const LogicalSceneSnapshotIdentityDTO& expected) const noexcept;

  [[nodiscard]] bool canonical_empty_for(
      const LogicalSceneSnapshotIdentityDTO& expected) const noexcept;

  friend bool operator==(const LogicalSceneSnapshotDTO&,
                         const LogicalSceneSnapshotDTO&) = default;
};

namespace logical_scene_detail {

[[nodiscard]] inline bool FinitePose(const Pose3DTO& pose) noexcept {
  const auto& position = pose.position_m;
  const auto& orientation = pose.orientation;
  if (!std::isfinite(position.x_m) || !std::isfinite(position.y_m) ||
      !std::isfinite(position.z_m) || !std::isfinite(orientation.x) ||
      !std::isfinite(orientation.y) || !std::isfinite(orientation.z) ||
      !std::isfinite(orientation.w)) {
    return false;
  }
  const double norm_squared =
      orientation.x * orientation.x + orientation.y * orientation.y +
      orientation.z * orientation.z + orientation.w * orientation.w;
  return std::isfinite(norm_squared) && norm_squared > 0.0;
}

[[nodiscard]] inline bool PositiveFinite(const double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] inline bool ValidCollisionShape(
    const CollisionShapeDTO& shape) noexcept {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BoxShapeDTO>) {
          return PositiveFinite(value.dimensions_m.x_m) &&
                 PositiveFinite(value.dimensions_m.y_m) &&
                 PositiveFinite(value.dimensions_m.z_m);
        } else if constexpr (std::is_same_v<T, SphereShapeDTO>) {
          return PositiveFinite(value.radius_m);
        } else if constexpr (std::is_same_v<T, CylinderShapeDTO>) {
          return PositiveFinite(value.radius_m) &&
                 PositiveFinite(value.height_m);
        } else {
          return !value.resource_uri.empty() &&
                 PositiveFinite(value.scale.x_m) &&
                 PositiveFinite(value.scale.y_m) &&
                 PositiveFinite(value.scale.z_m);
        }
      },
      shape);
}

[[nodiscard]] inline bool CanonicalPair(const CollisionPairId& pair) noexcept {
  return !pair.body_a.empty() && !pair.body_b.empty() &&
         pair.body_a <= pair.body_b;
}

template <typename T, typename Projection>
[[nodiscard]] bool StrictlyIncreasing(const std::vector<T>& values,
                                      Projection projection) noexcept {
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (projection(values[index]).empty()) {
      return false;
    }
    if (index != 0U &&
        !(projection(values[index - 1U]) < projection(values[index]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool LocallyValidConfiguredPolicy(
    const CollisionPolicySnapshot& policy) noexcept {
  if (policy.policy_id.empty() || policy.revision.revision_id == 0U ||
      !IsExactLowercaseSha256Digest(policy.revision.content_hash)) {
    return false;
  }
  const auto valid_pair_set = [](const auto& pairs) {
    for (std::size_t index = 0U; index < pairs.size(); ++index) {
      if (!CanonicalPair(pairs[index]) ||
          (index != 0U && !(pairs[index - 1U] < pairs[index]))) {
        return false;
      }
    }
    return true;
  };
  if (!valid_pair_set(policy.allowed_static_static_pairs) ||
      !valid_pair_set(policy.allowed_touch_link_pairs)) {
    return false;
  }
  for (std::size_t index = 0U; index < policy.support_contacts.size();
       ++index) {
    const auto& contact = policy.support_contacts[index];
    if (!CanonicalPair(contact.pair) ||
        (index != 0U &&
         !(policy.support_contacts[index - 1U].pair < contact.pair))) {
      return false;
    }
  }
  return StrictlyIncreasing(
      policy.contact_grasps,
      [](const ContactGraspPolicyEntry& grasp) -> const std::string& {
        return grasp.policy_id;
      });
}

[[nodiscard]] inline bool ValidFrameRef(
    const SceneFrameRefDTO& frame) noexcept {
  return std::visit([](const auto& value) { return value.valid(); }, frame);
}

[[nodiscard]] inline bool NormalizedSupportContact(
    const SupportContactDTO& contact) noexcept {
  const auto nonnegative_finite = [](const double value) {
    return std::isfinite(value) && value >= 0.0;
  };
  if (contact.support_object_id.empty() ||
      !nonnegative_finite(contact.max_start_penetration_m) ||
      !nonnegative_finite(contact.release_tolerance_m) ||
      !nonnegative_finite(contact.min_release_progress_m) ||
      !nonnegative_finite(contact.max_contact_penetration_m)) {
    return false;
  }

  switch (contact.mode) {
    case CollisionContactPolicyMode::kContactRelease:
      if (contact.max_contact_penetration_m != 0.0) {
        return false;
      }
      switch (contact.release_evidence_mode) {
        case ContactReleaseEvidenceMode::kSignedPenetration:
          return true;
        case ContactReleaseEvidenceMode::kBinaryContact:
          return !contact.enforce_max_start_penetration &&
                 contact.max_start_penetration_m == 0.0 &&
                 contact.release_tolerance_m == 0.0 &&
                 contact.min_release_progress_m == 0.0 &&
                 !contact.require_strictly_decreasing_penetration;
      }
      return false;
    case CollisionContactPolicyMode::kBoundedPenetration:
    case CollisionContactPolicyMode::kContactProcess:
      return contact.release_evidence_mode ==
                 ContactReleaseEvidenceMode::kSignedPenetration &&
             !contact.enforce_max_start_penetration &&
             contact.max_start_penetration_m == 0.0 &&
             contact.release_tolerance_m == 0.0 &&
             contact.min_release_progress_m == 0.0 &&
             !contact.require_separation_by_end &&
             !contact.require_strictly_decreasing_penetration &&
             !contact.allow_recontact &&
             (contact.mode == CollisionContactPolicyMode::kBoundedPenetration ||
              contact.max_contact_penetration_m == 0.0);
  }
  return false;
}

}  // namespace logical_scene_detail

inline bool IsCanonicalEmptyCollisionPolicy(
    const CollisionPolicySnapshot& policy) noexcept {
  return policy.policy_id == "free_space" &&
         policy.revision.revision_id == 1U &&
         policy.revision.content_hash == kCanonicalEmptyCollisionPolicyDigest &&
         policy.allowed_static_static_pairs.empty() &&
         policy.allowed_touch_link_pairs.empty() &&
         policy.support_contacts.empty() && policy.contact_grasps.empty() &&
         policy.require_full_diagnostics;
}

inline bool SceneObjectProviderEvidenceDTO::valid() const noexcept {
  return !provider_instance_id.empty() && !provider_manifest_id.empty() &&
         !provider_manifest_revision.empty() &&
         !provider_source_epoch.empty();
}

inline bool SceneObjectSnapshotDTO::locally_valid() const noexcept {
  if (scene_resource_id.empty() || collision_shapes.empty() ||
      !std::all_of(collision_shapes.begin(), collision_shapes.end(),
                   logical_scene_detail::ValidCollisionShape) ||
      (provider_evidence.has_value() && !provider_evidence->valid()) ||
      !logical_scene_detail::StrictlyIncreasing(
          support_contacts,
          [](const SupportContactDTO& contact) -> const std::string& {
            return contact.support_object_id;
          }) ||
      !std::all_of(support_contacts.begin(), support_contacts.end(),
                   logical_scene_detail::NormalizedSupportContact)) {
    return false;
  }
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CellFixedSceneObjectPlacementDTO>) {
          return !value.cell_id.empty() &&
                 logical_scene_detail::FinitePose(value.pose_in_cell);
        } else {
          return !value.carried_object_attachment_id.empty() &&
                 !value.carried_object_attachment_revision.empty();
        }
      },
      placement);
}

inline bool CarriedObjectAttachmentDTO::locally_valid() const noexcept {
  if (carried_object_attachment_id.empty() ||
      carried_object_attachment_revision.empty() || scene_resource_id.empty() ||
      controllable_component_id.empty() || command_target_id.empty() ||
      !logical_scene_detail::ValidFrameRef(carrier_frame_ref) ||
      !logical_scene_detail::FinitePose(pose_in_carrier_frame) ||
      (grasp_lost_check && !grasp_lost_check->bound.valid()) ||
      !logical_scene_detail::StrictlyIncreasing(
          touch_body_roles,
          [](const CollisionBodyRoleId& role) -> const std::string& {
            return role.value();
          })) {
    return false;
  }
  const auto* component_frame =
      std::get_if<ComponentSceneFrameRefDTO>(&carrier_frame_ref);
  return component_frame != nullptr &&
         component_frame->controllable_component_id ==
             controllable_component_id;
}

namespace logical_scene_detail {

/// Validates exactly the semantic content covered by
/// ComputeLogicalSceneContentHash. Owner lineage and sequencing fields are
/// deliberately outside this predicate because they are outside that hash.
[[nodiscard]] inline bool LocallyValidSceneContent(
    const LogicalSceneSnapshotDTO& snapshot) noexcept {
  if (snapshot.cell_id.empty() ||
      !LocallyValidConfiguredPolicy(snapshot.configured_collision_policy) ||
      !logical_scene_detail::StrictlyIncreasing(
          snapshot.objects,
          [](const SceneObjectSnapshotDTO& object) -> const std::string& {
            return object.scene_resource_id.value();
          }) ||
      !logical_scene_detail::StrictlyIncreasing(
          snapshot.carried_object_attachments,
          [](const CarriedObjectAttachmentDTO& attachment)
              -> const std::string& {
            return attachment.carried_object_attachment_id.value();
          })) {
    return false;
  }

  for (const auto& object : snapshot.objects) {
    if (!object.locally_valid()) {
      return false;
    }
    if (const auto* placement =
            std::get_if<CellFixedSceneObjectPlacementDTO>(&object.placement);
        placement != nullptr && placement->cell_id != snapshot.cell_id) {
      return false;
    }
  }

  for (const auto& object : snapshot.objects) {
    for (const auto& contact : object.support_contacts) {
      if (contact.support_object_id == object.scene_resource_id.value()) {
        return false;
      }
      const auto support = std::lower_bound(
          snapshot.objects.begin(), snapshot.objects.end(),
          contact.support_object_id,
          [](const SceneObjectSnapshotDTO& candidate,
             const std::string& support_id) {
            return candidate.scene_resource_id.value() < support_id;
          });
      if (support == snapshot.objects.end() ||
          support->scene_resource_id.value() != contact.support_object_id) {
        return false;
      }
      const auto reciprocal = std::lower_bound(
          support->support_contacts.begin(), support->support_contacts.end(),
          object.scene_resource_id.value(),
          [](const SupportContactDTO& candidate,
             const std::string& support_id) {
            return candidate.support_object_id < support_id;
          });
      if (reciprocal != support->support_contacts.end() &&
          reciprocal->support_object_id == object.scene_resource_id.value()) {
        return false;
      }
    }
  }
  for (const auto& attachment : snapshot.carried_object_attachments) {
    if (!attachment.locally_valid()) {
      return false;
    }
  }

  std::size_t carried_object_count = 0U;
  for (const auto& object : snapshot.objects) {
    const auto* placement =
        std::get_if<CarriedSceneObjectPlacementDTO>(&object.placement);
    if (placement == nullptr) {
      continue;
    }
    ++carried_object_count;
    const auto match =
        std::lower_bound(snapshot.carried_object_attachments.begin(),
                         snapshot.carried_object_attachments.end(),
                         placement->carried_object_attachment_id,
                         [](const CarriedObjectAttachmentDTO& attachment,
                            const CarriedObjectAttachmentId& id) {
                           return attachment.carried_object_attachment_id < id;
                         });
    if (match == snapshot.carried_object_attachments.end() ||
        match->carried_object_attachment_id !=
            placement->carried_object_attachment_id ||
        match->carried_object_attachment_revision !=
            placement->carried_object_attachment_revision ||
        match->scene_resource_id != object.scene_resource_id) {
      return false;
    }
  }
  return carried_object_count == snapshot.carried_object_attachments.size();
}

}  // namespace logical_scene_detail

inline bool LogicalSceneSnapshotDTO::locally_valid() const noexcept {
  return revision.valid() &&
         logical_scene_detail::LocallyValidSceneContent(*this);
}

inline bool LogicalSceneSnapshotDTO::has_exact_identity(
    const LogicalSceneSnapshotIdentityDTO& expected) const noexcept {
  return expected.valid() && identity() == expected;
}

inline bool LogicalSceneSnapshotDTO::canonical_empty_for(
    const LogicalSceneSnapshotIdentityDTO& expected) const noexcept {
  return locally_valid() && has_exact_identity(expected) &&
         revision.revision_id == 1U && transaction_sequence == 0U &&
         topology_epoch == 0U && policy_epoch == 0U &&
         IsCanonicalEmptyCollisionPolicy(configured_collision_policy) &&
         objects.empty() && carried_object_attachments.empty();
}

}  // namespace manyforge::core
