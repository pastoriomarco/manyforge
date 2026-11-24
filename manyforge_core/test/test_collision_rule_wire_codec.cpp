// The JSON shape of collision rules: round trip of every selection and value,
// complete releases, and closed-shape refusals.

#include <manyforge_core/collision_rule_codec.hpp>

#include <gtest/gtest.h>

#include <manyforge_content_identity/canonical_value.hpp>

#include <string>
#include <vector>

namespace manyforge::core {
namespace {

using content_identity::canonical_json;
using content_identity::parse_canonical_json;

std::vector<CollisionRuleDTO> rules() {
  CollisionReleaseRule release;
  release.max_start_penetration_m = 0.002;
  release.allow_recontact = true;
  CollisionContactRule spring{0.02, 0.0, 0.001};
  spring.spring_group =
      CollisionSpringGroup{ControllableComponentId("tube_holder"), "holder_rod", {0.0, 0.0, 1.0}};
  return {
      {"everything", {}, {}, CollisionClearanceRule{0.002}},
      {"tip_on_part",
       {CollisionRuleActiveTcpContactBodiesSelection{}},
       {CollisionRuleSceneObjectSelection{SceneResourceId("part")}},
       CollisionContactRule{0.002, 0.8, std::nullopt}},
      {"lift",
       {CollisionRuleAttachedObjectSelection{ControllableComponentId("gripper")}},
       {CollisionRuleSceneObjectsSelection{}},
       release},
      {"arm_far",
       {CollisionRuleComponentSelection{ControllableComponentId("robot")},
        CollisionRuleLinkSelection{ControllableComponentId("robot"), "link5"},
        CollisionRuleModelGroupSelection{ControllableComponentId("robot"), "manipulator"}},
       {CollisionRuleCollisionBodySelection{ProgramCollisionBodyReferenceDTO{
           ControllableComponentId("gripper"), CollisionBodyRoleId("gripper_collision")}}},
       CollisionClearanceRule{0.01}},
      {"tube_into_endplate",
       {CollisionRuleCollisionBodySelection{ProgramCollisionBodyReferenceDTO{
            ControllableComponentId("tube_holder"), CollisionBodyRoleId("holder_rod")}},
        CollisionRuleAttachedObjectSelection{ControllableComponentId("tube_holder"),
                                             "holder_rod"}},
       {CollisionRuleSceneObjectSelection{SceneResourceId("endplate")}},
       spring},
  };
}

TEST(CollisionRuleCodec, RoundTripsEveryRow) {
  const auto encoded = CollisionRulesValue(rules());
  const auto decoded = DecodeCollisionRules(encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), rules());
  // The row without selections has no `a` and no `b`.
  EXPECT_EQ(encoded.as_array().front().find_object_member("a"), nullptr);
  EXPECT_EQ(encoded.as_array().front().find_object_member("b"), nullptr);
}

TEST(CollisionRuleCodec, RefusesAReleaseThatLeavesAKeyOut) {
  const auto decoded = DecodeCollisionRules(parse_canonical_json(
      R"([{"a":[{"component_id":"gripper","kind":"attached_object"}],)"
      R"("b":[{"kind":"scene_objects"}],"release":{},"rule_id":"lift"}])"));
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error().details.at("reason"), "collision_rule_value_invalid");
}

TEST(CollisionRuleCodec, RefusesAnythingOutsideTheClosedShape) {
  const auto reason = [](const std::string& json) {
    const auto decoded = DecodeCollisionRules(parse_canonical_json(json));
    return decoded.ok() ? std::string{} : decoded.error().details.at("reason");
  };
  EXPECT_EQ(reason(R"({})"), "collision_rules_bound_exceeded");
  // Two values on one row.
  EXPECT_EQ(reason(R"([{"clearance_m":0.001,"contact":{},"rule_id":"r"}])"),
            "collision_rule_value_invalid");
  // An unknown row key (`leg` is never sent).
  EXPECT_EQ(reason(R"([{"clearance_m":0.001,"leg":"descend","rule_id":"r"}])"),
            "collision_rule_selection_invalid");
  // An unknown selection kind and a selection with a missing key.
  EXPECT_EQ(reason(R"([{"a":[{"kind":"model_group"}],"b":[{"kind":"scene_objects"}],)"
                   R"("contact":{},"rule_id":"r"}])"),
            "collision_rule_selection_invalid");
  EXPECT_EQ(reason(R"([{"a":[{"kind":"component"}],"b":[{"kind":"scene_objects"}],)"
                   R"("contact":{},"rule_id":"r"}])"),
            "collision_rule_selection_invalid");
  // A blackboard key is resolved before the row is sent.
  EXPECT_EQ(reason(R"([{"a":[{"blackboard_key":"k","kind":"scene_object"}],)"
                   R"("b":[{"kind":"scene_objects"}],"contact":{},"rule_id":"r"}])"),
            "collision_rule_selection_invalid");
  // An unknown contact key.
  EXPECT_EQ(reason(R"([{"a":[{"kind":"scene_objects"}],"b":[{"kind":"scene_objects"}],)"
                   R"("contact":{"during":"end"},"rule_id":"r"}])"),
            "collision_rule_value_invalid");
  // The shape check runs on the decoded rows.
  EXPECT_EQ(reason(R"([{"clearance_m":0.5,"rule_id":"r"}])"), "collision_rule_value_invalid");
}

TEST(CollisionRuleCodec, ReadsTheSpringSliderRowAsSent) {
  // OD-371: the expanded `spring_slider` row of a motion payload.
  const auto decoded = DecodeCollisionRules(parse_canonical_json(
      R"([{"a":[{"collision_body_id":"holder_rod","component_id":"tube_holder",)"
      R"("kind":"collision_body"},{"collision_body_role_id":"holder_rod",)"
      R"("component_id":"tube_holder","kind":"attached_object"}],)"
      R"("b":[{"kind":"scene_object","scene_resource_id":"endplate"}],)"
      R"("contact":{"end_from_fraction":0,"max_compression_step_m":0.001,)"
      R"("max_penetration_m":0.02,"spring_group":{"axis":[0,0,1],)"
      R"("component_id":"tube_holder","element_id":"holder_rod"}},)"
      R"("rule_id":"tube_into_endplate"}])"));
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), std::vector<CollisionRuleDTO>{rules().back()});
  // A spring group without a spring is refused.
  const auto no_spring = DecodeCollisionRules(parse_canonical_json(
      R"([{"a":[{"kind":"scene_objects"}],"b":[{"kind":"scene_objects"}],)"
      R"("contact":{"spring_group":{"axis":[0,0,1],"component_id":"t","element_id":"e"}},)"
      R"("rule_id":"r"}])"));
  ASSERT_FALSE(no_spring.ok());
  EXPECT_EQ(no_spring.error().details.at("reason"), "collision_rule_value_invalid");
}

TEST(CollisionRuleCodec, AdmittedRulesWriteResolvedBodies) {
  const std::vector<CollisionRule> admitted{
      {"tip", {CollisionRuleResolvedBodiesSelection{{"component/a", "component/b"}}},
       {CollisionRuleSceneObjectSelection{SceneResourceId("part")}}, CollisionContactRule{}}};
  EXPECT_EQ(canonical_json(CollisionRulesValue(admitted)),
            R"([{"a":[{"collision_body_ids":["component/a","component/b"],)"
            R"("kind":"collision_bodies"}],"b":[{"kind":"scene_object",)"
            R"("scene_resource_id":"part"}],"contact":{"end_from_fraction":0.8},"rule_id":"tip"}])");
}

}  // namespace
}  // namespace manyforge::core
