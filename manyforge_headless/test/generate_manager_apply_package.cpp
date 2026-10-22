#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical_empty_admission_internal.hpp"
#include "content_identity_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "robotiq_gripper_only_retention_fixture.hpp"
#include "retained_bundle_internal.hpp"
#include "../../manyforge_roboplan/src/retained_ros2_control_lifecycle_plans_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct Arguments final {
  std::filesystem::path package_root;
  std::filesystem::path metadata_path;
  bool assistant_native;
  bool gripper_only;
  // Present only for the three explicit pneumatic command Programs.
  std::optional<double> pneumatic_dwell_s;
  bool pneumatic_no_output;
  bool robotiq_position;
};

[[nodiscard]] bool SameExecutionHostInfrastructure(
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate) {
  const auto &old_graph = predecessor.value().cell_graph;
  const auto &new_graph = candidate.value().cell_graph;
  if (old_graph.cell_id != new_graph.cell_id ||
      old_graph.runtime_units.size() != 1U ||
      new_graph.runtime_units.size() != 1U) {
    return false;
  }
  const auto &old_host = old_graph.runtime_units.front();
  const auto &new_host = new_graph.runtime_units.front();
  // Empty and populated cells share infrastructure, not controller settings.
  return old_host.runtime_unit_id == new_host.runtime_unit_id &&
         old_host.runtime_profile_id == new_host.runtime_profile_id &&
         old_host.runtime_profile_revision == new_host.runtime_profile_revision &&
         old_host.dependency_runtime_unit_ids ==
             new_host.dependency_runtime_unit_ids &&
         predecessor.value().compiled_products.runtime_unit_products ==
             candidate.value().compiled_products.runtime_unit_products;
}

void WriteDeploymentSource(
    const std::filesystem::path &package_root,
    const deployment::RevisionVerifiedDeploymentArtifact &deployment) {
  const auto path =
      package_root /
      (deployment.deployment_artifact_id().value() + ".deployment.json");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create Manager Apply Deployment source");
  }
  stream << deployment::canonical_json(deployment.normalized_authored_root());
  stream.flush();
  if (!stream) {
    throw std::runtime_error("cannot write Manager Apply Deployment source");
  }
}

[[nodiscard]] Arguments ParseArguments(const int argc, char **argv) {
  if ((argc != 5 && argc != 7) ||
      std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[3]) != "--metadata" ||
      std::string_view(argv[4]).empty() ||
      (argc == 7 && (std::string_view(argv[5]) != "--profile" ||
                     (std::string_view(argv[6]) != "assistant-ros2-control" &&
                      std::string_view(argv[6]) != "gripper-only" &&
                      std::string_view(argv[6]) != "gripper-only-robotiq-position" &&
                      std::string_view(argv[6]) != "gripper-pneumatic-commanded" &&
                      std::string_view(argv[6]) != "gripper-pneumatic-none-zero" &&
                      std::string_view(argv[6]) != "gripper-pneumatic-none-dwell" &&
                      std::string_view(argv[6]) != "ros2-control")))) {
    throw std::invalid_argument(
        "usage: generate_manager_apply_package --package-root <path> "
        "--metadata <path> [--profile assistant-ros2-control|gripper-only|ros2-control|"
        "gripper-pneumatic-commanded|gripper-pneumatic-none-zero|"
        "gripper-pneumatic-none-dwell|gripper-only-robotiq-position]");
  }
  const auto profile = argc == 7 ? std::string_view(argv[6]) : std::string_view{};
  std::optional<double> pneumatic_dwell_s;
  if (profile == "gripper-pneumatic-commanded") pneumatic_dwell_s = 0.8;
  if (profile == "gripper-pneumatic-none-zero") pneumatic_dwell_s = 0.0;
  if (profile == "gripper-pneumatic-none-dwell") pneumatic_dwell_s = 0.25;
  return {argv[2], argv[4], profile == "assistant-ros2-control",
          profile == "gripper-only" || profile == "gripper-only-robotiq-position" ||
              pneumatic_dwell_s.has_value(),
          pneumatic_dwell_s,
          profile == "gripper-pneumatic-none-zero" ||
              profile == "gripper-pneumatic-none-dwell",
          profile == "gripper-only-robotiq-position"};
}

[[nodiscard]] deployment::RetentionQualifiedDeployment CompileEmpty(
    const deployment::RevisionVerifiedDeploymentArtifact &source,
    const std::span<const deployment::QualifiedRuntimeProfile> profiles) {
  const auto composition = deployment::compose_cell_occurrences(
      source.graph(), std::span<const deployment::SourceVerifiedModelAsset>{},
      std::span<const deployment::QualifiedComponentType>{});
  const auto overlay = deployment::select_site_overlay_for_deployment(
      source.applicability_inputs().value().site_overlay_ref,
      source.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = deployment::qualify_cell_applicability(
      composition, source.applicability_inputs(), profiles,
      std::span<const deployment::QualifiedCommandSchema>{},
      std::span<const deployment::QualifiedCapabilityDefinition>{},
      std::span<const deployment::QualifiedControlInterfaceContract>{},
      std::span<const deployment::QualifiedControllerTopology>{},
      std::span<const deployment::QualifiedProviderManifest>{},
      std::span<const deployment::QualifiedStopEvidencePolicy>{},
      std::span<const deployment::BuildQualifiedValueSchemaEntry>{},
      std::span<const deployment::QualifiedCommissioningEvidenceRef>{}, overlay,
      std::span<const deployment::QualifiedContactPolicy>{});
  return deployment::qualify_deployment_for_retention(
      source, deployment::compile_cell_ir(applicability));
}

[[nodiscard]] deployment::RevisionVerifiedDeploymentArtifact
NamedRuntimeOnlyCandidate(
    const deployment::RevisionVerifiedDeploymentArtifact &predecessor,
    const std::span<const deployment::QualifiedRuntimeProfile> profiles) {
  auto root = predecessor.normalized_authored_root().as_object();
  for (auto &[name, value] : root) {
    if (name == "deployment_artifact_id") {
      value = deployment::CanonicalValue("i6_manager_apply_runtime_only");
    } else if (name == "name") {
      value = deployment::CanonicalValue("I6 Manager Apply runtime only");
    } else if (name == "deployment_revision") {
      value = deployment::CanonicalValue(std::string(kZeroDigest));
    }
  }
  return deployment::compute_deployment_artifact_revision(
      deployment::parse_strict_json_model_yaml(deployment::canonical_json(
          deployment::CanonicalValue(std::move(root)))),
      profiles);
}

struct ProgramPublication final {
  core::ProgramBindingId binding_id;
  core::ProgramRevision program_revision;
  /// The exact authored assignment the binding was compiled from.
  deployment::CanonicalValue role_assignment;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
};

void WriteMetadata(
    const std::filesystem::path &path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &candidate,
    const deployment::RetainedBundle &physical_candidate,
    const ProgramPublication &program,
    const std::filesystem::path &admission_filename) {
  const auto bytes = deployment::canonical_json(
      deployment::CanonicalValue(deployment::CanonicalValue::Object{
          {"admission_filename",
           deployment::CanonicalValue(admission_filename.string())},
          {"candidate_retained_bundle_id",
           deployment::CanonicalValue(candidate.retained_bundle_id().value())},
          {"cell_id", deployment::CanonicalValue(admission.cell_id().value())},
          {"deployment_artifact_id",
           deployment::CanonicalValue(
               candidate.value().deployment_artifact_id.value())},
          {"deployment_revision",
           deployment::CanonicalValue(
               candidate.value().deployment_revision.value())},
          {"predecessor_graph_revision",
           deployment::CanonicalValue(admission.graph_revision().value())},
          {"predecessor_retained_bundle_id",
           deployment::CanonicalValue(admission.retained_bundle_id().value())},
          {"physical_candidate_retained_bundle_id",
           deployment::CanonicalValue(
               physical_candidate.retained_bundle_id().value())},
          {"physical_deployment_artifact_id",
           deployment::CanonicalValue(
               physical_candidate.value().deployment_artifact_id.value())},
          {"physical_deployment_revision",
           deployment::CanonicalValue(
               physical_candidate.value().deployment_revision.value())},
          {"program_binding_id",
           deployment::CanonicalValue(program.binding_id.value())},
          {"program_id", deployment::CanonicalValue("program_apply_fixture")},
          {"program_revision",
           deployment::CanonicalValue(program.program_revision.value())},
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

[[nodiscard]] std::string ProgramBytes(const deployment::RetainedBundle &bundle,
                                       const bool initial_scene,
                                       const bool gripper_only,
                                       const std::optional<double> pneumatic_dwell_s,
                                       const bool robotiq_position = false) {
  const auto arm_binding =
      std::find_if(bundle.value().cell_graph.control_bindings.begin(),
                   bundle.value().cell_graph.control_bindings.end(),
                   [](const auto &binding) {
                     return binding.command_target_id.value() ==
                            deployment::test::kLite6ArmTargetId;
                   });
  if (!gripper_only &&
      arm_binding == bundle.value().cell_graph.control_bindings.end()) {
    throw std::logic_error("assistant-native bundle has no arm capability");
  }
  const std::string arm_capability =
      gripper_only ? "execution.trajectory.execute"
                   : arm_binding->capability_id.value();
  deployment::CanonicalValue root(deployment::CanonicalValue::Object{
      {"api_version", deployment::CanonicalValue("manyforge.program.v2")},
      {"catalog",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"api_version",
            deployment::CanonicalValue("manyforge.node_catalog.v2")},
           {"content_version",
            deployment::CanonicalValue(
                bundle.value().catalogs.node_environment.content_version)},
       })},
      {"component_roles",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"robot",
            deployment::CanonicalValue(deployment::CanonicalValue::Object{})},
       })},
      {"name", deployment::CanonicalValue("Assistant native fixture")},
      {"program_id", deployment::CanonicalValue("program_apply_fixture")},
      {"program_profile", deployment::CanonicalValue("composable-cell-v1")},
      {"program_revision",
       deployment::CanonicalValue(std::string(kZeroDigest))},
      {"scene", initial_scene
                    ? deployment::parse_strict_json_model_yaml(R"([
        {"scene_resource_id":"apply_fixture_marker",
         "shape":{"type":"box","box_dimensions_m":[0.1,0.1,0.1]},
         "pose":{"frame_ref":{"kind":"cell_frame"},
                 "position_m":[3.0,0.0,0.0],
                 "orientation":{"xyzw":[0.0,0.0,0.0,1.0]}}}
      ])").root()
                    : deployment::CanonicalValue(
                          deployment::CanonicalValue::Array{})},
      {"target_roles",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"arm",
            deployment::CanonicalValue(deployment::CanonicalValue::Object{
                {"component_role", deployment::CanonicalValue("robot")},
                {"required_capabilities",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue(
                         deployment::CanonicalValue::Object{
                             {"capability_id",
                              deployment::CanonicalValue(
                                  arm_capability)},
                             {"disposition",
                              deployment::CanonicalValue("required")},
                             {"parameters",
                              deployment::CanonicalValue(
                                  deployment::CanonicalValue::Object{})},
                         })})},
                {"required_collision_body_roles",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue("arm_collision")})},
                {"required_frame_roles",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue("tcp")})},
                {"required_model_roles",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue("arm_model")})},
                {"required_resource_roles",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue("joints")})},
            })},
       })},
      {"tree",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"children",
            deployment::CanonicalValue(deployment::CanonicalValue::Array{
                deployment::CanonicalValue(deployment::CanonicalValue::Object{
                    {"children", deployment::CanonicalValue(
                                     deployment::CanonicalValue::Array{})},
                    {"instance_id",
                     deployment::CanonicalValue("assistant_native_timer")},
                    {"node_id", deployment::CanonicalValue("timer")},
                    {"parameters",
                     deployment::CanonicalValue(
                         deployment::CanonicalValue::Object{
                             {"duration_s", deployment::CanonicalValue(0.001)},
                         })},
                }),
            })},
           {"instance_id", deployment::CanonicalValue("root")},
           {"node_id", deployment::CanonicalValue("sequence")},
           {"parameters",
            deployment::CanonicalValue(deployment::CanonicalValue::Object{})},
       })},
  });
  if (gripper_only) {
    // One explicit qualified test source; the maintained default is unchanged.
    auto fields = root.as_object();
    for (auto &[field, value] : fields) {
      if (field == "target_roles") {
        auto targets = value.as_object();
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                                     [](const auto &entry) {
                                       return entry.first == "arm";
                                     }),
                      targets.end());
        targets.emplace_back("gripper",
                             deployment::parse_strict_json_model_yaml(R"({
          "component_role":"robot",
          "required_capabilities":[
            {"capability_id":"tool.gripper.command","parameters":{},"disposition":"required"}],
          "required_frame_roles":["tcp"], "required_resource_roles":["fingers"],
          "required_collision_body_roles":["gripper_collision"], "required_model_roles":["arm_model"]
        })")
                                 .root());
        value = deployment::CanonicalValue(std::move(targets));
      }
    }
    root = deployment::CanonicalValue(std::move(fields));
  }
  if (pneumatic_dwell_s.has_value()) {
    if (!gripper_only) {
      throw std::logic_error("pneumatic command Program requires its gripper role");
    }
    using Value = deployment::CanonicalValue;
    auto fields = root.as_object();
    for (auto &[field, value] : fields) {
      if (field == "target_roles") {
        auto targets = value.as_object();
        auto gripper = targets.front().second.as_object();
        for (auto &[name, selected] : gripper) {
          if (name == "required_capabilities") {
            auto capabilities = selected.as_array();
            capabilities.push_back(deployment::parse_strict_json_model_yaml(R"({
              "capability_id":"tool.gripper.feedback", "parameters":{},
              "disposition":"required"
            })").root());
            selected = Value(std::move(capabilities));
          }
        }
        targets.front().second = Value(std::move(gripper));
        value = Value(std::move(targets));
      } else if (field == "tree" && *pneumatic_dwell_s > 0.0) {
        // Genuine Program time creates the two observation windows. Command
        // delivery itself is never extended by this qualification fixture.
        const auto command = [&](const char *instance_id, const char *preset,
                                 const char *policy) {
          return Value(Value::Object{
              {"children", Value(Value::Array{})},
              {"instance_id", Value(instance_id)},
              {"node_id", Value("gripper.command_position")},
              {"operator_stop_policy", Value(policy)},
              {"parameters", Value(Value::Object{
                  {"command_duration_s", Value(*pneumatic_dwell_s)},
                  {"dwell_while_held", Value("pause")},
                  {"position", Value(Value::Object{
                      {"kind", Value("named_preset")},
                      {"name", Value(preset)},
                  })},
                  {"target_role", Value("gripper")},
              })},
          });
        };
        const auto timer = [](const char *instance_id, const char *policy) {
          return Value(Value::Object{
              {"children", Value(Value::Array{})},
              {"instance_id", Value(instance_id)},
              {"node_id", Value("timer")},
              {"operator_stop_policy", Value(policy)},
              {"parameters", Value(Value::Object{
                  {"duration_s", Value(5.0)},
                  {"duration_while_held", Value("pause")},
              })},
          });
        };
        value = Value(Value::Object{
            {"children", Value(Value::Array{
                command("pneumatic_close", "closed", "continue"),
                timer("pneumatic_continue_timer", "continue"),
                command("pneumatic_open_continue", "open", "continue"),
                command("pneumatic_close_held", "closed", "hold"),
                timer("pneumatic_finish_timer", "hold"),
            })},
            {"instance_id", Value("root")},
            {"node_id", Value("sequence")},
            {"operator_stop_policy", Value("continue")},
            {"parameters", Value(Value::Object{})},
        });
      } else if (field == "tree") {
        value = Value(Value::Object{
            {"children", Value(Value::Array{Value(Value::Object{
                {"children", Value(Value::Array{})},
                {"instance_id", Value("pneumatic_close")},
                {"node_id", Value("gripper.command_position")},
                {"parameters", Value(Value::Object{
                    {"command_duration_s", Value(*pneumatic_dwell_s)},
                    {"position", Value(Value::Object{
                        {"kind", Value("named_preset")},
                        {"name", Value("closed")},
                    })},
                    {"target_role", Value("gripper")},
                })},
            })})},
            {"instance_id", Value("root")},
            {"node_id", Value("sequence")},
            {"parameters", Value(Value::Object{})},
        });
      }
    }
    root = Value(std::move(fields));
  }
  if (robotiq_position) {
    if (!gripper_only || pneumatic_dwell_s.has_value())
      throw std::logic_error("standalone Position Program requires its dedicated scalar selection");
    using Value = deployment::CanonicalValue;
    auto fields = root.as_object();
    for (auto &[field, value] : fields) {
      if (field == "name") {
        value = Value("Standalone Robotiq Position Manager fixture");
      } else if (field == "target_roles") {
        auto targets = value.as_object();
        if (targets.size() != 1U || targets.front().first != "gripper")
          throw std::logic_error("standalone Position Program requires only its gripper role");
        auto gripper = targets.front().second.as_object();
        for (auto &[name, selected] : gripper) {
          if (name == "required_model_roles") {
            selected = Value(Value::Array{Value("gripper_model")});
          } else if (name == "required_capabilities") {
            auto capabilities = selected.as_array();
            capabilities.emplace_back(Value::Object{
                {"capability_id", Value("tool.gripper.feedback")},
                {"parameters", Value(Value::Object{})},
                {"disposition", Value("required")},
            });
            selected = Value(std::move(capabilities));
          }
        }
        targets.front().second = Value(std::move(gripper));
        value = Value(std::move(targets));
      } else if (field == "tree") {
        Value::Array children;
        for (const auto preset : {"closed", "open"}) {
          children.emplace_back(Value::Object{
              {"children", Value(Value::Array{})},
              {"instance_id", Value(std::string("robotiq_") + preset)},
              {"node_id", Value("gripper.command_position")},
              {"parameters", Value(Value::Object{
                  {"command_duration_s", Value(1.0)},
                  {"position", Value(Value::Object{
                      {"kind", Value("named_preset")},
                      {"name", Value(preset)},
                  })},
                  {"target_role", Value("gripper")},
              })},
          });
        }
        value = Value(Value::Object{
            {"children", Value(std::move(children))},
            {"instance_id", Value("root")},
            {"node_id", Value("sequence")},
            {"parameters", Value(Value::Object{})},
        });
      }
    }
    root = Value(std::move(fields));
  }
  const auto revision = deployment::detail::compute_content_identity(
      root, deployment::detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = "manyforge.program.v2",
                .own_field = "program_revision",
            });
  auto members = root.as_object();
  for (auto &[field, value] : members) {
    if (field == "program_revision") {
      value = deployment::CanonicalValue(revision);
    }
  }
  return deployment::canonical_json(
      deployment::CanonicalValue(std::move(members)));
}

[[nodiscard]] ProgramPublication PublishAssistantNativeProgram(
    const std::filesystem::path &package_root,
    const std::shared_ptr<const deployment::RetainedBundle> &bundle,
    const bool initial_scene = false,
    const bool gripper_only = false,
    const std::optional<double> pneumatic_dwell_s = std::nullopt,
    const bool robotiq_position = false) {
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error("assistant-native bundle has no sole model map");
  }
  const deployment::FilesystemVerifiedRetainedModelMapSource model_maps(
      package_root);
  const auto model_map =
      model_maps.get_verified(deployment::RetainedModelMapQuery{
          bundle->retained_bundle_id(),
          bundle->value()
              .compiled_products.symbol_role_maps.front()
              .product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("assistant-native model map did not reopen");
  }
  const auto program = deployment::verify_program_binding_contract(
      ProgramBytes(*bundle, initial_scene, gripper_only, pneumatic_dwell_s,
                   robotiq_position), *bundle);
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  using Value = deployment::CanonicalValue;
  Value::Array targets;
  if (!gripper_only) {
    targets.emplace_back(Value::Object{
        {"command_target_id",
         Value(std::string(deployment::test::kLite6ArmTargetId))},
        {"target_role_id", Value("arm")},
    });
  }
  if (gripper_only) {
    targets.emplace_back(Value::Object{
        {"command_target_id",
         Value(std::string(deployment::test::kLite6GripperTargetId))},
        {"target_role_id", Value("gripper")},
    });
  }
  // One authored assignment value is both compiled here and carried by the
  // published prepared activations, so the two can never diverge.
  Value role_assignment(Value::Object{
      {"component_roles",
       Value(Value::Array{Value(Value::Object{
           {"component_role_id", Value("robot")},
           {"controllable_component_id",
            Value(std::string(
                robotiq_position ? deployment::test::kRobotiqGripperOnlyComponentId
                                 : deployment::test::kLite6ControllableComponentId))},
       })})},
      {"target_roles", Value(std::move(targets))},
  });
  const auto binding = deployment::compile_program_binding(
      *program, deployment::parse_program_role_assignment(role_assignment),
      *bundle, *model_map, catalogs);
  deployment::FilesystemProgramRepository programs(package_root);
  deployment::FilesystemProgramBindingRepository bindings(package_root);
  static_cast<void>(programs.put_if_absent(program, *bundle));
  static_cast<void>(bindings.put_if_absent(binding, *bundle, *model_map));
  return {binding->program_binding_id(), program->value().program_revision,
          std::move(role_assignment), program};
}

/// Assemble and publish one real prepared activation per Scenario ID the
/// process tests submit, and return their Apply summaries by Scenario ID.
/// The Manager admits only stored, reverified prepared values.
[[nodiscard]] deployment::CanonicalValue PublishPreparedActivations(
    const std::filesystem::path &package_root,
    const deployment::RetainedBundle &bundle, const ProgramPublication &program,
    const deployment::IRawArtifactStore &raw_artifacts,
    std::initializer_list<std::string_view> scenario_ids) {
  const deployment::FilesystemVerifiedRetainedModelMapSource model_maps(
      package_root);
  const auto model_map =
      model_maps.get_verified(deployment::RetainedModelMapQuery{
          bundle.retained_bundle_id(),
          bundle.value().compiled_products.symbol_role_maps.front().product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("prepared-activation model map did not reopen");
  }
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  const deployment::FilesystemPreparedActivationRepository repository(
      package_root);
  deployment::CanonicalValue::Object summaries;
  for (const auto scenario_id : scenario_ids) {
    const auto prepared = deployment::assemble_prepared_activation(
        deployment::PreparedActivationAssemblyInput{
            deployment::PreparedScenarioOrigin{
                std::string(scenario_id),
                core::ContentDigest("sha256:" + std::string(64U, 'e')),
                "/fixtures/" + std::string(scenario_id) + ".scenario"},
            core::ContentDigest("sha256:" + std::string(64U, '2')),
            "optional",
        },
        bundle, *model_map, *program.program, program.role_assignment, catalogs,
        raw_artifacts);
    static_cast<void>(repository.put_if_absent(prepared));
    deployment::CanonicalValue::Object summary;
    for (const auto &[name, value] : prepared.normalized_root().as_object()) {
      if (name != "activation_payload") summary.emplace_back(name, value);
    }
    summaries.emplace_back(std::string(scenario_id),
                           deployment::CanonicalValue(std::move(summary)));
  }
  return deployment::CanonicalValue(std::move(summaries));
}

void WriteAssistantNativeMetadata(
    const std::filesystem::path &path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &physical,
    const ProgramPublication &program,
    const std::filesystem::path &admission_filename,
    deployment::CanonicalValue prepared_activations) {
  const auto bytes = deployment::canonical_json(
      deployment::CanonicalValue(deployment::CanonicalValue::Object{
          {"admission_filename",
           deployment::CanonicalValue(admission_filename.string())},
          {"cell_id", deployment::CanonicalValue(admission.cell_id().value())},
          {"prepared_activations", std::move(prepared_activations)},
          {"predecessor_graph_revision",
           deployment::CanonicalValue(admission.graph_revision().value())},
          {"predecessor_retained_bundle_id",
           deployment::CanonicalValue(admission.retained_bundle_id().value())},
          {"physical_candidate_retained_bundle_id",
           deployment::CanonicalValue(physical.retained_bundle_id().value())},
          {"physical_deployment_artifact_id",
           deployment::CanonicalValue(
               physical.value().deployment_artifact_id.value())},
          {"physical_deployment_revision",
           deployment::CanonicalValue(
               physical.value().deployment_revision.value())},
          {"physical_graph_revision",
           deployment::CanonicalValue(
               physical.value().cell_graph.graph_revision.value())},
          {"program_binding_id",
           deployment::CanonicalValue(program.binding_id.value())},
          {"program_revision",
           deployment::CanonicalValue(program.program_revision.value())},
      }));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create assistant-native fixture metadata");
  }
  stream << bytes;
  stream.flush();
  if (!stream) {
    throw std::runtime_error(
        "cannot persist assistant-native fixture metadata");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    std::filesystem::create_directories(arguments.package_root);
    auto source = [&]() {
      const auto plans = manyforge::roboplan_backend::detail::
          ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
      const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
          core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
      if (!semantic.ok()) {
        throw std::logic_error("Manager Apply ROS lifecycle catalog is unavailable");
      }
      if (arguments.robotiq_position) {
        return deployment::test::make_robotiq_gripper_only_retention_fixture(
            semantic.value().value());
      }
      if (arguments.pneumatic_dwell_s.has_value()) {
        // The command node also requires the genuine feedback capability.
        // This is the existing single-gripper constructor with that qualified
        // read binding enabled, not a second actuator or synthetic observation.
        return deployment::test::make_lite6_controllable_retention_fixture(
            false, false, 1U, true, true, {}, false, true, "resource", true,
            semantic.value().value(), semantic.value().value(), true,
            true, true, std::nullopt, false, deployment::test::kLite6ArmTargetId,
            std::nullopt, std::nullopt, {}, {}, true, {},
            "manyforge.provider.ros2_control", true, true, nullptr, nullptr,
            false, false, "pneumatic", arguments.pneumatic_dwell_s,
            arguments.pneumatic_no_output ? "none" : "commanded");
      }
      if (arguments.gripper_only) {
        // Preserve the canonical pre-migration position fixture and timer
        // Program; pneumatic command coverage uses the explicit new profiles.
        return deployment::test::make_lite6_ros2_control_gripper_only_cclc_product_retention_fixture(
            semantic.value().value(), "position_commanded", std::nullopt,
            nullptr, "commanded");
      }
      return arguments.assistant_native
          ? deployment::test::make_lite6_ros2_control_cclc_assistant_product_retention_fixture(
                semantic.value().value())
          : deployment::test::make_lite6_ros2_control_cclc_product_retention_fixture(
                semantic.value().value());
    }();
    const auto &selected_profile = source.ros2_control_execution_runtime_profile;
    if (!selected_profile.has_value()) {
      throw std::logic_error("Manager Apply fixture lacks its ROS runtime profile");
    }
    const std::array<deployment::QualifiedRuntimeProfile, 1U> profiles{
        *selected_profile};
    const auto predecessor_source = [&]() {
      auto empty = deployment::detail::derive_cell_scoped_canonical_empty_deployment(
          source.qualified.deployment(), profiles);
      // Produce a genuine zero-component ROS compiler input. The physical
      // source's controller/profile/position rows are not empty-host settings.
      auto root = empty.normalized_authored_root().as_object();
      for (auto &[name, value] : root) {
        if (name == "deployment_revision") {
          value = deployment::CanonicalValue(std::string(kZeroDigest));
        } else if (name == "graph") {
          auto graph = value.as_object();
          for (auto &[field, selected] : graph) {
            if (field != "runtime_units") continue;
            auto units = selected.as_array();
            if (units.size() != 1U) {
              throw std::logic_error("Manager Apply ROS fixture has no exact empty host");
            }
            auto unit = units.front().as_object();
            for (auto &[key, entry] : unit) {
              if (key == "configuration") {
                entry = deployment::ros2_control_execution_runtime_contract::
                    empty_host_configuration(std::nullopt);
              }
            }
            units.front() = deployment::CanonicalValue(std::move(unit));
            selected = deployment::CanonicalValue(std::move(units));
          }
          value = deployment::CanonicalValue(std::move(graph));
        }
      }
      return deployment::compute_deployment_artifact_revision(
          deployment::parse_strict_json_model_yaml(deployment::canonical_json(
              deployment::CanonicalValue(std::move(root)))), profiles);
    }();
    const deployment::test::Lite6ControllableCompilerInputArtifactSource
        input_source(source);
    deployment::FilesystemBuildManifestRepository manifests(
        arguments.package_root);
    deployment::FilesystemCatalogArtifactStore catalogs(arguments.package_root);
    deployment::FilesystemRawArtifactStore raw_artifacts(
        arguments.package_root);
    deployment::FilesystemRetainedBundleRepository bundles(
        arguments.package_root);
    auto predecessor = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            CompileEmpty(predecessor_source, profiles), manifests, input_source,
            {}, catalogs, raw_artifacts, bundles);
    std::shared_ptr<const deployment::RetainedBundle> candidate;
    if (!arguments.assistant_native && !arguments.gripper_only) {
      const auto candidate_source =
          NamedRuntimeOnlyCandidate(predecessor_source, profiles);
      WriteDeploymentSource(arguments.package_root, candidate_source);
      candidate = deployment::detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              CompileEmpty(candidate_source, profiles), manifests, input_source,
              {}, catalogs, raw_artifacts, bundles);
    }
    auto physical_candidate = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            source.qualified, manifests, input_source,
            source.structural_preparations(), catalogs, raw_artifacts, bundles);
    WriteDeploymentSource(arguments.package_root,
                          source.qualified.deployment());
    if (predecessor == nullptr || physical_candidate == nullptr ||
        (!arguments.assistant_native && !arguments.gripper_only &&
         candidate == nullptr) ||
        (candidate != nullptr &&
         !SameExecutionHostInfrastructure(*predecessor, *candidate)) ||
        !SameExecutionHostInfrastructure(*predecessor, *physical_candidate)) {
      throw std::logic_error(
          "Manager Apply fixture lost exact execution-host continuity");
    }
    const auto &host_id =
        predecessor->value().cell_graph.runtime_units.front().runtime_unit_id;
    static_cast<void>(deployment::ros2_control_execution_runtime_contract::
                          require_configuration(*predecessor, host_id));
    if (candidate != nullptr) {
      static_cast<void>(deployment::ros2_control_execution_runtime_contract::
                            require_configuration(*candidate, host_id));
    }
    if (arguments.pneumatic_dwell_s.has_value()) {
      const auto configuration = deployment::ros2_control_execution_runtime_contract::
          require_configuration(*physical_candidate, host_id);
      const auto expected_mode = arguments.pneumatic_no_output
          ? core::GripperOutputMode::kNone : core::GripperOutputMode::kCommanded;
      if (!configuration.initial_joint_positions_rad.empty() ||
          !configuration.motion_target_limits.empty() ||
          configuration.gripper_target_configurations.size() != 1U) {
        throw std::logic_error("pneumatic Manager fixture lost its no-motion shape");
      }
      const auto &gripper = configuration.gripper_target_configurations.front();
      if (gripper.controllable_component_id.value() !=
              deployment::test::kLite6ControllableComponentId ||
          gripper.command_target_id.value() != deployment::test::kLite6GripperTargetId ||
          gripper.actuation_kind != "pneumatic" ||
          gripper.output_mode != expected_mode ||
          gripper.command_duration_s != *arguments.pneumatic_dwell_s) {
        throw std::logic_error("pneumatic Manager fixture changed its strict authored row");
      }
    }
    if (arguments.robotiq_position) {
      const auto &graph = physical_candidate->value().cell_graph;
      const auto configuration = deployment::ros2_control_execution_runtime_contract::
          require_configuration(*physical_candidate, host_id);
      if (graph.model_instances.size() != 1U ||
          graph.model_instances.front().model_asset_id.value() != "robotiq_2f_85" ||
          graph.controllable_components.size() != 1U || graph.control_domains.size() != 1U ||
          graph.control_bindings.size() != 2U || !graph.component_attachments.empty() ||
          !configuration.initial_joint_positions_rad.empty() ||
          !configuration.motion_target_limits.empty() || configuration.isaac_world ||
          configuration.controller_profiles.size() != 1U ||
          configuration.controller_profiles.front().mock_profile !=
              std::optional<std::string>("ur5e_robotiq_2f_85") ||
          configuration.gripper_target_configurations.size() != 1U)
        throw std::logic_error("Robotiq Manager fixture loses its genuine no-arm shape");
      const auto &command = graph.control_bindings.front();
      const auto &feedback = graph.control_bindings.back();
      const auto &gripper = configuration.gripper_target_configurations.front();
      if (command.capability_id.value() != "tool.gripper.command" ||
          feedback.capability_id.value() != "tool.gripper.feedback" ||
          command.controllable_component_id.value() != deployment::test::kRobotiqGripperOnlyComponentId ||
          feedback.controllable_component_id != command.controllable_component_id ||
          command.command_target_id.value() != "gripper" ||
          feedback.command_target_id != command.command_target_id ||
          feedback.control_domain_id != command.control_domain_id ||
          command.resource_bindings.size() != 1U ||
          feedback.resource_bindings != command.resource_bindings ||
          feedback.provider_instance_id != command.provider_instance_id ||
          feedback.provider_runtime_unit_id != command.provider_runtime_unit_id ||
          gripper.controllable_component_id != command.controllable_component_id ||
          gripper.command_target_id != command.command_target_id ||
          gripper.actuation_kind != "position_commanded" ||
          gripper.output_mode != core::GripperOutputMode::kCommanded ||
          gripper.command_duration_s != 1.0 || gripper.delivery_result_timeout_s ||
          gripper.mount_frame != "robotiq_85_base_link" ||
          gripper.attach_frame != "robotiq_85_base_link" ||
          gripper.tcp_frame != "robotiq_85_base_link" || gripper.touch_links.size() != 9U)
        throw std::logic_error("Robotiq Manager fixture changes its single physical owner or Position configuration");
    }
    const auto admission_filename =
        deployment::cell_scoped_canonical_empty_admission_filename(
            predecessor->value().cell_graph.cell_id);
    const auto admission =
        deployment::detail::publish_cell_scoped_canonical_empty_admission(
            arguments.package_root, admission_filename, *predecessor);

    if (arguments.assistant_native || arguments.gripper_only) {
      const auto program = PublishAssistantNativeProgram(
          arguments.package_root, physical_candidate, false,
          arguments.gripper_only, arguments.pneumatic_dwell_s,
          arguments.robotiq_position);
      WriteAssistantNativeMetadata(
          arguments.metadata_path, admission, *physical_candidate, program,
          admission_filename,
          PublishPreparedActivations(
              arguments.package_root, *physical_candidate, program,
              raw_artifacts,
              {"assistant_native_process_fixture", "program_apply_fixture"}));
      return 0;
    }

    const auto program = PublishAssistantNativeProgram(
        arguments.package_root, physical_candidate, true, false);
    WriteMetadata(arguments.metadata_path, admission, *candidate,
                  *physical_candidate, program, admission_filename);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
