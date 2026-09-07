#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_lite6_product/lifecycle_profile.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical_empty_admission_internal.hpp"
#include "content_identity_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "retained_bundle_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct Arguments final {
  std::filesystem::path package_root;
  std::filesystem::path metadata_path;
  bool assistant_native;
};

[[nodiscard]] Arguments ParseArguments(const int argc, char **argv) {
  if ((argc != 5 && argc != 7) ||
      std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[3]) != "--metadata" ||
      std::string_view(argv[4]).empty() ||
      (argc == 7 && (std::string_view(argv[5]) != "--profile" ||
                     std::string_view(argv[6]) != "assistant-native"))) {
    throw std::invalid_argument(
        "usage: generate_manager_apply_package --package-root <path> "
        "--metadata <path> [--profile assistant-native]");
  }
  return {argv[2], argv[4], argc == 7};
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
      std::span<const deployment::QualifiedRuntimePolicy>{},
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

void WriteMetadata(
    const std::filesystem::path &path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &candidate,
    const deployment::RetainedBundle &physical_candidate,
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

struct ProgramPublication final {
  core::ProgramBindingId binding_id;
  core::ScenarioCatalogRevision catalog_revision;
};

[[nodiscard]] std::string
ProgramBytes(const deployment::RetainedBundle &bundle) {
  const auto arm_binding =
      std::find_if(bundle.value().cell_graph.control_bindings.begin(),
                   bundle.value().cell_graph.control_bindings.end(),
                   [](const auto &binding) {
                     return binding.command_target_id.value() ==
                            deployment::test::kLite6ArmTargetId;
                   });
  if (arm_binding == bundle.value().cell_graph.control_bindings.end()) {
    throw std::logic_error("assistant-native bundle has no arm capability");
  }
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
      {"scene",
       deployment::CanonicalValue(deployment::CanonicalValue::Array{})},
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
                                  arm_binding->capability_id.value())},
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
    const std::shared_ptr<const deployment::RetainedBundle> &bundle) {
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
      ProgramBytes(*bundle), *bundle);
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  const auto binding = deployment::compile_program_binding(
      *program,
      core::ProgramRoleAssignmentDTO{
          {core::ProgramComponentRoleAssignmentDTO{
              core::ComponentRoleId("robot"),
              core::ControllableComponentId(std::string(
                  deployment::test::kLite6ControllableComponentId))}},
          {core::ProgramTargetRoleAssignmentDTO{
              core::TargetRoleId("arm"),
              core::CommandTargetId(
                  std::string(deployment::test::kLite6ArmTargetId))}},
      },
      *bundle, *model_map, catalogs);
  deployment::FilesystemProgramRepository programs(package_root);
  deployment::FilesystemProgramBindingRepository bindings(package_root);
  static_cast<void>(programs.put_if_absent(program, *bundle));
  static_cast<void>(bindings.put_if_absent(binding, *bundle, *model_map));
  const auto catalog = deployment::build_scenario_catalog({
      deployment::ScenarioCatalogRowDTO{
          .scenario_id = "program_apply_fixture",
          .description = "Assistant and native Program Apply fixture",
          .aliases = {},
          .deployment_artifact_id = bundle->value().deployment_artifact_id,
          .deployment_revision = bundle->value().deployment_revision,
          .retained_bundle_id = bundle->retained_bundle_id(),
          .program_binding_id = binding->program_binding_id(),
          .deployment_compatibility =
              deployment::ScenarioDeploymentCompatibilityDTO{"manyforge-only",
                                                             {}},
          .assistant_mode = deployment::ScenarioAssistantMode::kRequired,
          .typed_parameter_overrides =
              deployment::CanonicalValue(deployment::CanonicalValue::Object{}),
      },
  });
  deployment::FilesystemScenarioCatalogRepository scenarios(package_root,
                                                            "maintained");
  if (scenarios.compare_and_set(std::nullopt, catalog) !=
      deployment::ScenarioCatalogPublishResult::kPublished) {
    throw std::logic_error("assistant-native scenario catalog not published");
  }
  return {binding->program_binding_id(), catalog->revision()};
}

void WriteAssistantNativeMetadata(
    const std::filesystem::path &path,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &physical,
    const ProgramPublication &program,
    const std::filesystem::path &admission_filename) {
  const auto bytes = deployment::canonical_json(
      deployment::CanonicalValue(deployment::CanonicalValue::Object{
          {"admission_filename",
           deployment::CanonicalValue(admission_filename.string())},
          {"cell_id", deployment::CanonicalValue(admission.cell_id().value())},
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
          {"scenario_catalog_revision",
           deployment::CanonicalValue(program.catalog_revision.value())},
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
    const auto arm_lifecycle_semantic = manyforge::lite6_product::
        compute_lite6_lifecycle_catalog_semantic_digest();
    const auto gripper_lifecycle_semantic = manyforge::lite6_product::
        compute_lite6_gripper_lifecycle_catalog_semantic_digest();
    if (!arm_lifecycle_semantic.ok() || !gripper_lifecycle_semantic.ok()) {
      throw std::logic_error(
          "Manager Apply fixture lifecycle catalogs are unavailable");
    }
    auto source =
        arguments.assistant_native
            ? deployment::test::
                  make_lite6_native_cclc_assistant_product_retention_fixture(
                      arm_lifecycle_semantic.value().value(),
                      gripper_lifecycle_semantic.value().value())
            : deployment::test::
                  make_lite6_native_cclc_product_retention_fixture(
                      arm_lifecycle_semantic.value().value(),
                      gripper_lifecycle_semantic.value().value());
    if (!source.native_execution_runtime_profile.has_value()) {
      throw std::logic_error(
          "Manager Apply fixture lacks its native runtime profile");
    }
    const std::array<deployment::QualifiedRuntimeProfile, 1U> profiles{
        *source.native_execution_runtime_profile};
    const auto predecessor_source =
        deployment::detail::derive_cell_scoped_canonical_empty_deployment(
            source.qualified.deployment(), profiles);
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
    if (!arguments.assistant_native) {
      const auto candidate_source =
          NamedRuntimeOnlyCandidate(predecessor_source, profiles);
      candidate = deployment::detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              CompileEmpty(candidate_source, profiles), manifests, input_source,
              {}, catalogs, raw_artifacts, bundles);
    }
    auto physical_candidate = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            source.qualified, manifests, input_source,
            source.structural_preparations(), catalogs, raw_artifacts, bundles);
    if (predecessor == nullptr || physical_candidate == nullptr ||
        (!arguments.assistant_native && candidate == nullptr) ||
        (candidate != nullptr &&
         (predecessor->value().cell_graph.runtime_units !=
              candidate->value().cell_graph.runtime_units ||
          predecessor->value().compiled_products.runtime_unit_products !=
              candidate->value().compiled_products.runtime_unit_products)) ||
        predecessor->value().cell_graph.runtime_units !=
            physical_candidate->value().cell_graph.runtime_units ||
        predecessor->value().compiled_products.runtime_unit_products !=
            physical_candidate->value()
                .compiled_products.runtime_unit_products) {
      throw std::logic_error(
          "Manager Apply fixture lost exact execution-host continuity");
    }
    const auto admission_filename =
        deployment::cell_scoped_canonical_empty_admission_filename(
            predecessor->value().cell_graph.cell_id);
    const auto admission =
        deployment::detail::publish_cell_scoped_canonical_empty_admission(
            arguments.package_root, admission_filename, *predecessor);

    if (arguments.assistant_native) {
      const auto program = PublishAssistantNativeProgram(arguments.package_root,
                                                         physical_candidate);
      WriteAssistantNativeMetadata(arguments.metadata_path, admission,
                                   *physical_candidate, program,
                                   admission_filename);
      return 0;
    }

    const auto catalog = deployment::build_scenario_catalog({
        deployment::ScenarioCatalogRowDTO{
            .scenario_id = "manager_apply_physical_recovery",
            .description =
                std::string("Physical managed composition recovery proof"),
            .aliases = {},
            .deployment_artifact_id =
                physical_candidate->value().deployment_artifact_id,
            .deployment_revision =
                physical_candidate->value().deployment_revision,
            .retained_bundle_id = physical_candidate->retained_bundle_id(),
            .program_binding_id = core::ProgramBindingId(
                physical_candidate->retained_bundle_id().value()),
            .deployment_compatibility =
                deployment::ScenarioDeploymentCompatibilityDTO{"manyforge-only",
                                                               {}},
            .assistant_mode = deployment::ScenarioAssistantMode::kForbidden,
            .typed_parameter_overrides = deployment::CanonicalValue(
                deployment::CanonicalValue::Object{}),
        },
        deployment::ScenarioCatalogRowDTO{
            .scenario_id = "manager_apply_runtime_only",
            .description = std::string("Runtime-only production Apply proof"),
            .aliases = {},
            .deployment_artifact_id = candidate->value().deployment_artifact_id,
            .deployment_revision = candidate->value().deployment_revision,
            .retained_bundle_id = candidate->retained_bundle_id(),
            .program_binding_id =
                core::ProgramBindingId(candidate->retained_bundle_id().value()),
            .deployment_compatibility =
                deployment::ScenarioDeploymentCompatibilityDTO{"manyforge-only",
                                                               {}},
            .assistant_mode = deployment::ScenarioAssistantMode::kForbidden,
            .typed_parameter_overrides = deployment::CanonicalValue(
                deployment::CanonicalValue::Object{}),
        },
    });
    deployment::FilesystemScenarioCatalogRepository scenario_catalog(
        arguments.package_root, "maintained");
    if (scenario_catalog.compare_and_set(std::nullopt, catalog) !=
        deployment::ScenarioCatalogPublishResult::kPublished) {
      throw std::logic_error(
          "Manager Apply fixture scenario catalog was not published");
    }
    WriteMetadata(arguments.metadata_path, admission, *candidate,
                  *physical_candidate, admission_filename);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
