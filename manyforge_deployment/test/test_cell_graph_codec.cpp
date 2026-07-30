#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000001";

[[nodiscard]] std::string empty_graph(std::string_view cell_id = "cell_a") {
  return "api_version: manyforge.cell_graph.v1\n"
         "graph_revision: " +
         std::string(kZeroDigest) +
         "\n"
         "cell_id: " +
         std::string(cell_id) +
         "\n"
         "runtime_units: []\n"
         "model_instances: []\n"
         "controllable_components: []\n"
         "control_domains: []\n"
         "control_bindings: []\n"
         "component_attachments: []\n";
}

[[nodiscard]] TypedCellGraphCandidate decode(std::string_view yaml) {
  return decode_cell_graph_candidate(parse_strict_json_model_yaml(yaml));
}

[[nodiscard]] IntrinsicallyValidatedCellGraphCandidate validate(std::string yaml) {
  return validate_intrinsic_cell_graph_candidate(decode(yaml));
}

[[nodiscard]] std::string two_instance_graph(bool reverse) {
  const std::string first_model = R"YAML(
  - model_instance_id: robot_1_model
    model_asset_id: lite6_model
    model_revision: )YAML" + std::string(kZeroDigest) + "\n";
  const std::string second_model = R"YAML(
  - model_revision: )YAML" + std::string(kZeroDigest) +
                                   R"YAML(
    model_asset_id: lite6_model
    model_instance_id: robot_2_model
)YAML";
  const std::string first_component = R"YAML(
  - controllable_component_id: robot_1
    component_type_id: articulated_arm
    component_type_revision: )YAML" + std::string(kZeroDigest) +
                                      R"YAML(
    model_role_bindings:
      - {model_role_id: base, model_instance_id: robot_1_model}
    runtime_unit_ids: []
    root_pose_in_cell:
      position_m: [0.0, 0.0, 0.0]
      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
)YAML";
  const std::string second_component = R"YAML(
  - root_pose_in_cell:
      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
      position_m: [1.0, 0.0, 0.0]
    runtime_unit_ids: []
    model_role_bindings:
      - model_instance_id: robot_2_model
        model_role_id: base
    component_type_revision: )YAML" + std::string(kZeroDigest) +
                                       R"YAML(
    component_type_id: articulated_arm
    controllable_component_id: robot_2
)YAML";

  return "component_attachments: []\n"
         "control_bindings: []\n"
         "api_version: manyforge.cell_graph.v1\n"
         "control_domains: []\n"
         "graph_revision: " +
         std::string(kZeroDigest) +
         "\n"
         "controllable_components:\n" +
         (reverse ? second_component + first_component : first_component + second_component) +
         "cell_id: two_lite6_cell\n"
         "model_instances:\n" +
         (reverse ? second_model + first_model : first_model + second_model) +
         "runtime_units: []\n";
}

[[nodiscard]] std::string full_graph() {
  return "api_version: manyforge.cell_graph.v1\n"
         "graph_revision: " +
         std::string(kZeroDigest) + R"YAML(
cell_id: full_cell
runtime_units:
  - runtime_unit_id: observer
    runtime_profile_id: observer_profile
    runtime_profile_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    dependency_runtime_unit_ids: []
    configuration: {}
  - runtime_unit_id: driver
    runtime_profile_id: driver_profile
    runtime_profile_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    dependency_runtime_unit_ids: [observer]
    configuration: {robot_ip_ref: site.robot.ip}
model_instances:
  - model_instance_id: arm_model
    model_asset_id: arm_asset
    model_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
  - model_instance_id: tool_model
    model_asset_id: tool_asset
    model_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
controllable_components:
  - controllable_component_id: arm
    component_type_id: arm_type
    component_type_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    model_role_bindings:
      - {model_role_id: base, model_instance_id: arm_model}
    runtime_unit_ids: [observer, driver]
    root_pose_in_cell:
      position_m: [0.0, 0.0, 0.0]
      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
  - controllable_component_id: tool
    component_type_id: tool_type
    component_type_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    model_role_bindings:
      - {model_role_id: body, model_instance_id: tool_model}
    runtime_unit_ids: []
control_domains:
  - control_domain_id: arm_motion
    control_domain_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    runtime_unit_ids: [driver]
    control_coordinator_endpoint_role: controller_manager
    controller_topology_id: standard
    topology_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    containment_granularity: controller_set
control_bindings:
  - control_binding_id: arm_trajectory
    control_binding_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    controllable_component_id: arm
    command_target_id: arm
    capability_id: execution.trajectory.execute
    capability_definition_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    control_domain_id: arm_motion
    resource_bindings:
      - {resource_role_id: arm_joints, resource_id: joint_2}
      - {resource_role_id: arm_joints, resource_id: joint_1}
    control_interface_contract_id: arm_position
    control_interface_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    controller_topology_id: standard
    topology_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    legal_control_route_ids: [route_b, route_a]
    route_endpoint_projections:
      - control_route_id: route_b
        command_endpoint_role_ids: [command_b, command_a]
        state_endpoint_role_ids: [state_b, state_a]
      - control_route_id: route_a
        command_endpoint_role_ids: [command_a]
        state_endpoint_role_ids: [state_a]
    provider_instance_id: hardware
    provider_manifest_id: xarm_hardware
    provider_manifest_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    state_authority_id: measured_joint_state
    state_authority_class: measured
    stop_evidence_policy_id: measured_rest
    stop_evidence_policy_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    containment_granularity: controller_set
component_attachments:
  - component_attachment_id: tool_on_arm
    component_attachment_revision: )YAML" +
         std::string(kZeroDigest) + R"YAML(
    parent:
      controllable_component_id: arm
      attachment_interface_id: flange
    child:
      controllable_component_id: tool
      attachment_interface_id: mount
    child_interface_pose_in_parent_interface:
      position_m: [0.0, 0.0, 0.1]
      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
)YAML";
}

// Independently authored from the closed graph schema, not emitted by the
// codec under test. It freezes the complete structural normalization surface.
constexpr std::string_view kFullGraphNormalizedJsonGolden =
    "{\"api_version\":\"manyforge.cell_graph.v1\",\"cell_id\":\"full_cell\",\"component_"
    "attachments\":["
    "{\"child\":{\"attachment_interface_id\":\"mount\",\"controllable_component_id\":\"tool\"},"
    "\"child_interface_pose_in_parent_interface\":{\"orientation_xyzw\":[0,0,0,1],\"position_m\":"
    "[0,0,0.1]},\"component_attachment_id\":\"tool_on_arm\",\"component_attachment_revision\":"
    "\"sha256:0000000000000000000000000000000000000000000000000000000000000000\",\"parent\":{"
    "\"attachment_interface_id\":\"flange\",\"controllable_component_id\":\"arm\"}}],\"control_"
    "bindings\":[{\"capability_"
    "definition_revision\":\"sha256:000000000000000000000000000000000000000000000000000000000000"
    "0000\",\"capability_id\":\"execution.trajectory.execute\",\"command_target_id\":\"arm\","
    "\"containment_granularity\":"
    "\"controll"
    "er_set\",\"control_binding_id\":\"arm_trajectory\",\"control_binding_revision\":\"sha256:"
    "0000000"
    "000000000000000000000000000000000000000000000000000000000\",\"control_domain_id\":\"arm_moti"
    "on\",\"control_interface_contract_id\":\"arm_position\",\"control_interface_revision\":"
    "\"sha256:"
    "0000000000000000000000000000000000000000000000000000000000000000\",\"controllable_componen"
    "t_id\":\"arm\",\"controller_topology_id\":\"standard\",\"legal_control_route_ids\":[\"route_"
    "a\",\"ro"
    "ute_b\"],\"provider_instance_id\":\"hardware\",\"provider_manifest_id\":\"xarm_hardware\","
    "\"provider_manifest_revision\":\"sha256:"
    "00000"
    "00000000000000000000000000000000000000000000000000000000000\",\"resource_bindings\":[{\"reso"
    "urce_id\":\"joint_2\",\"resource_role_id\":\"arm_joints\"},{\"resource_id\":\"joint_1\","
    "\"resource_r"
    "ole_id\":\"arm_joints\"}],\"route_endpoint_projections\":[{\"command_endpoint_role_ids\":["
    "\"comm"
    "and_a\"],\"control_route_id\":\"route_a\",\"state_endpoint_role_ids\":[\"state_a\"]},{"
    "\"command_en"
    "dpoint_role_ids\":[\"command_a\",\"command_b\"],\"control_route_id\":\"route_b\",\"state_"
    "endpoint_"
    "role_ids\":[\"state_a\",\"state_b\"]}],\"state_authority_class\":\"measured\",\"state_"
    "authority_id"
    "\":\"measured_joint_state\",\"stop_evidence_policy_id\":\"measured_rest\",\"stop"
    "_evidence_policy_revision\":\"sha256:00000000000000000000000000000000000000000000000000000"
    "00000000000\",\"topology_revision\":\"sha256:00000000000000000000000000000000000000000000000"
    "00000000000000000\"}],\"control_domains\":[{\"containment_granularity\":\"controller_set\","
    "\"con"
    "trol_coordinator_endpoint_role\":\"controller_manager\",\"control_domain_id\":\"arm_motion\","
    "\"c"
    "ontrol_domain_revision\":\"sha256:00000000000000000000000000000000000000000000000000000000"
    "00000000\",\"controller_topology_id\":\"standard\",\"runtime_unit_ids\":[\"driver\"],"
    "\"topology_re"
    "vision\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\"}],\"con"
    "trollable_components\":[{\"component_type_id\":\"arm_type\",\"component_type_revisio"
    "n\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\",\"controllab"
    "le_component_id\":\"arm\",\"model_role_bindings\":[{\"model_instance_id\":\"arm_model\","
    "\"model_ro"
    "le_id\":\"base\"}],\"root_pose_in_cell\":{\"orientation_xyzw\":[0,0,0,1],\"position_m\":[0,0,"
    "0]},\"runtime_unit_ids\":[\"driver\",\"observer\"]},{\"component_type_id\""
    ":\"tool_type\",\"component_type_revision\":\"sha256:00000000000000000000000000000000000000000"
    "00000000000000000000000\",\"controllable_component_id\":\"tool\",\"model_role_bindings\":[{"
    "\"mod"
    "el_instance_id\":\"tool_model\",\"model_role_id\":\"body\"}],\"runtime_unit_ids\":[]}],"
    "\"graph_rev"
    "ision\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\",\"model_"
    "instances\":[{\"model_asset_id\":\"arm_asset\",\"model_instance_"
    "id\":\""
    "arm_model\",\"model_revision\":\"sha256:0000000000000000000000000000000000000000000000000000"
    "000000000000\"},{\"model_asset_id\":\"tool_"
    "asset\",\""
    "model_instance_id\":\"tool_model\",\"model_revision\":\"sha256:0000000000000000000000000000000"
    "000000000000000000000000000000000\"}],\"runtime_units\":[{"
    "\"configuration\""
    ":{\"robot_ip_ref\":\"site.robot.ip\"},\"dependency_runtime_unit_ids\":[\"observer\"],"
    "\"runtime_profile_id\":\"driver_profile\",\"runtime_profile_revision\":\"sha256:"
    "0000000000000000000000000000000000000000000000000000000000000000\",\"runtime_unit_id\":"
    "\"driver\"},{\"configuration\":{"
    "},\"d"
    "ependency_runtime_unit_ids\":[],\"runtime_profile_id\":\"observer_profile\",\"runtime_profile_"
    "revision\":\"sha256:0000000000000000000000000000000000000000000000000000000000000000\","
    "\"runtime_unit_id\":\"observer\"}]}";

[[nodiscard]] std::string replace_once(std::string text, std::string_view from,
                                       std::string_view to) {
  const std::size_t position = text.find(from);
  if (position == std::string::npos) {
    throw std::logic_error("test replacement source was not found");
  }
  text.replace(position, from.size(), to);
  return text;
}

[[nodiscard]] std::string two_control_binding_graph(
    std::string_view second_provider_instance_id,
    std::string_view second_provider_manifest_id,
    std::string_view second_provider_manifest_revision) {
  std::string graph = full_graph();
  const std::size_t binding_start =
      graph.find("  - control_binding_id: arm_trajectory\n");
  const std::size_t attachments = graph.find("component_attachments:\n");
  if (binding_start == std::string::npos || attachments == std::string::npos ||
      binding_start >= attachments) {
    throw std::logic_error("control-binding fixture boundary was not found");
  }
  std::string second_binding =
      graph.substr(binding_start, attachments - binding_start);
  second_binding =
      replace_once(std::move(second_binding), "arm_trajectory",
                   "arm_trajectory_aux");
  second_binding = replace_once(
      std::move(second_binding), "provider_instance_id: hardware",
      "provider_instance_id: " + std::string(second_provider_instance_id));
  second_binding = replace_once(
      std::move(second_binding), "provider_manifest_id: xarm_hardware",
      "provider_manifest_id: " + std::string(second_provider_manifest_id));
  second_binding = replace_once(
      std::move(second_binding),
      "provider_manifest_revision: " + std::string(kZeroDigest),
      "provider_manifest_revision: " +
          std::string(second_provider_manifest_revision));
  graph.insert(attachments, second_binding);
  return graph;
}

TEST(CellGraphCodec, EmptyGraphRetainsTypedDeclaredRevisionAsCandidate) {
  const TypedCellGraphCandidate candidate = decode(empty_graph());

  EXPECT_EQ(candidate.graph().graph_revision.value(), kZeroDigest);
  EXPECT_NO_THROW({
    const auto validated = validate_intrinsic_cell_graph_candidate(candidate);
    EXPECT_EQ(validated.graph().cell_id.value(), "cell_a");
  });
}

TEST(CellGraphCodec, EnforcesBoundedAsciiIdentityGrammar) {
  const std::string maximum_length(128U, 'a');
  EXPECT_EQ(decode(empty_graph(maximum_length)).graph().cell_id.value(), maximum_length);
  EXPECT_EQ(decode(empty_graph("a.B_2-c")).graph().cell_id.value(), "a.B_2-c");

  EXPECT_THROW({ (void)decode(empty_graph(std::string(129U, 'a'))); }, CellGraphCodecError);
  EXPECT_THROW({ (void)decode(empty_graph("_leading_separator")); }, CellGraphCodecError);
  EXPECT_THROW({ (void)decode(empty_graph("contains/slash")); }, CellGraphCodecError);
  EXPECT_THROW({ (void)decode(empty_graph("ròbot")); }, CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(), "attachment_interface_id: flange",
                                  "attachment_interface_id: \"\""));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(), "attachment_interface_id: flange",
                                  "attachment_interface_id: contains/slash"));
      },
      CellGraphCodecError);
}

TEST(CellGraphCodec, ShuffledEquivalentGraphsHaveOneNormalizedCandidate) {
  const TypedCellGraphCandidate forward = decode(two_instance_graph(false));
  const TypedCellGraphCandidate reverse = decode(two_instance_graph(true));

  EXPECT_EQ(canonical_json(forward.structurally_normalized_root()),
            canonical_json(reverse.structurally_normalized_root()));
}

TEST(CellGraphCodec, RetainedFieldChangeChangesNormalizedCandidate) {
  const TypedCellGraphCandidate first = decode(empty_graph("cell_a"));
  const TypedCellGraphCandidate second = decode(empty_graph("cell_b"));

  EXPECT_NE(canonical_json(first.structurally_normalized_root()),
            canonical_json(second.structurally_normalized_root()));
}

TEST(CellGraphCodec, RejectsRemovedCatalogByteDigestCompatibilityFields) {
  const std::string digest = std::string(kZeroDigest);
  EXPECT_THROW(
      {
        (void)decode(replace_once(
            full_graph(), "    dependency_runtime_unit_ids: []",
            "    runtime_profile_digest: " + digest + "\n    dependency_runtime_unit_ids: []"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(
            full_graph(), "    model_role_bindings:",
            "    component_type_digest: " + digest + "\n    model_role_bindings:"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(
            full_graph(), "    state_authority_id: measured_joint_state",
            "    provider_digest: " + digest + "\n    state_authority_id: measured_joint_state"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(
            full_graph(), "    containment_granularity: controller_set\ncomponent_attachments:",
            "    stop_evidence_policy_digest: " + digest +
                "\n    containment_granularity: controller_set\ncomponent_attachments:"));
      },
      CellGraphCodecError);
}

TEST(CellGraphCodec, TwoSameTypeInstancesAreDistinctAndIntrinsicallyValid) {
  const auto validated = validate(two_instance_graph(true));

  ASSERT_EQ(validated.graph().model_instances.size(), 2U);
  ASSERT_EQ(validated.graph().controllable_components.size(), 2U);
  EXPECT_EQ(validated.graph().model_instances[0].model_instance_id.value(), "robot_1_model");
  EXPECT_EQ(validated.graph().controllable_components[1].component_type_id.value(),
            "articulated_arm");
}

TEST(CellGraphCodec, RejectsOneModelBoundToMultipleRolesInOneComponent) {
  std::string graph = two_instance_graph(false);
  graph = replace_once(std::move(graph),
                       "      - {model_role_id: base, model_instance_id: robot_1_model}\n"
                       "    runtime_unit_ids: []",
                       "      - {model_role_id: visual, model_instance_id: robot_1_model}\n"
                       "      - {model_role_id: base, model_instance_id: robot_1_model}\n"
                       "    runtime_unit_ids: []");

  EXPECT_THROW({ (void)decode(std::move(graph)); }, CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsOneModelRoleBoundToMultipleModels) {
  std::string graph = two_instance_graph(false);
  graph = replace_once(std::move(graph),
                       "      - {model_role_id: base, model_instance_id: robot_1_model}\n"
                       "    runtime_unit_ids: []",
                       "      - {model_role_id: base, model_instance_id: robot_1_model}\n"
                       "      - {model_role_id: base, model_instance_id: robot_2_model}\n"
                       "    runtime_unit_ids: []");

  EXPECT_THROW({ (void)decode(std::move(graph)); }, CellGraphCodecError);
}

TEST(CellGraphCodec, AllowsInjectiveMultiModelBindingPendingCatalogBijection) {
  std::string graph = empty_graph();
  graph = replace_once(
      std::move(graph), "model_instances: []",
      "model_instances:\n"
      "  - model_instance_id: base_model\n"
      "    model_asset_id: base_asset\n"
      "    model_revision: " +
          std::string(kZeroDigest) +
          "\n"
          "  - model_instance_id: tool_model\n"
          "    model_asset_id: tool_asset\n"
          "    model_revision: " +
          std::string(kZeroDigest));
  graph = replace_once(
      std::move(graph), "controllable_components: []",
      "controllable_components:\n"
      "  - controllable_component_id: aggregate\n"
      "    component_type_id: aggregate_type\n"
      "    component_type_revision: " +
          std::string(kZeroDigest) +
          "\n"
          "    model_role_bindings:\n"
          "      - {model_role_id: base, model_instance_id: base_model}\n"
          "      - {model_role_id: tool, model_instance_id: tool_model}\n"
          "    runtime_unit_ids: []\n"
          "    root_pose_in_cell:\n"
          "      position_m: [0.0, 0.0, 0.0]\n"
          "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]");

  const auto validated = validate(std::move(graph));
  ASSERT_EQ(validated.graph().controllable_components.size(), 1U);
  EXPECT_EQ(validated.graph().controllable_components[0].model_role_bindings.size(), 2U);
}

TEST(CellGraphCodec, NormalizesSetsButPreservesResourceBindingOrder) {
  const auto validated = validate(full_graph());
  const auto& graph = validated.graph();

  ASSERT_EQ(graph.controllable_components[0].runtime_unit_ids.size(), 2U);
  EXPECT_EQ(graph.controllable_components[0].runtime_unit_ids[0].value(), "driver");
  ASSERT_EQ(graph.control_bindings.size(), 1U);
  const auto& binding = graph.control_bindings.front();
  ASSERT_EQ(binding.resource_bindings.size(), 2U);
  EXPECT_EQ(binding.resource_bindings[0].resource_id.value(), "joint_2");
  EXPECT_EQ(binding.resource_bindings[1].resource_id.value(), "joint_1");
  EXPECT_EQ(binding.legal_control_route_ids[0].value(), "route_a");
  EXPECT_EQ(binding.route_endpoint_projections[1].command_endpoint_role_ids[0].value(),
            "command_a");
}

TEST(CellGraphCodec, FullGraphMatchesIndependentNormalizedJsonGolden) {
  const TypedCellGraphCandidate candidate = decode(full_graph());

  EXPECT_EQ(canonical_json(candidate.structurally_normalized_root()),
            kFullGraphNormalizedJsonGolden);
}

TEST(CellGraphCodec, RejectsUnknownAndDuplicateFields) {
  EXPECT_THROW(
      {
        (void)decode(replace_once(empty_graph(), "cell_id: cell_a", "cell_id: cell_a\nextra: no"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(
            replace_once(empty_graph(), "cell_id: cell_a", "cell_id: cell_a\ncell_id: cell_b"));
      },
      ArtifactCodecError);
  EXPECT_THROW(
      {
        (void)decode(
            replace_once(full_graph(), "runtime_unit_id: observer", "runtime_unit_id: driver"));
      },
      CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsLegacyTwoFieldProviderSelector) {
  const std::string legacy_graph =
      replace_once(full_graph(),
                   "    provider_manifest_id: xarm_hardware\n", "");

  try {
    (void)decode(legacy_graph);
    FAIL() << "legacy instance/revision provider selector must not be accepted";
  } catch (const CellGraphCodecError& error) {
    EXPECT_NE(
        std::string(error.what())
            .find("missing required field 'provider_manifest_id'"),
        std::string::npos);
  }
}

TEST(CellGraphCodec, RejectsRemovedInstanceParametersAsUnknownField) {
  const std::string legacy_graph =
      replace_once(full_graph(), "    model_asset_id: arm_asset\n",
                   "    model_asset_id: arm_asset\n"
                   "    instance_parameters: {}\n");

  try {
    (void)decode(legacy_graph);
    FAIL() << "legacy instance_parameters must not be accepted or ignored";
  } catch (const CellGraphCodecError& error) {
    EXPECT_NE(std::string(error.what()).find("unknown field 'instance_parameters'"),
              std::string::npos);
  }
}

TEST(CellGraphCodec, OptionalPlacementUsesAbsenceAndRejectsNullOrLegacyFields) {
  EXPECT_NO_THROW({ (void)decode(two_instance_graph(false)); });

  const std::string root_pose =
      "    root_pose_in_cell:\n"
      "      position_m: [0.0, 0.0, 0.0]\n"
      "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]\n";
  EXPECT_THROW(
      {
        (void)decode(replace_once(two_instance_graph(false), root_pose,
                                  "    root_pose_in_cell: null\n"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(), "    model_asset_id: arm_asset\n",
                                  "    model_asset_id: arm_asset\n"
                                  "    root_pose_in_cell: null\n"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(), "    model_asset_id: arm_asset\n",
                                  "    model_asset_id: arm_asset\n"
                                  "    mount_parent_frame_role: cell_world\n"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(),
                                  "    child_interface_pose_in_parent_interface:",
                                  "    mount_pose:"));
      },
      CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsMalformedDigestAndQuaternion) {
  EXPECT_THROW(
      { (void)decode(replace_once(empty_graph(), std::string(kZeroDigest), "sha256:ABC")); },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(two_instance_graph(false),
                                  "orientation_xyzw: [0.0, 0.0, 0.0, 1.0]",
                                  "orientation_xyzw: [0.0, 0.0, 0.0, 2.0]"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(two_instance_graph(false),
                                  "position_m: [0.0, 0.0, 0.0]",
                                  "position_m: [0.0, 0.0, 1e309]"));
      },
      ArtifactCodecError);
}

TEST(CellGraphCodec, UsesPracticalQuaternionToleranceWithoutRenormalizing) {
  std::string rounded =
      replace_once(two_instance_graph(false), "orientation_xyzw: [0.0, 0.0, 0.0, 1.0]",
                   "orientation_xyzw: [0.0, 0.0, 0.70710678, 0.70710678]");
  const TypedCellGraphCandidate candidate = decode(std::move(rounded));
  ASSERT_TRUE(candidate.graph().controllable_components[0].root_pose_in_cell.has_value());
  EXPECT_DOUBLE_EQ(
      candidate.graph().controllable_components[0].root_pose_in_cell->orientation_xyzw[2],
      0.70710678);

  std::string near_boundary =
      replace_once(two_instance_graph(false), "orientation_xyzw: [0.0, 0.0, 0.0, 1.0]",
                   "orientation_xyzw: [0.0, 0.0, 0.0, 1.0000004]");
  const TypedCellGraphCandidate near_candidate = decode(std::move(near_boundary));
  ASSERT_TRUE(near_candidate.graph().controllable_components[0].root_pose_in_cell.has_value());
  EXPECT_DOUBLE_EQ(
      near_candidate.graph().controllable_components[0].root_pose_in_cell->orientation_xyzw[3],
      1.0000004);

  std::string outside =
      replace_once(two_instance_graph(false), "orientation_xyzw: [0.0, 0.0, 0.0, 1.0]",
                   "orientation_xyzw: [0.0, 0.0, 0.0, 1.000001]");
  EXPECT_THROW({ (void)decode(std::move(outside)); }, CellGraphCodecError);
}

TEST(CellGraphCodec, PreservesQuaternionSignRepresentation) {
  const TypedCellGraphCandidate positive = decode(two_instance_graph(false));
  const TypedCellGraphCandidate negative =
      decode(replace_once(two_instance_graph(false), "orientation_xyzw: [0.0, 0.0, 0.0, 1.0]",
                          "orientation_xyzw: [0.0, 0.0, 0.0, -1.0]"));

  ASSERT_TRUE(positive.graph().controllable_components[0].root_pose_in_cell.has_value());
  ASSERT_TRUE(negative.graph().controllable_components[0].root_pose_in_cell.has_value());
  EXPECT_EQ(positive.graph()
                .controllable_components[0]
                .root_pose_in_cell->orientation_xyzw[3],
            1.0);
  EXPECT_EQ(negative.graph()
                .controllable_components[0]
                .root_pose_in_cell->orientation_xyzw[3],
            -1.0);
  EXPECT_NE(canonical_json(positive.structurally_normalized_root()),
            canonical_json(negative.structurally_normalized_root()));
}

TEST(CellGraphCodec, PreservesUnresolvedCatalogConstrainedArrayOrder) {
  std::string graph = full_graph();
  graph = replace_once(std::move(graph), "configuration: {robot_ip_ref: site.robot.ip}",
                       "configuration: {ordered_or_set_unknown: [second, first]}");
  const TypedCellGraphCandidate candidate = decode(std::move(graph));

  const auto& unresolved = std::get<core::JsonValueDTO::Array>(
      candidate.graph().runtime_units[0].configuration.at("ordered_or_set_unknown").value);
  ASSERT_EQ(unresolved.size(), 2U);
  EXPECT_EQ(std::get<std::string>(unresolved[0].value), "second");
  EXPECT_EQ(std::get<std::string>(unresolved[1].value), "first");
}

TEST(CellGraphCodec, RejectsDanglingReferencesAndRuntimeCycles) {
  EXPECT_THROW(
      {
        (void)validate(replace_once(two_instance_graph(false), "model_instance_id: robot_1_model}",
                                    "model_instance_id: missing_model}"));
      },
      CellGraphCodecError);

  std::string cyclic = full_graph();
  cyclic = replace_once(std::move(cyclic), "dependency_runtime_unit_ids: []",
                        "dependency_runtime_unit_ids: [driver]");
  EXPECT_THROW({ (void)validate(std::move(cyclic)); }, CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsMultipleModelOwnersAndTopologyMismatch) {
  std::string shared = two_instance_graph(false);
  shared = replace_once(std::move(shared),
                        "model_instance_id: robot_2_model\n"
                        "        model_role_id: base",
                        "model_instance_id: robot_1_model\n"
                        "        model_role_id: base");
  EXPECT_THROW({ (void)validate(std::move(shared)); }, CellGraphCodecError);

  std::string mismatch = full_graph();
  const std::size_t binding = mismatch.find("control_bindings:");
  ASSERT_NE(binding, std::string::npos);
  const std::size_t topology = mismatch.find("controller_topology_id: standard", binding);
  ASSERT_NE(topology, std::string::npos);
  mismatch.replace(topology, std::string("controller_topology_id: standard").size(),
                   "controller_topology_id: different");
  EXPECT_THROW({ (void)validate(std::move(mismatch)); }, CellGraphCodecError);
}

TEST(CellGraphCodec,
     OneProviderInstanceSelectsOneManifestIdentityAcrossControlBindings) {
  EXPECT_NO_THROW({
    (void)validate(two_control_binding_graph(
        "hardware", "xarm_hardware", kZeroDigest));
  });
  EXPECT_THROW(
      {
        (void)validate(two_control_binding_graph(
            "hardware", "other_hardware", kZeroDigest));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)validate(two_control_binding_graph(
            "hardware", "xarm_hardware", kOneDigest));
      },
      CellGraphCodecError);
  EXPECT_NO_THROW({
    (void)validate(two_control_binding_graph(
        "hardware_aux", "other_hardware", kOneDigest));
  });
}

TEST(CellGraphCodec, EnforcesOwnedPassiveAndComponentRootPlacement) {
  std::string owned_model_with_root = full_graph();
  owned_model_with_root =
      replace_once(std::move(owned_model_with_root), "    model_asset_id: arm_asset\n",
                   "    model_asset_id: arm_asset\n"
                   "    root_pose_in_cell:\n"
                   "      position_m: [0.0, 0.0, 0.0]\n"
                   "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]\n");
  EXPECT_THROW({ (void)validate(std::move(owned_model_with_root)); }, CellGraphCodecError);

  std::string passive_missing = empty_graph();
  passive_missing = replace_once(
      std::move(passive_missing), "model_instances: []",
      "model_instances:\n"
      "  - model_instance_id: passive_model\n"
      "    model_asset_id: fixture_asset\n"
      "    model_revision: " +
          std::string(kZeroDigest));
  EXPECT_THROW({ (void)validate(passive_missing); }, CellGraphCodecError);

  std::string passive_placed =
      replace_once(std::move(passive_missing), "    model_asset_id: fixture_asset\n",
                   "    model_asset_id: fixture_asset\n"
                   "    root_pose_in_cell:\n"
                   "      position_m: [0.0, 0.0, 0.0]\n"
                   "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]\n");
  EXPECT_NO_THROW({ (void)validate(std::move(passive_placed)); });

  const std::string component_root =
      "    root_pose_in_cell:\n"
      "      position_m: [0.0, 0.0, 0.0]\n"
      "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]\n";
  EXPECT_THROW(
      { (void)validate(replace_once(two_instance_graph(false), component_root, "")); },
      CellGraphCodecError);

  std::string attached_with_root = full_graph();
  attached_with_root =
      replace_once(std::move(attached_with_root),
                   "    model_role_bindings:\n"
                   "      - {model_role_id: body, model_instance_id: tool_model}\n"
                   "    runtime_unit_ids: []",
                   "    model_role_bindings:\n"
                   "      - {model_role_id: body, model_instance_id: tool_model}\n"
                   "    runtime_unit_ids: []\n"
                   "    root_pose_in_cell:\n"
                   "      position_m: [0.0, 0.0, 0.0]\n"
                   "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]");
  EXPECT_THROW({ (void)validate(std::move(attached_with_root)); }, CellGraphCodecError);

  std::string model_free_with_root = empty_graph();
  model_free_with_root = replace_once(
      std::move(model_free_with_root), "controllable_components: []",
      "controllable_components:\n"
      "  - controllable_component_id: io\n"
      "    component_type_id: io_type\n"
      "    component_type_revision: " +
          std::string(kZeroDigest) +
          "\n"
          "    model_role_bindings: []\n"
          "    runtime_unit_ids: []\n"
          "    root_pose_in_cell:\n"
          "      position_m: [0.0, 0.0, 0.0]\n"
          "      orientation_xyzw: [0.0, 0.0, 0.0, 1.0]");
  EXPECT_THROW({ (void)validate(std::move(model_free_with_root)); }, CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsInvalidBindingRouteAndResourceDuplicates) {
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(), "legal_control_route_ids: [route_b, route_a]",
                                  "legal_control_route_ids: [route_a]"));
      },
      CellGraphCodecError);
  EXPECT_THROW(
      {
        (void)decode(replace_once(full_graph(),
                                  "{resource_role_id: arm_joints, resource_id: joint_1}",
                                  "{resource_role_id: arm_joints, resource_id: joint_2}"));
      },
      CellGraphCodecError);
}

TEST(CellGraphCodec, RejectsAttachmentCyclesAndMultipleParents) {
  const std::string original = full_graph();
  const std::string edge_marker = "component_attachments:\n";
  const std::size_t edge_position = original.find(edge_marker);
  ASSERT_NE(edge_position, std::string::npos);
  const std::string edge = original.substr(edge_position + edge_marker.size());

  std::string cycle = original;
  std::string reverse_edge = edge;
  reverse_edge = replace_once(std::move(reverse_edge), "tool_on_arm", "arm_on_tool");
  reverse_edge = replace_once(std::move(reverse_edge), "controllable_component_id: arm",
                              "controllable_component_id: TEMP");
  reverse_edge = replace_once(std::move(reverse_edge), "controllable_component_id: tool",
                              "controllable_component_id: arm");
  reverse_edge = replace_once(std::move(reverse_edge), "controllable_component_id: TEMP",
                              "controllable_component_id: tool");
  cycle.append(reverse_edge);
  EXPECT_THROW({ (void)validate(std::move(cycle)); }, CellGraphCodecError);

  std::string self = original;
  const std::size_t attachments = self.find(edge_marker);
  ASSERT_NE(attachments, std::string::npos);
  const std::size_t child_component =
      self.find("controllable_component_id: tool", attachments);
  ASSERT_NE(child_component, std::string::npos);
  self.replace(child_component, std::string("controllable_component_id: tool").size(),
               "controllable_component_id: arm");
  EXPECT_THROW({ (void)validate(std::move(self)); }, CellGraphCodecError);

  std::string duplicate = original;
  std::string duplicate_edge = edge;
  duplicate_edge = replace_once(std::move(duplicate_edge), "tool_on_arm", "tool_on_arm_duplicate");
  duplicate.append(duplicate_edge);
  EXPECT_THROW({ (void)validate(std::move(duplicate)); }, CellGraphCodecError);

  std::string multiple_parent = original;
  std::string second_edge = edge;
  second_edge = replace_once(std::move(second_edge), "tool_on_arm", "tool_on_arm_2");
  second_edge = replace_once(std::move(second_edge), "attachment_interface_id: flange",
                             "attachment_interface_id: flange_2");
  second_edge = replace_once(std::move(second_edge), "attachment_interface_id: mount",
                             "attachment_interface_id: mount_2");
  multiple_parent.append(second_edge);
  EXPECT_THROW({ (void)validate(std::move(multiple_parent)); }, CellGraphCodecError);
}

}  // namespace
}  // namespace manyforge::deployment
