#include "manyforge_roboplan/retained_static_frames.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

namespace runtime_contract = deployment::ros2_control_execution_runtime_contract;

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("retained static frames: " + message);
}

[[nodiscard]] const core::ModelInstanceDTO &
model_instance(const core::CellGraphDTO &graph, const core::ModelInstanceId &id) {
  const auto found = std::find_if(
      graph.model_instances.begin(), graph.model_instances.end(),
      [&](const auto &row) { return row.model_instance_id == id; });
  if (found == graph.model_instances.end()) fail("model instance is absent");
  return *found;
}

[[nodiscard]] const deployment::VerifiedRetainedModelSourceUrdf &
source_of(std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
          const core::ModelInstanceDTO &instance) {
  const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto &row) {
    return row.model_asset_id == instance.model_asset_id &&
           row.model_revision == instance.model_revision;
  });
  if (found == sources.end()) fail("model source URDF is absent");
  return *found;
}

// The offset frames of one occurrence whose parents are in `links` (all of
// them for the component's own model, whose description is complete).
void append_offset_frames(const deployment::VerifiedRetainedModelSourceUrdf &source,
                          const core::ModelInstanceDTO &instance,
                          std::set<std::string> *links,
                          std::vector<deployment::UrdfJointIr> &result) {
  for (const auto &frame : deployment::occurrence_offset_frames(
           source.offset_frame_roles, instance.custom_tcps)) {
    // Descriptor offsets come first, so a custom TCP on one finds its parent.
    if (links != nullptr && !links->contains(frame.parent_link_name)) continue;
    if (links != nullptr) links->insert(frame.link_name);
    result.push_back(deployment::offset_frame_joint(frame));
  }
}

} // namespace

std::vector<deployment::UrdfJointIr>
fixed_links_below(const deployment::UrdfDocumentIr &urdf, const std::string &root_link) {
  std::vector<deployment::UrdfJointIr> result;
  std::set<std::string> reached{root_link};
  for (bool grown = true; grown;) {
    grown = false;
    for (const auto &joint : urdf.joints) {
      if (joint.type != deployment::UrdfJointType::kFixed ||
          !reached.contains(joint.parent_link_name) ||
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

bool retained_component_runs_no_process(
    const runtime_contract::Configuration &configuration,
    const core::ControllableComponentId &component) {
  if (std::any_of(configuration.motion_target_limits.begin(),
                  configuration.motion_target_limits.end(),
                  [&](const auto &row) { return row.controllable_component_id == component; }))
    return false;
  bool gripper = false;
  for (const auto &row : configuration.gripper_target_configurations) {
    if (row.controllable_component_id != component) continue;
    gripper = true;
    if (row.output_mode != core::GripperOutputMode::kNone && !row.pneumatic_io) return false;
  }
  return gripper;
}

std::vector<deployment::UrdfJointIr> retained_component_static_frames(
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const runtime_contract::Configuration &configuration,
    const core::ControllableComponentId &component) {
  const auto owner = std::find_if(
      graph.controllable_components.begin(), graph.controllable_components.end(),
      [&](const auto &row) { return row.controllable_component_id == component; });
  if (owner == graph.controllable_components.end()) fail("component is absent");

  std::vector<deployment::UrdfJointIr> result;
  for (const auto &role : owner->model_role_bindings) {
    const auto &instance = model_instance(graph, role.model_instance_id);
    append_offset_frames(source_of(sources, instance), instance, nullptr, result);
  }

  // Compiled link name -> its occurrence and source link name.
  std::map<std::string, std::pair<core::ModelInstanceId, std::string>, std::less<>> links;
  for (const auto &symbol : model_map.symbols()) {
    if (symbol.source.kind == deployment::OccurrenceSymbolKind::kLink)
      links.emplace(symbol.compiled_name,
                    std::pair{symbol.source.model_instance_id, symbol.source.source_name});
  }
  const auto source_link = [&](const std::string &compiled) -> const auto & {
    const auto found = links.find(compiled);
    if (found == links.end()) fail("an attachment link has no source symbol");
    return found->second;
  };
  for (const auto &connection : model_map.fixed_connections()) {
    const auto *attachment =
        std::get_if<deployment::CompositionComponentAttachment>(&connection.source);
    if (attachment == nullptr || attachment->parent_component_id != component) continue;
    const auto &[child_model, child_root] = source_link(connection.compiled_child_link_name);
    result.push_back(deployment::UrdfJointIr{
        "~mount/" + child_root, deployment::UrdfJointType::kFixed,
        deployment::UrdfCoordinateKind::kNone,
        source_link(connection.compiled_parent_link_name).second, child_root,
        deployment::urdf_origin_from_pose(connection.child_pose_in_parent),
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt});
    if (!retained_component_runs_no_process(configuration, attachment->child_component_id))
      continue;
    // A process-less child: its links fixed to its root, then its offset
    // frames on those links.
    const auto &instance = model_instance(graph, child_model);
    const auto &source = source_of(sources, instance);
    const auto child = deployment::parse_model_documents_for_projection(
        source.urdf_bytes, std::nullopt);
    std::set<std::string> published{child_root};
    for (auto &joint : fixed_links_below(child.urdf, child_root)) {
      published.insert(joint.child_link_name);
      result.push_back(std::move(joint));
    }
    append_offset_frames(source, instance, &published, result);
  }
  return result;
}

std::string retained_component_description(
    const std::string &urdf_bytes, const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const runtime_contract::Configuration &configuration,
    const core::ControllableComponentId &component) {
  return deployment::append_fixed_links(
      urdf_bytes, retained_component_static_frames(graph, model_map, sources,
                                                   configuration, component));
}

} // namespace manyforge::roboplan_backend
