#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_lite6_product/lifecycle_profile.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "native_execution_runtime_unit_internal.hpp"

namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using deployment::CanonicalValue;

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

struct GeneratorArguments final {
  std::filesystem::path output_root;
  std::optional<std::filesystem::path> test_build_identity_target;
  std::optional<StopQualificationInputPaths> stop_qualification;
  bool cclc_product{};
};

[[nodiscard]] GeneratorArguments parse_arguments(int argc, char** argv) {
  if (argc < 3 || argc % 2 == 0) {
    throw std::invalid_argument(
        "usage: generate_lite6_native_execution_fixture "
        "--output-root PATH [--fixture-kind cclc-product] "
        "[--test-build-identity-target PATH | "
        "--stop-implementation-build PATH --stop-installed-target PATH "
        "--stop-runner-contract PATH --stop-runner-executable PATH "
        "--stop-implementation-evidence PATH "
        "--stop-substitution-evidence PATH]");
  }

  std::map<std::string, std::filesystem::path, std::less<>> values;
  for (int index = 1; index < argc; index += 2) {
    if (argv[index] == nullptr || argv[index + 1] == nullptr ||
        std::string_view(argv[index]).empty() ||
        std::string_view(argv[index + 1]).empty() ||
        !values.emplace(argv[index], argv[index + 1]).second) {
      throw std::invalid_argument(
          "native execution fixture arguments are malformed or duplicated");
    }
  }

  const auto take = [&](std::string_view name)
      -> std::optional<std::filesystem::path> {
    const auto found = values.find(name);
    if (found == values.end()) {
      return std::nullopt;
    }
    return found->second;
  };
  const auto output_root = take("--output-root");
  if (!output_root.has_value()) {
    throw std::invalid_argument(
        "native execution fixture output root is missing");
  }
  const auto test_identity = take("--test-build-identity-target");
  const auto fixture_kind = take("--fixture-kind");
  const bool cclc_product =
      fixture_kind.has_value() && fixture_kind->string() == "cclc-product";
  const std::array qualification_names{
      std::string_view("--stop-implementation-build"),
      std::string_view("--stop-installed-target"),
      std::string_view("--stop-runner-contract"),
      std::string_view("--stop-runner-executable"),
      std::string_view("--stop-implementation-evidence"),
      std::string_view("--stop-substitution-evidence"),
  };
  std::array<std::optional<std::filesystem::path>, 6U> qualification_values;
  std::size_t qualification_count = 0U;
  for (std::size_t index = 0U; index < qualification_names.size(); ++index) {
    qualification_values[index] = take(qualification_names[index]);
    qualification_count += qualification_values[index].has_value() ? 1U : 0U;
  }
  const std::size_t expected_argument_count =
      1U + (fixture_kind.has_value() ? 1U : 0U) +
      (test_identity.has_value() ? 1U : 0U) + qualification_count;
  if (values.size() != expected_argument_count ||
      (fixture_kind.has_value() && !cclc_product) ||
      (qualification_count != 0U &&
       qualification_count != qualification_names.size()) ||
      (test_identity.has_value() && qualification_count != 0U) ||
      (cclc_product && qualification_count == 0U)) {
    throw std::invalid_argument(
        "native execution fixture argument set is unknown, partial, or "
        "combines authoritative and test-only identity modes");
  }

  GeneratorArguments result{*output_root, test_identity, std::nullopt,
                            cclc_product};
  if (qualification_count != 0U) {
    result.stop_qualification = StopQualificationInputPaths{
        *qualification_values[0], *qualification_values[1],
        *qualification_values[2], *qualification_values[3],
        *qualification_values[4], *qualification_values[5]};
  }
  return result;
}

[[nodiscard]] std::string read_exact_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read native execution fixture input: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string read_qualification_document(
    const std::filesystem::path& path) {
  std::string bytes = read_exact_file(path);
  if (bytes.empty() ||
      bytes.size() >
          deployment::detail::kMaxStopQualificationEvidenceBytes) {
    throw std::runtime_error(
        "native Stop qualification document violates its byte bound: " +
        path.string());
  }
  return bytes;
}

[[nodiscard]] const core::RetainedArtifactRefDTO& require_exact_artifact(
    const core::RetainedProductDTO& product, std::string_view role,
    std::string_view media_type, std::string_view expected_bytes) {
  const auto found = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [&](const auto& artifact) {
        return artifact.artifact_role_id.value() == role;
      });
  if (found == product.artifact_refs.end() ||
      found->media_type != media_type || expected_bytes.empty() ||
      found->byte_size != expected_bytes.size() ||
      found->content_digest.value() !=
          deployment::sha256_digest(expected_bytes)) {
    throw std::logic_error(
        "Stop-qualified retained fixture lost an exact artifact: " +
        std::string(role));
  }
  return *found;
}

void require_stop_qualified_durable_projection(
    const deployment::RetainedBundle& bundle,
    const deployment::detail::StopQualificationEvidenceCandidate& candidate,
    const std::size_t expected_control_domains = 1U,
    const bool expect_substitution = true) {
  if (!candidate.implementation_qualification_package_bytes.has_value() ||
      !candidate.controlled_substitution_package_bytes.has_value()) {
    throw std::logic_error(
        "Stop-qualified retained fixture lacks one evidence family");
  }
  const auto& compiled = bundle.value().compiled_products;
  if (compiled.control_domain_products.size() != expected_control_domains ||
      std::any_of(compiled.control_domain_products.begin(),
                  compiled.control_domain_products.end(), [](const auto &row) {
                    return row.products.size() != 1U;
                  })) {
    throw std::logic_error(
        "Stop-qualified retained fixture has the wrong control-domain "
        "cardinality");
  }
  for (const auto &domain : compiled.control_domain_products) {
    const auto &control = domain.products.front();
    if (control.product_contract_id.value() !=
            deployment::detail::kControlDomainControlContractId ||
        control.artifact_refs.size() != (expect_substitution ? 3U : 2U)) {
      throw std::logic_error(
          "Stop-qualified retained fixture has the wrong control closure");
    }
    const auto main = std::find_if(
        control.artifact_refs.begin(), control.artifact_refs.end(),
        [](const auto &artifact) {
          return artifact.artifact_role_id.value() ==
                 deployment::detail::kControlDomainControlArtifactRole;
        });
    if (main == control.artifact_refs.end() ||
        main->media_type != deployment::detail::kControlDomainControlMediaType) {
      throw std::logic_error(
          "Stop-qualified retained fixture lost its primary control artifact");
    }
    static_cast<void>(require_exact_artifact(
        control, core::kStopImplementationQualificationArtifactRole,
        core::kStopImplementationQualificationMediaType,
        *candidate.implementation_qualification_package_bytes));
    if (expect_substitution) {
      static_cast<void>(require_exact_artifact(
          control,
          core::kControlledStopSubstitutionQualificationArtifactRole,
          core::kControlledStopSubstitutionQualificationMediaType,
          *candidate.controlled_substitution_package_bytes));
    }
  }

  if (compiled.runtime_unit_products.size() != 1U) {
    throw std::logic_error(
        "Stop-qualified retained fixture has no sole runtime group");
  }
  const auto& runtime_group = compiled.runtime_unit_products.front();
  const std::string expected_product_id =
      deployment::detail::native_execution_runtime_product_id(
          runtime_group.runtime_unit_id);
  const auto runtime = std::find_if(
      runtime_group.products.begin(), runtime_group.products.end(),
      [&](const auto& product) {
        return product.product_id.value() == expected_product_id;
      });
  if (runtime == runtime_group.products.end() ||
      runtime->product_contract_id.value() !=
          deployment::detail::kNativeExecutionRuntimeProductContractId) {
    throw std::logic_error(
        "Stop-qualified retained fixture lost its exact runtime product");
  }
  static_cast<void>(require_exact_artifact(
      *runtime,
      deployment::detail::kNativeExecutionRuntimeBuildIdentityArtifactRole,
      deployment::detail::kNativeExecutionRuntimeBuildIdentityMediaType,
      candidate.implementation_build_closure_bytes));
}

[[nodiscard]] CanonicalValue build_input(std::string id,
                                         std::string_view digest) {
  return CanonicalValue(CanonicalValue::Object{
      {"input_id", CanonicalValue(std::move(id))},
      {"content_digest", CanonicalValue(std::string(digest))},
  });
}

[[nodiscard]] std::string make_test_build_identity(
    std::string_view installed_target_bytes) {
  if (installed_target_bytes.empty()) {
    throw std::runtime_error("native execution fixture target is empty");
  }
  const std::string target_digest =
      deployment::sha256_digest(installed_target_bytes);
  CanonicalValue root(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(
           deployment::detail::kStopImplementationBuildClosureApiVersion))},
      {"implementation_id",
       CanonicalValue("manyforge.native_execution_host")},
      {"implementation_source_build_fingerprint",
       CanonicalValue(std::string(kZeroDigest))},
      {"source_inputs",
       CanonicalValue(CanonicalValue::Array{
           build_input("source:test_fixture", target_digest)})},
      {"generated_semantic_inputs",
       CanonicalValue(CanonicalValue::Array{
           build_input("generated:test_fixture", target_digest)})},
      {"build_inputs",
       CanonicalValue(CanonicalValue::Array{
           build_input("abi:test_fixture", target_digest),
           build_input("compile:test_fixture", target_digest),
           build_input("link:test_fixture", target_digest),
           build_input("toolchain:test_fixture", target_digest)})},
      {"installed_target",
       CanonicalValue(CanonicalValue::Object{
           {"installed_target_id",
            CanonicalValue("manyforge_native_execution_host")},
           {"byte_size",
            CanonicalValue(static_cast<double>(installed_target_bytes.size()))},
           {"content_digest", CanonicalValue(target_digest)},
       })},
  });
  const std::string fingerprint = deployment::detail::compute_content_identity(
      root, deployment::detail::ContentIdentitySpec{
                .identity_domain =
                    "stop-implementation-source-build-fingerprint",
                .api_version = std::string(
                    deployment::detail::
                        kStopImplementationBuildClosureApiVersion),
                .own_field = "implementation_source_build_fingerprint",
            });
  auto members = root.as_object();
  for (auto& [name, value] : members) {
    if (name == "implementation_source_build_fingerprint") {
      value = CanonicalValue(fingerprint);
      const std::string bytes =
          deployment::canonical_json(CanonicalValue(std::move(members)));
      static_cast<void>(
          deployment::detail::parse_stop_implementation_build_identity(bytes));
      return bytes;
    }
  }
  throw std::logic_error(
      "native execution fixture build identity has no fingerprint field");
}

void write_exact_file(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create native execution fixture file: " + path.string());
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error("cannot write native execution fixture file: " + path.string());
  }
}

[[nodiscard]] CanonicalValue make_program_root(const deployment::RetainedBundle& bundle) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.program.v1")},
      {"program_id", CanonicalValue("i6_native_joint_motion_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue("I6 addressed native joint motion")},
      {"catalog", CanonicalValue(CanonicalValue::Object{
                      {"api_version", CanonicalValue("manyforge.node_catalog.v2")},
                      {"content_version",
                       CanonicalValue(bundle.value().catalogs.node_environment.content_version)},
                  })},
      {"component_roles", CanonicalValue(CanonicalValue::Object{
                              {"robot", CanonicalValue(CanonicalValue::Object{})},
                          })},
      {"target_roles",
       CanonicalValue(CanonicalValue::Object{
           {"arm",
            CanonicalValue(CanonicalValue::Object{
                {"component_role", CanonicalValue("robot")},
                {"required_capabilities",
                 CanonicalValue(CanonicalValue::Array{
                     CanonicalValue(CanonicalValue::Object{
                         {"capability_id", CanonicalValue("execution.trajectory.execute")},
                         {"parameters", CanonicalValue(CanonicalValue::Object{})},
                         {"disposition", CanonicalValue("required")},
                     }),
                     CanonicalValue(CanonicalValue::Object{
                         {"capability_id", CanonicalValue("planning.motion.joint_goal")},
                         {"parameters", CanonicalValue(CanonicalValue::Object{})},
                         {"disposition", CanonicalValue("required")},
                     }),
                 })},
                {"required_frame_roles", CanonicalValue(CanonicalValue::Array{
                                             CanonicalValue("base"), CanonicalValue("tcp")})},
                {"required_resource_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("joints")})},
                {"required_collision_body_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("arm_collision")})},
                {"required_model_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("arm_model")})},
            })},
           {"gripper",
            CanonicalValue(CanonicalValue::Object{
                {"component_role", CanonicalValue("robot")},
                {"required_capabilities",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                     {"capability_id", CanonicalValue("tool.gripper.command")},
                     {"parameters", CanonicalValue(CanonicalValue::Object{})},
                     {"disposition", CanonicalValue("required")},
                 })})},
                {"required_frame_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("tcp")})},
                {"required_resource_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("fingers")})},
                {"required_collision_body_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("gripper_collision")})},
                {"required_model_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("arm_model")})},
            })},
       })},
      {"tree", CanonicalValue(CanonicalValue::Object{
                   {"node_id", CanonicalValue("sequence")},
                   {"instance_id", CanonicalValue("root")},
                   {"parameters", CanonicalValue(CanonicalValue::Object{})},
                   {"children", CanonicalValue(CanonicalValue::Array{})},
               })},
  });
}

[[nodiscard]] std::string make_program_bytes(const deployment::RetainedBundle& bundle) {
  CanonicalValue root = make_program_root(bundle);
  const std::string revision =
      deployment::detail::compute_content_identity(root, deployment::detail::ContentIdentitySpec{
                                                             .identity_domain = "program-revision",
                                                             .api_version = "manyforge.program.v1",
                                                             .own_field = "program_revision",
                                                         });
  auto members = root.as_object();
  for (auto& [name, value] : members) {
    if (name == "program_revision") {
      value = CanonicalValue(revision);
      return deployment::canonical_json(CanonicalValue(std::move(members)));
    }
  }
  throw std::logic_error("native execution fixture Program has no revision");
}

[[nodiscard]] core::LogicalSceneSnapshotDTO make_empty_scene(const core::CellId& cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{
      core::SceneRevision{core::logical_scene_identity::MintSceneId(
                              {cell_id, core::BootstrapId("i6_native_execution_fixture"),
                               core::BootstrapGeneration(1U)}),
                          1U, core::ContentDigest(std::string(kZeroDigest))},
      cell_id,
      0U,
      0U,
      0U,
      std::move(policy),
      {},
      {},
  };
  snapshot.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
  if (!snapshot.locally_valid()) {
    throw std::logic_error("native execution fixture scene is invalid");
  }
  return snapshot;
}

[[nodiscard]] CanonicalValue scene_revision(const core::SceneRevision& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"scene_id", CanonicalValue(value.scene_id.value())},
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"content_hash", CanonicalValue(value.content_hash.value())},
  });
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto& output_root = arguments.output_root;
    std::filesystem::create_directories(output_root);
    const auto package_root = output_root / "retained-package";
    const auto manager_state_root = output_root / "manager-state";
    std::filesystem::create_directories(package_root);
    std::filesystem::create_directories(manager_state_root);

    std::optional<std::string> test_build_identity;
    if (arguments.test_build_identity_target.has_value()) {
      test_build_identity = make_test_build_identity(
          read_exact_file(*arguments.test_build_identity_target));
    }
    std::optional<deployment::detail::StopQualificationEvidenceCandidate>
        stop_candidate;
    if (arguments.stop_qualification.has_value()) {
      const auto& paths = *arguments.stop_qualification;
      std::string installed_target = read_exact_file(paths.installed_target);
      std::string runner_executable = read_exact_file(paths.runner_executable);
      if (installed_target.empty() || runner_executable.empty()) {
        throw std::runtime_error(
            "native Stop qualification executable input is empty");
      }
      stop_candidate.emplace(
          read_qualification_document(paths.implementation_build),
          std::move(installed_target),
          read_qualification_document(paths.runner_contract),
          std::move(runner_executable),
          read_qualification_document(paths.implementation_evidence),
          read_qualification_document(paths.substitution_evidence));
    }
    auto materialized = [&]() {
      if (stop_candidate.has_value()) {
        if (arguments.cclc_product) {
          const auto arm_semantic = manyforge::lite6_product::
              compute_lite6_lifecycle_catalog_semantic_digest();
          const auto gripper_semantic = manyforge::lite6_product::
              compute_lite6_gripper_lifecycle_catalog_semantic_digest();
          if (!arm_semantic.ok() || !gripper_semantic.ok()) {
            throw std::logic_error(
                "native CCLC lifecycle semantic digest is unavailable");
          }
          return deployment::test::
              materialize_lite6_native_cclc_product_retained_consumer_fixture(
                  package_root,
                  std::span<const deployment::detail::
                                StopQualificationEvidenceCandidate>(
                      &*stop_candidate, 1U),
                  arm_semantic.value().value(),
                  gripper_semantic.value().value());
        }
        return deployment::test::
            materialize_lite6_stop_qualified_native_motion_retained_consumer_fixture(
                package_root,
                std::span<const deployment::detail::
                              StopQualificationEvidenceCandidate>(
                    &*stop_candidate, 1U));
      }
      if (test_build_identity.has_value()) {
        return deployment::test::
            materialize_lite6_test_build_identified_native_motion_arm_gripper_retained_consumer_fixture(
                package_root, *test_build_identity);
      }
      return deployment::test::
          materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
              package_root);
    }();
    if (materialized.bundle == nullptr) {
      throw std::logic_error("native execution fixture has no bundle");
    }
    if (stop_candidate.has_value() && !arguments.cclc_product) {
      const core::RetainedBundleId retained_bundle_id =
          materialized.bundle->retained_bundle_id();
      const std::string retained_bundle_bytes{
          materialized.bundle->canonical_bytes()};
      materialized.bundle.reset();
      const deployment::FilesystemVerifiedRetainedBundleSource durable_source(
          package_root);
      const auto durable = durable_source.get_verified(retained_bundle_id);
      if (durable == nullptr ||
          durable->canonical_bytes() != retained_bundle_bytes) {
        throw std::logic_error(
            "Stop-qualified retained fixture failed fresh durable readback");
      }
      require_stop_qualified_durable_projection(*durable, *stop_candidate);
      const CanonicalValue manifest(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(
               "manyforge.i7.stop-qualified-retained-fixture.v1")},
          {"retained_bundle_id",
           CanonicalValue(durable->retained_bundle_id().value())},
          {"implementation_build_digest",
           CanonicalValue(deployment::sha256_digest(
               stop_candidate->implementation_build_closure_bytes))},
          {"qualification_runner_contract_document_digest",
           CanonicalValue(deployment::sha256_digest(
               stop_candidate->qualification_runner_contract_bytes))},
          {"implementation_evidence_digest",
           CanonicalValue(deployment::sha256_digest(
               *stop_candidate
                    ->implementation_qualification_package_bytes))},
          {"substitution_evidence_digest",
           CanonicalValue(deployment::sha256_digest(
               *stop_candidate->controlled_substitution_package_bytes))},
      });
      write_exact_file(output_root / "manifest.json",
                       deployment::canonical_json(manifest));
      std::cout << (output_root / "manifest.json").string() << '\n';
      return 0;
    }
    if (stop_candidate.has_value()) {
      require_stop_qualified_durable_projection(*materialized.bundle,
                                                *stop_candidate, 2U, false);
    }
    const auto& bundle = *materialized.bundle;
    const auto& products = bundle.value().compiled_products;
    if (products.symbol_role_maps.size() != 1U) {
      throw std::logic_error("native execution fixture has no sole model map");
    }
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(package_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle.retained_bundle_id(), products.symbol_role_maps.front().product_id});
    if (!model_map.has_value()) {
      throw std::logic_error("native execution fixture model map is absent");
    }

    const std::string program_bytes = make_program_bytes(bundle);
    const auto program = deployment::verify_program_binding_contract(program_bytes, bundle);
    deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
    auto binding = deployment::compile_program_binding(
        *program,
        core::ProgramRoleAssignmentDTO{
            {core::ProgramComponentRoleAssignmentDTO{
                core::ComponentRoleId("robot"),
                core::ControllableComponentId(
                    std::string(deployment::test::kLite6ControllableComponentId))}},
            {core::ProgramTargetRoleAssignmentDTO{
                 core::TargetRoleId("arm"),
                 core::CommandTargetId(std::string(deployment::test::kLite6ArmTargetId))},
             core::ProgramTargetRoleAssignmentDTO{
                 core::TargetRoleId("gripper"),
                 core::CommandTargetId(std::string(deployment::test::kLite6GripperTargetId))}}},
        bundle, *model_map, catalog_store);
    deployment::FilesystemProgramRepository program_repository(package_root);
    if (!program_repository.put_if_absent(program, bundle)) {
      throw std::logic_error("native execution fixture Program already exists");
    }
    deployment::FilesystemProgramBindingRepository binding_repository(package_root);
    if (!binding_repository.put_if_absent(binding, bundle, *model_map)) {
      throw std::logic_error("native execution fixture Program Binding already exists");
    }

    const auto empty_scene = make_empty_scene(bundle.value().cell_graph.cell_id);
    const core::ProjectionRevisionDTO projection{bundle.retained_bundle_id(),
                                                 bundle.value().cell_graph.graph_revision,
                                                 empty_scene.revision};
    component_manager::SqliteActiveBundlePublicationStore publication_store(
        manager_state_root / std::string(component_manager::kActiveBundlePublicationStoreFilename),
        bundle.value().cell_graph.cell_id);
    const auto receipt =
        publication_store.PublishActiveBundle(core::ActiveBundlePublicationRequestDTO{
            core::BootstrapPublicationTransactionDTO{
                core::BootstrapId("i6_native_execution_fixture"), core::BootstrapGeneration(1U)},
            projection,
            bundle.retained_bundle_id(),
            bundle.value().cell_graph.graph_revision,
            std::nullopt,
            core::ContentDigest(deployment::sha256_digest("i6-native-fixture-materialization")),
            core::ContentDigest(deployment::sha256_digest("i6-native-fixture-retired-surfaces")),
        });
    publication_store.Close();

    const auto program_path = output_root / "program.json";
    write_exact_file(program_path, program_bytes);
    write_exact_file(output_root / "program_binding.json",
                     std::string(binding->canonical_bytes()));
    const CanonicalValue manifest(CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.i6.native-execution-fixture.v1")},
        {"cclc_product", CanonicalValue(arguments.cclc_product)},
        {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
        {"retained_bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
        {"graph_revision", CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
        {"scene_revision", scene_revision(empty_scene.revision)},
        {"active_publication_generation",
         CanonicalValue(static_cast<double>(receipt.publication_generation.value()))},
        {"program_id", CanonicalValue(program->value().program_id.value())},
        {"program_revision", CanonicalValue(program->value().program_revision.value())},
        {"program_binding_id", CanonicalValue(binding->program_binding_id().value())},
        {"component_role_id", CanonicalValue("robot")},
        {"target_role_id", CanonicalValue("arm")},
        {"gripper_target_role_id", CanonicalValue("gripper")},
        {"execution_capability_id", CanonicalValue("execution.trajectory.execute")},
        {"planning_capability_ids",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue("planning.motion.cartesian_path"),
             CanonicalValue("planning.motion.joint_goal"),
             CanonicalValue("planning.motion.pose_goal"),
         })},
        {"gripper_capability_id", CanonicalValue("tool.gripper.command")},
        {"gripper_command_schema_id",
         CanonicalValue(std::string(core::kGripperPositionCommandSchemaId))},
        {"gripper_command_schema_revision",
         CanonicalValue(std::string(core::kGripperPositionCommandSchemaRevision))},
        {"source_joint_names",
         CanonicalValue(CanonicalValue::Array{CanonicalValue("joint1"), CanonicalValue("joint2"),
                                              CanonicalValue("joint3"), CanonicalValue("joint4"),
                                              CanonicalValue("joint5"), CanonicalValue("joint6")})},
    });
    write_exact_file(output_root / "manifest.json", deployment::canonical_json(manifest));
    std::cout << (output_root / "manifest.json").string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
