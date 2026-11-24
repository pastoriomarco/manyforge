#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/collision_rule.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment {

/// A `collision_rules` list that does not have the authored shape. `reason()`
/// is the typed reason code; `what()` names the path and the violation.
class CollisionRuleCodecError final : public std::invalid_argument {
public:
  CollisionRuleCodecError(std::string reason, const std::string &message)
      : std::invalid_argument(message), reason_(std::move(reason)) {}
  [[nodiscard]] const std::string &reason() const noexcept { return reason_; }

private:
  std::string reason_;
};

/// Where a `collision_rules` list is authored. It decides the allowed
/// selections and row keys:
/// - kCell (Deployment): row 0 is the row without selections and no other
///   row is; no `scene_object`, no `active_tcp_contact_bodies`, no `leg`;
///   `model_group {component_id, group}` (an SRDF group of the component's
///   model) only here.
///   A move's `scene_object` names `scene_resource_id`, or `blackboard_key`
///   with an optional `field` of that key.
/// - kMove (motion node): at most one row without selections; no `leg`; a
///   row may name `spring_slider {component_id, element_id}` (a generated
///   tool's spring slider, OD-371) in place of `a`, with `b` and an optional
///   `contact` holding `end_from_fraction` only.
/// - kSequence (pick and place sequence nodes): as kMove, and every row
///   names its `leg` (`approach`, `descend` or `retreat`).
enum class CollisionRuleRows { kCell, kMove, kSequence };

/// One `scene_object` selection that reads the blackboard: the key itself
/// (type scene_resource), or one field of a structured key.
struct CollisionRuleBlackboardSceneObject final {
  std::string blackboard_key;
  std::optional<std::string> field;

  friend bool operator==(const CollisionRuleBlackboardSceneObject &,
                         const CollisionRuleBlackboardSceneObject &) = default;
};

struct DecodedCollisionRuleRow final {
  /// A Scene object selected through the blackboard has an empty
  /// `scene_resource_id` here; its key is in `blackboard_scene_objects`.
  /// A `spring_slider` row has an empty `a` and a contact value holding its
  /// `end_from_fraction` only: the motion spells it out from the slider.
  core::CollisionRuleDTO rule;
  /// The row's `spring_slider`: the tool and the slider element, which is
  /// also the slider's collision body role.
  std::optional<core::ProgramCollisionBodyReferenceDTO> spring_slider;
  /// kSequence only.
  std::optional<std::string> leg;
};

struct DecodedCollisionRules final {
  std::vector<DecodedCollisionRuleRow> rows;
  /// Every `scene_object` selected through the blackboard, in row order.
  std::vector<CollisionRuleBlackboardSceneObject> blackboard_scene_objects;
};

/// Decode and check one authored `collision_rules` list: at most
/// `core::kMaxCollisionRules` rows; unique non-empty rule ids within
/// `core::kMaxCollisionRuleIdBytes`; `a` and `b` both absent or both with 1 to
/// `core::kMaxCollisionRuleSelections` selections; exactly one value key
/// (`clearance_m`, `contact`, `release`); a row without selections only with
/// `clearance_m`; every number finite and in range. A release row keeps the
/// release envelope's own validity rules (binary evidence carries no signed
/// limits). Throws CollisionRuleCodecError.
[[nodiscard]] DecodedCollisionRules decode_collision_rules(
    const CanonicalValue &value, CollisionRuleRows rows, std::string_view path);

/// The Deployment's rules (kCell) as plain rules.
[[nodiscard]] std::vector<core::CollisionRuleDTO> decode_cell_collision_rules(
    const CanonicalValue &value, std::string_view path);

/// The authored shape of `rules`, release rows with every key spelled out.
[[nodiscard]] CanonicalValue
encode_collision_rules(const std::vector<core::CollisionRuleDTO> &rules);

} // namespace manyforge::deployment
