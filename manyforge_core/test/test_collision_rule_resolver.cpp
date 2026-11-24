// The move's resolved collision-rule table: the first rule's pair set, rule
// values per pair, move rows replacing cell values, overlap and unresolved
// selections, and the shape check.

#include <manyforge_core/collision/collision_rule_resolver.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

const char kLink5[] = "component/robot/model/arm/body/arm_collision/collision/0";
const char kFinger[] = "component/gripper/model/tool/body/gripper_collision/collision/0";
const char kPad[] = "component/gripper/model/tool/body/gripper_collision/collision/1";
const char kPart[] = "scene/part/shape/0";
const char kTable[] = "scene/table/shape/0";
const char kWall[] = "scene/wall/shape/0";
const char kSeen[] = "runtime_object/seen/shape/0";

CollisionWorldBody body(std::string id, const CollisionOwnerKind owner, std::string frame,
                        std::optional<CollisionBodyId> parent = std::nullopt) {
  CollisionWorldBody result;
  result.body_id = std::move(id);
  result.owner_kind = owner;
  result.frame_id = std::move(frame);
  result.parent_body_id = std::move(parent);
  return result;
}

/// Robot arm link, two gripper links, a carried part, a table, a wall and one
/// runtime object.
CollisionWorldSnapshot world(const bool part_carried = true) {
  CollisionWorldSnapshot snapshot;
  snapshot.bodies = {
      body(kLink5, CollisionOwnerKind::kComponentModelBody, "link5"),
      body(kFinger, CollisionOwnerKind::kComponentModelBody, "finger"),
      body(kPad, CollisionOwnerKind::kComponentModelBody, "pad"),
      body(kPart, CollisionOwnerKind::kSceneResource, part_carried ? "pad" : "cell",
           part_carried ? std::optional<CollisionBodyId>{kPad} : std::nullopt),
      body(kTable, CollisionOwnerKind::kSceneResource, "cell"),
      body(kWall, CollisionOwnerKind::kSceneResource, "cell"),
      body(kSeen, CollisionOwnerKind::kRuntimeObject, "cell"),
  };
  std::sort(snapshot.bodies.begin(), snapshot.bodies.end(),
            [](const auto& left, const auto& right) { return left.body_id < right.body_id; });
  return snapshot;
}

CollisionPairId pair(std::string a, std::string b) {
  return a < b ? CollisionPairId{std::move(a), std::move(b)}
               : CollisionPairId{std::move(b), std::move(a)};
}

CollisionRule everything(const double clearance_m, std::string rule_id = "everything") {
  return CollisionRule{std::move(rule_id), {}, {}, CollisionClearanceRule{clearance_m}};
}

CollisionRuleSelection component(const char* id) {
  return CollisionRuleComponentSelection{ControllableComponentId(id)};
}
CollisionRuleSelection attached(const char* id) {
  return CollisionRuleAttachedObjectSelection{ControllableComponentId(id)};
}
CollisionRuleSelection object(const char* id) {
  return CollisionRuleSceneObjectSelection{SceneResourceId(id)};
}

TEST(CollisionRuleResolver, SelectionPairsOfACarriedObjectAndASceneObject) {
  // OD-356: a Mock input's geometric contact selects its pairs like a rule.
  const auto carried = resolve_collision_selection_pairs(world(), {attached("gripper")},
                                                         {object("table")});
  ASSERT_TRUE(carried.ok());
  EXPECT_EQ(carried.value(), (std::vector<CollisionPairId>{pair(kPart, kTable)}));

  // Nothing carried: no pair, not an error.
  const auto empty = resolve_collision_selection_pairs(world(false), {attached("gripper")},
                                                       {object("table")});
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(empty.value().empty());

  const auto missing = resolve_collision_selection_pairs(world(), {attached("gripper")},
                                                         {object("endplate")});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"), "collision_rule_selection_unresolved");
}

TEST(CollisionRuleResolver, FirstRuleHoldsEveryRobotEnvironmentPair) {
  const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001)}, {});
  ASSERT_TRUE(resolved.ok());
  ASSERT_NE(resolved.value().clearance_margins, nullptr);
  const auto& margins = *resolved.value().clearance_margins;
  // Robot: three component bodies and the carried part. Environment: table,
  // wall and the runtime object.
  EXPECT_EQ(margins.pairs.size(), 12U);
  EXPECT_TRUE(std::is_sorted(margins.pairs.begin(), margins.pairs.end(),
                             [](const auto& left, const auto& right) {
                               return left.pair < right.pair;
                             }));
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kTable)), 0.001);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPart, kSeen)), 0.001);
  EXPECT_EQ(margins.rule_id_for(pair(kPart, kWall)), "everything");
  // Robot x robot and environment x environment pairs keep 0.
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kFinger)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPad, kPart)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kTable, kWall)), 0.0);
  EXPECT_TRUE(resolved.value().contact_rules.empty());
}

TEST(CollisionRuleResolver, ZeroClearanceOrNoFirstRuleGivesNoMargins) {
  auto resolved = resolve_collision_rules(world(), {}, {everything(0.0)}, {});
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value().clearance_margins, nullptr);
  resolved = resolve_collision_rules(world(), {}, {}, {});
  ASSERT_TRUE(resolved.ok());
  EXPECT_EQ(resolved.value().clearance_margins, nullptr);
}

TEST(CollisionRuleResolver, StaticStaticAndGripPairsAreOutsideTheTable) {
  CollisionPolicySnapshot base;
  base.allowed_touch_link_pairs = {pair(kPart, kPad)};
  // The arm link stands still here: with the cell-fixed table and wall it is
  // a static body.
  base.static_body_ids = {kLink5, kTable, kWall};
  const CollisionRule grip{"grip", {attached("gripper")}, {component("gripper")},
                           CollisionContactRule{0.001, 0.0, std::nullopt}};
  const CollisionRule mount{"mount", {component("robot")}, {object("table")},
                            CollisionContactRule{0.001, 0.0, std::nullopt}};
  const auto resolved =
      resolve_collision_rules(world(), base, {everything(0.001), grip, mount}, {});
  ASSERT_TRUE(resolved.ok());
  // The grip pair keeps its attachment allowance and the mount pair is two
  // static bodies; only the finger pair is ruled.
  ASSERT_EQ(resolved.value().contact_rules.size(), 1U);
  EXPECT_EQ(resolved.value().contact_rules.front().pair, pair(kPart, kFinger));
  // The first rule gives no clearance to two static bodies; a static body
  // keeps it against a moving or runtime body.
  ASSERT_NE(resolved.value().clearance_margins, nullptr);
  const auto& margins = *resolved.value().clearance_margins;
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kTable)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kWall)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kSeen)), 0.001);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kFinger, kTable)), 0.001);
}

TEST(CollisionRuleResolver, BasePolicyContactRulesStayUnlessARuleHoldsTheirPair) {
  CollisionPolicySnapshot base;
  base.contact_rules = {
      CollisionPairContactRule{pair(kLink5, kTable), "held", CollisionReleaseRule{}},
      CollisionPairContactRule{pair(kPart, kTable), "held", CollisionReleaseRule{}}};
  const CollisionRule lift{"lift", {attached("gripper")}, {object("table")},
                           CollisionContactRule{0.001, 0.0, std::nullopt}};
  const auto resolved = resolve_collision_rules(world(), base, {everything(0.001)}, {lift});
  ASSERT_TRUE(resolved.ok());
  ASSERT_EQ(resolved.value().contact_rules.size(), 2U);
  EXPECT_EQ(resolved.value().contact_rules[0].pair, pair(kLink5, kTable));
  EXPECT_EQ(resolved.value().contact_rules[0].rule_id, "held");
  EXPECT_EQ(resolved.value().contact_rules[1].pair, pair(kPart, kTable));
  EXPECT_EQ(resolved.value().contact_rules[1].rule_id, "lift");
  // A held pair gets no clearance from the first rule.
  EXPECT_DOUBLE_EQ(resolved.value().clearance_margins->margin_for(pair(kLink5, kTable)), 0.0);
  EXPECT_DOUBLE_EQ(resolved.value().clearance_margins->margin_for(pair(kLink5, kWall)), 0.001);
}

TEST(CollisionRuleResolver, ContactRuleNamesWhichBodyIsItsSideA) {
  // OD-358: a spring's compression shifts the rule's side a.
  const CollisionRule press{"press", {attached("gripper")}, {object("table")},
                            CollisionContactRule{0.008, 0.0, 0.002}};
  const auto resolved = resolve_collision_rules(world(), {}, {}, {press});
  ASSERT_TRUE(resolved.ok());
  ASSERT_EQ(resolved.value().contact_rules.size(), 1U);
  const auto& held = resolved.value().contact_rules[0];
  EXPECT_EQ(held.pair, pair(kPart, kTable));
  EXPECT_FALSE(held.side_a_is_body_b);  // scene/part < scene/table
  const CollisionRule reversed{"press", {object("table")}, {attached("gripper")},
                               CollisionContactRule{0.008, 0.0, 0.002}};
  const auto swapped = resolve_collision_rules(world(), {}, {}, {reversed});
  ASSERT_TRUE(swapped.ok());
  EXPECT_TRUE(swapped.value().contact_rules[0].side_a_is_body_b);
}

TEST(CollisionRuleResolver, MoveRowWithoutSelectionsReplacesTheFirstRuleValue) {
  const CollisionRule holder{"holder", {component("robot")}, {object("table")},
                             CollisionClearanceRule{0.01}};
  const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001), holder},
                                                {everything(0.003, "this_move")});
  ASSERT_TRUE(resolved.ok());
  const auto& margins = *resolved.value().clearance_margins;
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kFinger, kWall)), 0.003);
  EXPECT_EQ(margins.rule_id_for(pair(kFinger, kWall)), "this_move");
  // A cell rule with selections keeps its pairs.
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kTable)), 0.01);
  EXPECT_EQ(margins.rule_id_for(pair(kLink5, kTable)), "holder");
}

TEST(CollisionRuleResolver, MoveRuleReplacesTheWholeValueOfExactlyItsPairs) {
  const CollisionRule cell{"keep_off_table", {component("robot")}, {object("table")},
                           CollisionClearanceRule{0.02}};
  CollisionReleaseRule release;
  release.max_start_penetration_m = 0.002;
  const CollisionRule lift{"lift", {attached("gripper")}, {object("table")}, release};
  const CollisionRule touch{"touch", {component("robot")}, {object("table")},
                            CollisionContactRule{std::nullopt, 0.8, std::nullopt}};
  const auto resolved =
      resolve_collision_rules(world(), {}, {everything(0.001), cell}, {lift, touch});
  ASSERT_TRUE(resolved.ok());
  const auto& contact = resolved.value().contact_rules;
  ASSERT_EQ(contact.size(), 2U);
  EXPECT_TRUE(std::is_sorted(contact.begin(), contact.end(),
                             [](const auto& left, const auto& right) {
                               return left.pair < right.pair;
                             }));
  const auto lifted = std::find_if(contact.begin(), contact.end(), [](const auto& entry) {
    return entry.pair == pair(kPart, kTable);
  });
  ASSERT_NE(lifted, contact.end());
  EXPECT_EQ(lifted->rule_id, "lift");
  ASSERT_NE(lifted->release(), nullptr);
  EXPECT_DOUBLE_EQ(lifted->release()->max_start_penetration_m, 0.002);
  const auto touched = std::find_if(contact.begin(), contact.end(), [](const auto& entry) {
    return entry.pair == pair(kLink5, kTable);
  });
  ASSERT_NE(touched, contact.end());
  ASSERT_NE(touched->contact(), nullptr);
  EXPECT_DOUBLE_EQ(touched->contact()->end_from_fraction, 0.8);
  // Contact pairs have no margin; other pairs keep the first rule.
  const auto& margins = *resolved.value().clearance_margins;
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPart, kTable)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kLink5, kTable)), 0.0);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPart, kWall)), 0.001);
}

TEST(CollisionRuleResolver, RuleMayGiveRobotRobotPairsAClearance) {
  const CollisionRule apart{"arm_off_gripper", {component("robot")}, {component("gripper")},
                            CollisionClearanceRule{0.005}};
  const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001), apart}, {});
  ASSERT_TRUE(resolved.ok());
  EXPECT_DOUBLE_EQ(resolved.value().clearance_margins->margin_for(pair(kLink5, kPad)), 0.005);
}

TEST(CollisionRuleResolver, LinkAndResolvedBodiesSelectSingleBodies) {
  const CollisionRule rule{
      "pad_on_wall",
      {CollisionRuleLinkSelection{ControllableComponentId("gripper"), "pad"},
       CollisionRuleResolvedBodiesSelection{{kFinger}}},
      {object("wall")},
      CollisionContactRule{std::nullopt, 0.0, std::nullopt}};
  const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001)}, {rule});
  ASSERT_TRUE(resolved.ok());
  ASSERT_EQ(resolved.value().contact_rules.size(), 2U);
  EXPECT_NE(std::find_if(resolved.value().contact_rules.begin(),
                         resolved.value().contact_rules.end(),
                         [](const auto& entry) { return entry.pair == pair(kPad, kWall); }),
            resolved.value().contact_rules.end());
}

TEST(CollisionRuleResolver, SceneObjectsSelectsDetachedObjectsOnly) {
  const CollisionRule rule{"gripper_far", {component("gripper")},
                           {CollisionRuleSceneObjectsSelection{}}, CollisionClearanceRule{0.02}};
  const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001), rule}, {});
  ASSERT_TRUE(resolved.ok());
  const auto& margins = *resolved.value().clearance_margins;
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPad, kTable)), 0.02);
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPad, kSeen)), 0.02);
  // The carried part is not a detached object.
  EXPECT_DOUBLE_EQ(margins.margin_for(pair(kPad, kPart)), 0.0);
}

TEST(CollisionRuleResolver, NothingCarriedGivesARuleWithoutPairs) {
  const CollisionRule lift{"lift", {attached("gripper")}, {object("table")},
                           CollisionReleaseRule{}};
  const auto resolved = resolve_collision_rules(world(false), {}, {everything(0.001)}, {lift});
  ASSERT_TRUE(resolved.ok());
  EXPECT_TRUE(resolved.value().contact_rules.empty());
}

TEST(CollisionRuleResolver, TwoMoveRulesHoldingOnePairAreRefused) {
  const CollisionRule first{"first", {component("robot")}, {object("table")},
                            CollisionContactRule{std::nullopt, 0.0, std::nullopt}};
  const CollisionRule second{"second", {component("robot")},
                             {CollisionRuleSceneObjectsSelection{}},
                             CollisionClearanceRule{0.01}};
  const auto resolved =
      resolve_collision_rules(world(), {}, {everything(0.001)}, {first, second});
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().code, kErrorInvalidCollisionRule);
  EXPECT_EQ(resolved.error().details.at("reason"), "collision_rules_overlap");
  EXPECT_EQ(resolved.error().details.at("collision_rule_id"), "first");
  EXPECT_EQ(resolved.error().details.at("other_collision_rule_id"), "second");
  // The same two rules as cell rules are refused too.
  EXPECT_FALSE(resolve_collision_rules(world(), {}, {everything(0.001), first, second}, {}).ok());
}

TEST(CollisionRuleResolver, SelectionThatNamesNothingIsRefused) {
  for (const auto& selection :
       {component("absent"), object("absent"),
        CollisionRuleSelection{
            CollisionRuleLinkSelection{ControllableComponentId("robot"), "absent"}},
        CollisionRuleSelection{CollisionRuleResolvedBodiesSelection{{"component/absent"}}}}) {
    const CollisionRule rule{"rule", {selection}, {object("table")}, CollisionContactRule{std::nullopt, 0.0, std::nullopt}};
    const auto resolved = resolve_collision_rules(world(), {}, {everything(0.001)}, {rule});
    ASSERT_FALSE(resolved.ok());
    EXPECT_EQ(resolved.error().details.at("reason"), "collision_rule_selection_unresolved");
    EXPECT_EQ(resolved.error().details.at("collision_rule_id"), "rule");
  }
}

TEST(CollisionRuleResolver, AttachedObjectOfOneBodySelectsOnlyWhatThatBodyCarries) {
  // OD-371: the part hangs on the pad; the finger carries nothing.
  const auto carried_by = [](std::vector<CollisionBodyId> parents) {
    return CollisionRuleSelection{CollisionRuleResolvedAttachedObjectSelection{std::move(parents)}};
  };
  const auto pad = resolve_collision_selection_pairs(world(), {carried_by({kPad})},
                                                     {object("table")});
  ASSERT_TRUE(pad.ok());
  EXPECT_EQ(pad.value(), (std::vector<CollisionPairId>{pair(kPart, kTable)}));

  const auto finger = resolve_collision_selection_pairs(world(), {carried_by({kFinger})},
                                                        {object("table")});
  ASSERT_TRUE(finger.ok());
  EXPECT_TRUE(finger.value().empty());

  // A spring rule on the finger and what it carries holds only the finger's
  // own pairs.
  const std::vector<CollisionRule> move{
      {"press",
       {CollisionRuleResolvedBodiesSelection{{kFinger}}, carried_by({kFinger})},
       {object("table")},
       CollisionContactRule{0.008, 0.0, 0.002}}};
  const auto resolved = resolve_collision_rules(world(), {}, {}, move);
  ASSERT_TRUE(resolved.ok());
  ASSERT_EQ(resolved.value().contact_rules.size(), 1U);
  EXPECT_EQ(resolved.value().contact_rules.front().pair, pair(kFinger, kTable));
}

TEST(CollisionRuleShape, AttachedObjectRoleIsResolvedBeforeAdmission) {
  CollisionContactRule spring{0.008, 0.0, 0.002};
  spring.spring_group =
      CollisionSpringGroup{ControllableComponentId("gripper"), "pad", {0.0, 0.0, 1.0}};
  const std::vector<CollisionRuleDTO> authored{
      {"press",
       {CollisionRuleAttachedObjectSelection{ControllableComponentId("gripper"), "pad"}},
       {CollisionRuleSceneObjectSelection{SceneResourceId("table")}},
       spring}};
  EXPECT_TRUE(collision_rules_shape_violation(authored).empty());

  const auto admitted = [&](CollisionRuleSelection selection, CollisionContactRule value) {
    return std::string(collision_rules_shape_violation(std::vector<CollisionRule>{
        {"press", {std::move(selection)}, {object("table")}, value}}));
  };
  EXPECT_EQ(admitted(CollisionRuleResolvedAttachedObjectSelection{{kPad}}, spring), "");
  EXPECT_EQ(admitted(CollisionRuleAttachedObjectSelection{ControllableComponentId("gripper"), "pad"},
                     spring),
            "collision_rule_selection_invalid");
  EXPECT_EQ(admitted(CollisionRuleResolvedAttachedObjectSelection{{}}, spring),
            "collision_rule_selection_invalid");
  // A spring group needs a spring and a unit axis.
  auto no_step = spring;
  no_step.max_compression_step_m.reset();
  EXPECT_EQ(admitted(attached("gripper"), no_step), "collision_rule_value_invalid");
  auto bad_axis = spring;
  bad_axis.spring_group->axis = {0.0, 0.0, 2.0};
  EXPECT_EQ(admitted(attached("gripper"), bad_axis), "collision_rule_value_invalid");
}

TEST(CollisionRuleShape, AcceptsTheDocumentedRows) {
  const std::vector<CollisionRule> rules{
      everything(0.002),
      {"contact", {component("robot")}, {object("table")}, CollisionContactRule{0.001, 0.8, std::nullopt}},
      {"release", {attached("gripper")}, {object("table")}, CollisionReleaseRule{}}};
  EXPECT_TRUE(collision_rules_shape_violation(rules).empty());
}

TEST(CollisionRuleShape, NamesTheFirstViolation) {
  const auto violation = [](std::vector<CollisionRule> rules) {
    return std::string(collision_rules_shape_violation(rules));
  };
  EXPECT_EQ(violation({everything(0.001, "")}), "collision_rule_id_invalid");
  EXPECT_EQ(violation({everything(0.001), everything(0.002)}), "collision_rule_id_duplicate");
  EXPECT_EQ(violation({everything(0.001), everything(0.002, "second")}),
            "collision_rule_selection_invalid");
  EXPECT_EQ(violation({{"one_side", {component("robot")}, {}, CollisionContactRule{std::nullopt, 0.0, std::nullopt}}}),
            "collision_rule_selection_invalid");
  EXPECT_EQ(violation({everything(0.06)}), "collision_rule_value_invalid");
  EXPECT_EQ(violation({{"no_pairs", {}, {}, CollisionContactRule{std::nullopt, 0.0, std::nullopt}}}),
            "collision_rule_value_invalid");
  EXPECT_EQ(violation({{"window", {component("robot")}, {object("table")},
                        CollisionContactRule{std::nullopt, 1.5, std::nullopt}}}),
            "collision_rule_value_invalid");
  CollisionReleaseRule binary;
  binary.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  // Binary evidence keeps the signed-only fields neutral.
  EXPECT_EQ(violation({{"binary", {component("robot")}, {object("table")}, binary}}),
            "collision_rule_value_invalid");
  binary.enforce_max_start_penetration = false;
  binary.release_tolerance_m = 0.0;
  binary.require_strictly_decreasing_penetration = false;
  EXPECT_EQ(violation({{"binary", {component("robot")}, {object("table")}, binary}}), "");
  EXPECT_EQ(violation(std::vector<CollisionRule>(kMaxCollisionRules + 1U, everything(0.001))),
            "collision_rules_bound_exceeded");
}

}  // namespace
}  // namespace manyforge::core
