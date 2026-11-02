#include <algorithm>
#include <array>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "build_manifest_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "maintained_activation_fixture.hpp"
#include "program_root_fixture.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace detail = manyforge::deployment::detail;
using deployment::CanonicalValue;
using deployment::test::maintained_activation_fixture_detail::Member;
using deployment::test::maintained_activation_fixture_detail::Read;
using deployment::test::maintained_activation_fixture_detail::With;

/// One example Scenario cell and the single role pair its Program binds.
/// The role declarations are read from the Scenario's own program.yaml.
struct Profile final {
  std::string_view scenario;
  void (*authored_mapper)(CanonicalValue::Object &);
  std::string_view component_role;
  std::string_view component_id;
  std::string_view target_role;
  std::string_view command_target_id;
  bool initial_scene;
};

/// A cell holding only one of the Scenario's tools, placed in the cell
/// instead of mounted: every other component, mount and assistant goes.
void KeepOnlyTool(CanonicalValue::Object &root, const std::string_view tool) {
  // The cell-wide execution settings the removed robot carried (the
  // compiler requires them on the runtime unit; Composer fills them per robot).
  constexpr std::array<std::string_view, 3> kCellSettings{
      "execution_tick_hz", "collision_monitor_period_ms", "planner_rng_seed"};
  CanonicalValue::Object cell_settings;
  for (const auto &[name, value] : root) {
    if (name != "components") continue;
    for (const auto &row : value.as_array()) {
      const auto *configuration = row.find_object_member("configuration");
      const auto *runtime = configuration == nullptr
                                ? nullptr
                                : configuration->find_object_member("runtime_configuration");
      if (runtime == nullptr || !runtime->is_object()) continue;
      for (const auto &[key, setting] : runtime->as_object())
        if (std::find(kCellSettings.begin(), kCellSettings.end(), key) != kCellSettings.end())
          cell_settings.emplace_back(key, setting);
      if (!cell_settings.empty()) break;
    }
  }
  // The tool-only cell has no assistant.
  std::erase_if(root, [](const auto &member) {
    return member.first == "assistant_model_components";
  });
  // A ROS runtime unit always carries its (here empty) arm start positions.
  cell_settings.emplace_back("initial_joint_positions_rad",
                             CanonicalValue(CanonicalValue::Array{}));
  for (auto &[name, value] : root) {
    if (name == "attachments") {
      value = CanonicalValue(CanonicalValue::Array{});
    } else if (name == "assistant_profile_id") {
      value = CanonicalValue(nullptr);
    } else if (name == "components") {
      CanonicalValue::Array kept;
      for (const auto &row : value.as_array()) {
        if (Member(row, "component_id").as_string() != tool) continue;
        auto placed = With(row, "root_pose_in_cell",
                           deployment::parse_strict_json_model_yaml(
                               R"({"position_m":[0.0,0.0,0.0],"orientation_xyzw":[0.0,0.0,0.0,1.0]})")
                               .root());
        auto configuration = Member(placed, "configuration").as_object();
        for (auto &[key, field] : configuration) {
          if (key != "runtime_configuration") continue;
          auto runtime = field.as_object();
          for (const auto &[setting, setting_value] : cell_settings)
            if (std::none_of(runtime.begin(), runtime.end(),
                             [&](const auto &member) { return member.first == setting; }))
              runtime.emplace_back(setting, setting_value);
          field = CanonicalValue(std::move(runtime));
        }
        kept.push_back(With(placed, "configuration", CanonicalValue(std::move(configuration))));
      }
      value = CanonicalValue(std::move(kept));
    }
  }
}
void KeepOnlyLiteGripper(CanonicalValue::Object &root) {
  KeepOnlyTool(root, "lite_gripper");
}
void KeepOnlyRobotiq(CanonicalValue::Object &root) {
  KeepOnlyTool(root, "robotiq_2f_85");
}

constexpr std::string_view kUsage =
    "usage: generate_manager_apply_package --package-root <path> "
    "--metadata <path> --profile ros2-control|assistant-ros2-control|"
    "gripper-pneumatic-commanded|gripper-only-robotiq-position";

[[nodiscard]] Profile SelectProfile(const std::string_view name) {
  // The Lite 6 machine (owner `lite6`, arm member `lite6_robot`) with its
  // mounted pneumatic Lite gripper.
  if (name == "ros2-control")
    return {"examples/mock_lite6_pick_and_place.scenario", nullptr, "robot",
            "lite6_robot", "arm", "arm", true};
  // The UR10e + Robotiq 2F-140 cell with its scene-authoring assistant.
  if (name == "assistant-ros2-control")
    return {"examples/mock_ur10e_assistant_scene_authoring.scenario", nullptr,
            "robot", "ur10e_robot", "arm", "arm", false};
  // The Lite gripper alone: pneumatic, commanded output, with feedback.
  if (name == "gripper-pneumatic-commanded")
    return {"examples/mock_lite6_pick_and_place.scenario", KeepOnlyLiteGripper,
            "tool", "lite_gripper", "gripper", "gripper", false};
  // The Robotiq 2F-85 alone: a Position gripper with no arm.
  if (name == "gripper-only-robotiq-position")
    return {"examples/mock_ur5_semantic_object_pick.scenario", KeepOnlyRobotiq,
            "tool", "robotiq_2f_85", "gripper", "gripper", false};
  throw std::invalid_argument(std::string(kUsage));
}

/// Store everything one materialized activation produced, including its
/// cell-scoped canonical-empty predecessor when one was prepared.
void Publish(const std::filesystem::path &package_root,
             const detail::PreparedActivationMaterialization &result) {
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  for (const auto &artifact : result.catalog_artifacts)
    static_cast<void>(catalogs.put_if_absent(
        artifact.content_addressed_store_key, artifact.content_digest,
        artifact.bytes));
  deployment::FilesystemRawArtifactStore raw(package_root);
  for (const auto &artifact : result.raw_artifacts)
    static_cast<void>(raw.put_if_absent(artifact.bytes));
  deployment::FilesystemRetainedBundleRepository bundles(package_root);
  static_cast<void>(bundles.put_if_absent(result.bundle));
  if (result.canonical_empty_bundle != nullptr)
    static_cast<void>(bundles.put_if_absent(result.canonical_empty_bundle));
}

struct ProgramPublication final {
  core::ProgramBindingId binding_id;
  core::ProgramRevision program_revision;
  /// The exact authored assignment the binding was compiled from.
  CanonicalValue role_assignment;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
};

/// A timer-only Program declaring the profile's role pair exactly as the
/// Scenario's own Program declares it.
[[nodiscard]] ProgramPublication PublishProgram(
    const std::filesystem::path &package_root,
    const detail::PreparedActivationMaterialization &physical,
    const Profile &profile) {
  using V = CanonicalValue;
  const auto source =
      deployment::parse_strict_json_model_yaml(
          Read(std::filesystem::path(MANYFORGE_WORKSPACE_ROOT) /
               profile.scenario / "program.yaml"))
          .root();
  deployment::test::ProgramOptions options;
  options.catalog_content_version =
      physical.bundle->value().catalogs.node_environment.content_version;
  options.logic_only = true;
  auto authored = deployment::test::make_program_root(options);
  authored = With(std::move(authored), "program_id", V("program_apply_fixture"));
  authored = With(std::move(authored), "name", V("Manager Apply fixture"));
  authored = With(std::move(authored), "component_roles",
                  V(V::Object{{std::string(profile.component_role),
                               Member(Member(source, "component_roles"),
                                      profile.component_role)}}));
  auto target_role = Member(Member(source, "target_roles"), profile.target_role);
  if (profile.target_role == "arm") {
    // Process tests resolve transforms between two of the arm's frame roles;
    // every example arm model names `base` and `flange`.
    target_role = With(std::move(target_role), "required_frame_roles",
                       V(V::Array{V("base"), V("flange")}));
  }
  authored = With(std::move(authored), "target_roles",
                  V(V::Object{{std::string(profile.target_role),
                               std::move(target_role)}}));
  authored = With(std::move(authored), "tree",
                  deployment::parse_strict_json_model_yaml(R"({
        "node_id":"sequence","instance_id":"root","parameters":{},
        "children":[{"node_id":"timer","instance_id":"apply_fixture_timer",
                     "parameters":{"duration_s":0.001},"children":[]}]})")
                      .root());
  if (profile.initial_scene) {
    // Apply never loads a Program's scene; the marker proves it stays out.
    authored = With(std::move(authored), "scene",
                    deployment::parse_strict_json_model_yaml(R"([
        {"scene_resource_id":"apply_fixture_marker",
         "shape":{"type":"box","box_dimensions_m":[0.1,0.1,0.1]},
         "pose":{"frame_ref":{"kind":"cell_frame"},
                 "position_m":[3.0,0.0,0.0],
                 "orientation":{"xyzw":[0.0,0.0,0.0,1.0]}}}])")
                        .root());
  }
  authored = deployment::test::with_program_revision(
      deployment::normalize_program_authoring(authored));
  const auto program = deployment::verify_program_binding_contract(
      deployment::canonical_json(authored), *physical.bundle);
  V role_assignment(V::Object{
      {"component_roles",
       V(V::Array{V(V::Object{
           {"component_role_id", V(std::string(profile.component_role))},
           {"controllable_component_id", V(std::string(profile.component_id))},
       })})},
      {"target_roles",
       V(V::Array{V(V::Object{
           {"command_target_id", V(std::string(profile.command_target_id))},
           {"target_role_id", V(std::string(profile.target_role))},
       })})},
  });
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  const auto binding = deployment::compile_program_binding(
      *program, deployment::parse_program_role_assignment(role_assignment),
      *physical.bundle, *physical.model_map, catalogs);
  static_cast<void>(deployment::FilesystemProgramRepository(package_root)
                        .put_if_absent(program, *physical.bundle));
  static_cast<void>(
      deployment::FilesystemProgramBindingRepository(package_root)
          .put_if_absent(binding, *physical.bundle, *physical.model_map));
  return {binding->program_binding_id(), program->value().program_revision,
          std::move(role_assignment), program};
}

/// Publish one real prepared activation of the physical cell per Scenario ID
/// the process tests submit, and return their Apply summaries by Scenario ID.
/// The Manager admits only stored, reverified prepared values.
[[nodiscard]] CanonicalValue PublishPreparedActivations(
    const std::filesystem::path &package_root,
    const detail::PreparedActivationMaterialization &physical,
    const ProgramPublication &program) {
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  deployment::FilesystemRawArtifactStore raw_artifacts(package_root);
  const deployment::FilesystemPreparedActivationRepository repository(
      package_root);
  CanonicalValue::Object summaries;
  for (const std::string_view scenario_id :
       {"assistant_native_process_fixture", "program_apply_fixture"}) {
    const auto prepared = deployment::assemble_prepared_activation(
        deployment::PreparedActivationAssemblyInput{
            deployment::PreparedScenarioOrigin{
                std::string(scenario_id),
                core::ContentDigest("sha256:" + std::string(64U, 'e')),
                "/fixtures/" + std::string(scenario_id) + ".scenario"},
            core::ContentDigest("sha256:" + std::string(64U, '2')),
            "optional",
        },
        *physical.bundle, *physical.model_map, *program.program,
        program.role_assignment, catalogs, raw_artifacts);
    static_cast<void>(repository.put_if_absent(prepared));
    CanonicalValue::Object summary;
    for (const auto &[name, value] : prepared.normalized_root().as_object()) {
      if (name != "activation_payload") summary.emplace_back(name, value);
    }
    summaries.emplace_back(std::string(scenario_id),
                           CanonicalValue(std::move(summary)));
  }
  return CanonicalValue(std::move(summaries));
}

void WriteMetadata(
    const std::filesystem::path &path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const std::filesystem::path &admission_filename,
    const deployment::RetainedBundle &close,
    const deployment::RetainedBundle &physical,
    const ProgramPublication &program, CanonicalValue prepared_activations) {
  using V = CanonicalValue;
  const auto bytes = deployment::canonical_json(V(V::Object{
      {"admission_filename", V(admission_filename.string())},
      {"cell_id", V(admission.cell_id().value())},
      // Close: the Apply of the empty intent, compiled the normal way.
      {"close_deployment_artifact_id",
       V(close.value().deployment_artifact_id.value())},
      {"close_deployment_revision", V(close.value().deployment_revision.value())},
      {"close_retained_bundle_id", V(close.retained_bundle_id().value())},
      {"physical_candidate_retained_bundle_id",
       V(physical.retained_bundle_id().value())},
      {"physical_deployment_artifact_id",
       V(physical.value().deployment_artifact_id.value())},
      {"physical_deployment_revision",
       V(physical.value().deployment_revision.value())},
      {"physical_graph_revision",
       V(physical.value().cell_graph.graph_revision.value())},
      {"predecessor_graph_revision", V(admission.graph_revision().value())},
      {"predecessor_retained_bundle_id",
       V(admission.retained_bundle_id().value())},
      {"prepared_activations", std::move(prepared_activations)},
      {"program_binding_id", V(program.binding_id.value())},
      {"program_id", V("program_apply_fixture")},
      {"program_revision", V(program.program_revision.value())},
      {"program_role_assignment", program.role_assignment},
  }));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create Manager Apply fixture metadata");
  }
  stream << bytes;
  stream.flush();
  if (!stream) {
    throw std::runtime_error("cannot persist Manager Apply fixture metadata");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 7 || std::string_view(argv[1]) != "--package-root" ||
        std::string_view(argv[3]) != "--metadata" ||
        std::string_view(argv[5]) != "--profile") {
      throw std::invalid_argument(std::string(kUsage));
    }
    const std::filesystem::path package_root(argv[2]);
    const std::filesystem::path metadata_path(argv[4]);
    const auto profile = SelectProfile(argv[6]);
    std::filesystem::create_directories(package_root);
    const std::filesystem::path workspace(MANYFORGE_WORKSPACE_ROOT);

    // The physical cell: the example Scenario through the normal activation
    // qualifier and materializer.
    const auto physical =
        deployment::test::MaterializeMaintainedInterruptDeployment(
            package_root, CanonicalValue(CanonicalValue::Object{}),
            profile.scenario, profile.authored_mapper);
    const auto &cell_id = physical.bundle->value().cell_graph.cell_id;

    // Close is an Apply of the empty intent (the built-in Untitled Scenario)
    // in the same cell. Its preparation also yields the cell-scoped
    // canonical-empty predecessor the Manager starts from.
    const auto empty_intent = detail::parse_activation_deployment_intent(
        deployment::parse_strict_json_model_yaml(
            Read(workspace / "manyforge_composer/backend/resources/"
                             "Untitled.scenario/deployment.yaml"))
            .root(),
        {});
    const auto close = detail::materialize_prepared_activation(
        workspace, {}, empty_intent, cell_id, {},
        /*prepare_canonical_empty=*/true);
    Publish(package_root, close);
    // The package verifier reads the persisted build authorities before it
    // admits the empty predecessor.
    deployment::FilesystemBuildManifestRepository manifests(package_root);
    static_cast<void>(
        detail::BuildQualifiedRetentionAuthority::qualify_persisted(manifests));
    const auto admission_filename =
        deployment::cell_scoped_canonical_empty_admission_filename(cell_id);
    const auto admission = detail::publish_cell_scoped_canonical_empty_admission(
        package_root, admission_filename, *close.canonical_empty_bundle);

    const auto program = PublishProgram(package_root, physical, profile);
    WriteMetadata(metadata_path, admission, admission_filename, *close.bundle,
                  *physical.bundle, program,
                  PublishPreparedActivations(package_root, physical, program));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
