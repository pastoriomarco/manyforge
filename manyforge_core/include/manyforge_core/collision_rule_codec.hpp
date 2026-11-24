#pragma once

#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/collision_rule.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// The one JSON shape of collision rules (authored rows, the motion payload
/// and compiled configuration):
/// `{rule_id, a?, b?, clearance_m | contact{max_penetration_m?,
/// end_from_fraction?, max_compression_step_m?, spring_group?{component_id,
/// element_id, axis[3]}} | release{...}}`; selections
/// `{kind: component, component_id}`,
/// `{kind: collision_body, component_id, collision_body_id}`,
/// `{kind: link, component_id, link}`, `{kind: model_group, component_id,
/// group}`, `{kind: attached_object, component_id, collision_body_role_id?}`,
/// `{kind: scene_objects}`, `{kind: scene_object, scene_resource_id}`,
/// `{kind: active_tcp_contact_bodies}`. The row without selections has no `a`
/// and no `b`. A contact without `end_from_fraction` reads as
/// kDefaultContactEndFromFraction (0 is the whole move); the key is always
/// written. A release has every key, written and read.
[[nodiscard]] content_identity::CanonicalValue CollisionRulesValue(
    const std::vector<CollisionRuleDTO>& rules);

/// Admitted rules, for identities and records only: resolved collision
/// bodies are written as `{kind: collision_bodies, collision_body_ids}`, the
/// objects carried by resolved bodies as `{kind: attached_object,
/// parent_body_ids}`.
[[nodiscard]] content_identity::CanonicalValue CollisionRulesValue(
    const std::vector<CollisionRule>& rules);

/// Closed-shape decode followed by `collision_rules_shape_violation`. The
/// error is `invalid_collision_rule` with `reason` and, for a row, `rule_index`.
[[nodiscard]] Result<std::vector<CollisionRuleDTO>> DecodeCollisionRules(
    const content_identity::CanonicalValue& value);

}  // namespace manyforge::core
