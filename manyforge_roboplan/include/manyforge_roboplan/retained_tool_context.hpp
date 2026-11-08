#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_core/types/tool_context.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::roboplan_backend {

/// What a robot's tool context is made of in its retained
/// Deployment, in compiled frame names of the composed cell URDF.
struct RetainedToolContextFrames final {
  /// (component, command target, frame role) of an attachment's carrier.
  using CarrierKey =
      std::tuple<core::ControllableComponentId, core::CommandTargetId,
                 core::FrameRoleId>;

  /// The robot's `flange`.
  std::string flange_link;
  /// The End-effector TCP: the frame of the cell kinematic index's default
  /// mark for the robot's motion target; absent when there is none [C-P2].
  std::optional<std::string> end_effector_tcp_frame;
  /// The robot and every component mounted on it, directly or by a chain.
  std::set<core::ControllableComponentId> carriers;
  /// Every frame of those components an object may be attached to, each
  /// under its own target's frame role.
  std::map<CarrierKey, std::string> carrier_frames;
  /// Each mounted tool's root link (the mount) and its model payload, in that
  /// link; tools without a payload are absent.
  std::vector<std::pair<std::string, core::ModelAssetPayloadDTO>> tool_payloads;
};

/// The frames of `robot` (its motion target `motion_target`). Throws
/// std::invalid_argument when the target or its `flange` frame is absent, or
/// when the End-effector mark names a frame the map lacks.
[[nodiscard]] RetainedToolContextFrames retained_tool_context_frames(
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const core::ControllableComponentId &robot,
    const core::CommandTargetId &motion_target);

/// One robot's tool context, vendor-neutral, from its retained
/// Deployment and the current scene. Built once per bind; `Compose` runs at
/// Enable and before every new motion goal.
///
/// - TCP: the given TCP frame (the robot's Active TCP: any frame of the
///   robot or of a tool mounted on it), else the End-effector TCP, as
///   T_flange_tcp, along the composed cell URDF's fixed joints up to the
///   robot's flange. With neither (no TCP at all), the flange: a zero offset;
/// whatever needs a TCP is refused before it gets here.
/// - Payload: each mounted tool's model payload at its mount, plus every
///   carried object attached to the robot or a mounted tool with a
///   `mass_kg`, as a point mass at its attach pose.
///
/// A frame not rigidly fixed to the flange refuses typed
/// (`tool_context_frame_not_rigid`), an attachment on a frame the robot and
/// its tools lack too (`tool_context_frame_unknown`).
class RetainedToolContextSource final {
public:
  /// The latest logical scene; null when this host has none (no attached
  /// mass then).
  using SceneReader =
      std::function<std::shared_ptr<const core::LogicalSceneSnapshotDTO>()>;

  /// Throws std::invalid_argument when a mounted tool's root is not rigidly
  /// fixed to the flange.
  RetainedToolContextSource(const deployment::UrdfDocumentIr &cell_urdf,
                            RetainedToolContextFrames frames,
                            SceneReader scene);

  /// The context with the TCP at `tcp_frame` (a compiled frame), or at the
  /// End-effector TCP when absent, or at the flange without one.
  [[nodiscard]] core::Result<core::ToolContextDTO>
  Compose(const std::optional<std::string> &tcp_frame) const;

  /// Compose from the exact committed snapshot selected by the caller.
  [[nodiscard]] core::Result<core::ToolContextDTO>
  Compose(const std::optional<std::string> &tcp_frame,
          const core::LogicalSceneSnapshotDTO &scene) const;

  /// Compose only the load; no Active TCP pose is required.
  [[nodiscard]] core::Result<core::ToolContextDTO>
  ComposePayload(const core::LogicalSceneSnapshotDTO &scene) const;

  /// T_flange_link through fixed joints only; absent when not rigid.
  [[nodiscard]] std::optional<core::Pose3DTO>
  PoseInFlange(const std::string &link) const;

private:
  [[nodiscard]] core::Result<core::ToolContextDTO>
  ComposeSnapshot(const std::optional<std::string> &tcp_frame,
                  const core::LogicalSceneSnapshotDTO *scene) const;
  struct ParentJoint final {
    std::string parent_link;
    bool fixed{false};
    core::Pose3DTO origin;
  };

  std::map<std::string, ParentJoint, std::less<>> parent_joints_;
  RetainedToolContextFrames frames_;
  std::vector<core::ToolContextPointMassDTO> tool_masses_;
  SceneReader scene_;
};

/// The pose of a URDF origin (fixed-axis roll-pitch-yaw, as URDF).
[[nodiscard]] core::Pose3DTO
urdf_origin_pose(const deployment::UrdfOriginIr &origin);

} // namespace manyforge::roboplan_backend
