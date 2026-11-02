#include "manyforge_roboplan/retained_tool_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <variant>

#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("retained tool context: " + message);
}

[[nodiscard]] core::Result<core::ToolContextDTO>
refused(std::string reason, std::string message, const std::string &frame) {
  return core::Result<core::ToolContextDTO>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
      std::move(message), {{"reason", std::move(reason)}, {"frame", frame}}});
}

[[nodiscard]] core::Result<core::ToolContextDTO>
not_rigid(const std::string &frame) {
  return refused("tool_context_frame_not_rigid",
                 "the frame is not rigidly fixed to the robot's flange", frame);
}

[[nodiscard]] const std::string *
compiled_frame(const deployment::CompositionTargetFrameResolution &frame) {
  const auto *model =
      std::get_if<deployment::CompositionComponentModelFrame>(&frame.frame);
  return model == nullptr ? nullptr : &model->compiled_frame_name;
}

/// The compiled frame of one of the target's own frame rows.
[[nodiscard]] const std::string *
own_frame(const deployment::CompositionTargetResolution &target,
          const core::FrameRoleId &role) {
  const auto frame = std::find_if(
      target.frames.begin(), target.frames.end(),
      [&](const auto &row) { return row.frame_role_id == role; });
  return frame == target.frames.end() ? nullptr : compiled_frame(*frame);
}

[[nodiscard]] core::Pose3DTO point(const std::array<double, 3> &position) {
  return {{position[0], position[1], position[2]}, {0.0, 0.0, 0.0, 1.0}};
}

} // namespace

core::Pose3DTO urdf_origin_pose(const deployment::UrdfOriginIr &origin) {
  const auto &[roll, pitch, yaw] = origin.rpy_rad;
  const double cr = std::cos(roll / 2.0), sr = std::sin(roll / 2.0);
  const double cp = std::cos(pitch / 2.0), sp = std::sin(pitch / 2.0);
  const double cy = std::cos(yaw / 2.0), sy = std::sin(yaw / 2.0);
  // R = Rz(yaw) Ry(pitch) Rx(roll).
  return {{origin.xyz_m[0], origin.xyz_m[1], origin.xyz_m[2]},
          {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
           cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy}};
}

RetainedToolContextFrames retained_tool_context_frames(
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const core::ControllableComponentId &robot,
    const core::CommandTargetId &motion_target) {
  RetainedToolContextFrames result;
  const auto *target = model_map.find_target(robot, motion_target);
  if (target == nullptr) fail("the robot's motion target is absent");
  const auto *flange = own_frame(*target, core::FrameRoleId("flange"));
  if (flange == nullptr) fail("the robot's motion target has no flange frame");
  result.flange_link = *flange;
  const auto &index = model_map.kinematic_index();
  // B1/B2: the End-effector mark's own frame row; no mark, no default.
  if (const auto mark = index.end_effector_entry(robot, motion_target)) {
    const auto *owner = model_map.find_target(mark->component_id, mark->target_id);
    const auto *tcp = owner == nullptr ? nullptr : own_frame(*owner, mark->frame_role_id);
    if (tcp == nullptr) fail("the end-effector TCP is absent from the model map");
    result.end_effector_tcp_frame = *tcp;
  }

  result.carriers = index.self_and_mounted(robot);
  for (const auto &candidate : model_map.targets()) {
    if (!result.carriers.contains(candidate.controllable_component_id)) continue;
    for (const auto &frame : candidate.frames) {
      if (const auto *name = compiled_frame(frame))
        result.carrier_frames.emplace(
            RetainedToolContextFrames::CarrierKey{
                candidate.controllable_component_id, candidate.command_target_id,
                frame.frame_role_id},
            *name);
    }
  }

  std::map<std::string, core::ModelInstanceId, std::less<>> link_instances;
  for (const auto &symbol : model_map.symbols()) {
    if (symbol.source.kind == deployment::OccurrenceSymbolKind::kLink)
      link_instances.emplace(symbol.compiled_name, symbol.source.model_instance_id);
  }
  for (const auto &connection : model_map.fixed_connections()) {
    const auto *attachment =
        std::get_if<deployment::CompositionComponentAttachment>(&connection.source);
    if (attachment == nullptr || attachment->child_component_id == robot ||
        !result.carriers.contains(attachment->child_component_id))
      continue;
    const auto instance_id = link_instances.find(connection.compiled_child_link_name);
    if (instance_id == link_instances.end()) fail("a mount link has no model");
    const auto instance = std::find_if(
        graph.model_instances.begin(), graph.model_instances.end(),
        [&](const auto &row) { return row.model_instance_id == instance_id->second; });
    if (instance == graph.model_instances.end()) fail("a mounted model is absent");
    const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto &row) {
      return row.model_asset_id == instance->model_asset_id &&
             row.model_revision == instance->model_revision;
    });
    if (source == sources.end()) fail("a mounted model's source is absent");
    if (source->payload)
      result.tool_payloads.emplace_back(connection.compiled_child_link_name,
                                        *source->payload);
  }
  return result;
}

RetainedToolContextSource::RetainedToolContextSource(
    const deployment::UrdfDocumentIr &cell_urdf, RetainedToolContextFrames frames,
    SceneReader scene)
    : frames_(std::move(frames)), scene_(std::move(scene)) {
  for (const auto &joint : cell_urdf.joints)
    parent_joints_.emplace(
        joint.child_link_name,
        ParentJoint{joint.parent_link_name,
                    joint.type == deployment::UrdfJointType::kFixed,
                    urdf_origin_pose(joint.origin)});
  // Each mounted tool's own payload, at its mount, in the flange frame.
  for (const auto &[mount_link, payload] : frames_.tool_payloads) {
    if (payload.mass_kg <= 0.0) continue;
    const auto mount = PoseInFlange(mount_link);
    if (!mount) fail("a mounted tool is not rigidly fixed to the flange");
    const auto at = core::multiply_poses(*mount, point(payload.center_of_mass_m));
    tool_masses_.push_back(
        {payload.mass_kg, {at.position_m.x_m, at.position_m.y_m, at.position_m.z_m}});
  }
}

std::optional<core::Pose3DTO>
RetainedToolContextSource::PoseInFlange(const std::string &link) const {
  core::Pose3DTO pose{};
  std::string current = link;
  // Every step moves to a parent; more steps than joints is a cycle.
  for (std::size_t steps = 0; current != frames_.flange_link; ++steps) {
    const auto joint = parent_joints_.find(current);
    if (joint == parent_joints_.end() || !joint->second.fixed ||
        steps > parent_joints_.size())
      return std::nullopt;
    pose = core::multiply_poses(joint->second.origin, pose);
    current = joint->second.parent_link;
  }
  return pose;
}

core::Result<core::ToolContextDTO> RetainedToolContextSource::Compose(
    const std::optional<std::string> &tcp_frame) const {
  const auto scene = scene_ ? scene_() : nullptr;
  return ComposeSnapshot(tcp_frame, scene.get());
}

core::Result<core::ToolContextDTO> RetainedToolContextSource::Compose(
    const std::optional<std::string> &tcp_frame,
    const core::LogicalSceneSnapshotDTO &scene) const {
  return ComposeSnapshot(tcp_frame, &scene);
}

core::Result<core::ToolContextDTO> RetainedToolContextSource::ComposePayload(
    const core::LogicalSceneSnapshotDTO &scene) const {
  return ComposeSnapshot(frames_.flange_link, &scene);
}

core::Result<core::ToolContextDTO> RetainedToolContextSource::ComposeSnapshot(
    const std::optional<std::string> &tcp_frame,
    const core::LogicalSceneSnapshotDTO *scene) const {
  // B-run-1: no TCP at all (the robot has no End-effector TCP, so its Active
  // TCP is none): the flange, a zero offset. A move never gets here without
  // a TCP (C-P2: it is refused before).
  const auto &selected =
      tcp_frame ? *tcp_frame : frames_.end_effector_tcp_frame.value_or(frames_.flange_link);
  const auto tcp = PoseInFlange(selected);
  if (!tcp) return not_rigid(selected);
  auto masses = tool_masses_;
  if (scene) {
    for (const auto &attachment : scene->carried_object_attachments) {
      const auto *ref =
          std::get_if<core::ComponentSceneFrameRefDTO>(&attachment.carrier_frame_ref);
      if (ref == nullptr || !frames_.carriers.contains(ref->controllable_component_id))
        continue;
      const auto object = std::lower_bound(
          scene->objects.begin(), scene->objects.end(), attachment.scene_resource_id,
          [](const core::SceneObjectSnapshotDTO &row, const core::SceneResourceId &id) {
            return row.scene_resource_id < id;
          });
      if (object == scene->objects.end() ||
          object->scene_resource_id != attachment.scene_resource_id ||
          object->mass_kg <= 0.0)
        continue;
      const auto carrier = frames_.carrier_frames.find(RetainedToolContextFrames::CarrierKey{
          ref->controllable_component_id, attachment.command_target_id,
          ref->frame_role_id});
      if (carrier == frames_.carrier_frames.end())
        return refused("tool_context_frame_unknown",
                       "an attached object's carrier frame is unknown",
                       ref->frame_role_id.value());
      const auto carrier_pose = PoseInFlange(carrier->second);
      if (!carrier_pose) return not_rigid(carrier->second);
      const auto at =
          core::multiply_poses(*carrier_pose, attachment.pose_in_carrier_frame);
      masses.push_back({object->mass_kg,
                        {at.position_m.x_m, at.position_m.y_m, at.position_m.z_m}});
    }
  }
  return core::Result<core::ToolContextDTO>::Ok(
      core::compose_tool_context(*tcp, masses));
}

} // namespace manyforge::roboplan_backend
