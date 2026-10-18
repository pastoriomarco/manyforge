#pragma once

#include "program_root_fixture.hpp"

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::deployment::test {

/// The package is produced by the existing ProgramCapturePackage CTest setup
/// using the normal maintained --check producer. Programs are authored here,
/// compiled against that package, then reloaded through both public
/// repositories.
struct MaintainedInterruptProgramFixture final {
  std::filesystem::path package_root;
  std::filesystem::path program_artifact_root;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> binding;
};

[[nodiscard]] inline MaintainedInterruptProgramFixture
LoadMaintainedInterruptProgram(
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root) {
  namespace d = deployment;
  namespace c = core;
  using V = d::CanonicalValue;
  const auto bundles = package_root / "retained-bundles" / "sha256";
  if (!std::filesystem::is_directory(bundles))
    throw std::logic_error("ProgramCapturePackage must produce the maintained "
                           "package before this test");
  d::FilesystemVerifiedRetainedBundleSource source(package_root);
  std::shared_ptr<const d::RetainedBundle> bundle;
  for (const auto &entry : std::filesystem::directory_iterator(bundles)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json")
      continue;
    const auto candidate = source.get_verified(
        c::RetainedBundleId("sha256:" + entry.path().stem().string()));
    if (!candidate ||
        candidate->value().cell_graph.cell_id !=
            c::CellId("lite6_pick_place_cell") ||
        candidate->value().deployment_artifact_id.value() !=
            "lite6_lite_gripper_pick_and_place")
      continue;
    if (bundle)
      throw std::logic_error(
          "maintained package has duplicate Lite6 motion bundles");
    bundle = candidate;
  }
  if (!bundle ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U)
    throw std::logic_error(
        "maintained Lite6 bundle and sole model map are required");
  d::FilesystemVerifiedRetainedModelMapSource maps(package_root);
  auto map = maps.get_verified(
      {bundle->retained_bundle_id(),
       bundle->value().compiled_products.symbol_role_maps.front().product_id});
  if (!map)
    throw std::logic_error("maintained Lite6 model map could not be verified");

  d::test::ProgramOptions options;
  options.catalog_content_version =
      bundle->value().catalogs.node_environment.content_version;
  options.resource_roles = {"arm_joints"};
  options.capability_ids = {"execution.feedback.phase_events",
                            "execution.trajectory.execute",
                            "planning.motion.pose_goal",
                            "planning.profile.balanced",
                            "planning.trajectory.time_parameterized",
                            "state.observed.current",
                            "validation.trajectory.non_collision"};
  auto fields = d::test::make_program_root(options).as_object();
  const auto replace = [&](const std::string &name, V value) {
    for (auto &[key, selected] : fields)
      if (key == name) {
        selected = std::move(value);
        return;
      }
    fields.emplace_back(name, std::move(value));
  };
  replace("program_id", V("maintained_interrupt_fixture"));
  replace("blackboard", d::parse_strict_json_model_yaml(R"({
    "namespaces":[],"keys":[{"key":"ready","type":"bool",
      "access":"read_write","persistence":"run","initial":false}]})")
                            .root());
  replace("tree", d::parse_strict_json_model_yaml(R"({
    "node_id":"motion.move_until_signal","instance_id":"move","children":[],
    "parameters":{"target_role":"arm","stop_mode":"soft_on_path","priority":0,
      "activation_mode":"require_inactive_at_arm","on_signal":"success","on_no_signal":"failure",
      "goal":{"frame_ref":{"kind":"target_frame","target_role":"arm","frame_role":"base"},
        "position_m":[0.1,0.1,0.1],"orientation":{"xyzw":[0,0,0,1]}},
      "interrupt_condition":{"compare":{"operand":{"key":"ready"},"op":"==","value":true}}}})")
                      .root());
  const auto authored = d::test::with_program_revision(
      d::normalize_program_authoring(V(std::move(fields))));
  const auto program =
      d::verify_program_binding_contract(d::canonical_json(authored), *bundle);
  d::FilesystemCatalogArtifactStore artifacts(package_root);
  const c::ProgramRoleAssignmentDTO assignment{
      {{c::ComponentRoleId("robot"),
        c::ControllableComponentId("lite6_robot")}},
      {{c::TargetRoleId("arm"), c::CommandTargetId("arm")}}};
  const auto binding = d::compile_program_binding(*program, assignment, *bundle,
                                                  *map, artifacts);

  d::FilesystemProgramRepository programs(program_artifact_root);
  (void)programs.put_if_absent(program, *bundle);
  const auto read_program = programs.get_verified(
      program->value().program_id, program->value().program_revision, *bundle);
  d::FilesystemProgramBindingRepository bindings(program_artifact_root);
  (void)bindings.put_if_absent(binding, *bundle, *map);
  const auto read_binding =
      bindings.get_verified(binding->program_binding_id(), *bundle, *map);
  if (!read_program || !read_binding ||
      read_program->canonical_program_bytes() !=
          program->canonical_program_bytes() ||
      read_binding->canonical_bytes() != binding->canonical_bytes())
    throw std::logic_error(
        "maintained interrupt Program pair did not reload exactly");
  return {package_root,    program_artifact_root, std::move(bundle),
          std::move(*map), read_program,          read_binding};
}

} // namespace manyforge::deployment::test
