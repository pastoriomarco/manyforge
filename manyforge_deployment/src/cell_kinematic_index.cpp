#include "manyforge_deployment/cell_kinematic_index.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace manyforge::deployment {

CellKinematicIndex::CellKinematicIndex(
    const core::CellGraphDTO& graph,
    std::vector<core::KinematicFramePathDTO> frame_paths)
    : edges_(graph.component_attachments),
      frame_paths_(std::move(frame_paths)) {
  components_.reserve(graph.controllable_components.size());
  default_tcps_.reserve(graph.controllable_components.size());
  for (const auto& component : graph.controllable_components) {
    components_.push_back(component.controllable_component_id);
    default_tcps_.push_back(component.default_tcp);
    for (const auto& role : component.model_role_bindings) {
      owners_.emplace_back(role.model_instance_id,
                           component.controllable_component_id);
    }
  }
  // Stable, so the first owner in graph order heads each model's run.
  std::stable_sort(owners_.begin(), owners_.end(),
                   [](const auto& left, const auto& right) {
                     return left.first < right.first;
                   });
  owners_.erase(std::unique(owners_.begin(), owners_.end(),
                            [](const auto& left, const auto& right) {
                              return left.first == right.first;
                            }),
                owners_.end());
}

const core::ControllableComponentId* CellKinematicIndex::owner_of(
    const core::ModelInstanceId& model) const {
  const auto found = std::lower_bound(
      owners_.begin(), owners_.end(), model,
      [](const auto& row, const auto& id) { return row.first < id; });
  if (found == owners_.end() || found->first != model) return nullptr;
  return &found->second;
}

const core::ComponentAttachmentEndpointDTO* CellKinematicIndex::mounted_on(
    const core::ControllableComponentId& child) const {
  const auto edge =
      std::find_if(edges_.begin(), edges_.end(), [&](const auto& row) {
        return row.child.controllable_component_id == child;
      });
  return edge == edges_.end() ? nullptr : &edge->parent;
}

bool CellKinematicIndex::carries(
    const core::ControllableComponentId& carrier,
    const core::ControllableComponentId& member) const {
  // A child has at most one incoming attachment and the graph is acyclic, so
  // walking up from the member reaches every ancestor once.
  for (auto current = member;;) {
    if (current == carrier) return true;
    const auto* parent = mounted_on(current);
    if (parent == nullptr) return false;
    current = parent->controllable_component_id;
  }
}

std::set<core::ControllableComponentId> CellKinematicIndex::self_and_mounted(
    const core::ControllableComponentId& carrier) const {
  std::set<core::ControllableComponentId> result;
  for (const auto& component : components_) {
    if (carries(carrier, component)) result.insert(component);
  }
  return result;
}

std::vector<core::ControllableComponentId> CellKinematicIndex::children_of(
    const core::ControllableComponentId& parent) const {
  std::vector<core::ControllableComponentId> result;
  for (const auto& edge : edges_) {
    if (edge.parent.controllable_component_id == parent)
      result.push_back(edge.child.controllable_component_id);
  }
  return result;
}

const core::ControllableComponentId* CellKinematicIndex::tool_on(
    const core::ControllableComponentId& parent,
    std::string_view interface) const {
  // The primary child; else the first child carrying a TCP (a non-tool
  // sibling, such as a camera, needs no primary mark); else the first child.
  const core::ControllableComponentId* first = nullptr;
  const core::ControllableComponentId* with_tcp = nullptr;
  for (const auto& edge : edges_) {
    if (edge.parent.controllable_component_id != parent ||
        edge.parent.attachment_interface_id.value() != interface)
      continue;
    const auto* child = &edge.child.controllable_component_id;
    if (edge.primary) return child;
    if (first == nullptr) first = child;
    if (with_tcp == nullptr && default_tcp_of(*child) != nullptr) with_tcp = child;
  }
  return with_tcp != nullptr ? with_tcp : first;
}

std::vector<core::ControllableComponentId> CellKinematicIndex::ancestors_of(
    const core::ControllableComponentId& component) const {
  std::vector<core::ControllableComponentId> result;
  for (const auto* parent = mounted_on(component); parent != nullptr;
       parent = mounted_on(parent->controllable_component_id)) {
    result.push_back(parent->controllable_component_id);
  }
  return result;
}

const core::ComponentDefaultTcpDTO* CellKinematicIndex::default_tcp_of(
    const core::ControllableComponentId& component) const {
  const auto found = std::find(components_.begin(), components_.end(), component);
  if (found == components_.end()) return nullptr;
  const auto& tcp = default_tcps_[static_cast<std::size_t>(
      std::distance(components_.begin(), found))];
  return tcp.has_value() ? &*tcp : nullptr;
}

std::optional<core::TcpEntryDTO> CellKinematicIndex::end_effector_entry(
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target) const {
  if (const auto* tool = tool_on(robot, "flange")) {
    const auto* tcp = default_tcp_of(*tool);
    if (tcp == nullptr) return std::nullopt;
    return core::TcpEntryDTO{*tool, tcp->command_target_id, tcp->frame_role_id};
  }
  if (const auto* own = default_tcp_of(robot)) {
    return core::TcpEntryDTO{robot, own->command_target_id, own->frame_role_id};
  }
  return core::TcpEntryDTO{robot, robot_target, core::FrameRoleId("flange")};
}

const core::KinematicFramePathDTO* CellKinematicIndex::find_frame_path(
    const core::ResolvedFrameDTO& terminal_frame) const noexcept {
  const core::KinematicFramePathDTO* match = nullptr;
  for (const auto& candidate : frame_paths_) {
    if (candidate.terminal_frame != terminal_frame) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &candidate;
  }
  return match;
}

}  // namespace manyforge::deployment
