// N17 S2: the launch values the Deployment compiler writes into each
// controller profile row (description, arm joints, tool gripper joint).
//
// - The static frames of a description (moved from the execution host).
// - Parity: on every example the compiled values are byte-identical to the
//   ones the M9 execution host derived from the model map and the planning
//   model's source URDFs.
#include <gtest/gtest.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "component_static_frames_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;
namespace host_contract = ros2_control_execution_runtime_contract;

// ===========================================================================
// Static frames
// ===========================================================================

// TCP-A: the TF static publication is the robot_state_publisher description:
// each static frame becomes one empty link and one fixed joint with the
// frame's origin, appended before </robot>; every other byte is kept.
TEST(ComponentStaticFrames, DescriptionCarriesOffsetFramesAsFixedLinks) {
  const std::string urdf =
      "<?xml version=\"1.0\"?>\n<robot name=\"arm\"><link name=\"link_eef\"/>"
      "<link name=\"link_base\"/><joint name=\"j\" type=\"fixed\"><parent "
      "link=\"link_base\"/><child link=\"link_eef\"/></joint></robot>\n";
  const core::PlacementPoseDTO pen{{0.0, 0.0, 0.12}, {0.0, 0.0, 0.0, 1.0}};
  const std::vector<core::ModelFrameRoleBindingDTO> roles{
      {core::FrameRoleId("flange"), "link_eef"},
      {core::FrameRoleId("pen"), "link_eef", pen}};
  const std::vector<core::ModelCustomTcpDTO> custom{
      {core::FrameRoleId("probe"), "pen", {{0.01, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}}};
  const auto frames = occurrence_offset_frames(roles, custom);
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[0].link_name, "pen");
  EXPECT_FALSE(frames[0].custom);
  EXPECT_EQ(frames[1].parent_link_name, "pen");
  EXPECT_TRUE(frames[1].custom);

  std::vector<UrdfJointIr> joints;
  for (const auto &frame : frames) joints.push_back(offset_frame_joint(frame));
  const auto description = append_fixed_links(urdf, joints);
  EXPECT_EQ(description.substr(0U, urdf.rfind("</robot>")),
            urdf.substr(0U, urdf.rfind("</robot>")));
  EXPECT_NE(description.find("<link name=\"pen\"/><joint name=\"~tcp/pen\" type=\"fixed\">"
                             "<origin xyz=\"0 0 0.12\"/><parent link=\"link_eef\"/>"
                             "<child link=\"pen\"/></joint>"),
            std::string::npos);
  EXPECT_NE(description.find("<link name=\"probe\"/><joint name=\"~tcp/probe\" type=\"fixed\">"
                             "<origin xyz=\"0.01 0 0\"/><parent link=\"pen\"/>"
                             "<child link=\"probe\"/></joint>"),
            std::string::npos);
  EXPECT_EQ(description.substr(description.size() - 9U), "</robot>\n");
  // The result is a URDF robot_state_publisher can read: it parses, and the
  // new links hang off their parents.
  const auto parsed = parse_model_documents_for_projection(description, std::nullopt);
  EXPECT_EQ(parsed.urdf.links.size(), 4U);
  EXPECT_EQ(parsed.urdf.joints.size(), 3U);

  EXPECT_EQ(append_fixed_links(urdf, {}), urdf);
  EXPECT_THROW((void)append_fixed_links("<robot name=\"x\">", joints), ModelSourceError);
}

// A process-less tool's static part: the links fixed to its root, with
// renamed joints; a moving finger and what hangs below it stay out.
TEST(ComponentStaticFrames, FixedLinksBelowStopAtMovingJoints) {
  const auto parsed = parse_model_documents_for_projection(
      "<robot name=\"tool\"><link name=\"mount\"/><link name=\"body\"/>"
      "<link name=\"tcp_link\"/><link name=\"finger\"/><link name=\"pad\"/>"
      "<joint name=\"a\" type=\"fixed\"><parent link=\"mount\"/><child link=\"body\"/>"
      "<origin xyz=\"0 0 0.05\"/></joint>"
      "<joint name=\"b\" type=\"fixed\"><parent link=\"body\"/><child link=\"tcp_link\"/></joint>"
      "<joint name=\"c\" type=\"prismatic\"><parent link=\"body\"/><child link=\"finger\"/>"
      "<axis xyz=\"1 0 0\"/><limit lower=\"0\" upper=\"0.04\" effort=\"1\" velocity=\"1\"/></joint>"
      "<joint name=\"d\" type=\"fixed\"><parent link=\"finger\"/><child link=\"pad\"/></joint>"
      "</robot>",
      std::nullopt);
  const auto joints = detail::fixed_links_below(parsed.urdf, "mount");
  ASSERT_EQ(joints.size(), 2U);
  std::vector<std::string> children;
  for (const auto &joint : joints) {
    children.push_back(joint.child_link_name);
    EXPECT_EQ(joint.name, "~fixed/" + joint.child_link_name);
  }
  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, (std::vector<std::string>{"body", "tcp_link"}));
}

// ===========================================================================
// Parity with the M9 execution host (delete with this section when S2 merges)
// ===========================================================================

// The M9 host's derivation (roboplan_backend::retained_component_description,
// retained_static_frames.cpp), verbatim but for names: from the component's
// spec, the retained model map and the planning model's source URDFs.
namespace m9 {

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("retained static frames: " + message);
}

const VerifiedRetainedModelSourceUrdf &
source_of(std::span<const VerifiedRetainedModelSourceUrdf> sources,
          const core::ModelInstanceDTO &instance) {
  const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto &row) {
    return row.model_asset_id == instance.model_asset_id &&
           row.model_revision == instance.model_revision;
  });
  if (found == sources.end()) fail("model source URDF is absent");
  return *found;
}

void append_offset_frames(const VerifiedRetainedModelSourceUrdf &source,
                          const core::ModelInstanceDTO &instance,
                          std::set<std::string> *links, std::vector<UrdfJointIr> &result) {
  for (const auto &frame : occurrence_offset_frames(source.offset_frame_roles, instance.custom_tcps)) {
    if (links != nullptr && !links->contains(frame.parent_link_name)) continue;
    if (links != nullptr) links->insert(frame.link_name);
    result.push_back(offset_frame_joint(frame));
  }
}

std::vector<UrdfJointIr> fixed_links_below(const UrdfDocumentIr &urdf, const std::string &root_link) {
  std::vector<UrdfJointIr> result;
  std::set<std::string> reached{root_link};
  for (bool grown = true; grown;) {
    grown = false;
    for (const auto &joint : urdf.joints) {
      if (joint.type != UrdfJointType::kFixed || !reached.contains(joint.parent_link_name) ||
          !reached.insert(joint.child_link_name).second)
        continue;
      auto copy = joint;
      copy.name = "~fixed/" + joint.child_link_name;
      result.push_back(std::move(copy));
      grown = true;
    }
  }
  return result;
}

std::vector<UrdfJointIr> retained_component_static_frames(
    const ComponentSpec &spec, const VerifiedRetainedModelMap &model_map,
    std::span<const VerifiedRetainedModelSourceUrdf> sources) {
  const auto &component = spec.controllable_component_id;
  std::vector<UrdfJointIr> result;
  for (const auto &instance : spec.models)
    append_offset_frames(source_of(sources, instance), instance, nullptr, result);
  std::map<std::string, std::pair<core::ModelInstanceId, std::string>, std::less<>> links;
  for (const auto &symbol : model_map.symbols()) {
    if (symbol.source.kind == OccurrenceSymbolKind::kLink)
      links.emplace(symbol.compiled_name,
                    std::pair{symbol.source.model_instance_id, symbol.source.source_name});
  }
  const auto source_link = [&](const std::string &compiled) -> const auto & {
    const auto found = links.find(compiled);
    if (found == links.end()) fail("an attachment link has no source symbol");
    return found->second;
  };
  for (const auto &connection : model_map.fixed_connections()) {
    const auto *attachment = std::get_if<CompositionComponentAttachment>(&connection.source);
    if (attachment == nullptr || attachment->parent_component_id != component) continue;
    const auto &[child_model, child_root] = source_link(connection.compiled_child_link_name);
    result.push_back(UrdfJointIr{
        "~mount/" + child_root, UrdfJointType::kFixed, UrdfCoordinateKind::kNone,
        source_link(connection.compiled_parent_link_name).second, child_root,
        urdf_origin_from_pose(connection.child_pose_in_parent), std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    const auto mount = std::find_if(spec.mounts.begin(), spec.mounts.end(), [&](const auto &row) {
      return row.child == attachment->child_component_id;
    });
    if (mount == spec.mounts.end()) fail("an attachment is absent from the component spec");
    if (!mount->child_runs_no_process) continue;
    const auto instance = std::find_if(
        mount->child_models.begin(), mount->child_models.end(),
        [&](const auto &row) { return row.model_instance_id == child_model; });
    if (instance == mount->child_models.end()) fail("model instance is absent");
    const auto &source = source_of(sources, *instance);
    const auto child = parse_model_documents_for_projection(source.urdf_bytes, std::nullopt);
    std::set<std::string> published{child_root};
    for (auto &joint : fixed_links_below(child.urdf, child_root)) {
      published.insert(joint.child_link_name);
      result.push_back(std::move(joint));
    }
    append_offset_frames(source, *instance, &published, result);
  }
  return result;
}

std::string retained_component_description(
    const std::string &urdf_bytes, const ComponentSpec &spec,
    const VerifiedRetainedModelMap &model_map,
    std::span<const VerifiedRetainedModelSourceUrdf> sources) {
  return append_fixed_links(urdf_bytes, retained_component_static_frames(spec, model_map, sources));
}

} // namespace m9

struct Example final {
  std::string name;
  std::shared_ptr<const RetainedBundle> bundle;
  std::filesystem::path package_root;
};

[[nodiscard]] const std::vector<Example> &Examples() {
  static const std::vector<Example> examples = [] {
    std::vector<Example> result;
    const std::filesystem::path root(MANYFORGE_EXAMPLE_BUNDLE_ROOT);
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(root))
      if (entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      std::ifstream stream(file, std::ios::binary);
      std::ostringstream bytes;
      bytes << stream.rdbuf();
      // The fixture is RFC 8785 JSON from Python; normalize it through ours.
      result.push_back({file.stem().string(),
                        detail::RetainedBundleReloadAuthority::reload(canonical_json(
                            parse_strict_json_model_yaml(bytes.str()).root())),
                        root / "packages" / file.stem() / "activation"});
    }
    if (result.empty()) throw std::logic_error("no example bundle was produced");
    return result;
  }();
  return examples;
}

/// The source joints of a component target's binding in the model map, in
/// binding order: what the M9 host took for its controllers file.
[[nodiscard]] std::vector<std::string> BindingSourceJoints(const VerifiedRetainedModelMap &map,
                                                           const core::ControllableComponentId &id,
                                                           std::string_view target) {
  for (const auto &binding : map.control_bindings()) {
    if (binding.controllable_component_id != id || binding.command_target_id.value() != target)
      continue;
    std::vector<std::string> joints;
    for (const auto &resource : binding.resources)
      joints.push_back(resource.coordinates.front().source_joint_name);
    return joints;
  }
  return {};
}

TEST(ComponentDescription, CompiledLaunchValuesEqualTheM9HostOnEveryExample) {
  std::size_t arms = 0;
  std::size_t tools = 0;
  for (const auto &example : Examples()) {
    SCOPED_TRACE(example.name);
    const auto &bundle = *example.bundle;
    const auto &products = bundle.value().compiled_products;
    if (products.symbol_role_maps.empty()) continue;
    const FilesystemVerifiedRetainedModelMapSource map_source(example.package_root);
    const auto model_map = map_source.get_verified(
        {bundle.retained_bundle_id(), products.symbol_role_maps.front().product_id});
    ASSERT_TRUE(model_map.has_value());
    const FilesystemVerifiedRetainedPlanningModelSource planning_source(example.package_root);
    const auto planning = planning_source.get_verified(
        {bundle.retained_bundle_id(), products.planning_collision_model.product_id});
    ASSERT_TRUE(planning.has_value());
    const auto &sources = planning->model_source_urdfs();
    for (const auto &unit : bundle.value().cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != host_contract::kRuntimeProfileId) continue;
      for (const auto &profile :
           host_contract::require_configuration(bundle, unit.runtime_unit_id).controller_profiles) {
        const auto &id = profile.controllable_component_id;
        SCOPED_TRACE(id.value());
        const auto spec = component_spec(bundle, SiteFacts{}, id);
        // The host launches from the spec's row: the same row.
        const auto *own = host_contract::find_controller_profile(spec.host_configuration, id);
        ASSERT_NE(own, nullptr);
        EXPECT_EQ(*own, profile);
        ASSERT_EQ(spec.models.size(), 1U);
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &row) {
          return row.model_asset_id == spec.models.front().model_asset_id &&
                 row.model_revision == spec.models.front().model_revision;
        });
        ASSERT_NE(source, sources.end());
        EXPECT_EQ(profile.description,
                  m9::retained_component_description(source->urdf_bytes, spec, *model_map, sources));
        const auto arm = BindingSourceJoints(*model_map, id, "arm");
        if (!arm.empty()) {
          ++arms;
          EXPECT_EQ(profile.joint_names, arm);
          EXPECT_FALSE(profile.gripper_joint_name.has_value());
        } else {
          ++tools;
          const auto gripper = BindingSourceJoints(*model_map, id, "gripper");
          ASSERT_EQ(gripper.size(), 1U);
          EXPECT_EQ(profile.gripper_joint_name, std::optional(gripper.front()));
          EXPECT_TRUE(profile.joint_names.empty());
        }
      }
    }
  }
  EXPECT_GT(arms, 0U);
  EXPECT_GT(tools, 0U);
}

} // namespace
} // namespace manyforge::deployment
