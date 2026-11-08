#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/global_io_rule_qualification.hpp>
#include <manyforge_deployment/prepared_catalog_source.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/program_io_stop_response_validation.hpp>
#include <manyforge_deployment/program_reference_validator.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>
#include <optional>
#include <unistd.h>
#include <utility>

#include "content_identity_internal.hpp"
#include "io_retained_fixture.hpp"
#include "maintained_activation_fixture.hpp"
#include "maintained_interrupt_program_fixture.hpp"
#include "pneumatic_program_fixture.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace {
namespace c = manyforge::core;
namespace d = manyforge::deployment;
using V = d::CanonicalValue;
struct Root final {
  inline static std::atomic<unsigned> sequence{};
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("manyforge-global-rules-" + std::to_string(::getpid()) + "." +
       std::to_string(++sequence));
  Root() {
    if (!std::filesystem::create_directory(path))
      throw std::logic_error("fixture root exists");
  }
  ~Root() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
V parse(std::string_view text) {
  return d::parse_strict_json_model_yaml(text).root();
}
V replace(const V &root, std::string name, V value) {
  auto object = root.as_object();
  for (auto &[key, selected] : object)
    if (key == name) {
      selected = std::move(value);
      return V(std::move(object));
    }
  object.emplace_back(std::move(name), std::move(value));
  return V(std::move(object));
}
V rule(V scope = V("all_motion"), bool signal = false) {
  return V(V::Object{
      {"rule_id", V("guard")},
      {"condition",
       signal
           ? parse(
                 R"({"compare":{"operand":{"signal":{"controllable_component_id":"device.io","signal_name":"ready"}},"op":"==","value":false}})")
           : parse(
                 R"({"compare":{"operand":{"key":"ready"},"op":"==","value":false}})")},
      {"scope", std::move(scope)},
      {"priority", V(0.0)},
      {"on_trip", V("stop")},
      {"after_stop", V("hold")},
      {"active_at_start", V(false)}});
}
V without(const V &root, std::string_view name) {
  auto object = root.as_object();
  std::erase_if(object, [&](const auto &field) { return field.first == name; });
  return V(std::move(object));
}
V groups(std::string name) {
  return V(V::Object{{"execution_groups", V(V::Array{V(std::move(name))})}});
}
V configuration(V selected_rule) {
  return V(V::Object{
      {"interrupt_rules", V(V::Array{std::move(selected_rule)})}});
}
auto logical_arm(const std::filesystem::path &root, const V &config) {
  return d::test::materialize_lite6_retained_consumer_fixture_from(
      root, d::test::make_lite6_controllable_retention_fixture(
                false, false, 1U, {}, false, true, "resource",
                true, {}, {}, false, false, std::nullopt,
                d::test::kLite6ArmTargetId, std::nullopt, std::nullopt, {}, true,
                {}, "manyforge.provider.ros2_control", false, true,
                nullptr, nullptr, true, "pneumatic", std::nullopt,
                "commanded", nullptr, &config));
}
auto arm(const std::filesystem::path &root, const V &config) {
  return d::test::MaterializeMaintainedInterruptDeployment(root, config);
}

TEST(RetainedTcpInventoryTest, MountedToolIsMarkedElsewhereFromTheOtherRobot) {
  Root root;
  const auto product = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"),
      "examples/mock_dual_arm_shared_group.scenario");
  ASSERT_TRUE(product.bundle);
  ASSERT_TRUE(product.model_map);
  const c::ControllableComponentId lite6("lite6_robot");
  const c::ControllableComponentId ur5e("ur5e_robot");
  const c::ControllableComponentId tool("lite_gripper");
  const auto &index = product.model_map->kinematic_index();
  EXPECT_TRUE(index.carries(lite6, tool));
  EXPECT_FALSE(index.carries(ur5e, tool));

  d::FilesystemCatalogArtifactStore catalog(root.path);
  const auto inventory = d::compile_direct_control_program_binding_inventory(
      *product.bundle, *product.model_map, catalog);
  const auto lite_binding = d::compile_direct_control_motion_program_binding(
      lite6, c::CommandTargetId("arm"), *product.bundle, *product.model_map, catalog);
  const auto ur_binding = d::compile_direct_control_motion_program_binding(
      ur5e, c::CommandTargetId("arm"), *product.bundle, *product.model_map, catalog);
  ASSERT_TRUE(lite_binding);
  ASSERT_TRUE(ur_binding);
  const auto lite_rows = d::direct_control_motion_tcps(
      *lite_binding, inventory, *product.bundle, *product.model_map);
  const auto ur_rows = d::direct_control_motion_tcps(
      *ur_binding, inventory, *product.bundle, *product.model_map);
  const auto carried = std::find_if(lite_rows.begin(), lite_rows.end(),
      [&](const auto &row) { return row.binding.entry.target.component_id == tool; });
  ASSERT_NE(carried, lite_rows.end());
  EXPECT_TRUE(carried->mounted_here);
  EXPECT_FALSE(carried->mounted_elsewhere);
  const auto elsewhere = std::find_if(ur_rows.begin(), ur_rows.end(),
      [&](const auto &row) { return row.binding.entry == carried->binding.entry; });
  ASSERT_NE(elsewhere, ur_rows.end());
  EXPECT_FALSE(elsewhere->mounted_here);
  EXPECT_TRUE(elsewhere->mounted_elsewhere);
  EXPECT_EQ(elsewhere->binding.frame, carried->binding.frame);
}

// MOUNT_COLLISION_EXCLUSIONS_PLAN: the maintained Lite 6 example's mount
// pairs compile sorted onto the graph edge and compose (every link exists).
TEST(MountCollisionExclusionsTest, MaintainedLite6MountCarriesItsSortedPairs) {
  Root root;
  const auto product = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"));
  ASSERT_TRUE(product.bundle);
  const auto &edges = product.bundle->value().cell_graph.component_attachments;
  ASSERT_EQ(edges.size(), 1U);
  using Pair = c::ComponentAttachmentCollisionExclusionDTO;
  EXPECT_EQ(edges.front().collision_exclusions,
            (std::vector<Pair>{{"lite_gripper_base_link", "link5"},
                               {"lite_gripper_left_finger", "link4"},
                               {"lite_gripper_left_finger", "link5"},
                               {"lite_gripper_left_finger", "link6"},
                               {"lite_gripper_left_finger", "link_eef"},
                               {"lite_gripper_right_finger", "link4"},
                               {"lite_gripper_right_finger", "link5"},
                               {"lite_gripper_right_finger", "link6"},
                               {"lite_gripper_right_finger", "link_eef"}}));
}

TEST(RetainedTcpInventoryTest, IncludesRealUnmountedToolWithoutCarryingIt) {
  Root root;
  const auto product = d::test::MaterializeMaintainedLite6WithUnmountedTool(root.path);
  ASSERT_TRUE(product.bundle);
  ASSERT_TRUE(product.model_map);
  const c::ControllableComponentId robot("lite6_robot");
  const c::ControllableComponentId tool("lite_gripper");
  const c::CommandTargetId arm_target("arm");
  const c::CommandTargetId tool_target("gripper");
  EXPECT_TRUE(product.model_map->kinematic_index().carries(robot, robot));
  EXPECT_FALSE(product.model_map->kinematic_index().carries(robot, tool));
  ASSERT_NE(product.model_map->find_target(tool, tool_target), nullptr);

  d::FilesystemCatalogArtifactStore catalog(root.path);
  const auto inventory = d::compile_direct_control_program_binding_inventory(
      *product.bundle, *product.model_map, catalog);
  const auto binding = d::compile_direct_control_motion_program_binding(
      robot, arm_target, *product.bundle, *product.model_map, catalog);
  ASSERT_TRUE(binding);
  const auto target = std::find_if(
      binding->value().target_bindings.begin(),
      binding->value().target_bindings.end(),
      [&](const auto &row) { return row.target.component_id == robot &&
                                    row.target.command_target_id == arm_target; });
  ASSERT_NE(target, binding->value().target_bindings.end());
  ASSERT_TRUE(target->robot_tcps.has_value());
  const auto row = std::find_if(target->robot_tcps->begin(), target->robot_tcps->end(),
      [&](const auto &tcp) { return tcp.entry.target.component_id == tool &&
                                   tcp.entry.target.command_target_id == tool_target &&
                                   tcp.entry.frame.frame_id.value() == "tcp"; });
  ASSERT_NE(row, target->robot_tcps->end());
  EXPECT_FALSE(row->end_effector);
  const auto rows = d::direct_control_motion_tcps(
      *binding, inventory, *product.bundle, *product.model_map);
  const auto projected = std::find_if(rows.begin(), rows.end(),
      [&](const auto &tcp) { return tcp.binding.entry == row->entry; });
  ASSERT_NE(projected, rows.end());
  EXPECT_FALSE(projected->mounted_here);
  EXPECT_FALSE(projected->mounted_elsewhere);
}

V program_root(const d::RetainedBundle &bundle, bool motion, bool key = true) {
  auto root = parse(
      R"({"api_version":"manyforge.program.v3","program_id":"global_rule_program",
    "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "program_profile":"composable-cell-v1","name":"Global rule fixture",
    "catalog":{"api_version":"manyforge.node_catalog.v3","content_version":"0.0.0"},
    "execution_groups":{"inspect":{"execution_policy":"independent"}},
    "blackboard":{"namespaces":[],"keys":[{"key":"ready","type":"bool","access":"read_write","persistence":"run","initial":false}]},
    "tree":{"node_id":"sequence","instance_id":"root","parameters":{},"execution_groups":["inspect"],"children":[]}})");
  root = replace(
      root, "catalog",
      replace(*root.find_object_member("catalog"), "content_version",
              V(bundle.value().catalogs.node_environment.content_version)));
  if (!key)
    root = replace(root, "blackboard", parse(R"({"namespaces":[],"keys":[]})"));
  if (motion) {
    root = replace(root, "execution_groups", parse(
        R"({"arm_group":{"execution_policy":"independent"}})"));
    root = replace(
        root, "tree",
        parse(
            R"({"node_id":"motion.move_to_pose","instance_id":"move","execution_groups":["arm_group"],"parameters":{"target":{"component_id":"lite6_robot","command_target_id":"arm"},"base_frame":{"component_id":"lite6_robot","frame_id":"base"},"tcp_endpoint":{"kind":"current"},"goal":{"frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_robot","frame_id":"base"}},"position_m":[0,0,0],"orientation":{"xyzw":[0,0,0,1]}}},"children":[]})"));
  }
  root = d::normalize_program_authoring(root);
  const auto revision = d::detail::compute_content_identity(
      root, {"program-revision", "manyforge.program.v3", "program_revision"});
  return replace(root, "program_revision", V(revision));
}
struct Selected final {
  std::shared_ptr<const d::ProgramBindingContract> program;
  std::shared_ptr<const d::ProgramBinding> binding;
  d::VerifiedRetainedModelMap map;
};
Selected select(const std::filesystem::path &root,
                const d::RetainedBundle &bundle, bool motion = false,
                bool key = true, const V *authored = nullptr) {
  d::FilesystemVerifiedRetainedModelMapSource maps(root);
  auto map = maps.get_verified(
      {bundle.retained_bundle_id(),
       bundle.value().compiled_products.symbol_role_maps.front().product_id});
  if (!map)
    throw std::logic_error("actual retained map missing");
  d::FilesystemCatalogArtifactStore catalog(root);
  auto program = d::verify_program_binding_contract(
      d::canonical_json(authored ? *authored : program_root(bundle, motion, key)),
      bundle, *map, catalog);
  auto binding = d::compile_program_binding(*program, bundle, *map, catalog);
  return {std::move(program), std::move(binding), std::move(*map)};
}

void coincident_gripper_presets(V::Object &document) {
  auto components = std::find_if(document.begin(), document.end(),
      [](const auto &entry) { return entry.first == "components"; });
  auto rows = components->second.as_array();
  for (auto &component : rows) {
    if (!component.find_object_member("model_source")) continue;
    auto configuration = *component.find_object_member("configuration");
    auto runtime = *configuration.find_object_member("runtime_configuration");
    // Since the tool split only the gripper component carries the presets.
    if (!runtime.find_object_member("gripper_target_configurations")) continue;
    auto grippers = runtime.find_object_member("gripper_target_configurations")->as_array();
    grippers.front() = replace(grippers.front(), "open_state_ref", V("closed"));
    runtime = replace(runtime, "gripper_target_configurations", V(std::move(grippers)));
    configuration = replace(configuration, "runtime_configuration", std::move(runtime));
    component = replace(component, "configuration", std::move(configuration));
  }
  components->second = V(std::move(rows));
}

void add_second_pneumatic_valve(V::Object &document) {
  auto components = std::find_if(document.begin(), document.end(),
      [](const auto &entry) { return entry.first == "components"; });
  auto rows = components->second.as_array();
  for (auto &component : rows) {
    const auto id = component.find_object_member("component_id")->as_string();
    // Machine owner and inactive member rows carry no runtime configuration.
    if (id != "lite_gripper" && id != "gripper_io") continue;
    auto configuration = *component.find_object_member("configuration");
    auto runtime = *configuration.find_object_member("runtime_configuration");
    if (id == "lite_gripper") {
      auto grippers = runtime.find_object_member("gripper_target_configurations")->as_array();
      auto mapping = *grippers.front().find_object_member("pneumatic_io");
      auto outputs = mapping.find_object_member("outputs")->as_array();
      outputs.push_back(parse(R"({"signal":{"controllable_component_id":"gripper_io",
          "signal_name":"valve_other"},"open_value":true,"close_value":false})"));
      mapping = replace(mapping, "outputs", V(std::move(outputs)));
      grippers.front() = replace(grippers.front(), "pneumatic_io", std::move(mapping));
      runtime = replace(runtime, "gripper_target_configurations", V(std::move(grippers)));
    } else if (id == "gripper_io") {
      auto signals = runtime.find_object_member("io_signals")->as_array();
      signals.push_back(parse(R"({"command_target_id":"io","controllable_component_id":"gripper_io",
          "direction":"output","provider":{"channel":"pneumatic_valve_other","profile":"mock_output"},
          "resource_role_id":"valve_other","signal_name":"valve_other",
          "source_id":"gripper_io.valve_other","value_type":"bool"})"));
      runtime = replace(runtime, "io_signals", V(std::move(signals)));
    }
    configuration = replace(configuration, "runtime_configuration", std::move(runtime));
    component = replace(component, "configuration", std::move(configuration));
  }
  components->second = V(std::move(rows));
}

TEST(RetainedIoPolicyCompilerTest, PneumaticMappedSignalsCompileAndReloadAcrossComponents) {
  Root root;
  const auto product = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"), "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  ASSERT_TRUE(product.model_map);
  const d::detail::PreparedCatalogSource catalog_source(product.catalog_artifacts);
  const auto fixture = d::test::compile_pneumatic_program_fixture(root.path, product, 2U);
  const auto &binding = fixture.binding->value();
  const auto gripper = std::find_if(binding.target_bindings.begin(), binding.target_bindings.end(),
      [](const auto &row) { return row.target.component_id.value() == "lite_gripper" &&
                                  row.target.command_target_id.value() == "gripper"; });
  ASSERT_NE(gripper, binding.target_bindings.end());
  ASSERT_TRUE(gripper->pneumatic_io);
  ASSERT_EQ(gripper->pneumatic_io->outputs.size(), 1U);
  ASSERT_EQ(gripper->pneumatic_io->inputs.size(), 1U);
  const auto &valve = gripper->pneumatic_io->outputs.front().signal;
  const auto &sensor = gripper->pneumatic_io->inputs.front().signal;
  EXPECT_EQ(valve.component_id.value(), "gripper_io");
  EXPECT_EQ(valve.signal_name, "valve");
  EXPECT_EQ(sensor.component_id.value(), "gripper_io");
  EXPECT_EQ(sensor.signal_name, "workpiece_grasped");
  EXPECT_EQ(std::count_if(binding.target_bindings.begin(), binding.target_bindings.end(),
      [&](const auto &row) { return row.target == gripper->target; }), 1);
  EXPECT_EQ(d::reload_verified_program_binding(fixture.binding->canonical_bytes(),
                                               *product.bundle, *product.model_map,
                                               catalog_source)->value(), binding);
}

TEST(RetainedIoPolicyCompilerTest, PneumaticOutputReusesExactSignalAcrossAuthoredNodes) {
  Root root;
  const auto product = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"), "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  const auto fixture = d::test::compile_pneumatic_program_fixture(root.path, product, 1U, 2U);
  const auto &binding = fixture.binding->value();
  const auto gripper = std::find_if(binding.target_bindings.begin(), binding.target_bindings.end(),
      [](const auto &row) { return row.target.component_id.value() == "lite_gripper" &&
                                  row.target.command_target_id.value() == "gripper"; });
  ASSERT_NE(gripper, binding.target_bindings.end());
  ASSERT_TRUE(gripper->pneumatic_io);
  ASSERT_EQ(gripper->pneumatic_io->outputs.size(), 1U);
  const auto &valve = gripper->pneumatic_io->outputs.front().signal;
  EXPECT_EQ(valve.component_id.value(), "gripper_io");
  EXPECT_EQ(valve.signal_name, "valve");
  EXPECT_EQ(std::count_if(binding.signal_bindings.begin(), binding.signal_bindings.end(),
      [&](const auto &row) { return row.reference == valve; }), 1);
}

TEST(RetainedIoPolicyCompilerTest, PneumaticMappedCoincidentPresetsFailAtActivationCompilation) {
  Root root;
  try {
    (void)d::test::MaterializeMaintainedInterruptDeployment(
        root.path, parse(R"({"interrupt_rules":[]})"), "examples/mock_lite6_pneumatic_pick_and_place.scenario",
        coincident_gripper_presets);
    FAIL() << "coincident mapped presets must be refused";
  } catch (const c::TypedError &error) {
    // Typed for the GUI; the message text is unchanged.
    EXPECT_EQ(error.code(), "pneumatic_io_presets_coincident");
    EXPECT_EQ(std::string(error.what()).rfind(
                  "Activation Deployment compiler: pneumatic_io_presets_coincident: ", 0),
              0U);
  }
}

TEST(RetainedIoPolicyCompilerTest, PneumaticReloadRejectsDuplicatePhysicalValve) {
  Root root;
  const auto product = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"), "examples/mock_lite6_pneumatic_pick_and_place.scenario",
      add_second_pneumatic_valve);
  const d::detail::PreparedCatalogSource catalog_source(product.catalog_artifacts);
  const auto fixture = d::test::compile_pneumatic_program_fixture(root.path, product);
  auto forged = fixture.binding->value();
  const auto gripper = std::find_if(forged.target_bindings.begin(), forged.target_bindings.end(),
      [](const auto &row) { return row.target.component_id.value() == "lite_gripper" &&
                                  row.target.command_target_id.value() == "gripper"; });
  ASSERT_NE(gripper, forged.target_bindings.end());
  ASSERT_TRUE(gripper->pneumatic_io);
  auto &outputs = gripper->pneumatic_io->outputs;
  ASSERT_EQ(outputs.size(), 2U);
  outputs.back() = outputs.front();
  EXPECT_THROW((void)d::detail::ProgramBindingConstructionAuthority::seal(std::move(forged)),
               d::ProgramBindingError);

  // A structurally valid route with changed valve polarity must also fail
  // contextual reload against the exact retained Deployment mapping.
  auto changed = fixture.binding->value();
  const auto changed_gripper = std::find_if(changed.target_bindings.begin(), changed.target_bindings.end(),
      [](const auto &row) { return row.target.component_id.value() == "lite_gripper" &&
                                  row.target.command_target_id.value() == "gripper"; });
  ASSERT_NE(changed_gripper, changed.target_bindings.end());
  ASSERT_TRUE(changed_gripper->pneumatic_io);
  ASSERT_FALSE(changed_gripper->pneumatic_io->outputs.empty());
  auto &first = changed_gripper->pneumatic_io->outputs.front();
  first.open_value = !first.open_value;
  first.close_value = !first.close_value;
  const auto resealed = d::detail::ProgramBindingConstructionAuthority::seal(std::move(changed));
  EXPECT_THROW((void)d::reload_verified_program_binding(resealed->canonical_bytes(),
                  *product.bundle, *product.model_map, catalog_source),
               d::ProgramBindingError);
}

TEST(RetainedIoPolicyCompilerTest,
     SignalOnlyGlobalsHaveNoProgramOrRunRequirement) {
  Root root;
  auto rules = V(V::Array{
      rule(V("all_motion"), true),
      replace(rule(V("everything"), true), "rule_id", V("emergency"))});
  const auto fixture = d::test::materialize_boolean_io_retained_fixture(
      root.path, true, false, false, false, false, &rules);
  const auto reloaded = d::detail::RetainedBundleReloadAuthority::reload(
      fixture.bundle->canonical_bytes());
  ASSERT_NE(reloaded, nullptr);
  const auto compiled =
      d::compile_retained_io_policies(nullptr, nullptr, *reloaded);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().activation_policies.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<c::IoActivationEverythingScopeDTO>(
      compiled.value().activation_policies[0].scope));
  EXPECT_TRUE(std::holds_alternative<c::IoActivationAllMotionScopeDTO>(
      compiled.value().activation_policies[1].scope));
  EXPECT_TRUE(compiled.value().referenced_deployment_blackboard_keys.empty());
  EXPECT_EQ(
      compiled.value().activation_policies[1].required_signal_sources,
      std::vector<c::InterruptSourceId>{c::InterruptSourceId("source.io")});
}

TEST(RetainedIoPolicyCompilerTest,
     GlobalRuleCompilesContractFieldsWithCompilerDefaults) {
  Root default_root, explicit_root, failing_root;
  // on_trip is required; after_stop defaults to hold (stop
  // only) and active_at_start to false.
  {
    Root missing_root;
    auto missing = V(V::Array{without(rule(V("all_motion"), true), "on_trip")});
    EXPECT_THROW(d::test::materialize_boolean_io_retained_fixture(
                     missing_root.path, true, false, false, false, false, &missing),
                 std::exception);
  }
  auto default_rules = V(V::Array{without(
      without(rule(V("all_motion"), true), "after_stop"), "active_at_start")});
  auto explicit_rules = V(V::Array{rule(V("all_motion"), true)});
  auto failing_rules = V(V::Array{replace(
      replace(rule(V("all_motion"), true), "after_stop", V("fail_node")),
      "active_at_start", V(true))});
  const auto defaults = d::test::materialize_boolean_io_retained_fixture(
      default_root.path, true, false, false, false, false, &default_rules);
  const auto explicit_stop = d::test::materialize_boolean_io_retained_fixture(
      explicit_root.path, true, false, false, false, false, &explicit_rules);
  const auto failing = d::test::materialize_boolean_io_retained_fixture(
      failing_root.path, true, false, false, false, false, &failing_rules);
  EXPECT_EQ(defaults.bundle->canonical_bytes(),
            explicit_stop.bundle->canonical_bytes());
  EXPECT_NE(defaults.bundle->canonical_bytes(),
            failing.bundle->canonical_bytes());
  const auto *configuration =
      defaults.bundle->normalized_root().find_object_member(
          "interrupt_policy_configuration");
  ASSERT_NE(configuration, nullptr);
  ASSERT_EQ(configuration->as_object().size(), 1U);
  const auto *sealed_rules =
      configuration->find_object_member("interrupt_rules");
  ASSERT_NE(sealed_rules, nullptr);
  ASSERT_EQ(sealed_rules->as_array().size(), 1U);
  const auto &sealed = sealed_rules->as_array().front();
  EXPECT_EQ(sealed.find_object_member("on_trip")->as_string(), "stop");
  EXPECT_EQ(sealed.find_object_member("after_stop")->as_string(), "hold");
  EXPECT_FALSE(sealed.find_object_member("active_at_start")->as_bool());
  const auto reloaded = d::detail::RetainedBundleReloadAuthority::reload(
      failing.bundle->canonical_bytes());
  ASSERT_NE(reloaded, nullptr);
  const auto stop_compiled =
      d::compile_retained_io_policies(nullptr, nullptr, *defaults.bundle);
  const auto failing_compiled =
      d::compile_retained_io_policies(nullptr, nullptr, *reloaded);
  ASSERT_TRUE(stop_compiled.ok()) << stop_compiled.error().message;
  ASSERT_TRUE(failing_compiled.ok()) << failing_compiled.error().message;
  ASSERT_EQ(stop_compiled.value().activation_policies.size(), 1U);
  ASSERT_EQ(failing_compiled.value().activation_policies.size(), 1U);
  const auto &stop = stop_compiled.value().activation_policies.front();
  EXPECT_EQ(stop.on_trip, c::IoRuleOnTripDTO::kStop);
  EXPECT_EQ(stop.after_stop, c::IoRuleAfterStopDTO::kHold);
  EXPECT_FALSE(stop.active_at_start);
  const auto &fail_node = failing_compiled.value().activation_policies.front();
  EXPECT_EQ(fail_node.on_trip, c::IoRuleOnTripDTO::kStop);
  EXPECT_EQ(fail_node.after_stop, c::IoRuleAfterStopDTO::kFailNode);
  EXPECT_TRUE(fail_node.active_at_start);
}
TEST(RetainedIoPolicyCompilerTest, GlobalNotifyCompilesWithoutAfterStop) {
  Root root;
  auto rules = V(V::Array{replace(
      replace(without(rule(V("all_motion"), true), "after_stop"), "on_trip",
              V("notify")),
      "active_at_start", V(true))});
  const auto retained = d::test::materialize_boolean_io_retained_fixture(
      root.path, true, false, false, false, false, &rules);
  const auto *sealed = retained.bundle->normalized_root()
                           .find_object_member("interrupt_policy_configuration")
                           ->find_object_member("interrupt_rules");
  EXPECT_EQ(sealed->as_array().front().find_object_member("after_stop"),
            nullptr);
  const auto compiled = d::compile_retained_io_policies(nullptr, nullptr, *retained.bundle);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().activation_policies.size(), 1U);
  const auto &notify = compiled.value().activation_policies.front();
  EXPECT_EQ(notify.on_trip, c::IoRuleOnTripDTO::kNotify);
  EXPECT_TRUE(notify.active_at_start);
  EXPECT_FALSE(notify.stop_entry_stage_id);
}
TEST(RetainedIoPolicyCompilerTest,
     ActualSelectedProgramTypesKeysAndProspectiveSystemDeclarations) {
  Root root;
  const auto config = configuration(rule());
  const auto fixture = logical_arm(root.path, config);
  auto selected = select(root.path, *fixture.bundle);
  const auto compiled = d::compile_retained_io_policies(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().referenced_deployment_blackboard_keys.size(), 1U);
  EXPECT_EQ(compiled.value().referenced_deployment_blackboard_keys[0].name,
            "ready");
  EXPECT_EQ(
      compiled.value().referenced_deployment_blackboard_keys[0].value_kind,
      c::IoConditionValueKind::kBoolean);
  const auto &declarations = selected.program->blackboard_key_declarations();
  EXPECT_TRUE(std::any_of(declarations.begin(), declarations.end(),
                          [](const auto &key) {
                            return key.name == "interrupt_state.guard" &&
                                   key.declared_type == "string";
                          }));
  const auto &authored_keys = selected.program->normalized_program_root()
                                  .find_object_member("blackboard")
                                  ->find_object_member("keys")
                                  ->as_array();
  EXPECT_TRUE(std::none_of(
      authored_keys.begin(), authored_keys.end(), [](const auto &key) {
        return key.find_object_member("key")->as_string().starts_with(
            "interrupt_state.");
      }));
  const auto absent = select(root.path, *fixture.bundle, false, false);
  EXPECT_FALSE(d::compile_retained_io_policies(
                   *absent.program, *selected.binding, *fixture.bundle)
                   .ok());
  EXPECT_FALSE(d::compile_retained_io_policies(*absent.program, *absent.binding,
                                               *fixture.bundle)
                   .ok());
  const auto no_program =
      d::compile_retained_io_policies(nullptr, nullptr, *fixture.bundle);
  ASSERT_TRUE(no_program.ok()) << no_program.error().message;
  ASSERT_EQ(no_program.value().activation_policies.size(), 1U);
  EXPECT_EQ(no_program.value().activation_policies.front().condition, nullptr);
  EXPECT_EQ(
      no_program.value().activation_policies.front().waiting_for_program_keys,
      (std::vector<std::string>{"ready"}));
  EXPECT_TRUE(no_program.value().referenced_deployment_blackboard_keys.empty());
  auto authored = program_root(*fixture.bundle, false);
  auto board = *authored.find_object_member("blackboard");
  auto keys = board.find_object_member("keys")->as_array();
  keys.push_back(parse(
      R"({"key":"interrupt_state.guard","type":"string","access":"read_write","persistence":"run"})"));
  authored = replace(authored, "blackboard", replace(board, "keys", V(keys)));
  authored = replace(authored, "program_revision",
                     V(d::detail::compute_content_identity(
                         authored, {"program-revision", "manyforge.program.v3",
                                    "program_revision"})));
  d::FilesystemCatalogArtifactStore catalog(root.path);
  EXPECT_THROW(d::verify_program_binding_contract(d::canonical_json(authored),
                                                  *fixture.bundle, selected.map, catalog),
               d::ProgramContractError);
}

TEST(RetainedIoPolicyCompilerTest,
     StaticGroupsResolveExactMotionWithoutRunAndKnownNonmovingGroupsStayEmpty) {
  Root root;
  const auto config = configuration(rule(groups("arm_group")));
  const auto fixture = arm(root.path, config);
  auto selected = select(root.path, *fixture.bundle, true);
  const auto compiled = d::compile_retained_io_policies(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  const auto *scope = std::get_if<c::IoActivationExecutionGroupsScopeDTO>(
      &compiled.value().activation_policies[0].scope);
  ASSERT_NE(scope, nullptr);
  EXPECT_EQ(scope->program_binding_id, selected.binding->program_binding_id());
  ASSERT_EQ(scope->subjects.size(), 1U);
  EXPECT_EQ(scope->subjects[0].controllable_component_id,
            c::ControllableComponentId("lite6_robot"));
  const auto &bindings = fixture.bundle->value().cell_graph.control_bindings;
  const auto control_it =
      std::find_if(bindings.begin(), bindings.end(), [](const auto &row) {
        return row.controllable_component_id ==
                   c::ControllableComponentId("lite6_robot") &&
               row.command_target_id == c::CommandTargetId("arm") &&
               row.capability_id ==
                   c::CapabilityId("execution.trajectory.execute");
      });
  ASSERT_NE(control_it, bindings.end());
  const auto &control = *control_it;
  EXPECT_EQ(scope->subjects[0].control_binding_id, control.control_binding_id);
  EXPECT_EQ(scope->subjects[0].control_binding_revision,
            control.control_binding_revision);
  const auto logical = select(root.path, *fixture.bundle);
  const auto unknown = d::compile_retained_io_policies(
      *logical.program, *logical.binding, *fixture.bundle);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"),
            "global_interrupt_group_unbound");
  EXPECT_EQ(unknown.error().details.at("rule_id"), "guard");
  EXPECT_EQ(unknown.error().details.at("group_id"), "arm_group");
  EXPECT_NE(unknown.error().message.find("rule guard"), std::string::npos);
  EXPECT_NE(unknown.error().message.find("group arm_group"), std::string::npos);
  Root io_root;
  auto io_rules = V(V::Array{rule(groups("inspect"), true)});
  const auto io = d::test::materialize_boolean_io_retained_fixture(
      io_root.path, true, false, false, false, false, &io_rules);
  const auto no_program =
      d::compile_retained_io_policies(nullptr, nullptr, *io.bundle);
  ASSERT_FALSE(no_program.ok());
  EXPECT_EQ(no_program.error().details.at("reason"),
            "global_interrupt_program_binding_missing");
  EXPECT_EQ(no_program.error().details.at("rule_id"), "guard");
  EXPECT_EQ(no_program.error().details.at("group_id"), "inspect");
  EXPECT_NE(no_program.error().message.find("Applied Program"), std::string::npos);
  EXPECT_NE(no_program.error().message.find("group inspect"), std::string::npos);
  const auto nonmoving = select(io_root.path, *io.bundle);
  const auto empty = d::compile_retained_io_policies(
      *nonmoving.program, *nonmoving.binding, *io.bundle);
  ASSERT_TRUE(empty.ok());
  EXPECT_TRUE(std::get<c::IoActivationExecutionGroupsScopeDTO>(
                  empty.value().activation_policies[0].scope)
                  .subjects.empty());
  Root explicit_entry_root;
  auto explicit_rules =
      V(V::Array{replace(rule(groups("inspect"), true), "stop_entry_stage_id",
                         V("software_hard"))});
  const auto explicit_io = d::test::materialize_boolean_io_retained_fixture(
      explicit_entry_root.path, true, false, false, false, false,
      &explicit_rules);
  const auto explicit_selected =
      select(explicit_entry_root.path, *explicit_io.bundle);
  const auto explicit_compiled = d::compile_retained_io_policies(
      *explicit_selected.program, *explicit_selected.binding,
      *explicit_io.bundle);
  ASSERT_TRUE(explicit_compiled.ok());
  const auto unavailable = d::validate_global_io_stop_entries(
      explicit_compiled.value(), *explicit_io.bundle, &explicit_selected.map,
      {});
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"),
            "io_rule_stop_entry_target_unqualified");
}

TEST(RetainedIoPolicyCompilerTest,
     GroupEntryUsesSameQualifiedRawChainAndActualPreparedApplyRecompile) {
  Root root;
  const auto config = configuration(replace(
      rule(groups("arm_group")), "stop_entry_stage_id", V("software_hard")));
  const auto fixture = arm(root.path, config);
  auto selected = select(root.path, *fixture.bundle, true);
  const auto compiled = d::compile_retained_io_policies(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_TRUE(compiled.ok());
  const auto refs =
      d::required_global_io_stop_artifacts(compiled.value(), *fixture.bundle);
  ASSERT_TRUE(refs.ok());
  ASSERT_EQ(refs.value().size(), 1U);
  d::FilesystemVerifiedRawArtifactSource raw(root.path);
  std::vector<d::PreparedActivationArtifact> artifacts;
  for (const auto &ref : refs.value()) {
    const auto bytes = raw.get_verified(ref.content_addressed_store_key,
                                        ref.content_digest, ref.byte_size);
    ASSERT_TRUE(bytes);
    artifacts.push_back(
        {ref.content_addressed_store_key, ref.content_digest, *bytes});
  }
  const auto valid = d::validate_global_io_stop_entries(
      compiled.value(), *fixture.bundle, &selected.map, artifacts);
  ASSERT_TRUE(valid.ok()) << valid.error().details.at("reason");
  EXPECT_FALSE(d::validate_global_io_stop_entries(
                   compiled.value(), *fixture.bundle, &selected.map, {})
                   .ok());
  d::FilesystemCatalogArtifactStore catalog(root.path);
  d::FilesystemRawArtifactStore raw_store(root.path);
  const auto prepared = d::assemble_prepared_activation(
      {d::PreparedScenarioOrigin{
           "global.apply.fixture",
           c::ContentDigest("sha256:" + std::string(64U, 'a')),
           "/fixtures/global-apply.scenario"},
       c::ContentDigest("sha256:" + std::string(64U, 'b')), "optional"},
      *fixture.bundle, selected.map, *selected.program, catalog,
      raw_store);
  d::FilesystemPreparedActivationRepository repository(root.path);
  ASSERT_TRUE(repository.put_if_absent(prepared));
  ASSERT_NE(repository.get_verified(prepared.prepared_activation_id()),
            nullptr);
  Root invalid_root;
  const auto wrong_config = configuration(replace(
      rule(groups("arm_group")), "stop_entry_stage_id", V("foreign_stage")));
  const auto wrong = arm(invalid_root.path, wrong_config);
  auto wrong_selected = select(invalid_root.path, *wrong.bundle, true);
  d::FilesystemCatalogArtifactStore wrong_catalog(invalid_root.path);
  d::FilesystemRawArtifactStore wrong_raw(invalid_root.path);
  EXPECT_THROW(
      d::assemble_prepared_activation(
          {d::PreparedScenarioOrigin{
               "global.apply.fixture",
               c::ContentDigest("sha256:" + std::string(64U, 'a')),
               "/fixtures/global-apply.scenario"},
           c::ContentDigest("sha256:" + std::string(64U, 'b')), "optional"},
          *wrong.bundle, wrong_selected.map, *wrong_selected.program,
          wrong_catalog, wrong_raw),
      d::PreparedActivationInputError);
}

TEST(RetainedIoPolicyCompilerTest,
     MissingGlobalGroupAndKeyKeepNamesThroughPreparedApplyException) {
  for (const bool missing_group : {true, false}) {
    SCOPED_TRACE(missing_group ? "missing group" : "missing key");
    Root root;
    const auto fixture = arm(root.path,
        configuration(rule(missing_group ? groups("arm_group") : V("all_motion"))));
    // Both inputs use the normal maintained bundle/model map and genuine
    // public Program binding. One Program lacks arm_group, the other ready.
    const auto selected = select(root.path, *fixture.bundle, false, missing_group);
    const auto qualification = d::compile_retained_io_policies(
        *selected.program, *selected.binding, *fixture.bundle);
    ASSERT_FALSE(qualification.ok());
    const std::string expected = missing_group ? "global_interrupt_group_unbound"
                                               : "global_rule_key_undeclared";
    EXPECT_EQ(qualification.error().details.at("reason"), expected);
    EXPECT_EQ(qualification.error().details.at("rule_id"), "guard");
    EXPECT_EQ(qualification.error().details.at(missing_group ? "group_id" : "key"),
              missing_group ? "arm_group" : "ready");
    d::FilesystemCatalogArtifactStore catalog(root.path);
    d::FilesystemRawArtifactStore raw(root.path);
    try {
      static_cast<void>(d::assemble_prepared_activation(
          {d::PreparedScenarioOrigin{
               "global.qualification.fixture",
               c::ContentDigest("sha256:" + std::string(64U, 'a')),
               "/fixtures/global-apply.scenario"},
           c::ContentDigest("sha256:" + std::string(64U, 'b')), "optional"},
          *fixture.bundle, selected.map, *selected.program,
          catalog, raw));
      FAIL() << "A selected Program without the required group/key must refuse";
    } catch (const d::PreparedActivationInputError &error) {
      const std::string message = error.what();
      EXPECT_NE(message.find(expected), std::string::npos);
      EXPECT_NE(message.find("rule guard"), std::string::npos);
      EXPECT_NE(message.find(missing_group ? "group arm_group" : "key ready"),
                std::string::npos);
    }
  }
}

TEST(RetainedIoPolicyCompilerTest,
     ClosedGlobalCarrierRefusesUnknownFieldsPriorityAndEmptyGroups) {
  for (const auto &invalid :
       {replace(rule(V("all_motion"), true), "priority", V(101.0)),
        replace(rule(V("all_motion"), true), "fallback_outcome",
                V("fail_node")),
        replace(rule(V("all_motion"), true), "success_through_stage_id",
                V("software_hard")),
        replace(rule(V("all_motion"), true), "on_trip", V("program_reset")),
        replace(rule(V("all_motion"), true), "on_trip", V(true)),
        replace(rule(V("all_motion"), true), "on_trip", V(nullptr)),
        // on_trip is required (after_stop and active_at_start default).
        without(rule(V("all_motion"), true), "on_trip"),
        replace(rule(V("all_motion"), true), "latch_release", V("operator")),
        // gate, full_stop and idle_behaviour are gone.
        replace(rule(V("all_motion"), true), "on_trip", V("gate")),
        replace(rule(V("all_motion"), true), "on_trip", V("full_stop")),
        replace(rule(V("all_motion"), true), "idle_behaviour", V("latch")),
        // after_stop belongs to on_trip stop only.
        replace(replace(rule(V("all_motion"), true), "on_trip", V("notify")),
                "after_stop", V("hold")),
        replace(rule(V("all_motion"), true), "after_stop", V("resume")),
        replace(rule(V("all_motion"), true), "active_at_start", V("yes")),
        replace(rule(V("all_motion"), true), "post_latch_disposition",
                V("await_recovery_handler")),
        replace(rule(V("all_motion"), true), "scope",
                V(V::Object{{"execution_groups", V(V::Array{})}}))}) {
    Root root;
    auto rules = V(V::Array{invalid});
    EXPECT_THROW(d::test::materialize_boolean_io_retained_fixture(
                     root.path, true, false, false, false, false, &rules),
                 std::exception);
  }
  Root root;
  auto duplicate =
      V(V::Array{rule(V("all_motion"), true), rule(V("everything"), true)});
  EXPECT_THROW(d::test::materialize_boolean_io_retained_fixture(
                   root.path, true, false, false, false, false, &duplicate),
               std::exception);
}

TEST(RetainedIoPolicyCompilerTest,
     EmptyGlobalAdditionPreservesExistingEmptyFixtureBytes) {
  Root ordinary_root, empty_root;
  const auto empty = V(V::Array{});
  const auto ordinary =
      d::test::materialize_boolean_io_retained_fixture(ordinary_root.path);
  const auto explicit_empty = d::test::materialize_boolean_io_retained_fixture(
      empty_root.path, true, false, false, false, false, &empty);
  EXPECT_EQ(ordinary.bundle->canonical_bytes(),
            explicit_empty.bundle->canonical_bytes());
  EXPECT_EQ(ordinary.bundle->normalized_root().find_object_member(
                "interrupt_policy_configuration"),
            nullptr);
  const auto compiled =
      d::compile_retained_io_policies(nullptr, nullptr, *ordinary.bundle);
  ASSERT_TRUE(compiled.ok());
  EXPECT_TRUE(compiled.value().activation_policies.empty());
}

V interrupt_key_program(V authored, std::string type,
                        const std::optional<V> &initial) {
  auto blackboard = *authored.find_object_member("blackboard");
  auto keys = blackboard.find_object_member("keys")->as_array();
  for (auto &key : keys) {
    if (key.find_object_member("key")->as_string() != "ready")
      continue;
    auto fields = key.as_object();
    std::erase_if(fields,
                  [](const auto &field) { return field.first == "initial"; });
    key = replace(V(std::move(fields)), "type", V(std::move(type)));
    if (initial) {
      fields = key.as_object();
      fields.emplace_back("initial", *initial);
      key = V(std::move(fields));
    }
  }
  authored = replace(authored, "blackboard",
                     replace(blackboard, "keys", V(std::move(keys))));
  authored = d::normalize_program_authoring(authored);
  return replace(authored, "program_revision",
                 V(d::detail::compute_content_identity(
                     authored, {"program-revision", "manyforge.program.v3",
                                "program_revision"})));
}
V initial_key_condition(const V &value) {
  return V(V::Object{
      {"compare", V(V::Object{{"operand", V(V::Object{{"key", V("ready")}})},
                              {"op", V("==")},
                              {"value", value}})}});
}

TEST(RetainedIoPolicyCompilerTest,
     GlobalInitialRequirementUsesActualQualifiedDeclarations) {
  for (const auto &[type, initial] :
       {std::pair{std::string("bool"), V(false)},
        std::pair{std::string("int64"), V(0.0)},
        std::pair{std::string("float64"), V(0.0)}}) {
    SCOPED_TRACE(type);
    Root root;
    auto rules = V(
        V::Array{replace(rule(), "condition", initial_key_condition(initial))});
    const auto fixture = d::test::materialize_boolean_io_retained_fixture(
        root.path, true, false, false, false, false, &rules);
    for (const bool declared : {false, true}) {
      SCOPED_TRACE(declared);
      const auto authored = interrupt_key_program(
          program_root(*fixture.bundle, false), type,
          declared ? std::optional<V>(initial) : std::nullopt);
      const auto selected =
          select(root.path, *fixture.bundle, false, true, &authored);
      const auto &keys = selected.program->blackboard_key_declarations();
      const auto key =
          std::find_if(keys.begin(), keys.end(),
                       [](const auto &row) { return row.name == "ready"; });
      ASSERT_NE(key, keys.end());
      EXPECT_EQ(key->has_declared_initial, declared);
      const auto compiled = d::compile_retained_io_policies(
          *selected.program, *selected.binding, *fixture.bundle);
      if (declared) {
        ASSERT_TRUE(compiled.ok()) << compiled.error().message;
        ASSERT_EQ(compiled.value().referenced_deployment_blackboard_keys.size(),
                  1U);
      } else {
        ASSERT_FALSE(compiled.ok());
        EXPECT_EQ(compiled.error().details.at("reason"),
                  "interrupt_key_initial_required");
      }
    }
  }
}

TEST(RetainedIoPolicyCompilerTest,
     NodeInitialRequirementUsesSameQualifiedCompiler) {
  Root root;
  auto config = configuration(rule());
  config = replace(config, "interrupt_rules", V(V::Array{}));
  const auto fixture = arm(root.path, config);
  for (const auto &[type, initial] :
       {std::pair{std::string("bool"), V(false)},
        std::pair{std::string("int64"), V(0.0)}}) {
    SCOPED_TRACE(type);
    auto authored = program_root(*fixture.bundle, true);
    auto tree =
        parse(R"({"node_id":"motion.move_until_signal","instance_id":"move",
      "execution_groups":["arm_group"],"children":[],
      "parameters":{"target":{"component_id":"lite6_robot","command_target_id":"arm"},"base_frame":{"component_id":"lite6_robot","frame_id":"base"},"tcp_endpoint":{"kind":"current"},"stop_mode":"soft_on_path","priority":0,
        "activation_mode":"require_inactive_at_arm","on_signal":"success","on_no_signal":"failure",
        "goal":{"frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_robot","frame_id":"base"}},
          "position_m":[0.1,0.1,0.1],"orientation":{"xyzw":[0,0,0,1]}},
        "interrupt_condition":{"compare":{"operand":{"key":"ready"},"op":"==","value":false}}}})");
    tree =
        replace(tree, "parameters",
                replace(*tree.find_object_member("parameters"),
                        "interrupt_condition", initial_key_condition(initial)));
    authored = replace(authored, "tree", tree);
    for (const bool declared : {false, true}) {
      SCOPED_TRACE(declared);
      const auto actual = interrupt_key_program(
          authored, type, declared ? std::optional<V>(initial) : std::nullopt);
      const auto selected =
          select(root.path, *fixture.bundle, true, true, &actual);
      const auto compiled = d::compile_program_io_conditions(
          *selected.program, *selected.binding, *fixture.bundle);
      if (declared) {
        ASSERT_TRUE(compiled.ok()) << compiled.error().message;
        ASSERT_EQ(compiled.value().program_node_conditions.size(), 1U);
      } else {
        ASSERT_FALSE(compiled.ok());
        EXPECT_EQ(compiled.error().details.at("reason"),
                  "interrupt_key_initial_required");
      }
    }
  }
}
TEST(RetainedIoPolicyCompilerTest,
     GlobalRuleStateOperandsRefuseWithOrWithoutAnActualProgram) {
  for (const bool actual : {true, false}) {
    Root root;
    auto selected_rule = rule();
    selected_rule = replace(
        selected_rule, "condition",
        parse(
            actual
                ? R"({"compare":{"operand":{"key":"interrupt_state.guard"},"op":"==","value":"clear"}})"
                : R"({"compare":{"operand":{"key":"interrupt_state.foreign"},"op":"==","value":"clear"}})"));
    const auto rules = V(V::Array{selected_rule});
    const auto fixture = d::test::materialize_boolean_io_retained_fixture(
        root.path, true, false, false, false, false, &rules);
    const auto selected = select(root.path, *fixture.bundle, false, false);
    const auto &declarations = selected.program->blackboard_key_declarations();
    const auto system = std::find_if(
        declarations.begin(), declarations.end(),
        [](const auto &key) { return key.name == "interrupt_state.guard"; });
    ASSERT_NE(system, declarations.end());
    EXPECT_FALSE(system->has_declared_initial);
    const auto compiled = d::compile_retained_io_policies(
        *selected.program, *selected.binding, *fixture.bundle);
    ASSERT_FALSE(compiled.ok());
    EXPECT_EQ(compiled.error().details.at("reason"),
              "global_rule_state_operand_unsupported");
    EXPECT_EQ(compiled.error().details.at("key"),
              actual ? "interrupt_state.guard" : "interrupt_state.foreign");
    const auto no_program =
        d::compile_retained_io_policies(nullptr, nullptr, *fixture.bundle);
    ASSERT_FALSE(no_program.ok());
    EXPECT_EQ(no_program.error().details.at("reason"),
              "global_rule_state_operand_unsupported");
  }
}

TEST(
    RetainedIoPolicyCompilerTest,
    NodeOutcomeNoneCompilesForTheActualSealedMotionInstanceWithoutUserInitial) {
  Root root;
  ASSERT_TRUE(std::filesystem::create_directory(root.path / "programs"));
  const auto fixture = d::test::LoadMaintainedInterruptProgram(
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT, root.path / "programs");
  auto authored = fixture.program->normalized_program_root();
  auto tree = *authored.find_object_member("tree");
  auto parameters = *tree.find_object_member("parameters");
  parameters = replace(
      parameters, "interrupt_condition",
      parse(
          R"({"compare":{"operand":{"key":"node_outcome.move"},"op":"==","value":"none"}})"));
  authored = replace(authored, "tree", replace(tree, "parameters", parameters));
  authored =
      d::test::with_program_revision(d::normalize_program_authoring(authored));
  const auto selected = select(MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT,
                               *fixture.bundle, true, true, &authored);
  const auto compiled = d::compile_program_io_conditions(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().program_node_conditions.size(), 1U);
  ASSERT_EQ(compiled.value().referenced_program_blackboard_keys.size(), 1U);
  EXPECT_EQ(compiled.value().referenced_program_blackboard_keys.front().name,
            "node_outcome.move");
  const auto &declarations = selected.program->blackboard_key_declarations();
  const auto system = std::find_if(
      declarations.begin(), declarations.end(),
      [](const auto &key) { return key.name == "node_outcome.move"; });
  ASSERT_NE(system, declarations.end());
  EXPECT_FALSE(system->has_declared_initial);
  EXPECT_EQ(selected.program->normalized_program_root(), authored);
}

TEST(
    RetainedIoPolicyCompilerTest,
    KnownProgramMissingGlobalUserKeyNamesRuleAndExactOperandWithoutBypassingInitial) {
  Root root;
  auto selected_rule = rule();
  selected_rule = replace(
      selected_rule, "condition",
      parse(
          R"({"all":[{"compare":{"operand":{"key":"ready"},"op":"==","value":false}},{"not":{"compare":{"operand":{"key":"count"},"op":"==","value":0}}}]})"));
  const auto fixture = logical_arm(root.path, configuration(selected_rule));
  const auto selected = select(root.path, *fixture.bundle);
  const auto missing = d::compile_retained_io_policies(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().details.at("reason"), "global_rule_key_undeclared");
  EXPECT_EQ(missing.error().details.at("rule_id"), "guard");
  EXPECT_EQ(missing.error().details.at("key"), "count");
  EXPECT_EQ(missing.error().details.at("condition_reason"),
            "interrupt_condition_key_refused");
  // Without a Program, the entire rule waits without any inferred key kind.
  const auto absent =
      d::compile_retained_io_policies(nullptr, nullptr, *fixture.bundle);
  ASSERT_TRUE(absent.ok()) << absent.error().message;
  ASSERT_EQ(absent.value().activation_policies.size(), 1U);
  EXPECT_EQ(absent.value().activation_policies.front().condition, nullptr);
  EXPECT_EQ(absent.value().activation_policies.front().waiting_for_program_keys,
            (std::vector<std::string>{"count", "ready"}));
  EXPECT_TRUE(absent.value().referenced_deployment_blackboard_keys.empty());

  for (const bool initialized : {false, true}) {
    auto authored = program_root(*fixture.bundle, false);
    auto blackboard = *authored.find_object_member("blackboard");
    auto keys = blackboard.find_object_member("keys")->as_array();
    auto count = parse(
        R"({"key":"count","type":"int64","access":"read_write","persistence":"run"})");
    if (initialized)
      count = replace(count, "initial", V(0.0));
    keys.push_back(std::move(count));
    std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
      return a.find_object_member("key")->as_string() <
             b.find_object_member("key")->as_string();
    });
    authored =
        replace(authored, "blackboard", replace(blackboard, "keys", V(keys)));
    authored = d::test::with_program_revision(
        d::normalize_program_authoring(authored));
    const auto actual =
        select(root.path, *fixture.bundle, false, true, &authored);
    const auto compiled = d::compile_retained_io_policies(
        *actual.program, *actual.binding, *fixture.bundle);
    if (initialized) {
      ASSERT_TRUE(compiled.ok()) << compiled.error().message;
      ASSERT_EQ(compiled.value().referenced_deployment_blackboard_keys.size(),
                2U);
      EXPECT_EQ(compiled.value().referenced_deployment_blackboard_keys[0].name,
                "count");
      EXPECT_EQ(
          compiled.value().referenced_deployment_blackboard_keys[0].value_kind,
          c::IoConditionValueKind::kInt64);
    } else {
      ASSERT_FALSE(compiled.ok());
      EXPECT_EQ(compiled.error().details.at("reason"),
                "interrupt_key_initial_required");
    }
  }
}

TEST(RetainedIoPolicyCompilerTest,
     ProgramChecksAndWaitsKeepActualRuleStateReadsUnder105) {
  Root root;
  const auto fixture = logical_arm(root.path, configuration(rule()));
  for (const auto *node_id : {"logic.check_value", "logic.wait_for_value"}) {
    auto authored = program_root(*fixture.bundle, false);
    auto tree =
        parse(R"({"node_id":"logic.check_value","instance_id":"read_state",
        "parameters":{"key":"interrupt_state.guard","value":"clear"},
        "children":[]})");
    tree = replace(tree, "node_id", V(node_id));
    authored = replace(authored, "tree", tree);
    authored = replace(authored, "execution_groups", V(V::Object{}));
    authored = d::test::with_program_revision(
        d::normalize_program_authoring(authored));
    const auto selected =
        select(root.path, *fixture.bundle, false, true, &authored);
    const auto global = d::compile_retained_io_policies(
        *selected.program, *selected.binding, *fixture.bundle);
    ASSERT_TRUE(global.ok()) << global.error().message;
    const auto nodes = d::compile_program_io_conditions(
        *selected.program, *selected.binding, *fixture.bundle);
    ASSERT_TRUE(nodes.ok()) << nodes.error().message;
    EXPECT_TRUE(nodes.value().program_node_conditions.empty());
    EXPECT_EQ(selected.program->normalized_program_root(), authored);
  }
}

TEST(
    RetainedIoPolicyCompilerTest,
    MixedGlobalWaitsWholeThenUsesActualAppliedDeclarationsWhileSignalSiblingStaysChecked) {
  Root root;
  auto mixed = rule();
  mixed = replace(mixed, "condition", parse(R"({"any":[
    {"compare":{"operand":{"signal":{"controllable_component_id":"device.io","signal_name":"ready"}},"op":"==","value":false}},
    {"compare":{"operand":{"key":"ready"},"op":"==","value":false}}]})"));
  auto sibling =
      replace(rule(V("all_motion"), true), "rule_id", V("independent"));
  const auto rules = V(V::Array{mixed, sibling});
  const auto fixture = d::test::materialize_boolean_io_retained_fixture(
      root.path, true, false, false, false, false, &rules);
  const auto waiting =
      d::compile_retained_io_policies(nullptr, nullptr, *fixture.bundle);
  ASSERT_TRUE(waiting.ok()) << waiting.error().message;
  ASSERT_EQ(waiting.value().activation_policies.size(), 2U);
  const auto &guard = waiting.value().activation_policies[0];
  const auto &independent = waiting.value().activation_policies[1];
  ASSERT_EQ(guard.policy_id, c::InterruptPolicyId("guard"));
  ASSERT_EQ(independent.policy_id, c::InterruptPolicyId("independent"));
  EXPECT_EQ(guard.condition, nullptr);
  EXPECT_EQ(guard.waiting_for_program_keys,
            (std::vector<std::string>{"ready"}));
  EXPECT_EQ(
      guard.required_signal_sources,
      (std::vector<c::InterruptSourceId>{c::InterruptSourceId("source.io")}));
  EXPECT_NE(independent.condition, nullptr);
  EXPECT_TRUE(independent.waiting_for_program_keys.empty());
  EXPECT_TRUE(waiting.value().referenced_deployment_blackboard_keys.empty());

  const auto selected = select(root.path, *fixture.bundle);
  const auto complete = d::compile_retained_io_policies(
      *selected.program, *selected.binding, *fixture.bundle);
  ASSERT_TRUE(complete.ok()) << complete.error().message;
  ASSERT_EQ(complete.value().activation_policies.size(), 2U);
  for (const auto &policy : complete.value().activation_policies) {
    EXPECT_NE(policy.condition, nullptr);
    EXPECT_TRUE(policy.waiting_for_program_keys.empty());
  }
  ASSERT_EQ(complete.value().referenced_deployment_blackboard_keys.size(), 1U);
  EXPECT_EQ(complete.value().referenced_deployment_blackboard_keys.front().name,
            "ready");
  EXPECT_EQ(
      complete.value().referenced_deployment_blackboard_keys.front().value_kind,
      c::IoConditionValueKind::kBoolean);
  const auto missing = select(root.path, *fixture.bundle, false, false);
  const auto refused = d::compile_retained_io_policies(
      *missing.program, *missing.binding, *fixture.bundle);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "global_rule_key_undeclared");
  EXPECT_EQ(refused.error().details.at("rule_id"), "guard");
  EXPECT_EQ(refused.error().details.at("key"), "ready");
}

TEST(RetainedIoPolicyCompilerTest,
     ActualTrialMotionConditionsBindOnlyTheirQualifiedDirectInputs) {
  Root root;
  const auto fixture = d::test::MaterializeMaintainedInterruptDeployment(
      root.path, parse(R"({"interrupt_rules":[]})"),
      "examples/mock_lite6_signal_moves.scenario");
  const auto &bundle = *fixture.bundle;
  ASSERT_TRUE(fixture.model_map.has_value());
  auto authored = program_root(bundle, true);
  auto node = parse(R"({"node_id":"motion.move_until_signal","instance_id":"search_trip",
    "children":[],"parameters":{
      "target":{"component_id":"lite6_robot","command_target_id":"arm"},
      "base_frame":{"component_id":"lite6_robot","frame_id":"base"},
      "tcp_endpoint":{"kind":"current"},"stop_mode":"soft_on_path",
      "activation_mode":"require_inactive_at_arm","on_signal":"success","on_no_signal":"failure",
      "goal":{"frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_robot","frame_id":"base"}},
        "position_m":[0.1,0.1,0.1],"orientation":{"xyzw":[0,0,0,1]}},
      "interrupt_condition":{"all":[
        {"compare":{"operand":{"signal":{"component_id":"station_io","signal_name":"probe_contact"}},"op":"==","value":true}},
        {"not":{"compare":{"operand":{"signal":{"component_id":"station_io","signal_name":"edge_sensor"}},"op":"==","value":true}}}]}}})");
  auto second = replace(node, "instance_id", V("search_no_trip"));
  auto second_parameters = *second.find_object_member("parameters");
  second_parameters = replace(second_parameters, "interrupt_condition", parse(R"({
    "compare":{"operand":{"signal":{"component_id":"station_io","signal_name":"edge_sensor"}},"op":"==","value":true}})"));
  second = replace(second, "parameters", second_parameters);
  auto tree = parse(R"({"node_id":"sequence","instance_id":"signal_trial",
    "execution_groups":["arm_group"],"parameters":{},"children":[]})");
  tree = replace(tree, "children", V(V::Array{node, second}));
  authored = replace(authored, "tree", tree);
  const auto seal = [](V value) {
    return d::test::with_program_revision(d::normalize_program_authoring(value));
  };
  d::FilesystemCatalogArtifactStore catalogs(root.path);
  const auto program = d::verify_program_binding_contract(d::canonical_json(seal(authored)), bundle,
                                                            *fixture.model_map, catalogs);
  const auto binding = d::compile_program_binding(*program, bundle, *fixture.model_map, catalogs);
  ASSERT_NE(binding, nullptr);
  const auto &signals = binding->value().signal_bindings;
  for (const auto *name : {"edge_sensor", "probe_contact"}) {
    const auto found = std::find_if(signals.begin(), signals.end(), [&](const auto &row) {
      return row.reference == c::ProgramSignalReferenceDTO{c::ControllableComponentId("station_io"), name};
    });
    ASSERT_NE(found, signals.end());
    EXPECT_EQ(found->direction, c::IoSignalDirection::kInput);
    EXPECT_EQ(found->value_type, c::IoSignalValueType::kBoolean);
  }
  const auto checked = d::compile_program_io_conditions(*program, *binding, bundle);
  ASSERT_TRUE(checked.ok()) << checked.error().message;
  ASSERT_EQ(checked.value().program_node_conditions.size(), 2U);
  const auto find_node = [&](std::string_view id) {
    return std::find_if(checked.value().program_node_conditions.begin(),
        checked.value().program_node_conditions.end(),
        [&](const auto &row) { return row.node_instance_id.value() == id; });
  };
  const auto trip = find_node("search_trip");
  const auto no_trip = find_node("search_no_trip");
  ASSERT_NE(trip, checked.value().program_node_conditions.end());
  ASSERT_NE(no_trip, checked.value().program_node_conditions.end());
  EXPECT_EQ(trip->required_signal_sources, (std::vector<c::InterruptSourceId>{
      c::InterruptSourceId("station_io.edge_sensor"), c::InterruptSourceId("station_io.probe_contact")}));
  EXPECT_EQ(no_trip->required_signal_sources, (std::vector<c::InterruptSourceId>{
      c::InterruptSourceId("station_io.edge_sensor")}));

  // An unknown direct signal and an output cannot be admitted as inputs.
  for (const auto *name : {"not_present", "lamp"}) {
    auto bad_parameters = *node.find_object_member("parameters");
    const V reference(V::Object{{"component_id", V("station_io")}, {"signal_name", V(name)}});
    const V bad_condition(V::Object{{"compare", V(V::Object{
        {"operand", V(V::Object{{"signal", reference}})}, {"op", V("==")}, {"value", V(true)}})}});
    bad_parameters = replace(bad_parameters, "interrupt_condition", bad_condition);
    const auto bad_node = replace(node, "parameters", bad_parameters);
    const auto bad_tree = replace(tree, "children", V(V::Array{bad_node, second}));
    const auto bytes = d::canonical_json(seal(replace(authored, "tree", bad_tree)));
    try {
      (void)d::verify_program_binding_contract(bytes, bundle, *fixture.model_map, catalogs);
      FAIL() << "invalid input reference must be refused before binding";
    } catch (const d::ProgramReferenceValidationRefusal &error) {
      const auto expected = std::string_view(name) == "not_present"
                                ? "signal_missing" : "signal_direction_mismatch";
      EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
                              [&](const auto &diagnostic) { return diagnostic.code == expected; }));
    }
  }
}

} // namespace
