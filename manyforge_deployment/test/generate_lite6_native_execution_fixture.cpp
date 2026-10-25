#include <algorithm>
#include <array>
#include <filesystem>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <fstream>
#include <iostream>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#ifdef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
#include "robotiq_gripper_only_retention_fixture.hpp"
#endif
#ifdef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT
#include "ur5_arm_only_retention_fixture.hpp"
#endif
#include "../../manyforge_roboplan/src/retained_ros2_control_lifecycle_plans_internal.hpp"

namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using deployment::CanonicalValue;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

enum class FixtureKind {
  kApplicationRos,
  kCCLCProductRos,
  kCCLCReadinessRos,
  kUr5ArmOnlyRos,
  kProjectionSuccessorRos,
  // Existing gripper-only path is the explicit pneumatic commanded fixture.
  kGripperOnly,
  kGripperOnlyNoneZero,
  kGripperOnlyNoneDwell,
  kRobotiqGripperOnlyPosition,
};

struct GeneratorArguments final {
  std::filesystem::path output_root;
  FixtureKind fixture_kind{FixtureKind::kApplicationRos};
};

[[nodiscard]] GeneratorArguments parse_arguments(int argc, char **argv) {
  if (argc < 3 || argc % 2 == 0) {
    throw std::invalid_argument(
        "usage: generate_lite6_native_execution_fixture "
        "--output-root PATH "
        "[--fixture-kind "
        "application-ros|cclc-product-ros|cclc-readiness-ros|ur5-arm-only-ros|"
        "projection-successor-ros|gripper-only|gripper-only-none-zero|"
        "gripper-only-none-dwell|gripper-only-robotiq-position]");
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

  const auto take =
      [&](std::string_view name) -> std::optional<std::filesystem::path> {
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
  FixtureKind selected_kind = FixtureKind::kApplicationRos;
  if (fixture_kind.has_value()) {
    if (fixture_kind->string() == "application-ros") {
      selected_kind = FixtureKind::kApplicationRos;
    } else if (fixture_kind->string() == "cclc-product-ros") {
      selected_kind = FixtureKind::kCCLCProductRos;
    } else if (fixture_kind->string() == "cclc-readiness-ros") {
      selected_kind = FixtureKind::kCCLCReadinessRos;
    } else if (fixture_kind->string() == "ur5-arm-only-ros") {
      selected_kind = FixtureKind::kUr5ArmOnlyRos;
    } else if (fixture_kind->string() == "projection-successor-ros") {
      selected_kind = FixtureKind::kProjectionSuccessorRos;
    } else if (fixture_kind->string() == "gripper-only") {
      selected_kind = FixtureKind::kGripperOnly;
    } else if (fixture_kind->string() == "gripper-only-robotiq-position") {
      selected_kind = FixtureKind::kRobotiqGripperOnlyPosition;
    } else if (fixture_kind->string() == "gripper-only-none-zero") {
      selected_kind = FixtureKind::kGripperOnlyNoneZero;
    } else if (fixture_kind->string() == "gripper-only-none-dwell") {
      selected_kind = FixtureKind::kGripperOnlyNoneDwell;
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

void write_exact_file(const std::filesystem::path &path,
                      std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot create native execution fixture file: " +
                             path.string());
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error("cannot write native execution fixture file: " +
                             path.string());
  }
}

[[nodiscard]] CanonicalValue make_program_root(const deployment::RetainedBundle& bundle,
                                               const bool include_arm,
                                               const bool include_gripper,
                                               const bool robotiq_position = false) {
  CanonicalValue::Object target_roles;
  if (include_arm) target_roles.emplace_back(
      "arm",
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
       }));
  if (include_gripper) {
    CanonicalValue::Array gripper_capabilities{CanonicalValue(CanonicalValue::Object{
        {"capability_id", CanonicalValue("tool.gripper.command")},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"disposition", CanonicalValue("required")},
    })};
    if (robotiq_position) gripper_capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue("tool.gripper.feedback")},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"disposition", CanonicalValue("required")},
    });
    target_roles.emplace_back(
        "gripper",
        CanonicalValue(CanonicalValue::Object{
            {"component_role", CanonicalValue("robot")},
            {"required_capabilities", CanonicalValue(std::move(gripper_capabilities))},
            {"required_frame_roles", CanonicalValue(CanonicalValue::Array{CanonicalValue("tcp")})},
            {"required_resource_roles",
             CanonicalValue(CanonicalValue::Array{CanonicalValue("fingers")})},
            {"required_collision_body_roles",
             CanonicalValue(CanonicalValue::Array{CanonicalValue("gripper_collision")})},
            {"required_model_roles",
             CanonicalValue(CanonicalValue::Array{CanonicalValue(
                 robotiq_position ? "gripper_model" : "arm_model")})},
        }));
  }
  CanonicalValue::Array children;
  if (robotiq_position) {
    if (include_arm || !include_gripper)
      throw std::logic_error("Robotiq Position Program requires only its scalar target");
    for (const auto preset : {"closed", "open"}) {
      children.emplace_back(CanonicalValue::Object{
          {"node_id", CanonicalValue("gripper.command_position")},
          {"instance_id", CanonicalValue(std::string("robotiq_") + preset)},
          {"parameters", CanonicalValue(CanonicalValue::Object{
              {"target_role", CanonicalValue("gripper")},
              {"position", CanonicalValue(CanonicalValue::Object{
                  {"kind", CanonicalValue("named_preset")},
                  {"name", CanonicalValue(preset)},
              })},
              {"command_duration_s", CanonicalValue(1.0)},
          })},
          {"children", CanonicalValue(CanonicalValue::Array{})},
      });
    }
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.program.v2")},
      {"program_id", CanonicalValue(robotiq_position
          ? "i6_robotiq_position_gripper_program" : "i6_native_joint_motion_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue(robotiq_position
          ? "I6 standalone Robotiq Position commands" : "I6 addressed native joint motion")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v2")},
           {"content_version",
            CanonicalValue(
                bundle.value().catalogs.node_environment.content_version)},
       })},
      {"component_roles",
       CanonicalValue(CanonicalValue::Object{
           {"robot", CanonicalValue(CanonicalValue::Object{})},
       })},
      {"target_roles", CanonicalValue(std::move(target_roles))},
      {"execution_groups",
       CanonicalValue(CanonicalValue::Object{
           {"selection.a",
            CanonicalValue(CanonicalValue::Object{
                {"execution_policy", CanonicalValue("independent")}})},
           {"selection.b",
            CanonicalValue(CanonicalValue::Object{
                {"execution_policy", CanonicalValue("independent")}})},
       })},
      {"tree", CanonicalValue(CanonicalValue::Object{
                   {"node_id", CanonicalValue("sequence")},
                   {"instance_id", CanonicalValue("root")},
                   {"execution_groups", CanonicalValue(CanonicalValue::Array{
                                            CanonicalValue("selection.a"),
                                            CanonicalValue("selection.b")})},
                   {"parameters", CanonicalValue(CanonicalValue::Object{})},
                   {"children", CanonicalValue(std::move(children))},
               })},
  });
}

[[nodiscard]] std::string make_program_bytes(const deployment::RetainedBundle& bundle,
                                             const bool include_arm,
                                             const bool include_gripper,
                                             const bool robotiq_position = false) {
  CanonicalValue root = make_program_root(bundle, include_arm, include_gripper,
                                          robotiq_position);
  const std::string revision = deployment::detail::compute_content_identity(
      root, deployment::detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = "manyforge.program.v2",
                .own_field = "program_revision",
            });
  auto members = root.as_object();
  for (auto &[name, value] : members) {
    if (name == "program_revision") {
      value = CanonicalValue(revision);
      return deployment::canonical_json(CanonicalValue(std::move(members)));
    }
  }
  throw std::logic_error("native execution fixture Program has no revision");
}

[[nodiscard]] core::LogicalSceneSnapshotDTO
make_empty_scene(const core::CellId &cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{
      core::SceneRevision{
          core::logical_scene_identity::MintSceneId(
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

[[nodiscard]] CanonicalValue scene_revision(const core::SceneRevision &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"scene_id", CanonicalValue(value.scene_id.value())},
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"content_hash", CanonicalValue(value.content_hash.value())},
  });
}

// This mapper acts on authored compiler inputs before graph revision is minted.
// The structural override supplies the real UR5 asset, roles and joint map.
void map_ur5_arm_only_graph(deployment::CanonicalValue::Object &root) {
  const std::map<std::string, std::string> identities{
      {"lite6_arm", "ur5_arm"},
      {"lite6_fixture_instance", "ur5_fixture_instance"},
      {"lite6_arm_binding", "ur5_arm_binding"},
      {"lite6_arm_domain", "ur5_arm_domain"},
      {"lite6_ros2_control_execution", "ur5_ros2_control_execution"},
      {"lite6_arm_provider_instance", "ur5_arm_provider_instance"},
      {"lite6_commanded_sim_state", "ur5_commanded_sim_state"},
      {"lite6_lite_gripper", "ur5_roboplan"},
      {"lite6_joint_1", "ur5_joint_1"},
      {"lite6_joint_2", "ur5_joint_2"},
      {"lite6_joint_3", "ur5_joint_3"},
      {"lite6_joint_4", "ur5_joint_4"},
      {"lite6_joint_5", "ur5_joint_5"},
      {"lite6_joint_6", "ur5_joint_6"},
  };
  const auto map_value = [&](auto &self, CanonicalValue &value) -> void {
    if (value.is_string()) {
      const auto found = identities.find(value.as_string());
      if (found != identities.end()) value = CanonicalValue(found->second);
    } else if (value.is_array()) {
      auto rows = value.as_array();
      for (auto &row : rows) self(self, row);
      value = CanonicalValue(std::move(rows));
    } else if (value.is_object()) {
      auto members = value.as_object();
      for (auto &[_, member] : members) self(self, member);
      value = CanonicalValue(std::move(members));
    }
  };
  for (auto &[_, value] : root) map_value(map_value, value);
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto &output_root = arguments.output_root;
    std::filesystem::create_directories(output_root);
    const auto package_root = output_root / "retained-package";
    const auto manager_state_root = output_root / "manager-state";
    std::filesystem::create_directories(package_root);
    std::filesystem::create_directories(manager_state_root);

    const bool robotiq_position =
        arguments.fixture_kind == FixtureKind::kRobotiqGripperOnlyPosition;
    const bool gripper_only = robotiq_position ||
                              arguments.fixture_kind == FixtureKind::kGripperOnly ||
                              arguments.fixture_kind == FixtureKind::kGripperOnlyNoneZero ||
                              arguments.fixture_kind == FixtureKind::kGripperOnlyNoneDwell;
    const auto gripper_output = (robotiq_position || arguments.fixture_kind == FixtureKind::kGripperOnly)
                                    ? core::GripperOutputMode::kCommanded
                                    : core::GripperOutputMode::kNone;
    const double gripper_dwell_s = robotiq_position ? 1.0
                                  : arguments.fixture_kind == FixtureKind::kGripperOnly
                                      ? 0.8
                                      : arguments.fixture_kind == FixtureKind::kGripperOnlyNoneDwell
                                            ? 0.25 : 0.0;
    const bool readiness_fixture =
        arguments.fixture_kind == FixtureKind::kCCLCReadinessRos ||
        arguments.fixture_kind == FixtureKind::kUr5ArmOnlyRos;
    auto materialized = [&]() {
      if (robotiq_position) {
#ifdef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
        const auto plans = manyforge::roboplan_backend::detail::
            ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
        const auto digest = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
            core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
        if (!digest.ok()) throw std::logic_error("ROS Robotiq lifecycle anchor is unavailable");
        return deployment::test::materialize_lite6_retained_consumer_fixture_from(
            package_root, deployment::test::make_robotiq_gripper_only_retention_fixture(
                              digest.value().value()));
#else
        throw std::logic_error("Robotiq Position fixture requires its genuine prepared model inputs");
#endif
      }
      if (arguments.fixture_kind == FixtureKind::kUr5ArmOnlyRos) {
#ifdef MANYFORGE_UR5_ARM_ONLY_STRUCTURAL_FIXTURE_ROOT
        const auto plans = manyforge::roboplan_backend::detail::
            ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
        const auto digest = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
            core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
        if (!digest.ok()) throw std::logic_error("ROS UR5 lifecycle anchor is unavailable");
        const auto structural = deployment::test::make_ur5_arm_only_structural_retention_fixture();
        auto controllable = deployment::test::make_lite6_controllable_retention_fixture(
            false, false, 1U, false, true, "ur5_arm_only_ros_cell", false,
            true, "resource", true, digest.value().value(), {}, true,
            false, false, std::nullopt, false, deployment::test::kLite6ArmTargetId,
            std::nullopt, std::nullopt, {}, true, {},
            "manyforge.provider.ros2_control", true, true,
            &structural, map_ur5_arm_only_graph);
        return deployment::test::materialize_lite6_retained_consumer_fixture_from(
            package_root, std::move(controllable));
#else
        throw std::logic_error("UR5 arm-only fixture requires its genuine prepared model inputs");
#endif
      }
      if (arguments.fixture_kind == FixtureKind::kApplicationRos ||
          arguments.fixture_kind == FixtureKind::kCCLCProductRos ||
          arguments.fixture_kind == FixtureKind::kProjectionSuccessorRos) {
        const auto plans = manyforge::roboplan_backend::detail::
            ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
        const auto digest = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
            core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
        if (!digest.ok()) {
          throw std::logic_error("ROS aggregate lifecycle anchor is unavailable");
        }
        return deployment::test::
            materialize_lite6_ros2_control_cclc_product_retained_consumer_fixture(
                package_root, digest.value().value(), false,
                arguments.fixture_kind == FixtureKind::kProjectionSuccessorRos
                    ? std::optional<double>(0.1)
                    : std::nullopt);
      }
      if (arguments.fixture_kind == FixtureKind::kCCLCReadinessRos) {
        const auto plans = manyforge::roboplan_backend::detail::
            ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
        const auto digest = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
            core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
        if (!digest.ok()) {
          throw std::logic_error("ROS arm lifecycle anchor is unavailable");
        }
        return deployment::test::
            materialize_lite6_ros2_control_arm_only_cclc_product_retained_consumer_fixture(
                package_root, digest.value().value());
      }
      if (gripper_only) {
        const auto plans = manyforge::roboplan_backend::detail::
            ros2_control_lifecycle_plans_internal::retained_ros2_control_lifecycle_plans();
        const auto digest = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
            core::ProviderManifestId("manyforge.provider.ros2_control"), plans);
        if (!digest.ok()) {
          throw std::logic_error("ROS gripper lifecycle anchor is unavailable");
        }
        // DX796 H2: all selections are genuine pneumatic model-backed owners;
        // commanded keeps delivery/controller requirements, none has no output.
        return deployment::test::
            materialize_lite6_ros2_control_pneumatic_gripper_retained_consumer_fixture(
                package_root, digest.value().value(), gripper_output, gripper_dwell_s);
      }
      throw std::logic_error("native execution fixture kind is unhandled");
    }();
    if (materialized.bundle == nullptr) {
      throw std::logic_error("native execution fixture has no bundle");
    }
    const auto &bundle = *materialized.bundle;
    if (gripper_only) {
      const auto &graph = bundle.value().cell_graph;
      if (graph.runtime_units.size() != 1U ||
          graph.control_bindings.size() != (robotiq_position ? 2U : 1U) ||
          graph.control_bindings.front().capability_id.value() != "tool.gripper.command") {
        throw std::logic_error("gripper-only producer requires one actual scalar owner and no motion binding");
      }
      const auto config = deployment::ros2_control_execution_runtime_contract::require_configuration(
          bundle, graph.runtime_units.front().runtime_unit_id);
      if (config.gripper_target_configurations.size() != 1U ||
          !config.initial_joint_positions_rad.empty() || !config.motion_target_limits.empty() ||
          config.controller_profiles.size() != 1U ||
          config.controller_profiles.front().mock_profile != std::optional<std::string>(
              robotiq_position ? "ur5e_robotiq_2f_85" : "lite6_lite_gripper")) {
        throw std::logic_error("gripper-only producer lost its exact no-motion Mock configuration");
      }
      const auto &target = config.gripper_target_configurations.front();
      if (target.actuation_kind != (robotiq_position ? "position_commanded" : "pneumatic") ||
          (robotiq_position && target.delivery_result_timeout_s.has_value()) ||
          target.output_mode != gripper_output ||
          target.command_duration_s != gripper_dwell_s ||
          target.controllable_component_id != graph.control_bindings.front().controllable_component_id ||
          target.command_target_id != graph.control_bindings.front().command_target_id) {
        throw std::logic_error("gripper-only producer retained a different actuation/output/duration target");
      }
    }
    const auto &products = bundle.value().compiled_products;
    if (products.symbol_role_maps.size() != 1U) {
      throw std::logic_error("native execution fixture has no sole model map");
    }
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
        package_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle.retained_bundle_id(),
        products.symbol_role_maps.front().product_id});
    if (!model_map.has_value()) {
      throw std::logic_error("native execution fixture model map is absent");
    }

    if (robotiq_position) {
      const auto &graph = bundle.value().cell_graph;
      if (graph.model_instances.size() != 1U ||
          graph.model_instances.front().model_asset_id.value() != "robotiq_2f_85" ||
          graph.controllable_components.size() != 1U || graph.control_domains.size() != 1U ||
          !graph.component_attachments.empty() || model_map->targets().size() != 1U ||
          model_map->control_bindings().size() != 2U || model_map->coordinates().size() != 8U ||
          std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
              [](const auto &row) { return row.coordinate_kind == deployment::UrdfCoordinateKind::kAngular; }) != 6 ||
          std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
              [](const auto &row) { return row.coordinate_kind == deployment::UrdfCoordinateKind::kNone &&
                                         row.joint_type == deployment::UrdfJointType::kFixed; }) != 2 ||
          std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
              [](const auto &row) { return row.coordinate_kind != deployment::UrdfCoordinateKind::kNone &&
                                         !row.mimic_source_compiled_joint_name.has_value(); }) != 1 ||
          std::any_of(model_map->coordinates().begin(), model_map->coordinates().end(),
              [](const auto &row) { return !row.source_joint_name.starts_with("robotiq_85_"); }))
        throw std::logic_error("Robotiq fixture must retain only its actual scalar model and mimics");
      deployment::FilesystemVerifiedRetainedPlanningModelSource planning_source(package_root);
      const auto model = planning_source.get_verified({bundle.retained_bundle_id(),
          products.planning_collision_model.product_id});
      if (!model || model->model_source_urdfs().size() != 1U)
        throw std::logic_error("Robotiq fixture loses its verified source model");
      const auto documents = deployment::parse_model_documents_for_projection(
          model->model_source_urdfs().front().urdf_bytes, std::nullopt);
      const auto master = std::find_if(documents.urdf.joints.begin(), documents.urdf.joints.end(),
          [](const auto &joint) { return joint.name == "robotiq_85_left_knuckle_joint"; });
      if (documents.urdf.links.size() != 9U || documents.urdf.joints.size() != 8U ||
          std::count_if(documents.urdf.joints.begin(), documents.urdf.joints.end(),
              [](const auto &joint) { return joint.mimic.has_value(); }) != 5 ||
          master == documents.urdf.joints.end() || master->mimic || !master->limit ||
          master->limit->lower != 0.0 || master->limit->upper != 0.8 ||
          master->limit->velocity != 0.5 || master->limit->effort != 50.0)
        throw std::logic_error("Robotiq fixture must preserve its real source joints and limits");
      // The global map retains fixed joints as structural rows. Validate all
      // eight rows against the independently qualified source, including each
      // mimic's exact source link and affine law; they are not eight actuators.
      for (const auto &source : documents.urdf.joints) {
        const auto coordinate = std::find_if(model_map->coordinates().begin(), model_map->coordinates().end(),
            [&](const auto &row) { return row.source_joint_name == source.name; });
        if (coordinate == model_map->coordinates().end() || coordinate->joint_type != source.type ||
            coordinate->coordinate_kind != source.coordinate_kind ||
            coordinate->mimic_source_compiled_joint_name.has_value() != source.mimic.has_value())
          throw std::logic_error("Robotiq structural map differs from an actual source joint");
        if (source.mimic) {
          const auto parent = std::find_if(model_map->coordinates().begin(), model_map->coordinates().end(),
              [&](const auto &row) { return row.source_joint_name == source.mimic->joint_name; });
          if (parent == model_map->coordinates().end() ||
              *coordinate->mimic_source_compiled_joint_name != parent->compiled_joint_name ||
              coordinate->mimic_multiplier != source.mimic->multiplier ||
              coordinate->mimic_offset != source.mimic->offset)
            throw std::logic_error("Robotiq structural map changes a source mimic law");
        }
      }
      const auto &projected_resources = model_map->control_bindings().front().resources;
      if (projected_resources.size() != 1U || projected_resources.front().coordinates.size() != 6U)
        throw std::logic_error("Robotiq fixture must have one six-coordinate scalar resource projection");
      const auto &projection = projected_resources.front().coordinates;
      if (projection.front().source_joint_name != "robotiq_85_left_knuckle_joint" ||
          projection.front().derived_mimic ||
          std::count_if(projection.begin(), projection.end(), [](const auto &row) { return row.derived_mimic; }) != 5 ||
          std::any_of(projection.begin(), projection.end(), [](const auto &row) {
            return row.coordinate_kind != deployment::UrdfCoordinateKind::kAngular;
          }))
        throw std::logic_error("Robotiq actuation projection must keep its actual angular master and five mimics");
      const auto &binding = graph.control_bindings.front();
      const auto &feedback = graph.control_bindings.back();
      if (feedback.capability_id.value() != "tool.gripper.feedback" ||
          feedback.controllable_component_id != binding.controllable_component_id ||
          feedback.command_target_id != binding.command_target_id ||
          feedback.control_domain_id != binding.control_domain_id ||
          feedback.resource_bindings != binding.resource_bindings ||
          feedback.provider_instance_id != binding.provider_instance_id ||
          feedback.provider_runtime_unit_id != binding.provider_runtime_unit_id ||
          feedback.controller_topology_id != binding.controller_topology_id ||
          feedback.control_interface_contract_id != binding.control_interface_contract_id ||
          model_map->control_bindings().front().resources != model_map->control_bindings().back().resources)
        throw std::logic_error("Robotiq feedback must alias the same single physical command owner");
      if (binding.resource_bindings.size() != 1U ||
          binding.stop_evidence_policy_id.value() != "manyforge.gripper_position_cancellation")
        throw std::logic_error("Robotiq Position fixture loses its scalar binding or Position Stop policy");
      deployment::FilesystemVerifiedCatalogArtifactSource catalog_source(package_root);
      const core::StopEvidencePolicyCatalogRecordRefDTO *policy = nullptr;
      for (const auto &record : bundle.value().catalogs.records) {
        const auto *row = std::get_if<core::StopEvidencePolicyCatalogRecordRefDTO>(&record);
        if (row == nullptr || row->record_id != binding.stop_evidence_policy_id) continue;
        if (policy != nullptr) throw std::logic_error("Robotiq Position Stop source is ambiguous");
        policy = row;
      }
      if (policy == nullptr)
        throw std::logic_error("Robotiq Position fixture loses its selected Stop catalog");
      const auto bytes = catalog_source.get_verified(policy->content_addressed_store_key,
                                                     policy->content_digest);
      if (!bytes) throw std::logic_error("Robotiq Position Stop source is unavailable");
      const auto parsed = deployment::parse_strict_json_model_yaml(*bytes);
      const auto &stages = parsed.root().find_object_member("escalation_stages")->as_array();
      if (stages.size() != 2U || std::count_if(stages.begin(), stages.end(), [](const auto &stage) {
            return stage.find_object_member("stage_kind")->as_string() == "software_hard";
          }) != 1)
        throw std::logic_error("Robotiq Position fixture must keep exactly one software hard floor");
    }

    const bool include_gripper = !readiness_fixture;
    const bool include_arm = !gripper_only;
    const std::string program_bytes =
        make_program_bytes(bundle, include_arm, include_gripper, robotiq_position);
    const auto program =
        deployment::verify_program_binding_contract(program_bytes, bundle);
    deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
    std::vector<core::ProgramTargetRoleAssignmentDTO> target_assignments;
    if (include_arm) {
      target_assignments.push_back(
          {core::TargetRoleId("arm"),
           core::CommandTargetId(std::string(deployment::test::kLite6ArmTargetId))});
    }
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
                core::ControllableComponentId(std::string(
                    robotiq_position ? std::string_view("robotiq_gripper")
                    : arguments.fixture_kind == FixtureKind::kUr5ArmOnlyRos
                        ? std::string_view("ur5_arm")
                        : deployment::test::kLite6ControllableComponentId))}},
            std::move(target_assignments)},
        bundle, *model_map, catalog_store);
    deployment::FilesystemProgramRepository program_repository(package_root);
    if (!program_repository.put_if_absent(program, bundle)) {
      throw std::logic_error("native execution fixture Program already exists");
    }
    deployment::FilesystemProgramBindingRepository binding_repository(
        package_root);
    if (!binding_repository.put_if_absent(binding, bundle, *model_map)) {
      throw std::logic_error(
          "native execution fixture Program Binding already exists");
    }

    const auto empty_scene =
        make_empty_scene(bundle.value().cell_graph.cell_id);
    const core::ProjectionRevisionDTO projection{
        bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
        empty_scene.revision};
    component_manager::SqliteActiveBundlePublicationStore publication_store(
        manager_state_root /
            std::string(
                component_manager::kActiveBundlePublicationStoreFilename),
        bundle.value().cell_graph.cell_id);
    const auto receipt = publication_store.PublishActiveBundle(
        core::ActiveBundlePublicationRequestDTO{
            core::BootstrapPublicationTransactionDTO{
                core::BootstrapId("i6_native_execution_fixture"),
                core::BootstrapGeneration(1U)},
            projection,
            bundle.retained_bundle_id(),
            bundle.value().cell_graph.graph_revision,
            std::nullopt,
            core::ContentDigest(
                deployment::sha256_digest("i6-native-fixture-materialization")),
        });
    publication_store.Close();

    const auto program_path = output_root / "program.json";
    write_exact_file(program_path, program_bytes);
    write_exact_file(output_root / "program_binding.json",
                     std::string(binding->canonical_bytes()));
    CanonicalValue::Object manifest_members{
        {"api_version", CanonicalValue("manyforge.i6.native-execution-fixture.v1")},
        {"cclc_product",
         CanonicalValue(arguments.fixture_kind == FixtureKind::kCCLCProductRos ||
                        arguments.fixture_kind == FixtureKind::kProjectionSuccessorRos ||
                        gripper_only)},
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
        {"gripper_target_role_id", CanonicalValue("gripper")},
        {"gripper_capability_id", CanonicalValue("tool.gripper.command")},
        {"gripper_command_schema_id",
         CanonicalValue(std::string(core::kGripperPositionCommandSchemaId))},
        {"gripper_command_schema_revision",
         CanonicalValue(std::string(core::kGripperPositionCommandSchemaRevision))},
    };
    if (gripper_only) {
      // No motion identity exists anywhere in a gripper-only cell.
      manifest_members.emplace_back("gripper_only", CanonicalValue(true));
      // These fixture audit fields were checked against the typed retained
      // configuration above; they are not an alternative runtime input.
      manifest_members.emplace_back("gripper_actuation_kind", CanonicalValue(
          robotiq_position ? "position_commanded" : "pneumatic"));
      manifest_members.emplace_back("gripper_output_mode", CanonicalValue(
          gripper_output == core::GripperOutputMode::kNone ? "none" : "commanded"));
      manifest_members.emplace_back("gripper_command_duration_s", CanonicalValue(gripper_dwell_s));
    } else {
      manifest_members.emplace_back("target_role_id", CanonicalValue("arm"));
      manifest_members.emplace_back("execution_capability_id",
                                    CanonicalValue("execution.trajectory.execute"));
      manifest_members.emplace_back(
          "planning_capability_ids",
          CanonicalValue(CanonicalValue::Array{
              CanonicalValue("planning.motion.cartesian_path"),
              CanonicalValue("planning.motion.joint_goal"),
              CanonicalValue("planning.motion.pose_goal"),
          }));
      manifest_members.emplace_back(
          "source_joint_names",
          CanonicalValue(CanonicalValue::Array{
              CanonicalValue("joint1"), CanonicalValue("joint2"), CanonicalValue("joint3"),
              CanonicalValue("joint4"), CanonicalValue("joint5"), CanonicalValue("joint6")}));
    }
    const CanonicalValue manifest(std::move(manifest_members));
    write_exact_file(output_root / "manifest.json",
                     deployment::canonical_json(manifest));
    std::cout << (output_root / "manifest.json").string() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
