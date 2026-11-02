#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include <manyforge_core/types/typed_error.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"

// `component_profile_id` names a component model (its descriptor `id`);
// `components[].lane` picks one of the model's `lanes` rows. Model-free
// components and lane-free (pneumatic) tools name none.
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
      {"api_version", Value("manyforge.deployment_intent.v2")},
      {"deployment_artifact_id", Value("lane_io")},
      {"name", Value("lane_io")},
      {"attachments", Value(Array{})},
      {"components", Value(Array{device})},
      {"policies", Value(Object{})}});
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(intent, {}),
               std::invalid_argument);
}

} // namespace
