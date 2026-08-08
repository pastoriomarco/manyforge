#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/scene_frame_resolution.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

[[nodiscard]] bool finite_pose(const Pose3DTO& pose) noexcept {
  const double values[] = {pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m,
                           pose.orientation.x,  pose.orientation.y,  pose.orientation.z,
                           pose.orientation.w};
  for (const double value : values) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  const double norm =
      pose.orientation.x * pose.orientation.x + pose.orientation.y * pose.orientation.y +
      pose.orientation.z * pose.orientation.z + pose.orientation.w * pose.orientation.w;
  return std::abs(norm - 1.0) <= 1.0e-6;
}

[[nodiscard]] bool resolved_frame_valid(const ResolvedFrameDTO& frame) noexcept {
  return std::visit(
      [](const auto& value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResolvedCellFrameDTO>) {
          return !value.cell_id.empty() && !value.compiled_frame_name.empty();
        } else if constexpr (std::is_same_v<T, ResolvedComponentModelFrameDTO>) {
          return !value.controllable_component_id.empty() && !value.model_instance_id.empty() &&
                 !value.frame_role_id.empty() && !value.compiled_frame_name.empty() &&
                 !value.model_revision.empty();
        } else {
          return !value.model_instance_id.empty() && !value.frame_role_id.empty() &&
                 !value.compiled_frame_name.empty() && !value.model_revision.empty();
        }
      },
      frame);
}

[[nodiscard]] CanonicalValue text(const std::string_view value) {
  return CanonicalValue(std::string(value));
}

template <typename Id>
[[nodiscard]] CanonicalValue id(const Id& value) {
  return text(value.value());
}

[[nodiscard]] CanonicalValue resolved_frame_value(const ResolvedFrameDTO& frame) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResolvedCellFrameDTO>) {
          return CanonicalValue(
              CanonicalValue::Object{{"cell_id", id(value.cell_id)},
                                     {"compiled_frame_name", text(value.compiled_frame_name)},
                                     {"kind", text("cell")}});
        } else if constexpr (std::is_same_v<T, ResolvedComponentModelFrameDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"compiled_frame_name", text(value.compiled_frame_name)},
              {"controllable_component_id", id(value.controllable_component_id)},
              {"frame_role_id", id(value.frame_role_id)},
              {"kind", text("component_model")},
              {"model_instance_id", id(value.model_instance_id)},
              {"model_revision", id(value.model_revision)}});
        } else {
          return CanonicalValue(
              CanonicalValue::Object{{"compiled_frame_name", text(value.compiled_frame_name)},
                                     {"frame_role_id", id(value.frame_role_id)},
                                     {"kind", text("passive_model")},
                                     {"model_instance_id", id(value.model_instance_id)},
                                     {"model_revision", id(value.model_revision)}});
        }
      },
      frame);
}

[[nodiscard]] CanonicalValue pose_value(const Pose3DTO& pose) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation_xyzw",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(pose.orientation.x), CanonicalValue(pose.orientation.y),
           CanonicalValue(pose.orientation.z), CanonicalValue(pose.orientation.w)})},
      {"position_m", CanonicalValue(CanonicalValue::Array{CanonicalValue(pose.position_m.x_m),
                                                          CanonicalValue(pose.position_m.y_m),
                                                          CanonicalValue(pose.position_m.z_m)})}});
}

[[nodiscard]] CanonicalValue coordinate_position_value(
    const CoordinatePositionDTO& position) {
  if (const auto* angular = std::get_if<AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", text("angular_rad")},
        {"value_rad", CanonicalValue(angular->value_rad)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", text("linear_m")},
      {"value_m",
       CanonicalValue(std::get<LinearPositionDTO>(position).value_m)}});
}

[[nodiscard]] CanonicalValue coordinate_velocity_value(
    const CoordinateVelocityDTO& velocity) {
  if (const auto* angular = std::get_if<AngularVelocityDTO>(&velocity)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", text("angular_rad_s")},
        {"value_rad_s", CanonicalValue(angular->value_rad_s)}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", text("linear_m_s")},
      {"value_m_s",
       CanonicalValue(std::get<LinearVelocityDTO>(velocity).value_m_s)}});
}

[[nodiscard]] CanonicalValue controlled_state_value(
    const ControlledStateDTO& state) {
  CanonicalValue::Array resources;
  resources.reserve(state.resources.size());
  for (const auto& resource : state.resources) {
    resources.push_back(id(resource));
  }
  CanonicalValue::Array joint_names;
  joint_names.reserve(state.joint_names.size());
  for (const auto& joint_name : state.joint_names) {
    joint_names.push_back(text(joint_name));
  }
  CanonicalValue::Array positions;
  positions.reserve(state.positions.size());
  for (const auto& position : state.positions) {
    positions.push_back(coordinate_position_value(position));
  }
  CanonicalValue velocities(nullptr);
  if (state.velocities.has_value()) {
    CanonicalValue::Array values;
    values.reserve(state.velocities->size());
    for (const auto& velocity : *state.velocities) {
      values.push_back(coordinate_velocity_value(velocity));
    }
    velocities = CanonicalValue(std::move(values));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_control_configuration_generation",
       text(std::to_string(
           state.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       id(state.active_control_configuration_id)},
      {"command_target_id", id(state.command_target_id)},
      {"controllable_component_id", id(state.controllable_component_id)},
      {"control_binding_id", id(state.control_binding_id)},
      {"control_binding_revision", id(state.control_binding_revision)},
      {"control_domain_id", id(state.control_domain_id)},
      {"control_domain_revision", id(state.control_domain_revision)},
      {"controller_topology_id", id(state.controller_topology_id)},
      {"control_route_id", id(state.control_route_id)},
      {"joint_names", CanonicalValue(std::move(joint_names))},
      {"positions", CanonicalValue(std::move(positions))},
      {"resources", CanonicalValue(std::move(resources))},
      {"topology_revision", id(state.topology_revision)},
      {"velocities", std::move(velocities)}});
}

[[nodiscard]] CanonicalValue projection_binding_value(const ProjectionLeaseBindingDTO& binding) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", id(binding.projection_revision.graph_revision)},
      {"projection_lease_id", id(binding.projection_lease_id)},
      {"retained_bundle_id", id(binding.projection_revision.retained_bundle_id)},
      {"scene_content_hash", id(binding.projection_revision.scene_revision.content_hash)},
      {"scene_id", id(binding.projection_revision.scene_revision.scene_id)},
      {"scene_revision_id",
       text(std::to_string(binding.projection_revision.scene_revision.revision_id))}});
}

[[nodiscard]] bool selector_valid(const SceneFrameResolutionSelectorDTO& selector) noexcept {
  return std::visit(
      [](const auto& selected) {
        using T = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, SceneFrameRefDTO>) {
          return std::visit([](const auto& frame) { return frame.valid(); }, selected);
        } else {
          return selected.valid();
        }
      },
      selector);
}

[[nodiscard]] bool canonical_attachment_revisions(
    const std::vector<ComponentAttachmentRevision>& revisions) noexcept {
  return std::all_of(revisions.begin(), revisions.end(),
                     [](const auto& revision) { return !revision.empty(); }) &&
         std::adjacent_find(revisions.begin(), revisions.end(),
                            [](const auto& left, const auto& right) { return !(left < right); }) ==
             revisions.end();
}

}  // namespace

bool SceneFrameResolutionQueryDTO::valid() const noexcept {
  return selector_valid(selector) && projection_binding.valid();
}

bool CandidateSceneFrameResolutionQueryDTO::valid() const noexcept {
  try {
    return selector_valid(selector) && projection_binding.valid() &&
           validate_controlled_state(predicted_state).ok();
  } catch (...) {
    return false;
  }
}

bool SceneFrameResolutionContextDTO::valid() const noexcept {
  return projection_binding.valid() && logical_scene != nullptr && logical_scene->locally_valid() &&
         logical_scene->revision == projection_binding.projection_revision.scene_revision &&
         kinematic_path == kinematic_state_path_view.query.frame_path &&
         kinematic_state_path_view.query.retained_bundle_id ==
             projection_binding.projection_revision.retained_bundle_id &&
         kinematic_state_path_view.query.graph_revision ==
             projection_binding.projection_revision.graph_revision &&
         IsExactLowercaseSha256Digest(kinematic_state_path_view.view_digest.value());
}

bool ResolvedSceneFramePoseDTO::valid() const noexcept {
  if (!projection_binding.valid() || !resolved_frame_valid(resolved_frame) ||
      !finite_pose(frame_pose_in_cell) ||
      !canonical_attachment_revisions(component_attachment_revisions) ||
      kinematic_path.terminal_frame != resolved_frame ||
      kinematic_state_path_view.query.frame_path != kinematic_path ||
      kinematic_state_path_view.query.retained_bundle_id !=
          projection_binding.projection_revision.retained_bundle_id ||
      kinematic_state_path_view.query.graph_revision !=
          projection_binding.projection_revision.graph_revision ||
      !IsExactLowercaseSha256Digest(kinematic_state_path_view.view_digest.value()) ||
      (predicted_state_digest.has_value() &&
       !IsExactLowercaseSha256Digest(predicted_state_digest->value())) ||
      !IsExactLowercaseSha256Digest(resolution_evidence_digest.value())) {
    return false;
  }
  try {
    return ComputeSceneFrameResolutionEvidenceDigest(*this) == resolution_evidence_digest;
  } catch (...) {
    return false;
  }
}

bool SceneFrameResolutionReceiptDTO::valid() const noexcept {
  return projection_binding.valid() && resolved_frame_valid(resolved_frame) &&
         finite_pose(frame_pose_in_cell) &&
         canonical_attachment_revisions(component_attachment_revisions) &&
         (!predicted_state_digest.has_value() ||
          IsExactLowercaseSha256Digest(predicted_state_digest->value())) &&
         IsExactLowercaseSha256Digest(resolution_evidence_digest.value());
}

SceneFrameResolutionReceiptDTO MakeSceneFrameResolutionReceipt(
    const ResolvedSceneFramePoseDTO& resolution) {
  if (!resolution.valid()) {
    throw std::invalid_argument("cannot make a receipt from invalid scene-frame resolution");
  }
  return {resolution.resolved_frame, resolution.frame_pose_in_cell,
          resolution.component_attachment_revisions, resolution.projection_binding,
          resolution.predicted_state_digest,
          resolution.resolution_evidence_digest};
}

ContentDigest ComputeCandidateStateEvidenceDigest(
    const ControlledStateDTO& state) {
  if (!validate_controlled_state(state).ok()) {
    throw std::invalid_argument(
        "candidate controlled-state evidence is invalid");
  }
  return ContentDigest(content_identity::framed_sha256(
      "manyforge.candidate_controlled_state",
      "manyforge.candidate_controlled_state.v1", controlled_state_value(state)));
}

ContentDigest ComputeSceneFrameResolutionEvidenceDigest(
    const ResolvedSceneFramePoseDTO& resolution) {
  if (!resolution.projection_binding.valid() || !resolved_frame_valid(resolution.resolved_frame) ||
      !finite_pose(resolution.frame_pose_in_cell) ||
      !canonical_attachment_revisions(resolution.component_attachment_revisions) ||
      resolution.kinematic_path.terminal_frame != resolution.resolved_frame ||
      resolution.kinematic_state_path_view.query.frame_path != resolution.kinematic_path ||
      !IsExactLowercaseSha256Digest(resolution.kinematic_state_path_view.view_digest.value()) ||
      (resolution.predicted_state_digest.has_value() &&
       !IsExactLowercaseSha256Digest(resolution.predicted_state_digest->value()))) {
    throw std::invalid_argument("scene-frame resolution evidence input is incomplete");
  }
  CanonicalValue::Object fields{
      {"component_attachment_revisions",
       [&] {
         CanonicalValue::Array revisions;
         revisions.reserve(resolution.component_attachment_revisions.size());
         for (const auto& revision : resolution.component_attachment_revisions) {
           revisions.push_back(id(revision));
         }
         return CanonicalValue(std::move(revisions));
      }()},
      {"frame_pose_in_cell", pose_value(resolution.frame_pose_in_cell)},
      {"kinematic_state_view_digest", id(resolution.kinematic_state_path_view.view_digest)},
      {"projection_binding", projection_binding_value(resolution.projection_binding)},
      {"resolved_frame", resolved_frame_value(resolution.resolved_frame)}};
  if (resolution.predicted_state_digest.has_value()) {
    fields.emplace_back("predicted_state_digest",
                        id(*resolution.predicted_state_digest));
  }
  const CanonicalValue value(std::move(fields));
  return ContentDigest(content_identity::framed_sha256(
      "manyforge.scene_frame_resolution", "manyforge.scene_frame_resolution.v1", value));
}

}  // namespace manyforge::core
