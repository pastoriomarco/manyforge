#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "content_identity_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

namespace {

struct Arguments final {
  std::filesystem::path package_root;
  std::filesystem::path bundle_id_output;
};

[[nodiscard]] Arguments ParseArguments(const int argc, char **argv) {
  if (argc != 5 || std::string_view(argv[1]) != "--package-root" ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[3]) != "--bundle-id-output" ||
      std::string_view(argv[4]).empty()) {
    throw std::invalid_argument(
        "usage: generate_assistant_authoring_package --package-root <path> "
        "--bundle-id-output <path>");
  }
  return {std::filesystem::path(argv[2]), std::filesystem::path(argv[4])};
}

void WriteBundleId(const std::filesystem::path &path,
                   const std::string_view bundle_id) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create assistant fixture bundle-ID file");
  }
  stream << bundle_id << '\n';
  stream.flush();
  if (!stream) {
    throw std::runtime_error("cannot write assistant fixture bundle-ID file");
  }
}

[[nodiscard]] std::string
ProgramBytes(const deployment::RetainedBundle &bundle,
             const std::string_view name = "Program Apply fixture") {
  constexpr std::string_view kZeroDigest =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  deployment::CanonicalValue root(deployment::CanonicalValue::Object{
      {"api_version", deployment::CanonicalValue("manyforge.program.v1")},
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
      {"name", deployment::CanonicalValue(std::string(name))},
      {"program_id", deployment::CanonicalValue("program_apply_fixture")},
      {"program_profile", deployment::CanonicalValue("composable-cell-v1")},
      {"program_revision",
       deployment::CanonicalValue(std::string(kZeroDigest))},
      {"target_roles",
       deployment::CanonicalValue(deployment::CanonicalValue::Object{
           {"arm",
            deployment::CanonicalValue(deployment::CanonicalValue::Object{
                {"component_role", deployment::CanonicalValue("robot")},
                {"required_capabilities",
                 deployment::CanonicalValue(deployment::CanonicalValue::Array{
                     deployment::CanonicalValue(
                         deployment::CanonicalValue::Object{
                             {"capability_id", deployment::CanonicalValue(
                                                   "state.observed.current")},
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
                     deployment::CanonicalValue("program_apply_fixture_timer")},
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
                .api_version = "manyforge.program.v1",
                .own_field = "program_revision",
            });
  auto members = root.as_object();
  for (auto &[field, value] : members) {
    if (field == "name") {
      value = deployment::CanonicalValue(std::string(name));
    } else if (field == "program_revision") {
      value = deployment::CanonicalValue(revision);
    }
  }
  return deployment::canonical_json(
      deployment::CanonicalValue(std::move(members)));
}

void PublishProgramScenario(
    const std::filesystem::path &package_root,
    const std::shared_ptr<const deployment::RetainedBundle> &bundle) {
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error("Program Apply fixture has no sole model map");
  }
  const deployment::FilesystemVerifiedRetainedModelMapSource model_maps(
      package_root);
  auto model_map = model_maps.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      bundle->value().compiled_products.symbol_role_maps.front().product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("Program Apply fixture model map did not reopen");
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
          .description = "Native Program Apply fixture",
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
    throw std::logic_error("Program Apply fixture catalog was not published");
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    std::filesystem::create_directories(arguments.package_root);
    const auto fixture = manyforge::deployment::test::
        materialize_lite6_composer_assistant_retained_consumer_fixture(
            arguments.package_root);
    PublishProgramScenario(arguments.package_root, fixture.bundle);
    WriteBundleId(arguments.bundle_id_output,
                  fixture.bundle->retained_bundle_id().value());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
