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

// Real v3 authored nodes: repeated commands reference the same physical
// target or signal directly; no Program roles or synthetic assignment exist.
inline std::shared_ptr<const ProgramBindingContract> make_pneumatic_program_contract(
    const detail::PreparedActivationMaterialization &product, const std::filesystem::path &root,
    unsigned grippers = 1U,
    unsigned valve_uses = 0U, std::string_view gripper_target = "gripper",
    std::string_view gripper_component = "lite_gripper") {
  using V = CanonicalValue;
  using namespace maintained_activation_fixture_detail;
  ProgramOptions options;
  options.catalog_content_version = product.bundle->value().catalogs.node_environment.content_version;
  auto authored = make_program_root(options);
  V::Array children;
  for (unsigned i = 0; i < grippers; ++i) {
    children.emplace_back(V::Object{
        {"node_id", V("gripper.command_position")},
        {"instance_id", V("close_" + std::to_string(i))},
        {"parameters", V(V::Object{
          {"target", V(V::Object{{"component_id", V(std::string(gripper_component))},
                                  {"command_target_id", V(std::string(gripper_target))}})},
          {"position", V(V::Object{{"kind", V("named_preset")}, {"name", V("closed")}})},
          {"required_inputs", V(V::Array{V(V::Object{{"input", V("workpiece_grasped")},
                                                     {"value", V(true)}})})}})},
        {"children", V(V::Array{})}});
  }
  for (unsigned i = 0; i < valve_uses; ++i) {
    children.emplace_back(V::Object{
        {"node_id", V("io.set_output")},
        {"instance_id", V("write_" + std::to_string(i))},
        {"parameters", parse_strict_json_model_yaml(R"({
          "signal":{"component_id":"gripper_io","signal_name":"valve"},
          "value":false})").root()},
        {"children", V(V::Array{})}});
  }
  authored = With(std::move(authored), "tree", V(V::Object{
      {"node_id", V("sequence")}, {"instance_id", V("root")},
      {"parameters", V(V::Object{})}, {"children", V(std::move(children))}}));
  authored = with_program_revision(normalize_program_authoring(authored));
  if (!product.model_map) throw std::logic_error("pneumatic Program requires retained model map");
  FilesystemCatalogArtifactStore catalogs(root);
  return verify_program_binding_contract(canonical_json(authored), *product.bundle, *product.model_map, catalogs);
}

inline PneumaticProgramFixture compile_pneumatic_program_fixture(
    const std::filesystem::path &root, const detail::PreparedActivationMaterialization &product,
    unsigned grippers = 1U, unsigned valve_uses = 0U) {
  auto program = make_pneumatic_program_contract(product, root, grippers, valve_uses);
  FilesystemCatalogArtifactStore catalogs(root);
  auto binding = compile_program_binding(*program, *product.bundle, *product.model_map, catalogs);
  return {std::move(program), std::move(binding)};
}

} // namespace manyforge::deployment::test
