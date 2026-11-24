#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <manyforge_core/types/typed_error.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_io_provider_internal.hpp"

// A machine is an owner row with member rows.
// The intent parse drops deactivated members; the profile resolution checks
// the slots and gives members the owner's lane; I/O members become ordinary
// I/O device rows; the owner compiles to its `machines` row.
namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

Value Owner(std::string lane = "real", bool active = true) {
  Object configuration;
  if (lane == "real")
    configuration.emplace_back("device_address_ref", Value("lite6_main"));
  Object row{{"component_id", Value("lite6")},
             {"component_profile_id", Value("ufactory.lite6_machine")},
             {"lane", Value(std::move(lane))},
             {"configuration", Value(std::move(configuration))}};
  if (!active) row.emplace_back("active", Value(false));
  return Value(std::move(row));
}

Value IoMember(std::string id, std::string model, std::string role,
               std::optional<bool> active = std::nullopt,
               std::string owner = "lite6") {
  Object row{{"component_id", Value(std::move(id))},
             {"component_profile_id", Value(std::move(model))},
             {"owner", Value(std::move(owner))},
             {"role", Value(std::move(role))}};
  if (active) row.emplace_back("active", Value(*active));
  return Value(std::move(row));
}

// A model-free I/O device whose pneumatic-like configuration references
// `signal` of `component` (an active component using a member's signal).
Value SignalUser(std::string component, std::string signal) {
  return Value(Object{
      {"component_id", Value("station")},
      {"component_profile_id", Value("io_device_v0")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("io_provider")},
                    {"runtime_configuration",
                     Value(Object{{"uses",
                                   Value(Object{{"controllable_component_id",
                                                 Value(std::move(component))},
                                                {"signal_name",
                                                 Value(std::move(signal))}})}})}})}});
}

Value Intent(Array components) {
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v3")},
      {"collision_rules",
       Value(Array{Value(Object{{"clearance_m", Value(0.001)},
                   {"rule_id", Value("everything")}})})},
                      {"attachments", Value(Array{})},
                      {"components", Value(std::move(components))},
                      {"policies", Value(Object{})}});
}

// The arm member as the profile resolution sees it (no model source needed).
detail::ActivationDeploymentComponent Arm(std::string model = "ufactory.lite6") {
  detail::ActivationDeploymentComponent row{};
  row.component_id = "lite6_robot";
  row.component_profile_id = std::move(model);
  row.target_settings = Value(Object{});
  row.configuration = Value(Object{{"runtime_kind", Value("ros2_control")}});
  row.owner = "lite6";
  row.role = "arm";
  return row;
}

detail::ActivationDeploymentIntent Machine(std::string lane, bool with_arm = true) {
  auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{Owner(lane),
                   IoMember("lite6_robot_controller_io",
                            "ufactory.lite6_controller_io", "controller_io"),
                   IoMember("lite6_robot_tool_io", "ufactory.lite6_tool_io",
                            "tool_io", false)}),
      {});
  if (with_arm)
    intent.components.insert(intent.components.begin() + 1, Arm());
  else
    intent.inactive_members.push_back(
        {"lite6_robot", "ufactory.lite6", "lite6", "arm", "<robot name=\"lite6\"/>"});
  return intent;
}

std::vector<detail::ActivationComponentProfile>
Resolve(const detail::ActivationDeploymentIntent &intent) {
  return detail::resolve_activation_component_profiles(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
}

template <class Call> std::string Code(Call call) {
  try {
    call();
  } catch (const c::TypedError &error) {
    return error.code();
  }
  return {};
}

TEST(ActivationMachine, ParseKeepsOwnerAndActiveMembersAndDropsInactiveOnes) {
  const auto intent = Machine("real", false);
  ASSERT_EQ(intent.components.size(), 2U);
  const auto &owner = intent.components[0];
  EXPECT_EQ(owner.component_id, "lite6");
  EXPECT_EQ(owner.lane, std::optional<std::string>("real"));
  EXPECT_FALSE(owner.owner.has_value());
  EXPECT_EQ(owner.configuration.find_object_member("device_address_ref")->as_string(),
            "lite6_main");
  const auto &io = intent.components[1];
  EXPECT_EQ(io.owner, std::optional<std::string>("lite6"));
  EXPECT_EQ(io.role, std::optional<std::string>("controller_io"));
  EXPECT_FALSE(io.lane.has_value());
  ASSERT_EQ(intent.inactive_members.size(), 2U);  // tool I/O, then the arm
  EXPECT_EQ(intent.inactive_members.front().component_id, "lite6_robot_tool_io");
  EXPECT_EQ(intent.inactive_members.front().role, "tool_io");
}

TEST(ActivationMachine, ParseRefusesMissingOrInactiveOwnersAndInactiveSignals) {
  EXPECT_EQ(Code([] {
              (void)detail::parse_activation_deployment_intent(
                  Intent(Array{IoMember("io", "ufactory.lite6_controller_io",
                                        "controller_io", std::nullopt, "lite6")}),
                  {});
            }),
            "deployment_member_owner_missing");
  EXPECT_EQ(Code([] {
              (void)detail::parse_activation_deployment_intent(
                  Intent(Array{Owner("real", false),
                               IoMember("lite6_robot_controller_io",
                                        "ufactory.lite6_controller_io",
                                        "controller_io")}),
                  {});
            }),
            "deployment_member_owner_inactive");
  // An inactive owner with inactive members is left out entirely.
  const auto dropped = detail::parse_activation_deployment_intent(
      Intent(Array{Owner("real", false),
                   IoMember("lite6_robot_controller_io",
                            "ufactory.lite6_controller_io", "controller_io",
                            false)}),
      {});
  EXPECT_TRUE(dropped.components.empty());
  EXPECT_EQ(Code([] {
              (void)detail::parse_activation_deployment_intent(
                  Intent(Array{Owner(),
                               IoMember("lite6_robot_tool_io",
                                        "ufactory.lite6_tool_io", "tool_io",
                                        false),
                               SignalUser("lite6_robot_tool_io", "TO0")}),
                  {});
            }),
            "deployment_signal_member_inactive");
}

TEST(ActivationMachine, MembersTakeTheOwnersLaneAndAddress) {
  const auto intent = Machine("real");
  const auto profiles = Resolve(intent);
  ASSERT_EQ(profiles.size(), 3U);
  const auto &owner = profiles[0];
  EXPECT_EQ(owner.runtime_kind, detail::ActivationRuntimeKind::kMachine);
  ASSERT_TRUE(owner.lane_row.has_value());
  EXPECT_EQ(owner.lane_row->find_object_member("adapter")->as_string(), "xarm");
  const auto &arm = profiles[1];
  EXPECT_EQ(arm.runtime_kind, detail::ActivationRuntimeKind::kRos2Control);
  EXPECT_EQ(arm.lane, std::optional<std::string>("real"));
  EXPECT_EQ(arm.owner_component_id, std::optional<std::string>("lite6"));
  EXPECT_EQ(arm.member_role, std::optional<std::string>("arm"));
  ASSERT_TRUE(arm.owner_device_address_ref.has_value());
  EXPECT_EQ(arm.owner_device_address_ref->as_string(), "lite6_main");
  const auto &io = profiles[2];
  EXPECT_EQ(io.runtime_kind, detail::ActivationRuntimeKind::kIoProvider);
  EXPECT_EQ(io.lane_row->find_object_member("bank")->as_string(), "controller");
  // On Mock the machine model has no row: the host's passive owner.
  const auto mock = Resolve(Machine("mock"));
  EXPECT_FALSE(mock[0].lane_row.has_value());
  EXPECT_EQ(mock[1].lane, std::optional<std::string>("mock"));
  EXPECT_FALSE(mock[1].owner_device_address_ref.has_value());
}

TEST(ActivationMachine, SlotChecksRefuseUnknownAndRepeatedRoles) {
  auto unknown = Machine("mock");
  unknown.components[2].role = "camera";
  EXPECT_EQ(Code([&] { (void)Resolve(unknown); }), "deployment_member_role_unknown");
  auto wrong_model = Machine("mock");
  wrong_model.components[1] = Arm("ufactory.uf850");
  EXPECT_EQ(Code([&] { (void)Resolve(wrong_model); }),
            "deployment_member_role_unknown");
  auto repeated = Machine("mock");
  repeated.inactive_members.push_back(
      {"other_io", "ufactory.lite6_controller_io", "lite6", "controller_io"});
  EXPECT_EQ(Code([&] { (void)Resolve(repeated); }),
            "deployment_member_role_repeated");
}

// Any member may be deactivated on every lane, the arm
// included; a member slot without a member (removed) is refused.
TEST(ActivationMachine, AnyMemberMayBeDeactivatedButNoneRemoved) {
  for (const std::string lane : {"real", "mock", "isaac"}) {
    const auto armless = Resolve(Machine(lane, false));
    ASSERT_EQ(armless.size(), 2U) << lane;
    EXPECT_EQ(armless[1].member_role, std::optional<std::string>("controller_io"));
    auto absent_io = Machine(lane);
    absent_io.inactive_members.clear();  // the tool I/O member removed
    EXPECT_EQ(Code([&] { (void)Resolve(absent_io); }), "deployment_member_missing")
        << lane;
  }
}

// With its arm deactivated, the owner runs the machine's
// process itself where the machine has one (real): its row carries the arm's
// lane row, description and joints. On Mock and Isaac (no machine row) the
// owner is passive and carries none.
TEST(ActivationMachine, AnOwnerWithItsArmDeactivatedCarriesTheArmForItsProcess) {
  for (const std::string lane : {"real", "mock", "isaac"}) {
    auto intent = Machine(lane, false);
    const auto profiles = Resolve(intent);
    detail::expand_activation_machine_members(intent, profiles);
    const auto machines = detail::prepare_activation_machine_types(intent, profiles);
    ASSERT_EQ(machines.size(), 1U) << lane;
    const auto &row =
        machines.front().normalized_configuration.find_object_member("machines")
            ->as_array()
            .front();
    if (lane != "real") {
      EXPECT_FALSE(profiles[0].deactivated_arm.has_value()) << lane;
      EXPECT_EQ(row.find_object_member("arm_lane_row"), nullptr) << lane;
      EXPECT_EQ(row.find_object_member("arm_model_family_id"), nullptr) << lane;
      EXPECT_EQ(row.find_object_member("arm_description"), nullptr) << lane;
      EXPECT_EQ(row.find_object_member("arm_joint_names"), nullptr) << lane;
      continue;
    }
    ASSERT_TRUE(profiles[0].deactivated_arm.has_value());
    const auto &arm = *profiles[0].deactivated_arm;
    EXPECT_EQ(arm.lane_row.find_object_member("adapter")->as_string(), "xarm");
    EXPECT_EQ(arm.lane_row.find_object_member("vendor_model")->as_string(), "lite6");
    EXPECT_EQ(arm.model_family_id, "lite6");
    EXPECT_EQ(arm.description, "<robot name=\"lite6\"/>");
    EXPECT_EQ(arm.joint_names, (std::vector<std::string>{"joint1", "joint2", "joint3",
                                                         "joint4", "joint5", "joint6"}));
    EXPECT_EQ(row.find_object_member("arm_description")->as_string(), arm.description);
    EXPECT_EQ(row.find_object_member("arm_model_family_id")->as_string(), "lite6");
    EXPECT_EQ(row.find_object_member("arm_joint_names")->as_array().size(), 6U);
    EXPECT_NE(row.find_object_member("arm_lane_row")->as_string().find("\"robot_type\""),
              std::string::npos);
  }
  // With the arm active, its own composition runs the process.
  EXPECT_FALSE(Resolve(Machine("real"))[0].deactivated_arm.has_value());
}

// A machine's member model is never placed without its machine, on
// any lane.
TEST(ActivationMachine, AMachinePartWithoutItsMachineIsRefused) {
  for (const std::string lane : {"real", "mock", "isaac"}) {
    detail::ActivationDeploymentIntent intent{};
    auto arm = Arm();
    arm.owner.reset();
    arm.role.reset();
    arm.lane = lane;
    intent.components.push_back(std::move(arm));
    EXPECT_EQ(Code([&] { (void)Resolve(intent); }),
              "deployment_machine_part_without_machine")
        << lane;
  }
}

TEST(ActivationMachine, IoMembersExpandToOrdinaryIoDeviceRowsPerLane) {
  for (const std::string lane : {"real", "mock", "isaac"}) {
    auto intent = Machine(lane);
    const auto profiles = Resolve(intent);
    detail::expand_activation_machine_members(intent, profiles);
    const auto &configuration = intent.components[2].configuration;
    EXPECT_EQ(configuration.find_object_member("runtime_kind")->as_string(),
              "io_provider");
    const auto &runtime = *configuration.find_object_member("runtime_configuration");
    const auto &devices = runtime.find_object_member("io_provider_devices")->as_array();
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices.front().find_object_member("owner_component_id")->as_string(),
              "lite6");
    EXPECT_EQ(devices.front().find_object_member("member_role")->as_string(),
              "controller_io");
    const auto &signals = runtime.find_object_member("io_signals")->as_array();
    ASSERT_EQ(signals.size(), 16U);  // CI0-CI7, CO0-CO7
    for (const auto &row : signals) {
      const auto &name = row.find_object_member("signal_name")->as_string();
      const bool output = row.find_object_member("direction")->as_string() == "output";
      EXPECT_EQ(row.find_object_member("command_target_id")->as_string(), "io");
      EXPECT_EQ(row.find_object_member("resource_role_id")->as_string(), name);
      EXPECT_EQ(row.find_object_member("source_id")->as_string(),
                "lite6_robot_controller_io." + name);
      const auto &provider = *row.find_object_member("provider");
      const auto &profile = provider.find_object_member("profile")->as_string();
      if (lane == "real") {
        EXPECT_EQ(profile, "component_adapter");
        EXPECT_EQ(provider.find_object_member("bank")->as_string(), "controller");
        // Periodic stream inputs: the model's sample-age budget (100 ms).
        if (!output) {
          const auto &freshness = *row.find_object_member("freshness");
          EXPECT_EQ(row.find_object_member("update_mode")->as_string(), "sampled");
          EXPECT_EQ(freshness.find_object_member("mode")->as_string(), "sample_age");
          EXPECT_EQ(freshness.find_object_member("maximum_age_ns")->as_number(), 1e8);
        }
      } else {
        EXPECT_EQ(profile, output ? "mock_output" : "mock");
      }
    }
    // The owner's compiled row.
    const auto machines = detail::prepare_activation_machine_types(intent, profiles);
    ASSERT_EQ(machines.size(), 1U);
    const auto &row =
        machines.front().normalized_configuration.find_object_member("machines")
            ->as_array()
            .front();
    EXPECT_EQ(row.find_object_member("controllable_component_id")->as_string(), "lite6");
    EXPECT_EQ(row.find_object_member("lane")->as_string(), lane);
    EXPECT_EQ(row.find_object_member("machine_lane_row") != nullptr, lane == "real");
    EXPECT_EQ(row.find_object_member("device_address_ref") != nullptr, lane == "real");
    EXPECT_TRUE(machines.front().type.value().command_targets.empty());
  }
}

// OD-356: an I/O member row may carry `mock_inputs`; the parse keeps them
// for the member's signal expansion.
TEST(ActivationMachine, AnIoMemberRowKeepsItsMockInputs) {
  auto member = IoMember("lite6_robot_controller_io", "ufactory.lite6_controller_io",
                         "controller_io");
  auto row = member.as_object();
  row.emplace_back("mock_inputs", Value(Array{}));
  const auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{Owner("mock"), Value(std::move(row))}), {});
  ASSERT_EQ(intent.components.size(), 2U);
  ASSERT_NE(intent.components[1].configuration.find_object_member("mock_inputs"), nullptr);
}

// OD-356: an I/O member's input follows geometry on Mock and Isaac (a
// geometric-contact provider); on Real the physical input is read.
TEST(ActivationMachine, AMockInputFromGeometryBecomesAGeometricContactInput) {
  const auto selection = [](const char *kind, const char *key, const char *id) {
    return Value(Object{{"kind", Value(kind)}, {key, Value(id)}});
  };
  const auto behaviour = Value(Array{Value(Object{
      {"signal_name", Value("CI0")},
      {"geometric_contact",
       Value(Object{{"robot_component_id", Value("lite6_robot")},
                    {"a", Value(Array{selection("attached_object", "component_id", "lite_gripper")})},
                    {"b", Value(Array{selection("scene_object", "scene_resource_id", "endplate")})},
                    {"trip_depth_m", Value(0.004)},
                    {"release_depth_m", Value(0.003)}})}})});
  for (const std::string lane : {"real", "mock", "isaac"}) {
    auto intent = Machine(lane);
    intent.components[2].configuration = Value(Object{{"mock_inputs", behaviour}});
    const auto profiles = Resolve(intent);
    detail::expand_activation_machine_members(intent, profiles);
    const auto &signals = intent.components[2]
                              .configuration.find_object_member("runtime_configuration")
                              ->find_object_member("io_signals")
                              ->as_array();
    for (const auto &row : signals) {
      const auto &name = row.find_object_member("signal_name")->as_string();
      const auto &provider = *row.find_object_member("provider");
      const auto &profile = provider.find_object_member("profile")->as_string();
      if (name != "CI0") {
        EXPECT_NE(profile, "geometric_contact") << name;
        continue;
      }
      if (lane == "real") {
        EXPECT_EQ(profile, "component_adapter");
        continue;
      }
      EXPECT_EQ(profile, "geometric_contact") << lane;
      EXPECT_EQ(provider.find_object_member("robot_component_id")->as_string(), "lite6_robot");
      EXPECT_EQ(provider.find_object_member("trip_depth_m")->as_number(), 0.004);
      EXPECT_EQ(provider.find_object_member("release_depth_m")->as_number(), 0.003);
      EXPECT_EQ(provider.find_object_member("a")->as_array().size(), 1U);
      EXPECT_EQ(row.find_object_member("update_mode")->as_string(), "sampled");
      const auto &freshness = *row.find_object_member("freshness");
      EXPECT_EQ(freshness.find_object_member("mode")->as_string(), "sample_age");
      EXPECT_EQ(freshness.find_object_member("maximum_age_ns")->as_number(), 1e9);
    }
  }

  // An output, an unknown input, or a missing selection is refused.
  for (const auto &bad : {Value(Array{Value(Object{{"signal_name", Value("CO0")},
                                                   {"geometric_contact", Value(Object{})}})}),
                          Value(Array{Value(Object{{"signal_name", Value("TI9")},
                                                   {"geometric_contact", Value(Object{})}})}),
                          Value(Array{Value(Object{
                              {"signal_name", Value("CI1")},
                              {"geometric_contact",
                               Value(Object{{"robot_component_id", Value("lite6_robot")}})}})})}) {
    auto intent = Machine("mock");
    intent.components[2].configuration = Value(Object{{"mock_inputs", bad}});
    const auto profiles = Resolve(intent);
    EXPECT_EQ(Code([&] { detail::expand_activation_machine_members(intent, profiles); }),
              "io_mock_input_invalid");
  }
}

// OD-371: an input that follows a generated tool's spring slider compiles to
// the geometric-contact provider: the slider body and the objects it carries
// against `b`, on the robot the tool is mounted on, with the slider's trip
// and release depths.
TEST(ActivationMachine, AMockInputFollowsASpringSlider) {
  const auto mounted = [](detail::ActivationDeploymentIntent &intent,
                          std::vector<detail::ActivationComponentProfile> &profiles) {
    detail::ActivationDeploymentComponent tool{};
    tool.component_id = "tube_holder";
    tool.component_profile_id = "manyforge.tool";
    tool.target_settings = Value(Object{});
    tool.configuration = Value(Object{});
    intent.components.push_back(std::move(tool));
    detail::ActivationComponentProfile profile{};
    profile.component_id = "tube_holder";
    profile.runtime_kind = detail::ActivationRuntimeKind::kRos2Control;
    detail::GeneratedToolComponent generated;
    generated.collision_bodies["holder_rod"] = {"tool_holder_rod"};
    generated.spring_sliders.push_back({.element_id = "holder_rod",
                                        .axis = {0.0, 0.0, 1.0},
                                        .stroke_m = 0.020,
                                        .trip_depth_m = 0.0095,
                                        .release_depth_m = 0.009,
                                        .max_compression_step_m = 0.002});
    profile.generated_tool = std::move(generated);
    profiles.push_back(std::move(profile));
    intent.attachments = Value(Array{Value(Object{
        {"child", Value(Object{{"component_id", Value("tube_holder")},
                               {"interface", Value("mount")}})},
        {"parent", Value(Object{{"component_id", Value("lite6_robot")},
                                {"interface", Value("flange")}})}})});
  };
  const auto endplate = Value(Array{Value(Object{{"kind", Value("scene_object")},
                                                 {"scene_resource_id", Value("endplate")}})});
  const auto row = [&](const char *tool, const char *element) {
    return Value(Object{
        {"signal_name", Value("CI0")},
        {"spring_slider",
         Value(Object{{"component_id", Value(tool)}, {"element_id", Value(element)}})},
        {"b", endplate},
        {"maximum_sample_age_ms", Value(500.0)}});
  };

  auto intent = Machine("mock");
  intent.components[2].configuration =
      Value(Object{{"mock_inputs", Value(Array{row("tube_holder", "holder_rod")})}});
  auto profiles = Resolve(intent);
  mounted(intent, profiles);
  detail::expand_activation_machine_members(intent, profiles);
  const auto &signals = intent.components[2]
                            .configuration.find_object_member("runtime_configuration")
                            ->find_object_member("io_signals")
                            ->as_array();
  const auto input = std::find_if(signals.begin(), signals.end(), [](const Value &signal) {
    return signal.find_object_member("signal_name")->as_string() == "CI0";
  });
  ASSERT_NE(input, signals.end());
  const auto &provider = *input->find_object_member("provider");
  EXPECT_EQ(provider.find_object_member("profile")->as_string(), "geometric_contact");
  EXPECT_EQ(provider.find_object_member("robot_component_id")->as_string(), "lite6_robot");
  EXPECT_EQ(provider.find_object_member("trip_depth_m")->as_number(), 0.0095);
  EXPECT_EQ(provider.find_object_member("release_depth_m")->as_number(), 0.009);
  EXPECT_EQ(*provider.find_object_member("b"), endplate);
  EXPECT_EQ(*provider.find_object_member("a"),
            Value(Array{Value(Object{{"kind", Value("collision_body")},
                                     {"component_id", Value("tube_holder")},
                                     {"collision_body_id", Value("holder_rod")}}),
                        Value(Object{{"kind", Value("attached_object")},
                                     {"component_id", Value("tube_holder")},
                                     {"collision_body_role_id", Value("holder_rod")}})}));
  EXPECT_EQ(input->find_object_member("freshness")
                ->find_object_member("maximum_age_ns")
                ->as_number(),
            5e8);

  // An unknown component, a component that is not a generated tool, an
  // element that is not a spring slider, and an unmounted tool are refused.
  for (const auto &[tool, element, reason, mount] :
       {std::tuple{"no_tool", "holder_rod", "tool_unknown", true},
        std::tuple{"lite6_robot", "holder_rod", "tool_not_generated", true},
        std::tuple{"tube_holder", "body", "element_not_spring_slider", true},
        std::tuple{"tube_holder", "holder_rod", "tool_not_mounted", false}}) {
    auto refused = Machine("mock");
    refused.components[2].configuration =
        Value(Object{{"mock_inputs", Value(Array{row(tool, element)})}});
    auto refused_profiles = Resolve(refused);
    mounted(refused, refused_profiles);
    if (!mount) refused.attachments = Value(Array{});
    try {
      detail::expand_activation_machine_members(refused, refused_profiles);
      ADD_FAILURE() << reason;
    } catch (const c::TypedError &error) {
      EXPECT_EQ(error.code(), "io_mock_input_invalid");
      EXPECT_EQ(error.details().at("reason"), reason);
      EXPECT_EQ(error.details().at("tool_component_id"), tool);
      EXPECT_EQ(error.details().at("element_id"), element);
    }
  }

  // Exactly one form per row.
  auto both = row("tube_holder", "holder_rod").as_object();
  both.emplace_back("geometric_contact", Value(Object{}));
  auto refused = Machine("mock");
  refused.components[2].configuration =
      Value(Object{{"mock_inputs", Value(Array{Value(std::move(both))})}});
  auto refused_profiles = Resolve(refused);
  mounted(refused, refused_profiles);
  EXPECT_EQ(Code([&] { detail::expand_activation_machine_members(refused, refused_profiles); }),
            "io_mock_input_invalid");
}

// An owner-less model-form I/O device (OD-327): a row of its own with its
// lane, no owner or role; real signals carry its family's lane keys and each
// signal's index; Mock and Isaac get the host's Mock I/O.
TEST(ActivationMachine, AnOwnerLessIoBankExpandsWithItsFamilysKeysAndIndexes) {
  for (const std::string lane : {"real", "mock", "isaac"}) {
    auto intent = detail::parse_activation_deployment_intent(
        Intent(Array{Value(Object{{"component_id", Value("plc_io")},
                                  {"component_profile_id", Value("manyforge.io_bank_64x64")},
                                  {"lane", Value(lane)},
                                  {"configuration", Value(Object{})}})}),
        {});
    const auto profiles = Resolve(intent);
    ASSERT_EQ(profiles.size(), 1U);
    EXPECT_FALSE(profiles.front().owner_component_id.has_value());
    detail::expand_activation_machine_members(intent, profiles);
    const auto &runtime =
        *intent.components.front().configuration.find_object_member("runtime_configuration");
    const auto &devices = runtime.find_object_member("io_provider_devices")->as_array();
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices.front().as_object().size(), 1U);  // its id only
    const auto &signals = runtime.find_object_member("io_signals")->as_array();
    ASSERT_EQ(signals.size(), 128U);  // I0-I63, Q0-Q63
    for (const auto &row : signals) {
      const auto &name = row.find_object_member("signal_name")->as_string();
      const bool output = row.find_object_member("direction")->as_string() == "output";
      const auto &provider = *row.find_object_member("provider");
      const auto &profile = provider.find_object_member("profile")->as_string();
      if (lane != "real") {
        EXPECT_EQ(profile, output ? "mock_output" : "mock");
        continue;
      }
      EXPECT_EQ(profile, "component_adapter");
      EXPECT_EQ(provider.find_object_member("adapter")->as_string(), "ros_io_bank");
      EXPECT_EQ(provider.find_object_member("rate_hz")->as_number(), 200.0);
      EXPECT_EQ(provider.find_object_member("index")->as_number(),
                std::stod(name.substr(1)));
      EXPECT_EQ(provider.find_object_member("input_maximum_age_ms"), nullptr);
      if (!output) {
        EXPECT_EQ(row.find_object_member("freshness")
                      ->find_object_member("maximum_age_ns")
                      ->as_number(),
                  5e7);
      }
    }
  }
}

TEST(ActivationMachine, AMachinesIoBankIsNotAnOwnerLessDevice) {
  auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{Value(Object{{"component_id", Value("io")},
                                {"component_profile_id", Value("ufactory.lite6_controller_io")},
                                {"lane", Value("mock")},
                                {"configuration", Value(Object{})}})}),
      {});
  EXPECT_EQ(Code([&] { (void)Resolve(intent); }), "deployment_machine_part_without_machine");
}

} // namespace
