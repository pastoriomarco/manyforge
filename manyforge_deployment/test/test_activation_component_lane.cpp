#include <gtest/gtest.h>
#include <algorithm>
#include <array>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/types/typed_error.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_generated_tool_internal.hpp"
#include "activation_io_provider_internal.hpp"
#include "activation_prepared_model_internal.hpp"

// `component_profile_id` names a component model (its descriptor `id`);
// `components[].lane` picks one of the model's `lanes` rows. Model-free
// components and lane-free (pneumatic or passive) tools name none.
namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

detail::ActivationDeploymentComponent Component(std::string profile_id,
                                                std::optional<std::string> lane) {
  detail::ActivationDeploymentComponent row{};
  row.component_id = "robot";
  row.component_profile_id = std::move(profile_id);
  row.target_settings = Value(Object{});
  row.configuration = Value(Object{{"runtime_kind", Value("ros2_control")}});
  row.lane = std::move(lane);
  return row;
}

std::vector<detail::ActivationComponentProfile>
Resolve(detail::ActivationDeploymentComponent component) {
  detail::ActivationDeploymentIntent intent{};
  intent.components.push_back(std::move(component));
  return detail::resolve_activation_component_profiles(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
}

std::string RefusalCode(detail::ActivationDeploymentComponent component) {
  try {
    (void)Resolve(std::move(component));
  } catch (const c::TypedError &error) {
    return error.code();
  }
  return {};
}

TEST(ActivationComponentLane, AuthoredLaneSelectsTheModelLaneRow) {
  for (const std::string lane : {"isaac", "mock"}) {
    const auto profiles = Resolve(Component("universal_robots.ur10e", lane));
    ASSERT_EQ(profiles.size(), 1U);
    EXPECT_EQ(profiles.front().lane, lane);
    EXPECT_EQ(profiles.front().model_family_id, "ur10e");
    ASSERT_TRUE(profiles.front().lane_row.has_value());
    // The row is the descriptor's `lanes.<lane>` row, verbatim.
    EXPECT_EQ(*profiles.front().lane_row,
              *profiles.front().model_descriptor.find_object_member("lanes")
                   ->find_object_member(lane));
    EXPECT_EQ(profiles.front().lane_row->find_object_member("adapter")->as_string(),
              lane);
  }
}

TEST(ActivationComponentLane, ModelWithLanesRequiresOneOfThem) {
  EXPECT_EQ(RefusalCode(Component("universal_robots.ur10e", std::nullopt)),
            "component_lane_required");
  // A machine's arm is placed only as a member of its machine.
  EXPECT_EQ(RefusalCode(Component("ufactory.lite6", "mock")),
            "deployment_machine_part_without_machine");
  EXPECT_EQ(RefusalCode(Component("universal_robots.ur5e", "isaac")),
            "component_lane_unknown");
  EXPECT_EQ(RefusalCode(Component("universal_robots.ur5e", "simulator")),
            "component_lane_unknown");
  // A position-commanded tool has no lane-free form.
  EXPECT_EQ(RefusalCode(Component("robotiq.robotiq_2f_85", std::nullopt)),
            "component_lane_required");
}

TEST(ActivationComponentLane, PneumaticToolMayBeLaneFree) {
  auto profiles = Resolve(Component("ufactory.lite_gripper", std::nullopt));
  ASSERT_EQ(profiles.size(), 1U);
  EXPECT_FALSE(profiles.front().lane.has_value());
  EXPECT_FALSE(profiles.front().lane_row.has_value());
  profiles = Resolve(Component("ufactory.lite_gripper", "mock"));
  EXPECT_EQ(profiles.front().lane, std::optional<std::string>("mock"));
}

// OD-336: the generic passive tool has nothing to drive (`lanes: {}`): it
// names no lane on any lane, and an authored lane is no lane of its model.
TEST(ActivationComponentLane, PassiveToolIsLaneFree) {
  const auto profiles = Resolve(Component("manyforge.passive_tool", std::nullopt));
  ASSERT_EQ(profiles.size(), 1U);
  EXPECT_EQ(profiles.front().model_family_id, "passive_tool");
  EXPECT_FALSE(profiles.front().lane.has_value());
  EXPECT_FALSE(profiles.front().lane_row.has_value());
  EXPECT_EQ(RefusalCode(Component("manyforge.passive_tool", "mock")),
            "component_lane_unknown");
}

// OD-336: a passive tool's payload is its URDF's inertials, the centre of
// mass in the mount frame; a URDF without inertial has none (0 kg).
TEST(ActivationComponentLane, PassiveToolPayloadComesFromTheUrdfInertials) {
  d::UrdfDocumentIr urdf{};
  urdf.links.push_back({"passive_tool_mount", std::nullopt, {}, {}});
  urdf.links.push_back({"body", std::nullopt, {}, {}});
  urdf.links.push_back({"passive_tool_tcp", std::nullopt, {}, {}});
  d::UrdfJointIr body{};
  body.name = "body_fix";
  body.type = d::UrdfJointType::kFixed;
  body.parent_link_name = "passive_tool_mount";
  body.child_link_name = "body";
  body.origin.xyz_m = {0.0, 0.0, 0.1};
  body.origin.rpy_rad = {0.0, 1.5707963267948966, 0.0};  // body x along mount -z
  d::UrdfJointIr tcp = body;
  tcp.name = "tcp_fix";
  tcp.parent_link_name = "body";
  tcp.child_link_name = "passive_tool_tcp";
  tcp.origin = {};
  urdf.joints = {body, tcp};
  EXPECT_FALSE(detail::passive_tool_urdf_payload(urdf, "passive_tool_mount").has_value());

  d::UrdfInertialIr inertial{};
  inertial.mass_kg = 0.5;
  inertial.origin.xyz_m = {0.02, 0.0, 0.0};
  urdf.links[1].inertial = inertial;
  inertial.mass_kg = 1.5;
  inertial.origin.xyz_m = {0.0, 0.0, 0.0};
  urdf.links[0].inertial = inertial;
  const auto payload = detail::passive_tool_urdf_payload(urdf, "passive_tool_mount");
  ASSERT_TRUE(payload.has_value());
  EXPECT_DOUBLE_EQ(payload->find_object_member("mass_kg")->as_number(), 2.0);
  const auto &center = payload->find_object_member("center_of_mass_m")->as_array();
  ASSERT_EQ(center.size(), 3U);
  // 0.5 kg at mount (0, 0, 0.1 - 0.02), 1.5 kg at the mount origin.
  EXPECT_NEAR(center[0].as_number(), 0.0, 1e-12);
  EXPECT_NEAR(center[1].as_number(), 0.0, 1e-12);
  EXPECT_NEAR(center[2].as_number(), 0.5 * 0.08 / 2.0, 1e-12);
}

// OD-350: `<collision>` links no binding covers join `default_collision`,
// sorted and merged with an authored default; full coverage changes nothing.
Value CollisionSemantics(std::vector<std::pair<std::string, std::vector<std::string>>> bodies) {
  Array rows;
  for (auto &[role, links] : bodies) {
    Array names;
    for (auto &link : links) names.emplace_back(std::move(link));
    rows.emplace_back(Object{{"collision_body_role_id", Value(std::move(role))},
                             {"source_link_names", Value(std::move(names))}});
  }
  return Value(Object{{"root_frame_role_id", Value("mount")},
                      {"collision_body_role_bindings", Value(std::move(rows))}});
}

d::UrdfDocumentIr CollisionUrdf() {
  d::UrdfDocumentIr urdf{};
  for (const auto *name : {"mount", "body", "finger_b", "finger_a"}) {
    d::UrdfLinkIr link{name, std::nullopt, {}, {}};
    if (std::string_view(name) != "mount") link.collisions.emplace_back();
    urdf.links.push_back(std::move(link));
  }
  return urdf;
}

Value Bindings(const Value &semantics) {
  return *semantics.find_object_member("collision_body_role_bindings");
}

TEST(ActivationPreparedModel, UncoveredCollisionLinksJoinTheDefaultBody) {
  const auto semantics = detail::default_collision_semantics(
      CollisionSemantics({{"body_collision", {"body"}}}), CollisionUrdf());
  ASSERT_TRUE(semantics.has_value());
  EXPECT_EQ(*semantics->find_object_member("root_frame_role_id"), Value("mount"));
  EXPECT_EQ(Bindings(*semantics),
            Bindings(CollisionSemantics({{"body_collision", {"body"}},
                                         {"default_collision", {"finger_a", "finger_b"}}})));
}

TEST(ActivationPreparedModel, FullyCoveredModelGetsNoDefaultBody) {
  EXPECT_FALSE(detail::default_collision_semantics(
                   CollisionSemantics({{"body_collision", {"body", "mount"}},
                                       {"fingers", {"finger_a", "finger_b"}}}),
                   CollisionUrdf())
                   .has_value());
}

TEST(ActivationPreparedModel, AuthoredDefaultBodyIsMerged) {
  const auto semantics = detail::default_collision_semantics(
      CollisionSemantics({{"default_collision", {"finger_b"}}, {"tool", {"body"}}}),
      CollisionUrdf());
  ASSERT_TRUE(semantics.has_value());
  EXPECT_EQ(Bindings(*semantics),
            Bindings(CollisionSemantics({{"default_collision", {"finger_a", "finger_b"}},
                                         {"tool", {"body"}}})));
}

TEST(ActivationPreparedModel, ModelWithoutCollisionLinksGetsNoDefaultBody) {
  d::UrdfDocumentIr urdf{};
  urdf.links.push_back({"mount", std::nullopt, {}, {}});
  urdf.links.push_back({"tcp", std::nullopt, {}, {}});
  EXPECT_FALSE(
      detail::default_collision_semantics(CollisionSemantics({}), urdf).has_value());
}

// OD-352 A1: an optional body (a passive tool's `tip`) binds only when the
// model has its links; without them it is left out, so the tool's links join
// `default_collision` alone. A non-optional body is kept whatever the model.
TEST(ActivationPreparedModel, OptionalBodyWithoutItsLinksIsLeftUnbound) {
  const std::vector<std::string> optional{"tip"};
  const auto semantics = CollisionSemantics(
      {{"body_collision", {"body"}}, {"tip", {"passive_tool_tip"}}});
  const auto without = detail::without_absent_optional_bodies(
      semantics, optional, nullptr);
  EXPECT_EQ(Bindings(without),
            Bindings(CollisionSemantics({{"body_collision", {"body"}}})));
  const auto urdf = CollisionUrdf();
  EXPECT_EQ(Bindings(detail::without_absent_optional_bodies(semantics, optional, &urdf)),
            Bindings(CollisionSemantics({{"body_collision", {"body"}}})));
  auto with_tip = CollisionUrdf();
  with_tip.links.push_back({"passive_tool_tip", std::nullopt, {}, {}});
  EXPECT_EQ(Bindings(detail::without_absent_optional_bodies(semantics, optional, &with_tip)),
            Bindings(semantics));
  EXPECT_EQ(Bindings(detail::without_absent_optional_bodies(semantics, {}, nullptr)),
            Bindings(semantics));
}

TEST(ActivationPreparedModel, OptionalBodiesComeFromTheDescriptorRoles) {
  EXPECT_TRUE(detail::optional_collision_body_role_ids(Value(Object{})).empty());
  const auto descriptor = Value(Object{
      {"roles", Value(Object{{"optional_collision_bodies", Value(Array{Value("tip")})}})}});
  EXPECT_EQ(detail::optional_collision_body_role_ids(descriptor),
            std::vector<std::string>{"tip"});
}

// OD-352, OD-357: what a tool's parameters declare (two jaw pairs, the
// outer one with pads), as capture carries it.
Value GeneratedComponent() {
  const auto links = [](std::vector<std::string> names) {
    Array result;
    for (auto &name : names) result.emplace_back(std::move(name));
    return Value(std::move(result));
  };
  const auto target = [&](std::string pair, std::vector<std::string> bodies,
                          std::string grip_body, std::string tcp_frame) {
    return Value(Object{
        {"command_target_id", Value(pair)},
        {"group", Value(pair)},
        {"joint", Value("tool_" + pair + "_joint")},
        {"collision_body_role_ids", links(std::move(bodies))},
        {"grip_body_role_id", Value(std::move(grip_body))},
        {"states", Value(Object{{"open", Value("open")}, {"closed", Value("closed")}})},
        {"command_duration_s", Value(0.5)},
        {"attach_frame", Value("tool_" + pair)},
        {"tcp_frame", Value(std::move(tcp_frame))},
    });
  };
  return Value(Object{
      {"mount_frame", Value("tool_mount")},
      {"collision_bodies",
       Value(Object{
           {"body", links({"tool_body"})},
           {"inner_fingers", links({"tool_inner_positive_jaw", "tool_inner_negative_jaw"})},
           {"outer_fingers", links({"tool_outer_positive_jaw", "tool_outer_negative_jaw"})},
           {"outer_pads", links({"tool_outer_positive_pad", "tool_outer_negative_pad"})},
       })},
      {"tcps",
       Value(Array{
           Value(Object{{"id", Value("tcp")},
                        {"frame", Value("tool_tcp")},
                        {"contact_body_role_ids", links({"outer_pads"})}}),
           Value(Object{{"id", Value("inner_tcp")},
                        {"frame", Value("tool_tcp_inner_tcp")},
                        {"contact_body_role_ids", links({"inner_fingers"})}}),
       })},
      {"gripper_targets",
       Value(Array{target("outer", {"outer_fingers", "outer_pads"}, "outer_pads", "tool_tcp"),
                   target("inner", {"inner_fingers"}, "inner_fingers", "tool_tcp_inner_tcp")})},
      {"tool_ends", Value(Array{})},
      {"spring_sliders", Value(Array{})},
  });
}

// One wiring row: a tool end and its valve output.
Value Wiring(const std::string &pair) {
  return Value(Object{
      {"command_target_id", Value(pair)},
      {"controllable_component_id", Value("gripper")},
      {"pneumatic_io",
       Value(Object{
           {"inputs", Value(Object{})},
           {"outputs", Value(Array{Value(Object{
                           {"close_value", Value(true)},
                           {"open_value", Value(false)},
                           {"signal", Value(Object{
                                          {"controllable_component_id", Value("gripper_io")},
                                          {"signal_name", Value(pair + "_valve")}})},
                       })})},
       })},
  });
}

detail::ActivationDeploymentComponent
GeneratedTool(std::string profile_id, Array rows) {
  auto row = Component(std::move(profile_id), std::nullopt);
  row.component_id = "gripper";
  row.configuration = Value(Object{
      {"runtime_kind", Value("ros2_control")},
      {"runtime_configuration",
       Value(Object{{"gripper_target_configurations", Value(std::move(rows))}})}});
  // The profile resolution reads the declaration only, not the model bytes.
  row.model_source = detail::CapturedComponentModelSource{
      "gripper",
      detail::CapturedModelSourceArtifact{
          "urdf", "application/xml",
          c::ContentDigest("sha256:" + std::string(64, '0')), "<robot/>"},
      std::nullopt,
      {},
      GeneratedComponent()};
  return row;
}

std::vector<std::string> CommandTargets(const Value &shape) {
  std::vector<std::string> result;
  for (const auto &target : shape.find_object_member("command_targets")->as_array())
    result.push_back(target.find_object_member("command_target_id")->as_string());
  return result;
}

TEST(ActivationGeneratedTool, EachJawPairIsAGripperTargetBesideTheBody) {
  const auto tool = detail::parse_generated_tool_component(GeneratedComponent());
  const auto shape = detail::generated_tool_type_shape(tool);
  EXPECT_EQ(CommandTargets(shape), (std::vector<std::string>{"tool", "outer", "inner"}));
  const auto &targets = shape.find_object_member("command_targets")->as_array();
  // The body target has every collision body and TCP; the default is `tcp`.
  EXPECT_EQ(targets[0].find_object_member("default_tcp_frame_role_id")->as_string(), "tcp");
  EXPECT_EQ(targets[0].find_object_member("frame_roles")->as_array().size(), 2U);
  EXPECT_EQ(targets[0].find_object_member("collision_body_roles")->as_array().size(), 5U);
  EXPECT_TRUE(targets[0].find_object_member("capabilities")->as_array().empty());
  // A pair has its own group, joint and bodies, and the gripper capabilities.
  const auto &outer = targets[1];
  EXPECT_EQ(outer.find_object_member("capabilities")->as_array().size(), 4U);
  EXPECT_EQ(outer.find_object_member("collision_body_roles")->as_array().size(), 3U);
  EXPECT_EQ(outer.find_object_member("model_group_roles")
                ->as_array().front().find_object_member("asset_model_group_role_id")->as_string(),
            "outer");
  EXPECT_EQ(outer.find_object_member("resource_roles")
                ->as_array().front().find_object_member("asset_resource_role_id")->as_string(),
            detail::generated_tool_resource_role_id("outer"));
  const auto semantics = detail::generated_tool_semantics(tool);
  EXPECT_EQ(semantics.find_object_member("model_group_role_bindings")->as_array().size(), 2U);
  EXPECT_EQ(semantics.find_object_member("resource_role_bindings")->as_array().size(), 2U);
  EXPECT_EQ(semantics.find_object_member("frame_role_bindings")->as_array().size(), 3U);
}

TEST(ActivationGeneratedTool, EveryJawPairIsATargetOfTheDerivedType) {
  const auto profiles =
      Resolve(GeneratedTool("manyforge.tool", Array{Wiring("inner"), Wiring("outer")}));
  ASSERT_TRUE(profiles.front().generated_tool.has_value());
  EXPECT_EQ(profiles.front().generated_tool->gripper_targets.size(), 2U);
  ASSERT_TRUE(profiles.front().derived_component_type_id.has_value());
  EXPECT_TRUE(profiles.front().derived_component_type_id->starts_with("manyforge.tool."));
  EXPECT_EQ(profiles.front().model_family.find_object_member("semantics")
                ->find_object_member("resource_role_bindings")->as_array().size(),
            2U);
  // A lane-free tool, as every tool without a controller of its own.
  EXPECT_FALSE(profiles.front().lane.has_value());
  // The TCPs keep their own contact bodies through the derived type.
  const auto shape = detail::generated_tool_type_shape(*profiles.front().generated_tool);
  const auto &frames = shape.find_object_member("command_targets")->as_array().front()
                           .find_object_member("frame_roles")->as_array();
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[1].find_object_member("contact_body_role_ids")->as_array().front().as_string(),
            "inner_fingers");
}

// OD-371: a tool with a spring slider `rod` (its body on the link tool_rod).
Value GeneratedComponentWithSlider(std::optional<double> release) {
  auto declared = GeneratedComponent().as_object();
  for (auto &[name, value] : declared) {
    if (name == "collision_bodies") {
      auto bodies = value.as_object();
      bodies.emplace_back("rod", Value(Array{Value("tool_rod")}));
      value = Value(std::move(bodies));
    } else if (name == "spring_sliders") {
      Object slider{
          {"collision_body_role_id", Value("rod")},
          {"axis", Value(Array{Value(0.0), Value(0.0), Value(1.0)})},
          {"stroke_m", Value(0.02)},
          {"trip_depth_m", Value(0.0095)},
          {"max_compression_step_m", Value(0.002)},
      };
      if (release) slider.emplace_back("release_depth_m", Value(*release));
      value = Value(Array{Value(std::move(slider))});
    }
  }
  return Value(std::move(declared));
}

TEST(ActivationGeneratedTool, EachSpringSliderIsAFrameOfTheBodyTarget) {
  const auto tool = detail::parse_generated_tool_component(GeneratedComponentWithSlider(0.009));
  const auto slider = detail::generated_spring_slider(tool, "rod");
  ASSERT_TRUE(slider.has_value());
  EXPECT_EQ(slider->axis, (std::array<double, 3>{0.0, 0.0, 1.0}));
  EXPECT_EQ(slider->stroke_m, 0.02);
  EXPECT_EQ(slider->trip_depth_m, 0.0095);
  EXPECT_EQ(slider->release_depth_m, std::optional<double>(0.009));
  EXPECT_EQ(slider->max_compression_step_m, 0.002);
  EXPECT_FALSE(detail::generated_spring_slider(tool, "outer_pads").has_value());
  EXPECT_FALSE(detail::generated_spring_slider(
                   detail::parse_generated_tool_component(GeneratedComponentWithSlider(
                       std::nullopt)), "rod")->release_depth_m.has_value());
  // A frame of the `tool` target (not a TCP), bound to the slider link.
  const auto shape = detail::generated_tool_type_shape(tool);
  const auto &frames = shape.find_object_member("command_targets")->as_array().front()
                           .find_object_member("frame_roles")->as_array();
  ASSERT_EQ(frames.size(), 3U);
  EXPECT_EQ(frames[2].find_object_member("frame_role_id")->as_string(), "rod");
  EXPECT_EQ(frames[2].find_object_member("asset_frame_role_id")->as_string(), "rod");
  EXPECT_EQ(frames[2].find_object_member("tcp"), nullptr);
  // Its body carries its travel in the type, the retained declaration.
  const auto &bodies = shape.find_object_member("command_targets")->as_array().front()
                           .find_object_member("collision_body_roles")->as_array();
  const auto body = std::find_if(bodies.begin(), bodies.end(), [](const auto &row) {
    return row.find_object_member("collision_body_role_id")->as_string() == "rod";
  });
  ASSERT_NE(body, bodies.end());
  const auto *travel = body->find_object_member("spring_slider");
  ASSERT_NE(travel, nullptr);
  EXPECT_EQ(travel->find_object_member("stroke_m")->as_number(), 0.02);
  EXPECT_EQ(travel->find_object_member("release_depth_m")->as_number(), 0.009);
  EXPECT_EQ(std::count_if(bodies.begin(), bodies.end(), [](const auto &row) {
              return row.find_object_member("spring_slider") != nullptr;
            }),
            1);
  const auto semantics = detail::generated_tool_semantics(tool);
  const auto &bindings = semantics.find_object_member("frame_role_bindings")->as_array();
  const auto rod = std::find_if(bindings.begin(), bindings.end(), [](const auto &row) {
    return row.find_object_member("frame_role_id")->as_string() == "rod";
  });
  ASSERT_NE(rod, bindings.end());
  EXPECT_EQ(rod->find_object_member("source_link_name")->as_string(), "tool_rod");
}

// OD-357: the Deployment holds a generated tool's wiring only; the rest of
// each gripper row comes from its parameters.
TEST(ActivationGeneratedTool, WiringRowsBecomeCompleteGripperRows) {
  auto component = GeneratedTool("manyforge.tool", Array{Wiring("outer"), Wiring("inner")});
  detail::ActivationDeploymentIntent intent{};
  intent.components.push_back(component);
  const auto profiles = detail::resolve_activation_component_profiles(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
  detail::expand_generated_tool_wiring(intent, profiles);
  const auto &rows = intent.components.front().configuration
                         .find_object_member("runtime_configuration")
                         ->find_object_member("gripper_target_configurations")->as_array();
  ASSERT_EQ(rows.size(), 2U);
  const auto &outer = rows[0];  // the parameters' order
  EXPECT_EQ(outer.find_object_member("command_target_id")->as_string(), "outer");
  EXPECT_EQ(outer.find_object_member("open_state_ref")->as_string(), "open");
  EXPECT_EQ(outer.find_object_member("closed_state_ref")->as_string(), "closed");
  EXPECT_EQ(outer.find_object_member("command_duration_s")->as_number(), 0.5);
  EXPECT_EQ(outer.find_object_member("attach_frame")->as_string(), "tool_outer");
  EXPECT_EQ(outer.find_object_member("mount_frame")->as_string(), "tool_mount");
  EXPECT_EQ(outer.find_object_member("tcp_frame")->as_string(), "tool_tcp");
  EXPECT_EQ(outer.find_object_member("output_mode")->as_string(), "commanded");
  EXPECT_EQ(outer.find_object_member("actuation_kind")->as_string(), "pneumatic");
  EXPECT_EQ(*outer.find_object_member("pneumatic_io"),
            *Wiring("outer").find_object_member("pneumatic_io"));
  EXPECT_EQ(rows[1].find_object_member("tcp_frame")->as_string(), "tool_tcp_inner_tcp");
}

std::string WiringRefusal(Array rows) {
  auto component = GeneratedTool("manyforge.tool", std::move(rows));
  detail::ActivationDeploymentIntent intent{};
  intent.components.push_back(component);
  try {
    const auto profiles = detail::resolve_activation_component_profiles(
        std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
    detail::expand_generated_tool_wiring(intent, profiles);
  } catch (const c::TypedError &error) {
    return error.code();
  }
  return {};
}

TEST(ActivationGeneratedTool, LoadRefusesMissingOrForeignWiring) {
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer")}), "generated_tool_wiring_missing");
  EXPECT_EQ(WiringRefusal(Array{}), "generated_tool_wiring_missing");
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer"), Wiring("inner"), Wiring("side")}),
            "generated_tool_wiring_unknown");
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer"), Wiring("inner"), Wiring("inner")}),
            "generated_tool_wiring_unknown");
  auto repeated = Wiring("inner").as_object();
  repeated.emplace_back("open_state_ref", Value("open"));
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer"), Value(std::move(repeated))}),
            "generated_tool_wiring_invalid");
  auto unwired = Wiring("inner").as_object();
  std::erase_if(unwired, [](const auto &field) { return field.first == "pneumatic_io"; });
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer"), Value(std::move(unwired))}),
            "generated_tool_wiring_invalid");
  EXPECT_EQ(WiringRefusal(Array{Wiring("outer"), Wiring("inner")}), "");
}

TEST(ActivationGeneratedTool, OnlyTheToolFamilyTakesParameters) {
  EXPECT_EQ(RefusalCode(GeneratedTool("ufactory.lite_gripper", Array{Wiring("outer")})),
            "parametric_tool_kind_unsupported");
  EXPECT_EQ(RefusalCode(GeneratedTool("manyforge.passive_tool", Array{})),
            "parametric_tool_kind_unsupported");
  EXPECT_EQ(RefusalCode(Component("manyforge.tool", std::nullopt)),
            "generated_tool_parameters_required");
  // A passive tool from its URDF keeps its family type.
  const auto passive = Resolve(Component("manyforge.passive_tool", std::nullopt));
  EXPECT_FALSE(passive.front().generated_tool.has_value());
  EXPECT_FALSE(passive.front().derived_component_type_id.has_value());
}

TEST(ActivationComponentLane, UnknownModelIsRefused) {
  EXPECT_THROW((void)Resolve(Component("lite6_v0", "mock")), std::invalid_argument);
}

TEST(ActivationComponentLane, ModelFreeComponentRefusesAuthoredLane) {
  const Value device(Object{
      {"component_id", Value("station")},
      {"component_profile_id", Value("io_device_v0")},
      {"target_settings", Value(Object{})},
      {"lane", Value("mock")},
      {"configuration",
       Value(Object{{"runtime_kind", Value("io_provider")},
                    {"runtime_configuration", Value(Object{})}})}});
  const Value intent(Object{
      {"api_version", Value("manyforge.deployment_intent.v3")},
      {"collision_rules",
       Value(Array{Value(Object{{"clearance_m", Value(0.001)},
                   {"rule_id", Value("everything")}})})},
      {"attachments", Value(Array{})},
      {"components", Value(Array{device})},
      {"policies", Value(Object{})}});
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(intent, {}),
               std::invalid_argument);
}

} // namespace
