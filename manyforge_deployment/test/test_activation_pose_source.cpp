#include <gtest/gtest.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_deployment/io_provider_domain.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_prepared_model_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
namespace contract = manyforge::deployment::ros2_control_execution_runtime_contract;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

Value Binding(std::string object_id, std::string policy_id, bool eligible) {
  return Value(Object{
      {"eligible", Value(eligible)},
      {"entity", Value("/World/" + object_id)},
      {"frame_kind", Value("cell_frame")},
      {"lifetime_ns", Value(2000000000.0)},
      {"local_orientation_xyzw",
       Value(Array{Value(0.0), Value(0.0), Value(0.0), Value(1.0)})},
      {"local_position_xyz", Value(Array{Value(0.0), Value(0.0), Value(0.0)})},
      {"object_id", Value(std::move(object_id))},
      {"policy_id", Value(std::move(policy_id))},
      {"source_coordinate_frame_id", Value("universe")},
      {"source_header_frame_id", Value("box")},
      {"target_frame_id", Value("universe")}});
}

Value Joint() {
  return Value(Object{{"eligible", Value(false)},
                      {"lifetime_ns", Value(1000000000.0)},
                      {"object_id", Value("robot")},
                      {"policy_id", Value("viewer.measured_robot")},
                      {"required_joint_names",
                       Value(Array{Value("joint1"), Value("joint2")})},
                      {"topic", Value("/isaac_joint_states")}});
}

Value PoseSource(std::string id, Array entities,
                 std::optional<Value> joint = Joint(),
                 std::optional<std::string> row_owner = std::nullopt) {
  Object row{{"controllable_component_id", Value(row_owner.value_or(id))},
             {"simulation_poll_period_ms", Value(250.0)},
             {"simulation_entities", Value(std::move(entities))},
             {"detection3d_sources", Value(Array{})}};
  if (joint)
    row.emplace_back("joint_state", std::move(*joint));
  return Value(Object{
      {"component_id", Value(id)},
      {"component_profile_id", Value("pose_source_v0")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("pose_source")},
                    {"runtime_configuration",
                     Value(Object{{"pose_sources",
                                   Value(Array{Value(std::move(row))})}})}})}});
}

Value Intent(Array components) {
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v2")},
                      {"deployment_artifact_id", Value("authored_pose")},
                      {"name", Value("authored_pose")},
                      {"compatibility", Value(Object{
                          {"lane", Value("manyforge-only")},
                          {"provides", Value(Array{})},
                          {"tags", Value(Array{})}})},
                      {"attachments", Value(Array{})},
                      {"components", Value(std::move(components))},
                      {"policies", Value(Object{})}});
}

detail::PreparedActivationMaterialization Materialize(const Value &authored) {
  const auto intent = detail::parse_activation_deployment_intent(authored, {});
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  return detail::materialize_prepared_activation(
      root, {}, intent, c::CellId("authored_pose_cell"), profiles, false);
}

std::string Refusal(const Value &authored) {
  try {
    (void)Materialize(authored);
  } catch (const std::exception &error) {
    return error.what();
  }
  return {};
}

TEST(ActivationPoseSource, ModelFreeComponentCompilesToOneProviderDomain) {
  const auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{PoseSource(
          "scene_objects",
          Array{Binding("robot", "viewer.measured_tcp", false),
                Binding("cheese_box", "continuous_unattached", true)})}),
      {});
  ASSERT_EQ(intent.components.size(), 1U);
  EXPECT_FALSE(intent.components.front().model_source);
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  ASSERT_EQ(profiles.size(), 1U);
  EXPECT_EQ(profiles.front().runtime_kind,
            detail::ActivationRuntimeKind::kPoseSource);
  EXPECT_TRUE(
      detail::qualify_activation_prepared_models(intent, profiles, {}).empty());
  const auto materialized = detail::materialize_prepared_activation(
      root, {}, intent, c::CellId("authored_pose_cell"), profiles, false);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  EXPECT_TRUE(d::verify_retained_bundle_identity(bundle));
  const auto &graph = bundle.value().cell_graph;
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  EXPECT_TRUE(graph.controllable_components.front()
                  .component_type_id.value()
                  .starts_with("manyforge.pose_source."));
  ASSERT_EQ(graph.control_domains.size(), 1U);
  const auto *provider = graph.control_domains.front().provider();
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(provider->provider_manifest_id.value(),
            c::kPoseSourceLifecycleManifestId);
  EXPECT_EQ(provider->controllable_component_id.value(), "scene_objects");
  EXPECT_EQ(provider->lifecycle_command_timeout_ns, 30'000'000'000LL);
  EXPECT_EQ(provider->signal_names,
            (std::vector<std::string>{
                "poses/cheese_box/continuous_unattached",
                "poses/robot/viewer.measured_robot",
                "poses/robot/viewer.measured_tcp"}));
  EXPECT_TRUE(graph.control_domains.front()
                  .control_domain_id.value()
                  .starts_with("pose_source.provider."));
  // One scene.pose.observe target binding plus the five lifecycle selectors.
  const auto &providers = bundle.value().capability_provider_bindings;
  EXPECT_EQ(std::count_if(providers.begin(), providers.end(),
                          [](const auto &row) {
                            return row.capability_id.value() ==
                                   "scene.pose.observe";
                          }),
            1);
  EXPECT_EQ(std::count_if(providers.begin(), providers.end(),
                          [](const auto &row) {
                            return row.provider_manifest_id.value() ==
                                   c::kPoseSourceLifecycleManifestId;
                          }),
            6);

  const auto configuration = contract::require_configuration(
      bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.pose_sources.size(), 1U);
  const auto &source = configuration.pose_sources.front();
  EXPECT_EQ(source.controllable_component_id.value(), "scene_objects");
  EXPECT_EQ(source.observer_configuration.find_object_member(
                "controllable_component_id"),
            nullptr);
  ASSERT_NE(source.observer_configuration.find_object_member(
                "simulation_poll_period_ms"),
            nullptr);
  ASSERT_TRUE(source.joint_state.has_value());
  EXPECT_EQ(source.joint_state->topic, "/isaac_joint_states");
  ASSERT_EQ(source.bindings.size(), 3U);
  // Observer order: joint_state, simulation entities, Detection3D sources.
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[0]),
            "poses/robot/viewer.measured_robot");
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[1]),
            "poses/robot/viewer.measured_tcp");
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[2]),
            "poses/cheese_box/continuous_unattached");
  EXPECT_TRUE(source.bindings[2].eligible);
  EXPECT_EQ(source.bindings[2].lifetime_ns, 2'000'000'000LL);
  EXPECT_EQ(contract::find_pose_source(
                configuration, c::ControllableComponentId("scene_objects")),
            &source);
  EXPECT_EQ(contract::find_pose_source(configuration,
                                       c::ControllableComponentId("other")),
            nullptr);
}

TEST(ActivationPoseSource, AbsentJointStateDecodesAsNone) {
  const auto materialized = Materialize(Intent(Array{PoseSource(
      "scene_objects", Array{Binding("box", "continuous_unattached", true)},
      std::nullopt)}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  const auto configuration = contract::require_configuration(
      *materialized.bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.pose_sources.size(), 1U);
  EXPECT_FALSE(configuration.pose_sources.front().joint_state.has_value());
  EXPECT_EQ(configuration.pose_sources.front().bindings.size(), 1U);
}

TEST(ActivationPoseSource, InvalidBindingsAndForeignRowsAreRefused) {
  const auto refused = [](const Value &authored, std::string_view reason) {
    const auto message = Refusal(authored);
    EXPECT_NE(message.find(reason), std::string::npos) << message;
  };
  refused(Intent(Array{PoseSource(
              "scene_objects", Array{Binding("Box", "continuous", true)})}),
          "pose_source_binding_id_invalid");
  refused(Intent(Array{PoseSource(
              "scene_objects",
              Array{Binding(std::string(40, 'a'), std::string(30, 'b'),
                            true)})}),
          "pose_source_binding_id_invalid");
  refused(Intent(Array{PoseSource(
              "scene_objects", Array{Binding("box", "continuous", true),
                                     Binding("box", "continuous", false)})}),
          "pose_source_binding_duplicate");
  refused(Intent(Array{PoseSource("scene_objects", Array{}, std::nullopt)}),
          "pose_source_bindings_empty");
  refused(Intent(Array{PoseSource("scene_objects",
                                  Array{Binding("box", "continuous", true)},
                                  Joint(), "other_component")}),
          "does not name its actual component");
  // A pose source needs its exact model-free profile.
  auto wrong_profile = PoseSource("scene_objects",
                                  Array{Binding("box", "continuous", true)});
  auto object = wrong_profile.as_object();
  for (auto &[name, value] : object)
    if (name == "component_profile_id")
      value = Value("io_device_v0");
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(
                   Intent(Array{Value(std::move(object))}), {}),
               std::invalid_argument);
}
} // namespace
