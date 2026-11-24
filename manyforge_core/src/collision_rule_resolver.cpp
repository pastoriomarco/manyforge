#include <manyforge_core/collision/collision_rule_resolver.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using BodyList = std::vector<const CollisionWorldBody*>;

[[nodiscard]] CollisionPairId canonical_pair(const CollisionBodyId& first,
                                             const CollisionBodyId& second) {
  return first < second ? CollisionPairId{first, second} : CollisionPairId{second, first};
}

[[nodiscard]] bool is_robot(const CollisionWorldBody& body) noexcept {
  return body.owner_kind == CollisionOwnerKind::kComponentModelBody ||
         (body.owner_kind == CollisionOwnerKind::kSceneResource && body.parent_body_id.has_value());
}

[[nodiscard]] bool is_environment(const CollisionWorldBody& body) noexcept {
  return body.owner_kind == CollisionOwnerKind::kRuntimeObject ||
         (body.owner_kind == CollisionOwnerKind::kSceneResource &&
          !body.parent_body_id.has_value());
}

[[nodiscard]] bool has_prefix(const std::string& text, const std::string& prefix) noexcept {
  return text.size() > prefix.size() && text.compare(0U, prefix.size(), prefix) == 0;
}

[[nodiscard]] std::string component_prefix(const ControllableComponentId& component_id) {
  return "component/" + component_id.value() + "/";
}

[[nodiscard]] const CollisionWorldBody* find_body(const CollisionWorldSnapshot& world,
                                                  const CollisionBodyId& body_id) noexcept {
  const auto found =
      std::lower_bound(world.bodies.begin(), world.bodies.end(), body_id,
                       [](const CollisionWorldBody& candidate, const CollisionBodyId& selected) {
                         return candidate.body_id < selected;
                       });
  return found != world.bodies.end() && found->body_id == body_id ? &*found : nullptr;
}

struct SelectedBodies {
  BodyList bodies;
  /// False when a selection that must name something names nothing.
  bool resolved = true;
  std::string_view unresolved_kind;
  std::string unresolved_reference;
};

void select(const CollisionWorldSnapshot& world, const CollisionRuleSelection& selection,
            SelectedBodies& selected) {
  const auto before = selected.bodies.size();
  const auto unresolved = [&](const std::string_view kind, std::string reference) {
    if (selected.resolved) {
      selected.resolved = false;
      selected.unresolved_kind = kind;
      selected.unresolved_reference = std::move(reference);
    }
  };
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, CollisionRuleComponentSelection>) {
          const auto prefix = component_prefix(value.component_id);
          for (const auto& body : world.bodies) {
            if (body.owner_kind == CollisionOwnerKind::kComponentModelBody &&
                has_prefix(body.body_id, prefix)) {
              selected.bodies.push_back(&body);
            }
          }
          if (selected.bodies.size() == before) {
            unresolved("component", value.component_id.value());
          }
        } else if constexpr (std::is_same_v<Value, CollisionRuleResolvedBodiesSelection>) {
          for (const auto& body_id : value.collision_body_ids) {
            const auto* body = find_body(world, body_id);
            if (body == nullptr || body->owner_kind != CollisionOwnerKind::kComponentModelBody) {
              unresolved("collision_body", body_id);
              continue;
            }
            selected.bodies.push_back(body);
          }
        } else if constexpr (std::is_same_v<Value, CollisionRuleLinkSelection>) {
          const auto prefix = component_prefix(value.component_id);
          for (const auto& body : world.bodies) {
            if (body.owner_kind == CollisionOwnerKind::kComponentModelBody &&
                has_prefix(body.body_id, prefix) && body.frame_id == value.link) {
              selected.bodies.push_back(&body);
            }
          }
          if (selected.bodies.size() == before) {
            unresolved("link", value.component_id.value() + "/" + value.link);
          }
        } else if constexpr (std::is_same_v<Value, CollisionRuleAttachedObjectSelection>) {
          // Nothing carried is a rule without pairs, not an error.
          const auto prefix = component_prefix(value.component_id);
          for (const auto& body : world.bodies) {
            if (body.owner_kind == CollisionOwnerKind::kSceneResource &&
                body.parent_body_id.has_value() && has_prefix(*body.parent_body_id, prefix)) {
              selected.bodies.push_back(&body);
            }
          }
        } else if constexpr (std::is_same_v<Value,
                                            CollisionRuleResolvedAttachedObjectSelection>) {
          // The objects carried by these bodies; nothing carried is no pair.
          for (const auto& body : world.bodies) {
            if (body.owner_kind == CollisionOwnerKind::kSceneResource &&
                body.parent_body_id.has_value() &&
                std::binary_search(value.parent_body_ids.begin(), value.parent_body_ids.end(),
                                   *body.parent_body_id)) {
              selected.bodies.push_back(&body);
            }
          }
        } else if constexpr (std::is_same_v<Value, CollisionRuleSceneObjectsSelection>) {
          for (const auto& body : world.bodies) {
            if (is_environment(body)) {
              selected.bodies.push_back(&body);
            }
          }
        } else {
          const auto prefix = "scene/" + value.scene_resource_id.value() + "/shape/";
          for (const auto& body : world.bodies) {
            if (body.owner_kind == CollisionOwnerKind::kSceneResource &&
                has_prefix(body.body_id, prefix)) {
              selected.bodies.push_back(&body);
            }
          }
          if (selected.bodies.size() == before) {
            unresolved("scene_object", value.scene_resource_id.value());
          }
        }
      },
      selection);
}

[[nodiscard]] SelectedBodies select_side(const CollisionWorldSnapshot& world,
                                         const std::vector<CollisionRuleSelection>& side) {
  SelectedBodies selected;
  for (const auto& selection : side) {
    select(world, selection, selected);
  }
  std::sort(selected.bodies.begin(), selected.bodies.end(),
            [](const CollisionWorldBody* left, const CollisionWorldBody* right) {
              return left->body_id < right->body_id;
            });
  selected.bodies.erase(std::unique(selected.bodies.begin(), selected.bodies.end()),
                        selected.bodies.end());
  return selected;
}

/// A pair's rule, and whether the rule's side a selected the pair's body_b.
struct PairRule {
  const CollisionRule* rule = nullptr;
  bool side_a_is_body_b = false;
};
using PairRules = std::map<CollisionPairId, PairRule>;

[[nodiscard]] ErrorDTO unresolved_error(const CollisionRule& rule, const SelectedBodies& side) {
  return ErrorDTO{kErrorInvalidCollisionRule,
                  "Collision rule '" + rule.rule_id + "' selects a " +
                      std::string(side.unresolved_kind) + " that is not in the collision world: " +
                      side.unresolved_reference,
                  {{"reason", "collision_rule_selection_unresolved"},
                   {"collision_rule_id", rule.rule_id},
                   {"selection_kind", std::string(side.unresolved_kind)},
                   {"reference_id", side.unresolved_reference}}};
}

/// The pairs outside the rule table: two static bodies, and the grip pairs of
/// an attachment. No rule gives them a value.
struct NeverRuled {
  const CollisionPolicySnapshot& policy;
  /// Canonical, sorted.
  std::vector<CollisionPairId> touch_link_pairs;

  [[nodiscard]] bool operator()(const CollisionPairId& pair) const noexcept {
    return policy.static_pair(pair) ||
           std::binary_search(touch_link_pairs.begin(), touch_link_pairs.end(), pair);
  }
};

/// Pairs of every rule with selections in `rules`. Two rules holding one pair
/// are refused.
[[nodiscard]] Result<PairRules> pairs_of(const CollisionWorldSnapshot& world,
                                         const NeverRuled& never_ruled,
                                         const std::vector<CollisionRule>& rules) {
  PairRules pairs;
  for (const auto& rule : rules) {
    if (!rule.has_selections()) {
      continue;
    }
    const auto side_a = select_side(world, rule.a);
    if (!side_a.resolved) {
      return Result<PairRules>::Err(unresolved_error(rule, side_a));
    }
    const auto side_b = select_side(world, rule.b);
    if (!side_b.resolved) {
      return Result<PairRules>::Err(unresolved_error(rule, side_b));
    }
    for (const auto* first : side_a.bodies) {
      for (const auto* second : side_b.bodies) {
        if (first == second) {
          continue;
        }
        auto pair = canonical_pair(first->body_id, second->body_id);
        if (never_ruled(pair)) {
          continue;
        }
        const bool side_a_is_body_b = pair.body_b == first->body_id;
        const auto [held, inserted] =
            pairs.emplace(std::move(pair), PairRule{&rule, side_a_is_body_b});
        if (!inserted && held->second.rule != &rule) {
          return Result<PairRules>::Err(ErrorDTO{
              kErrorInvalidCollisionRule,
              "Collision rules '" + held->second.rule->rule_id + "' and '" + rule.rule_id +
                  "' hold the same pair: " + held->first.body_a + " and " + held->first.body_b,
              {{"reason", "collision_rules_overlap"},
               {"collision_rule_id", held->second.rule->rule_id},
               {"other_collision_rule_id", rule.rule_id},
               {"body_a", held->first.body_a},
               {"body_b", held->first.body_b}}});
        }
      }
    }
  }
  return Result<PairRules>::Ok(std::move(pairs));
}

[[nodiscard]] const CollisionRule* rule_without_selections(
    const std::vector<CollisionRule>& rules) noexcept {
  const auto found = std::find_if(rules.begin(), rules.end(), [](const CollisionRule& rule) {
    return !rule.has_selections();
  });
  return found != rules.end() ? &*found : nullptr;
}

class MarginBuilder final {
 public:
  void add(CollisionPairId pair, const CollisionRule& rule, const double clearance_m) {
    if (!(clearance_m > 0.0)) {
      return;
    }
    if (last_rule_ != &rule) {
      const auto known = std::find(margins_.rule_ids.begin(), margins_.rule_ids.end(), rule.rule_id);
      last_index_ = static_cast<std::uint32_t>(known - margins_.rule_ids.begin());
      if (known == margins_.rule_ids.end()) {
        margins_.rule_ids.push_back(rule.rule_id);
      }
      last_rule_ = &rule;
    }
    margins_.pairs.push_back(CollisionPairMargin{std::move(pair), clearance_m, last_index_});
  }

  [[nodiscard]] std::shared_ptr<const CollisionClearanceMargins> build() {
    if (margins_.pairs.empty()) {
      return nullptr;
    }
    std::sort(margins_.pairs.begin(), margins_.pairs.end(),
              [](const CollisionPairMargin& left, const CollisionPairMargin& right) {
                return left.pair < right.pair;
              });
    return std::make_shared<const CollisionClearanceMargins>(std::move(margins_));
  }

 private:
  CollisionClearanceMargins margins_;
  const CollisionRule* last_rule_ = nullptr;
  std::uint32_t last_index_ = 0U;
};

}  // namespace

Result<ResolvedCollisionRules> resolve_collision_rules(
    const CollisionWorldSnapshot& world, const CollisionPolicySnapshot& base_policy,
    const std::vector<CollisionRule>& cell_rules, const std::vector<CollisionRule>& move_rules) {
  using Resolved = Result<ResolvedCollisionRules>;

  NeverRuled never_ruled{base_policy, {}};
  never_ruled.touch_link_pairs.reserve(base_policy.allowed_touch_link_pairs.size());
  for (const auto& pair : base_policy.allowed_touch_link_pairs) {
    never_ruled.touch_link_pairs.push_back(canonical_pair(pair.body_a, pair.body_b));
  }
  std::sort(never_ruled.touch_link_pairs.begin(), never_ruled.touch_link_pairs.end());

  auto cell_pairs = pairs_of(world, never_ruled, cell_rules);
  if (!cell_pairs.ok()) {
    return Resolved::Err(cell_pairs.error());
  }
  auto move_pairs = pairs_of(world, never_ruled, move_rules);
  if (!move_pairs.ok()) {
    return Resolved::Err(move_pairs.error());
  }
  // A move rule replaces the whole value of exactly its pairs.
  PairRules ruled = std::move(move_pairs.value());
  for (auto& [pair, rule] : cell_pairs.value()) {
    ruled.emplace(pair, rule);
  }

  ResolvedCollisionRules resolved;
  // A contact rule the base policy already holds stays, unless a rule of the
  // cell or the move holds its pair.
  for (const auto& held : base_policy.contact_rules) {
    if (ruled.find(held.pair) == ruled.end()) {
      resolved.contact_rules.push_back(held);
    }
  }
  MarginBuilder margins;
  for (const auto& [pair, held] : ruled) {
    const auto* rule = held.rule;
    if (const auto* clearance = std::get_if<CollisionClearanceRule>(&rule->value)) {
      margins.add(pair, *rule, clearance->clearance_m);
    } else if (const auto* contact = std::get_if<CollisionContactRule>(&rule->value)) {
      resolved.contact_rules.push_back(
          CollisionPairContactRule{pair, rule->rule_id, *contact, held.side_a_is_body_b});
    } else {
      resolved.contact_rules.push_back(
          CollisionPairContactRule{pair, rule->rule_id, std::get<CollisionReleaseRule>(rule->value),
                                   held.side_a_is_body_b});
    }
  }

  // The first rule: every robot x environment pair no other rule holds. The
  // move's row without selections replaces the cell's value.
  const auto* first_rule = rule_without_selections(move_rules);
  if (first_rule == nullptr) {
    first_rule = rule_without_selections(cell_rules);
  }
  const auto* first_clearance =
      first_rule != nullptr ? std::get_if<CollisionClearanceRule>(&first_rule->value) : nullptr;
  if (first_clearance != nullptr) {
    const auto clearance_m = first_clearance->clearance_m;
    if (clearance_m > 0.0) {
      for (const auto& robot : world.bodies) {
        if (!is_robot(robot)) {
          continue;
        }
        for (const auto& environment : world.bodies) {
          if (!is_environment(environment)) {
            continue;
          }
          auto pair = canonical_pair(robot.body_id, environment.body_id);
          if (never_ruled(pair) ||
              ruled.find(pair) != ruled.end() ||
              base_policy.contact_rule_for(pair) != nullptr) {
            continue;
          }
          margins.add(std::move(pair), *first_rule, clearance_m);
        }
      }
    }
  }
  std::sort(resolved.contact_rules.begin(), resolved.contact_rules.end(),
            [](const CollisionPairContactRule& left, const CollisionPairContactRule& right) {
              return left.pair < right.pair;
            });
  resolved.clearance_margins = margins.build();
  return Resolved::Ok(std::move(resolved));
}

Result<std::vector<CollisionPairId>> resolve_collision_selection_pairs(
    const CollisionWorldSnapshot& world, const std::vector<CollisionRuleSelection>& a,
    const std::vector<CollisionRuleSelection>& b) {
  const auto unresolved = [](const SelectedBodies& side) {
    return Result<std::vector<CollisionPairId>>::Err(ErrorDTO{
        kErrorInvalidCollisionRule,
        "The selection names a " + std::string(side.unresolved_kind) +
            " that is not in the collision world: " + side.unresolved_reference,
        {{"reason", "collision_rule_selection_unresolved"},
         {"selection_kind", std::string(side.unresolved_kind)},
         {"reference_id", side.unresolved_reference}}});
  };
  const auto side_a = select_side(world, a);
  if (!side_a.resolved) {
    return unresolved(side_a);
  }
  const auto side_b = select_side(world, b);
  if (!side_b.resolved) {
    return unresolved(side_b);
  }
  std::vector<CollisionPairId> pairs;
  pairs.reserve(side_a.bodies.size() * side_b.bodies.size());
  for (const auto* first : side_a.bodies) {
    for (const auto* second : side_b.bodies) {
      if (first != second) {
        pairs.push_back(canonical_pair(first->body_id, second->body_id));
      }
    }
  }
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  return Result<std::vector<CollisionPairId>>::Ok(std::move(pairs));
}

}  // namespace manyforge::core
