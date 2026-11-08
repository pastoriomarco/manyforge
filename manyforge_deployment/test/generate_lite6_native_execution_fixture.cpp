#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "maintained_activation_fixture.hpp"
#ifdef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
#include "lite6_retained_consumer_fixture.hpp"
#include "robotiq_gripper_only_retention_fixture.hpp"
#endif

namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using deployment::CanonicalValue;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kLite6Scenario =
    "examples/mock_lite6_pick_and_place.scenario";
constexpr std::string_view kUr5Scenario = "examples/mock_ur5_pick_and_place.scenario";

enum class FixtureKind {
  kApplicationRos,
  kUr5ArmOnlyRos,
  kProjectionSuccessorRos,
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
        "application-ros|ur5-arm-only-ros|projection-successor-ros|"
        "gripper-only-robotiq-position]");
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
    } else if (fixture_kind->string() == "ur5-arm-only-ros") {
      selected_kind = FixtureKind::kUr5ArmOnlyRos;
    } else if (fixture_kind->string() == "projection-successor-ros") {
      selected_kind = FixtureKind::kProjectionSuccessorRos;
    } else if (fixture_kind->string() == "gripper-only-robotiq-position") {
      selected_kind = FixtureKind::kRobotiqGripperOnlyPosition;
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

[[nodiscard]] CanonicalValue make_program_root(const deployment::RetainedBundle &bundle,
                                               const std::optional<core::ProgramTargetReferenceDTO> &arm,
                                               const std::optional<core::ProgramTargetReferenceDTO> &gripper,
                                               const CanonicalValue::Array &joint_goal,
                                               const bool robotiq_position) {
  CanonicalValue::Array children;
  const auto target = [](const core::ProgramTargetReferenceDTO &ref) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue(ref.component_id.value())},
        {"command_target_id", CanonicalValue(ref.command_target_id.value())}});
  };
  if (gripper) {
    for (const auto preset : {"closed", "open"}) {
      children.emplace_back(CanonicalValue::Object{
          {"node_id", CanonicalValue("gripper.command_position")},
          {"instance_id", CanonicalValue(std::string("robotiq_") + preset)},
          {"parameters", CanonicalValue(CanonicalValue::Object{
              {"target", target(*gripper)},
              {"position", robotiq_position
                  ? CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("named_preset")},
                                                           {"name", CanonicalValue(preset)}})
                  : CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("linear_m")},
                                                           {"value_m", CanonicalValue(std::string_view(preset) == "open" ? 0.0081 : 0.0)}})},
              {"command_duration_s", CanonicalValue(1.0)},
          })},
          {"children", CanonicalValue(CanonicalValue::Array{})},
      });
    }
  }
  if (arm) {
    if (joint_goal.empty()) throw std::logic_error("native motion fixture has no real source joints");
    const auto base = CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue(arm->component_id.value())}, {"frame_id", CanonicalValue("base")}});
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue("model.get_frame_pose")},
        {"instance_id", CanonicalValue("fixture_fk")},
        {"parameters", CanonicalValue(CanonicalValue::Object{
            {"target", target(*arm)}, {"frame", base}, {"output_key", CanonicalValue("fixture_pose")}})},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
    const auto pose = deployment::parse_strict_json_model_yaml(
        R"({"position_m":[0.3,0.0,0.3],"orientation":{"xyzw":[0,0,0,1]}})").root();
    const auto add_motion = [&](std::string_view id, std::string_view instance,
                                std::string_view parameter, CanonicalValue value) {
      CanonicalValue::Object parameters{{"target", target(*arm)}, {"base_frame", base},
                                        {std::string(parameter), std::move(value)}};
      if (gripper) {
        parameters.emplace_back("tcp_endpoint", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("entry")}, {"target", target(*gripper)},
            {"frame", CanonicalValue(CanonicalValue::Object{
                {"component_id", CanonicalValue(gripper->component_id.value())},
                {"frame_id", CanonicalValue("tcp")}})}}));
      }
      children.emplace_back(CanonicalValue::Object{
          {"node_id", CanonicalValue(std::string(id))}, {"instance_id", CanonicalValue(std::string(instance))},
          {"parameters", CanonicalValue(std::move(parameters))},
          {"children", CanonicalValue(CanonicalValue::Array{})}});
    };
    add_motion("motion.move_to_joint_target", "fixture_joint", "goal", CanonicalValue(joint_goal));
    add_motion("motion.move_to_pose", "fixture_pose", "goal", pose);
    const auto next_pose = deployment::parse_strict_json_model_yaml(
        R"({"position_m":[0.31,0.0,0.3],"orientation":{"xyzw":[0,0,0,1]}})").root();
    add_motion("motion.follow_cartesian_path", "fixture_cartesian", "path",
               CanonicalValue(CanonicalValue::Array{pose, next_pose}));
  }
  CanonicalValue::Object root{
      {"api_version", CanonicalValue("manyforge.program.v3")},
      {"program_id", CanonicalValue(robotiq_position
          ? "i6_robotiq_position_gripper_program" : "i6_native_joint_motion_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue(robotiq_position
          ? "I6 standalone Robotiq Position commands" : "I6 addressed native joint motion")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v3")},
           {"content_version",
            CanonicalValue(
                bundle.value().catalogs.node_environment.content_version)},
       })},
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
  };
  if (arm) {
    root.emplace_back("blackboard", CanonicalValue(CanonicalValue::Object{
        {"namespaces", CanonicalValue(CanonicalValue::Array{})},
        {"keys", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"key", CanonicalValue("fixture_pose")}, {"type", CanonicalValue("stored_pose")},
            {"access", CanonicalValue("read_write")}, {"persistence", CanonicalValue("run")}})})}}));
  }
  return CanonicalValue(std::move(root));
}

[[nodiscard]] std::string make_program_bytes(const deployment::RetainedBundle &bundle,
                                             const std::optional<core::ProgramTargetReferenceDTO> &arm,
                                             const std::optional<core::ProgramTargetReferenceDTO> &gripper,
                                             const CanonicalValue::Array &joint_goal,
                                             const bool robotiq_position) {
  CanonicalValue root = make_program_root(bundle, arm, gripper, joint_goal, robotiq_position);
  const std::string revision = deployment::detail::compute_content_identity(
      root, deployment::detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = "manyforge.program.v3",
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


// The projection successor is the same maintained cell with a new authored
// arm seed, so it mints a new graph/bundle on the same host infrastructure.
void map_projection_successor(CanonicalValue::Object &root) {
  using deployment::test::maintained_activation_fixture_detail::Member;
  using deployment::test::maintained_activation_fixture_detail::With;
  for (auto &[name, value] : root) {
    if (name != "components") continue;
    auto rows = value.as_array();
    for (auto &row : rows) {
      if (Member(row, "component_id").as_string() != "lite6_robot") continue;
      auto configuration = Member(row, "configuration");
      auto runtime = Member(configuration, "runtime_configuration");
      auto seed = Member(runtime, "initial_joint_positions_rad").as_array();
      seed.front() = CanonicalValue(0.1);
      runtime = With(std::move(runtime), "initial_joint_positions_rad",
                     CanonicalValue(std::move(seed)));
      configuration = With(std::move(configuration), "runtime_configuration",
                           std::move(runtime));
      row = With(std::move(row), "configuration", std::move(configuration));
    }
    value = CanonicalValue(std::move(rows));
  }
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
    const auto no_policies =
        deployment::parse_strict_json_model_yaml("{}").root();
    const std::shared_ptr<const deployment::RetainedBundle> materialized = [&]() {
      switch (arguments.fixture_kind) {
      case FixtureKind::kRobotiqGripperOnlyPosition:
#ifdef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
        return deployment::test::materialize_lite6_retained_consumer_fixture_from(
                   package_root, deployment::test::make_robotiq_gripper_only_retention_fixture(
                                     deployment::test::kWithLifecycleOffers))
            .bundle;
#else
        throw std::logic_error("Robotiq Position fixture requires its genuine prepared model inputs");
#endif
      case FixtureKind::kUr5ArmOnlyRos:
        return deployment::test::MaterializeMaintainedInterruptDeployment(
                   package_root, no_policies, kUr5Scenario)
            .bundle;
      case FixtureKind::kProjectionSuccessorRos:
        return deployment::test::MaterializeMaintainedInterruptDeployment(
                   package_root, no_policies, kLite6Scenario,
                   map_projection_successor)
            .bundle;
      case FixtureKind::kApplicationRos:
        break;
      }
      return deployment::test::MaterializeMaintainedInterruptDeployment(
                 package_root, no_policies, kLite6Scenario)
          .bundle;
    }();
    if (materialized == nullptr) {
      throw std::logic_error("native execution fixture has no bundle");
    }
    const auto &bundle = *materialized;
    if (robotiq_position) {
      const auto &graph = bundle.value().cell_graph;
      if (graph.runtime_units.size() != 1U ||
          graph.control_bindings.size() != 2U ||
          graph.control_bindings.front().capability_id.value() != "tool.gripper.command") {
        throw std::logic_error("gripper-only producer requires one actual scalar owner and no motion binding");
      }
      const auto config = deployment::ros2_control_execution_runtime_contract::require_configuration(
          bundle, graph.runtime_units.front().runtime_unit_id);
      if (config.gripper_target_configurations.size() != 1U ||
          !config.initial_joint_positions_rad.empty() || !config.motion_target_limits.empty() ||
          config.controller_profiles.size() != 1U ||
          config.controller_profiles.front().lane != "mock" ||
          config.controller_profiles.front().model_family_id != "robotiq_2f_85") {
        throw std::logic_error("gripper-only producer lost its exact no-motion Mock configuration");
      }
      const auto &target = config.gripper_target_configurations.front();
      if (target.actuation_kind != "position_commanded" ||
          target.delivery_result_timeout_s.has_value() ||
          target.output_mode != core::GripperOutputMode::kCommanded ||
          target.command_duration_s != 1.0 ||
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
    std::optional<core::ProgramTargetReferenceDTO> arm_target;
    std::optional<core::ProgramTargetReferenceDTO> gripper_target;
    for (const auto &row : bundle.value().cell_graph.control_bindings) {
      const core::ProgramTargetReferenceDTO target{row.controllable_component_id, row.command_target_id};
      auto *slot = row.capability_id.value() == "execution.trajectory.execute" ? &arm_target
                 : row.capability_id.value() == "tool.gripper.command" ? &gripper_target : nullptr;
      if (slot && *slot && **slot != target)
        throw std::logic_error("native fixture has ambiguous command targets");
      if (slot) *slot = target;
    }
    const bool include_arm = arm_target.has_value();
    const bool include_gripper = gripper_target.has_value();
    if (include_arm == robotiq_position || (robotiq_position && !include_gripper))
      throw std::logic_error("native fixture has an unexpected motion/gripper target shape");
    CanonicalValue::Array joint_goal;
    if (arm_target) {
      const auto arm = std::find_if(model_map->control_bindings().begin(), model_map->control_bindings().end(),
          [&](const auto &row) {
            return row.controllable_component_id == arm_target->component_id &&
                   row.command_target_id == arm_target->command_target_id;
          });
      if (arm == model_map->control_bindings().end())
        throw std::logic_error("native fixture arm has no verified source joint map");
      std::set<std::string> names;
      for (const auto &resource : arm->resources)
        for (const auto &coordinate : resource.coordinates)
          if (names.insert(coordinate.source_joint_name).second)
            joint_goal.emplace_back(CanonicalValue::Object{{"joint_name", CanonicalValue(coordinate.source_joint_name)},
                                                           {"position_rad", CanonicalValue(0.0)}});
    }
    const std::string program_bytes =
        make_program_bytes(bundle, arm_target, gripper_target, joint_goal, robotiq_position);
    deployment::FilesystemCatalogArtifactStore catalog_store(package_root);
    const auto program =
        deployment::verify_program_binding_contract(program_bytes, bundle, model_map, catalog_store);
    auto binding = deployment::compile_program_binding(
        *program, bundle, *model_map, catalog_store);
    if (arm_target) {
      const auto &targets = binding->value().target_bindings;
      const auto arm = std::find_if(targets.begin(), targets.end(),
          [&](const auto &row) { return row.target == *arm_target; });
      if (arm == targets.end() || std::none_of(arm->capability_bindings.begin(),
          arm->capability_bindings.end(), [](const auto &capability) {
            return capability.capability_id.value() == "model.kinematics.fk";
          }))
        throw std::logic_error("native motion fixture does not bind the authored FK requirement");
    }
    deployment::FilesystemProgramRepository program_repository(package_root);
    if (!program_repository.put_if_absent(program, bundle)) {
      throw std::logic_error("native execution fixture Program already exists");
    }
    deployment::FilesystemProgramBindingRepository binding_repository(
        package_root);
    if (!binding_repository.put_if_absent(
            binding, bundle, *model_map, catalog_store)) {
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
    // Exact referenced components in stable order.
    CanonicalValue::Array component_ids;
    std::set<std::string> components;
    if (arm_target) components.insert(arm_target->component_id.value());
    if (gripper_target) components.insert(gripper_target->component_id.value());
    for (const auto &id : components) component_ids.emplace_back(id);
    CanonicalValue::Object manifest_members{
        {"api_version", CanonicalValue("manyforge.i6.native-execution-fixture.v2")},
        {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
        {"retained_bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
        {"graph_revision", CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
        {"scene_revision", scene_revision(empty_scene.revision)},
        {"active_publication_generation",
         CanonicalValue(static_cast<double>(receipt.publication_generation.value()))},
        {"program_id", CanonicalValue(program->value().program_id.value())},
        {"program_revision", CanonicalValue(program->value().program_revision.value())},
        {"program_binding_id", CanonicalValue(binding->program_binding_id().value())},
        {"controllable_component_ids", CanonicalValue(std::move(component_ids))},
    };
    if (include_gripper) {
      manifest_members.emplace_back("gripper_target", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(gripper_target->component_id.value())},
          {"command_target_id", CanonicalValue(gripper_target->command_target_id.value())}}));
      manifest_members.emplace_back("gripper_capability_id",
                                    CanonicalValue("tool.gripper.command"));
      manifest_members.emplace_back(
          "gripper_command_schema_id",
          CanonicalValue(std::string(core::kGripperPositionCommandSchemaId)));
      manifest_members.emplace_back(
          "gripper_command_schema_revision",
          CanonicalValue(std::string(core::kGripperPositionCommandSchemaRevision)));
    }
    if (robotiq_position) {
      // No motion identity exists anywhere in a gripper-only cell.
      manifest_members.emplace_back("gripper_only", CanonicalValue(true));
      // These fixture audit fields were checked against the typed retained
      // configuration above; they are not an alternative runtime input.
      manifest_members.emplace_back("gripper_actuation_kind",
                                    CanonicalValue("position_commanded"));
      manifest_members.emplace_back("gripper_output_mode", CanonicalValue("commanded"));
      manifest_members.emplace_back("gripper_command_duration_s", CanonicalValue(1.0));
    } else {
      manifest_members.emplace_back("motion_target", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(arm_target->component_id.value())},
          {"command_target_id", CanonicalValue(arm_target->command_target_id.value())}}));
      manifest_members.emplace_back("execution_capability_id",
                                    CanonicalValue("execution.trajectory.execute"));
      manifest_members.emplace_back(
          "planning_capability_ids",
          CanonicalValue(CanonicalValue::Array{
              CanonicalValue("planning.motion.cartesian_path"),
              CanonicalValue("planning.motion.joint_goal"),
              CanonicalValue("planning.motion.pose_goal"),
          }));
      // The arm target's own source joints, in its resource order.
      CanonicalValue::Array joints;
      const auto arm = std::find_if(
          model_map->control_bindings().begin(), model_map->control_bindings().end(),
          [](const auto &row) { return row.command_target_id.value() == "arm"; });
      if (arm == model_map->control_bindings().end())
        throw std::logic_error("native execution fixture has no arm coordinate map");
      for (const auto &resource : arm->resources)
        for (const auto &coordinate : resource.coordinates)
          joints.emplace_back(coordinate.source_joint_name);
      manifest_members.emplace_back("source_joint_names", CanonicalValue(std::move(joints)));
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
