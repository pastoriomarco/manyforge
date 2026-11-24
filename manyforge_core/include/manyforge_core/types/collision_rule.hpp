#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/program_reference.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Collision rules (OD-46). One rule holds every pair made of one member of
/// `a` and one member of `b` and gives those pairs one value: a clearance, a
/// contact allowance or a release. Cell rules come from the Deployment
/// (`collision_rules`), move rules from the motion node (`collision_rules`).
///
/// The first cell rule has no selections: it holds every robot x environment
/// pair (a carried object counts as robot) and its value is a clearance. A
/// move row without selections replaces that value for the move.
///
/// Pairs the models never check (SRDF disabled pairs, attachment
/// `collision_exclusions`), static-static pairs and the grip pairs of an
/// attachment (`touch_bodies`) are outside the table.

inline constexpr std::size_t kMaxCollisionRules = 64U;
inline constexpr std::size_t kMaxCollisionRuleSelections = 16U;
inline constexpr std::size_t kMaxCollisionRuleIdBytes = 128U;
inline constexpr std::string_view kEverythingCollisionRuleId = "everything";

/// Every collision body of one component.
struct CollisionRuleComponentSelection final {
  ControllableComponentId component_id;
  friend bool operator==(const CollisionRuleComponentSelection&,
                         const CollisionRuleComponentSelection&) = default;
};

/// One collision body (role) of one component, as authored.
struct CollisionRuleCollisionBodySelection final {
  ProgramCollisionBodyReferenceDTO body;
  friend bool operator==(const CollisionRuleCollisionBodySelection&,
                         const CollisionRuleCollisionBodySelection&) = default;
};

/// One link of one component, by its name in the cell model.
struct CollisionRuleLinkSelection final {
  ControllableComponentId component_id;
  std::string link;
  friend bool operator==(const CollisionRuleLinkSelection&,
                         const CollisionRuleLinkSelection&) = default;
};

/// The links of one SRDF group of a component's model, by the group's name
/// in the component's source model (one of the component's planning groups).
/// Cell rules only: the execution host resolves it to the collision bodies of
/// the group's links.
struct CollisionRuleModelGroupSelection final {
  ControllableComponentId component_id;
  std::string group;
  friend bool operator==(const CollisionRuleModelGroupSelection&,
                         const CollisionRuleModelGroupSelection&) = default;
};

/// Every Scene object the component currently carries; with
/// `collision_body_role_id`, only the objects carried by that collision body
/// (role) of the component (OD-371).
struct CollisionRuleAttachedObjectSelection final {
  ControllableComponentId component_id;
  std::optional<std::string> collision_body_role_id{};
  friend bool operator==(const CollisionRuleAttachedObjectSelection&,
                         const CollisionRuleAttachedObjectSelection&) = default;
};

/// Every detached Scene object.
struct CollisionRuleSceneObjectsSelection final {
  friend bool operator==(const CollisionRuleSceneObjectsSelection&,
                         const CollisionRuleSceneObjectsSelection&) = default;
};

/// One Scene object. Move rules only.
struct CollisionRuleSceneObjectSelection final {
  SceneResourceId scene_resource_id;
  friend bool operator==(const CollisionRuleSceneObjectSelection&,
                         const CollisionRuleSceneObjectSelection&) = default;
};

/// The contact bodies of the move's TCP. Move rules only.
struct CollisionRuleActiveTcpContactBodiesSelection final {
  friend bool operator==(const CollisionRuleActiveTcpContactBodiesSelection&,
                         const CollisionRuleActiveTcpContactBodiesSelection&) = default;
};

/// Collision bodies already resolved to the ids of the retained collision
/// world. Never authored: admission produces it from a collision body or the
/// TCP's contact bodies.
struct CollisionRuleResolvedBodiesSelection final {
  /// Sorted, unique.
  std::vector<CollisionBodyId> collision_body_ids;
  friend bool operator==(const CollisionRuleResolvedBodiesSelection&,
                         const CollisionRuleResolvedBodiesSelection&) = default;
};

/// The Scene objects carried by these collision bodies (their
/// `parent_body_id`), as ids of the retained collision world. Never authored:
/// admission produces it from an attached object selection with a collision
/// body role.
struct CollisionRuleResolvedAttachedObjectSelection final {
  /// Sorted, unique.
  std::vector<CollisionBodyId> parent_body_ids;
  friend bool operator==(const CollisionRuleResolvedAttachedObjectSelection&,
                         const CollisionRuleResolvedAttachedObjectSelection&) = default;
};

/// Selection as authored and as sent with a motion.
using CollisionRuleSelectionDTO =
    std::variant<CollisionRuleComponentSelection, CollisionRuleCollisionBodySelection,
                 CollisionRuleLinkSelection, CollisionRuleAttachedObjectSelection,
                 CollisionRuleSceneObjectsSelection, CollisionRuleSceneObjectSelection,
                 CollisionRuleActiveTcpContactBodiesSelection, CollisionRuleModelGroupSelection>;

/// Selection after admission: collision bodies, model groups and TCP contact
/// bodies are ids, an attached object selection with a collision body role is
/// the ids of that role's bodies; the others are resolved against the move's
/// collision world. An admitted attached object selection has no role.
using CollisionRuleSelection =
    std::variant<CollisionRuleComponentSelection, CollisionRuleResolvedBodiesSelection,
                 CollisionRuleLinkSelection, CollisionRuleAttachedObjectSelection,
                 CollisionRuleSceneObjectsSelection, CollisionRuleSceneObjectSelection,
                 CollisionRuleResolvedAttachedObjectSelection>;

/// Value `clearance_m` of a collision rule, in [0, kMaxCollisionClearanceM].
struct CollisionClearanceRule final {
  double clearance_m = 0.0;
  friend bool operator==(const CollisionClearanceRule&,
                         const CollisionClearanceRule&) = default;
};

using CollisionRuleValue =
    std::variant<CollisionClearanceRule, CollisionContactRule, CollisionReleaseRule>;

/// One collision rule as authored and as sent with a motion. `a` and `b` are
/// both empty (the row without selections, clearance only) or both non-empty.
struct CollisionRuleDTO final {
  std::string rule_id;
  std::vector<CollisionRuleSelectionDTO> a;
  std::vector<CollisionRuleSelectionDTO> b;
  CollisionRuleValue value{CollisionClearanceRule{}};

  friend bool operator==(const CollisionRuleDTO&, const CollisionRuleDTO&) = default;
};

/// One collision rule after admission.
struct CollisionRule final {
  std::string rule_id;
  std::vector<CollisionRuleSelection> a;
  std::vector<CollisionRuleSelection> b;
  CollisionRuleValue value{CollisionClearanceRule{}};

  [[nodiscard]] bool has_selections() const noexcept { return !a.empty() || !b.empty(); }

  friend bool operator==(const CollisionRule&, const CollisionRule&) = default;
};

/// Shape check shared by every ingress: unique non-empty ids within the
/// bounds, `a` and `b` both empty or both non-empty within the bound, a row
/// without selections only with a clearance value and at most once, every
/// number finite and in range (clearance 0..kMaxCollisionClearanceM,
/// max_penetration_m >= 0, end_from_fraction 0..1, release numbers 0..1000,
/// a spring group only on a spring with a finite unit axis and non-empty ids;
/// a release with binary evidence keeps the signed-only fields neutral:
/// no start limit, no progress, tolerance 0, not strictly decreasing).
/// Returns the reason code of the first violation, empty when valid.
[[nodiscard]] std::string_view collision_rules_shape_violation(
    const std::vector<CollisionRuleDTO>& rules) noexcept;
[[nodiscard]] std::string_view collision_rules_shape_violation(
    const std::vector<CollisionRule>& rules) noexcept;

}  // namespace manyforge::core
