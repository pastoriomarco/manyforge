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

Value Numbers(std::initializer_list<double> values) {
  Array result;
  for (const auto value : values)
    result.emplace_back(value);
  return Value(std::move(result));
}

Value Simulation(bool relative = false) {
  Object simulation{{"poll_period_ms", Value(250.0)},
                    {"source_coordinate_frame_id", Value("universe")},
                    {"target_frame_id", Value("universe")}};
  if (relative)
    simulation.emplace_back(
        "relative_reference",
        Value(Object{{"entity", Value("/World/robot/tool_base")},
                     {"link", Value("tool_link")},
                     {"source_header_frame_id", Value("gripper_frame")}}));
  return Value(std::move(simulation));
}

Value Detection() {
  return Value(Object{{"topic", Value("/output")},
                      {"target_frame_id", Value("link_base")},
                      {"fallback_frame_id", Value("link_base")}});
}

Value Overlay(bool tcp = true) {
  Object overlay{
      {"robot_component_id", Value("robot")},
      {"joint_state",
       Value(Object{{"topic", Value("/isaac_joint_states")},
                    {"required_joint_names",
                     Value(Array{Value("joint1"), Value("joint2")})}})}};
  if (tcp)
    overlay.emplace_back(
        "tcp_entity",
        Value(Object{{"entity", Value("/World/robot/tool_base")},
                     {"source_header_frame_id", Value("tcp_frame")},
                     {"local_position_xyz", Numbers({0.0, 0.0, 0.2})}}));
  return Value(std::move(overlay));
}

/// Authored `pose_source.v2` component; `fields` are the optional blocks.
Value PoseSource(std::string id, Object fields) {
  fields.emplace_back("stream_budget_ns", Value(1000000000.0));
  return Value(Object{
      {"component_id", Value(id)},
      {"component_profile_id", Value("pose_source_v0")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("pose_source")},
                    {"runtime_configuration", Value(std::move(fields))}})}});
}

Value EntityLink(std::string object_id, std::string policy,
                 std::string source = "scene_objects") {
  return Value(Object{
      {"scene_resource_id", Value(object_id)},
      {"source_id", Value(std::move(source))},
      {"source_key",
       Value(Object{{"simulation_entity",
                     Value(Object{{"entity", Value("/World/" + object_id)},
                                  {"source_header_frame_id",
                                   Value(object_id + "_frame")}})}})},
      {"apply_policy", Value(std::move(policy))}});
}

Value DetectionLink(std::string object_id, std::string policy) {
  return Value(Object{
      {"scene_resource_id", Value(object_id)},
      {"source_id", Value("scene_objects")},
      {"source_key",
       Value(Object{{"detection3d",
                     Value(Object{{"class_id", Value("part")},
                                  {"detection_id", Value("")},
                                  {"confidence_min", Value(0.5)},
                                  {"max_position_variance_m2", Value(0.0025)},
                                  {"selection", Value("first")}})}})},
      {"apply_policy", Value(std::move(policy))}});
}

Value Intent(Array components, Array links = {}, Array streams = {}) {
  Object policies;
  if (!links.empty() || !streams.empty())
    policies.emplace_back(
        "interrupt_config",
        Value(Object{{"cancel_validation_timeout_ns", Value(1000.0)},
                     {"motion_scope_arm_timeout_ns", Value(1000.0)},
                     {"queue_depth", Value(8.0)},
                     {"supervisory_poll_rate_hz", Value(10.0)}}));
  if (!links.empty())
    policies.emplace_back("scene_pose_sources", Value(std::move(links)));
  if (!streams.empty())
    policies.emplace_back("component_streams", Value(std::move(streams)));
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v2")},
                      {"deployment_artifact_id", Value("authored_pose")},
                      {"name", Value("authored_pose")},
                      {"compatibility", Value(Object{
                          {"lane", Value("manyforge-only")},
                          {"provides", Value(Array{})},
                          {"tags", Value(Array{})}})},
                      {"attachments", Value(Array{})},
                      {"components", Value(std::move(components))},
                      {"policies", Value(std::move(policies))}});
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
  // A sim link, the robot overlay (joints + TCP) and a budget override of the
  // object's stream join into today's expanded unit row.
  const auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{PoseSource("scene_objects",
                              Object{{"simulation", Simulation()},
                                     {"robot_overlay", Overlay()}})},
             Array{EntityLink("cheese_box", "continuous_unattached")},
             Array{Value(Object{
                 {"controllable_component_id", Value("scene_objects")},
                 {"stream_id", Value("poses/cheese_box")},
                 {"budget_ns", Value(2000000000.0)}})}),
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
            (std::vector<std::string>{"poses/cheese_box",
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
  // The link itself is retained as the object's policy row.
  ASSERT_EQ(bundle.value().scene_pose_sources.size(), 1U);
  EXPECT_EQ(bundle.value().scene_pose_sources.front().apply_policy,
            "continuous_unattached");

  const auto configuration = contract::require_configuration(
      bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.pose_sources.size(), 1U);
  const auto &source = configuration.pose_sources.front();
  EXPECT_EQ(source.controllable_component_id.value(), "scene_objects");
  EXPECT_EQ(source.observer_configuration.find_object_member(
                "controllable_component_id"),
            nullptr);
  EXPECT_EQ(source.observer_configuration
                .find_object_member("simulation_poll_period_ms")
                ->as_number(),
            250.0);
  ASSERT_TRUE(source.joint_state.has_value());
  EXPECT_EQ(source.joint_state->topic, "/isaac_joint_states");
  ASSERT_EQ(source.bindings.size(), 3U);
  // Observer order: joint_state, simulation entities (by object), Detection3D.
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[0]),
            "poses/robot/viewer.measured_robot");
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[1]),
            "poses/cheese_box");
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings[2]),
            "poses/robot/viewer.measured_tcp");
  EXPECT_FALSE(source.bindings[0].eligible);
  EXPECT_TRUE(source.bindings[1].eligible);
  EXPECT_FALSE(source.bindings[2].eligible);
  EXPECT_EQ(source.bindings[0].lifetime_ns, 1'000'000'000LL);
  EXPECT_EQ(source.bindings[1].lifetime_ns, 2'000'000'000LL);
  EXPECT_EQ(source.bindings[2].lifetime_ns, 1'000'000'000LL);
  const auto &entities = source.observer_configuration
                             .find_object_member("simulation_entities")
                             ->as_array();
  ASSERT_EQ(entities.size(), 2U);
  EXPECT_EQ(
      d::canonical_json(entities[0]),
      d::canonical_json(Value(Object{
          {"eligible", Value(true)},
          {"entity", Value("/World/cheese_box")},
          {"frame_kind", Value("cell_frame")},
          {"lifetime_ns", Value(2000000000.0)},
          {"local_orientation_xyzw", Numbers({0.0, 0.0, 0.0, 1.0})},
          {"local_position_xyz", Numbers({0.0, 0.0, 0.0})},
          {"object_id", Value("cheese_box")},
          {"policy_id", Value("scene")},
          {"source_coordinate_frame_id", Value("universe")},
          {"source_header_frame_id", Value("cheese_box_frame")},
          {"target_frame_id", Value("universe")}})));
  EXPECT_EQ(d::canonical_json(
                *entities[1].find_object_member("local_position_xyz")),
            "[0,0,0.2]");
  EXPECT_EQ(entities[1].find_object_member("source_header_frame_id")
                ->as_string(),
            "tcp_frame");
  // Overlay rows never carry the gripper reference.
  EXPECT_EQ(entities[1].find_object_member("relative_references"), nullptr);
  EXPECT_EQ(contract::find_pose_source(
                configuration, c::ControllableComponentId("scene_objects")),
            &source);
  EXPECT_EQ(contract::find_pose_source(configuration,
                                       c::ControllableComponentId("other")),
            nullptr);
}

TEST(ActivationPoseSource, AbsentJointStateDecodesAsNone) {
  const auto materialized = Materialize(
      Intent(Array{PoseSource("scene_objects",
                              Object{{"simulation", Simulation(false)}})},
             Array{EntityLink("box", "observe_only")}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  const auto configuration = contract::require_configuration(
      *materialized.bundle, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.pose_sources.size(), 1U);
  const auto &source = configuration.pose_sources.front();
  EXPECT_FALSE(source.joint_state.has_value());
  ASSERT_EQ(source.bindings.size(), 1U);
  EXPECT_EQ(source.bindings.front().lifetime_ns, 1'000'000'000LL);
  // No relative reference on the component: none in the expanded row.
  EXPECT_EQ(source.observer_configuration
                .find_object_member("simulation_entities")
                ->as_array()
                .front()
                .find_object_member("relative_references"),
            nullptr);
}

TEST(ActivationPoseSource, Detection3dLinkExpandsWithTheComponentTopic) {
  const auto materialized = Materialize(Intent(
      Array{PoseSource("scene_objects",
                       Object{{"detection3d", Detection()},
                              {"robot_overlay", Overlay(false)}})},
      Array{DetectionLink("part", "before_planning")},
      Array{Value(Object{{"controllable_component_id", Value("scene_objects")},
                         {"stream_id", Value("poses/part")},
                         {"budget_ns", Value(5000000000.0)}})}));
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  const auto configuration = contract::require_configuration(
      *materialized.bundle, graph.runtime_units.front().runtime_unit_id);
  const auto &source = configuration.pose_sources.front();
  const auto &observer = source.observer_configuration;
  // No simulation block: the poll period is the unused default.
  EXPECT_EQ(observer.find_object_member("simulation_poll_period_ms")
                ->as_number(),
            250.0);
  EXPECT_TRUE(
      observer.find_object_member("simulation_entities")->as_array().empty());
  const auto &detections =
      observer.find_object_member("detection3d_sources")->as_array();
  ASSERT_EQ(detections.size(), 1U);
  EXPECT_EQ(
      d::canonical_json(detections.front()),
      d::canonical_json(Value(Object{
          {"eligible", Value(true)},
          {"fallback_frame_id", Value("link_base")},
          {"frame_kind", Value("cell_frame")},
          {"lifetime_ns", Value(5000000000.0)},
          {"object_id", Value("part")},
          {"policy_id", Value("scene")},
          {"selector",
           Value(Object{{"class_id", Value("part")},
                        {"confidence_min", Value(0.5)},
                        {"detection_id", Value("")},
                        {"max_position_variance_m2", Value(0.0025)},
                        {"selection", Value("first")}})},
          {"target_frame_id", Value("link_base")},
          {"topic", Value("/output")}})));
  EXPECT_EQ(source.bindings.back().object_id, "part");
  EXPECT_EQ(contract::pose_source_stream_id(source.bindings.back()),
            "poses/part");
}

TEST(ActivationPoseSource, InvalidLinksAndComponentsAreRefused) {
  const auto refused = [](const Value &authored, std::string_view reason) {
    const auto message = Refusal(authored);
    EXPECT_NE(message.find(reason), std::string::npos) << message;
  };
  const auto simulated = [](bool relative = false) {
    return Array{PoseSource("scene_objects",
                            Object{{"simulation", Simulation(relative)}})};
  };
  const auto detected = [] {
    return Array{
        PoseSource("scene_objects", Object{{"detection3d", Detection()}})};
  };
  refused(Intent(simulated(), Array{EntityLink("box", "continuous", "other")}),
          "scene_pose_source_unknown_component");
  refused(Intent(detected(), Array{EntityLink("box", "continuous")}),
          "scene_pose_source_mode_missing");
  refused(Intent(simulated(), Array{DetectionLink("part", "continuous")}),
          "scene_pose_source_mode_missing");
  refused(Intent(Array{PoseSource("scene_objects",
                                  Object{{"robot_overlay", Overlay()}})}),
          "scene_pose_source_mode_missing");
  refused(Intent(simulated(false),
                 Array{EntityLink("box", "continuous_relative_to_attached")}),
          "scene_pose_source_relative_reference_missing");
  // A legacy source-wide reference cannot be mapped without a gripper target.
  refused(Intent(simulated(true), Array{EntityLink("box", "continuous")}),
          "scene_pose_source_relative_references_required");
  refused(Intent(detected(),
                 Array{DetectionLink("part", "continuous_relative_to_attached")}),
          "scene_pose_source_relative_requires_simulation");
  refused(Intent(simulated(), Array{EntityLink("box", "continuous"),
                                    EntityLink("box", "observe_only")}),
          "scene_pose_source_duplicate_object");
  // The existing expanded-row checks still apply after the join.
  refused(Intent(simulated(), Array{EntityLink("Box", "continuous")}),
          "pose_source_binding_id_invalid");
  refused(Intent(simulated(),
                 Array{EntityLink(std::string(60, 'a'), "continuous")}),
          "pose_source_binding_id_invalid");
  refused(Intent(simulated()), "pose_source_bindings_empty");
  // The authored schema is closed: the old wrapper shape is refused.
  EXPECT_FALSE(Refusal(Intent(Array{PoseSource(
                           "scene_objects",
                           Object{{"pose_sources", Value(Array{})}})}))
                   .empty());
  // A pose source needs its exact model-free profile.
  auto wrong_profile =
      PoseSource("scene_objects", Object{{"simulation", Simulation()}});
  auto object = wrong_profile.as_object();
  for (auto &[name, value] : object)
    if (name == "component_profile_id")
      value = Value("io_device_v0");
  EXPECT_THROW((void)detail::parse_activation_deployment_intent(
                   Intent(Array{Value(std::move(object))}), {}),
               std::invalid_argument);
}
} // namespace
