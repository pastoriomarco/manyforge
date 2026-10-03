#include <algorithm>
#include <cmath>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using core::ErrorCode;
using core::ErrorDTO;

[[nodiscard]] ErrorDTO error(ErrorCode code, std::string message,
                             std::string reason,
                             std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  return ErrorDTO{std::string(core::to_string(code)), std::move(message),
                  std::move(details)};
}

[[nodiscard]] core::PlanResultDTO plan_error(ErrorDTO value) {
  return {std::nullopt, std::move(value), {}};
}

[[nodiscard]] core::IkResultDTO ik_error(ErrorDTO value) {
  return {false, {}, {}, {}, std::move(value)};
}

[[nodiscard]] std::optional<ErrorDTO> validate_retained_projection(
    const core::ProjectionLeaseBindingDTO &binding,
    const core::CollisionWorldSnapshotPtr &world,
    const std::shared_ptr<const core::CollisionPolicySnapshot> &policy) {
  if (!binding.valid() || !world || !policy ||
      world->revision.revision_id == 0U ||
      !core::IsExactLowercaseSha256Digest(world->revision.content_hash) ||
      policy->revision.revision_id == 0U ||
      !core::IsExactLowercaseSha256Digest(policy->revision.content_hash)) {
    return error(ErrorCode::kInvalidRequest,
                 "RoboPlan requires one exact retained world and policy",
                 "native_planner_projection_inventory_invalid");
  }
  return std::nullopt;
}

template <typename Value>
[[nodiscard]] bool unique_nonempty(const std::vector<Value> &values) {
  return !values.empty() &&
         std::set<Value>(values.begin(), values.end()).size() == values.size();
}

[[nodiscard]] bool
unique_nonempty_strings(const std::vector<std::string> &values) {
  return !values.empty() &&
         std::all_of(values.begin(), values.end(),
                     [](const std::string &value) { return !value.empty(); }) &&
         std::set<std::string>(values.begin(), values.end()).size() ==
             values.size();
}

[[nodiscard]] bool
frame_matches_binding(const core::ResolvedFrameDTO &frame,
                      const RoboPlanCandidateBindingDTO &binding) {
  if (core::compiled_frame_name(frame).empty()) {
    return false;
  }
  if (const auto *component =
          std::get_if<core::ResolvedComponentModelFrameDTO>(&frame)) {
    return component->controllable_component_id ==
               binding.controllable_component_id &&
           component->model_instance_id == binding.model_instance_id &&
           component->model_revision == binding.model_revision &&
           !component->frame_role_id.empty();
  }
  if (const auto *passive =
          std::get_if<core::ResolvedPassiveModelFrameDTO>(&frame)) {
    return passive->model_instance_id == binding.model_instance_id &&
           passive->model_revision == binding.model_revision &&
           !passive->frame_role_id.empty();
  }
  return !std::get<core::ResolvedCellFrameDTO>(frame).cell_id.empty();
}

[[nodiscard]] bool contains_frame(const RoboPlanCandidateBindingDTO &binding,
                                  const core::ResolvedFrameDTO &frame) {
  return std::count(binding.permitted_frames.begin(),
                    binding.permitted_frames.end(), frame) == 1;
}

[[nodiscard]] bool
frame_inventory_valid(const RoboPlanCandidateBindingDTO &binding) {
  if (binding.permitted_frames.empty() ||
      binding.permitted_frames.size() > 4096U) {
    return false;
  }
  return std::all_of(binding.permitted_frames.begin(),
                     binding.permitted_frames.end(), [&](const auto &frame) {
                       return frame_matches_binding(frame, binding) &&
                              contains_frame(binding, frame);
                     });
}

[[nodiscard]] bool
binding_shape_valid(const RoboPlanCandidateBindingDTO &binding) {
  return !binding.controllable_component_id.empty() &&
         !binding.command_target_id.empty() &&
         !binding.model_instance_id.empty() &&
         !binding.model_revision.empty() &&
         !binding.compiled_group_name.empty() &&
         unique_nonempty(binding.resources) &&
         unique_nonempty_strings(binding.compiled_joint_names) &&
         binding.resources.size() == binding.compiled_joint_names.size() &&
         frame_inventory_valid(binding);
}

[[nodiscard]] bool contains_exact_model_binding(
    const std::vector<core::PlannerModelBindingDTO> &models,
    const RoboPlanCandidateBindingDTO &binding) {
  return std::count(models.begin(), models.end(),
                    core::PlannerModelBindingDTO{binding.model_instance_id,
                                                 binding.model_revision}) == 1;
}

[[nodiscard]] bool runtime_units_valid(
    const std::vector<core::PlannerRuntimeUnitBindingDTO> &units) {
  std::set<std::pair<core::RuntimeUnitId, core::RuntimeEpoch>> identities;
  for (const auto &unit : units) {
    if (unit.runtime_unit_id.empty() || unit.runtime_epoch.empty() ||
        !identities.emplace(unit.runtime_unit_id, unit.runtime_epoch).second) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool state_matches_request(const core::PlanRequestDTO &request) {
  const auto &state = request.start_state;
  return state.controllable_component_id == request.controllable_component_id &&
         state.command_target_id == request.command_target_id &&
         state.control_domain_id == request.control_domain_id &&
         state.control_domain_revision == request.control_domain_revision &&
         state.control_binding_id == request.control_binding_id &&
         state.control_binding_revision == request.control_binding_revision &&
         state.controller_topology_id == request.controller_topology_id &&
         state.topology_revision == request.topology_revision &&
         state.control_route_id == request.control_route_id &&
         state.active_control_configuration_id ==
             request.active_control_configuration_id &&
         state.active_control_configuration_generation ==
             request.active_control_configuration_generation &&
         state.resources == request.resources &&
         state.joint_names == request.model_group_binding.compiled_joint_names;
}

[[nodiscard]] std::optional<ErrorDTO>
validate_plan_binding(const core::PlanRequestDTO &request,
                      const core::ProjectionLeaseBindingDTO &projection,
                      const RoboPlanCandidateBindingDTO &binding) {
  const auto &requested_revision =
      request.projection_binding.projection_revision;
  const auto &anchored_revision = projection.projection_revision;
  if (!request.projection_binding.valid() ||
      requested_revision.retained_bundle_id !=
          anchored_revision.retained_bundle_id ||
      requested_revision.graph_revision != anchored_revision.graph_revision ||
      requested_revision.scene_revision.scene_id !=
          anchored_revision.scene_revision.scene_id) {
    return error(ErrorCode::kInvalidRequest,
                 "Planning request crosses the retained projection lineage",
                 "native_planner_projection_binding_mismatch");
  }
  if (request.operation_id.empty() || request.participant_id.empty() ||
      request.program_id.empty() || request.program_revision.empty() ||
      request.program_binding_id.empty() || request.program_run_id.empty() ||
      request.move_id.empty() || request.component_type_id.empty() ||
      request.component_type_revision.empty() ||
      request.capability_id.empty() ||
      request.capability_definition_revision.empty() ||
      request.control_domain_id.empty() ||
      request.control_domain_revision.empty() ||
      request.control_binding_id.empty() ||
      request.control_binding_revision.empty() ||
      request.control_interface_contract_id.empty() ||
      request.control_interface_revision.empty() ||
      request.controller_topology_id.empty() ||
      request.topology_revision.empty() || request.control_route_id.empty() ||
      request.activation_set_id.empty() ||
      request.active_control_configuration_id.empty() ||
      !request.active_control_configuration_generation.nonzero() ||
      request.provider_instance_id.empty() ||
      request.provider_manifest_id.empty() ||
      request.provider_manifest_revision.empty() ||
      request.provider_runtime_epoch.empty() ||
      !runtime_units_valid(request.runtime_unit_bindings)) {
    return error(
        ErrorCode::kInvalidRequest,
        "Planning request lacks complete owner-issued runtime identity",
        "native_planner_runtime_identity_incomplete");
  }
  if (request.controllable_component_id != binding.controllable_component_id ||
      request.command_target_id != binding.command_target_id ||
      request.model_group_binding.model_instance_id !=
          binding.model_instance_id ||
      request.model_group_binding.model_revision != binding.model_revision ||
      request.model_group_binding.model_group_role_id.empty() ||
      request.model_group_binding.compiled_group_name !=
          binding.compiled_group_name ||
      request.model_group_binding.compiled_joint_names !=
          binding.compiled_joint_names ||
      !contains_exact_model_binding(request.model_bindings, binding) ||
      request.resources != binding.resources ||
      !contains_frame(binding, request.base_frame) ||
      !contains_frame(binding, request.tcp_frame)) {
    return error(ErrorCode::kInvalidRequest,
                 "Planning request crosses its exact participant binding",
                 "native_planner_participant_binding_mismatch");
  }
  const auto state = core::validate_controlled_state(request.start_state);
  if (!state.ok() || !state_matches_request(request)) {
    return error(ErrorCode::kStartStateMismatch,
                 "Planning start state crosses its active control binding",
                 "native_planner_start_state_binding_mismatch");
  }
  return std::nullopt;
}

[[nodiscard]] bool ik_state_matches_request(const core::IkRequestDTO &request) {
  const auto &state = request.seed_state;
  return state.controllable_component_id == request.controllable_component_id &&
         state.command_target_id == request.command_target_id &&
         state.resources == request.resources;
}

[[nodiscard]] std::optional<ErrorDTO>
validate_ik_binding(const core::IkRequestDTO &request,
                    const core::ProjectionLeaseBindingDTO &projection,
                    const RoboPlanCandidateBindingDTO &binding) {
  const auto &requested_revision =
      request.projection_binding.projection_revision;
  const auto &anchored_revision = projection.projection_revision;
  if (!request.projection_binding.valid() ||
      requested_revision.retained_bundle_id !=
          anchored_revision.retained_bundle_id ||
      requested_revision.graph_revision != anchored_revision.graph_revision ||
      requested_revision.scene_revision.scene_id !=
          anchored_revision.scene_revision.scene_id) {
    return error(ErrorCode::kInvalidRequest,
                 "IK request crosses the retained projection lineage",
                 "native_ik_projection_binding_mismatch");
  }
  if (request.controllable_component_id != binding.controllable_component_id ||
      request.command_target_id != binding.command_target_id ||
      !contains_exact_model_binding(request.model_bindings, binding) ||
      request.resources != binding.resources ||
      !contains_frame(binding, request.base_frame) ||
      !contains_frame(binding, request.tcp_frame) ||
      request.seed_state.joint_names != binding.compiled_joint_names) {
    return error(ErrorCode::kInvalidRequest,
                 "IK request crosses its exact participant binding",
                 "native_ik_participant_binding_mismatch");
  }
  const auto state = core::validate_controlled_state(request.seed_state);
  if (!state.ok() || !ik_state_matches_request(request)) {
    return error(ErrorCode::kStartStateMismatch,
                 "IK seed crosses its exact participant binding",
                 "native_ik_seed_binding_mismatch");
  }
  if (!std::isfinite(request.timeout_s)) {
    return error(ErrorCode::kInvalidRequest, "IK timeout must be finite",
                 "native_ik_timeout_invalid");
  }
  return std::nullopt;
}

[[nodiscard]] bool
trajectory_shape_valid(const core::TrajectoryDTO &trajectory,
                       const RoboPlanCandidateBindingDTO &binding) {
  if (trajectory.resources != binding.resources ||
      trajectory.joint_names != binding.compiled_joint_names ||
      trajectory.waypoints.size() < 2U) {
    return false;
  }
  double previous = -1.0;
  for (const auto &waypoint : trajectory.waypoints) {
    const auto dimension = binding.resources.size();
    if (!std::isfinite(waypoint.time_from_start_s) ||
        waypoint.time_from_start_s < 0.0 ||
        waypoint.time_from_start_s <= previous ||
        waypoint.positions_rad.size() != dimension ||
        (!waypoint.velocities_rad_s.empty() &&
         waypoint.velocities_rad_s.size() != dimension) ||
        (!waypoint.accelerations_rad_s2.empty() &&
         waypoint.accelerations_rad_s2.size() != dimension) ||
        !std::all_of(waypoint.positions_rad.begin(),
                     waypoint.positions_rad.end(),
                     [](double value) { return std::isfinite(value); }) ||
        !std::all_of(waypoint.velocities_rad_s.begin(),
                     waypoint.velocities_rad_s.end(),
                     [](double value) { return std::isfinite(value); }) ||
        !std::all_of(waypoint.accelerations_rad_s2.begin(),
                     waypoint.accelerations_rad_s2.end(),
                     [](double value) { return std::isfinite(value); })) {
      return false;
    }
    previous = waypoint.time_from_start_s;
  }
  return trajectory.waypoints.front().time_from_start_s == 0.0;
}

} // namespace

RoboPlanPlannerBackend::RoboPlanPlannerBackend(
    core::ProjectionLeaseBindingDTO projection_binding,
    RoboPlanCandidateBindingDTO candidate_binding,
    std::unique_ptr<IRoboPlanCandidateClient> candidate_client,
    RoboPlanPlannerBackendConfig config)
    : projection_binding_(std::move(projection_binding)),
      candidate_binding_(std::move(candidate_binding)),
      candidate_client_(std::move(candidate_client)), config_(config) {
  if (!projection_binding_.valid() ||
      !binding_shape_valid(candidate_binding_) || !candidate_client_ ||
      candidate_client_->binding() != candidate_binding_ ||
      !std::isfinite(config_.default_single_shot_ik_timeout_s) ||
      config_.default_single_shot_ik_timeout_s <= 0.0) {
    throw std::invalid_argument(
        "RoboPlan addressed planner construction is inconsistent");
  }
}

core::Result<void> RoboPlanPlannerBackend::RebindProjection(
    const core::ProjectionLeaseBindingDTO& expected_predecessor,
    core::ProjectionLeaseBindingDTO candidate) {
  if (!expected_predecessor.valid() || !candidate.valid()) {
    return core::Result<void>::Err(error(
        ErrorCode::kInvalidRequest, "planner product rebind has an invalid lease",
        "native_planner_product_rebind_invalid"));
  }
  std::unique_lock lock(projection_mutex_);
  if (projection_binding_ != expected_predecessor) {
    return core::Result<void>::Err(error(
        ErrorCode::kInvalidStateTransition,
        "planner product rebind crossed its exact predecessor",
        "native_planner_product_rebind_stale_predecessor"));
  }
  // The candidate client selects the exact retained Scene lease per request.
  // Re-anchor the unchanged worker without replacing its engine or model.
  projection_binding_ = std::move(candidate);
  return core::Result<void>::Ok();
}

core::Result<void> IRoboPlanCandidateClient::replace_retained_model(
    const RoboPlanWorkerModelDTO &) {
  return core::Result<void>::Err(error(
      ErrorCode::kInvalidStateTransition,
      "Fixed RoboPlan candidate client cannot replace its planning model",
      "native_planner_model_replacement_unavailable"));
}

core::Result<void>
RoboPlanPlannerBackend::ReplaceRetainedModel(const RoboPlanWorkerModelDTO &model) {
  std::unique_lock lock(projection_mutex_);
  return candidate_client_->replace_retained_model(model);
}

core::PlanResultDTO
RoboPlanPlannerBackend::plan(const core::PlanRequestDTO &request) {
  std::shared_lock projection_lock(projection_mutex_);
  if (auto invalid = validate_plan_binding(request, projection_binding_,
                                           candidate_binding_);
      invalid.has_value()) {
    return plan_error(std::move(*invalid));
  }
  if (candidate_client_->requires_retained_projection()) {
    if (auto invalid = validate_retained_projection(
            request.projection_binding, request.retained_collision_world,
            request.retained_collision_policy);
        invalid.has_value()) {
      return plan_error(std::move(*invalid));
    }
    auto selected = candidate_client_->select_retained_projection(
        RoboPlanCandidateProjectionDTO{request.projection_binding,
                                       request.retained_collision_world,
                                       request.retained_collision_policy});
    if (!selected.ok()) {
      return plan_error(selected.error());
    }
  }

  RoboPlanCandidateRequestDTO candidate{request.controllable_component_id,
                                        request.command_target_id,
                                        candidate_binding_.model_instance_id,
                                        candidate_binding_.model_revision,
                                        request.resources,
                                        request.base_frame,
                                        request.tcp_frame,
                                        request.start_state,
                                        request.motion,
                                        request.tolerances,
                                        request.speed_profile,
                                        request.planning_profile,
                                        request.per_resource_dynamics_limits,
                                        request.planning_timeout_s,
                                        request.planning_attempt_index,
                                        request.action_span};
  try {
    auto result = candidate_client_->compute(candidate);
    if (!result.ok()) {
      return plan_error(result.error());
    }
    if (!trajectory_shape_valid(result.value(), candidate_binding_)) {
      return plan_error(
          error(ErrorCode::kInvariantViolation,
                "RoboPlan candidate returned a malformed portable trajectory",
                "native_planner_result_invalid"));
    }
    return {std::move(result.value()), std::nullopt,
            request.projection_binding.projection_revision};
  } catch (const std::exception &exception) {
    return plan_error(error(ErrorCode::kPlanningFailed,
                            "RoboPlan candidate transport failed: " +
                                std::string(exception.what()),
                            "native_planner_transport_failure"));
  } catch (...) {
    return plan_error(error(ErrorCode::kPlanningFailed,
                            "RoboPlan candidate transport failed",
                            "native_planner_transport_failure"));
  }
}

void RoboPlanPlannerBackend::request_cancel() {
  candidate_client_->request_cancel();
}

core::IkResultDTO
RoboPlanPlannerBackend::solve_ik(const core::IkRequestDTO &request) {
  std::shared_lock projection_lock(projection_mutex_);
  if (auto invalid =
          validate_ik_binding(request, projection_binding_, candidate_binding_);
      invalid.has_value()) {
    return ik_error(std::move(*invalid));
  }
  if (candidate_client_->requires_retained_projection()) {
    if (auto invalid = validate_retained_projection(
            request.projection_binding, request.retained_collision_world,
            request.retained_collision_policy);
        invalid.has_value()) {
      return ik_error(std::move(*invalid));
    }
    auto selected = candidate_client_->select_retained_projection(
        RoboPlanCandidateProjectionDTO{request.projection_binding,
                                       request.retained_collision_world,
                                       request.retained_collision_policy});
    if (!selected.ok()) {
      return ik_error(selected.error());
    }
  }
  const double timeout = request.timeout_s > 0.0
                             ? request.timeout_s
                             : config_.default_single_shot_ik_timeout_s;
  try {
    auto result = candidate_client_->solve_ik(
        request.goal_pose, request.base_frame, request.tcp_frame,
        request.seed_state, timeout);
    if (!result.ok()) {
      return ik_error(result.error());
    }
    if (result.value().size() != candidate_binding_.resources.size() ||
        !std::all_of(result.value().begin(), result.value().end(),
                     [](double value) { return std::isfinite(value); })) {
      return ik_error(
          error(ErrorCode::kInvariantViolation,
                "RoboPlan candidate returned a malformed IK solution",
                "native_ik_result_invalid"));
    }
    return {true, candidate_binding_.resources,
            candidate_binding_.compiled_joint_names, std::move(result.value()),
            std::nullopt};
  } catch (const std::exception &exception) {
    return ik_error(
        error(ErrorCode::kIkFailed,
              "RoboPlan IK transport failed: " + std::string(exception.what()),
              "native_ik_transport_failure"));
  } catch (...) {
    return ik_error(error(ErrorCode::kIkFailed, "RoboPlan IK transport failed",
                          "native_ik_transport_failure"));
  }
}

} // namespace manyforge::roboplan_backend
