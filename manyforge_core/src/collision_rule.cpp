#include <manyforge_core/types/collision_rule.hpp>

#include <cmath>
#include <set>
#include <type_traits>

namespace manyforge::core {
namespace {

inline constexpr double kMaxCollisionReleaseMetricM = 1000.0;

[[nodiscard]] bool non_negative(const double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

/// A spring group names its slider and has a unit axis (OD-371).
[[nodiscard]] bool spring_group_valid(const CollisionSpringGroup& group) noexcept {
  const auto& axis = group.axis;
  const double norm = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  return !group.component_id.empty() && !group.element_id.empty() && std::isfinite(norm) &&
         std::abs(norm - 1.0) <= 1e-6;
}

/// Sorted, unique, non-empty ids.
[[nodiscard]] bool body_ids_valid(const std::vector<CollisionBodyId>& ids) noexcept {
  if (ids.empty()) {
    return false;
  }
  for (std::size_t index = 0U; index < ids.size(); ++index) {
    if (ids[index].empty() || (index > 0U && !(ids[index - 1U] < ids[index]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool value_valid(const CollisionRuleValue& value) noexcept {
  return std::visit(
      [](const auto& rule) {
        using Rule = std::decay_t<decltype(rule)>;
        if constexpr (std::is_same_v<Rule, CollisionClearanceRule>) {
          return non_negative(rule.clearance_m) && rule.clearance_m <= kMaxCollisionClearanceM;
        } else if constexpr (std::is_same_v<Rule, CollisionContactRule>) {
          // A spring's step limit needs its stroke (the depth limit).
          return (!rule.max_penetration_m || non_negative(*rule.max_penetration_m)) &&
                 (!rule.max_compression_step_m ||
                  (rule.max_penetration_m && non_negative(*rule.max_compression_step_m) &&
                   *rule.max_compression_step_m > 0.0 &&
                   *rule.max_compression_step_m <= kMaxCollisionReleaseMetricM)) &&
                 (!rule.spring_group ||
                  (rule.max_compression_step_m && spring_group_valid(*rule.spring_group))) &&
                 non_negative(rule.end_from_fraction) && rule.end_from_fraction <= 1.0;
        } else {
          const auto metric = [](const double number) {
            return non_negative(number) && number <= kMaxCollisionReleaseMetricM;
          };
          if (!metric(rule.max_start_penetration_m) || !metric(rule.release_tolerance_m) ||
              !metric(rule.min_release_progress_m)) {
            return false;
          }
          // Binary evidence has no depth: the signed-only fields stay neutral.
          return rule.release_evidence_mode == ContactReleaseEvidenceMode::kSignedPenetration ||
                 (!rule.enforce_max_start_penetration && rule.max_start_penetration_m == 0.0 &&
                  rule.min_release_progress_m == 0.0 && rule.release_tolerance_m == 0.0 &&
                  !rule.require_strictly_decreasing_penetration);
        }
      },
      value);
}

[[nodiscard]] bool selection_valid(const CollisionRuleSelectionDTO& selection) noexcept {
  return std::visit(
      [](const auto& selected) {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, CollisionRuleComponentSelection>) {
          return !selected.component_id.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleAttachedObjectSelection>) {
          return !selected.component_id.empty() &&
                 (!selected.collision_body_role_id || !selected.collision_body_role_id->empty());
        } else if constexpr (std::is_same_v<Selected, CollisionRuleCollisionBodySelection>) {
          return !selected.body.component_id.empty() && !selected.body.collision_body_id.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleLinkSelection>) {
          return !selected.component_id.empty() && !selected.link.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleModelGroupSelection>) {
          return !selected.component_id.empty() && !selected.group.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleSceneObjectSelection>) {
          return !selected.scene_resource_id.empty();
        } else {
          return true;
        }
      },
      selection);
}

[[nodiscard]] bool selection_valid(const CollisionRuleSelection& selection) noexcept {
  return std::visit(
      [](const auto& selected) {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, CollisionRuleComponentSelection>) {
          return !selected.component_id.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleAttachedObjectSelection>) {
          // Admission turns a collision body role into its bodies.
          return !selected.component_id.empty() && !selected.collision_body_role_id;
        } else if constexpr (std::is_same_v<Selected, CollisionRuleResolvedBodiesSelection>) {
          return body_ids_valid(selected.collision_body_ids);
        } else if constexpr (std::is_same_v<Selected,
                                            CollisionRuleResolvedAttachedObjectSelection>) {
          return body_ids_valid(selected.parent_body_ids);
        } else if constexpr (std::is_same_v<Selected, CollisionRuleLinkSelection>) {
          return !selected.component_id.empty() && !selected.link.empty();
        } else if constexpr (std::is_same_v<Selected, CollisionRuleSceneObjectSelection>) {
          return !selected.scene_resource_id.empty();
        } else {
          return true;
        }
      },
      selection);
}

template <typename Rule>
[[nodiscard]] std::string_view shape_violation(const std::vector<Rule>& rules) noexcept {
  if (rules.size() > kMaxCollisionRules) {
    return "collision_rules_bound_exceeded";
  }
  bool row_without_selections = false;
  for (std::size_t index = 0U; index < rules.size(); ++index) {
    const auto& rule = rules[index];
    if (rule.rule_id.empty() || rule.rule_id.size() > kMaxCollisionRuleIdBytes) {
      return "collision_rule_id_invalid";
    }
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      if (rules[earlier].rule_id == rule.rule_id) {
        return "collision_rule_id_duplicate";
      }
    }
    if (rule.a.empty() != rule.b.empty() || rule.a.size() > kMaxCollisionRuleSelections ||
        rule.b.size() > kMaxCollisionRuleSelections) {
      return "collision_rule_selection_invalid";
    }
    for (const auto* side : {&rule.a, &rule.b}) {
      for (const auto& selection : *side) {
        if (!selection_valid(selection)) {
          return "collision_rule_selection_invalid";
        }
      }
    }
    if (!value_valid(rule.value)) {
      return "collision_rule_value_invalid";
    }
    if (rule.a.empty()) {
      // The row without selections: a clearance, at most once.
      if (!std::holds_alternative<CollisionClearanceRule>(rule.value)) {
        return "collision_rule_value_invalid";
      }
      if (row_without_selections) {
        return "collision_rule_selection_invalid";
      }
      row_without_selections = true;
    }
  }
  return {};
}

}  // namespace

std::string_view collision_rules_shape_violation(
    const std::vector<CollisionRuleDTO>& rules) noexcept {
  return shape_violation(rules);
}

std::string_view collision_rules_shape_violation(const std::vector<CollisionRule>& rules) noexcept {
  return shape_violation(rules);
}

}  // namespace manyforge::core
