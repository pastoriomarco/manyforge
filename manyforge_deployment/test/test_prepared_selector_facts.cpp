#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>

#include <manyforge_deployment/program_reference_facts.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include "lite6_retained_consumer_fixture.hpp"
#include "maintained_activation_fixture.hpp"
#include "prepared_selector_facts_internal.hpp"

namespace {
namespace deployment = manyforge::deployment;
namespace core = manyforge::core;
using V = deployment::CanonicalValue;

struct TemporaryRoot {
  inline static std::atomic<unsigned> sequence{};
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("manyforge-selector-facts-" + std::to_string(::getpid()) + "." +
       std::to_string(++sequence));
  TemporaryRoot() { std::filesystem::create_directory(path); }
  ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
};

const V* FindRow(const V::Array& rows, const std::string& component,
                 const char* field, const std::string& id) {
  const V* found = nullptr;
  for (const auto& row : rows) {
    const auto* reference = row.find_object_member("reference");
    if (!reference) continue;
    const auto* component_id = reference->find_object_member("component_id");
    const auto* selected_id = reference->find_object_member(field);
    if (component_id && selected_id && component_id->as_string() == component &&
        selected_id->as_string() == id) {
      EXPECT_EQ(found, nullptr) << component << "/" << id;
      found = &row;
    }
  }
  return found;
}

TEST(PreparedSelectorFacts, IncludesComponentAliasesAndOwnedModelIds) {
  TemporaryRoot root;
  const auto candidate = deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path, deployment::parse_strict_json_model_yaml(R"({"interrupt_rules":[]})").root());
  ASSERT_TRUE(candidate.model_map.has_value());
  const auto facts = deployment::detail::prepared_selector_facts(candidate);
  const auto* frames_value = facts.find_object_member("frames");
  const auto* bodies_value = facts.find_object_member("bodies");
  ASSERT_NE(frames_value, nullptr);
  ASSERT_NE(bodies_value, nullptr);
  const auto& frames = frames_value->as_array();
  const auto& bodies = bodies_value->as_array();
  EXPECT_FALSE(frames.empty());
  EXPECT_FALSE(bodies.empty());
  const auto& model = *candidate.model_map;

  const auto check_frame = [&](const core::ProgramFrameReferenceDTO& reference) {
    const auto* row = FindRow(frames, reference.component_id.value(), "frame_id", reference.frame_id.value());
    ASSERT_NE(row, nullptr);
    const auto* ambiguous = row->find_object_member("ambiguous");
    ASSERT_NE(ambiguous, nullptr);
    const auto matches = deployment::find_program_frame_matches(reference, model);
    EXPECT_EQ(ambiguous->as_bool(), matches.size() != 1U);
    const auto* moving = row->find_object_member("moving_targets");
    ASSERT_NE(moving, nullptr);
    std::set<std::pair<std::string, std::string>> actual;
    for (const auto& item : moving->as_array()) {
      const auto* component = item.find_object_member("component_id");
      const auto* target = item.find_object_member("command_target_id");
      ASSERT_NE(component, nullptr);
      ASSERT_NE(target, nullptr);
      actual.emplace(component->as_string(), target->as_string());
    }
    std::set<std::pair<std::string, std::string>> expected;
    if (matches.size() == 1U)
      for (const auto& target : model.targets()) {
        if (!model.kinematic_index().carries(target.controllable_component_id, reference.component_id)) continue;
        for (const auto& association : target.frames) {
          const auto* physical = std::get_if<deployment::CompositionComponentModelFrame>(&association.frame);
          if (physical && physical->controllable_component_id == reference.component_id &&
              physical->model_instance_id == matches.front().model_instance_id &&
              physical->model_revision == matches.front().model_revision &&
              physical->compiled_frame_name == matches.front().compiled_frame_name)
            expected.emplace(target.controllable_component_id.value(), target.command_target_id.value());
        }
      }
    EXPECT_EQ(actual, expected);
  };
  const auto check_body = [&](const core::ProgramCollisionBodyReferenceDTO& reference) {
    const auto* row = FindRow(bodies, reference.component_id.value(), "collision_body_id", reference.collision_body_id.value());
    ASSERT_NE(row, nullptr);
    const auto* ambiguous = row->find_object_member("ambiguous");
    ASSERT_NE(ambiguous, nullptr);
    EXPECT_EQ(ambiguous->as_bool(), deployment::find_program_collision_body_matches(reference, model).size() != 1U);
  };
  for (const auto& target : model.targets()) {
    for (const auto& row : target.frames)
      if (const auto* frame = std::get_if<deployment::CompositionComponentModelFrame>(&row.frame);
          frame && frame->controllable_component_id == target.controllable_component_id)
        check_frame({target.controllable_component_id, row.frame_role_id});
    for (const auto& row : target.collision_bodies)
      check_body({target.controllable_component_id, row.collision_body_role_id});
  }
  for (const auto& path : model.frame_paths())
    if (const auto* frame = std::get_if<deployment::CompositionComponentModelFrame>(&path.terminal_frame))
      check_frame({frame->controllable_component_id, frame->frame_role_id});
  for (const auto& body : model.model_collision_bodies())
    if (body.owner_component_id)
      check_body({*body.owner_component_id, body.collision_body_role_id});
}

TEST(PreparedSelectorFacts, ProjectsDeploymentInterruptRulesAndSignalDirections) {
  TemporaryRoot root;
  const auto candidate = deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path, V(V::Object{}), "examples/mock_lite6_light_curtain_rule.scenario");
  const auto facts = deployment::detail::prepared_selector_facts(candidate);
  const auto* rules = facts.find_object_member("rules");
  ASSERT_NE(rules, nullptr);
  ASSERT_EQ(rules->as_array().size(), 1U);
  const auto& rule = rules->as_array().front();
  ASSERT_EQ(rule.as_object().size(), 5U);
  EXPECT_EQ(rule.find_object_member("rule_id")->as_string(), "light_curtain");
  EXPECT_EQ(rule.find_object_member("scope")->as_string(), "all_motion");
  EXPECT_EQ(rule.find_object_member("on_trip")->as_string(), "stop");
  EXPECT_TRUE(rule.find_object_member("active_at_start")->as_bool());
  const auto* compare = rule.find_object_member("condition")->find_object_member("compare");
  ASSERT_NE(compare, nullptr);
  EXPECT_TRUE(compare->find_object_member("value")->as_bool());

  const auto* signal = FindRow(facts.find_object_member("signals")->as_array(), "station_io",
                               "signal_name", "curtain_blocked");
  ASSERT_NE(signal, nullptr);
  EXPECT_EQ(signal->find_object_member("direction")->as_string(), "input");
  EXPECT_EQ(signal->find_object_member("value_type")->as_string(), "bool");

  TemporaryRoot empty_root;
  const auto empty = deployment::test::MaterializeMaintainedInterruptDeployment(
      empty_root.path, deployment::parse_strict_json_model_yaml(R"({"interrupt_rules":[]})").root(),
      "examples/mock_lite6_light_curtain_rule.scenario");
  EXPECT_TRUE(deployment::detail::prepared_selector_facts(empty).find_object_member("rules")->as_array().empty());
}

TEST(PreparedSelectorFacts, DefaultTcpUsesDeclaredAliasFromQualifiedTarget) {
  TemporaryRoot root;
  const auto fixture = deployment::test::materialize_lite6_retained_consumer_fixture_from(
      root.path, deployment::test::make_lite6_ros2_control_observed_consumer_retention_fixture(
                     false, false, "flange", true));
  ASSERT_NE(fixture.bundle, nullptr);
  const auto& products = fixture.bundle->value().compiled_products.symbol_role_maps;
  ASSERT_EQ(products.size(), 1U);
  const deployment::FilesystemVerifiedRetainedModelMapSource maps(root.path);
  const auto model = maps.get_verified({fixture.bundle->retained_bundle_id(), products.front().product_id});
  ASSERT_TRUE(model);
  const core::ProgramTargetReferenceDTO target{
      core::ControllableComponentId(std::string(deployment::test::kLite6ControllableComponentId)),
      core::CommandTargetId(std::string(deployment::test::kLite6ArmTargetId))};
  const auto value = deployment::detail::prepared_target_default_tcp(*model, target);
  const auto* frame = value.find_object_member("frame");
  ASSERT_NE(frame, nullptr);
  const auto* frame_id = frame->find_object_member("frame_id");
  ASSERT_NE(frame_id, nullptr);
  EXPECT_EQ(frame_id->as_string(), "flange");

  const deployment::FilesystemCatalogArtifactStore catalog(root.path);
  const auto inventory = deployment::qualified_candidate_robot_tcps(target, *fixture.bundle, *model, catalog);
  const auto found = std::find_if(inventory.begin(), inventory.end(), [](const auto& row) {
    return row.entry.frame.frame_id == core::FrameRoleId("flange");
  });
  ASSERT_NE(found, inventory.end());
  EXPECT_EQ(found->entry.target, target);
  const auto* physical_target = model->find_target(target.component_id, target.command_target_id);
  ASSERT_NE(physical_target, nullptr);
  const auto* alias = deployment::find_target_frame_role(*physical_target, core::FrameRoleId("flange"));
  ASSERT_NE(alias, nullptr);
  const auto* physical = std::get_if<deployment::CompositionComponentModelFrame>(&alias->frame);
  ASSERT_NE(physical, nullptr);
  EXPECT_EQ(physical->frame_role_id.value(), "flange");
}
}  // namespace
