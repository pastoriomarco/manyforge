#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include "activation_deployment_compiler_internal.hpp"
#include "activation_deployment_intent_internal.hpp"

// The authored `attachments` rows of a Deployment intent.
// The full compile of a mounted tool is PrepareActivationToolSplit.
namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

Value Pose() {
  return Value(Object{
      {"orientation_xyzw", Value(Array{Value(0.0), Value(0.0), Value(0.0), Value(1.0)})},
      {"position_m", Value(Array{Value(0.0), Value(0.0), Value(0.0)})}});
}

Value Physical(std::string_view id, bool with_pose) {
  Object row{
      {"component_id", Value(std::string(id))},
      {"component_profile_id", Value("profile")},
      {"configuration", Value(Object{})},
      {"model_source",
       Value(Object{{"package_roots", Value(Object{})},
                    {"urdf", Value(Object{{"project_path", Value("model.urdf")}})}})},
      {"target_settings", Value(Object{})},
  };
  if (with_pose) row.emplace_back("root_pose_in_cell", Pose());
  return Value(std::move(row));
}

Value Mount(std::optional<Value> pose = std::nullopt) {
  Object row{
      {"child", Value(Object{{"component_id", Value("lite_gripper")},
                             {"interface", Value("mount")}})},
      {"parent", Value(Object{{"component_id", Value("lite6_robot")},
                              {"interface", Value("flange")}})},
  };
  if (pose) row.emplace_back("pose", *pose);
  return Value(std::move(row));
}

Value Intent(Array components, Array attachments) {
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v3")},
      {"collision_rules",
       Value(Array{Value(Object{{"clearance_m", Value(0.001)},
                   {"rule_id", Value("everything")}})})},
                      {"attachments", Value(std::move(attachments))},
                      {"components", Value(std::move(components))},
                      {"policies", Value(Object{})}});
}

std::vector<detail::CapturedComponentModelSource> Captured() {
  const auto source = [](std::string id) {
    return detail::CapturedComponentModelSource{
        std::move(id),
        {"asset://model.urdf", "application/xml",
         c::ContentDigest("sha256:" + std::string(64U, '0')), ""},
        std::nullopt,
        {}};
  };
  return {source("lite6_robot"), source("lite_gripper")};
}

detail::ActivationDeploymentIntent Parse(Array components, Array attachments) {
  return detail::parse_activation_deployment_intent(
      Intent(std::move(components), std::move(attachments)), Captured());
}

// OD-352, OD-357: a row's `model_source.parameters` (a parameter file, or
// the parameters inline) replaces urdf, srdf and package_roots; its captured
// model always has an SRDF.
TEST(ActivationParameterFile, ParsesAParameterFileRowAlone) {
  const auto row = [](Object source) {
    auto value = Physical("lite_gripper", false).as_object();
    for (auto &[name, field] : value)
      if (name == "model_source") field = Value(std::move(source));
    return Value(std::move(value));
  };
  const auto parameters =
      Value(Object{{"project_path", Value("tool/probe.tool.yaml")}});
  const auto inline_parameters = Value(Object{
      {"api_version", Value("manyforge.parametric_tool.v1")},
      {"kind", Value("tool")}});
  auto captured = Captured();
  captured[1].srdf = detail::CapturedModelSourceArtifact{
      "asset://model.srdf", "application/xml",
      c::ContentDigest("sha256:" + std::string(64U, '1')), ""};
  const auto parse = [&](Object source, auto sources) {
    return detail::parse_activation_deployment_intent(
        Intent(Array{Physical("lite6_robot", true), row(std::move(source))},
               Array{Mount()}),
        std::move(sources));
  };
  EXPECT_EQ(parse(Object{{"parameters", parameters}}, captured).components.size(), 2U);
  EXPECT_EQ(parse(Object{{"parameters", inline_parameters}}, captured).components.size(), 2U);
  EXPECT_THROW((void)parse(Object{{"parameters", parameters}}, Captured()),
               std::invalid_argument);
  EXPECT_THROW((void)parse(Object{{"package_roots", Value(Object{})},
                                  {"parameters", parameters}},
                           captured),
               std::invalid_argument);
  EXPECT_THROW((void)parse(Object{{"parameters", Value(Object{{"path", Value("x")}})}},
                           captured),
               std::invalid_argument);
}

TEST(ActivationAttachments, MountedChildOmitsItsRootPose) {
  const auto intent = Parse(
      Array{Physical("lite6_robot", true), Physical("lite_gripper", false)},
      Array{Mount()});
  ASSERT_EQ(intent.components.size(), 2U);
  EXPECT_TRUE(intent.components[0].root_pose_in_cell.has_value());
  EXPECT_FALSE(intent.components[1].root_pose_in_cell.has_value());
  EXPECT_EQ(intent.attachments.as_array().size(), 1U);
}

TEST(ActivationAttachments, AcceptsAnOptionalMountPose) {
  const auto intent = Parse(
      Array{Physical("lite6_robot", true), Physical("lite_gripper", false)},
      Array{Mount(Pose())});
  EXPECT_NE(intent.attachments.as_array().front().find_object_member("pose"), nullptr);
}

TEST(ActivationAttachments, RefusesAMountedChildWithARootPose) {
  EXPECT_THROW(
      (void)Parse(Array{Physical("lite6_robot", true), Physical("lite_gripper", true)},
                  Array{Mount()}),
      std::invalid_argument);
}

TEST(ActivationAttachments, UnmountedComponentStillRequiresItsRootPose) {
  EXPECT_THROW(
      (void)Parse(Array{Physical("lite6_robot", true), Physical("lite_gripper", false)},
                  Array{}),
      std::invalid_argument);
}

TEST(ActivationAttachments, RefusesMalformedRows) {
  const Array components{Physical("lite6_robot", true), Physical("lite_gripper", false)};
  auto unknown_field = Mount().as_object();
  unknown_field.emplace_back("component_attachment_id", Value("lite_gripper"));
  auto missing_interface = Mount().as_object();
  for (auto &[name, value] : missing_interface)
    if (name == "parent")
      value = Value(Object{{"component_id", Value("lite6_robot")}});
  Object bad_pose = Mount().as_object();
  bad_pose.emplace_back("pose", Value(Object{{"position_m", Value(Array{})}}));
  for (auto row : {Value(std::move(unknown_field)), Value(std::move(missing_interface)),
                   Value(std::move(bad_pose))}) {
    EXPECT_THROW((void)Parse(components, Array{std::move(row)}), std::invalid_argument);
  }
}

} // namespace

// A component row's `custom_tcps` (the GUI-authored custom TCPs):
// `{tcp_id, parent_frame, pose}` rows, uniquely sorted, with the
// authoring pose `{position_m, orientation: {rpy_deg} | {xyzw}}`.
namespace {

Value CustomTcp(std::string_view id, std::string_view parent, Value orientation) {
  return Value(Object{
      {"parent_frame", Value(std::string(parent))},
      {"pose", Value(Object{{"orientation", std::move(orientation)},
                            {"position_m", Value(Array{Value(0.0), Value(0.0), Value(0.12)})}})},
      {"tcp_id", Value(std::string(id))}});
}

Value Rpy() { return Value(Object{{"rpy_deg", Value(Array{Value(0.0), Value(0.0), Value(90.0)})}}); }

Value Xyzw(double w) {
  return Value(Object{{"xyzw", Value(Array{Value(0.0), Value(0.0), Value(0.0), Value(w)})}});
}

Value RobotWithCustomTcps(Array rows) {
  auto row = Physical("lite6_robot", true).as_object();
  row.emplace_back("custom_tcps", Value(std::move(rows)));
  return Value(std::move(row));
}

TEST(ActivationCustomTcps, ParsesSortedRowsInEitherOrientationSpelling) {
  const auto intent = Parse(
      Array{RobotWithCustomTcps(Array{CustomTcp("pen", "flange", Rpy()),
                                      CustomTcp("probe", "pen", Xyzw(1.0))}),
            Physical("lite_gripper", false)},
      Array{Mount()});
  const auto &custom = intent.components[0].custom_tcps;
  ASSERT_EQ(custom.size(), 2U);
  EXPECT_EQ(custom[0].tcp_id, "pen");
  EXPECT_EQ(custom[0].parent_frame, "flange");
  EXPECT_EQ(custom[1].parent_frame, "pen");  // chaining is the compiler's refusal
  EXPECT_TRUE(intent.components[1].custom_tcps.empty());
}

TEST(ActivationCustomTcps, RefusesUnsortedDuplicateOrMalformedRows) {
  const auto parse = [](Array rows) {
    return Parse(Array{RobotWithCustomTcps(std::move(rows)), Physical("lite_gripper", false)},
                 Array{Mount()});
  };
  EXPECT_THROW((void)parse(Array{CustomTcp("probe", "flange", Rpy()),
                                 CustomTcp("pen", "flange", Rpy())}),
               std::invalid_argument);
  EXPECT_THROW((void)parse(Array{CustomTcp("pen", "flange", Rpy()),
                                 CustomTcp("pen", "flange", Rpy())}),
               std::invalid_argument);
  EXPECT_THROW((void)parse(Array{CustomTcp("pen", "flange", Xyzw(2.0))}),
               std::invalid_argument);
  EXPECT_THROW((void)parse(Array{CustomTcp("Pen", "flange", Rpy())}), std::invalid_argument);
  EXPECT_THROW((void)parse(Array{CustomTcp("pen", "", Rpy())}), std::invalid_argument);
  auto extra = CustomTcp("pen", "flange", Rpy()).as_object();
  extra.emplace_back("fine_tuning", Value(Object{}));
  EXPECT_THROW((void)parse(Array{Value(std::move(extra))}), std::invalid_argument);
}

// A custom TCP's `contact_bodies`: absent keeps the component's default, a
// list (also an empty one) is taken as written, sorted.
TEST(ActivationCustomTcps, ContactBodiesAreOptionalAndSorted) {
  const auto with_bodies = [](Value bodies) {
    auto row = CustomTcp("pen", "flange", Rpy()).as_object();
    row.emplace_back("contact_bodies", std::move(bodies));
    return Value(std::move(row));
  };
  const auto parse = [](Value row) {
    return Parse(Array{RobotWithCustomTcps(Array{std::move(row)}),
                       Physical("lite_gripper", false)},
                 Array{Mount()})
        .components[0]
        .custom_tcps[0]
        .contact_bodies;
  };
  EXPECT_FALSE(parse(CustomTcp("pen", "flange", Rpy())).has_value());
  EXPECT_EQ(parse(with_bodies(Value(Array{}))),
            std::optional<std::vector<std::string>>(std::vector<std::string>{}));
  EXPECT_EQ(parse(with_bodies(Value(Array{Value("tip"), Value("gripper_fingers")}))),
            std::optional<std::vector<std::string>>(
                std::vector<std::string>{"gripper_fingers", "tip"}));
  EXPECT_THROW((void)parse(with_bodies(Value(Array{Value("tip"), Value("tip")}))),
               std::invalid_argument);
  EXPECT_THROW((void)parse(with_bodies(Value(Array{Value(1.0)}))), std::invalid_argument);
  EXPECT_THROW((void)parse(with_bodies(Value("tip"))), std::invalid_argument);
}

}  // namespace

// The Deployment's `collision_rules`: required, the first rule without
// selections, cell selections only; kept in the authored shape with release
// rules spelled out.
namespace {

Value IntentWithRules(Value rules) {
  auto root = Intent(Array{Physical("lite6_robot", true), Physical("lite_gripper", false)},
                     Array{Mount()})
                  .as_object();
  std::erase_if(root, [](const auto &field) { return field.first == "collision_rules"; });
  if (!rules.is_null()) root.emplace_back("collision_rules", std::move(rules));
  return Value(std::move(root));
}

Value Rule(std::string_view json) {
  return manyforge::deployment::parse_strict_json_model_yaml(json).root();
}

TEST(ActivationCollisionRules, KeepsTheAuthoredRulesWithReleaseSpelledOut) {
  const auto intent = detail::parse_activation_deployment_intent(
      IntentWithRules(Rule(R"([
          {"rule_id":"everything","clearance_m":0.002},
          {"rule_id":"gripper_on_parts",
           "a":[{"kind":"collision_body","component_id":"lite_gripper",
                 "collision_body_id":"gripper_fingers"}],
           "b":[{"kind":"scene_objects"}],"contact":{"max_penetration_m":0.001}},
          {"rule_id":"parts_leave",
           "a":[{"kind":"attached_object","component_id":"lite_gripper"}],
           "b":[{"kind":"scene_objects"}],"release":{}}])")),
      Captured());
  const auto &rules = intent.collision_rules.as_array();
  ASSERT_EQ(rules.size(), 3U);
  EXPECT_EQ(rules[0].find_object_member("clearance_m")->as_number(), 0.002);
  EXPECT_EQ(rules[1].find_object_member("rule_id")->as_string(), "gripper_on_parts");
  const auto *release = rules[2].find_object_member("release");
  ASSERT_NE(release, nullptr);
  EXPECT_EQ(release->as_object().size(), 8U);
  EXPECT_EQ(release->find_object_member("release_evidence_mode")->as_string(),
            "signed_penetration");
}

TEST(ActivationCollisionRules, RefusesAMissingOrMisshapenList) {
  const auto refused = [](Value rules, std::string_view code) {
    try {
      (void)detail::parse_activation_deployment_intent(IntentWithRules(std::move(rules)),
                                                       Captured());
      FAIL() << "accepted";
    } catch (const manyforge::core::TypedError &error) {
      EXPECT_EQ(error.code(), code) << error.what();
    } catch (const std::invalid_argument &error) {
      EXPECT_TRUE(code.empty()) << error.what();
    }
  };
  refused(Value(nullptr), "");  // the key is required
  refused(Rule(R"([])"), "collision_rule_selection_invalid");
  refused(Rule(R"([{"rule_id":"r","a":[{"kind":"scene_objects"}],
                    "b":[{"kind":"scene_objects"}],"contact":{}}])"),
          "collision_rule_selection_invalid");
  refused(Rule(R"([{"rule_id":"everything","clearance_m":0.06}])"),
          "collision_rule_value_invalid");
  refused(Rule(R"([{"rule_id":"everything","clearance_m":0.001},
                   {"rule_id":"tip","a":[{"kind":"active_tcp_contact_bodies"}],
                    "b":[{"kind":"scene_objects"}],"contact":{}}])"),
          "collision_rule_selection_invalid");
  refused(Rule(R"([{"rule_id":"everything","clearance_m":0.001},
                   {"rule_id":"everything","a":[{"kind":"scene_objects"}],
                    "b":[{"kind":"scene_objects"}],"contact":{}}])"),
          "collision_rule_id_duplicate");
}

}  // namespace

// A mount's `primary` and a component's `default_tcp`, then
// the default TCP each component's graph row carries (`compiled_default_tcp`).
namespace {

Value Primary(Value mount, Value primary) {
  auto row = mount.as_object();
  row.emplace_back("primary", std::move(primary));
  return Value(std::move(row));
}

Value WithDefaultTcp(Value component, Value default_tcp) {
  auto row = component.as_object();
  row.emplace_back("default_tcp", std::move(default_tcp));
  return Value(std::move(row));
}

Value Pair(std::string_view target, std::string_view role) {
  return Value(Object{{"command_target_id", Value(std::string(target))},
                      {"frame_role_id", Value(std::string(role))}});
}

TEST(ActivationB5, ParsesAMountPrimaryAndAComponentDefaultTcp) {
  const auto intent = Parse(
      Array{Physical("lite6_robot", true),
            WithDefaultTcp(Physical("lite_gripper", false), Pair("gripper", "cup_center"))},
      Array{Primary(Mount(), Value(true))});
  ASSERT_TRUE(intent.components[1].default_tcp.has_value());
  EXPECT_EQ(*intent.components[1].default_tcp,
            (c::ComponentDefaultTcpDTO{c::CommandTargetId("gripper"),
                                       c::FrameRoleId("cup_center")}));
  EXPECT_FALSE(intent.components[0].default_tcp.has_value());
  EXPECT_TRUE(intent.attachments.as_array().front().find_object_member("primary")->as_bool());
}

Value Exclusions(Array pairs) {
  auto row = Mount().as_object();
  row.emplace_back("collision_exclusions", Value(std::move(pairs)));
  return Value(std::move(row));
}

Value ExclusionPair(Value child_link, Value parent_link) {
  return Value(Object{{"child_link", std::move(child_link)},
                      {"parent_link", std::move(parent_link)}});
}

// MOUNT_COLLISION_EXCLUSIONS_PLAN: a mount's optional authored pairs; `[]`
// is the same as absent; repeated, non-text and unknown-field pairs refused.
TEST(ActivationMountCollisionExclusions, ParsesPairsAndRefusesMalformedOnes) {
  const Array components{Physical("lite6_robot", true), Physical("lite_gripper", false)};
  EXPECT_NO_THROW((void)Parse(components, Array{Exclusions(Array{
      ExclusionPair(Value("lite_gripper_left_finger"), Value("link5")),
      ExclusionPair(Value("lite_gripper_left_finger"), Value("link4"))})}));
  EXPECT_NO_THROW((void)Parse(components, Array{Exclusions(Array{})}));
  const auto pair = ExclusionPair(Value("lite_gripper_left_finger"), Value("link4"));
  auto extra = pair.as_object();
  extra.emplace_back("reason", Value("Never"));
  for (auto bad : {Array{pair, pair},
                   Array{ExclusionPair(Value("lite_gripper_left_finger"), Value(4.0))},
                   Array{ExclusionPair(Value(""), Value("link4"))},
                   Array{Value(std::move(extra))}}) {
    EXPECT_THROW((void)Parse(components, Array{Exclusions(std::move(bad))}),
                 std::invalid_argument);
  }
  auto not_array = Mount().as_object();
  not_array.emplace_back("collision_exclusions", Value("link4"));
  EXPECT_THROW((void)Parse(components, Array{Value(std::move(not_array))}),
               std::invalid_argument);
}

TEST(ActivationB5, RefusesMalformedPrimaryOrDefaultTcp) {
  EXPECT_THROW((void)Parse(Array{Physical("lite6_robot", true), Physical("lite_gripper", false)},
                           Array{Primary(Mount(), Value("yes"))}),
               std::invalid_argument);
  for (auto bad : {Value(Object{{"command_target_id", Value("gripper")}}), Pair("gripper", "Tcp"),
                   Value("gripper.tcp")}) {
    EXPECT_THROW((void)Parse(Array{Physical("lite6_robot", true),
                                   WithDefaultTcp(Physical("lite_gripper", false), std::move(bad))},
                             Array{Mount()}),
                 std::invalid_argument);
  }
}

c::ComponentTypeFrameRoleDTO Frame(std::string_view role, bool tcp) {
  return c::ComponentTypeFrameRoleDTO{c::FrameRoleId(std::string(role)),
                                      c::ModelRoleId("tool_model"),
                                      c::FrameRoleId(std::string(role)), {}, tcp};
}

c::ComponentTypeCommandTargetDTO Target(std::string_view id,
                                        std::vector<c::ComponentTypeFrameRoleDTO> frames,
                                        std::optional<std::string_view> default_tcp) {
  c::ComponentTypeCommandTargetDTO target{c::CommandTargetId(std::string(id)), {}, {}, {}, {}, {}};
  target.frame_roles = std::move(frames);
  if (default_tcp) target.default_tcp_frame_role_id = c::FrameRoleId(std::string(*default_tcp));
  return target;
}

c::ComponentTypeDTO Type(std::vector<c::ComponentTypeCommandTargetDTO> targets) {
  return c::ComponentTypeDTO{c::ComponentTypeId("tool_type"), c::ComponentTypeRevision("1"),
                             {}, {}, std::nullopt, {}, std::move(targets), {}};
}

detail::ActivationDeploymentComponent Row(
    std::optional<c::ComponentDefaultTcpDTO> default_tcp = std::nullopt,
    std::vector<std::string> custom = {}) {
  detail::ActivationDeploymentComponent row{};
  row.component_id = "tool";
  for (auto &id : custom) row.custom_tcps.push_back({std::move(id), "tcp", Value(Object{})});
  row.default_tcp = std::move(default_tcp);
  return row;
}

c::ComponentDefaultTcpDTO Tcp(std::string_view target, std::string_view role) {
  return {c::CommandTargetId(std::string(target)), c::FrameRoleId(std::string(role))};
}

// The typed refusal of `compiled_default_tcp`: its catalog code and message.
std::pair<std::string, std::string> Refusal(const detail::ActivationDeploymentComponent &row,
                                            const c::ComponentTypeDTO &type) {
  try {
    (void)detail::compiled_default_tcp(row, type);
  } catch (const c::TypedError &error) {
    return {error.code(), error.what()};
  }
  return {};
}

TEST(ActivationB5, OneTargetDeclaringADefaultNeedsNoField) {
  // A gripper with two tagged TCPs on one target (as the 2F-85: `base`, `tcp`).
  const auto type = Type({Target("gripper", {Frame("base", true), Frame("tcp", true)}, "tcp")});
  EXPECT_EQ(detail::compiled_default_tcp(Row(), type), Tcp("gripper", "tcp"));
  // An authored choice among them wins; a custom TCP may be chosen too.
  EXPECT_EQ(detail::compiled_default_tcp(Row(Tcp("gripper", "base")), type),
            Tcp("gripper", "base"));
  EXPECT_EQ(detail::compiled_default_tcp(Row(Tcp("gripper", "pen"), {"pen"}), type),
            Tcp("gripper", "pen"));
}

TEST(ActivationB5, TwoTargetToolWithTcpOnBothNeedsADefaultTcp) {
  const auto type = Type({Target("arm", {Frame("base", false), Frame("flange", true),
                                         Frame("tcp", true)}, "tcp"),
                          Target("gripper", {Frame("tcp", true)}, "tcp")});
  const auto [code, required] = Refusal(Row(), type);
  EXPECT_EQ(code, "component_default_tcp_required");
  EXPECT_NE(required.find("component_default_tcp_required: tool"), std::string::npos)
      << required;
  // Named, the pair is carried as that pair, target included.
  EXPECT_EQ(detail::compiled_default_tcp(Row(Tcp("gripper", "tcp")), type),
            Tcp("gripper", "tcp"));
  EXPECT_EQ(detail::compiled_default_tcp(Row(Tcp("arm", "tcp")), type), Tcp("arm", "tcp"));
}

TEST(ActivationB5, AnUnknownOrUntaggedPairIsRefused) {
  const auto type = Type({Target("arm", {Frame("base", false), Frame("flange", true),
                                         Frame("tcp", true)}, "tcp"),
                          Target("gripper", {Frame("tcp", true), Frame("side", false)}, "tcp"),
                          Target("camera", {Frame("lens", false)}, std::nullopt)});
  for (const auto &pair : {Tcp("wrist", "tcp"), Tcp("gripper", "side"), Tcp("arm", "base"),
                           Tcp("camera", "lens"), Tcp("gripper", "flange")}) {
    const auto [code, message] = Refusal(Row(pair), type);
    EXPECT_EQ(code, "component_default_tcp_unknown") << message;
    EXPECT_NE(message.find("component_default_tcp_unknown: tool"), std::string::npos)
        << pair.command_target_id.value() << "/" << pair.frame_role_id.value() << ": "
        << message;
  }
}

TEST(ActivationB5, AComponentWithoutTcpsHasNoDefault) {
  // An untagged `tcp` is no TCP; a target with no declared default
  // exposes no custom TCP either.
  const auto type = Type({Target("gripper", {Frame("tcp", false)}, std::nullopt)});
  EXPECT_FALSE(detail::compiled_default_tcp(Row(), type).has_value());
  EXPECT_FALSE(detail::compiled_default_tcp(Row(std::nullopt, {"pen"}), type).has_value());
  EXPECT_EQ(Refusal(Row(Tcp("gripper", "tcp")), type).first, "component_default_tcp_unknown");
}

}  // namespace
