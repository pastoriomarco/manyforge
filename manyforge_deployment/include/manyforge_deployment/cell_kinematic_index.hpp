#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/kinematic_state_view.hpp>
#include <manyforge_core/types/resolved_frame.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment {

/// The cell graph's mount and ownership index, plus the retained kinematic
/// frame paths built beside it. It is derived data: no state, authority,
/// lifecycle, id or digest of its own. The Deployment (compiled into the
/// retained cell graph) owns what is mounted on what and who owns each model;
/// the verified model map owns the frame paths.
///
/// It relies on the graph invariants the codec and structure validation
/// already enforce (one parent per child, acyclic, unique model ownership) and
/// checks none of them. Queries return null or empty; callers keep their own
/// typed errors. Returned pointers live as long as the index.
class CellKinematicIndex final {
 public:
  explicit CellKinematicIndex(
      const core::CellGraphDTO& graph,
      std::vector<core::KinematicFramePathDTO> frame_paths = {});

  // Mounts and owners (graph).

  /// The component whose model role bindings name this model; null for a
  /// passive model. The first owner in graph order.
  [[nodiscard]] const core::ControllableComponentId* owner_of(
      const core::ModelInstanceId& model) const;
  /// The parent endpoint of the component's mount; null when not mounted.
  [[nodiscard]] const core::ComponentAttachmentEndpointDTO* mounted_on(
      const core::ControllableComponentId& child) const;
  /// True when `member` is `carrier` or mounted below it (transitive).
  [[nodiscard]] bool carries(const core::ControllableComponentId& carrier,
                             const core::ControllableComponentId& member) const;
  /// The graph components carried by `carrier`, itself included (transitive).
  [[nodiscard]] std::set<core::ControllableComponentId> self_and_mounted(
      const core::ControllableComponentId& carrier) const;
  /// The components mounted directly on `parent`, in graph edge order.
  [[nodiscard]] std::vector<core::ControllableComponentId> children_of(
      const core::ControllableComponentId& parent) const;
  /// The tool on `parent`'s `interface`: the primary child there (graph
  /// validation requires exactly one primary, on a tool, when two or more
  /// components tagged `tool` share the interface); else the first child that
  /// has a default TCP, so a sibling without a TCP (a camera that is no tool)
  /// needs no mark; else the first child. The graph carries no tags, so two
  /// cases follow from "has a default TCP" rather than from the `tool` tag:
  /// a primary mark on a non-tool beside at most one tool is still honoured,
  /// and beside a tool without a TCP (a bracket) a non-tool that has one (a
  /// camera with a TCP) is chosen, so its TCP becomes the End-effector TCP.
  [[nodiscard]] const core::ControllableComponentId* tool_on(
      const core::ControllableComponentId& parent,
      std::string_view interface) const;
  /// The mount chain above `component`, nearest first, itself excluded.
  [[nodiscard]] std::vector<core::ControllableComponentId> ancestors_of(
      const core::ControllableComponentId& component) const;

  // TCP defaults.

  /// The component's compiled default TCP; null when it has none.
  [[nodiscard]] const core::ComponentDefaultTcpDTO* default_tcp_of(
      const core::ControllableComponentId& component) const;
  /// The End-effector TCP of `robot` (a default mark on one entry): the
  /// default TCP of the tool on its `flange`, none when that tool has no
  /// default (no fallback); else the robot's own default TCP; else
  /// `{robot, robot_target, flange}`. Direct (one level); resolves no names.
  [[nodiscard]] std::optional<core::TcpEntryDTO> end_effector_entry(
      const core::ControllableComponentId& robot,
      const core::CommandTargetId& robot_target) const;

  // Frame paths (moved from VerifiedRetainedModelMap).

  [[nodiscard]] const std::vector<core::KinematicFramePathDTO>& frame_paths()
      const noexcept {
    return frame_paths_;
  }
  /// Exact compiler-owned path lookup for one retained runtime frame.
  /// Missing and duplicate terminal identities both return null.
  [[nodiscard]] const core::KinematicFramePathDTO* find_frame_path(
      const core::ResolvedFrameDTO& terminal_frame) const noexcept;

 private:
  std::vector<core::ControllableComponentId> components_;  // graph order
  // Parallel to `components_`.
  std::vector<std::optional<core::ComponentDefaultTcpDTO>> default_tcps_;
  std::vector<core::ComponentAttachmentEdgeDTO> edges_;    // graph order
  // Sorted by model id; the first owner in graph order per model.
  std::vector<std::pair<core::ModelInstanceId, core::ControllableComponentId>>
      owners_;
  std::vector<core::KinematicFramePathDTO> frame_paths_;
};

}  // namespace manyforge::deployment
