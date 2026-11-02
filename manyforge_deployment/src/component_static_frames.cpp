#include "component_static_frames_internal.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace manyforge::deployment::detail {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("component static frames: " + message);
}

[[nodiscard]] const VerifiedRetainedModelSourceUrdf &
source_of(std::span<const VerifiedRetainedModelSourceUrdf> sources,
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
void append_offset_frames(const VerifiedRetainedModelSourceUrdf &source,
                          const core::ModelInstanceDTO &instance,
                          std::set<std::string> *links,
                          std::vector<UrdfJointIr> &result) {
  for (const auto &frame :
       occurrence_offset_frames(source.offset_frame_roles, instance.custom_tcps)) {
    // Descriptor offsets come first, so a custom TCP on one finds its parent.
    if (links != nullptr && !links->contains(frame.parent_link_name)) continue;
    if (links != nullptr) links->insert(frame.link_name);
    result.push_back(offset_frame_joint(frame));
  }
}

} // namespace

std::vector<UrdfJointIr> fixed_links_below(const UrdfDocumentIr &urdf,
                                           const std::string &root_link) {
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

std::vector<UrdfJointIr> component_static_frames(const ComponentDescriptionInput &input) {
  const auto &component = input.component;
  std::vector<UrdfJointIr> result;
  for (const auto &instance : input.facts.models)
    append_offset_frames(source_of(input.sources, instance), instance, nullptr, result);

  // Compiled link name -> its occurrence and source link name.
  std::map<std::string, std::pair<core::ModelInstanceId, std::string>, std::less<>> links;
  for (const auto &symbol : input.symbols) {
    if (symbol.source.kind == OccurrenceSymbolKind::kLink)
      links.emplace(symbol.compiled_name,
                    std::pair{symbol.source.model_instance_id, symbol.source.source_name});
  }
  const auto source_link = [&](const std::string &compiled) -> const auto & {
    const auto found = links.find(compiled);
    if (found == links.end()) fail("an attachment link has no source symbol");
    return found->second;
  };
  for (const auto &connection : input.fixed_connections) {
    const auto *attachment = std::get_if<CompositionComponentAttachment>(&connection.source);
    if (attachment == nullptr || attachment->parent_component_id != component) continue;
    const auto &[child_model, child_root] = source_link(connection.compiled_child_link_name);
    result.push_back(UrdfJointIr{
        "~mount/" + child_root, UrdfJointType::kFixed, UrdfCoordinateKind::kNone,
        source_link(connection.compiled_parent_link_name).second, child_root,
        urdf_origin_from_pose(connection.child_pose_in_parent), std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    const auto mount = std::find_if(
        input.facts.mounts.begin(), input.facts.mounts.end(),
        [&](const auto &row) { return row.child == attachment->child_component_id; });
    if (mount == input.facts.mounts.end()) fail("an attachment is absent from the component's mounts");
    if (!mount->child_runs_no_process) continue;
    // A process-less child: its links fixed to its root, then its offset
    // frames on those links.
    const auto instance = std::find_if(
        mount->child_models.begin(), mount->child_models.end(),
        [&](const auto &row) { return row.model_instance_id == child_model; });
    if (instance == mount->child_models.end()) fail("model instance is absent");
    const auto &source = source_of(input.sources, *instance);
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

std::string component_description(const std::string &urdf_bytes,
                                  const ComponentDescriptionInput &input) {
  return append_fixed_links(urdf_bytes, component_static_frames(input));
}

} // namespace manyforge::deployment::detail
