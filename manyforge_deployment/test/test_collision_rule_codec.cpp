#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/collision_rule_codec.hpp>

namespace {

namespace core = manyforge::core;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::CollisionRuleCodecError;
using manyforge::deployment::CollisionRuleRows;
using manyforge::deployment::decode_cell_collision_rules;
using manyforge::deployment::decode_collision_rules;
using manyforge::deployment::encode_collision_rules;
using manyforge::deployment::parse_strict_json_model_yaml;

[[nodiscard]] CanonicalValue json(std::string_view text) {
  return parse_strict_json_model_yaml(text).root();
}

void expect_refused(std::string_view text, CollisionRuleRows rows,
                    std::string_view reason) {
  try {
    static_cast<void>(decode_collision_rules(json(text), rows, "collision_rules"));
    FAIL() << "accepted: " << text;
  } catch (const CollisionRuleCodecError &error) {
    EXPECT_EQ(error.reason(), reason) << text << ": " << error.what();
    EXPECT_NE(std::string_view(error.what()).find("collision_rules"),
              std::string_view::npos);
  }
}

TEST(CollisionRuleCodec, DecodesEveryCellSelectionAndValue) {
  const auto rules = decode_cell_collision_rules(json(R"([
      {"rule_id":"everything","clearance_m":0.001},
      {"rule_id":"arms_apart",
       "a":[{"kind":"component","component_id":"left_arm"},
            {"kind":"attached_object","component_id":"left_gripper"}],
       "b":[{"kind":"collision_body","component_id":"right_arm","collision_body_id":"arm_collision"},
            {"kind":"link","component_id":"right_arm","link":"link5"}],
       "clearance_m":0.02},
      {"rule_id":"pads_on_parts",
       "a":[{"kind":"collision_body","component_id":"left_gripper","collision_body_id":"gripper_pads"}],
       "b":[{"kind":"scene_objects"}],
       "contact":{"max_penetration_m":0.001}},
      {"rule_id":"fixture_release",
       "a":[{"kind":"attached_object","component_id":"left_gripper"}],
       "b":[{"kind":"scene_objects"}],
       "release":{"release_evidence_mode":"binary_contact"}}])"),
                                                 "collision_rules");
  ASSERT_EQ(rules.size(), 4U);
  EXPECT_EQ(rules[0].rule_id, core::kEverythingCollisionRuleId);
  EXPECT_TRUE(rules[0].a.empty());
  EXPECT_TRUE(rules[0].b.empty());
  EXPECT_DOUBLE_EQ(std::get<core::CollisionClearanceRule>(rules[0].value).clearance_m, 0.001);

  ASSERT_EQ(rules[1].a.size(), 2U);
  EXPECT_EQ(std::get<core::CollisionRuleComponentSelection>(rules[1].a[0]).component_id.value(),
            "left_arm");
  EXPECT_EQ(std::get<core::CollisionRuleAttachedObjectSelection>(rules[1].a[1])
                .component_id.value(),
            "left_gripper");
  const auto &body = std::get<core::CollisionRuleCollisionBodySelection>(rules[1].b[0]).body;
  EXPECT_EQ(body.component_id.value(), "right_arm");
  EXPECT_EQ(body.collision_body_id.value(), "arm_collision");
  EXPECT_EQ(std::get<core::CollisionRuleLinkSelection>(rules[1].b[1]).link, "link5");

  const auto &contact = std::get<core::CollisionContactRule>(rules[2].value);
  ASSERT_TRUE(contact.max_penetration_m.has_value());
  EXPECT_DOUBLE_EQ(*contact.max_penetration_m, 0.001);
  EXPECT_DOUBLE_EQ(contact.end_from_fraction, core::kDefaultContactEndFromFraction);
  EXPECT_TRUE(std::holds_alternative<core::CollisionRuleSceneObjectsSelection>(rules[2].b[0]));

  // Binary evidence carries no signed limit: its absent keys default to none.
  const auto &release = std::get<core::CollisionReleaseRule>(rules[3].value);
  EXPECT_EQ(release.release_evidence_mode, core::ContactReleaseEvidenceMode::kBinaryContact);
  EXPECT_FALSE(release.enforce_max_start_penetration);
  EXPECT_DOUBLE_EQ(release.release_tolerance_m, 0.0);
  EXPECT_FALSE(release.require_strictly_decreasing_penetration);
  EXPECT_TRUE(release.require_separation_by_end);
  EXPECT_FALSE(release.allow_recontact);
}

TEST(CollisionRuleCodec, SignedReleaseKeepsItsDefaults) {
  const auto rules = decode_collision_rules(
      json(R"([{"rule_id":"lift","a":[{"kind":"active_tcp_contact_bodies"}],
                "b":[{"kind":"scene_object","scene_resource_id":"table"}],"release":{}}])"),
      CollisionRuleRows::kMove, "collision_rules");
  ASSERT_EQ(rules.rows.size(), 1U);
  EXPECT_EQ(std::get<core::CollisionReleaseRule>(rules.rows.front().rule.value),
            core::CollisionReleaseRule{});
  EXPECT_FALSE(rules.rows.front().leg.has_value());
  EXPECT_TRUE(std::holds_alternative<core::CollisionRuleActiveTcpContactBodiesSelection>(
      rules.rows.front().rule.a.front()));
  EXPECT_EQ(std::get<core::CollisionRuleSceneObjectSelection>(rules.rows.front().rule.b.front())
                .scene_resource_id.value(),
            "table");
}

TEST(CollisionRuleCodec, MoveRowsReadSceneObjectsFromTheBlackboard) {
  const auto rules = decode_collision_rules(
      json(R"([
          {"rule_id":"everything_else","clearance_m":0.0},
          {"rule_id":"tip","a":[{"kind":"active_tcp_contact_bodies"}],
           "b":[{"kind":"scene_object","blackboard_key":"trial.probe_object"},
                {"kind":"scene_object","blackboard_key":"plan","field":"selected_object_id"}],
           "contact":{"max_penetration_m":0.002,"end_from_fraction":0.8}}])"),
      CollisionRuleRows::kMove, "collision_rules");
  ASSERT_EQ(rules.blackboard_scene_objects.size(), 2U);
  EXPECT_EQ(rules.blackboard_scene_objects[0].blackboard_key, "trial.probe_object");
  EXPECT_FALSE(rules.blackboard_scene_objects[0].field.has_value());
  EXPECT_EQ(rules.blackboard_scene_objects[1].blackboard_key, "plan");
  EXPECT_EQ(rules.blackboard_scene_objects[1].field, std::optional<std::string>("selected_object_id"));
  const auto &contact = std::get<core::CollisionContactRule>(rules.rows[1].rule.value);
  EXPECT_DOUBLE_EQ(contact.end_from_fraction, 0.8);
}

TEST(CollisionRuleCodec, SequenceRowsNameTheirLeg) {
  const auto rules = decode_collision_rules(
      json(R"([{"rule_id":"lift","leg":"retreat",
                "a":[{"kind":"attached_object","component_id":"gripper"}],
                "b":[{"kind":"scene_object","scene_resource_id":"table"}],"release":{}}])"),
      CollisionRuleRows::kSequence, "collision_rules");
  EXPECT_EQ(rules.rows.front().leg, std::optional<std::string>("retreat"));
  expect_refused(R"([{"rule_id":"lift","clearance_m":0.001}])", CollisionRuleRows::kSequence,
                 "collision_rule_value_invalid");
  expect_refused(R"([{"rule_id":"lift","leg":"place","clearance_m":0.001}])",
                 CollisionRuleRows::kSequence, "collision_rule_value_invalid");
  expect_refused(R"([{"rule_id":"lift","leg":"descend","clearance_m":0.001}])",
                 CollisionRuleRows::kMove, "collision_rule_value_invalid");
}

TEST(CollisionRuleCodec, RefusesRowsOutsideTheAuthoredShape) {
  const auto move = CollisionRuleRows::kMove;
  const auto cell = CollisionRuleRows::kCell;
  constexpr std::string_view sides =
      R"("a":[{"kind":"scene_objects"}],"b":[{"kind":"scene_objects"}])";
  const auto row = [&](std::string_view rest) {
    return std::string(R"([{"rule_id":"r",)") + std::string(sides) + "," + std::string(rest) + "}]";
  };
  // Ids.
  expect_refused(R"([{"clearance_m":0.001}])", move, "collision_rule_id_invalid");
  expect_refused(R"([{"rule_id":"","clearance_m":0.001}])", move, "collision_rule_id_invalid");
  expect_refused(R"([{"rule_id":"r","clearance_m":0.001},{"rule_id":"r",)" + std::string(sides) +
                     R"(,"contact":{}}])",
                 move, "collision_rule_id_duplicate");
  // Values.
  expect_refused(row(R"("clearance_m":0.001,"contact":{})"), move, "collision_rule_value_invalid");
  expect_refused(std::string(R"([{"rule_id":"r",)") + std::string(sides) + "}]", move,
                 "collision_rule_value_invalid");
  expect_refused(row(R"("clearance_m":0.0501)"), move, "collision_rule_value_invalid");
  expect_refused(row(R"("clearance_m":-0.001)"), move, "collision_rule_value_invalid");
  expect_refused(row(R"("contact":{"end_from_fraction":1.01})"), move, "collision_rule_value_invalid");
  expect_refused(row(R"("contact":{"max_penetration_m":-1.0})"), move, "collision_rule_value_invalid");
  expect_refused(row(R"("contact":{"during":"end"})"), move, "collision_rule_value_invalid");
  expect_refused(row(R"("release":{"release_evidence_mode":"force"})"), move,
                 "collision_rule_value_invalid");
  expect_refused(row(R"("release":{"release_evidence_mode":"binary_contact",
                                   "enforce_max_start_penetration":true})"),
                 move, "collision_rule_value_invalid");
  expect_refused(row(R"("release":{"allow_recontact":"no"})"), move, "collision_rule_value_invalid");
  expect_refused(R"([{"rule_id":"r","contact":{}}])", move, "collision_rule_value_invalid");
  // Selections.
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"scene_objects"}],"clearance_m":0.001}])", move,
                 "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[],"b":[{"kind":"scene_objects"}],"contact":{}}])", move,
                 "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"model_group","component_id":"arm"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 move, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"component"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 move, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"scene_objects","component_id":"arm"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 move, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"scene_object"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 move, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","a":[{"kind":"scene_object","scene_resource_id":"t","field":"f"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 move, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"r","clearance_m":0.001},{"rule_id":"s","clearance_m":0.002}])",
                 move, "collision_rule_selection_invalid");
  // A Deployment: row 0 has no selections, and only the cell's selections.
  expect_refused(R"([])", cell, "collision_rule_selection_invalid");
  expect_refused(row(R"("contact":{})"), cell, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"everything","clearance_m":0.001},
                     {"rule_id":"r","a":[{"kind":"active_tcp_contact_bodies"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 cell, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"everything","clearance_m":0.001},
                     {"rule_id":"r","a":[{"kind":"scene_object","scene_resource_id":"table"}],
                      "b":[{"kind":"scene_objects"}],"contact":{}}])",
                 cell, "collision_rule_selection_invalid");
  // The bound.
  std::string many = "[";
  for (std::size_t index = 0U; index <= core::kMaxCollisionRules; ++index) {
    many += std::string(index == 0U ? "" : ",") + R"({"rule_id":"r)" + std::to_string(index) +
            R"(",)" + std::string(sides) + R"(,"contact":{}})";
  }
  expect_refused(many + "]", move, "collision_rules_bound_exceeded");
}

TEST(CollisionRuleCodec, EncodingSpellsReleaseOutAndDecodesBack) {
  const auto rules = decode_cell_collision_rules(json(R"([
      {"rule_id":"everything","clearance_m":0.002},
      {"rule_id":"contact_all","a":[{"kind":"component","component_id":"arm"}],
       "b":[{"kind":"scene_objects"}],"contact":{}},
      {"rule_id":"release_all","a":[{"kind":"attached_object","component_id":"gripper"}],
       "b":[{"kind":"link","component_id":"arm","link":"base"}],"release":{}}])"),
                                                 "collision_rules");
  const CanonicalValue encoded = encode_collision_rules(rules);
  EXPECT_EQ(decode_cell_collision_rules(encoded, "collision_rules"), rules);
  const std::string text = manyforge::deployment::canonical_json(encoded);
  EXPECT_NE(text.find(R"("contact":{"end_from_fraction":0.8})"), std::string::npos);
  EXPECT_NE(text.find(R"("release_evidence_mode":"signed_penetration")"), std::string::npos);
  EXPECT_NE(text.find(R"("release_tolerance_m":)"), std::string::npos);
  EXPECT_NE(text.find(R"("allow_recontact":false)"), std::string::npos);
}

}  // namespace

namespace {

TEST(CollisionRuleCodec, ModelGroupsAreCellSelectionsOnly) {
  constexpr std::string_view cell = R"([
      {"rule_id":"everything","clearance_m":0.001},
      {"rule_id":"arm_scene",
       "a":[{"kind":"model_group","component_id":"robot","group":"manipulator"}],
       "b":[{"kind":"scene_objects"}],
       "clearance_m":0.01}])";
  const auto rules = decode_cell_collision_rules(json(cell), "collision_rules");
  ASSERT_EQ(rules.size(), 2U);
  const auto &group = std::get<core::CollisionRuleModelGroupSelection>(rules[1].a[0]);
  EXPECT_EQ(group.component_id.value(), "robot");
  EXPECT_EQ(group.group, "manipulator");
  EXPECT_EQ(decode_cell_collision_rules(encode_collision_rules(rules), "collision_rules"), rules);

  expect_refused(R"([{"rule_id":"arm_scene",
       "a":[{"kind":"model_group","component_id":"robot","group":"manipulator"}],
       "b":[{"kind":"scene_objects"}],"clearance_m":0.01}])",
                 CollisionRuleRows::kMove, "collision_rule_selection_invalid");
  expect_refused(R"([{"rule_id":"everything","clearance_m":0.001},
      {"rule_id":"arm_scene","a":[{"kind":"model_group","component_id":"robot"}],
       "b":[{"kind":"scene_objects"}],"clearance_m":0.01}])",
                 CollisionRuleRows::kCell, "collision_rule_selection_invalid");
}

// OD-371: a move row switches a generated tool's spring slider on: the
// slider in place of `a`, `b`, and a contact holding its end window only.
TEST(CollisionRuleCodec, MoveRowsSwitchASpringSliderOn) {
  const auto decoded = decode_collision_rules(json(R"([
      {"rule_id":"tube_into_endplate",
       "spring_slider":{"component_id":"tube_holder","element_id":"holder_rod"},
       "b":[{"kind":"scene_object","scene_resource_id":"endplate"}],
       "contact":{"end_from_fraction":0}},
      {"rule_id":"default_window",
       "spring_slider":{"component_id":"tube_holder","element_id":"holder_rod"},
       "b":[{"kind":"scene_objects"}]}])"),
      CollisionRuleRows::kMove, "collision_rules");
  ASSERT_EQ(decoded.rows.size(), 2U);
  const auto &row = decoded.rows[0];
  ASSERT_TRUE(row.spring_slider.has_value());
  EXPECT_EQ(row.spring_slider->component_id.value(), "tube_holder");
  EXPECT_EQ(row.spring_slider->collision_body_id.value(), "holder_rod");
  EXPECT_TRUE(row.rule.a.empty());
  ASSERT_EQ(row.rule.b.size(), 1U);
  const auto &contact = std::get<core::CollisionContactRule>(row.rule.value);
  EXPECT_DOUBLE_EQ(contact.end_from_fraction, 0.0);
  EXPECT_FALSE(contact.max_penetration_m.has_value());
  EXPECT_DOUBLE_EQ(std::get<core::CollisionContactRule>(decoded.rows[1].rule.value)
                       .end_from_fraction,
                   core::kDefaultContactEndFromFraction);

  const std::string slider =
      R"("spring_slider":{"component_id":"tube_holder","element_id":"holder_rod"})";
  const std::string b = R"("b":[{"kind":"scene_objects"}])";
  expect_refused("[{\"rule_id\":\"everything\",\"clearance_m\":0.001},{\"rule_id\":\"s\"," +
                     slider + "," + b + "}]",
                 CollisionRuleRows::kCell, "collision_rule_selection_invalid");
  expect_refused("[{\"rule_id\":\"s\"," + slider + "," + b +
                     R"(,"a":[{"kind":"scene_objects"}]}])",
                 CollisionRuleRows::kMove, "collision_rule_selection_invalid");
  expect_refused("[{\"rule_id\":\"s\"," + slider + "}]", CollisionRuleRows::kMove,
                 "collision_rule_selection_invalid");
  expect_refused("[{\"rule_id\":\"s\"," + slider + "," + b +
                     R"(,"contact":{"max_penetration_m":0.01}}])",
                 CollisionRuleRows::kMove, "collision_rule_value_invalid");
  expect_refused("[{\"rule_id\":\"s\"," + slider + "," + b + R"(,"clearance_m":0}])",
                 CollisionRuleRows::kMove, "collision_rule_value_invalid");
  expect_refused(
      R"([{"rule_id":"s","spring_slider":{"component_id":"tube_holder"},"b":[{"kind":"scene_objects"}]}])",
      CollisionRuleRows::kMove, "collision_rule_selection_invalid");
}

// OD-371: an attached_object may name the collision body that carries it.
TEST(CollisionRuleCodec, CarriedObjectsOfOneBodyRoundTrip) {
  const auto rules = decode_cell_collision_rules(json(R"([
      {"rule_id":"everything","clearance_m":0.001},
      {"rule_id":"tube_apart",
       "a":[{"kind":"attached_object","component_id":"tube_holder","collision_body_role_id":"holder_rod"}],
       "b":[{"kind":"scene_objects"}],
       "clearance_m":0.002}])"),
                                                 "collision_rules");
  const auto &carried = std::get<core::CollisionRuleAttachedObjectSelection>(rules[1].a[0]);
  ASSERT_TRUE(carried.collision_body_role_id.has_value());
  EXPECT_EQ(*carried.collision_body_role_id, "holder_rod");
  EXPECT_EQ(decode_cell_collision_rules(encode_collision_rules(rules), "collision_rules"), rules);
}

}  // namespace
