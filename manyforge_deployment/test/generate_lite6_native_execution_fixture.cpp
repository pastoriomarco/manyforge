#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include <vector>

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

enum class FixtureKind {
  kApplication,
  kCCLCProduct,
  kCCLCReadiness,
};

struct GeneratorArguments final {
  std::filesystem::path output_root;
  FixtureKind fixture_kind{FixtureKind::kApplication};
};

[[nodiscard]] GeneratorArguments parse_arguments(int argc, char** argv) {
  if (argc < 3 || argc % 2 == 0) {
    throw std::invalid_argument(
        "usage: generate_lite6_native_execution_fixture "
        "--output-root PATH "
        "[--fixture-kind cclc-product|cclc-readiness]");
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
  const auto fixture_kind = take("--fixture-kind");
  FixtureKind selected_kind = FixtureKind::kApplication;
  if (fixture_kind.has_value()) {
    if (fixture_kind->string() == "cclc-product") {
      selected_kind = FixtureKind::kCCLCProduct;
    } else if (fixture_kind->string() == "cclc-readiness") {
      selected_kind = FixtureKind::kCCLCReadiness;
    } else {
      throw std::invalid_argument(
          "native execution fixture kind is unsupported");
    }
  }
  const std::size_t expected_argument_count =
      1U + (fixture_kind.has_value() ? 1U : 0U);
  if (values.size() != expected_argument_count) {
    throw std::invalid_argument(
        "native execution fixture argument set is unknown, partial, or "
        "contains an unsupported option");
  }
  return GeneratorArguments{*output_root, selected_kind};
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

[[nodiscard]] CanonicalValue make_program_root(
    const deployment::RetainedBundle& bundle, const bool include_gripper) {
  CanonicalValue::Object target_roles{
      {"arm",
       CanonicalValue(CanonicalValue::Object{
           {"component_role", CanonicalValue("robot")},
           {"required_capabilities",
            CanonicalValue(CanonicalValue::Array{
                CanonicalValue(CanonicalValue::Object{
                    {"capability_id",
                     CanonicalValue("execution.trajectory.execute")},
                    {"parameters", CanonicalValue(CanonicalValue::Object{})},
                    {"disposition", CanonicalValue("required")},
                }),
                CanonicalValue(CanonicalValue::Object{
                    {"capability_id",
                     CanonicalValue("planning.motion.joint_goal")},
                    {"parameters", CanonicalValue(CanonicalValue::Object{})},
                    {"disposition", CanonicalValue("required")},
                }),
            })},
           {"required_frame_roles",
            CanonicalValue(CanonicalValue::Array{CanonicalValue("base"),
                                                  CanonicalValue("tcp")})},
           {"required_resource_roles",
            CanonicalValue(CanonicalValue::Array{CanonicalValue("joints")})},
           {"required_collision_body_roles",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue("arm_collision")})},
           {"required_model_roles",
            CanonicalValue(CanonicalValue::Array{CanonicalValue("arm_model")})},
       })},
  };
  if (include_gripper) {
    target_roles.emplace_back(
        "gripper",
        CanonicalValue(CanonicalValue::Object{
            {"component_role", CanonicalValue("robot")},
            {"required_capabilities",
             CanonicalValue(CanonicalValue::Array{CanonicalValue(
                 CanonicalValue::Object{
                     {"capability_id",
                      CanonicalValue("tool.gripper.command")},
                     {"parameters", CanonicalValue(CanonicalValue::Object{})},
                     {"disposition", CanonicalValue("required")},
                 })})},
            {"required_frame_roles",
             CanonicalValue(CanonicalValue::Array{CanonicalValue("tcp")})},
            {"required_resource_roles",
             CanonicalValue(
                 CanonicalValue::Array{CanonicalValue("fingers")})},
            {"required_collision_body_roles",
             CanonicalValue(
                 CanonicalValue::Array{CanonicalValue("gripper_collision")})},
            {"required_model_roles",
             CanonicalValue(
                 CanonicalValue::Array{CanonicalValue("arm_model")})},
        }));
  }
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
      {"target_roles", CanonicalValue(std::move(target_roles))},
      {"tree", CanonicalValue(CanonicalValue::Object{
                   {"node_id", CanonicalValue("sequence")},
                   {"instance_id", CanonicalValue("root")},
                   {"parameters", CanonicalValue(CanonicalValue::Object{})},
                   {"children", CanonicalValue(CanonicalValue::Array{})},
               })},
  });
}

[[nodiscard]] std::string make_program_bytes(
    const deployment::RetainedBundle& bundle, const bool include_gripper) {
  CanonicalValue root = make_program_root(bundle, include_gripper);
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

    auto materialized = [&]() {
      if (arguments.fixture_kind == FixtureKind::kCCLCProduct) {
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
                package_root, arm_semantic.value().value(),
                gripper_semantic.value().value());
      }
      if (arguments.fixture_kind == FixtureKind::kCCLCReadiness) {
        return deployment::test::
            materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
                package_root);
      }
      return deployment::test::
          materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
              package_root);
    }();
    if (materialized.bundle == nullptr) {
      throw std::logic_error("native execution fixture has no bundle");
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

    const bool include_gripper =
        arguments.fixture_kind != FixtureKind::kCCLCReadiness;
    const std::string program_bytes =
        make_program_bytes(bundle, include_gripper);
    const auto program = deployment::verify_program_binding_contract(program_bytes, bundle);
    deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
    std::vector<core::ProgramTargetRoleAssignmentDTO> target_assignments{
        {core::TargetRoleId("arm"),
         core::CommandTargetId(
             std::string(deployment::test::kLite6ArmTargetId))},
    };
    if (include_gripper) {
      target_assignments.push_back(
          {core::TargetRoleId("gripper"),
           core::CommandTargetId(
               std::string(deployment::test::kLite6GripperTargetId))});
    }
    auto binding = deployment::compile_program_binding(
        *program,
        core::ProgramRoleAssignmentDTO{
            {core::ProgramComponentRoleAssignmentDTO{
                core::ComponentRoleId("robot"),
                core::ControllableComponentId(
                    std::string(deployment::test::kLite6ControllableComponentId))}},
            std::move(target_assignments)},
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
        {"cclc_product",
         CanonicalValue(arguments.fixture_kind == FixtureKind::kCCLCProduct)},
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
