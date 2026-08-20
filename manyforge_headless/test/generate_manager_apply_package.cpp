#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_lite6_product/lifecycle_profile.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical_empty_admission_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "retained_bundle_internal.hpp"
#include "stop_qualification_evidence_internal.hpp"

namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct StopQualificationInputPaths final {
  std::filesystem::path implementation_build;
  std::filesystem::path installed_target;
  std::filesystem::path runner_contract;
  std::filesystem::path runner_executable;
  std::filesystem::path implementation_evidence;
  std::filesystem::path substitution_evidence;
};

struct Arguments final {
  std::filesystem::path package_root;
  std::filesystem::path metadata_path;
  StopQualificationInputPaths stop_qualification;
};

[[nodiscard]] Arguments ParseArguments(const int argc, char **argv) {
  if (argc != 17 || std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[3]) != "--metadata" ||
      std::string_view(argv[4]).empty() ||
      std::string_view(argv[5]) != "--stop-implementation-build" ||
      std::string_view(argv[6]).empty() ||
      std::string_view(argv[7]) != "--stop-installed-target" ||
      std::string_view(argv[8]).empty() ||
      std::string_view(argv[9]) != "--stop-runner-contract" ||
      std::string_view(argv[10]).empty() ||
      std::string_view(argv[11]) != "--stop-runner-executable" ||
      std::string_view(argv[12]).empty() ||
      std::string_view(argv[13]) != "--stop-implementation-evidence" ||
      std::string_view(argv[14]).empty() ||
      std::string_view(argv[15]) != "--stop-substitution-evidence" ||
      std::string_view(argv[16]).empty()) {
    throw std::invalid_argument(
        "usage: generate_manager_apply_package --package-root <path> "
        "--metadata <path> --stop-implementation-build <path> "
        "--stop-installed-target <path> --stop-runner-contract <path> "
        "--stop-runner-executable <path> "
        "--stop-implementation-evidence <path> "
        "--stop-substitution-evidence <path>");
  }
  return {argv[2],
          argv[4],
          {argv[6], argv[8], argv[10], argv[12], argv[14], argv[16]}};
}

[[nodiscard]] std::string ReadExactFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read Manager Apply fixture input: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string
ReadQualificationDocument(const std::filesystem::path &path) {
  auto bytes = ReadExactFile(path);
  if (bytes.empty() ||
      bytes.size() > deployment::detail::kMaxStopQualificationEvidenceBytes) {
    throw std::runtime_error(
        "Manager Apply native Stop qualification input violates its byte "
        "bound: " +
        path.string());
  }
  return bytes;
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
        deployment::test::make_lite6_native_cclc_product_retention_fixture(
            arm_lifecycle_semantic.value().value(),
            gripper_lifecycle_semantic.value().value());
    if (!source.native_execution_runtime_profile.has_value()) {
      throw std::logic_error(
          "Manager Apply fixture lacks its native runtime profile");
    }
    const auto &qualification_paths = arguments.stop_qualification;
    auto installed_target =
        ReadExactFile(qualification_paths.installed_target);
    auto runner_executable =
        ReadExactFile(qualification_paths.runner_executable);
    if (installed_target.empty() || runner_executable.empty()) {
      throw std::runtime_error(
          "Manager Apply native Stop executable input is empty");
    }
    const deployment::detail::StopQualificationEvidenceCandidate
        stop_qualification(
            ReadQualificationDocument(qualification_paths.implementation_build),
            std::move(installed_target),
            ReadQualificationDocument(qualification_paths.runner_contract),
            std::move(runner_executable),
            ReadQualificationDocument(
                qualification_paths.implementation_evidence),
            ReadQualificationDocument(
                qualification_paths.substitution_evidence));
    const std::span<
        const deployment::detail::StopQualificationEvidenceCandidate>
        stop_qualifications(&stop_qualification, 1U);
    const std::array<deployment::QualifiedRuntimeProfile, 1U> profiles{
        *source.native_execution_runtime_profile};
    const auto predecessor_source =
        deployment::detail::derive_cell_scoped_canonical_empty_deployment(
            source.qualified.deployment(), profiles);
    const auto candidate_source =
        NamedRuntimeOnlyCandidate(predecessor_source, profiles);
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
            {}, catalogs, raw_artifacts, bundles, stop_qualifications);
    auto candidate = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            CompileEmpty(candidate_source, profiles), manifests, input_source,
            {}, catalogs, raw_artifacts, bundles, stop_qualifications);
    auto physical_candidate = deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            source.qualified, manifests, input_source,
            source.structural_preparations(), catalogs, raw_artifacts, bundles,
            stop_qualifications);
    if (predecessor == nullptr || candidate == nullptr ||
        physical_candidate == nullptr ||
        predecessor->value().cell_graph.runtime_units !=
            candidate->value().cell_graph.runtime_units ||
        predecessor->value().compiled_products.runtime_unit_products !=
            candidate->value().compiled_products.runtime_unit_products ||
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
