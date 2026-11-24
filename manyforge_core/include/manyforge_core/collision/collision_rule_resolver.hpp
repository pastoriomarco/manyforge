#pragma once

#include <memory>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_rule.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// One move's resolved collision-rule table.
struct ResolvedCollisionRules {
  /// Pairs under a clearance rule with a clearance above 0. Null when there
  /// are none.
  std::shared_ptr<const CollisionClearanceMargins> clearance_margins;
  /// Pairs under a contact or release rule, sorted by pair, unique.
  std::vector<CollisionPairContactRule> contact_rules;
};

/// Resolve the cell rules and one move's rules against the move's collision
/// world, once, at admission.
///
/// 1. The cell's first rule (no selections) gives its clearance to every
///    robot x environment pair: a component body or a carried Scene object
///    against a detached Scene object or a runtime object.
/// 2. Every other cell rule gives its value to exactly its pairs (one member
///    of `a`, one of `b`).
/// 3. A move rule with selections replaces the whole value of exactly its
///    pairs. The move rule without selections replaces the value of the pairs
///    the cell's first rule still holds.
///
/// A pair of two static bodies (`base_policy.static_body_ids`) and a pair in
/// `base_policy.allowed_touch_link_pairs` never get a value. A pair in
/// `base_policy.contact_rules` keeps its rule unless a cell or move rule
/// holds it, and gets no clearance from the first rule. Two cell rules
/// with selections, or two move rules, that hold the same pair are refused
/// (`collision_rules_overlap`, with the pair and both rule ids). A component,
/// collision body, link or Scene object selection that names nothing in the
/// world is refused (`collision_rule_selection_unresolved`); an attached
/// object selection may be empty. The objects carried by resolved bodies are
/// the carried Scene objects whose `parent_body_id` is one of those bodies.
[[nodiscard]] Result<ResolvedCollisionRules> resolve_collision_rules(
    const CollisionWorldSnapshot& world, const CollisionPolicySnapshot& base_policy,
    const std::vector<CollisionRule>& cell_rules, const std::vector<CollisionRule>& move_rules);

/// The pairs of two selections in a collision world (one body of `a`, one of
/// `b`), canonical, sorted and unique; the same resolution a rule's pairs get.
/// Refused (`collision_rule_selection_unresolved`) when a component,
/// collision body, link or Scene object selection names nothing; an attached
/// object selection may be empty, and then holds no pair.
[[nodiscard]] Result<std::vector<CollisionPairId>> resolve_collision_selection_pairs(
    const CollisionWorldSnapshot& world, const std::vector<CollisionRuleSelection>& a,
    const std::vector<CollisionRuleSelection>& b);

}  // namespace manyforge::core
