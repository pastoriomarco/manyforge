#pragma once

#include "maintained_activation_fixture.hpp"
#include "program_binding_internal.hpp"
#include "program_root_fixture.hpp"
#include <manyforge_deployment/program_binding_compiler.hpp>

namespace manyforge::deployment::test {

struct PneumaticProgramFixture final {
  std::shared_ptr<const ProgramBindingContract> program;
  std::shared_ptr<const ProgramBinding> binding;
};

// Exercise the maintained Mock Scenario's actual model plus its separate I/O
// component. Additional authored aliases make physical-signal reuse explicit.
inline PneumaticProgramFixture compile_pneumatic_program_fixture(
    const std::filesystem::path &root, const detail::PreparedActivationMaterialization &product,
    unsigned grippers = 1U, unsigned valve_aliases = 0U) {
  using V = CanonicalValue;
  using namespace maintained_activation_fixture_detail;
  const auto source = parse_strict_json_model_yaml(Read(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT) /
      "examples/lite6_pneumatic_mock.scenario/program.yaml")).root();
  ProgramOptions options;
  options.catalog_content_version = product.bundle->value().catalogs.node_environment.content_version;
  options.logic_only = true;
  auto authored = make_program_root(options);
  V::Object components{{"tool", V(V::Object{})}};
  if (valve_aliases) components.emplace_back("device", V(V::Object{}));
  V::Object targets;
  V::Array children;
  core::ProgramRoleAssignmentDTO assignment;
  // The gripper is its own component since the M6 tool split.
  assignment.component_roles.push_back({core::ComponentRoleId("tool"),
                                        core::ControllableComponentId("lite_gripper")});
  const auto &gripper_target = Member(Member(source, "target_roles"), "gripper");
  for (unsigned i = 0; i < grippers; ++i) {
    const auto name = "gripper_" + std::to_string(i);
    targets.emplace_back(name, gripper_target);
    assignment.target_roles.push_back({core::TargetRoleId(name), core::CommandTargetId("gripper")});
    children.emplace_back(V::Object{
        {"node_id", V("gripper.command_position")}, {"instance_id", V("close_" + std::to_string(i))},
        {"parameters", parse_strict_json_model_yaml(
            "{\"target_role\":\"" + name + "\",\"position\":{\"kind\":\"named_preset\",\"name\":\"closed\"},"
            "\"required_inputs\":[{\"input\":\"workpiece_grasped\",\"value\":true}]}").root()},
        {"children", V(V::Array{})}});
  }
  if (valve_aliases)
    assignment.component_roles.push_back({core::ComponentRoleId("device"),
                                          core::ControllableComponentId("gripper_io")});
  for (unsigned i = 0; i < valve_aliases; ++i) {
    const auto name = "valve_" + std::to_string(i);
    targets.emplace_back(name, parse_strict_json_model_yaml(R"({
      "component_role":"device","required_capabilities":[{"capability_id":"io.gpio.set_output",
      "parameters":{},"disposition":"required"}],"required_frame_roles":[],
      "required_resource_roles":["valve"],"required_collision_body_roles":[],"required_model_roles":[]})").root());
    assignment.target_roles.push_back({core::TargetRoleId(name), core::CommandTargetId("io")});
    children.emplace_back(V::Object{
        {"node_id", V("io.set_output")}, {"instance_id", V("write_" + std::to_string(i))},
        {"parameters", parse_strict_json_model_yaml(
            "{\"target_role\":\"" + name + "\",\"resource_role\":\"valve\",\"value\":false}").root()},
        {"children", V(V::Array{})}});
  }
  authored = With(std::move(authored), "component_roles", V(std::move(components)));
  authored = With(std::move(authored), "target_roles", V(std::move(targets)));
  authored = With(std::move(authored), "tree", V(V::Object{
      {"node_id", V("sequence")}, {"instance_id", V("root")},
      {"parameters", V(V::Object{})}, {"children", V(std::move(children))}}));
  authored = with_program_revision(normalize_program_authoring(authored));
  auto program = verify_program_binding_contract(canonical_json(authored), *product.bundle);
  FilesystemCatalogArtifactStore catalogs(root);
  auto binding = compile_program_binding(*program, assignment, *product.bundle, *product.model_map, catalogs);
  return {std::move(program), std::move(binding)};
}

} // namespace manyforge::deployment::test
