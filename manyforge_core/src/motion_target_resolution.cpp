#include <algorithm>
#include <cmath>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/motion_target_resolution.hpp>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace manyforge::core {

Result<ResolvedMotionSpecDTO> resolve_motion_target(
    const ExecuteRequestDTO &request,
    const ProgramFrameResolutionContextDTO *resolution_context,
    const IReferencePoseResolver &reference_resolver) {
  const auto invalid_target_shape = [](std::string message,
                                       std::string reason) {
    return Result<ResolvedMotionSpecDTO>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 std::move(message),
                 {{"reason", std::move(reason)}}});
  };

  const auto *joint_goal = std::get_if<ResolvedJointGoalDTO>(&request.motion);

  std::set<RuntimeObjectId> referenced_runtime_objects;
  const auto collect_runtime_object = [&](const auto &reference) {
    if (reference.has_value() &&
        reference->type == ReferenceDTO::Type::kRuntimeObject &&
        reference_identity_matches_type(*reference)) {
      referenced_runtime_objects.insert(
          std::get<RuntimeObjectId>(reference->id));
    }
  };
  collect_runtime_object(request.target_reference);
  collect_runtime_object(request.offset_reference);
  if (request.resolved_contact_grasp.has_value()) {
    referenced_runtime_objects.insert(
        request.resolved_contact_grasp->runtime_object_id);
  }
  std::set<RuntimeObjectId> bound_runtime_objects;
  for (const auto &binding : request.runtime_object_bindings) {
    if (binding.runtime_object_id.empty() ||
        binding.scene_resource_id.empty() ||
        !bound_runtime_objects.insert(binding.runtime_object_id).second) {
      return invalid_target_shape(
          "Runtime-object bindings must be complete and unique",
          "runtime_object_binding_invalid");
    }
  }
  if (bound_runtime_objects != referenced_runtime_objects) {
    return invalid_target_shape(
        "Runtime-object bindings must exactly cover the referenced keys",
        "runtime_object_binding_coverage_mismatch");
  }
  if (joint_goal != nullptr) {
    if (joint_goal->joint_names.empty()) {
      return invalid_target_shape(
          "Joint target must contain at least one joint", "joint_target_empty");
    }
    if (joint_goal->joint_names.size() != joint_goal->positions_rad.size()) {
      return invalid_target_shape(
          "Joint names and positions must have the same length",
          "joint_target_shape_mismatch");
    }
    std::unordered_set<std::string> target_joint_names;
    target_joint_names.reserve(joint_goal->joint_names.size());
    for (const auto &joint_name : joint_goal->joint_names) {
      if (joint_name.empty()) {
        return invalid_target_shape("Joint target names must be non-empty",
                                    "joint_target_empty_joint");
      }
      if (!target_joint_names.insert(joint_name).second) {
        return invalid_target_shape(
            "Joint target names must not contain duplicates",
            "joint_target_duplicate_joint");
      }
    }
    if (!std::all_of(joint_goal->positions_rad.begin(),
                     joint_goal->positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      return invalid_target_shape("Joint target positions must be finite",
                                  "joint_target_value_not_finite");
    }
  }

  if (request.target_mode == MotionTargetModeDTO::kAbsolute) {
    if (request.target_reference.has_value() ||
        request.offset_reference.has_value()) {
      return invalid_target_shape(
          "Absolute motion must not carry target references",
          "absolute_target_has_reference");
    }
    return Result<ResolvedMotionSpecDTO>::Ok(request.motion);
  }

  if (joint_goal != nullptr) {
    if (request.target_reference.has_value() ||
        request.offset_reference.has_value()) {
      return invalid_target_shape(
          "Relative joint motion uses named joint deltas and cannot carry a "
          "spatial reference",
          "relative_joint_has_reference");
    }
    if (resolution_context == nullptr || !resolution_context->valid()) {
      return Result<ResolvedMotionSpecDTO>::Err(
          ErrorDTO{kErrorStartStateMismatch,
                   "Relative joint motion requires a fresh authority-selected "
                   "start state",
                   {{"reason", "relative_joint_start_state_unavailable"}}});
    }
    const auto &start_state = resolution_context->start_state;
    if (start_state.joint_names.size() != start_state.positions.size()) {
      return invalid_target_shape(
          "Observed start state is incomplete for relative joint resolution",
          "relative_joint_start_state_incomplete");
    }

    std::unordered_map<std::string, double> start_by_name;
    start_by_name.reserve(start_state.joint_names.size());
    for (std::size_t index = 0; index < start_state.joint_names.size();
         ++index) {
      const auto *angular =
          std::get_if<AngularPositionDTO>(&start_state.positions[index]);
      if (angular == nullptr) {
        return invalid_target_shape(
            "Relative joint start state contains a non-angular coordinate",
            "relative_joint_start_state_unit_mismatch");
      }
      if (!start_by_name
               .emplace(start_state.joint_names[index], angular->value_rad)
               .second) {
        return invalid_target_shape(
            "Observed start state has duplicate joint names",
            "relative_joint_start_state_duplicate_joint");
      }
    }

    ResolvedJointGoalDTO resolved = *joint_goal;
    for (std::size_t index = 0; index < resolved.joint_names.size(); ++index) {
      const auto start = start_by_name.find(resolved.joint_names[index]);
      if (start == start_by_name.end()) {
        return Result<ResolvedMotionSpecDTO>::Err(ErrorDTO{
            kErrorInvalidRequest,
            "Relative joint target names a joint absent from the start state",
            {{"reason", "relative_joint_unknown_joint"},
             {"joint", resolved.joint_names[index]}}});
      }
      resolved.positions_rad[index] += start->second;
      if (!std::isfinite(resolved.positions_rad[index])) {
        return invalid_target_shape(
            "Relative joint resolution produced a non-finite target",
            "relative_joint_result_not_finite");
      }
    }
    return Result<ResolvedMotionSpecDTO>::Ok(
        ResolvedMotionSpecDTO{std::move(resolved)});
  }

  if (std::holds_alternative<NamedTargetDTO>(request.motion)) {
    return invalid_target_shape("Named targets are absolute-only",
                                "relative_named_target_unsupported");
  }
  if (!request.target_reference.has_value()) {
    return invalid_target_shape(
        "Relative pose and Cartesian motion require target_reference",
        "relative_target_reference_missing");
  }
  if (resolution_context == nullptr || !resolution_context->valid()) {
    return Result<ResolvedMotionSpecDTO>::Err(ErrorDTO{
        kErrorStartStateMismatch,
        "Relative spatial motion requires one exact Program start context",
        {{"reason", "relative_reference_context_unavailable"}}});
  }

  auto target_pose = reference_resolver.resolve_reference_pose(
      *request.target_reference, *resolution_context);
  if (!target_pose.ok()) {
    return Result<ResolvedMotionSpecDTO>::Err(target_pose.error());
  }
  Pose3DTO offset_basis_pose = target_pose.value();
  if (request.offset_reference.has_value()) {
    auto offset_reference_pose = reference_resolver.resolve_reference_pose(
        *request.offset_reference, *resolution_context);
    if (!offset_reference_pose.ok()) {
      return Result<ResolvedMotionSpecDTO>::Err(offset_reference_pose.error());
    }
    offset_basis_pose = offset_reference_pose.value();
  }
  const auto resolve_offset = [&](const Pose3DTO &offset) {
    Pose3DTO basis;
    basis.position_m = target_pose.value().position_m;
    basis.orientation = offset_basis_pose.orientation;
    return multiply_poses(basis, offset);
  };

  if (const auto *pose_goal = std::get_if<PoseGoalDTO>(&request.motion)) {
    return Result<ResolvedMotionSpecDTO>::Ok(
        ResolvedMotionSpecDTO{PoseGoalDTO{resolve_offset(pose_goal->goal)}});
  }
  if (const auto *path = std::get_if<CartesianPathDTO>(&request.motion)) {
    CartesianPathDTO resolved = *path;
    for (auto &waypoint : resolved.waypoints) {
      waypoint = resolve_offset(waypoint);
    }
    return Result<ResolvedMotionSpecDTO>::Ok(
        ResolvedMotionSpecDTO{std::move(resolved)});
  }
  return invalid_target_shape(
      "Relative target mode is unsupported for this motion payload",
      "relative_motion_type_unsupported");
}

} // namespace manyforge::core
