#pragma once

// Builders for the contact half of a move's resolved collision-rule table
// (CollisionPolicySnapshot::contact_rules), shared by the collision tests.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <manyforge_core/types/collision.hpp>

namespace manyforge::core::testing {

/// A pair under a `release` rule (default: the strict release).
inline CollisionPairContactRule release_rule(CollisionPairId pair = {},
                                             CollisionReleaseRule release = {},
                                             std::string rule_id = "release_rule") {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return CollisionPairContactRule{std::move(pair), std::move(rule_id), release};
}

/// A pair under a `contact` rule. No limit: the depth is not checked. No
/// fraction: the whole move (`end_from_fraction` 0).
inline CollisionPairContactRule contact_rule(CollisionPairId pair = {},
                                             std::optional<double> max_penetration_m = std::nullopt,
                                             std::optional<double> end_from_fraction = std::nullopt,
                                             std::string rule_id = "contact_rule") {
  if (pair.body_b < pair.body_a) {
    std::swap(pair.body_a, pair.body_b);
  }
  return CollisionPairContactRule{std::move(pair), std::move(rule_id),
                                  CollisionContactRule{max_penetration_m, end_from_fraction.value_or(0.0), std::nullopt}};
}

/// Add one pair rule keeping `contact_rules` sorted by pair.
inline void add_contact_rule(CollisionPolicySnapshot& policy, CollisionPairContactRule rule) {
  const auto position = std::lower_bound(
      policy.contact_rules.begin(), policy.contact_rules.end(), rule.pair,
      [](const CollisionPairContactRule& entry, const CollisionPairId& pair) {
        return entry.pair < pair;
      });
  policy.contact_rules.insert(position, std::move(rule));
}

inline CollisionReleaseRule& release_of(CollisionPairContactRule& rule) {
  return std::get<CollisionReleaseRule>(rule.value);
}

inline CollisionContactRule& contact_of(CollisionPairContactRule& rule) {
  return std::get<CollisionContactRule>(rule.value);
}

}  // namespace manyforge::core::testing
