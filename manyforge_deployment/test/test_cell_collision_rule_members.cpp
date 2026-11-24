#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <manyforge_deployment/cell_collision_rule_members.hpp>

namespace {

namespace core = manyforge::core;
using manyforge::deployment::CellCollisionFacts;
using manyforge::deployment::CellOpenMeshes;
using manyforge::deployment::cell_mesh_depth_reports;
using manyforge::deployment::cell_collision_rule_changes;
using manyforge::deployment::cell_collision_rules_overlap;
using manyforge::deployment::collision_rule_side_members;
using Member = manyforge::deployment::CollisionRuleMember;

[[nodiscard]] CellCollisionFacts facts() {
  CellCollisionFacts result;
  auto &robot = result.components["robot"];
  robot.links = {"link5", "link6", "base"};
  robot.collision_bodies["default_collision"] = {"link5", "link6", "base"};
  // `tool0` carries no collision body: the group selects only link5, link6.
  robot.model_groups["wrist"] = {"link5", "link6", "tool0"};
  auto &gripper = result.components["gripper"];
  gripper.links = {"left_pad", "right_pad", "body"};
  gripper.collision_bodies["pads"] = {"left_pad", "right_pad"};
  return result;
}

[[nodiscard]] core::CollisionRuleDTO rule(std::string id,
                                          std::vector<core::CollisionRuleSelectionDTO> a,
                                          std::vector<core::CollisionRuleSelectionDTO> b) {
  return {std::move(id), std::move(a), std::move(b), core::CollisionContactRule{}};
}

[[nodiscard]] core::CollisionRuleDTO everything(double clearance_m) {
  return {"everything", {}, {}, core::CollisionClearanceRule{clearance_m}};
}

const core::ControllableComponentId kRobot{"robot"};
const core::ControllableComponentId kGripper{"gripper"};

TEST(CellCollisionRuleMembers, ResolvesEveryCellSelection) {
  const auto members = collision_rule_side_members(
      {core::CollisionRuleModelGroupSelection{kRobot, "wrist"},
       core::CollisionRuleCollisionBodySelection{{kGripper, core::CollisionBodyRoleId("pads")}},
       core::CollisionRuleAttachedObjectSelection{kGripper},
       core::CollisionRuleSceneObjectsSelection{}},
      facts());
  const std::vector<Member> expected{
      {Member::Kind::kLink, "gripper", "left_pad"},
      {Member::Kind::kLink, "gripper", "right_pad"},
      {Member::Kind::kLink, "robot", "link5"},
      {Member::Kind::kLink, "robot", "link6"},
      {Member::Kind::kAttachedObject, "gripper", ""},
      {Member::Kind::kSceneObjects, "", ""},
  };
  EXPECT_EQ(members, expected);
  EXPECT_EQ(collision_rule_side_members({core::CollisionRuleComponentSelection{kRobot}}, facts())
                .size(),
            3U);
  EXPECT_TRUE(collision_rule_side_members(
                  {core::CollisionRuleModelGroupSelection{kRobot, "missing"}}, facts())
                  .empty());
}

TEST(CellCollisionRuleMembers, RefusesTwoRulesHoldingOnePair) {
  const std::vector<core::CollisionRuleDTO> rules{
      everything(0.001),
      rule("pads_part", {core::CollisionRuleCollisionBodySelection{
                            {kGripper, core::CollisionBodyRoleId("pads")}}},
           {core::CollisionRuleAttachedObjectSelection{kGripper}}),
      rule("gripper_part", {core::CollisionRuleComponentSelection{kGripper}},
           {core::CollisionRuleAttachedObjectSelection{kGripper}}),
  };
  const auto overlap = cell_collision_rules_overlap(rules, facts());
  ASSERT_TRUE(overlap.has_value());
  EXPECT_EQ(overlap->code, "invalid_collision_rule");
  EXPECT_EQ(overlap->details.at("reason"), "collision_rules_overlap");
  EXPECT_EQ(overlap->details.at("collision_rule_id"), "pads_part");
  EXPECT_EQ(overlap->details.at("other_collision_rule_id"), "gripper_part");
}

TEST(CellCollisionRuleMembers, AcceptsDisjointRulesAndCarriedSymbols) {
  const std::vector<core::CollisionRuleDTO> rules{
      everything(0.001),
      rule("wrist_scene", {core::CollisionRuleModelGroupSelection{kRobot, "wrist"}},
           {core::CollisionRuleSceneObjectsSelection{}}),
      rule("base_scene", {core::CollisionRuleLinkSelection{kRobot, "base"}},
           {core::CollisionRuleSceneObjectsSelection{}}),
      // A carried object and the detached Scene objects are different symbols.
      rule("part_scene", {core::CollisionRuleAttachedObjectSelection{kGripper}},
           {core::CollisionRuleSceneObjectsSelection{}}),
  };
  EXPECT_FALSE(cell_collision_rules_overlap(rules, facts()).has_value());
}

TEST(CellCollisionRuleMembers, ListsAddedRemovedAndChangedRules) {
  const auto wrist = rule("wrist_scene", {core::CollisionRuleModelGroupSelection{kRobot, "wrist"}},
                          {core::CollisionRuleSceneObjectsSelection{}});
  const auto gone = rule("base_scene", {core::CollisionRuleLinkSelection{kRobot, "base"}},
                         {core::CollisionRuleSceneObjectsSelection{}});
  const auto added = rule("part_scene", {core::CollisionRuleAttachedObjectSelection{kGripper}},
                          {core::CollisionRuleSceneObjectsSelection{}});
  auto before = facts();
  before.components["robot"].model_groups["wrist"] = {"link6"};
  const auto changes = cell_collision_rule_changes({everything(0.001), wrist, gone}, before,
                                                   {everything(0.002), wrist, added}, facts());
  const auto &rows = changes.as_array();
  ASSERT_EQ(rows.size(), 4U);
  const auto field = [&](std::size_t index, const char *name) {
    return rows[index].find_object_member(name)->as_string();
  };
  EXPECT_EQ(field(0U, "rule_id"), "everything");
  EXPECT_EQ(field(0U, "change"), "changed");
  // Same row, members changed with the model: listed.
  EXPECT_EQ(field(1U, "rule_id"), "wrist_scene");
  EXPECT_EQ(field(1U, "change"), "changed");
  EXPECT_EQ(rows[1U].find_object_member("now")->find_object_member("a")->as_array().size(), 2U);
  EXPECT_EQ(rows[1U].find_object_member("was")->find_object_member("a")->as_array().size(), 1U);
  EXPECT_EQ(field(2U, "rule_id"), "part_scene");
  EXPECT_EQ(field(2U, "change"), "added");
  EXPECT_TRUE(rows[2U].find_object_member("was")->is_null());
  EXPECT_EQ(field(3U, "rule_id"), "base_scene");
  EXPECT_EQ(field(3U, "change"), "removed");
  EXPECT_TRUE(rows[3U].find_object_member("now")->is_null());
}

TEST(CellCollisionRuleMembers, ListsNothingWhenNothingChanged) {
  const std::vector<core::CollisionRuleDTO> rules{everything(0.001)};
  EXPECT_TRUE(cell_collision_rule_changes(rules, facts(), rules, facts()).as_array().empty());
}

// OD-363: a depth rule or trip depth that selects a mesh that is not closed
// is reported in the Apply preview, one row per such mesh.
TEST(CellCollisionRuleMembers, ReportsDepthsThatSelectAnOpenMesh) {
  CellOpenMeshes open;
  open.links["left_pad"] = "package://gripper/pad.stl";
  open.scene_objects["endplate"] = "meshes/endplate.stl";
  auto depth_rule = rule("pads_scene", {core::CollisionRuleCollisionBodySelection{
                                            {kGripper, core::CollisionBodyRoleId("pads")}}},
                         {core::CollisionRuleSceneObjectsSelection{}});
  depth_rule.value = core::CollisionContactRule{0.004, 0.0, std::nullopt};
  // A contact rule without a depth and a clearance rule measure no depth.
  const auto touch_only = rule("body_scene", {core::CollisionRuleComponentSelection{kGripper}},
                               {core::CollisionRuleSceneObjectsSelection{}});

  manyforge::deployment::IoSignalProviderConfiguration provider;
  provider.profile = "geometric_contact";
  // A Mock input's collision body (OD-371) is reported by its links.
  provider.contact_a = {
      core::CollisionRuleCollisionBodySelection{{kGripper, core::CollisionBodyRoleId("pads")}}};
  provider.contact_b = {core::CollisionRuleSceneObjectSelection{core::SceneResourceId("endplate")}};
  provider.trip_depth_m = 0.004;
  const manyforge::deployment::RetainedIoSignalConfiguration trip{
      core::RuntimeUnitId("tool_io"),
      core::CommandTargetId("tool_io"),
      core::ResourceRoleId("io"),
      {core::IoSignalId{core::ControllableComponentId("tool_io"), "TI0"}, core::InterruptSourceId("tool_io"),
       core::IoSignalValueType::kBoolean, core::IoSignalDirection::kInput,
       core::IoSignalUpdateMode::kSampled, core::IoTransitionCapability::kSampledOnly,
       {core::IoFreshnessMode::kSampleAge, 25}, std::nullopt,
       core::IoSignalLivenessMode::kPeriodic},
      provider};
  auto no_depth = trip;
  no_depth.provider.trip_depth_m.reset();

  const auto reports = cell_mesh_depth_reports({everything(0.001), depth_rule, touch_only},
                                               {trip, no_depth}, facts(), open);
  const auto &rows = reports.as_array();
  ASSERT_EQ(rows.size(), 4U);
  const auto text = [&](std::size_t index, const char *name) {
    return rows[index].find_object_member(name)->as_string();
  };
  const auto member = [&](std::size_t index, const char *name) {
    return rows[index].find_object_member("member")->find_object_member(name)->as_string();
  };
  for (std::size_t index = 0; index < rows.size(); ++index)
    EXPECT_EQ(text(index, "code"), "mesh_not_closed");
  EXPECT_EQ(text(0U, "collision_rule_id"), "pads_scene");
  EXPECT_EQ(member(0U, "kind"), "link");
  EXPECT_EQ(member(0U, "link"), "left_pad");
  EXPECT_EQ(text(0U, "mesh_uri"), "package://gripper/pad.stl");
  EXPECT_EQ(text(1U, "collision_rule_id"), "pads_scene");
  EXPECT_EQ(member(1U, "kind"), "scene_object");
  EXPECT_EQ(member(1U, "scene_resource_id"), "endplate");
  EXPECT_EQ(text(2U, "signal_id"), "tool_io/TI0");
  EXPECT_EQ(member(2U, "link"), "left_pad");
  EXPECT_EQ(text(3U, "signal_id"), "tool_io/TI0");
  EXPECT_EQ(member(3U, "scene_resource_id"), "endplate");

  EXPECT_TRUE(cell_mesh_depth_reports({depth_rule}, {trip}, facts(), {}).as_array().empty());
}

}  // namespace
