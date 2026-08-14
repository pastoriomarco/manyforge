#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/cache/trajectory_cache_compatibility.hpp>
#include <manyforge_core/cache/trajectory_cache_intent.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/execution_admission_support.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/motion_target_resolution.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>
#include <manyforge_core/types/kernel_event_factories.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace manyforge::core {
namespace {

ErrorDTO stale_control_command_error() {
  return ErrorDTO{kErrorInvalidRequest,
                  "Control command was superseded",
                  {{"reason_code", "stale_control_command"}}};
}

void fail_closed_invalid_controlled_stop_result(const MotionInterruptStopProfileDTO& profile,
                                                ControlledStopResultDTO& result) {
  const auto validation = controlled_stop_result_field_error(profile, result);
  if (!validation.has_value()) {
    return;
  }
  result.accepted = false;
  result.backend_stop_confirmed = false;
  result.completed_by_natural_completion = false;
  result.error =
      ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
               "Execution backend returned an invalid controlled-stop result",
               {{"reason", "controlled_stop_result_invalid"}, {"validation_reason", *validation}}};
}

// Per-attempt state: keeps terminal acknowledgement behind the complete
// normal-completion event/outcome batch without holding a lock across event
// subscriber callbacks.
class CompletionPublicationGuard {
 public:
  CompletionPublicationGuard(std::atomic<std::uint64_t>& owner, std::uint64_t execution_id)
      : owner_(owner), execution_id_(execution_id) {
    std::uint64_t expected = 0;
    if (!owner_.compare_exchange_strong(expected, execution_id_, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      throw std::logic_error("overlapping normal-completion publication attempts");
    }
  }

  CompletionPublicationGuard(const CompletionPublicationGuard&) = delete;
  CompletionPublicationGuard& operator=(const CompletionPublicationGuard&) = delete;

  ~CompletionPublicationGuard() {
    std::uint64_t expected = execution_id_;
    if (!owner_.compare_exchange_strong(expected, 0, std::memory_order_release,
                                        std::memory_order_relaxed)) {
      std::terminate();
    }
  }

 private:
  std::atomic<std::uint64_t>& owner_;
  std::uint64_t execution_id_;
};

double clamp_unit_interval(double value) {
  return std::max(0.0, std::min(1.0, value));
}

void update_atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
  }
}

std::string diagnostic_double(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

Result<ControlledStateDTO> controlled_state_for_binding(const ExecuteRequestDTO& request,
                                                        const RobotStateDTO& observed) {
  const auto reject_state = [](std::string reason) {
    return Result<ControlledStateDTO>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Observed state does not match the resolved execution binding",
                 {{"reason", std::move(reason)}}});
  };
  if (observed.joint_names.empty() ||
      observed.joint_names.size() != observed.positions_rad.size() ||
      (!observed.velocities_rad_s.empty() &&
       observed.velocities_rad_s.size() != observed.joint_names.size())) {
    return reject_state("observed_angular_state_shape_mismatch");
  }
  const auto& expected_names = request.binding.model_group_binding.compiled_joint_names;
  if (request.binding.resources.empty() ||
      request.binding.resources.size() != expected_names.size()) {
    return reject_state("resolved_resource_projection_shape_mismatch");
  }

  std::unordered_map<std::string, std::size_t> observed_index;
  observed_index.reserve(observed.joint_names.size());
  for (std::size_t index = 0U; index < observed.joint_names.size(); ++index) {
    if (observed.joint_names[index].empty() ||
        !observed_index.emplace(observed.joint_names[index], index).second) {
      return reject_state("observed_angular_state_identity_invalid");
    }
  }

  std::vector<CoordinatePositionDTO> positions;
  positions.reserve(expected_names.size());
  std::optional<std::vector<CoordinateVelocityDTO>> velocities;
  if (!observed.velocities_rad_s.empty()) {
    velocities.emplace();
    velocities->reserve(expected_names.size());
  }
  for (const auto& name : expected_names) {
    const auto found = observed_index.find(name);
    if (found == observed_index.end()) {
      return reject_state("resolved_resource_missing_from_observed_state");
    }
    positions.emplace_back(AngularPositionDTO{observed.positions_rad[found->second]});
    if (velocities.has_value()) {
      velocities->emplace_back(AngularVelocityDTO{observed.velocities_rad_s[found->second]});
    }
  }

  ControlledStateDTO state{request.binding.controllable_component_id,
                           request.binding.command_target_id,
                           request.binding.control_domain_id,
                           request.binding.control_domain_revision,
                           request.binding.control_binding_id,
                           request.binding.control_binding_revision,
                           request.binding.controller_topology_id,
                           request.binding.topology_revision,
                           request.binding.control_route_id,
                           request.binding.active_control_configuration_id,
                           request.binding.active_control_configuration_generation,
                           request.binding.resources,
                           expected_names,
                           std::move(positions),
                           std::move(velocities)};
  if (auto validation = validate_controlled_state(state); !validation.ok()) {
    return Result<ControlledStateDTO>::Err(validation.error());
  }
  return Result<ControlledStateDTO>::Ok(std::move(state));
}

Result<ControlledStateDTO> controlled_state_for_publication(
    const ExecuteRequestDTO& request, const ObservedStatePublicationDTO& publication,
    const std::int64_t now_ns) {
  const auto reject_state = [](std::string reason) {
    return Result<ControlledStateDTO>::Err(
        ErrorDTO{kErrorStartStateMismatch,
                 "Addressed observed state does not match the resolved "
                 "execution binding",
                 {{"reason", std::move(reason)}}});
  };
  if (auto validation = validate_observed_state_publication(publication, now_ns);
      !validation.ok()) {
    return Result<ControlledStateDTO>::Err(validation.error());
  }
  const auto& binding = request.binding;
  const auto& snapshot = publication.snapshot;
  if (publication.controllable_component_id != binding.controllable_component_id ||
      publication.command_target_id != binding.command_target_id ||
      publication.control_domain_id != binding.control_domain_id ||
      publication.control_domain_revision != binding.control_domain_revision ||
      publication.control_binding_id != binding.control_binding_id ||
      publication.control_binding_revision != binding.control_binding_revision ||
      publication.controller_topology_id != binding.controller_topology_id ||
      publication.topology_revision != binding.topology_revision ||
      publication.control_route_id != binding.control_route_id ||
      publication.active_control_configuration_id != binding.active_control_configuration_id ||
      publication.active_control_configuration_generation !=
          binding.active_control_configuration_generation ||
      publication.provider_instance_id != binding.provider_instance_id ||
      publication.provider_manifest_id != binding.provider_manifest_id ||
      publication.provider_manifest_revision != binding.provider_manifest_revision ||
      publication.runtime_epoch != binding.provider_runtime_epoch) {
    return reject_state("addressed_observed_state_identity_mismatch");
  }
  if (snapshot.resources != binding.resources ||
      snapshot.joint_names != binding.model_group_binding.compiled_joint_names ||
      snapshot.positions.size() != snapshot.resources.size() ||
      (snapshot.velocities.has_value() &&
       snapshot.velocities->size() != snapshot.resources.size())) {
    return reject_state("addressed_observed_state_projection_mismatch");
  }
  ControlledStateDTO state{binding.controllable_component_id,
                           binding.command_target_id,
                           binding.control_domain_id,
                           binding.control_domain_revision,
                           binding.control_binding_id,
                           binding.control_binding_revision,
                           binding.controller_topology_id,
                           binding.topology_revision,
                           binding.control_route_id,
                           binding.active_control_configuration_id,
                           binding.active_control_configuration_generation,
                           binding.resources,
                           snapshot.joint_names,
                           snapshot.positions,
                           snapshot.velocities};
  if (auto validation = validate_controlled_state(state); !validation.ok()) {
    return Result<ControlledStateDTO>::Err(validation.error());
  }
  return Result<ControlledStateDTO>::Ok(std::move(state));
}

std::unordered_set<std::string> observed_target_object_ids(
    const ExecuteRequestDTO& request,
    const std::optional<CollisionPolicySnapshot>& policy_snapshot) {
  std::unordered_set<std::string> out;
  if (request.target_reference.has_value() &&
      request.target_reference->type == ReferenceDTO::Type::kSceneResource &&
      reference_identity_matches_type(*request.target_reference)) {
    out.insert(std::string(reference_identity_value(*request.target_reference)));
  }
  if (request.offset_reference.has_value() &&
      request.offset_reference->type == ReferenceDTO::Type::kSceneResource &&
      reference_identity_matches_type(*request.offset_reference)) {
    out.insert(std::string(reference_identity_value(*request.offset_reference)));
  }
  if (!request.contact_policy_id.empty() && policy_snapshot.has_value()) {
    for (const auto& grasp : policy_snapshot->contact_grasps) {
      if (grasp.policy_id == request.contact_policy_id && !grasp.selected_object_id.empty()) {
        out.insert(grasp.selected_object_id);
      }
    }
  }
  return out;
}

std::unordered_map<CollisionBodyId, std::string> body_hashes_by_id(
    const CollisionWorldSnapshotPtr& snapshot) {
  std::unordered_map<CollisionBodyId, std::string> out;
  if (!snapshot) {
    return out;
  }
  out.reserve(snapshot->bodies.size());
  for (const auto& body : snapshot->bodies) {
    out.emplace(body.body_id, body.body_content_hash.empty()
                                  ? compute_collision_world_body_content_hash(body)
                                  : body.body_content_hash);
  }
  return out;
}

[[nodiscard]] const CollisionWorldBody* find_collision_body(
    const CollisionWorldSnapshot& world, const CollisionBodyId& body_id) noexcept;

[[nodiscard]] std::string resolved_contact_grasp_policy_id(
    const ExecuteRequestDTO& request, const ProjectionRevisionDTO& projection,
    const ResolvedContactGraspBindingDTO& binding) {
  std::string material;
  const auto append = [&](std::string_view value) {
    material += std::to_string(value.size());
    material.push_back(':');
    material.append(value);
    material.push_back(';');
  };
  append(binding.contact_policy_id.value());
  append(binding.contact_policy_revision.value());
  append(request.binding.program_binding_id.value());
  append(request.binding.program_run_id.value());
  append(request.binding.operation_id.value());
  append(request.binding.participant_id.value());
  append(binding.runtime_object_id.value());
  append(binding.selected_object_id.value());
  append(projection.retained_bundle_id.value());
  append(projection.graph_revision.value());
  append(projection.scene_revision.content_hash.value());
  for (const auto& body_id : binding.allowed_robot_body_ids) {
    append(body_id);
  }
  const std::string digest = content_identity::sha256_digest(material);
  return "contact_grasp.bound." + digest.substr(std::string("sha256:").size());
}

[[nodiscard]] Result<void> install_resolved_contact_grasp_policy(
    ExecuteRequestDTO& request, CollisionWorldLeaseInventory& inventory) {
  if (!request.resolved_contact_grasp.has_value()) {
    return Result<void>::Ok();
  }
  const auto& binding = *request.resolved_contact_grasp;
  if (request.contact_policy_id != binding.contact_policy_id.value() ||
      binding.contact_policy_revision.empty() || binding.runtime_object_id.empty() ||
      binding.selected_object_id.empty() || binding.allowed_robot_body_ids.empty() ||
      !std::is_sorted(binding.allowed_robot_body_ids.begin(),
                      binding.allowed_robot_body_ids.end()) ||
      std::adjacent_find(binding.allowed_robot_body_ids.begin(),
                         binding.allowed_robot_body_ids.end()) !=
          binding.allowed_robot_body_ids.end() ||
      !std::isfinite(binding.max_penetration_m) || binding.max_penetration_m <= 0.0 ||
      !std::isfinite(binding.terminal_window_start_fraction) ||
      binding.terminal_window_start_fraction < 0.0 ||
      binding.terminal_window_start_fraction > 1.0 || !inventory.logical_scene ||
      !inventory.bundle.world || !inventory.bundle.policy.has_value()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidContactPolicy,
                                      "Resolved contact-grasp binding is incomplete",
                                      {{"reason", "resolved_contact_grasp_binding_invalid"}}});
  }
  const auto runtime_binding = std::lower_bound(
      request.runtime_object_bindings.begin(), request.runtime_object_bindings.end(),
      binding.runtime_object_id, [](const auto& candidate, const RuntimeObjectId& id) {
        return candidate.runtime_object_id < id;
      });
  if (runtime_binding == request.runtime_object_bindings.end() ||
      runtime_binding->runtime_object_id != binding.runtime_object_id ||
      runtime_binding->scene_resource_id != binding.selected_object_id) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidContactPolicy,
                                      "Resolved contact-grasp object differs from operation intent",
                                      {{"reason", "contact_grasp_runtime_object_mismatch"}}});
  }
  const auto object =
      std::lower_bound(inventory.logical_scene->objects.begin(),
                       inventory.logical_scene->objects.end(), binding.selected_object_id,
                       [](const SceneObjectSnapshotDTO& candidate, const SceneResourceId& id) {
                         return candidate.scene_resource_id < id;
                       });
  if (object == inventory.logical_scene->objects.end() ||
      object->scene_resource_id != binding.selected_object_id ||
      !std::holds_alternative<CellFixedSceneObjectPlacementDTO>(object->placement) ||
      object->collision_shapes.empty()) {
    return Result<void>::Err(
        ErrorDTO{kErrorInvalidContactPolicy,
                 "Contact-grasp selected object is missing, carried, or shapeless",
                 {{"reason", "contact_grasp_selected_object_not_detached"},
                  {"selected_object_id", binding.selected_object_id.value()}}});
  }
  for (std::size_t shape_index = 0U; shape_index < object->collision_shapes.size(); ++shape_index) {
    const auto* body =
        find_collision_body(*inventory.bundle.world,
                            make_scene_collision_body_id(binding.selected_object_id, shape_index));
    if (body == nullptr || body->owner_kind != CollisionOwnerKind::kSceneResource ||
        body->parent_body_id.has_value()) {
      return Result<void>::Err(
          ErrorDTO{kErrorInvalidContactPolicy,
                   "Contact-grasp selected object differs from the retained world",
                   {{"reason", "contact_grasp_selected_object_projection_mismatch"},
                    {"selected_object_id", binding.selected_object_id.value()}}});
    }
  }
  std::vector<std::string> allowed_body_ids;
  allowed_body_ids.reserve(binding.allowed_robot_body_ids.size());
  for (const auto& body_id : binding.allowed_robot_body_ids) {
    const auto* body = find_collision_body(*inventory.bundle.world, body_id);
    if (body == nullptr || body->owner_kind != CollisionOwnerKind::kComponentModelBody) {
      return Result<void>::Err(ErrorDTO{kErrorInvalidContactPolicy,
                                        "Contact-grasp robot body differs from the retained world",
                                        {{"reason", "contact_grasp_robot_body_projection_mismatch"},
                                         {"collision_body_id", body_id}}});
    }
    allowed_body_ids.push_back(body_id);
  }

  const std::string effective_id = resolved_contact_grasp_policy_id(
      request, inventory.projection_binding.projection_revision, binding);
  CollisionPolicySnapshot effective = *inventory.bundle.policy;
  effective.contact_grasps.clear();
  effective.contact_grasps.push_back(ContactGraspPolicyEntry{
      effective_id, binding.selected_object_id.value(), std::move(allowed_body_ids),
      binding.max_penetration_m, binding.terminal_window_start_fraction});
  effective.require_full_diagnostics = true;
  try {
    effective.revision.content_hash =
        logical_scene_identity::ComputeCollisionPolicyContentDigest(effective).value();
  } catch (const std::exception& error) {
    return Result<void>::Err(ErrorDTO{
        kErrorInvariantViolation,
        "Resolved contact-grasp policy could not be identified",
        {{"reason", "resolved_contact_grasp_identity_failed"}, {"exception", error.what()}}});
  }
  inventory.bundle.policy = std::move(effective);
  request.contact_policy_id = effective_id;
  return Result<void>::Ok();
}

[[nodiscard]] const CollisionWorldBody* find_collision_body(
    const CollisionWorldSnapshot& world, const CollisionBodyId& body_id) noexcept {
  const auto found =
      std::lower_bound(world.bodies.begin(), world.bodies.end(), body_id,
                       [](const CollisionWorldBody& candidate, const CollisionBodyId& selected) {
                         return candidate.body_id < selected;
                       });
  return found != world.bodies.end() && found->body_id == body_id ? &*found : nullptr;
}

[[nodiscard]] bool carried_detached_scene_pair(const CollisionWorldSnapshot& world,
                                               const CollisionPairId& pair) noexcept {
  const auto* first = find_collision_body(world, pair.body_a);
  const auto* second = find_collision_body(world, pair.body_b);
  if (first == nullptr || second == nullptr ||
      first->owner_kind != CollisionOwnerKind::kSceneResource ||
      second->owner_kind != CollisionOwnerKind::kSceneResource) {
    return false;
  }
  return first->parent_body_id.has_value() != second->parent_body_id.has_value();
}

[[nodiscard]] std::vector<CollisionPairId> dynamic_carried_start_contact_pairs(
    const CollisionWorldSnapshot& world, const std::vector<CollisionPairId>& start_pairs,
    const CollisionPolicySnapshot& policy) {
  std::vector<CollisionPairId> discovered;
  for (const auto& pair : start_pairs) {
    if (!carried_detached_scene_pair(world, pair) ||
        std::any_of(policy.support_contacts.begin(), policy.support_contacts.end(),
                    [&](const SupportContactPolicyEntry& entry) { return entry.pair == pair; })) {
      continue;
    }
    discovered.push_back(pair);
  }
  std::sort(discovered.begin(), discovered.end());
  discovered.erase(std::unique(discovered.begin(), discovered.end()), discovered.end());
  return discovered;
}

[[nodiscard]] std::vector<CollisionPairId> dynamic_carried_start_contact_pairs(
    const CollisionWorldSnapshot& world, const CollisionValidationDecisionBundle& validation,
    const CollisionPolicySnapshot& policy) {
  const auto first_waypoint = std::find_if(
      validation.waypoint_records.begin(), validation.waypoint_records.end(),
      [](const CollisionValidationWaypointRecord& record) { return record.waypoint_index == 0; });
  return first_waypoint == validation.waypoint_records.end()
             ? std::vector<CollisionPairId>{}
             : dynamic_carried_start_contact_pairs(world, first_waypoint->result.normalized_pairs,
                                                   policy);
}

[[nodiscard]] Result<CollisionPolicySnapshot> with_dynamic_carried_start_contacts(
    const CollisionPolicySnapshot& base, const std::vector<CollisionPairId>& discovered) {
  if (discovered.empty()) {
    return Result<CollisionPolicySnapshot>::Ok(base);
  }
  CollisionPolicySnapshot overlay = base;
  if (overlay.support_contacts.size() + discovered.size() >
      RuntimeCollisionQuerySession::kMaxSupportContacts) {
    return Result<CollisionPolicySnapshot>::Err(
        ErrorDTO{kErrorValidationCollision,
                 "Dynamic carried-object start contacts exceed the runtime bound",
                 {{"reason", "dynamic_start_contact_capacity_exceeded"},
                  {"configured_count", std::to_string(overlay.support_contacts.size())},
                  {"discovered_count", std::to_string(discovered.size())}}});
  }
  for (const auto& pair : discovered) {
    // The defaults intentionally preserve the historical strict release
    // semantics: touching at start may release monotonically, penetration is
    // rejected unless an explicit authored policy grants a bounded envelope,
    // and recontact/final contact remain forbidden.
    SupportContactPolicyEntry entry;
    entry.pair = pair;
    overlay.support_contacts.push_back(std::move(entry));
  }
  std::sort(overlay.support_contacts.begin(), overlay.support_contacts.end(),
            [](const SupportContactPolicyEntry& left, const SupportContactPolicyEntry& right) {
              return left.pair < right.pair;
            });
  overlay.require_full_diagnostics = true;
  try {
    overlay.revision.content_hash =
        logical_scene_identity::ComputeCollisionPolicyContentDigest(overlay).value();
  } catch (const std::exception& error) {
    return Result<CollisionPolicySnapshot>::Err(ErrorDTO{
        kErrorInvariantViolation,
        "Dynamic carried-object contact policy could not be identified",
        {{"reason", "dynamic_start_contact_policy_identity_failed"}, {"exception", error.what()}}});
  }
  return Result<CollisionPolicySnapshot>::Ok(std::move(overlay));
}

[[nodiscard]] ErrorDTO dynamic_carried_start_contact_query_error(const CollisionResult& result) {
  std::map<std::string, std::string> details{
      {"reason", "dynamic_start_contact_query_inconclusive"},
      {"engine_status", runtime_collision_engine_status_name(result.engine_status)}};
  std::string message =
      "Dynamic carried-object start-contact discovery was inconclusive "
      "(engine_status=" +
      std::string(runtime_collision_engine_status_name(result.engine_status)) + ")";
  std::optional<std::pair<std::string, std::string>> displayed_diagnostic;
  std::size_t diagnostic_index = 0U;
  for (const auto& [key, value] : result.raw_diagnostics) {
    if (diagnostic_index >= 8U) {
      details["engine_diagnostics_truncated"] = "true";
      break;
    }
    const auto suffix = std::to_string(diagnostic_index++);
    details["engine_diagnostic_" + suffix + "_key"] = key.substr(0U, 128U);
    details["engine_diagnostic_" + suffix + "_value"] = value.substr(0U, 512U);
    if (!displayed_diagnostic.has_value() && key != "snapshot_sync_duration_ns" &&
        key != "total_call_duration_ns") {
      displayed_diagnostic = std::pair{key.substr(0U, 128U), value.substr(0U, 512U)};
    }
  }
  if (!displayed_diagnostic.has_value() && !result.raw_diagnostics.empty()) {
    displayed_diagnostic = std::pair{result.raw_diagnostics.begin()->first.substr(0U, 128U),
                                     result.raw_diagnostics.begin()->second.substr(0U, 512U)};
  }
  if (displayed_diagnostic.has_value()) {
    message += ": " + displayed_diagnostic->first + "=" + displayed_diagnostic->second;
  }
  return ErrorDTO{kErrorValidationCollision, std::move(message), std::move(details)};
}

void enrich_live_objects_with_body_hashes(std::vector<SceneSnapshotLiveObject>& live_objects,
                                          const CollisionWorldSnapshotPtr& snapshot) {
  const auto body_hashes = body_hashes_by_id(snapshot);
  for (auto& object : live_objects) {
    for (const auto& body_id : object.collision_body_ids) {
      const auto it = body_hashes.find(body_id);
      if (it != body_hashes.end()) {
        object.body_content_hashes[body_id] = it->second;
      }
    }
  }
}

const char* collision_thread_safety_to_string(CollisionEngineThreadSafetyClass thread_safety) {
  switch (thread_safety) {
    case CollisionEngineThreadSafetyClass::kSingleContextExclusive:
      return "single_context_exclusive";
    case CollisionEngineThreadSafetyClass::kImmutableModelWorkerData:
      return "immutable_model_worker_data";
    case CollisionEngineThreadSafetyClass::kProcessIsolated:
      return "process_isolated";
  }
  return "unknown";
}

ErrorDTO validation_deferred_runtime_monitor_active_error(
    ExecutionState state, const CollisionPipelineResources& resources,
    const CollisionValidationAdmission& admission,
    const RuntimeCollisionProtectionSnapshot& protection) {
  std::map<std::string, std::string> details{
      {"reason", "validation_deferred_runtime_monitor_active"},
      {"reason_code", "validation_deferred_runtime_monitor_active"},
      {"kernel_state", to_string(state)},
      {"resource_policy", "role_owned_collision_resources"},
      {"admission_status", "runtime_monitor_lease_active_shared_resource"},
      {"resource_conflict_reason", admission.resource_conflict_reason},
      {"runtime_protection_generation", std::to_string(protection.generation)},
      {"runtime_protection_execution_id", std::to_string(protection.execution_id)},
      {"runtime_protection_state_at_acquire", to_string(protection.state_at_acquire)},
      {"runtime_protection_reason", protection.reason},
      {"runtime_monitor_resource", resources.runtime_monitor_engine ? "configured" : "missing"},
      {"validation_engine_count", std::to_string(resources.validation_engines.size())},
      {"operator_guidance",
       "retry validation after active motion/stop completes or configure an "
       "independent validation collision resource"}};
  if (resources.runtime_monitor_engine) {
    const auto& capability = resources.runtime_monitor_engine->capability();
    details["runtime_engine_id"] = capability.engine_id;
    details["runtime_engine_thread_safety"] =
        collision_thread_safety_to_string(capability.thread_safety);
  }
  return ErrorDTO{kErrorValidationDeferred,
                  "Collision validation deferred because runtime collision "
                  "monitoring owns the collision resource",
                  std::move(details)};
}

ErrorDTO validation_admission_error(ExecutionState state,
                                    const CollisionPipelineResources& resources,
                                    const CollisionValidationAdmission& admission,
                                    const RuntimeCollisionProtectionSnapshot& protection) {
  switch (admission.status) {
    case CollisionValidationAdmissionStatus::kRuntimeMonitorLeaseActiveSharedResource:
      return validation_deferred_runtime_monitor_active_error(state, resources, admission,
                                                              protection);
    case CollisionValidationAdmissionStatus::kMissingSnapshotPublisher:
      return authoritative_collision_validation_config_error(
          "missing_collision_world_snapshot_publisher");
    case CollisionValidationAdmissionStatus::kMissingRuntimeResource:
      return authoritative_collision_validation_config_error(
          "missing_runtime_collision_query_engine",
          {{"runtime_protection_active", protection.active ? "true" : "false"},
           {"runtime_protection_generation", std::to_string(protection.generation)}});
    case CollisionValidationAdmissionStatus::kMissingValidationResource:
      return authoritative_collision_validation_config_error("missing_collision_query_engines");
    case CollisionValidationAdmissionStatus::kRuntimeQueryCapabilityUnsupported:
      return authoritative_collision_validation_config_error(
          "runtime_collision_query_capability_unsupported",
          {{"resource_conflict_reason", admission.resource_conflict_reason}});
    case CollisionValidationAdmissionStatus::kRuntimeLookaheadResourceConflict:
      return authoritative_collision_validation_config_error(
          "runtime_collision_lookahead_resource_conflict",
          {{"resource_conflict_reason", admission.resource_conflict_reason}});
    case CollisionValidationAdmissionStatus::kAdmitted:
      break;
  }
  return authoritative_collision_validation_config_error(
      "unknown_collision_validation_admission_failure");
}

bool runtime_lookahead_requires_authority(const RuntimeCollisionLookaheadResult& result) noexcept {
  return result.status == RuntimeCollisionLookaheadResultStatus::kCollision ||
         result.status == RuntimeCollisionLookaheadResultStatus::kInconclusive;
}

std::map<std::string, std::string> runtime_lookahead_authority_details(
    const RuntimeCollisionLookaheadResult& result) {
  std::map<std::string, std::string> details{
      {"surface", "observed_runtime_lookahead"},
      {"reason_code", "runtime_collision_lookahead"},
      {"reason", result.reason},
      {"execution_generation", std::to_string(result.execution_generation)},
      {"runtime_bundle_epoch", std::to_string(result.runtime_bundle_epoch)},
      {"collision_world_revision_id", std::to_string(result.collision_world_revision.revision_id)},
      {"collision_world_content_hash", result.collision_world_revision.content_hash},
      {"lookahead_cursor_time_ns", std::to_string(result.cursor_time_ns)},
      {"lookahead_covered_until_time_ns", std::to_string(result.covered_until_time_ns)},
      {"lookahead_checked_samples", std::to_string(result.checked_samples)},
      {"lookahead_termination", runtime_collision_lookahead_termination_name(result.termination)}};
  if (result.first_failing_waypoint) {
    details["collision_result_first_failure_waypoint"] =
        std::to_string(*result.first_failing_waypoint);
  }
  if (result.first_failing_time_ns) {
    details["collision_result_first_failure_time_ns"] =
        std::to_string(*result.first_failing_time_ns);
  }
  if (result.projection_revision.has_value()) {
    details["retained_bundle_id"] = result.projection_revision->retained_bundle_id.value();
    details["graph_revision"] = result.projection_revision->graph_revision.value();
    details["scene_id"] = result.projection_revision->scene_revision.scene_id.value();
    details["scene_revision_id"] =
        std::to_string(result.projection_revision->scene_revision.revision_id);
    details["scene_content_hash"] = result.projection_revision->scene_revision.content_hash.value();
  }
  return details;
}

[[nodiscard]] bool prepared_command_matches_private_request(
    const PreparedOperationParticipantDTO& participant, const RuntimeParticipantBindingDTO& binding,
    const GripperCommandRequestDTO& request) noexcept {
  const auto* command = std::get_if<PreparedCapabilityCommandDTO>(&participant.payload);
  if (command == nullptr || binding.resources.size() != 1U || !binding.control_path.has_value() ||
      participant.command_target_id != binding.command_target_id) {
    return false;
  }
  const auto& resource = binding.resources.front();
  const auto& value = std::get<GripperPositionCommandValueDTO>(command->command_value);
  const auto kind = coordinate_kind(value.position);
  const auto target = kind == CoordinateKind::kAngular
                          ? std::get<AngularPositionDTO>(value.position).value_rad
                          : std::get<LinearPositionDTO>(value.position).value_m;
  std::optional<double> effort;
  if (value.max_effort.has_value()) {
    effort = kind == CoordinateKind::kAngular
                 ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                 : std::get<LinearEffortDTO>(*value.max_effort).value_n;
  }
  double duration = 0.0;
  for (const auto& option : command->validated_options) {
    if (option.option_id.value() != "command_duration_s") {
      return false;
    }
    duration = option.value_s;
  }
  const bool model_backed = resource.source_joint_name.has_value();
  const auto expected_group = model_backed && binding.model_groups.size() == 1U
                                  ? binding.model_groups.front().compiled_group_name
                                  : std::string{};
  return request.gripper_id == binding.command_target_id.value() &&
         request.group_id == expected_group &&
         request.joint_name == resource.compiled_resource_name &&
         request.target_position == target && request.duration_s == duration &&
         request.max_effort == effort && request.commanded_state == "position" &&
         request.actuation_kind == (kind == CoordinateKind::kAngular ? "angular" : "linear") &&
         request.state_authority == binding.control_path->state_authority_id.value() &&
         request.completion_policy == "goal_fenced_target_reached" &&
         request.grasp_confirmation_policy == "binding_feedback_required" &&
         request.transport_kind == "resolved_binding" &&
         request.evidence == std::vector<std::string>{"command_terminal"} &&
         request.evidence_state == "required_pending" &&
         request.completion_timeout_s == command->completion_timeout_s;
}

}  // namespace

class ExecutionKernel::EventDeferralScope {
 public:
  explicit EventDeferralScope(ExecutionKernel& kernel)
      : kernel_(kernel), previous_(active_event_deferral_) {
    active_event_deferral_ = this;
  }

  EventDeferralScope(const EventDeferralScope&) = delete;
  EventDeferralScope& operator=(const EventDeferralScope&) = delete;

  ~EventDeferralScope() { flush(); }

  void flush() {
    if (flushed_) {
      return;
    }
    flushed_ = true;
    active_event_deferral_ = previous_;
    if (previous_ != nullptr) {
      previous_->append(std::move(events_));
      return;
    }
    for (auto& event : events_) {
      kernel_.dispatch_prepared_event(event);
    }
  }

  void defer(KernelEvent event) { events_.push_back(std::move(event)); }

 private:
  void append(std::vector<KernelEvent>&& events) {
    events_.reserve(events_.size() + events.size());
    for (auto& event : events) {
      events_.push_back(std::move(event));
    }
  }

  ExecutionKernel& kernel_;
  EventDeferralScope* previous_ = nullptr;
  std::vector<KernelEvent> events_;
  bool flushed_ = false;
};

thread_local ExecutionKernel::EventDeferralScope* ExecutionKernel::active_event_deferral_ = nullptr;

struct ExecutionKernel::PlanningSuccess final {
  PlanRequestDTO request;
  PlanResultDTO result;
};

struct ExecutionKernel::PreparedMotionSession final {
  explicit PreparedMotionSession(ResolvedPreparedMotionExecutionRequestDTO supplied)
      : authority(std::move(supplied)) {}

  ResolvedPreparedMotionExecutionRequestDTO authority;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::uint64_t execution_id{};
  ResolvedPreparedMotionExecutionStateDTO state{
      ResolvedPreparedMotionExecutionStateDTO::kPreparing};
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ErrorDTO> error;
  bool dispatch_requested{};
  bool cancel_requested{};
  bool resume_cycle{};
};

struct ExecutionKernel::PreparedCapabilityCommandSession final {
  explicit PreparedCapabilityCommandSession(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO supplied)
      : authority(std::move(supplied)) {}

  ResolvedPreparedCapabilityCommandExecutionRequestDTO authority;
  mutable std::mutex mutex;
  std::uint64_t preparation_id{};
  ResolvedPreparedCapabilityCommandStateDTO state{
      ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed};
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ControllerGoalId> controller_goal_id;
  std::optional<ControllerGoalGeneration> controller_goal_generation;
  std::optional<GripperCommandResultDTO> dispatch_result;
  std::optional<ErrorDTO> error;
};

ExecutionKernel::ExecutionKernel(IPlannerBackend& planner, ExecutionKernelPorts ports,
                                 IExecutionBackend& execution_backend, IClock& clock,
                                 IScheduler& scheduler, TrajectoryCache& cache,
                                 ExecutionKernelConfig config)
    : planner_(planner),
      observed_state_(ports.observed_state),
      kinematics_(ports.kinematics),
      reference_resolver_(ports.reference_resolver),
      trajectory_validator_(ports.trajectory_validator),
      collision_projector_(ports.collision_projector),
      collision_diagnostic_comparator_(ports.collision_diagnostic_comparator),
      gripper_command_backend_(ports.gripper_command_backend),
      execution_backend_(execution_backend),
      clock_(clock),
      scheduler_(scheduler),
      cache_(cache),
      config_(std::move(config)),
      sm_(clock),
      stop_policy_(
          sm_, scheduler, clock, [this]() { cache_.invalidate_all(); },
          [this](ExecutionState terminal_state) -> Result<void> {
            if (terminal_state == ExecutionState::kStopped) {
              // StopPolicy reaches this callback only after its backend stop
              // callback has returned successfully. This is the definitive
              // backstop for deferred admission/execute-worker races: those
              // paths may publish partial evidence, but only this authority
              // upgrades the same owner generation to backend-confirmed.
              publish_operator_stop_certificate(
                  sm_.status().execution_id, StopCertificateOutcome::kSucceeded,
                  /*backend_stop_confirmed=*/true, "stop_policy_terminal");
            }
            auto outcome = commit_terminal(terminal_state, "stop_policy_terminal",
                                           TerminalCommitter::Origin::kStopAuthority);
            return outcome.ok() ? Result<void>::Ok()
                                : Result<void>::Err(ErrorCode::kInvalidStateTransition);
          },
          [this](bool requested) {
            terminal_stop_authority_asserted_.store(requested, std::memory_order_release);
          }),
      terminal_committer_(
          sm_,
          /*stop_requested=*/
          [this] { return terminal_stop_authority_asserted_.load(std::memory_order_acquire); },
          /*fault_latch_active=*/
          [this] { return terminal_fault_authority_asserted_.load(std::memory_order_acquire); },
          /*trip_stop_active=*/
          [this] { return motion_trip_stop_.active(); }),
      runtime_collision_monitor_(
          clock_, scheduler_, RuntimeCollisionMonitor::Config{config_.collision_monitor_period_ns},
          [this]() { return collision_check_tick(); },
          [this]() {
            return sm_.status().state == ExecutionState::kExecuting ||
                   sm_.status().state == ExecutionState::kPaused || motion_trip_stop_.active() ||
                   operator_stop_worker_.active() || fault_preempt_stop_active();
          }) {
  runtime_collision_monitor_.set_fail_closed_dump_hook(
      [this]() { (void)flight_recorder_.dump_now("runtime_monitor_drain_timeout"); });
  if (config_.collision_monitor_period_ns <= 0) {
    throw std::invalid_argument("collision_monitor_period_ns must be > 0");
  }
  if (config_.validation_revision.empty()) {
    throw std::invalid_argument("validation_revision must be non-empty");
  }
  if (config_.runtime_collision_lookahead_cycle_budget_ns <= 0 ||
      config_.runtime_collision_lookahead_horizon_ns <= 0 ||
      config_.runtime_collision_lookahead_max_samples < 2 ||
      config_.runtime_collision_lookahead_max_samples > 4096 ||
      config_.runtime_collision_lookahead_workers < 1 ||
      config_.runtime_collision_lookahead_workers > 64 ||
      config_.runtime_collision_lookahead_min_waypoints_for_parallel < 1) {
    throw std::invalid_argument("runtime collision lookahead configuration is invalid");
  }
  if (config_.runtime_collision_immediate_query_budget_ns <= 0) {
    throw std::invalid_argument("runtime_collision_immediate_query_budget_ns must be > 0");
  }
  if (config_.runtime_collision_immediate_query_budget_ns > config_.collision_monitor_period_ns) {
    throw std::invalid_argument(
        "runtime_collision_immediate_query_budget_ns must be <= "
        "collision_monitor_period_ns");
  }
  if (config_.runtime_collision_lookahead_query_budget_ns <= 0 ||
      config_.runtime_collision_lookahead_query_budget_ns >
          config_.runtime_collision_lookahead_cycle_budget_ns) {
    throw std::invalid_argument(
        "runtime_collision_lookahead_query_budget_ns must be > 0 and <= "
        "runtime_collision_lookahead_cycle_budget_ns");
  }
  if (config_.validation_collision_query_timeout_ns <= 0) {
    throw std::invalid_argument("validation_collision_query_timeout_ns must be > 0");
  }
  if (config_.validation_collision_wall_timeout_ns <= 0) {
    throw std::invalid_argument("validation_collision_wall_timeout_ns must be > 0");
  }
  if (config_.scene_snapshot_lease_max_age_ns <= 0) {
    throw std::invalid_argument("scene_snapshot_lease_max_age_ns must be > 0");
  }
  if (config_.enable_collision_diagnostic_compare && collision_diagnostic_comparator_ == nullptr) {
    throw std::invalid_argument("collision diagnostic compare requires a comparator port");
  }
  if (config_.recovery_ready_dwell_ns < 0) {
    throw std::invalid_argument("recovery_ready_dwell_ns must be >= 0");
  }
  if (config_.recovery_ready_min_samples == 0) {
    throw std::invalid_argument("recovery_ready_min_samples must be >= 1");
  }
  if (!std::isfinite(config_.recovery_at_rest_velocity_threshold_rad_s) ||
      config_.recovery_at_rest_velocity_threshold_rad_s < 0.0) {
    throw std::invalid_argument(
        "recovery_at_rest_velocity_threshold_rad_s must be finite and >= 0");
  }
  if (config_.recovery_observation_poll_period_ns <= 0) {
    throw std::invalid_argument("recovery_observation_poll_period_ns must be > 0");
  }
  if (!std::isfinite(config_.scene_snapshot_default_observed_drift_tolerance_m) ||
      config_.scene_snapshot_default_observed_drift_tolerance_m < 0.0) {
    throw std::invalid_argument(
        "scene_snapshot_default_observed_drift_"
        "tolerance_m must be finite and >= 0");
  }
  if (!std::isfinite(config_.scene_snapshot_default_observed_angular_tolerance_rad) ||
      config_.scene_snapshot_default_observed_angular_tolerance_rad < 0.0) {
    throw std::invalid_argument(
        "scene_snapshot_default_observed_angular_"
        "tolerance_rad must be finite and >= 0");
  }
  if (config_.native_reset_completion_timeout_s <= 0.0 ||
      !std::isfinite(config_.native_reset_completion_timeout_s)) {
    throw std::invalid_argument("native_reset_completion_timeout_s must be finite and > 0");
  }
  if (config_.single_shot_ik_timeout_s <= 0.0 || !std::isfinite(config_.single_shot_ik_timeout_s)) {
    throw std::invalid_argument("single_shot_ik_timeout_s must be finite and > 0");
  }
  if (config_.planner_max_planning_time_s <= 0.0 ||
      !std::isfinite(config_.planner_max_planning_time_s)) {
    throw std::invalid_argument("planner_max_planning_time_s must be finite and > 0");
  }
  if (config_.authoritative_validation_collision_replan_attempts < 0) {
    config_.authoritative_validation_collision_replan_attempts = 0;
  }
  if (config_.validation_collision_workers < 1 || config_.validation_collision_workers > 64) {
    throw std::invalid_argument("validation_collision_workers must be in [1, 64]");
  }
  if (config_.validation_collision_min_waypoints_for_parallel < 1) {
    config_.validation_collision_min_waypoints_for_parallel = 1;
  }
  ParallelCollisionValidationWorkerPool::Config validation_pool_config;
  validation_pool_config.worker_count = config_.validation_collision_workers;
  validation_pool_config.min_waypoints_for_parallel =
      config_.validation_collision_min_waypoints_for_parallel;
  validation_pool_config.validation_wall_timeout =
      std::chrono::nanoseconds(config_.validation_collision_wall_timeout_ns);
  validation_pool_config.clock = &clock_;
  validation_pool_config.scheduler = &scheduler_;
  collision_validation_worker_pool_ =
      std::make_unique<ParallelCollisionValidationWorkerPool>(std::move(validation_pool_config));
  // The operator-stop profile is invoked by `ExecutionKernel::stop()` on the
  // dedicated operator-stop worker so controlled-stop failures can be inspected
  // and escalated. The stop-policy callback stays the legacy hard-stop path
  // used by system faults and explicit hard-stop escalation.
  stop_policy_.set_stop_callback([this]() -> Result<void> {
    auto stop_result = execution_backend_.stop();
    if (stop_result.accepted) {
      return Result<void>::Ok();
    }
    const auto state = sm_.status().state;
    const auto error =
        stop_result.error.value_or(ErrorDTO{kErrorStopFailed,
                                            "Execution backend refused hard stop",
                                            {{"reason", "backend_hard_stop_refused"}}});
    const bool backend_already_stopped =
        state == ExecutionState::kStopping &&
        execution_backend_.get_execution_snapshot().state == ExecutionState::kStopped;
    if (error.code == kErrorInvalidRequest &&
        (state == ExecutionState::kPlanning || state == ExecutionState::kValidating ||
         state == ExecutionState::kPaused || state == ExecutionState::kStopped ||
         state == ExecutionState::kIdle ||
         (state == ExecutionState::kStopping &&
          (operator_stop_worker_.active() || backend_already_stopped)))) {
      return Result<void>::Ok();
    }
    return Result<void>::Err(error);
  });
  // Spec/230 §6.1 pause chain: the Stopping → Paused transition fires
  // inside the scheduled lambda of stop_policy_.request_pause(). This
  // callback emits the terminal state_transition event so external
  // observers see the full pause boundary.
  stop_policy_.set_pause_completed_callback([this]() {
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                ExecutionState::kStopping,
                ExecutionState::kPaused,
                std::nullopt,
                {}});
  });
  install_builtin_interrupt_sources();
  // Seed a complete first-read DTO without emitting a lifecycle event before
  // subscribers can exist. Later authority changes use the normal revisioned
  // publication path.
  auto initial_assessment = build_recovery_assessment(capture_recovery_authority_locked(),
                                                      recovery_readiness_policy(), 0);
  {
    std::lock_guard<std::mutex> lock(recovery_assessment_mutex_);
    recovery_assessment_ = std::move(initial_assessment);
  }
}

ExecutionKernel::~ExecutionKernel() {
  // Retire the lower-priority lookahead workers while every kernel authority
  // they may call is still alive. Relying on member destruction order would
  // permit a final result callback into partially torn-down state.
  std::shared_ptr<RuntimeCollisionLookaheadSession> lookahead;
  {
    // The execute worker can finish concurrently and release collision
    // protection from stop_collision_monitoring(). Serialize the retirement
    // exchange with that release before joining the session's own worker.
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    lookahead = runtime_collision_lookahead_session_.exchange(nullptr, std::memory_order_acq_rel);
  }
  if (lookahead) {
    lookahead->disarm();
    lookahead.reset();
  }
  const auto drain_scheduler_or_terminate = [this](const char* phase) {
    constexpr auto kSchedulerDrainTimeout = std::chrono::seconds(1);
    if (!scheduler_.drain_all_for(kSchedulerDrainTimeout)) {
      // Fail closed: these scheduler callbacks can capture `this`. Once
      // teardown starts, continuing after a missed drain risks UAF, while
      // waiting forever can hang process shutdown.
      std::cerr
          << "manyforge_execution_kernel_scheduler_drain_timeout" << " phase=" << phase
          << " timeout_ms="
          << std::chrono::duration_cast<std::chrono::milliseconds>(kSchedulerDrainTimeout).count()
          << '\n';
      // Flush the flight ring before dying: the event history of the last
      // seconds is exactly the evidence a fail-closed teardown needs to be
      // diagnosable post-mortem.
      (void)flight_recorder_.dump_now("scheduler_drain_timeout");
      std::terminate();
    }
  };

  invalidate_gripper_authority(GripperInvalidationReason::kShutdown);
  stop_policy_.cancel_pending_callbacks();
  drain_scheduler_or_terminate("stop_policy_cancel");
  request_fault_preempt_controlled_stop();
  motion_trip_stop_.request_stop();
  // Scheduled collision callbacks capture `this`; cancel them before any
  // member teardown so destruction cannot race a monitor tick.
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown, std::nullopt,
                            /*drain_callbacks=*/true);
  drain_scheduler_or_terminate("collision_monitor_stop");

  fault_preempt_worker_.stop_and_join();
  operator_stop_worker_.stop_and_join();

  // Join before member destruction so worker callbacks cannot notify the
  // condition variable after it has been destroyed. This is intentionally
  // explicit instead of relying on std::jthread member teardown order.
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    motion_trip_stop_.request_stop();
    worker_signals_.abandon();
    worker_thread_.join();
  }

  // RT-8B freshness deadlines capture `this`. A pending deadline is not
  // covered by scheduler drain (drain only waits for already-dequeued work),
  // and either stop worker may have replaced the handle after the earlier
  // drains. Cancel after every kernel-owned worker is joined, then drain once
  // more so an already-dequeued callback cannot outlive the kernel.
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (recovery_freshness_deadline_handle_ != 0) {
      scheduler_.cancel(recovery_freshness_deadline_handle_);
      recovery_freshness_deadline_handle_ = 0;
      recovery_freshness_deadline_ns_ = 0;
    }
    if (recovery_observation_poll_handle_ != 0) {
      scheduler_.cancel(recovery_observation_poll_handle_);
      recovery_observation_poll_handle_ = 0;
    }
    std::lock_guard<std::mutex> refresh_lock(recovery_refresh_mutex_);
    if (recovery_refresh_handle_ != 0) {
      scheduler_.cancel(recovery_refresh_handle_);
      recovery_refresh_handle_ = 0;
      recovery_refresh_scheduled_ = false;
    }
  }
  drain_scheduler_or_terminate("recovery_freshness_deadline_cancel");
}

bool ExecutionKernel::operator_stop_worker_is_current(std::uint64_t generation,
                                                      std::stop_token stop_token) const {
  return operator_stop_worker_.is_current(generation, stop_token);
}

bool ExecutionKernel::cancel_operator_stop_worker(bool invalidate_generation) {
  return operator_stop_worker_.cancel(invalidate_generation);
}

void ExecutionKernel::join_finished_operator_stop_worker() {
  operator_stop_worker_.join_finished();
}

bool ExecutionKernel::wait_for_operator_stop_worker(std::chrono::steady_clock::duration timeout) {
  return operator_stop_worker_.wait_for(timeout);
}

bool ExecutionKernel::request_operator_stop_controlled_stop() {
  std::shared_ptr<std::stop_source> source;
  {
    std::lock_guard<std::mutex> lk(operator_stop_source_mutex_);
    source = operator_stop_source_;
  }
  return source && source->request_stop();
}

std::uint64_t ExecutionKernel::fault_preempt_stop_generation() const {
  std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
  return fault_preempt_stop_generation_;
}

bool ExecutionKernel::fault_preempt_stop_active() const {
  std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
  return static_cast<bool>(fault_preempt_stop_source_);
}

bool ExecutionKernel::signal_fault_preempt_stop_path_collision(
    std::map<std::string, std::string> details) {
  {
    std::lock_guard<std::mutex> lock(fault_preempt_stop_path_collision_mutex_);
    if (!fault_preempt_stop_path_collision_detected_.load(std::memory_order_acquire)) {
      fault_preempt_stop_path_collision_details_ = std::move(details);
      fault_preempt_stop_path_collision_detected_.store(true, std::memory_order_release);
    }
  }
  return request_fault_preempt_controlled_stop();
}

std::optional<std::map<std::string, std::string>>
ExecutionKernel::fault_preempt_stop_path_collision_details() const {
  if (!fault_preempt_stop_path_collision_detected_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(fault_preempt_stop_path_collision_mutex_);
  if (!fault_preempt_stop_path_collision_detected_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  return fault_preempt_stop_path_collision_details_;
}

void ExecutionKernel::clear_fault_preempt_stop_path_collision() {
  std::lock_guard<std::mutex> lock(fault_preempt_stop_path_collision_mutex_);
  fault_preempt_stop_path_collision_details_.clear();
  fault_preempt_stop_path_collision_detected_.store(false, std::memory_order_release);
}

void ExecutionKernel::set_fault_preempt_intervention_error(ErrorDTO error) {
  std::lock_guard<std::mutex> lock(fault_preempt_intervention_error_mutex_);
  fault_preempt_intervention_error_ = std::move(error);
}

std::optional<ErrorDTO> ExecutionKernel::fault_preempt_intervention_error_for_active_latch() const {
  std::lock_guard<std::mutex> lock(fault_preempt_intervention_error_mutex_);
  if (!fault_preempt_intervention_error_.has_value()) {
    return std::nullopt;
  }
  const auto active_latch = interrupt_manager_.active_latch();
  const auto latch_detail = fault_preempt_intervention_error_->details.find("latch_id");
  if (!active_latch || latch_detail == fault_preempt_intervention_error_->details.end() ||
      latch_detail->second != active_latch->latch_id) {
    return std::nullopt;
  }
  return fault_preempt_intervention_error_;
}

void ExecutionKernel::clear_fault_preempt_intervention_error() {
  std::lock_guard<std::mutex> lock(fault_preempt_intervention_error_mutex_);
  fault_preempt_intervention_error_.reset();
}

bool ExecutionKernel::signal_operator_stop_path_collision(
    std::map<std::string, std::string> details) {
  {
    std::lock_guard<std::mutex> lock(operator_stop_path_collision_mutex_);
    if (!operator_stop_path_collision_detected_.load(std::memory_order_acquire)) {
      operator_stop_path_collision_details_ = std::move(details);
      operator_stop_path_collision_detected_.store(true, std::memory_order_release);
    }
  }
  return request_operator_stop_controlled_stop();
}

std::optional<std::map<std::string, std::string>>
ExecutionKernel::operator_stop_path_collision_details() const {
  if (!operator_stop_path_collision_detected_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(operator_stop_path_collision_mutex_);
  if (!operator_stop_path_collision_detected_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  return operator_stop_path_collision_details_;
}

bool ExecutionKernel::start_operator_stop_worker(
    MotionInterruptStopProfileDTO profile, ExecutionState pre_completion_state,
    std::optional<std::uint64_t> runtime_collision_generation,
    std::uint64_t stop_owner_generation) {
  operator_stop_command_active_.store(true, std::memory_order_release);
  const bool started = operator_stop_worker_.start(
      [this, profile = std::move(profile), pre_completion_state, runtime_collision_generation,
       stop_owner_generation](std::uint64_t generation, std::stop_token stop_token) mutable {
        run_operator_stop_worker(generation, stop_owner_generation, std::move(profile),
                                 pre_completion_state, runtime_collision_generation, stop_token);
      },
      [this]() { try_refresh_recovery_assessment("operator_stop_worker_finished"); });
  if (!started) {
    operator_stop_command_active_.store(false, std::memory_order_release);
  }
  return started;
}

void ExecutionKernel::run_operator_stop_worker(
    std::uint64_t generation, std::uint64_t stop_owner_generation,
    MotionInterruptStopProfileDTO profile, ExecutionState pre_completion_state,
    std::optional<std::uint64_t> runtime_collision_generation, std::stop_token stop_token) {
  const auto clear_stop_command_owner = std::shared_ptr<void>(nullptr, [this](void*) {
    operator_stop_command_active_.store(false, std::memory_order_release);
  });
  begin_runtime_velocity_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void*) { end_runtime_velocity_scale_freeze(); });

  const std::int64_t effective_deadline_ns =
      effective_controlled_stop_deadline_ns(profile, config_.complete_move_on_signal_timeout_ns);
  {
    std::lock_guard<std::mutex> lock(operator_stop_path_collision_mutex_);
    operator_stop_path_collision_details_.clear();
    operator_stop_path_collision_detected_.store(false, std::memory_order_release);
  }
  auto hard_stop_floor_attempt = std::make_shared<HardStopFloorAttempt>();
  auto controlled_stop_source = std::make_shared<std::stop_source>();
  {
    std::lock_guard<std::mutex> lk(operator_stop_source_mutex_);
    operator_stop_source_ = controlled_stop_source;
  }
  const auto clear_operator_stop_source =
      std::shared_ptr<void>(nullptr, [this, controlled_stop_source](void*) {
        std::lock_guard<std::mutex> lk(operator_stop_source_mutex_);
        if (operator_stop_source_ == controlled_stop_source) {
          operator_stop_source_.reset();
        }
      });
  auto deadline_watchdog_fired = std::make_shared<std::atomic_bool>(false);
  auto stop_session_complete = std::make_shared<std::atomic_bool>(false);
  std::stop_callback worker_stop_notify(
      stop_token, [controlled_stop_source]() { controlled_stop_source->request_stop(); });
  ScheduleHandle stop_deadline_handle = 0;
  if (effective_deadline_ns > 0) {
    const auto requested_mode = std::string(stop_mode_string(profile.stop_mode));
    stop_deadline_handle = scheduler_.schedule(
        clock_.now() + effective_deadline_ns,
        [this, controlled_stop_source, hard_stop_floor_attempt, deadline_watchdog_fired,
         stop_session_complete, requested_mode]() {
          if (stop_session_complete->load(std::memory_order_acquire)) {
            return;
          }
          deadline_watchdog_fired->store(true, std::memory_order_release);
          const auto legacy =
              request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
          controlled_stop_source->request_stop();
          const auto fallback_outcome = legacy.accepted ? std::string("legacy_stop_succeeded")
                                                        : std::string("legacy_stop_failed");
          std::map<std::string, std::string> details{{"reason", "in_stop_completion_timeout"},
                                                     {"phase", "operator_stop_deadline_watchdog"},
                                                     {"requested_stop_mode", requested_mode},
                                                     {"emergency_fallback", fallback_outcome},
                                                     {"hard_stop_transitioned", "true"}};
          add_hard_stop_floor_details(details, execution_backend_);
          emit_event({KernelEventType::kErrorRaised,
                      sm_.status().execution_id,
                      clock_.now(),
                      {},
                      {},
                      ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                               "operator controlled stop exceeded "
                               "effective stop deadline; emergency hard-stop "
                               "floor issued by deadline watchdog",
                               details},
                      details});
        },
        SchedulerPriority::StopControl);
  }

  const auto stop_wall_start = std::chrono::steady_clock::now();
  const auto stop_wall_deadline =
      effective_deadline_ns > 0 ? stop_wall_start + std::chrono::nanoseconds(effective_deadline_ns)
                                : stop_wall_start;
  auto controlled_result =
      execution_backend_.execute_controlled_stop(profile, controlled_stop_source->get_token());
  fail_closed_invalid_controlled_stop_result(profile, controlled_result);
  stop_session_complete->store(true, std::memory_order_release);
  if (stop_deadline_handle != 0) {
    scheduler_.cancel(stop_deadline_handle);
  }
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  bool controlled_ok = controlled_result.accepted && controlled_result.backend_stop_confirmed &&
                       !controlled_result.error.has_value();
  bool confirmed_but_not_settled = false;
  bool settle_signal_observed = false;
  auto stop_path_collision_details = operator_stop_path_collision_details();
  std::string stop_failure_reason = "operator_stop_profile_failed";
  std::string stop_failure_message =
      "operator stop profile failed; emergency legacy backend.stop() "
      "fallback engaged";
  std::map<std::string, std::string> settle_details;

  if (controlled_ok && !stop_path_collision_details.has_value()) {
    const double threshold = kDefaultSettleVelocityThresholdRadS;
    const auto settle_result = wait_for_measured_rest_after_confirmed_stop(
        execution_backend_, threshold, stop_wall_deadline, stop_token, [this]() {
          return operator_stop_path_collision_detected_.load(std::memory_order_acquire);
        });
    settle_signal_observed = settle_result.settle_signal_available;
    stop_path_collision_details = operator_stop_path_collision_details();
    if (settle_result.aborted && stop_path_collision_details.has_value()) {
      controlled_ok = false;
    } else if (settle_result.settle_signal_available && !settle_result.at_rest) {
      controlled_ok = false;
      confirmed_but_not_settled = true;
      stop_failure_reason = "stop_not_settled_measured_velocity";
      stop_failure_message =
          "operator controlled stop confirmed but measured velocity did not "
          "settle within the effective stop deadline";
      controlled_result.error =
          ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                   stop_failure_message,
                   {{"reason", stop_failure_reason},
                    {"measured_max_abs_velocity_rad_s",
                     std::to_string(settle_result.max_abs_velocity_rad_s.value_or(0.0))},
                    {"settle_velocity_threshold_rad_s", std::to_string(threshold)}}};
      settle_details["measured_max_abs_velocity_rad_s"] =
          std::to_string(settle_result.max_abs_velocity_rad_s.value_or(0.0));
      settle_details["settle_velocity_threshold_rad_s"] = std::to_string(threshold);
      settle_details["settle_wait_timed_out"] = settle_result.timed_out ? "true" : "false";
    }
  }

  std::optional<StopResultDTO> fallback_legacy_result;
  if (deadline_watchdog_fired->load(std::memory_order_acquire)) {
    fallback_legacy_result = hard_stop_floor_result_if_attempted(hard_stop_floor_attempt);
    if (!fallback_legacy_result.has_value()) {
      fallback_legacy_result =
          request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
    }
    controlled_ok = false;
    stop_failure_reason = "in_stop_completion_timeout";
    stop_failure_message =
        "operator controlled stop exceeded the effective stop deadline; "
        "emergency hard-stop floor was issued by the deadline watchdog";
    controlled_result.error =
        ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                 stop_failure_message,
                 {{"reason", stop_failure_reason}, {"phase", "operator_stop_deadline_watchdog"}}};
  }
  if (stop_path_collision_details.has_value()) {
    controlled_ok = false;
    confirmed_but_not_settled = false;
    stop_failure_reason = "operator_stop_path_collision";
    stop_failure_message =
        "collision detected on the operator stop path during controlled stop; "
        "emergency hard-stop floor engaged";
    auto details = *stop_path_collision_details;
    details["reason"] = stop_failure_reason;
    details["phase"] = "operator_stop";
    controlled_result.error = ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                       stop_failure_message, std::move(details)};
  }
  if (!controlled_ok && !confirmed_but_not_settled) {
    fallback_legacy_result =
        request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
  }
  if (!controlled_ok) {
    const std::string requested_mode = stop_mode_string(profile.stop_mode);
    const std::string executed_mode = stop_mode_string(controlled_result.executed_stop_mode);
    const std::string fallback_outcome =
        confirmed_but_not_settled ? std::string("not_attempted_controlled_stop_already_confirmed")
                                  : (fallback_legacy_result && fallback_legacy_result->accepted
                                         ? std::string("legacy_stop_succeeded")
                                         : std::string("legacy_stop_failed"));
    // Propagate the underlying controlled-stop failure cause into the
    // escalation event. A downgraded operator stop MUST be self-describing on
    // the event stream: it has to carry WHY soft_on_path was refused (the
    // planner error code + reason + the soft_on_path->spring_back fallback
    // pair), not only the fact that a downgrade happened.
    std::map<std::string, std::string> escalation_details{
        {"reason", stop_failure_reason},
        {"requested_stop_mode", requested_mode},
        {"executed_stop_mode", executed_mode},
        {"emergency_fallback", fallback_outcome},
        {"hard_stop_transitioned", confirmed_but_not_settled ? "false" : "true"},
        {"phase", "operator_stop"}};
    if (!confirmed_but_not_settled) {
      add_hard_stop_floor_details(escalation_details, execution_backend_);
    }
    for (const auto& [key, value] : settle_details) {
      escalation_details[key] = value;
    }
    if (controlled_result.error.has_value()) {
      escalation_details["controlled_stop_error_code"] = controlled_result.error->code;
      const auto reason_it = controlled_result.error->details.find("reason");
      if (reason_it != controlled_result.error->details.end()) {
        escalation_details["controlled_stop_error_reason"] = reason_it->second;
      }
    }
    // Carry the whole soft_on_path / fallback diagnostic surface (error code,
    // reason, per-detail jerk/accel/joint, and the fallback pairing) so the
    // escalation event is fully self-describing — nothing about WHY the
    // requested stop was refused is left only in dropped backend state.
    for (const auto& [diag_key, diag_value] : controlled_result.diagnostics) {
      if (diag_key.rfind("soft_on_path", 0) == 0 || diag_key.rfind("fallback", 0) == 0 ||
          diag_key.rfind("hard_stop", 0) == 0) {
        escalation_details[diag_key] = interrupt_structured_scalar_text(diag_value);
      }
    }
    emit_event(
        {KernelEventType::kErrorRaised,
         sm_.status().execution_id,
         clock_.now(),
         {},
         {},
         std::optional<ErrorDTO>{ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                          stop_failure_message, escalation_details}},
         escalation_details});
  }

  EventDeferralScope event_scope(*this);
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!operator_stop_worker_is_current(generation, stop_token)) {
    return;
  }

  const std::string requested_mode = stop_mode_string(profile.stop_mode);
  const std::string executed_mode = stop_mode_string(controlled_result.executed_stop_mode);

  if (!controlled_ok && confirmed_but_not_settled) {
    ErrorDTO unsettled_stop_error{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "operator controlled stop was accepted and confirmed, but measured "
        "velocity did not settle within the effective stop deadline; redundant "
        "JTC cancel "
        "was not attempted",
        {{"reason", "stop_not_settled_measured_velocity"},
         {"controlled_stop_error_reason", stop_failure_reason},
         {"requested_stop_mode", requested_mode},
         {"executed_stop_mode", executed_mode},
         {"emergency_fallback", "not_attempted_controlled_stop_already_confirmed"},
         {"hard_stop_transitioned", "false"},
         {"phase", "operator_stop"}}};
    for (const auto& [key, value] : settle_details) {
      unsettled_stop_error.details[key] = value;
    }
    operator_stop_worker_.set_intervention_error(unsettled_stop_error);
    sm_.set_last_error(error_code_from_dto(unsettled_stop_error));
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                              runtime_collision_generation);
    cache_.invalidate_all();
    auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                                      TerminalCommitter::Origin::kStopAuthority);
    if (!transition.ok()) {
      return;
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    snapshot.error = unsettled_stop_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                unsettled_stop_error,
                unsettled_stop_error.details});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "stop_not_settled_measured_velocity"}}});
    return;
  }

  if (stop_path_collision_details.has_value()) {
    const std::string fallback_outcome = fallback_legacy_result && fallback_legacy_result->accepted
                                             ? std::string("legacy_stop_succeeded")
                                             : std::string("legacy_stop_failed");
    auto collision_details = *stop_path_collision_details;
    collision_details["reason"] = "operator_stop_path_collision";
    collision_details["controlled_stop_error_reason"] = stop_failure_reason;
    collision_details["requested_stop_mode"] = requested_mode;
    collision_details["executed_stop_mode"] = executed_mode;
    collision_details["emergency_fallback"] = fallback_outcome;
    collision_details["hard_stop_transitioned"] = "true";
    collision_details["phase"] = "operator_stop";
    add_hard_stop_floor_details(collision_details, execution_backend_);
    if (controlled_result.error.has_value()) {
      collision_details["controlled_stop_error_code"] = controlled_result.error->code;
      const auto reason_it = controlled_result.error->details.find("reason");
      if (reason_it != controlled_result.error->details.end()) {
        collision_details["controlled_stop_detail_reason"] = reason_it->second;
      }
    }
    if (fallback_legacy_result && fallback_legacy_result->error.has_value()) {
      collision_details["legacy_stop_error_code"] = fallback_legacy_result->error->code;
      const auto reason_it = fallback_legacy_result->error->details.find("reason");
      if (reason_it != fallback_legacy_result->error->details.end()) {
        collision_details["legacy_stop_error_reason"] = reason_it->second;
      }
    }
    ErrorDTO collision_stop_error{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "collision detected on the operator stop path during controlled stop; "
        "operator intervention required after emergency hard-stop floor",
        collision_details};
    operator_stop_worker_.set_intervention_error(collision_stop_error);
    sm_.set_last_error(error_code_from_dto(collision_stop_error));
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                              runtime_collision_generation);
    cache_.invalidate_all();
    auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                                      TerminalCommitter::Origin::kStopAuthority);
    if (!transition.ok()) {
      return;
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    snapshot.error = collision_stop_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                collision_stop_error,
                collision_stop_error.details});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "operator_stop_path_collision"}}});
    return;
  }

  if (!controlled_ok && fallback_legacy_result && !fallback_legacy_result->accepted) {
    ErrorDTO total_failure{std::string(kErrorMotionInterruptBackendStopFailed),
                           "operator stop failed: controlled-stop refused and emergency "
                           "legacy backend.stop() also failed",
                           {{"reason", "operator_stop_total_failure"},
                            {"controlled_stop_error_reason", stop_failure_reason},
                            {"requested_stop_mode", requested_mode},
                            {"executed_stop_mode", executed_mode},
                            {"emergency_fallback", "legacy_stop_failed"},
                            {"hard_stop_transitioned", "true"},
                            {"phase", "operator_stop"}}};
    add_hard_stop_floor_details(total_failure.details, execution_backend_);
    for (const auto& [key, value] : settle_details) {
      total_failure.details[key] = value;
    }
    if (controlled_result.error.has_value()) {
      total_failure.details["controlled_stop_error_code"] = controlled_result.error->code;
      const auto reason_it = controlled_result.error->details.find("reason");
      if (reason_it != controlled_result.error->details.end()) {
        total_failure.details["controlled_stop_error_reason"] = reason_it->second;
      }
      for (const auto& [key, value] : controlled_result.error->details) {
        total_failure.details["controlled_stop_detail_" + key] = value;
      }
    }
    for (const auto& [diag_key, diag_value] : controlled_result.diagnostics) {
      if (diag_key.rfind("soft_on_path", 0) == 0 || diag_key.rfind("fallback", 0) == 0 ||
          diag_key.rfind("hard_stop", 0) == 0) {
        total_failure.details[diag_key] = interrupt_structured_scalar_text(diag_value);
      }
    }
    if (fallback_legacy_result && fallback_legacy_result->error.has_value()) {
      total_failure.details["legacy_stop_error_code"] = fallback_legacy_result->error->code;
      const auto reason_it = fallback_legacy_result->error->details.find("reason");
      if (reason_it != fallback_legacy_result->error->details.end()) {
        total_failure.details["legacy_stop_error_reason"] = reason_it->second;
      }
      for (const auto& [key, value] : fallback_legacy_result->error->details) {
        total_failure.details["legacy_stop_detail_" + key] = value;
      }
    }
    operator_stop_worker_.set_intervention_error(total_failure);
    sm_.set_last_error(error_code_from_dto(total_failure));
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                              runtime_collision_generation);
    cache_.invalidate_all();
    auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                                      TerminalCommitter::Origin::kStopAuthority);
    if (!transition.ok()) {
      return;
    }
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    snapshot.error = total_failure;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                total_failure,
                total_failure.details});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                pre_completion_state,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {{"reason", "operator_stop_total_failure"}}});
    return;
  }

  auto snapshot = execution_backend_.get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();

  // Build the degraded-stop error and publish it to the terminal-error
  // fallback BEFORE the state machine becomes observably kStopped. A consumer
  // that polls the terminal state and then reads the snapshot must never see
  // kStopped without the error explaining the downgraded stop. get_execution_
  // snapshot() falls back to operator_stop_worker_.terminal_error() for a
  // kStopped snapshot that lacks an error, so this must be set before the
  // commit; setting it afterward (as the state-transition-lost and total-
  // failure branches do NOT — they set their fallback before committing)
  // opens a window where the fallback is still empty.
  std::optional<ErrorDTO> degraded_stop_error;
  if (!controlled_ok && fallback_legacy_result && fallback_legacy_result->accepted) {
    ErrorDTO error{std::string(kErrorMotionInterruptBackendStopFailed),
                   stop_failure_message,
                   {{"reason", stop_failure_reason},
                    {"requested_stop_mode", requested_mode},
                    {"executed_stop_mode", executed_mode},
                    {"emergency_fallback", "legacy_stop_succeeded"},
                    {"hard_stop_transitioned", "true"}}};
    add_hard_stop_floor_details(error.details, execution_backend_);
    for (const auto& [key, value] : settle_details) {
      error.details[key] = value;
    }
    operator_stop_worker_.set_terminal_error(error);
    degraded_stop_error = std::move(error);
  }

  // Publish the post-stop snapshot BEFORE the state machine becomes
  // observably kStopped, for the same reason as the degraded-stop error
  // above: while kStopping, observers are served a live backend read, but
  // the moment the state flips to a terminal state they are served the
  // published snapshot. Publishing after the commit leaves a window where a
  // consumer that saw kStopped reads a stale snapshot without the post-stop
  // joint state. If the commit is lost to another terminal authority, the
  // intervention branch below publishes its own corrected snapshot.
  snapshot.state = ExecutionState::kStopped;
  if (degraded_stop_error.has_value()) {
    snapshot.error = degraded_stop_error;
  }
  publish_execution_snapshot(std::move(snapshot));
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced,
                            runtime_collision_generation);

  {
    // RT-8B: commit the definitive stop certificate before terminal
    // `Stopped` becomes externally observable and before this worker's
    // owner flag clears, so replan recovery is actionable the moment the
    // terminal hold is visible (spec/230 §3.7.1).
    KernelStopCertificateDTO certificate;
    certificate.completion_result = StopCertificateOutcome::kSucceeded;
    certificate.stop_owner = StopOwnerKind::kOperatorStopWorker;
    certificate.stop_owner_generation = stop_owner_generation;
    certificate.execution_id = sm_.status().execution_id;
    certificate.control_command_epoch = control_command_epoch();
    certificate.requested_stop_mode = requested_mode;
    certificate.executed_stop_mode = executed_mode;
    certificate.preemption_result = InterruptPreemptionResult::kNoneRequired;
    certificate.backend_stop_confirmed = true;
    certificate.observed_state_at_rest = settle_signal_observed;
    certificate.observed_state_fresh = settle_signal_observed;
    certificate.at_rest_evidence =
        settle_signal_observed ? AtRestEvidence::kMeasuredVelocity : AtRestEvidence::kUnavailable;
    certificate.completed_at_ns = clock_.now();
    certificate.command_source = "operator_stop_worker";
    publish_stop_certificate(std::move(certificate));
  }
  // The physical/backend stop command is definitive at this point. Thread
  // cleanup and evidence emission continue, but they are not stop authority.
  operator_stop_command_active_.store(false, std::memory_order_release);
  (void)publish_terminal_recovery_assessment_locked(ExecutionState::kStopped,
                                                    "operator_stop_preterminal");
  auto transition = commit_terminal(ExecutionState::kStopped, "terminal_commit",
                                    TerminalCommitter::Origin::kStopAuthority);
  if (!transition.ok() && sm_.status().state != ExecutionState::kStopped) {
    ErrorDTO err{std::string(kErrorMotionInterruptBackendStopFailed),
                 "operator controlled stop completed, but terminal authority was "
                 "already claimed by another state before kStopped could be recorded",
                 {{"reason", "operator_stop_terminal_transition_lost"},
                  {"current_state", to_string(sm_.status().state)},
                  {"requested_stop_mode", requested_mode},
                  {"executed_stop_mode", executed_mode},
                  {"phase", "operator_stop"}}};
    sm_.set_last_error(error_code_from_dto(err));
    auto prev = sm_.status().state;
    auto intervention = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                                        TerminalCommitter::Origin::kStopAuthority);
    if (intervention.ok()) {
      auto failed_snapshot = execution_backend_.get_execution_snapshot();
      failed_snapshot.timestamp_ns = clock_.now();
      failed_snapshot.state = sm_.status().state;
      failed_snapshot.error = err;
      publish_execution_snapshot(std::move(failed_snapshot));
      emit_event({KernelEventType::kErrorRaised,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  err,
                  err.details});
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  prev,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "operator_stop_terminal_transition_lost"}}});
    }
    return;
  }

  std::map<std::string, std::string> stop_issued_details;
  if (controlled_ok) {
    stop_issued_details = controlled_stop_success_event_details(
        profile, controlled_result, execution_backend_, "operator_stop");
  }
  // Commit-coupled recovery publication (spec/400 Run rule): the certified
  // replan offer exists the moment terminal Stopped is observable, with no
  // post-terminal polling round trip.
  (void)refresh_recovery_assessment_locked("operator_stop_terminal");
  emit_event({KernelEventType::kStopIssued,
              sm_.status().execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              std::move(stop_issued_details)});
  emit_event({KernelEventType::kStateTransition,
              sm_.status().execution_id,
              clock_.now(),
              pre_completion_state,
              sm_.status().state,
              std::nullopt,
              {}});
  std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
  metrics_.stops_issued++;
}

TerminalCommitter::CommitOutcome ExecutionKernel::commit_terminal(
    ExecutionState proposed, const char* reason, TerminalCommitter::Origin origin) {
  auto outcome = terminal_committer_.commit({proposed, reason, origin});
  if (outcome.ok()) {
    switch (outcome.committed) {
      using enum ExecutionState;
      case kPaused:
        invalidate_gripper_authority(GripperInvalidationReason::kPause);
        break;
      case kStopping:
      case kStopped:
      case kInterrupted:
      case kFailed:
      case kInterventionRequired:
        invalidate_gripper_authority(GripperInvalidationReason::kStop);
        break;
      default:
        break;
    }
    // RT-8B: terminal transitions change recovery authority facts. This
    // function is reached both with and without lifecycle_mutex_ held, and
    // std::mutex try_lock by the owning thread is UB. Mark dirty and enqueue
    // a coalesced Monitoring-role refresh; lifecycle-held call sites may still
    // refresh synchronously when terminal ordering requires it.
    recovery_assessment_dirty_.store(true, std::memory_order_release);
    schedule_recovery_assessment_refresh("terminal_commit");
  }
  if (outcome.ok() && outcome.overrode) {
    // An authority asserted mid-decision overrode the proposed terminal —
    // exactly the laundering window the committer exists to close. Emit
    // the override as a structured event so the incident trail shows why
    // the terminal differs from what the calling path derived.
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                {},
                outcome.committed,
                std::nullopt,
                {{"reason", std::string(reason)},
                 {"terminal_authority_override", "true"},
                 {"proposed_terminal", std::string(to_string(proposed))}}});
  }
  return outcome;
}

void ExecutionKernel::emit_event(KernelEvent event) {
  prepare_event(event);
  if (active_event_deferral_ != nullptr) {
    active_event_deferral_->defer(std::move(event));
    return;
  }
  dispatch_prepared_event(event);
}

void ExecutionKernel::emit_event_immediate(KernelEvent event) {
  prepare_event(event);
  dispatch_prepared_event(event);
}

void ExecutionKernel::prepare_event(KernelEvent& event) const {
  stamp_active_request_correlation(event.details);
  if (event.error.has_value()) {
    for (const auto& [key, value] : event.error->details) {
      event.details.emplace(key, value);
    }
  }
}

void ExecutionKernel::dispatch_prepared_event(KernelEvent& event_to_emit) {
  event_to_emit.sequence = event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  flight_recorder_.record(event_to_emit);
  const auto subscriber_exceptions = event_publisher_.dispatch(event_to_emit);
  if (subscriber_exceptions > 0) {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.event_subscriber_exceptions += subscriber_exceptions;
  }
}

void ExecutionKernel::set_flight_recorder_directory(std::string directory) {
  flight_recorder_.set_dump_directory(std::move(directory));
}

std::string ExecutionKernel::dump_flight_record(const char* reason) noexcept {
  return flight_recorder_.dump_now(reason);
}

void ExecutionKernel::set_active_request_correlation(const ExecuteRequestDTO& request) {
  active_correlation_.set_from_request(request);
  { runtime_collision_monitor_.set_active_contact_policy_id(request.contact_policy_id); }
}

void ExecutionKernel::clear_active_request_correlation() {
  active_correlation_.clear_all();
  { runtime_collision_monitor_.clear_active_contact_policy_id(); }
}

void ExecutionKernel::stamp_active_request_correlation(
    std::map<std::string, std::string>& details) const {
  active_correlation_.stamp_details(details);
  {
    const auto contact_policy_id = runtime_collision_monitor_.active_contact_policy_id();
    if (!contact_policy_id.empty()) {
      details.emplace("contact_policy_id", contact_policy_id);
    }
  }
}

void ExecutionKernel::stamp_active_request_correlation(ExecutionSnapshot& snapshot) const {
  active_correlation_.stamp_snapshot(snapshot);
  if (snapshot.contact_policy_id.empty()) {
    snapshot.contact_policy_id = runtime_collision_monitor_.active_contact_policy_id();
  }
}

SubscriptionHandle ExecutionKernel::subscribe_events(EventCallback callback) {
  return event_publisher_.subscribe(std::move(callback));
}

void ExecutionKernel::unsubscribe_events(SubscriptionHandle handle) {
  event_publisher_.unsubscribe(handle);
}

Result<void> ExecutionKernel::register_interrupt_source(const InterruptSourceDefinition& source) {
  return interrupt_manager_.register_source(source);
}

Result<void> ExecutionKernel::register_interrupt_policy(const InterruptPolicyDefinition& policy) {
  return interrupt_manager_.register_policy(policy);
}

CollisionWorldRevision ExecutionKernel::current_authoritative_collision_world_revision() const {
  ICollisionWorldSnapshotPublisher* publisher = collision_resources_.snapshot().snapshot_publisher;
  if (publisher == nullptr) {
    return {};
  }
  try {
    auto snapshot = publisher->snapshot();
    if (snapshot) {
      return snapshot->revision;
    }
  } catch (const std::exception&) {
    // Callers use the default revision as "no authoritative revision
    // available"; validation itself reports the concrete publisher error.
  } catch (...) {
    // Callers use the default revision as "no authoritative revision
    // available"; validation itself reports the concrete publisher error.
  }
  return {};
}

ExecutionKernel::RetainedSceneSnapshotLease::RetainedSceneSnapshotLease(
    ICollisionWorldSnapshotPublisher& publisher, CollisionWorldLeaseInventory inventory)
    : publisher_(&publisher), inventory_(std::move(inventory)) {}

ExecutionKernel::RetainedSceneSnapshotLease::~RetainedSceneSnapshotLease() {
  release();
}

ExecutionKernel::RetainedSceneSnapshotLease::RetainedSceneSnapshotLease(
    RetainedSceneSnapshotLease&& other) noexcept
    : publisher_(std::exchange(other.publisher_, nullptr)),
      inventory_(std::move(other.inventory_)) {}

ExecutionKernel::RetainedSceneSnapshotLease& ExecutionKernel::RetainedSceneSnapshotLease::operator=(
    RetainedSceneSnapshotLease&& other) noexcept {
  if (this != &other) {
    release();
    publisher_ = std::exchange(other.publisher_, nullptr);
    inventory_ = std::move(other.inventory_);
  }
  return *this;
}

void ExecutionKernel::RetainedSceneSnapshotLease::release() noexcept {
  if (publisher_ == nullptr || inventory_.projection_binding.projection_lease_id.empty()) {
    publisher_ = nullptr;
    return;
  }
  try {
    publisher_->release_scene_snapshot_lease_inventory(
        inventory_.projection_binding.projection_lease_id);
  } catch (...) {
    // Cleanup is idempotent and must not mask the motion outcome. Publishers
    // expose release as a no-fail operation; a violating implementation is
    // diagnosed by its retention metrics/teardown checks.
  }
  publisher_ = nullptr;
}

ProjectionLeaseBindingDTO ExecutionKernel::RetainedSceneSnapshotLease::binding() const {
  return inventory_.projection_binding;
}

Result<ExecutionKernel::RetainedSceneSnapshotLease>
ExecutionKernel::retain_current_scene_snapshot_lease_inventory(std::uint64_t exec_id,
                                                               const std::string& move_id) {
  ICollisionWorldSnapshotPublisher* publisher = collision_resources_.snapshot().snapshot_publisher;
  if (publisher == nullptr) {
    return Result<RetainedSceneSnapshotLease>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Scene authority is unavailable for planning",
                 {{"reason", "scene_projection_authority_unavailable"}}});
  }
  std::string projection_lease_id = "execution_" + std::to_string(exec_id);
  if (!move_id.empty()) {
    projection_lease_id += "_" + move_id;
  }
  const ProjectionLeaseId requested_lease_id{projection_lease_id};
  try {
    auto inventory = publisher->retain_scene_snapshot_lease_inventory(requested_lease_id);
    if (!inventory.has_value() || !inventory->projection_binding.valid() ||
        inventory->projection_binding.projection_lease_id != requested_lease_id ||
        !inventory->bundle.world || inventory->bundle.world->revision.revision_id == 0 ||
        inventory->bundle.world->revision.content_hash.empty()) {
      if (inventory.has_value() && !inventory->projection_binding.projection_lease_id.empty()) {
        publisher->release_scene_snapshot_lease_inventory(
            inventory->projection_binding.projection_lease_id);
      }
      return Result<RetainedSceneSnapshotLease>::Err(
          ErrorDTO{kErrorExecutionRejected,
                   "Scene authority could not retain an exact planner projection",
                   {{"reason", "planner_projection_retention_failed"}}});
    }
    return Result<RetainedSceneSnapshotLease>::Ok(
        RetainedSceneSnapshotLease{*publisher, std::move(*inventory)});
  } catch (const std::exception& error) {
    return Result<RetainedSceneSnapshotLease>::Err(ErrorDTO{
        kErrorExecutionRejected,
        "Scene projection retention raised an exception",
        {{"reason", "planner_projection_retention_exception"}, {"exception", error.what()}}});
  } catch (...) {
    return Result<RetainedSceneSnapshotLease>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Scene projection retention raised a non-standard exception",
                 {{"reason", "planner_projection_retention_exception"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
}

std::optional<SceneSnapshotLease> ExecutionKernel::build_scene_snapshot_lease(
    const CollisionWorldLeaseInventory& inventory, const ExecuteRequestDTO& request,
    std::uint64_t exec_id) const {
  if (!inventory.bundle.world || !inventory.bundle.policy.has_value()) {
    return std::nullopt;
  }

  SceneSnapshotLease lease;
  lease.lease_id = inventory.projection_binding.projection_lease_id.empty()
                       ? "execution_" + std::to_string(exec_id)
                       : inventory.projection_binding.projection_lease_id.value();
  lease.validation_epoch = exec_id;
  lease.captured_at_ns = inventory.captured_at_ns > 0 ? inventory.captured_at_ns : clock_.now();
  lease.lease_max_age_ns = config_.scene_snapshot_lease_max_age_ns;
  lease.projection_revision = inventory.projection_binding.projection_revision;
  lease.world_revision = inventory.bundle.world->revision;
  lease.policy_revision = inventory.bundle.policy->revision;
  lease.topology_epoch = inventory.topology_epoch;
  lease.policy_epoch = inventory.policy_epoch;

  const auto tighten_lease_age = [](std::optional<std::int64_t>& current,
                                    std::optional<std::int64_t> candidate) {
    if (!candidate.has_value()) {
      return;
    }
    if (!current.has_value() || *candidate < *current) {
      current = *candidate;
    }
  };
  const auto target_ids = observed_target_object_ids(request, inventory.bundle.policy);
  const auto body_hashes = body_hashes_by_id(inventory.bundle.world);
  std::unordered_set<std::string> included_object_ids;
  lease.objects.reserve(inventory.live_objects.size());
  for (const auto& live : inventory.live_objects) {
    if (live.object_id.empty() || live.collision_body_ids.empty()) {
      continue;
    }
    included_object_ids.insert(live.object_id);
    SceneSnapshotLeaseObject object;
    object.object_id = live.object_id;
    object.collision_body_ids = live.collision_body_ids;
    object.object_revision_token = live.object_revision_token;
    object.pose = live.pose;
    object.source_id = live.source_id;
    object.source_timestamp_ns = live.source_timestamp_ns;
    object.source_freshness_budget_ns = live.source_freshness_budget_ns;
    tighten_lease_age(lease.lease_max_age_ns, live.lease_max_age_ns);

    const bool request_target = target_ids.find(live.object_id) != target_ids.end();
    if (live.declared_role == ObservedSceneObjectRole::kObservedTarget && request_target) {
      object.role = ObservedSceneObjectRole::kObservedTarget;
      object.target_drift_tolerance_m = live.target_drift_tolerance_m.value_or(
          config_.scene_snapshot_default_observed_drift_tolerance_m);
      object.target_angular_tolerance_rad = live.target_angular_tolerance_rad.value_or(
          config_.scene_snapshot_default_observed_angular_tolerance_rad);
    } else if (live.declared_role == ObservedSceneObjectRole::kObservedDynamicObstacle) {
      object.role = ObservedSceneObjectRole::kObservedDynamicObstacle;
      object.obstacle_drift_tolerance_m = live.obstacle_drift_tolerance_m.value_or(0.0);
      object.obstacle_angular_tolerance_rad = live.obstacle_angular_tolerance_rad.value_or(0.0);
      object.runtime_dynamic_obstacle_allowed = live.runtime_dynamic_obstacle_allowed;
    } else if (live.declared_role == ObservedSceneObjectRole::kObservedTarget) {
      object.role = ObservedSceneObjectRole::kSnapshotObstacle;
    } else if (live.declared_role.has_value()) {
      object.role = *live.declared_role;
    } else if (request_target) {
      object.role = ObservedSceneObjectRole::kObservedTarget;
      object.target_drift_tolerance_m = config_.scene_snapshot_default_observed_drift_tolerance_m;
      object.target_angular_tolerance_rad =
          config_.scene_snapshot_default_observed_angular_tolerance_rad;
    } else {
      object.role = ObservedSceneObjectRole::kSnapshotObstacle;
    }

    const bool pose_drift_role = object.role == ObservedSceneObjectRole::kObservedTarget ||
                                 object.role == ObservedSceneObjectRole::kObservedDynamicObstacle;
    const bool body_hash_identity_role =
        !pose_drift_role && object.role != ObservedSceneObjectRole::kSnapshotObstacle;
    if (body_hash_identity_role) {
      for (const auto& body_id : object.collision_body_ids) {
        const auto it = body_hashes.find(body_id);
        if (it != body_hashes.end()) {
          object.body_content_hashes[body_id] = it->second;
        }
      }
    }
    lease.objects.push_back(std::move(object));
  }
  for (const auto& target_id : target_ids) {
    if (included_object_ids.find(target_id) != included_object_ids.end()) {
      continue;
    }
    SceneSnapshotLeaseObject missing_target;
    missing_target.object_id = target_id;
    missing_target.role = ObservedSceneObjectRole::kObservedTarget;
    lease.objects.push_back(std::move(missing_target));
  }
  return lease;
}

std::optional<SceneSnapshotLeaseValidationContext>
ExecutionKernel::current_scene_snapshot_lease_validation_context() const {
  ICollisionWorldSnapshotPublisher* publisher = collision_resources_.snapshot().snapshot_publisher;
  if (publisher == nullptr) {
    return std::nullopt;
  }
  try {
    auto inventory = publisher->current_scene_snapshot_lease_inventory();
    if (!inventory.has_value() || !inventory->bundle.world ||
        !inventory->bundle.policy.has_value()) {
      return std::nullopt;
    }
    SceneSnapshotLeaseValidationContext context;
    context.now_ns = clock_.now();
    context.default_lease_max_age_ns = config_.scene_snapshot_lease_max_age_ns;
    context.live_projection_revision = inventory->projection_binding.projection_revision;
    context.live_world_revision = inventory->bundle.world->revision;
    context.live_policy_revision = inventory->bundle.policy->revision;
    context.topology_epoch = inventory->topology_epoch;
    context.policy_epoch = inventory->policy_epoch;
    context.live_objects = std::move(inventory->live_objects);
    enrich_live_objects_with_body_hashes(context.live_objects, inventory->bundle.world);
    return context;
  } catch (const std::exception&) {
  } catch (...) {
  }
  return std::nullopt;
}

std::shared_ptr<CollisionCancellationToken> ExecutionKernel::install_validation_cancel_token() {
  auto token = std::make_shared<CollisionCancellationToken>();
  {
    std::scoped_lock lock(validation_cancel_mutex_);
    active_validation_cancel_token_ = token;
  }
  if (auto latch = interrupt_manager_.active_latch();
      latch && latch->preemption_action_requested == InterruptPreemptionAction::kCancelValidation) {
    token->request_cancel("interrupt:" + latch->latch_id);
  }
  return token;
}

void ExecutionKernel::clear_validation_cancel_token(
    const std::shared_ptr<CollisionCancellationToken>& token) {
  std::scoped_lock lock(validation_cancel_mutex_);
  auto active = active_validation_cancel_token_.lock();
  if (active && active == token) {
    active_validation_cancel_token_.reset();
  }
}

void ExecutionKernel::request_validation_cancel_from_interrupt(
    const InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch ||
      result.active_latch->preemption_action_requested !=
          InterruptPreemptionAction::kCancelValidation) {
    return;
  }
  request_active_validation_cancel("interrupt:" + result.active_latch->latch_id);
}

void ExecutionKernel::request_planning_cancel_from_interrupt(const InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch ||
      result.active_latch->preemption_action_requested !=
          InterruptPreemptionAction::kCancelPlanning) {
    return;
  }
  request_active_planning_cancel();
}

void ExecutionKernel::request_active_validation_cancel(std::string reason) {
  std::shared_ptr<CollisionCancellationToken> token;
  {
    std::scoped_lock lock(validation_cancel_mutex_);
    token = active_validation_cancel_token_.lock();
  }
  if (token) {
    token->request_cancel(std::move(reason));
  }
}

void ExecutionKernel::request_active_planning_cancel() {
  planner_.request_cancel();
}

void ExecutionKernel::record_terminal_fault_authority(const InterruptIngestResult& result) {
  if ((result.latched || result.active_latch.has_value()) &&
      (!result.active_latch || result.active_latch->preemption_action_requested !=
                                   InterruptPreemptionAction::kCompleteMoveOnSignal)) {
    terminal_fault_authority_asserted_.store(true, std::memory_order_release);
  }
}

void ExecutionKernel::mark_active_interrupt_preemption_result(
    InterruptPreemptionResult result, const std::map<std::string, std::string>& extra_details) {
  auto latch = interrupt_manager_.active_latch();
  if (!latch) {
    return;
  }
  if (result == InterruptPreemptionResult::kSucceeded && active_trajectory_preempt_pending()) {
    return;
  }
  mark_interrupt_preemption_result(*latch, result, extra_details);
}

void ExecutionKernel::mark_interrupt_preemption_result(
    const InterruptLatch& latch, InterruptPreemptionResult result,
    const std::map<std::string, std::string>& extra_details) {
  const auto now = clock_.now();
  auto marked = interrupt_manager_.mark_preemption_result(latch.latch_id, result,
                                                          now - latch.latched_at_ns, now);
  if (marked.ok()) {
    auto marked_result = marked.value();
    record_terminal_fault_authority(marked_result);
    if (!extra_details.empty()) {
      for (auto& event : marked_result.events) {
        if (event.type == "interrupt_preempt_applied") {
          for (const auto& [key, value] : extra_details) {
            event.details[key] = value;
          }
        }
      }
    }
    emit_interrupt_events(marked_result);
  }
}

bool ExecutionKernel::active_trajectory_preempt_pending() const {
  auto latch = interrupt_manager_.active_latch();
  return latch.has_value() &&
         latch->preemption_action_requested == InterruptPreemptionAction::kPreemptTrajectory &&
         latch->preemption_result == InterruptPreemptionResult::kPending;
}

void ExecutionKernel::apply_interrupt_ingest_effects(const InterruptIngestResult& result,
                                                     bool apply_preemption_inline) {
  record_terminal_fault_authority(result);
  // Commit the observable kInterrupted state BEFORE requesting the in-flight
  // planner/validation cancel. request_*_cancel_from_interrupt unblocks the
  // worker's plan()/validate() with a cancellation error; if the state flip
  // came after, the worker could reach its cancel-outcome path (which is gated
  // on state == kInterrupted) and read the state before this thread committed
  // it, fall through to the generic-failed path, and lose the interrupt
  // preemption bookkeeping (preemption_result and the error snapshot) to a
  // terminal-authority override. Flipping first makes the worker's read
  // happen-after this commit through the cancel-flag synchronization.
  if (result.latched) {
    const auto prev_state = sm_.status().state;
    if (prev_state != ExecutionState::kInterrupted) {
      auto transition = commit_terminal(ExecutionState::kInterrupted, "terminal_commit");
      if (transition.ok()) {
        auto snapshot = get_execution_snapshot();
        snapshot.timestamp_ns = clock_.now();
        snapshot.state = ExecutionState::kInterrupted;
        publish_execution_snapshot(std::move(snapshot));
        emit_event({KernelEventType::kStateTransition,
                    sm_.status().execution_id,
                    clock_.now(),
                    prev_state,
                    ExecutionState::kInterrupted,
                    std::nullopt,
                    {{"reason", "interrupt_latched"}}});
      }
    }
  }
  request_validation_cancel_from_interrupt(result);
  request_planning_cancel_from_interrupt(result);
  emit_interrupt_events(result);
  if (apply_preemption_inline) {
    apply_latched_interrupt_preemption(result);
  }
}

void ExecutionKernel::install_builtin_interrupt_sources() {
  auto source = interrupt_manager_.register_source(runtime_collision_fault_source_definition());
  if (!source.ok()) {
    throw std::logic_error("failed to register kernel runtime collision interrupt source: " +
                           source.error().message);
  }
  auto policy = interrupt_manager_.register_policy(runtime_collision_fault_policy_definition());
  if (!policy.ok()) {
    throw std::logic_error("failed to register kernel runtime collision interrupt policy: " +
                           policy.error().message);
  }
}

InterruptIngestResult ExecutionKernel::publish_runtime_collision_fault_interrupt(
    const CollisionCheckResultDTO& check, const std::map<std::string, std::string>& metadata) {
  InterruptSignalUpdate signal;
  signal.source_id = kRuntimeCollisionFaultSourceId;
  signal.sequence_id =
      runtime_collision_fault_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  signal.timestamp_ns = clock_.now();
  signal.freshness_ns = 0;
  signal.value = true;
  signal.metadata["surface"] = interrupt_structured_string("observed_runtime_monitor");
  signal.metadata["in_collision"] = interrupt_structured_bool(check.in_collision);
  if (check.contact_body_a) {
    signal.metadata["contact_a"] = *check.contact_body_a;
  }
  if (check.contact_body_b) {
    signal.metadata["contact_b"] = *check.contact_body_b;
  }
  for (const auto& [key, value] : metadata) {
    signal.metadata[key] = value;
  }
  auto result =
      publish_fast_interrupt_signals_internal(std::vector<InterruptSignalUpdate>{std::move(signal)},
                                              /*apply_preemption_inline=*/false);
  schedule_latched_interrupt_preemption(result);
  // Collision-tick thread: provably never a lifecycle owner, so the
  // try-refresh is safe here and publishes the Interrupted/waiting
  // assessment promptly after the fault latch.
  try_refresh_recovery_assessment("runtime_collision_fault_latched");
  return result;
}

void ExecutionKernel::schedule_latched_interrupt_preemption(InterruptIngestResult result) {
  if (!result.latched || !result.active_latch ||
      result.active_latch->preemption_action_requested !=
          InterruptPreemptionAction::kPreemptTrajectory) {
    return;
  }
  // Stop authority has priority over speculative future checks. Invalidate
  // queued and in-flight permits before the preempt worker can block or issue
  // backend control so lower-priority collision work cannot survive into the
  // stop window.
  revoke_runtime_collision_lookahead_clear_permits();
  const auto latch_id = result.active_latch->latch_id;
  const auto source_id = result.active_latch->source_id;
  const auto policy_id = result.active_latch->policy_id;
  fault_preempt_worker_.join_finished();
  const bool started = fault_preempt_worker_.start(
      [this, result = std::move(result), latch_id](std::uint64_t generation,
                                                   std::stop_token stop_token) mutable {
        if (!fault_preempt_worker_.is_current(generation, stop_token)) {
          return;
        }
        auto active_latch = interrupt_manager_.active_latch();
        if (!active_latch || active_latch->latch_id != latch_id ||
            active_latch->preemption_result != InterruptPreemptionResult::kPending) {
          return;
        }
        apply_latched_interrupt_preemption(result, stop_token);
        try_refresh_recovery_assessment("fault_preempt_worker_definitive");
      },
      [this]() { try_refresh_recovery_assessment("fault_preempt_worker_finished"); });
  if (started) {
    return;
  }

  request_fault_preempt_controlled_stop();
  std::map<std::string, std::string> details{{"reason", "fault_preempt_worker_busy"},
                                             {"phase", "fault_preempt_worker_start"},
                                             {"latch_id", latch_id},
                                             {"source_id", source_id},
                                             {"policy_id", policy_id},
                                             {"preemption_result", "pending"}};
  emit_event({KernelEventType::kErrorRaised,
              sm_.status().execution_id,
              clock_.now(),
              {},
              {},
              ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                       "fault preempt worker was already active; pending "
                       "preempt watchdog remains armed",
                       details},
              details});
}

void ExecutionKernel::apply_latched_interrupt_preemption(const InterruptIngestResult& result,
                                                         std::stop_token worker_stop_token) {
  if (!result.latched || !result.active_latch) {
    return;
  }
  const auto& latch = *result.active_latch;
  clear_fault_preempt_intervention_error();
  if (latch.preemption_action_requested != InterruptPreemptionAction::kPreemptTrajectory) {
    return;
  }
  auto active_latch = interrupt_manager_.active_latch();
  if (!active_latch || active_latch->latch_id != latch.latch_id ||
      active_latch->preemption_result != InterruptPreemptionResult::kPending) {
    return;
  }
  cache_.invalidate_all();
  std::uint64_t fault_stop_generation = 0;
  {
    // One generation domain covers every fault-preempt attempt, including
    // legacy backend.stop() paths without a controlled-stop source. Capture
    // the value for this attempt; never re-read a potentially newer global
    // generation when publishing its definitive certificate.
    std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
    fault_stop_generation = ++fault_preempt_stop_generation_;
  }

  // Per spec/260 §2.2: when the active fault policy carries a
  // `stop_profile`, route the preempt through the same controlled-
  // stop machinery the desired-trip path uses
  // (`execute_controlled_stop` with the same
  // `ControlledStopResultDTO` surface). When no profile is set,
  // fall back to the legacy `execution_backend_.stop()`.
  std::optional<ControlledStopResultDTO> controlled_stop;
  std::optional<MotionInterruptStopProfileDTO> fault_stop_profile;
  // Per-policy stop_profile wins over the kernel-level default. If the
  // policy doesn't carry one but the deployment configured a kernel-
  // level `default_fault_stop_profile`, use that. Falls through to the
  // legacy `execution_backend_.stop()` only when both are unset.
  if (auto policy = interrupt_manager_.policy_by_id(latch.policy_id);
      policy.has_value() && policy->stop_profile) {
    fault_stop_profile = *policy->stop_profile;
  } else if (config_.default_fault_stop_profile) {
    fault_stop_profile = *config_.default_fault_stop_profile;
  }
  bool preempt_accepted = false;
  std::optional<ErrorDTO> preempt_error;
  std::map<std::string, std::string> preempt_event_details{{"phase", "preempt_trajectory"}};
  std::optional<std::map<std::string, std::string>> stop_path_collision_details;
  if (fault_stop_profile.has_value()) {
    clear_fault_preempt_stop_path_collision();
    auto fault_stop_source = std::make_shared<std::stop_source>();
    {
      std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
      fault_preempt_stop_source_ = fault_stop_source;
    }
    const auto clear_fault_stop_source =
        std::shared_ptr<void>(nullptr, [this, fault_stop_source](void*) {
          std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
          if (fault_preempt_stop_source_ == fault_stop_source) {
            fault_preempt_stop_source_.reset();
          }
        });
    const auto runtime_collision_generation = start_collision_monitoring(
        "fault_preempt_controlled_stop", RuntimeCollisionProtectionPurpose::kControlledStop);
    std::optional<std::uint64_t> active_fault_monitor_generation = runtime_collision_generation;
    const auto release_fault_monitor = [&](RuntimeCollisionMonitorRelease release) {
      if (!active_fault_monitor_generation.has_value()) {
        return;
      }
      stop_collision_monitoring(release, *active_fault_monitor_generation);
      active_fault_monitor_generation.reset();
    };
    const auto fault_monitor_scope = std::shared_ptr<void>(nullptr, [&](void*) {
      release_fault_monitor(RuntimeCollisionMonitorRelease::kFailClosedTerminal);
      clear_fault_preempt_stop_path_collision();
    });
    auto hard_stop_floor_attempt = std::make_shared<HardStopFloorAttempt>();
    auto deadline_watchdog_fired = std::make_shared<std::atomic_bool>(false);
    auto stop_session_complete = std::make_shared<std::atomic_bool>(false);
    std::stop_callback worker_stop_callback(
        worker_stop_token, [fault_stop_source]() { fault_stop_source->request_stop(); });
    ScheduleHandle stop_deadline_handle = 0;
    const auto effective_deadline_ns = effective_controlled_stop_deadline_ns(
        *fault_stop_profile, config_.complete_move_on_signal_timeout_ns);
    if (effective_deadline_ns > 0) {
      const auto requested_mode = std::string(stop_mode_string(fault_stop_profile->stop_mode));
      const auto fault_profile_id = fault_stop_profile->profile_id;
      const auto latch_id = latch.latch_id;
      const auto source_id = latch.source_id;
      const auto policy_id = latch.policy_id;
      stop_deadline_handle = scheduler_.schedule(
          clock_.now() + effective_deadline_ns,
          [this, fault_stop_source, hard_stop_floor_attempt, deadline_watchdog_fired,
           stop_session_complete, requested_mode, fault_profile_id, latch_id, source_id,
           policy_id]() {
            if (stop_session_complete->load(std::memory_order_acquire)) {
              return;
            }
            deadline_watchdog_fired->store(true, std::memory_order_release);
            const auto legacy =
                request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
            fault_stop_source->request_stop();
            const auto fallback_outcome = legacy.accepted ? std::string("legacy_stop_succeeded")
                                                          : std::string("legacy_stop_failed");
            std::map<std::string, std::string> details{{"reason", "in_stop_completion_timeout"},
                                                       {"phase", "fault_preempt_deadline_watchdog"},
                                                       {"fault_stop_profile_id", fault_profile_id},
                                                       {"requested_stop_mode", requested_mode},
                                                       {"latch_id", latch_id},
                                                       {"source_id", source_id},
                                                       {"policy_id", policy_id},
                                                       {"emergency_fallback", fallback_outcome},
                                                       {"hard_stop_transitioned", "true"}};
            add_hard_stop_floor_details(details, execution_backend_);
            emit_event({KernelEventType::kErrorRaised,
                        sm_.status().execution_id,
                        clock_.now(),
                        {},
                        {},
                        ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                 "fault controlled stop exceeded effective stop "
                                 "deadline; emergency hard-stop floor issued by "
                                 "deadline watchdog",
                                 details},
                        details});
          },
          SchedulerPriority::StopControl);
    }
    controlled_stop = execution_backend_.execute_controlled_stop(*fault_stop_profile,
                                                                 fault_stop_source->get_token());
    fail_closed_invalid_controlled_stop_result(*fault_stop_profile, *controlled_stop);
    stop_session_complete->store(true, std::memory_order_release);
    if (stop_deadline_handle != 0) {
      scheduler_.cancel(stop_deadline_handle);
    }
    stop_path_collision_details = fault_preempt_stop_path_collision_details();
    if (deadline_watchdog_fired->load(std::memory_order_acquire)) {
      controlled_stop->backend_stop_confirmed = false;
      controlled_stop->error = ErrorDTO{
          std::string(kErrorMotionInterruptBackendStopFailed),
          "fault controlled stop exceeded the effective stop deadline; "
          "emergency hard-stop floor was issued by the deadline watchdog",
          {{"reason", "in_stop_completion_timeout"}, {"phase", "fault_preempt_deadline_watchdog"}}};
    }
    if (stop_path_collision_details.has_value()) {
      auto collision_details = *stop_path_collision_details;
      collision_details["reason"] = "fault_preempt_stop_path_collision";
      collision_details["phase"] = "fault_preempt";
      collision_details["fault_stop_profile_id"] = fault_stop_profile->profile_id;
      collision_details["requested_stop_mode"] = stop_mode_string(fault_stop_profile->stop_mode);
      controlled_stop->backend_stop_confirmed = false;
      controlled_stop->error =
          ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                   "collision detected on the fault-preempt controlled-stop path; "
                   "emergency hard-stop floor is required",
                   collision_details};
    }
    const bool controlled_ok = controlled_stop->accepted &&
                               controlled_stop->backend_stop_confirmed &&
                               !controlled_stop->error.has_value();
    preempt_event_details["fault_stop_profile_id"] = fault_stop_profile->profile_id;
    preempt_event_details["requested_stop_mode"] = stop_mode_string(fault_stop_profile->stop_mode);
    preempt_event_details["executed_stop_mode"] =
        stop_mode_string(controlled_stop->executed_stop_mode);
    preempt_event_details["stop_command_source"] =
        stop_command_source_string(controlled_stop->stop_command_source);
    if (controlled_ok) {
      preempt_accepted = true;
      preempt_event_details = controlled_stop_success_event_details(
          *fault_stop_profile, *controlled_stop, execution_backend_, "fault_preempt");
      preempt_event_details["preempt_path"] = "controlled_stop";
      preempt_event_details["fault_stop_profile_id"] = fault_stop_profile->profile_id;
    } else {
      // Emergency fallback: industrial guarantee that a fault
      // preempt physically stops the robot even when the
      // configured stop_profile is unsupported, refused, times out,
      // or otherwise fails. Without this fallback a deployment with
      // a misconfigured fault profile (or a backend missing the
      // capability) would latch the fault but leave the controller
      // running. The controlled-stop failure is preserved in
      // diagnostics so the operator/audit trail sees the degradation;
      // the legacy stop result decides whether the preempt is
      // considered accepted.
      auto legacy_stop = request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
      bool legacy_recovered_already_stopped = false;
      if (!legacy_stop.accepted && stop_path_collision_details.has_value() &&
          legacy_stop.error.has_value() && legacy_stop.error->code == kErrorInvalidRequest) {
        try {
          const auto backend_snapshot = execution_backend_.get_execution_snapshot();
          legacy_recovered_already_stopped = backend_snapshot.state == ExecutionState::kStopped;
        } catch (const std::exception&) {
          legacy_recovered_already_stopped = false;
        } catch (...) {
          legacy_recovered_already_stopped = false;
        }
      }
      preempt_accepted = legacy_stop.accepted || legacy_recovered_already_stopped;
      const std::string controlled_reason =
          controlled_stop->error.has_value() && controlled_stop->error->details.contains("reason")
              ? controlled_stop->error->details.at("reason")
              : std::string("fault_stop_profile_not_confirmed");
      preempt_error = preempt_accepted
                          ? std::optional<ErrorDTO>(
                                ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                         "fault stop profile failed; emergency "
                                         "legacy backend.stop() guaranteed "
                                         "physical stop",
                                         {{"reason", "fault_stop_profile_failed_legacy_recovered"},
                                          {"controlled_stop_reason", controlled_reason},
                                          {"emergency_fallback", "legacy_stop_succeeded"},
                                          {"hard_stop_transitioned", "true"}}})
                          : std::optional<ErrorDTO>(
                                ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                         "fault stop failed: controlled-stop "
                                         "refused and emergency legacy "
                                         "backend.stop() also failed",
                                         {{"reason", "fault_stop_total_failure"},
                                          {"controlled_stop_reason", controlled_reason},
                                          {"emergency_fallback", "legacy_stop_failed"},
                                          {"hard_stop_transitioned", "true"}}});
      preempt_event_details["preempt_path"] =
          preempt_accepted ? "controlled_stop_failed_legacy_fallback_succeeded"
                           : "controlled_stop_failed_legacy_fallback_failed";
      preempt_event_details["emergency_fallback"] =
          preempt_accepted ? "legacy_stop_succeeded" : "legacy_stop_failed";
      preempt_event_details["hard_stop_transitioned"] = "true";
      if (legacy_recovered_already_stopped) {
        preempt_event_details["hard_stop_floor_already_stopped"] = "true";
      }
      add_hard_stop_floor_details(preempt_event_details, execution_backend_);
      if (stop_path_collision_details.has_value()) {
        for (const auto& [key, value] : *stop_path_collision_details) {
          preempt_event_details["stop_path_collision_" + key] = value;
        }
      }
    }
    release_fault_monitor(controlled_ok ? RuntimeCollisionMonitorRelease::kTerminalQuiesced
                                        : RuntimeCollisionMonitorRelease::kFailClosedTerminal);
  } else {
    if (worker_stop_token.stop_requested()) {
      return;
    }
    auto stop = execution_backend_.stop();
    preempt_accepted = stop.accepted;
    preempt_error = stop.error;
    preempt_event_details["preempt_path"] = "legacy_backend_stop";
  }

  const bool force_operator_intervention =
      stop_path_collision_details.has_value() || !preempt_accepted;
  if (force_operator_intervention) {
    std::map<std::string, std::string> terminal_details = preempt_event_details;
    terminal_details["reason"] = stop_path_collision_details.has_value()
                                     ? "fault_preempt_stop_path_collision"
                                     : "fault_stop_total_failure";
    terminal_details["latch_id"] = latch.latch_id;
    terminal_details["source_id"] = latch.source_id;
    terminal_details["policy_id"] = latch.policy_id;
    preempt_error = ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                             stop_path_collision_details.has_value()
                                 ? "fault-preempt stop-path collision required emergency hard-stop "
                                   "floor; operator intervention is required"
                                 : "fault-preempt controlled stop and emergency hard-stop floor "
                                   "both failed; operator intervention is required",
                             terminal_details};
    set_fault_preempt_intervention_error(*preempt_error);
  }

  const auto preemption_result = (force_operator_intervention || !preempt_accepted)
                                     ? InterruptPreemptionResult::kBackendPreemptFailed
                                     : InterruptPreemptionResult::kSucceeded;
  {
    // RT-8B: definitive fault-preempt completion record. Published before
    // the stop owner (fault_preempt_stop_source_) becomes observable as
    // inactive at scope exit, so admission can never see owner-inactive
    // without the matching certificate (spec/230 §3.7.1).
    KernelStopCertificateDTO certificate;
    certificate.completion_result = preemption_result == InterruptPreemptionResult::kSucceeded
                                        ? StopCertificateOutcome::kSucceeded
                                        : StopCertificateOutcome::kBackendPreemptFailed;
    certificate.stop_owner = StopOwnerKind::kFaultPreemptWorker;
    certificate.stop_owner_generation = fault_stop_generation;
    certificate.execution_id = latch.active_execution_id.value_or(sm_.status().execution_id);
    certificate.latch_id = latch.latch_id;
    certificate.control_command_epoch = control_command_epoch();
    if (auto it = preempt_event_details.find("requested_stop_mode");
        it != preempt_event_details.end()) {
      certificate.requested_stop_mode = it->second;
    }
    if (auto it = preempt_event_details.find("executed_stop_mode");
        it != preempt_event_details.end()) {
      certificate.executed_stop_mode = it->second;
    }
    certificate.preemption_result = preemption_result;
    certificate.backend_stop_confirmed = preempt_accepted;
    certificate.observed_state_at_rest = false;
    certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
    certificate.completed_at_ns = clock_.now();
    certificate.command_source = "fault_preempt_worker";
    certificate.terminal_error = preempt_error;
    publish_stop_certificate(std::move(certificate));
  }
  mark_interrupt_preemption_result(latch, preemption_result, preempt_event_details);
  // This function runs both from the fault worker (no lifecycle_mutex_) and
  // inline from acknowledgement paths that DO hold it; std::mutex try_lock
  // by the owner is UB, so only mark dirty here. The worker lambda and the
  // tick-side ingest tail (never lifecycle owners) perform the try-refresh;
  // acknowledgement paths refresh via their own locked call.
  recovery_assessment_dirty_.store(true, std::memory_order_release);
  if (preempt_error && (force_operator_intervention || !preempt_accepted)) {
    sm_.set_last_error(error_code_from_dto(*preempt_error));
    const auto prev_state = sm_.status().state;
    ExecutionState snapshot_state = ExecutionState::kInterrupted;
    if (force_operator_intervention) {
      auto terminal_commit = commit_terminal(ExecutionState::kInterventionRequired,
                                             preempt_error->details.at("reason").c_str(),
                                             TerminalCommitter::Origin::kStopAuthority);
      snapshot_state = terminal_commit.committed;
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = snapshot_state;
    snapshot.error = preempt_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                preempt_error,
                preempt_error->details});
    if (force_operator_intervention && snapshot_state != prev_state) {
      emit_event({KernelEventType::kStateTransition, sm_.status().execution_id, clock_.now(),
                  prev_state, snapshot_state, std::nullopt, preempt_error->details});
    }
  }
}

bool ExecutionKernel::request_fault_preempt_controlled_stop() {
  std::shared_ptr<std::stop_source> source;
  {
    std::lock_guard<std::mutex> lk(fault_preempt_stop_mutex_);
    source = fault_preempt_stop_source_;
  }
  if (source) {
    return source->request_stop();
  }
  return false;
}

InterruptRuntimeContext ExecutionKernel::current_interrupt_context() const {
  InterruptRuntimeContext context;
  const auto status = sm_.status();
  const auto phase = interrupt_phase_from_state(status.state);
  context.phase = phase.value_or(InterruptExecutionPhase::kHeld);
  if (status.execution_id != 0) {
    context.execution_id = status.execution_id;
  }
  if (auto move_id = active_correlation_.move_id(); !move_id.empty()) {
    context.move_id = std::move(move_id);
  }
  return context;
}

void ExecutionKernel::stamp_interrupt_latch_collision_world_revision(
    InterruptIngestResult& result) {
  if (!result.latched || !result.active_latch ||
      result.active_latch->borrowed_collision_world_revision.has_value()) {
    return;
  }
  const auto revision = current_authoritative_collision_world_revision();
  if (!collision_world_revision_is_set(revision)) {
    return;
  }
  const auto latch_id = result.active_latch->latch_id;
  if (!interrupt_manager_.stamp_active_latch_collision_world_revision(latch_id, revision)) {
    return;
  }
  result.active_latch->borrowed_collision_world_revision = revision;
  for (auto& event : result.events) {
    if (event.latch && event.latch->latch_id == latch_id) {
      event.latch->borrowed_collision_world_revision = revision;
    }
  }
}

void ExecutionKernel::emit_interrupt_events(const InterruptIngestResult& result) {
  for (const auto& event : result.events) {
    auto event_type = interrupt_event_type_from_name(event.type);
    if (!event_type) {
      continue;
    }
    emit_event({*event_type,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                project_interrupt_structured_value_to_text(event.details)});
  }
}

void ExecutionKernel::emit_interrupt_manager_events(
    const std::vector<InterruptManagerEvent>& events, std::uint64_t execution_id) {
  for (const auto& event : events) {
    auto event_type = interrupt_event_type_from_name(event.type);
    if (!event_type) {
      continue;
    }
    emit_event({*event_type,
                execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                project_interrupt_structured_value_to_text(event.details)});
  }
}

namespace {

const char* terminal_cause_string(MotionExecutionTerminalCause cause) {
  switch (cause) {
    using enum MotionExecutionTerminalCause;
    case kCompletedNormally:
      return "completed_normally";
    case kSignalTrip:
      return "signal_trip";
    case kNoSignalTrip:
      return "no_signal_trip";
    case kStopped:
      return "stopped";
    case kFailed:
      return "failed";
    case kInterruptedFault:
      return "interrupted_fault";
  }
  return "unknown";
}

}  // namespace

Result<void> ExecutionKernel::arm_motion_scopes_for_dispatch(const ExecuteRequestDTO& request,
                                                             std::uint64_t execution_id,
                                                             ActiveMotionExecution& motion_state) {
  motion_state.execution_id = execution_id;
  motion_state.move_id = request.move_id;
  motion_state.had_scopes = !request.motion_interrupt_scopes.empty();
  motion_state.arm_succeeded = false;
  motion_state.motion_trip_latch.reset();
  motion_state.outcome_emitted = false;
  const bool has_configured_stop_profiles =
      config_.operator_stop_profile || config_.default_fault_stop_profile;
  if (!motion_state.had_scopes && !has_configured_stop_profiles) {
    return Result<void>::Ok();
  }
  // Spec/260 §5.6 dual-boundary capability validation:
  // pre-dispatch backend-capability check. The InterruptManager
  // validates policy-level constraints (allowed_stop_modes,
  // node-category fallback rules, stop_profile presence). The
  // backend's controlled-stop capability is the second independent gate: the
  // requested stop_mode and sole qualified recoverable substitution MUST be
  // in the backend's supported_stop_modes set. Trip-time validation catches
  // live-controller divergence; this gate stops the request
  // before arming if the backend has not declared support.
  const auto capability = execution_backend_.get_motion_interrupt_stop_capability();
  const auto validate_profile_against_capability =
      [&capability](const MotionInterruptStopProfileDTO& profile, const std::string& profile_slot,
                    const std::string& scope_id,
                    const std::string& policy_id) -> std::optional<ErrorDTO> {
    if (!capability.supported_stop_modes.contains(profile.stop_mode)) {
      std::map<std::string, std::string> details{
          {"reason", "stop_mode_not_supported_by_backend"},
          {"profile_slot", profile_slot},
          {"requested_stop_mode", stop_mode_string(profile.stop_mode)}};
      if (!scope_id.empty()) {
        details["scope_id"] = scope_id;
      }
      if (!policy_id.empty()) {
        details["policy_id"] = policy_id;
      }
      return ErrorDTO{kErrorMotionInterruptArmFailed,
                      "configured stop_profile stop_mode is not in backend "
                      "supported_stop_modes",
                      std::move(details)};
    }
    if (profile.recoverable_substitution_mode.has_value() &&
        !capability.supported_stop_modes.contains(*profile.recoverable_substitution_mode)) {
      std::map<std::string, std::string> details{
          {"reason", "substitution_mode_not_supported_by_backend"},
          {"profile_slot", profile_slot},
          {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
          {"substitution_stop_mode", stop_mode_string(*profile.recoverable_substitution_mode)}};
      if (!scope_id.empty()) {
        details["scope_id"] = scope_id;
      }
      if (!policy_id.empty()) {
        details["policy_id"] = policy_id;
      }
      return ErrorDTO{kErrorMotionInterruptArmFailed,
                      "configured stop_profile substitution is not in backend "
                      "supported_stop_modes",
                      std::move(details)};
    }
    return std::nullopt;
  };
  if (config_.operator_stop_profile) {
    if (auto error = validate_profile_against_capability(*config_.operator_stop_profile,
                                                         "operator_stop_profile", "", "")) {
      return Result<void>::Err(*error);
    }
  }
  if (config_.default_fault_stop_profile) {
    if (auto error = validate_profile_against_capability(*config_.default_fault_stop_profile,
                                                         "default_fault_stop_profile", "", "")) {
      return Result<void>::Err(*error);
    }
  }
  if (!motion_state.had_scopes) {
    return Result<void>::Ok();
  }
  for (const auto& scope : request.motion_interrupt_scopes) {
    if (!scope.stop_profile) {
      // Manager will reject this with stop_profile_required if the
      // bound policy is complete_move_on_signal; otherwise the
      // backend gate has nothing to check.
      continue;
    }
    const auto& profile = *scope.stop_profile;
    if (auto error = validate_profile_against_capability(profile, "motion_interrupt_scope",
                                                         scope.scope_id, scope.policy_id)) {
      return Result<void>::Err(*error);
    }
    // Arm-time soft_on_path feasibility pre-check removed: the feasibility +
    // fallback disposition (hard_stop / spring_back / natural completion) is
    // decided at TRIP time by plan_soft_on_path_stop, which both backends now
    // honor. Arm time keeps only the backend-capability gate (above) + profile
    // well-formedness; per-emitted-trajectory accel/jerk consistency is checked
    // when the stop trajectory is generated, not pre-proven against every trip
    // point here.
  }
  auto arm = interrupt_manager_.arm_motion_interrupt_scopes(
      execution_id, request.move_id, request.motion_interrupt_scopes, clock_.now());
  if (!arm.ok()) {
    // Promote the manager's `details["reason"]` (e.g.
    // `motion_interrupt_arm_failed`, `motion_interrupt_source_active_at_arm`)
    // to the canonical ErrorDTO.code. The manager wraps every error as
    // `invalid_request` for legacy callers, but spec/210 §5.2 treats the
    // motion-interrupt identifiers as first-class error codes. Without
    // this promotion, `error_code_from_dto` collapses every arm failure
    // to `kInvalidRequest` and the state-machine surface loses fidelity.
    ErrorDTO err = arm.error();
    if (err.details.contains("reason")) {
      const auto& reason = err.details.at("reason");
      if (reason.rfind("motion_interrupt_", 0) == 0) {
        err.code = reason;
      }
    }
    emit_event(
        {KernelEventType::kMotionInterruptArmFailed,
         execution_id,
         clock_.now(),
         {},
         {},
         err,
         {{"move_id", request.move_id},
          {"reason", err.details.contains("reason") ? err.details.at("reason") : err.code}}});
    return Result<void>::Err(std::move(err));
  }
  // Surface scope_armed events; the manager has already validated each
  // binding atomically. If arming itself tripped a scope, route the
  // returned latch through the same side-effect helper as signal ingest.
  auto arm_result = std::move(arm).value();
  if (arm_result.latched) {
    stamp_interrupt_latch_collision_world_revision(arm_result);
    apply_interrupt_ingest_effects(arm_result,
                                   /*apply_preemption_inline=*/true);
  } else {
    emit_interrupt_manager_events(arm_result.events, execution_id);
  }
  motion_state.arm_succeeded = true;
  return Result<void>::Ok();
}

void ExecutionKernel::finalize_motion_execution_outcome(ActiveMotionExecution& motion_state,
                                                        MotionExecutionTerminalCause terminal_cause,
                                                        std::optional<ErrorDTO> error,
                                                        std::optional<MotionTripOutcomeDTO> trip,
                                                        std::string reap_reason) {
  if (motion_state.outcome_emitted || motion_state.execution_id == 0) {
    return;
  }
  motion_state.outcome_emitted = true;

  // Reap any armed bindings for this execution before publishing the
  // outcome. Reaping always runs even if no scopes were carried —
  // cheap when the binding map is empty and keeps the invariant that
  // every execution_id terminates with a clean binding table.
  if (motion_state.arm_succeeded) {
    auto reap = interrupt_manager_.reap_motion_interrupt_scopes(motion_state.execution_id,
                                                                reap_reason, clock_.now());
    emit_interrupt_manager_events(reap.events, motion_state.execution_id);
  }

  // Invariant enforcement (spec/210 §3.11). The kernel publishes one
  // immutable outcome per terminal execution; mis-shaped requests are
  // coerced to the closest valid taxonomy AND logged via a kErrorRaised
  // event so the operator surface sees the coercion in audit.
  MotionExecutionOutcomeDTO outcome;
  outcome.execution_id = motion_state.execution_id;
  outcome.move_id = motion_state.move_id;
  if (motion_state.correlation_captured) {
    outcome.action_span = motion_state.action_span;
  } else {
    outcome.action_span = active_correlation_.action_span();
  }
  if (motion_state.correlation_captured) {
    outcome.contact_policy_id = motion_state.contact_policy_id;
  } else {
    outcome.contact_policy_id = runtime_collision_monitor_.active_contact_policy_id();
  }
  outcome.terminal_cause = terminal_cause;

  const auto coerce = [&](MotionExecutionTerminalCause to, const char* reason) {
    emit_event(make_error_raised_event(
        motion_state.execution_id, clock_.now(),
        ErrorDTO{kErrorInvariantViolation, "motion outcome coerced", {{"reason", reason}}},
        {{"from", terminal_cause_string(outcome.terminal_cause)},
         {"to", terminal_cause_string(to)}}));
    outcome.terminal_cause = to;
  };

  switch (outcome.terminal_cause) {
    using enum MotionExecutionTerminalCause;
    case kSignalTrip:
      if (!trip || error) {
        coerce(kFailed, "signal_trip_requires_trip_and_forbids_error");
      }
      break;
    case kNoSignalTrip:
      if (!motion_state.had_scopes) {
        coerce(kCompletedNormally, "no_signal_trip_requires_scopes");
      }
      if (trip || error) {
        coerce(kFailed, "no_signal_trip_forbids_trip_and_error");
      }
      break;
    case kCompletedNormally:
      if (motion_state.had_scopes) {
        // Reached endpoint with scopes but no trip → no_signal_trip.
        outcome.terminal_cause = kNoSignalTrip;
      }
      if (trip || error) {
        coerce(kFailed, "completed_normally_forbids_trip_and_error");
      }
      break;
    case kStopped:
    case kInterruptedFault:
      if (trip || error) {
        coerce(kFailed, "terminal_cause_forbids_trip_and_error");
      }
      break;
    case kFailed:
      if (!error) {
        error = ErrorDTO{kErrorInternalError,
                         "Motion execution failed without a captured error",
                         {{"reason", "missing_error_on_failed_outcome"}}};
      }
      break;
  }

  if (outcome.terminal_cause == MotionExecutionTerminalCause::kSignalTrip) {
    outcome.trip = std::move(trip);
  } else if (outcome.terminal_cause == MotionExecutionTerminalCause::kFailed) {
    outcome.error = std::move(error);
  }

  std::map<std::string, std::string> details{
      {"execution_id", std::to_string(outcome.execution_id)},
      {"move_id", outcome.move_id},
      {"terminal_cause", terminal_cause_string(outcome.terminal_cause)},
      {"had_scopes", motion_state.had_scopes ? "true" : "false"},
      {"arm_succeeded", motion_state.arm_succeeded ? "true" : "false"},
  };
  if (outcome.trip) {
    details.emplace("latch_id", outcome.trip->latch_id);
    details.emplace("source_id", outcome.trip->source_id);
    details.emplace("policy_id", outcome.trip->policy_id);
    details.emplace("scope_id", outcome.trip->scope_id);
  }
  if (outcome.action_span) {
    const auto& span = *outcome.action_span;
    details.emplace("action_source", to_string(span.action_source));
    details.emplace("action_span_id", span.action_span_id);
    if (!span.program_run_id.empty()) {
      details.emplace("action_span_program_run_id", span.program_run_id);
    }
    if (!span.node_instance_id.empty()) {
      details.emplace("node_instance_id", span.node_instance_id);
    }
    if (!span.node_attempt_id.empty()) {
      details.emplace("node_attempt_id", span.node_attempt_id);
    }
    if (!span.parent_node_attempt_id.empty()) {
      details.emplace("parent_node_attempt_id", span.parent_node_attempt_id);
    }
  }
  if (!outcome.contact_policy_id.empty()) {
    details.emplace("contact_policy_id", outcome.contact_policy_id);
  }
  if (outcome.error) {
    details.emplace("error_code", outcome.error->code);
    if (outcome.error->details.contains("reason")) {
      details.emplace("error_reason", outcome.error->details.at("reason"));
    }
  }

  const auto outcome_execution_id = outcome.execution_id;
  auto outcome_error = outcome.error;

  // Store and evict FIFO when over the retention bound.
  {
    std::lock_guard<std::mutex> lock(motion_outcomes_mutex_);
    motion_execution_outcomes_.insert_or_assign(outcome_execution_id, std::move(outcome));
    motion_outcome_order_.push_back(outcome_execution_id);
    while (motion_outcome_order_.size() > config_.motion_execution_outcome_retention) {
      const auto evict_id = motion_outcome_order_.front();
      motion_outcome_order_.erase(motion_outcome_order_.begin());
      motion_execution_outcomes_.erase(evict_id);
    }
  }

  emit_event({KernelEventType::kMotionExecutionOutcomeRecorded,
              outcome_execution_id,
              clock_.now(),
              {},
              {},
              std::move(outcome_error),
              std::move(details)});
}

void ExecutionKernel::handle_motion_trip_latch(const ExecuteRequestDTO& request,
                                               const InterruptLatch& latch, std::uint64_t exec_id,
                                               const ExecutionSnapshot& /*latest_backend_snapshot*/,
                                               ActiveMotionExecution& motion_state,
                                               std::stop_token worker_stop_token) {
  const auto motion_trip_stop_generation =
      motion_trip_stop_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  // Locate the originating scope so we can recover the
  // MotionInterruptStopProfileDTO and node_category. The latch carries
  // the scope id; the request carries the scope vector. A trip latch
  // without a matching scope is a manager invariant violation and is
  // treated as a backend stop failure (intervention path: the robot
  // state is uncertain).
  motion_state.motion_trip_latch = latch;
  const MotionInterruptScopeDTO* matching_scope = nullptr;
  if (latch.motion_interrupt_scope_id) {
    for (const auto& s : request.motion_interrupt_scopes) {
      if (s.scope_id == *latch.motion_interrupt_scope_id) {
        matching_scope = &s;
        break;
      }
    }
  }
  if (!matching_scope || !matching_scope->stop_profile) {
    const auto prev_state_trip = sm_.status().state;
    sm_.set_last_error(ErrorCode::kMotionInterruptBackendStopFailed);
    if (prev_state_trip == ExecutionState::kInterrupted) {
      // Phase hop toward the controlled stop; if it is not admissible the
      // InterventionRequired commit below supersedes it either way.
      (void)sm_.try_transition(ExecutionState::kStopping);
    }
    ErrorDTO err{
        kErrorMotionInterruptBackendStopFailed,
        "complete_move_on_signal latch missing matching scope/stop_profile",
        {{"reason", "motion_interrupt_scope_lookup_failed"}, {"latch_id", latch.latch_id}}};
    KernelStopCertificateDTO certificate;
    certificate.completion_result = StopCertificateOutcome::kBackendPreemptFailed;
    certificate.stop_owner = StopOwnerKind::kMotionTripStop;
    certificate.stop_owner_generation = motion_trip_stop_generation;
    certificate.execution_id = exec_id;
    certificate.latch_id = latch.latch_id;
    certificate.control_command_epoch = control_command_epoch();
    certificate.command_source = "motion_trip_scope_lookup_failed";
    certificate.preemption_result = InterruptPreemptionResult::kBackendPreemptFailed;
    certificate.completed_at_ns = clock_.now();
    certificate.terminal_error = err;
    (void)publish_stop_certificate(std::move(certificate));
    commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                    TerminalCommitter::Origin::kMotionTrip);
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, err, {}});
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state_trip,
                ExecutionState::kInterventionRequired,
                std::nullopt,
                {}});
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      std::move(err), std::nullopt,
                                      "motion_trip_scope_lookup_failed");
    // Latch persists for operator intervention-acknowledge; do NOT
    // auto-clear on this failure path.
    return;
  }
  const auto& profile = *matching_scope->stop_profile;
  const auto node_category = matching_scope->node_category;

  // Move kernel to Stopping while the controlled stop is in flight.
  // The latch remains visible; spec/230 §3.4 forbids parking in
  // Interrupted during the controlled-stop window.
  const auto prev_state_trip = sm_.status().state;
  if (prev_state_trip == ExecutionState::kInterrupted) {
    auto t = sm_.try_transition(ExecutionState::kStopping);
    if (t.ok()) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev_state_trip,
                  ExecutionState::kStopping,
                  std::nullopt,
                  {{"reason", "motion_interrupt_trip_controlled_stop"}}});
    }
  }

  // Issue the controlled stop. The backend performs the stop and
  // reports what it actually did. Timing is captured around the call
  // so the kernel can validate against the effective Kernel Stop envelope.
  // This transitional path derives it from `deceleration_time_s` (defaulted)
  // plus the deployment stop-confirmation tolerance. Embed
  // latch->call and call->confirm latencies in the trip outcome.
  // Keep the runtime collision monitor running over the decelerating robot for
  // the duration of the stop. A collision on the stop path signals the trip
  // handler (via the stop token) to escalate to a hard stop. The monitor is
  // observed-state based, so this is the same monitoring a normal motion gets;
  // the only difference is the stop trajectory is not pre-validated (it stays
  // on the already-validated path).
  const std::int64_t effective_deadline_ns =
      effective_controlled_stop_deadline_ns(profile, config_.complete_move_on_signal_timeout_ns);

  std::stop_source trip_stop_source;
  std::stop_callback worker_stop_bridge(worker_stop_token,
                                        [&trip_stop_source]() { trip_stop_source.request_stop(); });
  motion_trip_stop_.begin(trip_stop_source);
  const auto motion_trip_owner_scope = std::shared_ptr<void>(nullptr, [this](void*) {
    // Release the stop owner only after this handler has published its
    // definitive certificate and terminal outcome. Admission must never
    // observe owner-inactive before the matching evidence exists.
    (void)motion_trip_stop_.finish();
  });
  const auto trip_runtime_collision_generation = start_collision_monitoring(
      "motion_trip_controlled_stop", RuntimeCollisionProtectionPurpose::kControlledStop);
  begin_runtime_velocity_scale_freeze();
  const auto runtime_scale_freeze_scope =
      std::shared_ptr<void>(nullptr, [this](void*) { end_runtime_velocity_scale_freeze(); });
  auto hard_stop_floor_attempt = std::make_shared<HardStopFloorAttempt>();
  auto deadline_watchdog_fired = std::make_shared<std::atomic_bool>(false);
  auto stop_session_complete = std::make_shared<std::atomic_bool>(false);

  // Kernel-enforced stop deadline. If the backend blocks past the effective
  // deadline, the Monitoring lane requests cancellation AND issues the
  // hard-stop floor.
  // The worker still owns final state/outcome publication when the backend
  // returns, but the physical hard-stop command is not delayed by that return.
  ScheduleHandle stop_deadline_handle = 0;
  if (effective_deadline_ns > 0) {
    const auto requested_mode = std::string(stop_mode_string(profile.stop_mode));
    const auto latch_id = latch.latch_id;
    const auto source_id = latch.source_id;
    const auto policy_id = latch.policy_id;
    const auto scope_id = latch.motion_interrupt_scope_id.value_or("");
    stop_deadline_handle = scheduler_.schedule(
        clock_.now() + effective_deadline_ns,
        [this, hard_stop_floor_attempt, requested_mode, latch_id, source_id, policy_id, scope_id,
         deadline_watchdog_fired, stop_session_complete]() {
          if (stop_session_complete->load(std::memory_order_acquire)) {
            return;
          }
          deadline_watchdog_fired->store(true, std::memory_order_release);
          const auto legacy =
              request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
          motion_trip_stop_.request_stop();
          const auto fallback_outcome = legacy.accepted ? std::string("legacy_stop_succeeded")
                                                        : std::string("legacy_stop_failed");
          std::map<std::string, std::string> details{{"reason", "in_stop_completion_timeout"},
                                                     {"phase", "motion_trip_deadline_watchdog"},
                                                     {"requested_stop_mode", requested_mode},
                                                     {"latch_id", latch_id},
                                                     {"source_id", source_id},
                                                     {"policy_id", policy_id},
                                                     {"scope_id", scope_id},
                                                     {"emergency_fallback", fallback_outcome},
                                                     {"hard_stop_transitioned", "true"}};
          emit_event({KernelEventType::kErrorRaised,
                      sm_.status().execution_id,
                      clock_.now(),
                      {},
                      {},
                      ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                               "motion-scoped controlled stop exceeded "
                               "effective stop deadline; emergency hard-stop "
                               "floor issued by deadline watchdog",
                               details},
                      details});
        },
        SchedulerPriority::StopControl);
  }

  const auto call_start_ns = clock_.now();
  const auto stop_wall_start = std::chrono::steady_clock::now();
  const auto stop_wall_deadline =
      effective_deadline_ns > 0 ? stop_wall_start + std::chrono::nanoseconds(effective_deadline_ns)
                                : stop_wall_start;
  auto stop_result =
      execution_backend_.execute_controlled_stop(profile, trip_stop_source.get_token());
  fail_closed_invalid_controlled_stop_result(profile, stop_result);
  const auto call_end_ns = clock_.now();
  stop_session_complete->store(true, std::memory_order_release);
  const bool deadline_watchdog_expired = deadline_watchdog_fired->load(std::memory_order_acquire);
  if (deadline_watchdog_expired) {
    stop_result.backend_stop_confirmed = false;
    stop_result.error = ErrorDTO{
        std::string(kErrorMotionInterruptBackendStopFailed),
        "motion-scoped controlled stop exceeded the effective stop deadline; "
        "emergency hard-stop floor was issued by the deadline watchdog",
        {{"reason", "in_stop_completion_timeout"}, {"phase", "motion_trip_deadline_watchdog"}}};
  }

  if (stop_deadline_handle != 0) {
    scheduler_.cancel(stop_deadline_handle);
  }
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced,
                            trip_runtime_collision_generation);
  const bool stop_path_collision = motion_trip_stop_.collision_detected();

  // Common failure-path downgrader. Industrial behavior (spec/230
  // §3.4 lines 563-564): backend-stop and stop-snapshot failures
  // transition to `InterventionRequired`, not `Failed`. The latch
  // persists for operator/runtime intervention-acknowledge; it is
  // NOT auto-cleared on the failure paths. The outcome record's
  // `terminal_cause` is still `failed` per spec/210 §3.11.
  const auto downgrade_to_intervention =
      [&](std::string reason, std::string message,
          std::string error_code = std::string(kErrorMotionInterruptBackendStopFailed),
          std::map<std::string, std::string> extra_details = {}) {
        ErrorDTO err{std::move(error_code),
                     std::move(message),
                     {{"reason", std::move(reason)},
                      {"latch_id", latch.latch_id},
                      {"source_id", latch.source_id},
                      {"policy_id", latch.policy_id},
                      {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
                      {"executed_stop_mode", stop_mode_string(stop_result.executed_stop_mode)}}};
        // Escalation evidence (e.g. hard_stop_transitioned) must ride the
        // terminal outcome, not only the transient event stream, so industrial
        // audit can reconstruct the disposition from the durable record alone.
        for (auto& [k, v] : extra_details) {
          err.details[k] = std::move(v);
        }
        KernelStopCertificateDTO certificate;
        certificate.completion_result = stop_result.backend_stop_confirmed
                                            ? StopCertificateOutcome::kPartial
                                            : StopCertificateOutcome::kBackendPreemptFailed;
        certificate.stop_owner = StopOwnerKind::kMotionTripStop;
        certificate.stop_owner_generation = motion_trip_stop_generation;
        certificate.execution_id = exec_id;
        certificate.latch_id = latch.latch_id;
        certificate.control_command_epoch = control_command_epoch();
        certificate.requested_stop_mode = stop_mode_string(profile.stop_mode);
        certificate.executed_stop_mode = stop_mode_string(stop_result.executed_stop_mode);
        certificate.command_source = stop_command_source_string(stop_result.stop_command_source);
        certificate.preemption_result = stop_result.backend_stop_confirmed
                                            ? InterruptPreemptionResult::kPartial
                                            : InterruptPreemptionResult::kBackendPreemptFailed;
        certificate.backend_stop_confirmed = stop_result.backend_stop_confirmed;
        certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
        certificate.completed_at_ns = clock_.now();
        certificate.terminal_error = err;
        (void)publish_stop_certificate(std::move(certificate));
        sm_.set_last_error(err.code == kErrorMotionInterruptStopStateInvalid
                               ? ErrorCode::kMotionInterruptStopStateInvalid
                               : ErrorCode::kMotionInterruptBackendStopFailed);
        const auto prev = sm_.status().state;
        commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit",
                        TerminalCommitter::Origin::kMotionTrip);
        emit_event(
            {KernelEventType::kMotionTripFailed, exec_id, clock_.now(), {}, {}, err, err.details});
        emit_event({KernelEventType::kStateTransition,
                    exec_id,
                    clock_.now(),
                    prev,
                    ExecutionState::kInterventionRequired,
                    std::nullopt,
                    {}});
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                          std::move(err), std::nullopt, "motion_trip_failed");
        // Latch persists. Do NOT auto-clear.
      };

  // Build a fully self-describing escalation detail map for the scoped
  // motion-trip failure/escalation paths. Carries the controlled-stop
  // error code/reason and the whole soft_on_path/fallback diagnostic
  // surface so escalation events on the event stream describe WHY the
  // requested stop was refused or escalated — not only the fact that an
  // escalation happened. Mirrors the operator-stop worker enrichment.
  const auto build_escalation_details = [&](std::string reason, std::string phase) {
    std::map<std::string, std::string> details{
        {"reason", std::move(reason)},
        {"phase", std::move(phase)},
        {"requested_stop_mode", stop_mode_string(profile.stop_mode)},
        {"executed_stop_mode", stop_mode_string(stop_result.executed_stop_mode)},
        {"latch_id", latch.latch_id},
        {"source_id", latch.source_id},
        {"policy_id", latch.policy_id},
        {"scope_id", latch.motion_interrupt_scope_id.value_or("")}};
    if (stop_result.error.has_value()) {
      details["controlled_stop_error_code"] = stop_result.error->code;
      const auto reason_it = stop_result.error->details.find("reason");
      if (reason_it != stop_result.error->details.end()) {
        details["controlled_stop_error_reason"] = reason_it->second;
      }
    }
    for (const auto& [k, v] : stop_result.diagnostics) {
      if (k.rfind("soft_on_path", 0) == 0 || k.rfind("fallback", 0) == 0 ||
          k.rfind("hard_stop", 0) == 0) {
        details[k] = interrupt_structured_scalar_text(v);
      }
    }
    return details;
  };

  // First-class hard-stop floor for the scoped motion-trip path. The legacy
  // `backend.stop()` IS the hard stop the escalation ladder calls for when a
  // controlled stop is refused, not confirmed within the deadline, or did not
  // reach measured rest. The escalation is marked `hard_stop_transitioned` so
  // the monitor/audit stream sees it as a hard-stop transition, not a generic
  // failure. Returns the durable escalation evidence (hard_stop_transitioned,
  // emergency_fallback) so the caller can fold it into the terminal outcome.
  const auto emergency_stop_floor = [&](std::string reason,
                                        std::string phase) -> std::map<std::string, std::string> {
    const auto legacy = request_hard_stop_floor_once(execution_backend_, hard_stop_floor_attempt);
    auto details = build_escalation_details(std::move(reason), std::move(phase));
    details["emergency_fallback"] =
        legacy.accepted ? "legacy_stop_succeeded" : "legacy_stop_failed";
    details["hard_stop_transitioned"] = "true";
    add_hard_stop_floor_details(details, execution_backend_);
    emit_event(
        {KernelEventType::kErrorRaised,
         exec_id,
         clock_.now(),
         {},
         {},
         std::optional<ErrorDTO>{ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                          "scoped motion-trip controlled stop escalated to the "
                                          "emergency hard-stop floor",
                                          details}},
         details});
    std::map<std::string, std::string> evidence{
        {"hard_stop_transitioned", "true"}, {"emergency_fallback", details["emergency_fallback"]}};
    evidence["hard_stop_level"] = details["hard_stop_level"];
    evidence["hard_stop_floor"] = details["hard_stop_floor"];
    evidence["hard_stop_floor_command_source"] = details["hard_stop_floor_command_source"];
    return evidence;
  };

  // Stop-path collision escalation (checked first). A dynamic obstacle entered
  // the stop path during the controlled stop, so the runtime monitor cancelled
  // the soft stop mid-ramp — the robot may still be moving toward the obstacle.
  // Escalate to the hard-stop floor (first-class) and hold for operator
  // acknowledgment.
  if (stop_path_collision) {
    auto floor_evidence =
        emergency_stop_floor("stop_path_collision", "motion_trip_stop_path_collision");
    downgrade_to_intervention("stop_path_collision",
                              "collision detected on the stop path during the controlled stop; "
                              "escalated to a hard stop",
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(floor_evidence));
    return;
  }

  // Short-path policy: a generic soft_on_path scope may
  // explicitly allow the original trajectory to complete naturally
  // when a useful wall-time replacement ramp would overshoot the
  // accepted endpoint. This is NOT a successful controlled stop; the
  // terminal outcome remains no_signal_trip semantics after the
  // backend proves the original motion is terminal. Precision-contact
  // nodes and profiles without the explicit policy fail closed.
  if (stop_result.completed_by_natural_completion) {
    if (profile.stop_mode != MotionInterruptStopMode::kSoftOnPath ||
        !profile.allow_natural_completion_on_short_path ||
        node_category == MotionInterruptNodeCategory::kPrecisionContact || !stop_result.accepted ||
        stop_result.error.has_value()) {
      downgrade_to_intervention("natural_completion_not_allowed_for_scope",
                                "backend reported natural completion for a scope that cannot "
                                "accept short-path natural completion");
      return;
    }
    if (effective_deadline_ns > 0 && stop_result.confirmation_latency_ns > effective_deadline_ns) {
      downgrade_to_intervention("natural_completion_exceeded_stop_deadline",
                                "soft_on_path natural completion exceeded the "
                                "effective stop deadline");
      return;
    }

    auto completion_transition = commit_terminal(ExecutionState::kCompleted, "terminal_commit",
                                                 TerminalCommitter::Origin::kMotionTrip);
    if (!completion_transition.ok()) {
      downgrade_to_intervention("natural_completion_completed_transition_rejected",
                                "soft_on_path natural completion could not commit Completed as "
                                "the terminal authority");
      return;
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kStopping,
                ExecutionState::kCompleted,
                std::nullopt,
                {{"reason", "motion_trip_short_path_natural_completion"},
                 {"latch_id", latch.latch_id},
                 {"scope_id", latch.motion_interrupt_scope_id.value_or("")},
                 {"requested_stop_mode", stop_mode_string(profile.stop_mode)}}});
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kNoSignalTrip,
                                      std::nullopt, std::nullopt, "motion_trip_natural_completion");
    auto clear =
        interrupt_manager_.clear_active_latch("auto_clear_after_motion_outcome", clock_.now());
    if (clear.ok()) {
      record_terminal_fault_authority(clear.value());
    }
    return;
  }

  // (1) Backend accepted the call. A refused controlled stop means the
  //     robot may still be in motion, so engage the emergency hard-stop
  //     floor before downgrading to intervention.
  if (!stop_result.accepted) {
    auto reason = stop_result.error && stop_result.error->details.contains("reason")
                      ? stop_result.error->details.at("reason")
                      : std::string("controlled_stop_not_accepted");
    auto msg = stop_result.error ? stop_result.error->message
                                 : "execute_controlled_stop returned accepted=false";
    auto floor_evidence = emergency_stop_floor("scoped_trip_controlled_stop_refused",
                                               "motion_trip_controlled_stop_refused");
    if (stop_result.error) {
      const auto validation = stop_result.error->details.find("validation_reason");
      if (validation != stop_result.error->details.end()) {
        floor_evidence["validation_reason"] = validation->second;
      }
    }
    downgrade_to_intervention(std::move(reason), std::move(msg),
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(floor_evidence));
    return;
  }
  // (2) Backend confirmed the stop within the effective stop deadline. An
  //     unconfirmed stop means the robot may still be in motion: engage the
  //     emergency hard-stop floor before downgrading.
  if (!stop_result.backend_stop_confirmed) {
    // In-stop completion timeout: the controlled stop did not reach confirmed
    // rest within the Kernel Stop envelope. First-class escalation to the hard-stop
    // floor (the robot may still be moving), then intervention.
    auto floor_evidence = emergency_stop_floor("in_stop_completion_timeout",
                                               "motion_trip_controlled_stop_unconfirmed");
    downgrade_to_intervention("backend_stop_not_confirmed",
                              "backend did not confirm controlled stop within the effective stop "
                              "deadline",
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(floor_evidence));
    return;
  }
  if (effective_deadline_ns > 0 && stop_result.confirmation_latency_ns > effective_deadline_ns) {
    downgrade_to_intervention("backend_stop_confirmation_exceeded_stop_deadline",
                              "controlled-stop confirmation exceeded the effective stop deadline");
    return;
  }
  const double settle_velocity_threshold_rad_s = kDefaultSettleVelocityThresholdRadS;
  const auto settle_result = wait_for_measured_rest_after_confirmed_stop(
      execution_backend_, settle_velocity_threshold_rad_s, stop_wall_deadline,
      trip_stop_source.get_token());
  if (settle_result.settle_signal_available && !settle_result.at_rest) {
    std::map<std::string, std::string> details{
        {"hard_stop_transitioned", "false"},
        {"emergency_fallback", "not_attempted_controlled_stop_already_confirmed"},
        {"settle_velocity_threshold_rad_s", std::to_string(settle_velocity_threshold_rad_s)},
        {"settle_wait_timed_out", settle_result.timed_out ? "true" : "false"}};
    if (settle_result.max_abs_velocity_rad_s) {
      details["measured_max_abs_velocity_rad_s"] =
          std::to_string(*settle_result.max_abs_velocity_rad_s);
    }
    downgrade_to_intervention("stop_not_settled_measured_velocity",
                              "backend confirmed the stop but measured velocity did not settle "
                              "within the effective stop deadline; redundant hard-stop cancel was "
                              "not attempted",
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(details));
    return;
  }
  // (3) Executed mode matches requested OR is an allowed fallback.
  const StopFallbackQualificationRow* fallback_qualification_row = nullptr;
  if (stop_result.executed_stop_mode != profile.stop_mode) {
    // Explicit hard-stop escalation. When soft_on_path is infeasible the
    // backend escalates to a hard deceleration: a fail-closed,
    // operator-visible intervention — NOT a silent qualified fallback
    // like spring_back_hold. The robot is at rest here (past the
    // accepted+confirmed gates), so no emergency floor is needed, but
    // this escalation is encoded and traced explicitly instead of
    // falling through to the generic "fallback_pair_not_qualified"
    // downgrade.
    //
    // Disposition decision: an executed stop more aggressive than the
    // requested mode is HELD for operator acknowledgment (intervention),
    // never auto-cleared as a qualified-success outcome. An escalation
    // past the requested stop mode must not be silently accepted, so it
    // is not promoted to an auto-clearing outcome (with settle-signal
    // confirmation and a resume policy) on this path.
    if (profile.stop_mode == MotionInterruptStopMode::kSoftOnPath &&
        stop_result.executed_stop_mode == MotionInterruptStopMode::kHardDeceleration) {
      auto details = build_escalation_details("soft_on_path_escalated_to_hard_deceleration",
                                              "motion_trip_hard_decel_escalation");
      emit_event(
          {KernelEventType::kErrorRaised,
           exec_id,
           clock_.now(),
           {},
           {},
           std::optional<ErrorDTO>{ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                                            "soft_on_path stop infeasible; backend escalated to a "
                                            "hard deceleration (operator intervention required)",
                                            details}},
           details});
      downgrade_to_intervention("soft_on_path_escalated_to_hard_deceleration",
                                "soft_on_path stop infeasible; backend escalated to a hard "
                                "deceleration (operator intervention required)");
      return;
    }
    const bool fallback_allowed =
        profile.recoverable_substitution_mode.has_value() &&
        *profile.recoverable_substitution_mode == stop_result.executed_stop_mode;
    if (!fallback_allowed) {
      downgrade_to_intervention("executed_mode_not_authorized_substitution",
                                "backend executed a stop_mode not in "
                                "stop_profile.recoverable_substitution_mode");
      return;
    }
    if (node_category == MotionInterruptNodeCategory::kPrecisionContact) {
      downgrade_to_intervention(
          "fallback_not_allowed_for_node_category",
          "precision-contact node category cannot accept a stop-mode fallback");
      return;
    }
    fallback_qualification_row = qualified_stop_fallback_pair(
        profile.stop_mode, stop_result.executed_stop_mode, node_category);
    if (fallback_qualification_row == nullptr) {
      downgrade_to_intervention("fallback_pair_not_qualified",
                                "requested/executed stop-mode fallback pair is not in the "
                                "Phase 4.5G qualification matrix");
      return;
    }
    if (!fallback_row_allows_command_source(*fallback_qualification_row,
                                            stop_result.stop_command_source)) {
      downgrade_to_intervention("fallback_command_source_not_qualified",
                                "controlled-stop fallback command source is not in the "
                                "Phase 4.5G qualification matrix row");
      return;
    }
    if (!stop_result.fallback_reason) {
      downgrade_to_intervention("fallback_reason_required",
                                "controlled-stop fallback executed without fallback_reason");
      return;
    }
  }
  // (4) Capture a FRESH post-stop snapshot. Using the worker's last
  // pre-trip poll would record a stale state. Pull the snapshot
  // directly from the backend now that it has confirmed the stop, and
  // recover joint names from the observed-state provider so spec/210 §3.7
  // size/ordering validation can actually run.
  const auto post_stop_backend_snapshot = execution_backend_.get_execution_snapshot();
  std::optional<RobotStateDTO> observed_state_provider_state;
  auto observed_state = observed_state_.current_state(clock_.now());
  if (observed_state.ok()) {
    observed_state_provider_state = std::move(observed_state.value());
  }
  std::string snap_failure_reason;
  auto snap_opt = capture_stop_snapshot(
      post_stop_backend_snapshot, observed_state_provider_state, stop_result.backend_stop_confirmed,
      node_category, clock_.now(), settle_velocity_threshold_rad_s, snap_failure_reason);
  if (!snap_opt) {
    downgrade_to_intervention(std::move(snap_failure_reason),
                              "controlled-stop snapshot capture or validation failed",
                              std::string(kErrorMotionInterruptStopStateInvalid));
    return;
  }
  // (4b) Measured-stop confirmation. A backend-confirmed stop must also show
  // measured rest. When the settle signal is available and the robot is NOT at
  // rest even after the bounded wait above, the reported stop did not actually
  // quiesce the robot. At this point the stop command was already accepted and
  // confirmed, so issuing another JTC cancel would be redundant and possibly
  // misleading; hold intervention for explicit recovery instead. An absent
  // settle signal (UNKNOWN) falls back to the backend's confirmation and does
  // not block the outcome.
  if (snap_opt->at_rest.has_value() && !*snap_opt->at_rest) {
    std::map<std::string, std::string> settle_evidence{
        {"hard_stop_transitioned", "false"},
        {"emergency_fallback", "not_attempted_controlled_stop_already_confirmed"},
        {"settle_velocity_threshold_rad_s", std::to_string(settle_velocity_threshold_rad_s)}};
    if (snap_opt->max_abs_velocity_rad_s) {
      settle_evidence["measured_max_abs_velocity_rad_s"] =
          std::to_string(*snap_opt->max_abs_velocity_rad_s);
    }
    downgrade_to_intervention("stop_not_settled_measured_velocity",
                              "backend confirmed the stop but measured velocity shows the robot is "
                              "not at rest; redundant hard-stop cancel was not attempted",
                              std::string(kErrorMotionInterruptBackendStopFailed),
                              std::move(settle_evidence));
    return;
  }
  // (5) Build and publish the signal_trip outcome.
  MotionTripOutcomeDTO trip;
  trip.latch_id = latch.latch_id;
  trip.source_id = latch.source_id;
  trip.policy_id = latch.policy_id;
  trip.scope_id = latch.motion_interrupt_scope_id.value_or("");
  trip.signal_snapshot = latch.signal_snapshot;
  trip.stop_snapshot = std::move(*snap_opt);
  trip.requested_stop_profile = profile;
  trip.executed_stop_mode = stop_result.executed_stop_mode;
  trip.stop_command_source = stop_result.stop_command_source;
  // signal_to_latch_ns measures time from when the source published
  // the signal (latch.signal_snapshot.timestamp_ns) to when the
  // manager accepted the latch (latch.latched_at_ns). Using the
  // handler's `clock_.now()` here would always be negative because
  // the latch already exists by the time we get called.
  trip.signal_to_latch_ns =
      std::max<std::int64_t>(0, latch.latched_at_ns - latch.signal_snapshot.timestamp_ns);
  trip.latch_to_backend_stop_call_ns =
      std::max<std::int64_t>(0, call_start_ns - latch.latched_at_ns);
  trip.latch_to_stop_confirmed_ns = std::max<std::int64_t>(0, call_end_ns - latch.latched_at_ns);
  trip.collision_world_revision = latch.borrowed_collision_world_revision;
  trip.stop_distance_rad = stop_result.stop_distance_rad;
  trip.path_deviation_max_rad = stop_result.path_deviation_max_rad;
  trip.initial_path_deviation_rad = stop_result.initial_path_deviation_rad;
  trip.fallback_reason = stop_result.fallback_reason;
  trip.diagnostics = stop_result.diagnostics;
  if (fallback_qualification_row != nullptr) {
    trip.diagnostics["fallback_qualification_id"] = fallback_qualification_row->qualification_id;
    trip.diagnostics["fallback_qualification_rationale"] = fallback_qualification_row->rationale;
    trip.diagnostics["fallback_qualification_required_telemetry"] =
        fallback_qualification_row->telemetry;
    trip.diagnostics["fallback_qualification_evidence_status"] =
        fallback_qualification_row->evidence_status;
    trip.diagnostics["fallback_pair"] = std::string(stop_mode_string(profile.stop_mode)) + "->" +
                                        stop_mode_string(stop_result.executed_stop_mode);
  }
  // Copy the scope's result_binding_key onto the trip outcome so
  // BT/runtime consumers
  // can correlate the outcome with the blackboard slot the motion
  // node owns. The key is the only stable identifier the BT layer
  // can use to dispatch the result downstream.
  if (matching_scope && matching_scope->result_binding_key) {
    trip.result_binding_key = *matching_scope->result_binding_key;
  }

  // Transition Stopping -> Completed BEFORE finalize so the outcome
  // record reflects the terminal state. The latch is cleared after
  // the outcome is durable via the auto_clear_after_motion_outcome
  // disposition path.
  {
    KernelStopCertificateDTO certificate;
    certificate.completion_result = StopCertificateOutcome::kSucceeded;
    certificate.stop_owner = StopOwnerKind::kMotionTripStop;
    certificate.stop_owner_generation = motion_trip_stop_generation;
    certificate.execution_id = exec_id;
    certificate.latch_id = latch.latch_id;
    certificate.control_command_epoch = control_command_epoch();
    certificate.requested_stop_mode = stop_mode_string(profile.stop_mode);
    certificate.executed_stop_mode = stop_mode_string(stop_result.executed_stop_mode);
    certificate.command_source = stop_command_source_string(stop_result.stop_command_source);
    certificate.preemption_result = InterruptPreemptionResult::kNoneRequired;
    certificate.backend_stop_confirmed = trip.stop_snapshot.backend_stop_confirmed;
    certificate.observed_state_source_timestamp_ns = trip.stop_snapshot.captured_at_ns;
    certificate.observed_state_received_timestamp_ns = trip.stop_snapshot.captured_at_ns;
    certificate.observed_state_fresh = true;
    certificate.observed_state_at_rest = trip.stop_snapshot.at_rest.value_or(false);
    certificate.at_rest_evidence = trip.stop_snapshot.at_rest.has_value()
                                       ? AtRestEvidence::kMeasuredVelocity
                                       : AtRestEvidence::kUnavailable;
    certificate.completed_at_ns = clock_.now();
    (void)publish_stop_certificate(std::move(certificate));
  }
  auto completion_transition = commit_terminal(ExecutionState::kCompleted, "terminal_commit",
                                               TerminalCommitter::Origin::kMotionTrip);
  if (!completion_transition.ok()) {
    downgrade_to_intervention("motion_trip_completed_transition_rejected",
                              "motion-trip controlled stop could not commit Completed as the "
                              "terminal authority");
    return;
  }
  auto trip_success_details = controlled_stop_success_event_details(
      profile, stop_result, execution_backend_, "motion_trip");
  trip_success_details["latch_id"] = latch.latch_id;
  trip_success_details["source_id"] = latch.source_id;
  trip_success_details["policy_id"] = latch.policy_id;
  trip_success_details["scope_id"] = trip.scope_id;
  trip_success_details["signal_to_latch_ns"] = std::to_string(trip.signal_to_latch_ns);
  trip_success_details["latch_to_backend_stop_call_ns"] =
      std::to_string(trip.latch_to_backend_stop_call_ns);
  trip_success_details["latch_to_stop_confirmed_ns"] =
      std::to_string(trip.latch_to_stop_confirmed_ns);
  if (trip.stop_snapshot.max_abs_velocity_rad_s.has_value()) {
    trip_success_details["stop_snapshot_max_abs_velocity_rad_s"] =
        std::to_string(*trip.stop_snapshot.max_abs_velocity_rad_s);
  }
  if (trip.stop_snapshot.at_rest.has_value()) {
    trip_success_details["stop_snapshot_at_rest"] = *trip.stop_snapshot.at_rest ? "true" : "false";
  }
  emit_event({KernelEventType::kMotionTripCompleted,
              exec_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              std::move(trip_success_details)});
  emit_event({KernelEventType::kStateTransition,
              exec_id,
              clock_.now(),
              ExecutionState::kStopping,
              ExecutionState::kCompleted,
              std::nullopt,
              {}});

  finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kSignalTrip,
                                    std::nullopt, std::move(trip), "motion_trip_completed");

  // Auto-clear only on the success path. Failure paths leave the
  // latch in place for operator intervention-acknowledge per
  // spec/230 §3.4 / spec/260 §5.6.
  auto clear =
      interrupt_manager_.clear_active_latch("auto_clear_after_motion_outcome", clock_.now());
  if (clear.ok()) {
    record_terminal_fault_authority(clear.value());
  }
}

std::optional<MotionExecutionOutcomeDTO> ExecutionKernel::get_execution_outcome(
    std::uint64_t execution_id) const {
  std::lock_guard<std::mutex> lock(motion_outcomes_mutex_);
  const auto it = motion_execution_outcomes_.find(execution_id);
  if (it == motion_execution_outcomes_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<MotionInterruptScopeStatusDTO> ExecutionKernel::active_motion_interrupt_scopes() const {
  return interrupt_manager_.active_motion_interrupt_scopes();
}

InterruptIngestResult ExecutionKernel::publish_fast_interrupt_signal(
    const InterruptSignalUpdate& update) {
  return publish_fast_interrupt_signals({update});
}

InterruptIngestResult ExecutionKernel::publish_supervisory_interrupt_signal(
    const InterruptSignalUpdate& update) {
  if (!interrupt_phase_from_state(sm_.status().state)) {
    auto result = reject_interrupts_without_active_phase({update});
    emit_interrupt_events(result);
    return result;
  }
  auto result = interrupt_manager_.ingest(update, InterruptSourceClass::kLowRateSupervisory,
                                          current_interrupt_context(), clock_.now());
  stamp_interrupt_latch_collision_world_revision(result);
  apply_interrupt_ingest_effects(result, /*apply_preemption_inline=*/true);
  return result;
}

InterruptIngestResult ExecutionKernel::publish_fast_interrupt_signals(
    const std::vector<InterruptSignalUpdate>& updates) {
  return publish_fast_interrupt_signals_internal(updates, /*apply_preemption_inline=*/true);
}

InterruptIngestResult ExecutionKernel::publish_fast_interrupt_signals_internal(
    const std::vector<InterruptSignalUpdate>& updates, bool apply_preemption_inline) {
  if (!interrupt_phase_from_state(sm_.status().state)) {
    interrupt_manager_.seed_samples(updates, InterruptSourceClass::kFastExecutionRelevant,
                                    clock_.now());
    auto result = reject_interrupts_without_active_phase(updates);
    emit_interrupt_events(result);
    return result;
  }
  auto result =
      interrupt_manager_.ingest_batch(updates, InterruptSourceClass::kFastExecutionRelevant,
                                      current_interrupt_context(), clock_.now());
  stamp_interrupt_latch_collision_world_revision(result);
  apply_interrupt_ingest_effects(result, apply_preemption_inline);
  return result;
}

std::optional<InterruptLatch> ExecutionKernel::get_active_interrupt_latch() const {
  return interrupt_manager_.active_latch();
}

Result<InterruptIngestResult> ExecutionKernel::apply_interrupt_disposition(
    const InterruptDisposition& disposition) {
  auto result = interrupt_manager_.apply_disposition(disposition, clock_.now());
  if (!result.ok()) {
    return result;
  }
  stamp_interrupt_latch_collision_world_revision(result.value());
  apply_interrupt_ingest_effects(result.value(),
                                 /*apply_preemption_inline=*/true);
  if (!interrupt_manager_.active_latch() && sm_.status().state == ExecutionState::kInterrupted) {
    const auto prev_state = sm_.status().state;
    auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
    if (transition.ok()) {
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.state = ExecutionState::kInterventionRequired;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  prev_state,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "interrupt_disposition"}}});
    }
  }
  return result;
}

void ExecutionKernel::publish_execution_snapshot(ExecutionSnapshot snapshot) {
  stamp_active_request_correlation(snapshot);
  const auto fallback_timestamp_ns = snapshot.timestamp_ns == 0 ? clock_.now() : std::int64_t{0};
  const auto published = snapshot_publisher_.publish(std::move(snapshot), fallback_timestamp_ns);
  flight_recorder_.note_published_snapshot(published.sequence_number, published.timestamp_ns,
                                           published.execution_id, published.state);

  {
    std::lock_guard<std::mutex> mlock(metrics_mutex_);
    metrics_.snapshots_published++;
  }
}

bool ExecutionKernel::wait_for_completion(int64_t timeout_ns) {
  return worker_signals_.wait_for_completion(timeout_ns);
}

RuntimeCollisionProtectionSnapshot ExecutionKernel::runtime_collision_protection_snapshot() const {
  std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
  return runtime_collision_protection_;
}

bool ExecutionKernel::refresh_published_runtime_collision_snapshot(
    std::map<std::string, std::string>* details, RuntimeCollisionQueryBundlePtr* published_bundle) {
  const auto fail_refresh = [this, details,
                             published_bundle](std::map<std::string, std::string> failure_details) {
    if (published_bundle != nullptr) {
      published_bundle->reset();
    }
    failure_details["reason_code"] = "runtime_collision_snapshot_refresh_failed";
    failure_details["surface"] = "runtime_collision_monitor";
    const auto cleared_epoch =
        runtime_collision_snapshot_invalidation_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    failure_details["runtime_collision_snapshot_invalidation_epoch"] =
        std::to_string(cleared_epoch);
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.runtime_collision_snapshot_refresh_failures++;
    }
    if (details) {
      *details = failure_details;
    }
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                ErrorDTO{kErrorExecutionAborted,
                         "runtime collision snapshot is unavailable for "
                         "monitor admission",
                         failure_details},
                failure_details});
    return false;
  };

  const auto resources = collision_resources_.snapshot();
  if (!resources.snapshot_publisher) {
    return fail_refresh({{"reason", "missing_collision_world_snapshot_publisher"}});
  }
  auto bundle = load_published_runtime_collision_query_bundle(*resources.snapshot_publisher);
  if (!bundle.ok()) {
    return fail_refresh(bundle.error().details);
  }
  if (!bundle.value() || !bundle.value()->world) {
    return fail_refresh({{"reason", "runtime_collision_snapshot_not_published"}});
  }
  if (published_bundle != nullptr) {
    *published_bundle = std::move(bundle.value());
  }
  return true;
}

std::uint64_t ExecutionKernel::start_collision_monitoring(
    std::string reason, RuntimeCollisionProtectionPurpose purpose,
    const CollisionWorldLeaseInventory* admission_inventory) {
  const bool stop_window = purpose == RuntimeCollisionProtectionPurpose::kControlledStop;
  std::uint64_t reused_generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    const auto active_session = runtime_collision_query_session_.load(std::memory_order_acquire);
    if (runtime_collision_protection_.active && active_session) {
      // Executing -> controlled-stop is one continuous protection window.
      // Reuse the prepared session so there is no unmonitored engine-rebuild
      // gap and contact-release history cannot be re-baselined at stop entry.
      runtime_collision_protection_.generation = ++runtime_collision_protection_next_generation_;
      runtime_collision_protection_.execution_id = sm_.status().execution_id;
      runtime_collision_protection_.state_at_acquire = sm_.status().state;
      runtime_collision_protection_.reason = std::move(reason);
      reused_generation = runtime_collision_protection_.generation;
      runtime_collision_monitor_.start();
    }
  }
  if (reused_generation != 0) {
    if (stop_window) {
      runtime_collision_stop_coverage_.start(reused_generation);
    }
    return reused_generation;
  }

  // A previous generation can still be finishing its engine query after its
  // pending schedule was cancelled. Runtime query results are non-owning views
  // into generation-local engine scratch, so the next session must not prepare
  // or mutate that engine until the old callback has fully returned.
  runtime_collision_monitor_.stop(/*drain_callbacks=*/true);

  std::map<std::string, std::string> refresh_details;
  RuntimeCollisionQueryBundlePtr initial_bundle;
  bool snapshot_ready = false;
  std::optional<ProjectionRevisionDTO> admission_projection_revision;
  if (admission_inventory != nullptr && admission_inventory->projection_binding.valid() &&
      admission_inventory->bundle.world && admission_inventory->bundle.policy.has_value()) {
    admission_projection_revision = admission_inventory->projection_binding.projection_revision;
    initial_bundle = build_runtime_collision_query_bundle(admission_inventory->bundle, 0U,
                                                          admission_projection_revision);
    snapshot_ready = initial_bundle && initial_bundle->world && initial_bundle->policy &&
                     initial_bundle->policy_index && initial_bundle->lookup_order_valid &&
                     initial_bundle->distance_selection_order_valid;
    if (!snapshot_ready) {
      refresh_details["reason"] = "retained_runtime_collision_bundle_invalid";
    }
  } else if (admission_inventory != nullptr) {
    refresh_details["reason"] = "retained_runtime_collision_inventory_invalid";
  } else {
    snapshot_ready =
        refresh_published_runtime_collision_snapshot(&refresh_details, &initial_bundle);
  }
  std::shared_ptr<RuntimeCollisionQuerySession> query_session;
  std::optional<ErrorDTO> query_prepare_error;
  const auto resources = collision_resources_.snapshot();
  std::optional<RuntimeCollisionStateProjection> projection;
  {
    std::lock_guard<std::mutex> projection_lock(runtime_collision_state_projection_mutex_);
    projection = runtime_collision_state_projection_;
  }
  if (!snapshot_ready) {
    query_prepare_error =
        ErrorDTO{kErrorValidationCollision, "Runtime collision snapshot publication is unavailable",
                 std::move(refresh_details)};
  } else if (!resources.snapshot_publisher) {
    query_prepare_error = ErrorDTO{kErrorValidationCollision,
                                   "Runtime collision snapshot publisher is missing",
                                   {{"reason", "missing_collision_world_snapshot_publisher"}}};
  } else if (!resources.runtime_monitor_engine) {
    query_prepare_error = ErrorDTO{kErrorValidationCollision,
                                   "Runtime collision query engine is missing",
                                   {{"reason", "missing_runtime_collision_query_engine"}}};
  } else if (!projection) {
    query_prepare_error = ErrorDTO{kErrorValidationCollision,
                                   "Runtime collision state projection is unavailable",
                                   {{"reason", "runtime_collision_projection_unavailable"}}};
  } else {
    query_session = std::make_shared<RuntimeCollisionQuerySession>(
        std::move(*projection), *resources.snapshot_publisher, resources.runtime_monitor_engine,
        runtime_collision_monitor_.active_contact_policy_id(),
        static_cast<std::int64_t>(config_.runtime_collision_immediate_query_budget_ns),
        std::move(admission_projection_revision));
    query_prepare_error = query_session->prepare(initial_bundle);
    if (query_prepare_error) {
      query_session.reset();
    }
  }
  {
    std::lock_guard<std::mutex> error_lock(runtime_collision_query_prepare_error_mutex_);
    runtime_collision_query_prepare_error_ = query_prepare_error;
  }
  runtime_collision_query_session_.store(std::move(query_session), std::memory_order_release);
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    runtime_collision_protection_.active = true;
    runtime_collision_protection_.generation = ++runtime_collision_protection_next_generation_;
    runtime_collision_protection_.execution_id = sm_.status().execution_id;
    runtime_collision_protection_.state_at_acquire = sm_.status().state;
    runtime_collision_protection_.reason = std::move(reason);
    generation = runtime_collision_protection_.generation;
    runtime_collision_monitor_.start();
  }
  if (stop_window) {
    runtime_collision_stop_coverage_.start(generation);
  }
  return generation;
}

void ExecutionKernel::stop_collision_monitoring(RuntimeCollisionMonitorRelease /*release*/,
                                                std::optional<std::uint64_t> expected_generation,
                                                bool drain_callbacks) {
  bool release_current = false;
  std::uint64_t coverage_execution_id = 0;
  std::optional<KernelEvent> coverage_summary;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    release_current = runtime_collision_protection_.active &&
                      (!expected_generation.has_value() ||
                       runtime_collision_protection_.generation == *expected_generation);
    if (release_current) {
      coverage_execution_id = runtime_collision_protection_.execution_id;
      runtime_collision_protection_.active = false;
      runtime_collision_protection_.reason.clear();
      if (const auto lookahead =
              runtime_collision_lookahead_session_.load(std::memory_order_acquire)) {
        lookahead->disarm();
      }
    }
    if (!release_current && expected_generation.has_value()) {
      return;
    }
    runtime_collision_monitor_.stop(drain_callbacks);
    runtime_collision_query_session_.store(nullptr, std::memory_order_release);
    if (release_current) {
      clear_runtime_collision_state_projection_unlocked();
      const auto generation = runtime_collision_stop_coverage_.active_generation();
      const auto now_ns = clock_.now();
      if (auto summary = runtime_collision_stop_coverage_.complete(generation, now_ns)) {
        std::map<std::string, std::string> details{
            {"reason", "runtime_collision_stop_coverage_summary"},
            {"monitor_generation", std::to_string(summary->generation)},
            {"ticks_attempted", std::to_string(summary->ticks_attempted)},
            {"collision_queries_executed", std::to_string(summary->queries_executed)},
            {"unavailable_ticks", std::to_string(summary->unavailable_ticks)},
            {"longest_unavailable_interval_ns", std::to_string(summary->longest_unavailable_ns)},
            {"coverage_complete", summary->ticks_attempted > 0 &&
                                          summary->ticks_attempted == summary->queries_executed &&
                                          summary->unavailable_ticks == 0
                                      ? "true"
                                      : "false"},
            {"effective_coverage_ratio",
             summary->ticks_attempted == 0
                 ? "0"
                 : std::to_string(static_cast<double>(summary->queries_executed) /
                                  static_cast<double>(summary->ticks_attempted))}};
        for (std::size_t i = 0; i < summary->unavailable_by_reason.size(); ++i) {
          if (summary->unavailable_by_reason[i] == 0) {
            continue;
          }
          const auto reason = static_cast<RuntimeCollisionUnavailableReason>(i);
          details[std::string("unavailable_reason_") +
                  runtime_collision_unavailable_reason_name(reason)] =
              std::to_string(summary->unavailable_by_reason[i]);
        }
        coverage_summary = KernelEvent{KernelEventType::kRuntimeCollisionCoverage,
                                       coverage_execution_id,
                                       now_ns,
                                       {},
                                       {},
                                       std::nullopt,
                                       std::move(details)};
      }
    }
  }
  if (coverage_summary.has_value()) {
    emit_event(std::move(*coverage_summary));
  }
}

void ExecutionKernel::arm_runtime_collision_lookahead(
    std::uint64_t execution_id, std::uint64_t execution_generation,
    std::shared_ptr<const TrajectoryDTO> collision_trajectory, const std::string& contact_policy_id,
    const CollisionWorldLeaseInventory& admission_inventory) {
  const auto lookahead = runtime_collision_lookahead_session_.load(std::memory_order_acquire);
  if (!lookahead) {
    return;
  }

  bool invalid_authority = false;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    const auto state = sm_.status().state;
    if (!runtime_collision_protection_.active ||
        runtime_collision_protection_.execution_id != execution_id ||
        (state != ExecutionState::kExecuting && state != ExecutionState::kPaused)) {
      return;
    }
    invalid_authority = execution_generation == 0 || !collision_trajectory ||
                        collision_trajectory->waypoints.empty() ||
                        !admission_inventory.projection_binding.valid();
    if (!invalid_authority) {
      lookahead->arm(execution_id, execution_generation, std::move(collision_trajectory),
                     contact_policy_id, admission_inventory.projection_binding.projection_revision);
      return;
    }
  }

  RuntimeCollisionLookaheadResult failure;
  failure.execution_id = execution_id;
  failure.execution_generation = execution_generation;
  failure.status = RuntimeCollisionLookaheadResultStatus::kInconclusive;
  failure.reason = execution_generation == 0
                       ? "execution_progress_generation_unavailable"
                       : (!collision_trajectory || collision_trajectory->waypoints.empty()
                              ? "runtime_lookahead_trajectory_unavailable"
                              : "runtime_lookahead_projection_revision_unavailable");
  apply_runtime_collision_lookahead_result(std::move(failure));
}

void ExecutionKernel::apply_runtime_collision_lookahead_result(
    RuntimeCollisionLookaheadResult result,
    const RuntimeCollisionLookaheadSession* source_session) {
  if (apply_runtime_collision_lookahead_authority(result, source_session) ==
      RuntimeCollisionLookaheadAuthorityDisposition::kAccepted) {
    emit_runtime_collision_lookahead_evidence(std::move(result));
  }
}

RuntimeCollisionLookaheadAuthorityDisposition
ExecutionKernel::apply_runtime_collision_lookahead_authority(
    RuntimeCollisionLookaheadResult& result,
    const RuntimeCollisionLookaheadSession* source_session) {
  if (!runtime_lookahead_requires_authority(result)) {
    return RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
  }

  // Keep pre-authority metadata fixed-size. Exact sample lists and engine
  // diagnostics are observer evidence and may be large at the configured
  // sample cap; building them before latching the stop would delay authority.
  auto authority_details = runtime_lookahead_authority_details(result);

  CollisionCheckResultDTO check;
  check.in_collision = result.status == RuntimeCollisionLookaheadResultStatus::kCollision;
  if (result.first_failing_pair) {
    check.contact_body_a = result.first_failing_pair->body_a;
    check.contact_body_b = result.first_failing_pair->body_b;
    authority_details["contact_a"] = result.first_failing_pair->body_a;
    authority_details["contact_b"] = result.first_failing_pair->body_b;
  }

  InterruptIngestResult interrupt;
  {
    EventDeferralScope event_scope(*this);
    std::unique_lock<std::mutex> lock(runtime_collision_protection_mutex_);
    if (result.arm_generation != 0) {
      const auto current_session =
          runtime_collision_lookahead_session_.load(std::memory_order_acquire);
      if (!current_session || current_session.get() != source_session ||
          !current_session->accept_result_for_current_arm(
              result.execution_id, result.execution_generation, result.arm_generation,
              result.permit_generation)) {
        return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
      }
    }
    const auto state = sm_.status().state;
    if (!runtime_collision_protection_.active ||
        runtime_collision_protection_.execution_id != result.execution_id ||
        (state != ExecutionState::kExecuting && state != ExecutionState::kPaused) ||
        motion_trip_stop_.active() || operator_stop_worker_.active() ||
        fault_preempt_stop_active()) {
      return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
    }
    if (result.arm_generation != 0 &&
        execution_backend_.claim_execution_progress_hazard(result.execution_generation) !=
            ExecutionProgressHazardClaimResult::kClaimed) {
      return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
    }
    cache_.invalidate_all();
    // Authority first. Any event/status publication below is evidence of a
    // stop request that has already been latched, never a predictor of one.
    interrupt = publish_runtime_collision_fault_interrupt(check, authority_details);
    lock.unlock();
    event_scope.flush();
  }

  result.stop_authority_signalled = !interrupt.rejected;
  result.authority_rejected = interrupt.rejected;
  result.authority_rejection_reason = interrupt.reason;
  return RuntimeCollisionLookaheadAuthorityDisposition::kAccepted;
}

void ExecutionKernel::emit_runtime_collision_lookahead_evidence(
    RuntimeCollisionLookaheadResult result) {
  if (!runtime_lookahead_requires_authority(result)) {
    return;
  }

  auto authority_details = runtime_lookahead_authority_details(result);
  CollisionCheckResultDTO check;
  check.in_collision = result.status == RuntimeCollisionLookaheadResultStatus::kCollision;
  if (result.first_failing_pair) {
    check.contact_body_a = result.first_failing_pair->body_a;
    check.contact_body_b = result.first_failing_pair->body_b;
    authority_details["contact_a"] = result.first_failing_pair->body_a;
    authority_details["contact_b"] = result.first_failing_pair->body_b;
  }

  std::string sampled_times_ns;
  for (const auto time_ns : result.sample_times_ns) {
    if (!sampled_times_ns.empty()) {
      sampled_times_ns.push_back(',');
    }
    sampled_times_ns += std::to_string(time_ns);
  }
  auto details = authority_details;
  details["stop_authority_signalled"] = result.stop_authority_signalled ? "true" : "false";
  details["lookahead_eligible_source_waypoints"] = std::to_string(result.eligible_source_waypoints);
  details["lookahead_sampling_reduced"] = result.sampling_reduced ? "true" : "false";
  details["lookahead_sample_times_ns"] = std::move(sampled_times_ns);
  details["lookahead_workers_used"] = std::to_string(result.workers_used);
  details["lookahead_cycle_duration_ns"] = std::to_string(result.cycle_duration_ns);
  details["lookahead_worker_active_duration_ns"] = std::to_string(result.worker_active_duration_ns);
  details["lookahead_start_jitter_ns"] = std::to_string(result.start_jitter_ns);
  details["lookahead_cycle_deadline_exceeded"] = result.cycle_deadline_exceeded ? "true" : "false";
  for (const auto& [key, value] : result.failure_details) {
    if (key == "collision_result_waypoint_index") {
      details["collision_result_lookahead_local_sample_index"] = value;
      continue;
    }
    details.try_emplace(key, value);
  }

  emit_contact_policy_violation_event(result.execution_id, details);

  KernelEvent event;
  event.type =
      check.in_collision ? KernelEventType::kCollisionDetected : KernelEventType::kErrorRaised;
  event.execution_id = result.execution_id;
  event.timestamp_ns = clock_.now();
  event.details = details;
  if (!check.in_collision) {
    event.error = ErrorDTO{kErrorExecutionAborted,
                           runtime_collision_lookahead_error_message(result.reason), details};
  }
  emit_event(std::move(event));
  if (check.in_collision) {
    std::lock_guard<std::mutex> collision_metrics_lock(metrics_mutex_);
    ++metrics_.collision_detections;
  }
  if (result.authority_rejected) {
    emit_event({KernelEventType::kErrorRaised,
                result.execution_id,
                clock_.now(),
                {},
                {},
                ErrorDTO{kErrorInvalidRequest,
                         "Runtime lookahead interrupt signal rejected",
                         {{"reason", result.authority_rejection_reason}}},
                {{"reason", result.authority_rejection_reason},
                 {"surface", "observed_runtime_lookahead"}}});
  }
}

void ExecutionKernel::emit_contact_policy_violation_event(
    std::uint64_t execution_id, const std::map<std::string, std::string>& source_details,
    std::optional<std::string_view> response_override) {
  const auto violation = source_details.find("contact_policy_violation");
  if (violation == source_details.end()) {
    return;
  }

  static constexpr std::array<const char*, 18> kPayloadKeys{"contact_policy_id",
                                                            "policy_mode",
                                                            "zone_id",
                                                            "release_evidence_mode",
                                                            "contact_body_a",
                                                            "contact_body_b",
                                                            "contact_present",
                                                            "initial_contact_pair_count",
                                                            "current_contact_pair_count",
                                                            "distance_request_mode",
                                                            "evaluated_distance_pair_count",
                                                            "penetration_depth_m",
                                                            "max_penetration_m",
                                                            "max_contact_penetration_m",
                                                            "response",
                                                            "classification",
                                                            "surface",
                                                            "within_penetration_bound"};
  std::map<std::string, std::string> details{{"violation_reason", violation->second}};
  for (const auto key : kPayloadKeys) {
    if (const auto found = source_details.find(key); found != source_details.end()) {
      details.emplace(found->first, found->second);
    }
  }
  if (response_override.has_value()) {
    details["response"] = std::string(*response_override);
  }

  emit_event({KernelEventType::kContactPolicyViolation,
              execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              std::move(details)});
}

void ExecutionKernel::revoke_runtime_collision_lookahead_clear_permits() noexcept {
  if (const auto lookahead = runtime_collision_lookahead_session_.load(std::memory_order_acquire)) {
    lookahead->revoke_clear_permits();
  }
}

void ExecutionKernel::set_runtime_collision_state_projection(
    RuntimeCollisionStateProjection projection) {
  std::lock_guard<std::mutex> protection_lock(runtime_collision_protection_mutex_);
  std::lock_guard<std::mutex> lock(runtime_collision_state_projection_mutex_);
  runtime_collision_state_projection_ = std::move(projection);
}

void ExecutionKernel::clear_runtime_collision_state_projection_unlocked() {
  std::lock_guard<std::mutex> lock(runtime_collision_state_projection_mutex_);
  runtime_collision_state_projection_.reset();
}

void ExecutionKernel::clear_runtime_collision_state_projection() {
  std::lock_guard<std::mutex> protection_lock(runtime_collision_protection_mutex_);
  clear_runtime_collision_state_projection_unlocked();
}

void ExecutionKernel::finish_execute_worker() {
  worker_signals_.mark_complete();
}

void ExecutionKernel::notify_execution_waiters() {
  worker_signals_.notify();
}

bool ExecutionKernel::retire_execute_worker(std::unique_lock<std::mutex>& lifecycle_lock) {
  if (!worker_thread_.joinable()) {
    return true;
  }
  if (worker_thread_.get_id() == std::this_thread::get_id()) {
    worker_thread_.request_stop();
    worker_signals_.notify();
    return false;
  }

  lifecycle_worker_retirement_in_progress_.store(true, std::memory_order_release);
  worker_thread_.request_stop();
  worker_signals_.notify();
  lifecycle_lock.unlock();
  worker_thread_.join();
  lifecycle_lock.lock();
  lifecycle_worker_retirement_in_progress_.store(false, std::memory_order_release);
  return true;
}

bool ExecutionKernel::finalize_overridden_terminal_authority(
    const ExecuteRequestDTO& request, std::uint64_t exec_id, ActiveMotionExecution& motion_state,
    ExecutionState committed, const char* reason, std::optional<ErrorDTO> error) {
  if (committed != ExecutionState::kStopped && committed != ExecutionState::kInterrupted &&
      committed != ExecutionState::kInterventionRequired) {
    return false;
  }
  auto terminal_snapshot = get_execution_snapshot();
  terminal_snapshot.timestamp_ns = clock_.now();
  terminal_snapshot.execution_id = committed == ExecutionState::kIdle ? 0 : exec_id;
  terminal_snapshot.move_id = committed == ExecutionState::kIdle ? std::string{} : request.move_id;
  terminal_snapshot.state = committed;
  if (error.has_value()) {
    terminal_snapshot.error = error;
  }
  publish_execution_snapshot(std::move(terminal_snapshot));
  switch (committed) {
    case ExecutionState::kStopped:
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                        std::nullopt, std::nullopt, reason);
      break;
    case ExecutionState::kInterrupted:
      finalize_motion_execution_outcome(motion_state,
                                        MotionExecutionTerminalCause::kInterruptedFault,
                                        std::nullopt, std::nullopt, reason);
      break;
    case ExecutionState::kInterventionRequired:
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          error.value_or(ErrorDTO{kErrorStopFailed,
                                  "Terminal authority entered intervention without captured "
                                  "error",
                                  {{"reason", reason}}}),
          std::nullopt, reason);
      break;
    default:
      return false;
  }
  return true;
}

bool ExecutionKernel::finalize_competing_terminal_authority(const ExecuteRequestDTO& request,
                                                            std::uint64_t exec_id,
                                                            ActiveMotionExecution& motion_state,
                                                            const char* reason) {
  if (motion_state.outcome_emitted) {
    return true;
  }

  if (operator_stop_worker_.active()) {
    const auto wait_budget =
        config_.operator_stop_profile
            ? effective_controlled_stop_deadline_duration(
                  *config_.operator_stop_profile, config_.complete_move_on_signal_timeout_ns)
            : std::chrono::steady_clock::duration::zero();
    const auto bounded_wait = wait_budget > std::chrono::steady_clock::duration::zero()
                                  ? wait_budget + std::chrono::milliseconds(250)
                                  : std::chrono::milliseconds(250);
    (void)wait_for_operator_stop_worker(bounded_wait);
  }

  const auto current = sm_.status().state;
  if (motion_state.outcome_emitted) {
    return true;
  }
  if (current == ExecutionState::kStopped || current == ExecutionState::kIdle) {
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, reason);
    return true;
  }
  if (current == ExecutionState::kInterventionRequired) {
    auto intervention_snapshot = get_execution_snapshot();
    intervention_snapshot.timestamp_ns = clock_.now();
    intervention_snapshot.execution_id = exec_id;
    intervention_snapshot.move_id = request.move_id;
    intervention_snapshot.state = ExecutionState::kInterventionRequired;
    if (!intervention_snapshot.error.has_value()) {
      std::optional<ErrorDTO> operator_stop_error = operator_stop_worker_.intervention_error();
      std::optional<ErrorDTO> fault_preempt_error =
          fault_preempt_intervention_error_for_active_latch();
      intervention_snapshot.error = operator_stop_error.value_or(
          fault_preempt_error.value_or(ErrorDTO{kErrorStopFailed,
                                                "Execution terminal authority moved to "
                                                "InterventionRequired without captured error",
                                                {{"reason", "terminal_authority_intervention"}}}));
    }
    const auto intervention_error = *intervention_snapshot.error;
    publish_execution_snapshot(std::move(intervention_snapshot));
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      intervention_error, std::nullopt, reason);
    return true;
  }
  if (current == ExecutionState::kInterrupted || interrupt_manager_.active_latch().has_value()) {
    if (active_trajectory_preempt_pending()) {
      return false;
    }
    const auto prev = current;
    if (current != ExecutionState::kInterrupted) {
      (void)commit_terminal(ExecutionState::kInterrupted, "terminal_commit");
    }
    auto interrupted_snapshot = get_execution_snapshot();
    interrupted_snapshot.timestamp_ns = clock_.now();
    interrupted_snapshot.execution_id = exec_id;
    interrupted_snapshot.move_id = request.move_id;
    interrupted_snapshot.state = ExecutionState::kInterrupted;
    publish_execution_snapshot(std::move(interrupted_snapshot));
    if (prev != ExecutionState::kInterrupted &&
        sm_.status().state == ExecutionState::kInterrupted) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev,
                  ExecutionState::kInterrupted,
                  std::nullopt,
                  {{"reason", "terminal_authority_interrupt_latch"}}});
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kInterruptedFault,
                                      std::nullopt, std::nullopt, reason);
    return true;
  }
  if (current == ExecutionState::kStopping && stop_policy_.stop_requested() &&
      !operator_stop_worker_.active() && !motion_trip_stop_.active()) {
    // Hard Stop moves Executing -> Stopping, then asks the backend to stop,
    // then commits Stopped. The backend handle may report terminal completion
    // in the narrow window before the Stop API commits its terminal. That is
    // not a suspicious normal-completion race; it is the expected stop
    // authority concluding its own path.
    publish_operator_stop_certificate(exec_id, StopCertificateOutcome::kPartial,
                                      /*backend_stop_confirmed=*/false,
                                      "execute_terminal_observed_stop_pending");
    auto transition =
        commit_terminal(ExecutionState::kStopped, "terminal_completion_stop_requested",
                        TerminalCommitter::Origin::kStopAuthority);
    if (transition.committed == ExecutionState::kStopped) {
      auto stopped_snapshot = get_execution_snapshot();
      stopped_snapshot.timestamp_ns = clock_.now();
      stopped_snapshot.execution_id = exec_id;
      stopped_snapshot.move_id = request.move_id;
      stopped_snapshot.state = ExecutionState::kStopped;
      publish_execution_snapshot(std::move(stopped_snapshot));
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                        std::nullopt, std::nullopt,
                                        "terminal_completion_stop_requested");
      return true;
    }
  }
  if (current == ExecutionState::kStopping || operator_stop_worker_.active()) {
    ErrorDTO err{kErrorStopFailed,
                 "Execution backend reported terminal completion while stop "
                 "authority was still active",
                 {{"reason", "terminal_completion_stop_authority_active"},
                  {"phase", reason},
                  {"state", to_string(current)}}};
    sm_.set_last_error(error_code_from_dto(err));
    const auto prev = sm_.status().state;
    auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
    if (transition.ok()) {
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = exec_id;
      snapshot.move_id = request.move_id;
      snapshot.state = ExecutionState::kInterventionRequired;
      snapshot.error = err;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, err, err.details});
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "terminal_completion_stop_authority_active"}}});
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed, err,
                                      std::nullopt, reason);
    return true;
  }
  return false;
}

ExecutionKernel::ExecuteWorkerPlanPhaseResult ExecutionKernel::execute_worker_plan_phase(
    std::stop_token st, const ExecuteRequestDTO& request,
    const CollisionWorldLeaseInventory& projection_inventory, std::uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
    ExecutionState worker_entry_state, ActiveMotionExecution& motion_state) {
  ExecuteWorkerPlanPhaseResult result;

  if (st.stop_requested()) {
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "early_stop_requested");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  const bool cache_hit = cached_trajectory.has_value();
  result.active_trajectory_from_cache = cache_hit;
  result.cache_revalidation_candidate = cache_hit && cache_revalidation_candidate;
  if (cache_hit) {
    auto cached_request = make_plan_request(request, projection_inventory, 0U, nullptr);
    if (!cached_request.ok()) {
      const auto error = cached_request.error();
      emit_event(make_error_raised_event(exec_id, clock_.now(), error));
      sm_.set_last_error(error_code_from_dto(error));
      const auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
      finalize_motion_execution_outcome(motion_state,
                                        terminal_commit.committed == ExecutionState::kStopped
                                            ? MotionExecutionTerminalCause::kStopped
                                            : MotionExecutionTerminalCause::kFailed,
                                        error, std::nullopt, "cached_plan_evidence_failed");
      result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return result;
    }
    result.plan_request = std::move(cached_request.value());
    result.planning_result =
        PlanResultDTO{std::make_optional(*cached_trajectory), std::nullopt,
                      projection_inventory.projection_binding.projection_revision};
    result.trajectory = std::move(*cached_trajectory);
    return result;
  }

  ScheduleHandle planning_timeout_handle = 0;
  const auto planning_start_ns = clock_.now();
  const int64_t planning_timeout_ns =
      (request.timeouts.planning_timeout_s > 0)
          ? static_cast<int64_t>(request.timeouts.planning_timeout_s * 1e9)
          : 0;
  if (planning_timeout_ns > 0) {
    const int64_t deadline = planning_start_ns + planning_timeout_ns;
    // Monitoring lane: the watchdog must fire even when the planner
    // and collision-monitor are blocked behind the scene lock. See
    // spec/220 threading invariants and ADR-0019.
    planning_timeout_handle = scheduler_.schedule(
        deadline, [this]() { stop_policy_.request_hard_stop(); }, SchedulerPriority::Monitoring);
  }

  auto plan_result = plan_trajectory_with_evidence(request, projection_inventory);
  const bool planning_timed_out =
      planning_timeout_ns > 0 && (clock_.now() - planning_start_ns) >= planning_timeout_ns;

  if (planning_timeout_handle != 0) {
    scheduler_.cancel(planning_timeout_handle);
  }

  // Reset and the planner result need one linearization order, but the worker
  // must never acquire lifecycle_mutex_: stop() can hold that mutex while it
  // waits for backend acknowledgement and the planner worker must remain able
  // to finish. This leaf is held only through the plan-result state decision;
  // external callbacks remain deferred until after it is released.
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> planning_result_lock(planning_result_mutex_);

  if (planning_reset_requested_.load(std::memory_order_acquire)) {
    // Reset owns the state transition and backend reset. The planner cancel can
    // complete before Reset commits Idle, so this worker must not reinterpret
    // the cancellation result as a planning failure and race Reset for terminal
    // authority.
    planning_result_lock.unlock();
    event_scope.flush();
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "planning_reset_requested");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (planning_timed_out || stop_policy_.stop_requested()) {
    auto prev = worker_entry_state;  // B-2: use captured state
    const auto message = planning_timed_out ? "Planning timed out" : "Planning interrupted by stop";
    sm_.set_last_error(ErrorCode::kPlanningTimeout);
    emit_event(make_error_raised_event(exec_id, clock_.now(),
                                       ErrorDTO{kErrorPlanningTimeout, message, {}}));
    publish_operator_stop_certificate(exec_id, StopCertificateOutcome::kPartial,
                                      /*backend_stop_confirmed=*/false,
                                      "planning_worker_observed_stop_pending");
    auto terminal_commit = commit_terminal(ExecutionState::kStopped, "terminal_commit");
    planning_result_lock.unlock();
    event_scope.flush();
    if (terminal_commit.committed != ExecutionState::kStopped) {
      if (finalize_overridden_terminal_authority(request, exec_id, motion_state,
                                                 terminal_commit.committed,
                                                 "planning_stop_overridden_by_terminal_authority",
                                                 ErrorDTO{kErrorPlanningTimeout, message, {}})) {
        result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return result;
      }
    }
    auto stopped_snapshot = get_execution_snapshot();
    stopped_snapshot.timestamp_ns = clock_.now();
    stopped_snapshot.execution_id = exec_id;
    stopped_snapshot.move_id = request.move_id;
    stopped_snapshot.state = terminal_commit.committed;
    stopped_snapshot.error = ErrorDTO{kErrorPlanningTimeout, message, {}};
    publish_execution_snapshot(std::move(stopped_snapshot));
    emit_event(make_state_transition_event(exec_id, clock_.now(), prev, terminal_commit.committed));
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "planning_stop");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (sm_.status().state == ExecutionState::kIdle) {
    planning_result_lock.unlock();
    event_scope.flush();
    auto prev = worker_entry_state;  // B-2: use captured state
    sm_.set_last_error(ErrorCode::kPlanningTimeout);
    emit_event(make_error_raised_event(
        exec_id, clock_.now(),
        ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}}));
    auto idle_snapshot = get_execution_snapshot();
    idle_snapshot.timestamp_ns = clock_.now();
    idle_snapshot.execution_id = 0;
    idle_snapshot.move_id.clear();
    idle_snapshot.state = ExecutionState::kIdle;
    idle_snapshot.progress = 0.0;
    idle_snapshot.error = ErrorDTO{kErrorPlanningTimeout, "Planning interrupted by reset", {}};
    publish_execution_snapshot(std::move(idle_snapshot));
    emit_event(make_state_transition_event(exec_id, clock_.now(), prev, ExecutionState::kIdle));
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "planning_reset");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (!plan_result.ok()) {
    if (planning_cancelled_by_interrupt(plan_result.error()) &&
        sm_.status().state == ExecutionState::kInterrupted) {
      planning_result_lock.unlock();
      event_scope.flush();
      emit_event(make_error_raised_event(exec_id, clock_.now(), plan_result.error()));
      mark_active_interrupt_preemption_result(InterruptPreemptionResult::kSucceeded,
                                              {{"phase", "cancel_planning"}});
      auto interrupted_snapshot = get_execution_snapshot();
      interrupted_snapshot.timestamp_ns = clock_.now();
      interrupted_snapshot.execution_id = exec_id;
      interrupted_snapshot.move_id = request.move_id;
      interrupted_snapshot.state = ExecutionState::kInterrupted;
      interrupted_snapshot.error = plan_result.error();
      publish_execution_snapshot(std::move(interrupted_snapshot));
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt, std::nullopt,
          "planning_cancelled_by_interrupt");
      result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return result;
    }
    emit_event(make_error_raised_event(exec_id, clock_.now(), plan_result.error()));
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.plans_failed++;
    }
    sm_.set_last_error(error_code_from_dto(plan_result.error()));
    auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    planning_result_lock.unlock();
    event_scope.flush();
    if (terminal_commit.committed != ExecutionState::kFailed) {
      if (terminal_commit.committed == ExecutionState::kStopped) {
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                          std::nullopt, std::nullopt,
                                          "planning_failed_overridden_by_stop");
      } else {
        finalize_motion_execution_outcome(motion_state,
                                          terminal_commit.committed == ExecutionState::kInterrupted
                                              ? MotionExecutionTerminalCause::kInterruptedFault
                                              : MotionExecutionTerminalCause::kFailed,
                                          plan_result.error(), std::nullopt,
                                          "planning_failed_overridden_by_terminal_authority");
      }
      result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return result;
    }
    auto failed_snapshot = get_execution_snapshot();
    failed_snapshot.timestamp_ns = clock_.now();
    failed_snapshot.execution_id = exec_id;
    failed_snapshot.move_id = request.move_id;
    failed_snapshot.state = ExecutionState::kFailed;
    failed_snapshot.error = plan_result.error();
    publish_execution_snapshot(std::move(failed_snapshot));
    emit_event(make_state_transition_event(exec_id, clock_.now(), ExecutionState::kPlanning,
                                           ExecutionState::kFailed));
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      plan_result.error(), std::nullopt, "planning_failed");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  result.plan_request = std::move(plan_result.value().request);
  result.planning_result = std::move(plan_result.value().result);
  result.trajectory = *result.planning_result->trajectory;

  emit_event({KernelEventType::kPlanCompleted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.plans_succeeded++;
  }

  auto prev_state2 = sm_.status().state;
  auto r2 = sm_.try_transition(ExecutionState::kValidating);
  if (!r2.ok()) {
    sm_.set_last_error(ErrorCode::kInternalError);
    auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    planning_result_lock.unlock();
    event_scope.flush();
    if (terminal_commit.committed != ExecutionState::kFailed) {
      finalize_motion_execution_outcome(
          motion_state,
          terminal_commit.committed == ExecutionState::kStopped
              ? MotionExecutionTerminalCause::kStopped
              : (terminal_commit.committed == ExecutionState::kInterrupted
                     ? MotionExecutionTerminalCause::kInterruptedFault
                     : MotionExecutionTerminalCause::kFailed),
          std::nullopt, std::nullopt, "validating_transition_overridden_by_terminal_authority");
      result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return result;
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state2,
                ExecutionState::kFailed,
                std::nullopt,
                {}});
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        ErrorDTO{kErrorInternalError, "Failed to transition to Validating", {}}, std::nullopt,
        "validating_transition_failed");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }
  planning_result_lock.unlock();
  event_scope.flush();
  emit_event({KernelEventType::kStateTransition,
              exec_id,
              clock_.now(),
              prev_state2,
              ExecutionState::kValidating,
              std::nullopt,
              {}});
  auto validating_snapshot = get_execution_snapshot();
  validating_snapshot.timestamp_ns = clock_.now();
  validating_snapshot.execution_id = exec_id;
  validating_snapshot.move_id = request.move_id;
  validating_snapshot.state = ExecutionState::kValidating;
  validating_snapshot.progress = 0.0;
  publish_execution_snapshot(std::move(validating_snapshot));
  return result;
}

ExecutionKernel::ExecuteWorkerValidationPhaseResult
ExecutionKernel::execute_worker_validate_replan_phase(
    std::stop_token st, const ExecuteRequestDTO& request, std::uint64_t exec_id,
    TrajectoryDTO& trajectory, std::optional<PlanRequestDTO>& plan_request,
    std::optional<PlanResultDTO>& planning_result, bool& active_trajectory_from_cache,
    bool& cache_revalidation_candidate,
    CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
    const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
    ActiveMotionExecution& motion_state) {
  ExecuteWorkerValidationPhaseResult phase_result;
  const int max_authoritative_validation_attempts =
      std::max(cache_revalidation_candidate ? 2 : 1,
               1 + config_.authoritative_validation_collision_replan_attempts);
  int authoritative_validation_attempt = 0;
  Result<CollisionValidationSuccess> val_result =
      Result<CollisionValidationSuccess>::Err(ErrorCode::kInternalError);
  while (true) {
    emit_event({KernelEventType::kValidationStarted,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"attempt_index", std::to_string(authoritative_validation_attempt)},
                 {"max_attempts", std::to_string(max_authoritative_validation_attempts)}}});

    val_result = validate_trajectory(trajectory, request, scene_snapshot_lease_inventory,
                                     scene_snapshot_lease);
    if (val_result.ok()) {
      break;
    }
    const auto& validation_error = val_result.error();
    const bool failed_revalidation_candidate =
        cache_revalidation_candidate && active_trajectory_from_cache;
    if (failed_revalidation_candidate) {
      emit_event({KernelEventType::kCacheRevalidationFailed,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  validation_error,
                  {{"move_id", request.move_id}, {"reason", "current_validation_failed"}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_revalidations_failed++;
    }
    const bool can_replan_authoritative_collision =
        (failed_revalidation_candidate ||
         authoritative_collision_replan_candidate(validation_error)) &&
        authoritative_validation_attempt + 1 < max_authoritative_validation_attempts &&
        sm_.status().state == ExecutionState::kValidating && !st.stop_requested() &&
        !stop_policy_.stop_requested();
    if (!can_replan_authoritative_collision) {
      break;
    }

    std::map<std::string, std::string> retry_details = validation_error.details;
    retry_details["reason"] = failed_revalidation_candidate ? "cache_revalidation_failed_replan"
                                                            : "authoritative_validation_replan";
    retry_details["attempt_index"] = std::to_string(authoritative_validation_attempt);
    retry_details["next_attempt_index"] = std::to_string(authoritative_validation_attempt + 1);
    retry_details["max_attempts"] = std::to_string(max_authoritative_validation_attempts);
    retry_details["move_id"] = request.move_id;
    emit_contact_policy_violation_event(exec_id, validation_error.details);
    emit_event({KernelEventType::kValidationFailed,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                std::move(retry_details)});

    const auto cache_key = trajectory_cache_key(request);
    if (request.cache_mode != CacheMode::kDisable && cache_.contains(cache_key)) {
      cache_.invalidate(cache_key);
      emit_event({KernelEventType::kCacheInvalidated,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id},
                   {"reason", failed_revalidation_candidate ? "cache_revalidation_failed"
                                                            : "authoritative_validation_replan"}}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.cache_invalidations++;
      }
    }

    auto retry_transition_from = sm_.status().state;
    auto retry_transition = sm_.try_transition(ExecutionState::kPlanning);
    if (!retry_transition.ok()) {
      break;
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                retry_transition_from,
                ExecutionState::kPlanning,
                std::nullopt,
                {{"reason", "authoritative_validation_replan"},
                 {"attempt_index", std::to_string(authoritative_validation_attempt + 1)}}});
    auto planning_retry_snapshot = get_execution_snapshot();
    planning_retry_snapshot.timestamp_ns = clock_.now();
    planning_retry_snapshot.execution_id = exec_id;
    planning_retry_snapshot.move_id = request.move_id;
    planning_retry_snapshot.state = ExecutionState::kPlanning;
    planning_retry_snapshot.progress = 0.0;
    publish_execution_snapshot(std::move(planning_retry_snapshot));

    ++authoritative_validation_attempt;
    emit_event({KernelEventType::kPlanStarted,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "authoritative_validation_replan"},
                 {"attempt_index", std::to_string(authoritative_validation_attempt)}}});
    ScheduleHandle replan_timeout_handle = 0;
    const auto replan_start_ns = clock_.now();
    const int64_t replan_timeout_ns =
        (request.timeouts.planning_timeout_s > 0)
            ? static_cast<int64_t>(request.timeouts.planning_timeout_s * 1e9)
            : 0;
    if (replan_timeout_ns > 0) {
      const int64_t deadline = replan_start_ns + replan_timeout_ns;
      replan_timeout_handle = scheduler_.schedule(
          deadline, [this]() { stop_policy_.request_hard_stop(); }, SchedulerPriority::Monitoring);
    }
    auto replan_result =
        plan_trajectory_with_evidence(request, scene_snapshot_lease_inventory,
                                      static_cast<std::uint32_t>(authoritative_validation_attempt));
    const bool replan_timed_out =
        replan_timeout_ns > 0 && (clock_.now() - replan_start_ns) >= replan_timeout_ns;
    if (replan_timeout_handle != 0) {
      scheduler_.cancel(replan_timeout_handle);
    }

    const auto preserve_replan_terminal_authority = [&](const std::string& reason) -> bool {
      const auto current = sm_.status().state;
      if (current == ExecutionState::kStopped || current == ExecutionState::kStopping) {
        if (current == ExecutionState::kStopping) {
          publish_operator_stop_certificate(exec_id, StopCertificateOutcome::kPartial,
                                            /*backend_stop_confirmed=*/false,
                                            "replan_terminal_authority_stop_pending");
          (void)commit_terminal(ExecutionState::kStopped, "terminal_commit");
        }
        auto stopped_snapshot = get_execution_snapshot();
        stopped_snapshot.timestamp_ns = clock_.now();
        stopped_snapshot.execution_id = exec_id;
        stopped_snapshot.move_id = request.move_id;
        stopped_snapshot.state = ExecutionState::kStopped;
        publish_execution_snapshot(std::move(stopped_snapshot));
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                          std::nullopt, std::nullopt, reason);
        return true;
      }
      if (current == ExecutionState::kInterrupted ||
          interrupt_manager_.active_latch().has_value()) {
        if (current != ExecutionState::kInterrupted) {
          const auto before_interrupt = current;
          if (commit_terminal(ExecutionState::kInterrupted, "terminal_commit").ok()) {
            emit_event({KernelEventType::kStateTransition,
                        exec_id,
                        clock_.now(),
                        before_interrupt,
                        ExecutionState::kInterrupted,
                        std::nullopt,
                        {{"reason", reason}}});
          }
        }
        auto interrupted_snapshot = get_execution_snapshot();
        interrupted_snapshot.timestamp_ns = clock_.now();
        interrupted_snapshot.execution_id = exec_id;
        interrupted_snapshot.move_id = request.move_id;
        interrupted_snapshot.state = ExecutionState::kInterrupted;
        publish_execution_snapshot(std::move(interrupted_snapshot));
        finalize_motion_execution_outcome(motion_state,
                                          MotionExecutionTerminalCause::kInterruptedFault,
                                          std::nullopt, std::nullopt, reason);
        return true;
      }
      if (current == ExecutionState::kInterventionRequired) {
        auto intervention_snapshot = get_execution_snapshot();
        intervention_snapshot.timestamp_ns = clock_.now();
        intervention_snapshot.execution_id = exec_id;
        intervention_snapshot.move_id = request.move_id;
        intervention_snapshot.state = ExecutionState::kInterventionRequired;
        if (!intervention_snapshot.error.has_value()) {
          intervention_snapshot.error =
              ErrorDTO{kErrorStopFailed,
                       "Authoritative validation replan lost to intervention",
                       {{"reason", reason}}};
        }
        const auto intervention_error = *intervention_snapshot.error;
        publish_execution_snapshot(std::move(intervention_snapshot));
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                          intervention_error, std::nullopt, reason);
        return true;
      }
      return false;
    };

    const auto post_replan_state = sm_.status().state;
    if (replan_timed_out || stop_policy_.stop_requested() ||
        post_replan_state == ExecutionState::kStopped ||
        post_replan_state == ExecutionState::kStopping) {
      auto prev = sm_.status().state;
      const auto message = replan_timed_out ? "Planning timed out" : "Planning interrupted by stop";
      if (replan_timed_out) {
        sm_.set_last_error(ErrorCode::kPlanningTimeout);
        emit_event({KernelEventType::kErrorRaised,
                    exec_id,
                    clock_.now(),
                    {},
                    {},
                    ErrorDTO{kErrorPlanningTimeout, message, {}},
                    {}});
      }
      publish_operator_stop_certificate(exec_id, StopCertificateOutcome::kPartial,
                                        /*backend_stop_confirmed=*/false,
                                        "validation_replan_worker_observed_stop_pending");
      auto terminal_commit = commit_terminal(ExecutionState::kStopped, "terminal_commit");
      if (terminal_commit.committed != ExecutionState::kStopped) {
        if (finalize_overridden_terminal_authority(
                request, exec_id, motion_state, terminal_commit.committed,
                "validation_replan_stop_overridden_by_terminal_authority",
                replan_timed_out
                    ? std::optional<ErrorDTO>{ErrorDTO{kErrorPlanningTimeout, message, {}}}
                    : std::nullopt)) {
          phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
          return phase_result;
        }
      }
      auto stopped_snapshot = get_execution_snapshot();
      stopped_snapshot.timestamp_ns = clock_.now();
      stopped_snapshot.execution_id = exec_id;
      stopped_snapshot.move_id = request.move_id;
      stopped_snapshot.state = terminal_commit.committed;
      if (replan_timed_out) {
        stopped_snapshot.error = ErrorDTO{kErrorPlanningTimeout, message, {}};
      } else {
        stopped_snapshot.error.reset();
      }
      publish_execution_snapshot(std::move(stopped_snapshot));
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev,
                  terminal_commit.committed,
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan_stop"}}});
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                        std::nullopt, std::nullopt, "validation_replan_stop");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    if (sm_.status().state == ExecutionState::kIdle) {
      auto idle_snapshot = get_execution_snapshot();
      idle_snapshot.timestamp_ns = clock_.now();
      idle_snapshot.execution_id = 0;
      idle_snapshot.move_id.clear();
      idle_snapshot.state = ExecutionState::kIdle;
      idle_snapshot.progress = 0.0;
      publish_execution_snapshot(std::move(idle_snapshot));
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                        std::nullopt, std::nullopt, "validation_replan_reset");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    if (preserve_replan_terminal_authority("validation_replan_lost_to_terminal_authority")) {
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    if (!replan_result.ok()) {
      if (planning_cancelled_by_interrupt(replan_result.error()) &&
          sm_.status().state == ExecutionState::kInterrupted) {
        emit_event({KernelEventType::kErrorRaised,
                    exec_id,
                    clock_.now(),
                    {},
                    {},
                    replan_result.error(),
                    {}});
        mark_active_interrupt_preemption_result(InterruptPreemptionResult::kSucceeded,
                                                {{"phase", "cancel_planning"}});
        auto interrupted_snapshot = get_execution_snapshot();
        interrupted_snapshot.timestamp_ns = clock_.now();
        interrupted_snapshot.execution_id = exec_id;
        interrupted_snapshot.move_id = request.move_id;
        interrupted_snapshot.state = ExecutionState::kInterrupted;
        interrupted_snapshot.error = replan_result.error();
        publish_execution_snapshot(std::move(interrupted_snapshot));
        finalize_motion_execution_outcome(
            motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt,
            std::nullopt, "validation_replan_cancelled_by_interrupt");
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
      if (preserve_replan_terminal_authority(
              "validation_replan_failure_lost_to_terminal_authority")) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
      emit_event({KernelEventType::kErrorRaised,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  replan_result.error(),
                  {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.plans_failed++;
      }
      sm_.set_last_error(error_code_from_dto(replan_result.error()));
      auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
      if (terminal_commit.committed != ExecutionState::kFailed) {
        if (preserve_replan_terminal_authority(
                "validation_replan_failure_overridden_by_terminal_authority")) {
          phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
          return phase_result;
        }
      }
      auto failed_snapshot = get_execution_snapshot();
      failed_snapshot.timestamp_ns = clock_.now();
      failed_snapshot.execution_id = exec_id;
      failed_snapshot.move_id = request.move_id;
      failed_snapshot.state = ExecutionState::kFailed;
      failed_snapshot.error = replan_result.error();
      publish_execution_snapshot(std::move(failed_snapshot));
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  ExecutionState::kPlanning,
                  ExecutionState::kFailed,
                  std::nullopt,
                  {{"reason", "authoritative_validation_replan_failed"}}});
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                        replan_result.error(), std::nullopt,
                                        "validation_replan_failed");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }

    plan_request = std::move(replan_result.value().request);
    planning_result = std::move(replan_result.value().result);
    trajectory = *planning_result->trajectory;
    active_trajectory_from_cache = false;
    cache_revalidation_candidate = false;
    emit_event({KernelEventType::kPlanCompleted,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "authoritative_validation_replan"},
                 {"attempt_index", std::to_string(authoritative_validation_attempt)}}});
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.plans_succeeded++;
    }
    auto retry_validating_from = sm_.status().state;
    auto retry_validating_transition = sm_.try_transition(ExecutionState::kValidating);
    if (!retry_validating_transition.ok()) {
      if (preserve_replan_terminal_authority(
              "validation_replan_transition_lost_to_terminal_authority")) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
      sm_.set_last_error(ErrorCode::kInternalError);
      auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
      if (terminal_commit.committed != ExecutionState::kFailed) {
        if (preserve_replan_terminal_authority(
                "validation_replan_transition_overridden_by_terminal_"
                "authority")) {
          phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
          return phase_result;
        }
      }
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  retry_validating_from,
                  ExecutionState::kFailed,
                  std::nullopt,
                  {{"reason", "authoritative_replan_validating_failed"}}});
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kFailed,
          ErrorDTO{kErrorInternalError, "Failed to transition retry to Validating", {}},
          std::nullopt, "validation_replan_transition_failed");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                retry_validating_from,
                ExecutionState::kValidating,
                std::nullopt,
                {{"reason", "authoritative_validation_replan"},
                 {"attempt_index", std::to_string(authoritative_validation_attempt)}}});
    auto retry_validating_snapshot = get_execution_snapshot();
    retry_validating_snapshot.timestamp_ns = clock_.now();
    retry_validating_snapshot.execution_id = exec_id;
    retry_validating_snapshot.move_id = request.move_id;
    retry_validating_snapshot.state = ExecutionState::kValidating;
    retry_validating_snapshot.progress = 0.0;
    publish_execution_snapshot(std::move(retry_validating_snapshot));
  }
  if (val_result.ok()) {
    auto projection_result = build_runtime_collision_state_projection(
        trajectory, val_result.value().collision_trajectory);
    if (!projection_result.ok()) {
      val_result = Result<CollisionValidationSuccess>::Err(projection_result.error());
    } else {
      set_runtime_collision_state_projection(std::move(projection_result.value()));
    }
  }

  if (!val_result.ok()) {
    if (validation_cancelled_by_interrupt(val_result.error()) &&
        sm_.status().state == ExecutionState::kInterrupted) {
      emit_event({KernelEventType::kValidationFailed,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "validation cancelled by interrupt"}}});
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, val_result.error(), {}});
      mark_active_interrupt_preemption_result(InterruptPreemptionResult::kSucceeded);
      auto interrupted_snapshot = get_execution_snapshot();
      interrupted_snapshot.timestamp_ns = clock_.now();
      interrupted_snapshot.execution_id = exec_id;
      interrupted_snapshot.move_id = request.move_id;
      interrupted_snapshot.state = ExecutionState::kInterrupted;
      interrupted_snapshot.error = val_result.error();
      publish_execution_snapshot(std::move(interrupted_snapshot));
      finalize_motion_execution_outcome(
          motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt, std::nullopt,
          "validation_cancelled_by_interrupt");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    emit_contact_policy_violation_event(exec_id, val_result.error().details);
    emit_event({KernelEventType::kValidationFailed,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "validation failed"}}});
    emit_event(
        {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, val_result.error(), {}});
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.validations_failed++;
    }
    sm_.set_last_error(error_code_from_dto(val_result.error()));
    auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    if (terminal_commit.committed != ExecutionState::kFailed) {
      if (finalize_overridden_terminal_authority(
              request, exec_id, motion_state, terminal_commit.committed,
              "validation_failed_overridden_by_terminal_authority", val_result.error())) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = ExecutionState::kFailed;
    snapshot.error = val_result.error();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kValidating,
                ExecutionState::kFailed,
                std::nullopt,
                {}});
    const auto cache_key = trajectory_cache_key(request);
    if (request.cache_mode != CacheMode::kDisable && cache_.contains(cache_key)) {
      cache_.invalidate(cache_key);
      emit_event({KernelEventType::kCacheInvalidated,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id}, {"reason", "validation_failed"}}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.cache_invalidations++;
      }
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      val_result.error(), std::nullopt, "validation_failed");
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return phase_result;
  }

  const auto& validation_telemetry = val_result.value().telemetry;
  emit_event(
      {KernelEventType::kValidationPassed,
       exec_id,
       clock_.now(),
       {},
       {},
       std::nullopt,
       {{"collision_workers_requested", std::to_string(validation_telemetry.workers_requested)},
        {"collision_workers_effective", std::to_string(validation_telemetry.workers_effective)},
        {"collision_workers_used", std::to_string(validation_telemetry.workers_actually_used)},
        {"collision_parallel_fallback_reason", validation_telemetry.fallback_reason},
        {"collision_ordered_contact_summary_used",
         validation_telemetry.ordered_contact_summary_used ? "true" : "false"},
        {"collision_validation_wall_ns", std::to_string(validation_telemetry.total_wall_ns)},
        {"dynamically_discovered_start_contact_count",
         std::to_string(val_result.value().dynamically_discovered_start_contact_count)}}});
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.validations_passed++;
  }

  if (stop_policy_.stop_requested()) {
    auto prev = sm_.status().state;
    sm_.set_last_error(ErrorCode::kInternalError);
    emit_event({KernelEventType::kErrorRaised,
                exec_id,
                clock_.now(),
                {},
                {},
                ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}},
                {}});
    publish_operator_stop_certificate(exec_id, StopCertificateOutcome::kPartial,
                                      /*backend_stop_confirmed=*/false,
                                      "validation_worker_observed_stop_pending");
    auto terminal_commit = commit_terminal(ExecutionState::kStopped, "terminal_commit");
    if (terminal_commit.committed != ExecutionState::kStopped) {
      if (finalize_overridden_terminal_authority(
              request, exec_id, motion_state, terminal_commit.committed,
              "validation_stop_overridden_by_terminal_authority",
              ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}})) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = terminal_commit.committed;
    snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by stop", {}};
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev,
                terminal_commit.committed,
                std::nullopt,
                {}});
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "validation_stop");
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return phase_result;
  }

  if (sm_.status().state == ExecutionState::kIdle) {
    auto prev = sm_.status().state;
    sm_.set_last_error(ErrorCode::kInternalError);
    emit_event({KernelEventType::kErrorRaised,
                exec_id,
                clock_.now(),
                {},
                {},
                ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}},
                {}});
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error = ErrorDTO{kErrorInternalError, "Validation interrupted by reset", {}};
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev,
                ExecutionState::kIdle,
                std::nullopt,
                {}});
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "validation_reset");
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return phase_result;
  }

  phase_result.collision_world_revision = val_result.value().collision_world_revision;
  phase_result.validation_evidence = PreparedMotionValidationEvidenceDTO{
      config_.validation_revision, val_result.value().noncollision_result,
      val_result.value().collision_world_revision, val_result.value().collision_policy_revision,
      val_result.value().collision_decision};
  phase_result.scene_snapshot_lease = std::move(val_result.value().scene_snapshot_lease);
  phase_result.collision_trajectory =
      std::make_shared<const TrajectoryDTO>(std::move(val_result.value().collision_trajectory));
  return phase_result;
}

ExecutionKernel::ExecuteWorkerDispatchPhaseResult ExecutionKernel::execute_worker_dispatch_phase(
    const ExecuteRequestDTO& request, std::uint64_t exec_id, const TrajectoryDTO& trajectory,
    bool active_trajectory_from_cache, bool cache_revalidation_candidate,
    const ProjectionLeaseBindingDTO& projection_binding,
    const CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
    const CollisionWorldRevision& validation_world_revision,
    const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
    std::shared_ptr<const TrajectoryDTO> collision_trajectory,
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_participant,
    ActiveMotionExecution& motion_state) {
  ExecuteWorkerDispatchPhaseResult phase_result;

  const auto fail_scene_snapshot_gate = [&](ErrorDTO stale_error, const char* override_reason,
                                            const char* terminal_reason) -> bool {
    const auto prev = sm_.status().state;
    sm_.set_last_error(error_code_from_dto(stale_error));
    auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    if (terminal_commit.committed != ExecutionState::kFailed) {
      if (finalize_overridden_terminal_authority(request, exec_id, motion_state,
                                                 terminal_commit.committed, override_reason,
                                                 stale_error)) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return true;
      }
    }
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, stale_error, {}});
    auto stale_snapshot = get_execution_snapshot();
    stale_snapshot.timestamp_ns = clock_.now();
    stale_snapshot.execution_id = exec_id;
    stale_snapshot.move_id = request.move_id;
    stale_snapshot.state = ExecutionState::kFailed;
    stale_snapshot.error = stale_error;
    publish_execution_snapshot(std::move(stale_snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev,
                ExecutionState::kFailed,
                std::nullopt,
                {{"reason", terminal_reason}}});
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      stale_error, std::nullopt, terminal_reason);
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return true;
  };

  if (collision_world_revision_is_set(validation_world_revision)) {
    if (scene_snapshot_lease.has_value()) {
      auto validation_context = current_scene_snapshot_lease_validation_context();
      SceneSnapshotLeaseValidationContext missing_context;
      missing_context.now_ns = clock_.now();
      missing_context.default_lease_max_age_ns = config_.scene_snapshot_lease_max_age_ns;
      auto lease_result = validate_scene_snapshot_lease(
          *scene_snapshot_lease,
          validation_context.has_value() ? *validation_context : missing_context);
      if (!lease_result.accepted) {
        std::map<std::string, std::string> details = std::move(lease_result.details);
        details["reason"] = lease_result.reason_code.empty() ? "scene_snapshot_lease_rejected"
                                                             : lease_result.reason_code;
        details["reason_code"] = details["reason"];
        details["scene_snapshot_lease_id"] = lease_result.lease_id;
        details["validation_collision_world_revision_id"] =
            std::to_string(lease_result.validated_world_revision.revision_id);
        details["validation_collision_world_content_hash"] =
            lease_result.validated_world_revision.content_hash;
        details["live_collision_world_revision_id"] =
            std::to_string(lease_result.live_world_revision.revision_id);
        details["live_collision_world_content_hash"] =
            lease_result.live_world_revision.content_hash;
        details["validation_collision_policy_revision_id"] =
            std::to_string(lease_result.validated_policy_revision.revision_id);
        details["live_collision_policy_revision_id"] =
            std::to_string(lease_result.live_policy_revision.revision_id);
        if (lease_result.object_id.has_value()) {
          details["object_id"] = *lease_result.object_id;
        }
        if (lease_result.source_id.has_value()) {
          details["source_id"] = *lease_result.source_id;
        }
        if (lease_result.lease_age_ns.has_value()) {
          details["scene_snapshot_lease_age_ns"] = std::to_string(*lease_result.lease_age_ns);
        }
        if (lease_result.source_age_ns.has_value()) {
          details["observed_scene_source_age_ns"] = std::to_string(*lease_result.source_age_ns);
        }
        if (lease_result.target_drift_m.has_value()) {
          details["target_drift_m"] = diagnostic_double(*lease_result.target_drift_m);
        }
        if (lease_result.target_angular_drift_rad.has_value()) {
          details["target_angular_drift_rad"] =
              diagnostic_double(*lease_result.target_angular_drift_rad);
        }
        if (lease_result.obstacle_drift_m.has_value()) {
          details["obstacle_drift_m"] = diagnostic_double(*lease_result.obstacle_drift_m);
        }
        if (lease_result.obstacle_angular_drift_rad.has_value()) {
          details["obstacle_angular_drift_rad"] =
              diagnostic_double(*lease_result.obstacle_angular_drift_rad);
        }

        std::string message = "Collision world changed after validation and before dispatch";
        if (lease_result.rejection == SceneSnapshotLeaseRejection::kTargetMovedAfterValidation) {
          message = "Observed target moved after validation and before dispatch";
        } else if (lease_result.rejection ==
                   SceneSnapshotLeaseRejection::kObstacleMovedAfterValidation) {
          message = "Observed obstacle moved after validation and before dispatch";
        } else if (lease_result.rejection ==
                   SceneSnapshotLeaseRejection::kCollisionPolicyChangedAfterValidation) {
          message = "Collision policy changed after validation and before dispatch";
        }

        ErrorDTO stale_error{kErrorSceneChangedDuringValidation, std::move(message),
                             std::move(details)};
        if (fail_scene_snapshot_gate(std::move(stale_error),
                                     "dispatch_scene_snapshot_lease_overridden_by_terminal_"
                                     "authority",
                                     "dispatch_scene_snapshot_lease_rejected")) {
          return phase_result;
        }
      } else if (lease_result.details.find("accepted_live_world_revision_divergence") !=
                     lease_result.details.end() &&
                 scene_snapshot_lease->objects.empty()) {
        auto details = std::move(lease_result.details);
        details["reason"] = "scene_snapshot_lease_missing_collision_coverage";
        details["reason_code"] = details["reason"];
        details["scene_snapshot_lease_id"] = lease_result.lease_id;
        details["validation_collision_world_revision_id"] =
            std::to_string(lease_result.validated_world_revision.revision_id);
        details["validation_collision_world_content_hash"] =
            lease_result.validated_world_revision.content_hash;
        details["live_collision_world_revision_id"] =
            std::to_string(lease_result.live_world_revision.revision_id);
        details["live_collision_world_content_hash"] =
            lease_result.live_world_revision.content_hash;
        ErrorDTO stale_error{kErrorSceneChangedDuringValidation,
                             "Collision world changed without lease-covered collision objects",
                             std::move(details)};
        if (fail_scene_snapshot_gate(std::move(stale_error),
                                     "dispatch_scene_snapshot_lease_overridden_by_terminal_"
                                     "authority",
                                     "dispatch_scene_snapshot_lease_missing_coverage")) {
          return phase_result;
        }
      } else if (lease_result.details.find("accepted_live_world_revision_divergence") !=
                 lease_result.details.end()) {
        auto details = std::move(lease_result.details);
        details["reason"] = "scene_snapshot_lease_live_divergence_accepted";
        details["reason_code"] = details["reason"];
        details["scene_snapshot_lease_id"] = lease_result.lease_id;
        details["validation_collision_world_revision_id"] =
            std::to_string(lease_result.validated_world_revision.revision_id);
        details["validation_collision_world_content_hash"] =
            lease_result.validated_world_revision.content_hash;
        details["live_collision_world_revision_id"] =
            std::to_string(lease_result.live_world_revision.revision_id);
        details["live_collision_world_content_hash"] =
            lease_result.live_world_revision.content_hash;
        emit_event({KernelEventType::kValidationPassed,
                    exec_id,
                    clock_.now(),
                    {},
                    {},
                    std::nullopt,
                    std::move(details)});
      }
    } else {
      const auto live_revision = current_authoritative_collision_world_revision();
      const bool live_revision_available = collision_world_revision_is_set(live_revision);
      if (!live_revision_available ||
          !collision_world_revisions_equal(live_revision, validation_world_revision)) {
        std::map<std::string, std::string> details;
        details["reason"] = live_revision_available
                                ? "collision_world_changed_after_validation"
                                : "collision_world_revision_unavailable_before_dispatch";
        details["validation_collision_world_revision_id"] =
            std::to_string(validation_world_revision.revision_id);
        details["validation_collision_world_content_hash"] = validation_world_revision.content_hash;
        details["live_collision_world_revision_id"] = std::to_string(live_revision.revision_id);
        details["live_collision_world_content_hash"] = live_revision.content_hash;
        ErrorDTO stale_error{kErrorSceneChangedDuringValidation,
                             "Collision world changed after validation and before dispatch",
                             std::move(details)};
        if (fail_scene_snapshot_gate(std::move(stale_error),
                                     "dispatch_revision_changed_overridden_by_terminal_authority",
                                     "dispatch_revision_changed")) {
          return phase_result;
        }
      }
    }
  }

  // Store against both the exact validation revision and the resolved planner
  // intent. This prevents same-endpoint Cartesian paths, speed profiles, or
  // per-joint cap variants from sharing a move_id cache entry.
  const auto dispatch_cache_key = trajectory_cache_key(request);
  if (request.cache_mode != CacheMode::kDisable && dispatch_cache_key.valid() &&
      (!active_trajectory_from_cache || cache_revalidation_candidate) &&
      collision_world_revision_is_set(validation_world_revision)) {
    // Tag every cache entry with the immutable snapshot revision that
    // validation actually used. If live pose-source updates publish newer
    // revisions before the next lookup, lookup_with_revision declines the hit.
    cache_.store(dispatch_cache_key, trajectory,
                 TrajectoryCacheRevisionEvidence{projection_binding.projection_revision,
                                                 validation_world_revision},
                 trajectory_cache_intent_identity(request));
    if (cache_revalidation_candidate) {
      emit_event({KernelEventType::kCacheRevalidated,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id},
                   {"revision_id", std::to_string(validation_world_revision.revision_id)}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_revalidations_succeeded++;
    }
  }

  {
    auto arm_result = arm_motion_scopes_for_dispatch(request, exec_id, motion_state);
    if (!arm_result.ok()) {
      auto prev = sm_.status().state;
      // Stamp the state machine with the specific arm-failure code
      // so audit/release evidence sees `motion_interrupt_arm_failed`
      // (or `motion_interrupt_source_active_at_arm`) rather than a
      // generic `internal_error`.
      sm_.set_last_error(error_code_from_dto(arm_result.error()));
      auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
      if (terminal_commit.committed != ExecutionState::kFailed) {
        if (finalize_overridden_terminal_authority(
                request, exec_id, motion_state, terminal_commit.committed,
                "arm_failed_overridden_by_terminal_authority", arm_result.error())) {
          phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
          return phase_result;
        }
      }
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, arm_result.error(), {}});
      auto arm_fail_snapshot = get_execution_snapshot();
      arm_fail_snapshot.timestamp_ns = clock_.now();
      arm_fail_snapshot.execution_id = exec_id;
      arm_fail_snapshot.move_id = request.move_id;
      arm_fail_snapshot.state = ExecutionState::kFailed;
      arm_fail_snapshot.error = arm_result.error();
      publish_execution_snapshot(std::move(arm_fail_snapshot));
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev,
                  ExecutionState::kFailed,
                  std::nullopt,
                  {}});
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                        arm_result.error(), std::nullopt, "arm_failed");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    if (sm_.status().state == ExecutionState::kInterrupted) {
      auto latch = interrupt_manager_.active_latch();
      if (latch && latch->active_execution_id == exec_id &&
          latch->preemption_action_requested == InterruptPreemptionAction::kCompleteMoveOnSignal) {
        auto latest_snapshot = get_execution_snapshot();
        latest_snapshot.timestamp_ns = clock_.now();
        latest_snapshot.execution_id = exec_id;
        latest_snapshot.move_id = request.move_id;
        handle_motion_trip_latch(request, *latch, exec_id, latest_snapshot, motion_state,
                                 std::stop_token{});
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
      if (latch && latch->active_execution_id == exec_id) {
        finalize_motion_execution_outcome(motion_state,
                                          MotionExecutionTerminalCause::kInterruptedFault,
                                          std::nullopt, std::nullopt, "arm_time_interrupt_latched");
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
    }
  }

  auto prev_state3 = sm_.status().state;
  auto r3 = sm_.try_transition(ExecutionState::kExecuting);
  if (!r3.ok()) {
    sm_.set_last_error(ErrorCode::kInternalError);
    auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    if (terminal_commit.committed != ExecutionState::kFailed) {
      if (finalize_overridden_terminal_authority(
              request, exec_id, motion_state, terminal_commit.committed,
              "executing_transition_overridden_by_terminal_authority")) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state3,
                ExecutionState::kFailed,
                std::nullopt,
                {}});
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        ErrorDTO{kErrorInternalError, "Failed to transition to Executing", {}}, std::nullopt,
        "state_transition_failed");
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return phase_result;
  }
  emit_event({KernelEventType::kStateTransition,
              exec_id,
              clock_.now(),
              prev_state3,
              ExecutionState::kExecuting,
              std::nullopt,
              {}});

  emit_event({KernelEventType::kExecuteStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
  auto snapshot = get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = request.move_id;
  snapshot.state = ExecutionState::kExecuting;
  publish_execution_snapshot(std::move(snapshot));

  phase_result.exec_start_ns = clock_.now();
  phase_result.exec_timeout_ns =
      (request.timeouts.execution_timeout_s > 0)
          ? static_cast<int64_t>(request.timeouts.execution_timeout_s * 1e9)
          : 0;
  motion_state.runtime_collision_protection_generation =
      start_collision_monitoring("execute_dispatch", RuntimeCollisionProtectionPurpose::kMotion,
                                 &scene_snapshot_lease_inventory);
  phase_result.execution_handle =
      prepared_participant
          ? execution_backend_.Dispatch(*prepared_participant, request.binding.controller_goal_id,
                                        request.binding.controller_goal_generation)
          : execution_backend_.execute_trajectory(trajectory);
  if (prepared_participant) {
    if (const auto invalid = prepared_execution_handle_field_error(
            phase_result.execution_handle, *prepared_participant,
            request.binding.controller_goal_id, request.binding.controller_goal_generation)) {
      ErrorDTO dispatch_error{std::string(to_string(ErrorCode::kInvariantViolation)),
                              "Backend returned a handle outside the prepared dispatch authority",
                              {{"reason", std::string(*invalid)}}};
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                motion_state.runtime_collision_protection_generation);
      sm_.set_last_error(ErrorCode::kInvariantViolation);
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, dispatch_error, {}});
      const auto previous_state = sm_.status().state;
      auto terminal_commit =
          commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
      if (terminal_commit.committed != ExecutionState::kInterventionRequired &&
          finalize_overridden_terminal_authority(
              request, exec_id, motion_state, terminal_commit.committed,
              "prepared_dispatch_membership_overridden_by_terminal_authority", dispatch_error)) {
        phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return phase_result;
      }
      auto failed_snapshot = get_execution_snapshot();
      failed_snapshot.timestamp_ns = clock_.now();
      failed_snapshot.execution_id = exec_id;
      failed_snapshot.move_id = request.move_id;
      failed_snapshot.state = terminal_commit.committed;
      failed_snapshot.error = dispatch_error;
      publish_execution_snapshot(std::move(failed_snapshot));
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  previous_state,
                  terminal_commit.committed,
                  dispatch_error,
                  {{"reason", "prepared_dispatch_membership_invalid"}}});
      finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                        dispatch_error, std::nullopt,
                                        "prepared_dispatch_membership_invalid");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
  }
  arm_runtime_collision_lookahead(exec_id, phase_result.execution_handle.execution_generation,
                                  std::move(collision_trajectory), request.contact_policy_id,
                                  scene_snapshot_lease_inventory);
  return phase_result;
}

void ExecutionKernel::execute_worker_finalize_phase(const ExecuteRequestDTO& request,
                                                    std::uint64_t exec_id,
                                                    ExecutionHandle& execution_handle,
                                                    ActiveMotionExecution& motion_state) {
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced,
                            motion_state.runtime_collision_protection_generation);

  if (finalize_competing_terminal_authority(request, exec_id, motion_state,
                                            "terminal_authority_pre_final_snapshot")) {
    return;
  }

  if (sm_.status().state == ExecutionState::kInterventionRequired) {
    auto intervention_snapshot = get_execution_snapshot();
    intervention_snapshot.timestamp_ns = clock_.now();
    intervention_snapshot.execution_id = exec_id;
    intervention_snapshot.move_id = request.move_id;
    intervention_snapshot.state = ExecutionState::kInterventionRequired;
    if (!intervention_snapshot.error.has_value()) {
      std::optional<ErrorDTO> operator_stop_error = operator_stop_worker_.intervention_error();
      std::optional<ErrorDTO> fault_preempt_error =
          fault_preempt_intervention_error_for_active_latch();
      intervention_snapshot.error = operator_stop_error.value_or(
          fault_preempt_error.value_or(ErrorDTO{kErrorStopFailed,
                                                "Execution entered InterventionRequired without "
                                                "captured error",
                                                {{"reason", "missing_error_on_intervention"}}}));
    }
    const auto intervention_error = *intervention_snapshot.error;
    publish_execution_snapshot(std::move(intervention_snapshot));
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      intervention_error, std::nullopt,
                                      "execution_intervention_required");
    return;
  }

  core::ExecutionSnapshot final_snapshot;
  if (execution_handle.get_snapshot) {
    final_snapshot = execution_handle.get_snapshot();
    final_snapshot.execution_id = exec_id;
    final_snapshot.move_id = request.move_id;
    if ((final_snapshot.state == ExecutionState::kStopped ||
         sm_.status().state == ExecutionState::kStopped) &&
        !final_snapshot.error.has_value()) {
      std::optional<ErrorDTO> operator_stop_error = operator_stop_worker_.terminal_error();
      if (operator_stop_error.has_value()) {
        final_snapshot.error = *operator_stop_error;
      }
    }
    publish_execution_snapshot(final_snapshot);
    sm_.set_progress(final_snapshot.progress);
  } else {
    final_snapshot.execution_id = exec_id;
    final_snapshot.move_id = request.move_id;
    final_snapshot.state = ExecutionState::kCompleted;
    final_snapshot.progress = 1.0;
    publish_execution_snapshot(final_snapshot);
    sm_.set_progress(1.0);
  }

  if (final_snapshot.state == ExecutionState::kStopped ||
      sm_.status().state == ExecutionState::kStopped ||
      sm_.status().state == ExecutionState::kIdle) {
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                      std::nullopt, std::nullopt, "execution_stop_terminal");
    return;
  }

  if (final_snapshot.state == ExecutionState::kFailed) {
    if (final_snapshot.error.has_value()) {
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, final_snapshot.error, {}});
      sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
    } else {
      sm_.set_last_error(ErrorCode::kInternalError);
    }
    auto prev_state4 = sm_.status().state;
    auto failed_transition = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    if (failed_transition.committed != ExecutionState::kFailed) {
      if (finalize_competing_terminal_authority(
              request, exec_id, motion_state,
              "execution_failed_overridden_by_terminal_authority")) {
        return;
      }
    }
    auto snapshot = final_snapshot;
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = failed_transition.committed;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state4,
                failed_transition.committed,
                std::nullopt,
                {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        final_snapshot.error.value_or(ErrorDTO{kErrorInternalError,
                                               "Execution failed without captured error",
                                               {{"reason", "missing_error_on_failed"}}}),
        std::nullopt, "execution_failed");
    return;
  }

  if (final_snapshot.state == ExecutionState::kInterventionRequired) {
    if (final_snapshot.error.has_value()) {
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, final_snapshot.error, {}});
      sm_.set_last_error(error_code_from_dto(*final_snapshot.error));
    } else {
      sm_.set_last_error(ErrorCode::kStopFailed);
    }
    auto prev_state4 = sm_.status().state;
    auto intervention_transition =
        commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
    if (intervention_transition.committed != ExecutionState::kInterventionRequired) {
      if (finalize_competing_terminal_authority(
              request, exec_id, motion_state,
              "execution_intervention_overridden_by_terminal_authority")) {
        return;
      }
    }
    auto snapshot = final_snapshot;
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = intervention_transition.committed;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state4,
                intervention_transition.committed,
                std::nullopt,
                {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(
        motion_state, MotionExecutionTerminalCause::kFailed,
        final_snapshot.error.value_or(ErrorDTO{kErrorStopFailed,
                                               "Execution required intervention without captured "
                                               "error",
                                               {{"reason", "missing_error_on_intervention"}}}),
        std::nullopt, "execution_intervention_required");
    return;
  }

  auto prev_state4 = sm_.status().state;
  CompletionPublicationGuard completion_publication{completion_publication_execution_id_, exec_id};
  auto completed_transition = commit_terminal(ExecutionState::kCompleted, "terminal_commit");
  if (completed_transition.committed != ExecutionState::kCompleted) {
    if (finalize_competing_terminal_authority(
            request, exec_id, motion_state, "normal_completion_overridden_by_terminal_authority")) {
      return;
    }
  }
  if (!completed_transition.ok()) {
    if (finalize_competing_terminal_authority(request, exec_id, motion_state,
                                              "normal_completion_transition_rejected")) {
      return;
    }
    ErrorDTO err{
        kErrorInternalError,
        "Execution backend reported completion, but the kernel could not "
        "commit Completed as terminal authority",
        {{"reason", "completed_transition_rejected"}, {"from_state", to_string(prev_state4)}}};
    sm_.set_last_error(error_code_from_dto(err));
    auto failure_transition =
        commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
    auto rejected_snapshot = final_snapshot;
    rejected_snapshot.timestamp_ns = clock_.now();
    rejected_snapshot.execution_id = exec_id;
    rejected_snapshot.move_id = request.move_id;
    rejected_snapshot.state = sm_.status().state;
    rejected_snapshot.error = err;
    publish_execution_snapshot(std::move(rejected_snapshot));
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, err, err.details});
    if (failure_transition.ok()) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  prev_state4,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {{"reason", "completed_transition_rejected"}}});
    }
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed, err,
                                      std::nullopt, "normal_completion_transition_rejected");
    return;
  }

  emit_event({KernelEventType::kExecuteCompleted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});

  auto snapshot2 = final_snapshot;
  snapshot2.timestamp_ns = clock_.now();
  snapshot2.execution_id = exec_id;
  snapshot2.move_id = request.move_id;
  snapshot2.state = ExecutionState::kCompleted;
  snapshot2.progress = 1.0;
  publish_execution_snapshot(std::move(snapshot2));
  emit_event({KernelEventType::kStateTransition,
              exec_id,
              clock_.now(),
              prev_state4,
              ExecutionState::kCompleted,
              std::nullopt,
              {}});
  {
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.executions_completed++;
  }
  // Trajectory reached its planned endpoint. Cause is
  // `completed_normally` for free moves; the finalize helper rewrites
  // to `no_signal_trip` when the request carried scopes (spec/210
  // §3.11 invariants).
  finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kCompletedNormally,
                                    std::nullopt, std::nullopt, "execution_completed");
}

Result<std::shared_ptr<const PreparedOperationParticipantDTO>>
ExecutionKernel::make_and_prepare_motion_participant(
    const ExecuteRequestDTO& request, const TrajectoryDTO& trajectory,
    const ExecuteWorkerPlanPhaseResult& plan_phase,
    const ExecuteWorkerValidationPhaseResult& validation_phase,
    const std::shared_ptr<PreparedMotionSession>& prepared_session) {
  const auto reject = [](std::string reason, std::string message) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(
        ErrorDTO{kErrorExecutionRejected, std::move(message), {{"reason", std::move(reason)}}});
  };
  if (!prepared_session || !plan_phase.plan_request || !plan_phase.planning_result ||
      !validation_phase.validation_evidence) {
    return reject("prepared_motion_evidence_incomplete",
                  "Prepared motion is missing planning or validation evidence");
  }

  const auto& authority = prepared_session->authority;
  const auto& binding = authority.binding.value();
  if (!binding.control_path) {
    return reject("prepared_motion_control_path_missing",
                  "Prepared motion requires one sealed control path");
  }
  const auto& path = *binding.control_path;

  PlanRequestDTO canonical_plan_request = *plan_phase.plan_request;
  canonical_plan_request.retained_collision_world.reset();
  canonical_plan_request.retained_collision_policy.reset();

  std::vector<PreparedMotionInterruptScopeDTO> prepared_scopes;
  prepared_scopes.reserve(request.motion_interrupt_scopes.size());
  for (const auto& scope : request.motion_interrupt_scopes) {
    if (!scope.stop_profile) {
      return reject("prepared_motion_interrupt_stop_profile_missing",
                    "Every prepared motion interrupt scope requires its exact Stop profile");
    }
    PreparedParticipantStopPlanContextDTO stop_context{
        request.binding.operation_id,
        authority.operation_revision,
        request.binding.participant_id,
        request.binding.program_id,
        request.binding.program_revision,
        request.binding.program_binding_id,
        request.binding.program_run_id,
        scope.stop_profile,
        ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
        scope.scope_id,
        scope.node_category};
    auto stop_plan = MakePreparedParticipantStopPlan(
        authority.stop_readiness.value().resolved_stop_base_plan, std::move(stop_context),
        binding.resources, authority.retained_default_stop_limits);
    if (!stop_plan.ok()) {
      return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(stop_plan.error());
    }
    prepared_scopes.push_back(PreparedMotionInterruptScopeDTO{
        MotionInterruptScopeId(scope.scope_id), InterruptPolicyId(scope.policy_id),
        scope.activation_mode, scope.arm_timeout_ns, scope.result_binding_key, scope.required,
        *scope.stop_profile, scope.node_category, scope.metadata, std::move(stop_plan.value())});
  }
  std::sort(prepared_scopes.begin(), prepared_scopes.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.scope_id < rhs.scope_id; });

  std::optional<PreparedContactPolicyRefDTO> contact_policy;
  if (request.resolved_contact_grasp) {
    if (request.contact_policy_id != request.resolved_contact_grasp->contact_policy_id.value()) {
      return reject("prepared_motion_contact_policy_mismatch",
                    "Resolved contact policy differs from the admitted request");
    }
    contact_policy =
        PreparedContactPolicyRefDTO{request.resolved_contact_grasp->contact_policy_id,
                                    request.resolved_contact_grasp->contact_policy_revision};
  } else if (!request.contact_policy_id.empty()) {
    return reject("prepared_motion_contact_policy_revision_missing",
                  "Prepared motion cannot self-attest an unversioned contact policy");
  }

  PreparedMotionExecutionDTO payload{
      MoveId(request.move_id),
      std::move(canonical_plan_request),
      trajectory,
      plan_phase.plan_request->start_state,
      PreparedMotionPlanningEvidenceDTO{*plan_phase.planning_result,
                                        plan_phase.plan_request->planning_attempt_index,
                                        plan_phase.plan_request->planner_capability_binding},
      *validation_phase.validation_evidence,
      std::move(contact_policy),
      PreparedRuntimePolicyRefDTO{authority.runtime_policy.value().policy.runtime_policy_id,
                                  authority.runtime_policy.value().policy.runtime_policy_revision},
      std::move(prepared_scopes),
      request.timeouts.execution_timeout_s};

  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());

  std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider;
  if (binding.capability_provider_path) {
    const auto& provider = *binding.capability_provider_path;
    capability_provider = PreparedCapabilityProviderRuntimeOccurrenceDTO{
        provider.capability_provider_binding_id, provider.capability_provider_binding_revision,
        PreparedProviderRuntimeOccurrenceDTO{
            provider.provider_instance_id, provider.provider_manifest_id,
            provider.provider_manifest_revision, provider.provider_runtime_unit_id,
            provider.provider_runtime_epoch}};
  }

  PreparedOperationParticipantDTO candidate{
      ContentDigest("pending"),
      request.binding.operation_id,
      authority.operation_revision,
      request.binding.participant_id,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      request.binding.program_run_id,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.controllable_component_id,
      authority.stop_readiness.value().component_lifecycle_generation,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      binding.model_bindings,
      binding.frame_bindings,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      path.control_interface_contract_id,
      path.control_interface_revision,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      path.activation_set_id,
      PreparedProviderRuntimeOccurrenceDTO{
          path.provider_instance_id, path.provider_manifest_id, path.provider_manifest_revision,
          path.provider_runtime_unit_id, path.provider_runtime_epoch},
      std::move(capability_provider),
      std::move(resources),
      PreparedParticipantFocusedPortDTO{path.provider_runtime_unit_id},
      {},
      {},
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      binding.runtime_unit_epochs,
      path.stop_evidence_policy_id,
      path.stop_evidence_policy_revision,
      *authority.ordinary_stop_plan,
      std::move(payload)};

  auto made = MakePreparedOperationParticipant(
      std::move(candidate), authority.binding, authority.stop_readiness, &authority.runtime_policy,
      authority.retained_default_stop_limits, authority.stop_node_category);
  if (!made.ok()) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(made.error());
  }
  if (made.value().ordinary_stop_plan != *authority.ordinary_stop_plan) {
    return reject("prepared_motion_ordinary_stop_plan_mismatch",
                  "Prepared participant changed the owner-issued ordinary Stop plan");
  }

  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  try {
    participant = std::make_shared<const PreparedOperationParticipantDTO>(std::move(made.value()));
  } catch (...) {
    return reject("prepared_motion_product_allocation_failed",
                  "Prepared participant allocation failed before backend Prepare");
  }

  Result<PrepareParticipantResultDTO> backend_result = [&]() {
    try {
      return Result<PrepareParticipantResultDTO>::Ok(execution_backend_.Prepare(*participant));
    } catch (const std::exception& error) {
      return MakePrepareParticipantResult(
          *participant, PrepareParticipantResultStateDTO::kRejected,
          ErrorDTO{kErrorExecutionRejected,
                   "Execution backend threw during side-effect-free Prepare",
                   {{"reason", "backend_prepare_exception"}, {"exception", error.what()}}});
    } catch (...) {
      return MakePrepareParticipantResult(
          *participant, PrepareParticipantResultStateDTO::kRejected,
          ErrorDTO{kErrorExecutionRejected,
                   "Execution backend threw during side-effect-free Prepare",
                   {{"reason", "backend_prepare_exception"},
                    {"exception", reason_codes::kUnknownNonStdException}}});
    }
  }();
  if (!backend_result.ok()) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(
        backend_result.error());
  }
  {
    std::lock_guard lock(prepared_session->mutex);
    prepared_session->backend_prepare_result = backend_result.value();
  }
  if (const auto invalid =
          prepare_participant_result_field_error(backend_result.value(), *participant)) {
    return reject("backend_prepare_result_invalid",
                  std::string("Execution backend returned an invalid Prepare result: ") +
                      std::string(*invalid));
  }
  if (backend_result.value().state != PrepareParticipantResultStateDTO::kPrepared) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(
        backend_result.value().error.value_or(ErrorDTO{kErrorExecutionRejected,
                                                       "Execution backend rejected prepared motion",
                                                       {{"reason", "backend_prepare_rejected"}}}));
  }
  return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Ok(std::move(participant));
}

void ExecutionKernel::async_execute_worker(
    std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
    RetainedSceneSnapshotLease retained_scene,
    std::optional<SceneSnapshotLease> scene_snapshot_lease,
    std::shared_ptr<PreparedMotionSession> prepared_session) {
  // Worker-local motion-execution state. Lives on this
  // stack frame so two overlapping workers (should never happen under
  // the lifecycle mutex, but defense in depth) cannot share state.
  // The RAII guard guarantees exactly one outcome is emitted per
  // terminal execution even if a new terminal path is added later and
  // forgets to call `finalize_motion_execution_outcome` explicitly.
  ActiveMotionExecution motion_state;
  motion_state.execution_id = exec_id;
  motion_state.move_id = request.move_id;
  struct WorkerCompletionGuard {
    ExecutionKernel* self;
    ActiveMotionExecution& motion_state;
    RetainedSceneSnapshotLease& retained_scene;
    std::optional<SceneSnapshotLease>& scene_snapshot_lease;
    std::shared_ptr<PreparedMotionSession>& prepared_session;
    ~WorkerCompletionGuard() {
      if (prepared_session) {
        bool notify = false;
        {
          std::lock_guard lock(prepared_session->mutex);
          if (prepared_session->state == ResolvedPreparedMotionExecutionStateDTO::kPreparing ||
              prepared_session->state == ResolvedPreparedMotionExecutionStateDTO::kPrepared ||
              prepared_session->state ==
                  ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested) {
            if (prepared_session->cancel_requested) {
              prepared_session->state =
                  ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
            } else {
              prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
              prepared_session->error =
                  ErrorDTO{kErrorInternalError,
                           "Prepared motion worker exited before dispatch",
                           {{"reason", "prepared_motion_worker_exited_before_dispatch"}}};
            }
            notify = true;
          }
        }
        if (notify) {
          prepared_session->changed.notify_all();
        }
      }
      // Backend natural completion leaves an explicit terminal cursor valid
      // until the kernel has disarmed lookahead and completed terminal
      // processing. Retire the exact generation at this final worker-owned
      // handoff; stop/reset/failure paths make this idempotent.
      if (motion_state.execution_progress_generation != 0) {
        self->execution_backend_.retire_execution_progress_generation(
            motion_state.execution_progress_generation);
      }
      // Completion is the quiescence boundary observed by reset, tests, and
      // callers. Release every worker-owned scene authority handle before
      // publishing it; otherwise a waiter can race retained-lease cleanup.
      scene_snapshot_lease.reset();
      retained_scene = RetainedSceneSnapshotLease{};
      self->finish_execute_worker();
    }
  };
  struct MotionOutcomeGuard {
    ExecutionKernel* self;
    ActiveMotionExecution& state;
    ~MotionOutcomeGuard() {
      if (state.outcome_emitted) {
        return;
      }
      // Defensive: an unexpected return path missed the explicit
      // finalize call. Emit `failed` with a diagnostic reason so we
      // never silently drop the outcome record.
      self->finalize_motion_execution_outcome(
          state, MotionExecutionTerminalCause::kFailed,
          ErrorDTO{kErrorInternalError,
                   "Worker exited without explicit motion outcome",
                   {{"reason", "worker_exit_without_outcome"}}},
          std::nullopt, "worker_exit_without_outcome");
    }
  };
  // Reverse destruction order is intentional: the outcome is made durable
  // first, then scene authority is released, then completion is signalled.
  WorkerCompletionGuard completion_guard{this, motion_state, retained_scene, scene_snapshot_lease,
                                         prepared_session};
  MotionOutcomeGuard outcome_guard{this, motion_state};
  const auto finalize_overridden_terminal_authority =
      [&](ExecutionState committed, const char* reason,
          std::optional<ErrorDTO> error = std::nullopt) -> bool {
    return this->finalize_overridden_terminal_authority(request, exec_id, motion_state, committed,
                                                        reason, std::move(error));
  };

  try {
    const auto projection_binding = retained_scene.binding();
    if (!projection_binding.valid()) {
      throw std::runtime_error("execute worker received an invalid retained projection binding");
    }
    // Capture initial state for correct prev_state in events (B-2)
    const auto worker_entry_state = sm_.status().state;

    auto plan_phase = execute_worker_plan_phase(
        st, request, retained_scene.inventory(), exec_id, std::move(cached_trajectory),
        cache_revalidation_candidate, worker_entry_state, motion_state);
    if (plan_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    TrajectoryDTO trajectory = std::move(plan_phase.trajectory);
    bool active_trajectory_from_cache = plan_phase.active_trajectory_from_cache;
    cache_revalidation_candidate = plan_phase.cache_revalidation_candidate;

    auto validation_phase = execute_worker_validate_replan_phase(
        st, request, exec_id, trajectory, plan_phase.plan_request, plan_phase.planning_result,
        active_trajectory_from_cache, cache_revalidation_candidate,
        retained_scene.mutable_inventory(), scene_snapshot_lease, motion_state);
    if (validation_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    const CollisionWorldRevision validation_world_revision =
        validation_phase.collision_world_revision;

    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_participant;
    if (prepared_session) {
      auto prepared = make_and_prepare_motion_participant(
          request, trajectory, plan_phase, validation_phase, prepared_session);
      bool cancelled = false;
      if (prepared.ok()) {
        std::lock_guard lock(prepared_session->mutex);
        cancelled = prepared_session->cancel_requested;
        if (!cancelled) {
          prepared_session->participant = prepared.value();
          prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kPrepared;
          prepared_participant = std::move(prepared.value());
        }
      } else {
        std::lock_guard lock(prepared_session->mutex);
        cancelled = prepared_session->cancel_requested;
        if (!cancelled) {
          prepared_session->error = prepared.error();
          prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
        }
      }
      prepared_session->changed.notify_all();

      if (!prepared.ok() || cancelled) {
        const auto previous = sm_.status().state;
        if (cancelled) {
          const auto committed = commit_terminal(ExecutionState::kStopped, "terminal_commit");
          auto stopped_snapshot = get_execution_snapshot();
          stopped_snapshot.timestamp_ns = clock_.now();
          stopped_snapshot.execution_id = exec_id;
          stopped_snapshot.move_id = request.move_id;
          stopped_snapshot.state = committed.committed;
          stopped_snapshot.error.reset();
          publish_execution_snapshot(std::move(stopped_snapshot));
          emit_event({KernelEventType::kStateTransition,
                      exec_id,
                      clock_.now(),
                      previous,
                      committed.committed,
                      std::nullopt,
                      {{"reason", "prepared_motion_cancelled_pre_dispatch"}}});
          finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                            std::nullopt, std::nullopt,
                                            "prepared_motion_cancelled_pre_dispatch");
        } else {
          const auto error = prepared.error();
          sm_.set_last_error(error_code_from_dto(error));
          const auto committed = commit_terminal(ExecutionState::kFailed, "terminal_commit");
          emit_event(make_error_raised_event(exec_id, clock_.now(), error));
          auto failed_snapshot = get_execution_snapshot();
          failed_snapshot.timestamp_ns = clock_.now();
          failed_snapshot.execution_id = exec_id;
          failed_snapshot.move_id = request.move_id;
          failed_snapshot.state = committed.committed;
          failed_snapshot.error = error;
          publish_execution_snapshot(std::move(failed_snapshot));
          emit_event({KernelEventType::kStateTransition,
                      exec_id,
                      clock_.now(),
                      previous,
                      committed.committed,
                      std::nullopt,
                      {{"reason", "prepared_motion_prepare_failed"}}});
          finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                            error, std::nullopt, "prepared_motion_prepare_failed");
        }
        return;
      }

      emit_event(
          {KernelEventType::kParticipantPrepared,
           exec_id,
           clock_.now(),
           {},
           {},
           std::nullopt,
           {{"prepared_contract_digest", prepared_participant->prepared_contract_digest.value()}}});

      {
        std::unique_lock lock(prepared_session->mutex);
        while (!prepared_session->dispatch_requested && !prepared_session->cancel_requested &&
               !st.stop_requested() && !stop_policy_.stop_requested()) {
          prepared_session->changed.wait_for(lock, std::chrono::milliseconds(10));
        }
        cancelled = prepared_session->cancel_requested || st.stop_requested() ||
                    stop_policy_.stop_requested();
        if (cancelled) {
          prepared_session->cancel_requested = true;
          prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
        }
      }
      if (cancelled) {
        const auto previous = sm_.status().state;
        const auto committed = commit_terminal(ExecutionState::kStopped, "terminal_commit");
        auto stopped_snapshot = get_execution_snapshot();
        stopped_snapshot.timestamp_ns = clock_.now();
        stopped_snapshot.execution_id = exec_id;
        stopped_snapshot.move_id = request.move_id;
        stopped_snapshot.state = committed.committed;
        stopped_snapshot.error.reset();
        publish_execution_snapshot(std::move(stopped_snapshot));
        emit_event({KernelEventType::kStateTransition,
                    exec_id,
                    clock_.now(),
                    previous,
                    committed.committed,
                    std::nullopt,
                    {{"reason", "prepared_motion_cancelled_before_dispatch"}}});
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                          std::nullopt, std::nullopt,
                                          "prepared_motion_cancelled_before_dispatch");
        return;
      }
    }

    auto dispatch_phase = execute_worker_dispatch_phase(
        request, exec_id, trajectory, active_trajectory_from_cache, cache_revalidation_candidate,
        projection_binding, retained_scene.inventory(), validation_world_revision,
        validation_phase.scene_snapshot_lease, std::move(validation_phase.collision_trajectory),
        prepared_participant, motion_state);
    if (dispatch_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    if (prepared_session) {
      std::lock_guard lock(prepared_session->mutex);
      prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kDispatched;
    }
    auto execution_handle = std::move(dispatch_phase.execution_handle);
    motion_state.execution_progress_generation = execution_handle.execution_generation;
    const auto exec_start_ns = dispatch_phase.exec_start_ns;
    const int64_t exec_timeout_ns = dispatch_phase.exec_timeout_ns;
    core::ExecutionSnapshot latest_backend_snapshot =
        std::move(dispatch_phase.latest_backend_snapshot);

    bool pause_in_progress = false;
    int64_t last_runtime_scale_sample_ns = exec_start_ns;
    double runtime_scaled_execution_ns = 0.0;
    const auto finalize_worker_stop_token_requested = [&]() {
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kWorkerRetirement,
                                motion_state.runtime_collision_protection_generation);
      const auto state_before_stop = sm_.status().state;
      auto backend_stop = execution_backend_.stop();
      const bool already_terminal = state_before_stop == ExecutionState::kStopped ||
                                    state_before_stop == ExecutionState::kIdle ||
                                    state_before_stop == ExecutionState::kCompleted ||
                                    state_before_stop == ExecutionState::kFailed ||
                                    state_before_stop == ExecutionState::kInterrupted ||
                                    state_before_stop == ExecutionState::kInterventionRequired;
      const bool stop_confirmed = backend_stop.accepted || already_terminal;
      if ((state_before_stop == ExecutionState::kExecuting ||
           state_before_stop == ExecutionState::kPaused) &&
          stop_confirmed) {
        (void)sm_.try_transition(ExecutionState::kStopping);
      }
      const ExecutionState target_state =
          stop_confirmed ? ExecutionState::kStopped : ExecutionState::kInterventionRequired;
      auto terminal_commit = commit_terminal(target_state, "worker_stop_token_terminal");
      auto terminal_state = terminal_commit.committed;
      if (!terminal_commit.ok()) {
        terminal_state = sm_.status().state;
      }
      auto terminal_snapshot = latest_backend_snapshot;
      terminal_snapshot.timestamp_ns = clock_.now();
      terminal_snapshot.execution_id = exec_id;
      terminal_snapshot.move_id = request.move_id;
      terminal_snapshot.state = terminal_state;
      std::optional<ErrorDTO> stop_error;
      if (!stop_confirmed) {
        stop_error = backend_stop.error.value_or(
            ErrorDTO{kErrorStopFailed,
                     "Worker stop token requested teardown but backend stop was not "
                     "accepted",
                     {{"reason", "worker_stop_token_backend_stop_failed"}}});
        terminal_snapshot.error = stop_error;
      }
      publish_execution_snapshot(std::move(terminal_snapshot));
      finalize_motion_execution_outcome(
          motion_state,
          stop_confirmed ? MotionExecutionTerminalCause::kStopped
                         : MotionExecutionTerminalCause::kFailed,
          stop_error, std::nullopt,
          stop_confirmed ? "worker_stop_token_requested" : "worker_stop_token_backend_stop_failed");
    };
    const auto fault_preempt_pending_budget_ns = [&]() -> int64_t {
      std::optional<MotionInterruptStopProfileDTO> profile;
      if (auto active = interrupt_manager_.active_policy();
          active.has_value() && active->stop_profile) {
        profile = *active->stop_profile;
      } else if (config_.default_fault_stop_profile) {
        profile = *config_.default_fault_stop_profile;
      }
      const auto base_ns = profile.has_value()
                               ? effective_controlled_stop_deadline_ns(
                                     *profile, config_.complete_move_on_signal_timeout_ns)
                               : 0;
      return std::max<int64_t>(base_ns, kExecutionPollNs) + kFaultPreemptPendingSlackNs;
    };
    std::optional<std::string> pending_fault_preempt_latch_id;
    int64_t pending_fault_preempt_deadline_ns = 0;

    while (true) {
      if (st.stop_requested()) {
        finalize_worker_stop_token_requested();
        return;
      }
      bool completed = true;
      if (execution_handle.wait_for_completion) {
        completed = execution_handle.wait_for_completion(kExecutionPollNs);
      }

      if (execution_handle.get_snapshot) {
        latest_backend_snapshot = execution_handle.get_snapshot();
        latest_backend_snapshot.execution_id = exec_id;
        latest_backend_snapshot.move_id = request.move_id;
        publish_execution_snapshot(latest_backend_snapshot);
        sm_.set_progress(latest_backend_snapshot.progress);
      }

      const auto now_ns = clock_.now();
      for (const auto lane : {InterruptSourceClass::kFastExecutionRelevant,
                              InterruptSourceClass::kLowRateSupervisory}) {
        auto stale_sources =
            interrupt_manager_.sweep_stale_sources(lane, current_interrupt_context(), now_ns);
        if (!stale_sources.events.empty() || stale_sources.latched) {
          stamp_interrupt_latch_collision_world_revision(stale_sources);
          apply_interrupt_ingest_effects(stale_sources,
                                         /*apply_preemption_inline=*/true);
        }
      }
      const bool paused = latest_backend_snapshot.state == ExecutionState::kPaused ||
                          sm_.status().state == ExecutionState::kPaused;
      if (paused) {
        if (!pause_in_progress) {
          pause_in_progress = true;
        }
      } else if (pause_in_progress) {
        pause_in_progress = false;
      }

      if (!paused) {
        const int64_t dt_ns = std::max<int64_t>(0, now_ns - last_runtime_scale_sample_ns);
        const double scale =
            clamp_unit_interval(runtime_velocity_scale_.load(std::memory_order_acquire));
        runtime_scaled_execution_ns += static_cast<double>(dt_ns) * scale;
      }
      last_runtime_scale_sample_ns = now_ns;

      // Check execution timeout (W-1)
      if (exec_timeout_ns > 0 &&
          runtime_scaled_execution_ns >= static_cast<double>(exec_timeout_ns)) {
        if (operator_stop_worker_.active()) {
          const auto wait_budget =
              config_.operator_stop_profile
                  ? effective_controlled_stop_deadline_duration(
                        *config_.operator_stop_profile, config_.complete_move_on_signal_timeout_ns)
                  : std::chrono::steady_clock::duration::zero();
          const auto bounded_wait = wait_budget > std::chrono::steady_clock::duration::zero()
                                        ? wait_budget + std::chrono::milliseconds(250)
                                        : std::chrono::milliseconds(250);
          (void)wait_for_operator_stop_worker(bounded_wait);
          const auto state_after_stop = sm_.status().state;
          if (state_after_stop == ExecutionState::kStopped ||
              state_after_stop == ExecutionState::kIdle) {
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                              std::nullopt, std::nullopt,
                                              "execution_timeout_deferred_to_operator_stop");
            return;
          }
          if (state_after_stop == ExecutionState::kInterventionRequired) {
            auto intervention_snapshot = get_execution_snapshot();
            intervention_snapshot.timestamp_ns = clock_.now();
            intervention_snapshot.execution_id = exec_id;
            intervention_snapshot.move_id = request.move_id;
            intervention_snapshot.state = ExecutionState::kInterventionRequired;
            if (!intervention_snapshot.error.has_value()) {
              intervention_snapshot.error =
                  ErrorDTO{kErrorStopFailed,
                           "Execution timeout deferred to operator stop, "
                           "which entered InterventionRequired without "
                           "captured error",
                           {{"reason", "execution_timeout_operator_stop_intervention"}}};
            }
            const auto intervention_error = *intervention_snapshot.error;
            publish_execution_snapshot(std::move(intervention_snapshot));
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                              intervention_error, std::nullopt,
                                              "execution_timeout_operator_stop_intervention");
            return;
          }
        }
        if (interrupt_manager_.active_latch().has_value() ||
            sm_.status().state == ExecutionState::kInterrupted) {
          if (sm_.status().state != ExecutionState::kInterrupted) {
            (void)commit_terminal(ExecutionState::kInterrupted, "terminal_commit");
          }
          auto interrupted_snapshot = get_execution_snapshot();
          interrupted_snapshot.timestamp_ns = clock_.now();
          interrupted_snapshot.execution_id = exec_id;
          interrupted_snapshot.move_id = request.move_id;
          interrupted_snapshot.state = ExecutionState::kInterrupted;
          publish_execution_snapshot(std::move(interrupted_snapshot));
          finalize_motion_execution_outcome(
              motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt,
              std::nullopt, "execution_timeout_deferred_to_interrupt");
          return;
        }
        stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                  motion_state.runtime_collision_protection_generation);
        (void)execution_backend_.stop();
        const bool stop_completed = drain_execution_handle_bounded(execution_handle);
        auto prev = sm_.status().state;
        sm_.set_last_error(stop_completed ? ErrorCode::kExecutionTimeout : ErrorCode::kStopFailed);
        auto terminal_state =
            stop_completed ? ExecutionState::kFailed : ExecutionState::kInterventionRequired;
        auto timeout_transition = commit_terminal(terminal_state, "execution_timeout_terminal");
        terminal_state = timeout_transition.committed;
        if (timeout_transition.committed == ExecutionState::kStopped ||
            timeout_transition.committed == ExecutionState::kInterrupted) {
          auto terminal_error =
              stop_completed ? ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}}
                             : operator_intervention_error(ErrorCode::kStopFailed,
                                                           "Execution timeout exceeded "
                                                           "and software stop completion "
                                                           "was not observed",
                                                           "execution_timeout_stop_drain", prev,
                                                           exec_id, request.move_id);
          if (finalize_overridden_terminal_authority(
                  timeout_transition.committed,
                  "execution_timeout_overridden_by_terminal_authority", terminal_error)) {
            return;
          }
        }
        if (!timeout_transition.ok()) {
          const auto current = sm_.status().state;
          if (current == ExecutionState::kStopped || current == ExecutionState::kIdle) {
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                              std::nullopt, std::nullopt,
                                              "execution_timeout_lost_to_stop");
            return;
          }
          if (current == ExecutionState::kInterrupted ||
              interrupt_manager_.active_latch().has_value()) {
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt,
                std::nullopt, "execution_timeout_lost_to_interrupt");
            return;
          }
          terminal_state = ExecutionState::kInterventionRequired;
          auto intervention_transition =
              commit_terminal(terminal_state, "execution_timeout_intervention_terminal");
          terminal_state = intervention_transition.committed;
          if (!intervention_transition.ok()) {
            terminal_state = sm_.status().state;
          }
        }

        auto to_snapshot = latest_backend_snapshot;
        to_snapshot.timestamp_ns = clock_.now();
        to_snapshot.execution_id = exec_id;
        to_snapshot.move_id = request.move_id;
        to_snapshot.state = terminal_state;
        ErrorDTO terminal_error =
            stop_completed ? ErrorDTO{kErrorExecutionTimeout, "Execution timeout exceeded", {}}
                           : operator_intervention_error(
                                 ErrorCode::kStopFailed,
                                 "Execution timeout exceeded and software stop "
                                 "completion was not observed",
                                 "execution_timeout_stop_drain", prev, exec_id, request.move_id);
        to_snapshot.error = terminal_error;
        publish_execution_snapshot(std::move(to_snapshot));

        emit_event(
            {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, terminal_error, {}});
        emit_event({KernelEventType::kStateTransition,
                    exec_id,
                    clock_.now(),
                    prev,
                    terminal_state,
                    std::nullopt,
                    {}});

        {
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.executions_failed++;
        }
        finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                          terminal_error, std::nullopt, "execution_timeout");
        return;
      }

      const auto loop_state = sm_.status().state;
      if (loop_state == ExecutionState::kStopped || loop_state == ExecutionState::kIdle ||
          loop_state == ExecutionState::kFailed ||
          loop_state == ExecutionState::kInterventionRequired) {
        break;
      }
      if (loop_state == ExecutionState::kInterrupted) {
        auto latch = interrupt_manager_.active_latch();
        if (latch &&
            latch->preemption_action_requested == InterruptPreemptionAction::kPreemptTrajectory) {
          if (latch->preemption_result == InterruptPreemptionResult::kPending) {
            const auto now = clock_.now();
            if (!pending_fault_preempt_latch_id.has_value() ||
                *pending_fault_preempt_latch_id != latch->latch_id) {
              pending_fault_preempt_latch_id = latch->latch_id;
              pending_fault_preempt_deadline_ns = now + fault_preempt_pending_budget_ns();
            }
            if (pending_fault_preempt_deadline_ns > 0 && now >= pending_fault_preempt_deadline_ns) {
              request_fault_preempt_controlled_stop();
              auto hard_stop = execution_backend_.stop();
              std::map<std::string, std::string> details{
                  {"reason", "fault_preempt_pending_timeout"},
                  {"phase", "fault_preempt_pending_watchdog"},
                  {"latch_id", latch->latch_id},
                  {"source_id", latch->source_id},
                  {"policy_id", latch->policy_id},
                  {"emergency_fallback",
                   hard_stop.accepted ? "legacy_stop_succeeded" : "legacy_stop_failed"},
                  {"hard_stop_transitioned", "true"}};
              add_hard_stop_floor_details(details, execution_backend_);
              mark_interrupt_preemption_result(
                  *latch, InterruptPreemptionResult::kBackendPreemptFailed, details);
              ErrorDTO terminal_error{std::string(kErrorMotionInterruptBackendStopFailed),
                                      "fault preempt remained pending past its effective stop "
                                      "deadline; emergency hard-stop floor issued and operator "
                                      "intervention is required",
                                      details};
              sm_.set_last_error(error_code_from_dto(terminal_error));
              const auto prev_state = sm_.status().state;
              auto terminal_commit = commit_terminal(ExecutionState::kInterventionRequired,
                                                     "fault_preempt_pending_timeout");
              auto terminal_state = terminal_commit.committed;
              if (!terminal_commit.ok()) {
                terminal_state = sm_.status().state;
              }
              auto terminal_snapshot = latest_backend_snapshot;
              terminal_snapshot.timestamp_ns = clock_.now();
              terminal_snapshot.execution_id = exec_id;
              terminal_snapshot.move_id = request.move_id;
              terminal_snapshot.state = terminal_state;
              terminal_snapshot.error = terminal_error;
              publish_execution_snapshot(std::move(terminal_snapshot));
              emit_event({KernelEventType::kErrorRaised,
                          exec_id,
                          clock_.now(),
                          {},
                          {},
                          terminal_error,
                          details});
              if (terminal_state != prev_state) {
                emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(), prev_state,
                            terminal_state, std::nullopt, details});
              }
              finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                                terminal_error, std::nullopt,
                                                "fault_preempt_pending_timeout");
              stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                        motion_state.runtime_collision_protection_generation);
              return;
            }
            continue;
          }
          if (latch->preemption_result == InterruptPreemptionResult::kBackendPreemptFailed) {
            ErrorDTO terminal_error = fault_preempt_intervention_error_for_active_latch().value_or(
                ErrorDTO{std::string(kErrorMotionInterruptBackendStopFailed),
                         "fault preempt failed; operator intervention is "
                         "required",
                         {{"reason", "fault_preempt_backend_failed"},
                          {"latch_id", latch->latch_id},
                          {"source_id", latch->source_id},
                          {"policy_id", latch->policy_id}}});
            sm_.set_last_error(error_code_from_dto(terminal_error));
            const auto prev_state = sm_.status().state;
            auto terminal_commit = commit_terminal(ExecutionState::kInterventionRequired,
                                                   terminal_error.details.at("reason").c_str(),
                                                   TerminalCommitter::Origin::kStopAuthority);
            auto terminal_state = terminal_commit.committed;
            if (!terminal_commit.ok()) {
              terminal_state = sm_.status().state;
            }
            auto terminal_snapshot = latest_backend_snapshot;
            terminal_snapshot.timestamp_ns = clock_.now();
            terminal_snapshot.execution_id = exec_id;
            terminal_snapshot.move_id = request.move_id;
            terminal_snapshot.state = terminal_state;
            terminal_snapshot.error = terminal_error;
            publish_execution_snapshot(std::move(terminal_snapshot));
            emit_event({KernelEventType::kErrorRaised,
                        exec_id,
                        clock_.now(),
                        {},
                        {},
                        terminal_error,
                        terminal_error.details});
            if (terminal_state != prev_state) {
              emit_event({KernelEventType::kStateTransition, exec_id, clock_.now(), prev_state,
                          terminal_state, std::nullopt, terminal_error.details});
            }
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                              terminal_error, std::nullopt,
                                              terminal_error.details.at("reason").c_str());
            stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                      motion_state.runtime_collision_protection_generation);
            return;
          }
          // Fault session policy preempted the move. The scope (if
          // any) is reaped as part of finalize; terminal_cause is
          // `interrupted_fault` (spec/260 §B.4). The fault's own
          // latch surface is the authoritative record; this outcome
          // exists for symmetric BT consumption.
          stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced,
                                    motion_state.runtime_collision_protection_generation);
          finalize_motion_execution_outcome(motion_state,
                                            MotionExecutionTerminalCause::kInterruptedFault,
                                            std::nullopt, std::nullopt, "interrupted_fault");
          return;
        }
        if (latch && latch->preemption_action_requested ==
                         InterruptPreemptionAction::kCompleteMoveOnSignal) {
          // Move-scoped trip handling. The helper
          // resolves the matching scope/stop_profile, transitions
          // Interrupted -> Stopping, invokes the controlled-stop
          // backend port, captures the validated stop snapshot,
          // enforces the conjunctive fallback rule, and transitions
          // Stopping -> Completed (signal_trip) or Stopping -> Failed.
          handle_motion_trip_latch(request, *latch, exec_id, latest_backend_snapshot, motion_state,
                                   st);
          return;
        }
      }

      if (latest_backend_snapshot.state == ExecutionState::kPaused ||
          loop_state == ExecutionState::kPaused) {
        if (worker_signals_.consume_resume_request()) {
          bool prepared_resume = false;
          ExecuteRequestDTO resume_request = request;
          if (prepared_session) {
            std::lock_guard session_lock(prepared_session->mutex);
            prepared_resume = prepared_session->resume_cycle;
            if (prepared_resume) {
              resume_request = prepared_session->authority.execution_request;
            }
          }
          auto finish_resume_interrupted = [&](ExecutionState terminal_state, ErrorDTO error,
                                               const char* reap_reason) {
            sm_.set_last_error(error_code_from_dto(error));
            if (terminal_state != sm_.status().state) {
              (void)commit_terminal(terminal_state, "trip_reap_terminal");
            }
            auto terminal_snapshot = latest_backend_snapshot;
            terminal_snapshot.timestamp_ns = clock_.now();
            terminal_snapshot.execution_id = terminal_state == ExecutionState::kIdle ? 0 : exec_id;
            terminal_snapshot.move_id =
                terminal_state == ExecutionState::kIdle ? std::string{} : request.move_id;
            terminal_snapshot.state = terminal_state;
            terminal_snapshot.error = error;
            publish_execution_snapshot(std::move(terminal_snapshot));
            emit_event({KernelEventType::kStateTransition,
                        exec_id,
                        clock_.now(),
                        ExecutionState::kValidating,
                        terminal_state,
                        std::nullopt,
                        {{"reason", reap_reason}}});
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kStopped,
                                              std::nullopt, std::nullopt, reap_reason);
          };

          auto fail_resume_closed = [&](ErrorDTO error, const char* failure_reason,
                                        const char* outcome_reap_reason, bool count_plan_failure,
                                        bool count_validation_failure) {
            if (count_validation_failure) {
              emit_contact_policy_violation_event(exec_id, error.details);
              emit_event({KernelEventType::kValidationFailed,
                          exec_id,
                          clock_.now(),
                          {},
                          {},
                          std::nullopt,
                          {{"reason", failure_reason},
                           {"resume_phase", "validation"},
                           {"resume_replan", "true"}}});
            }
            emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, error, {}});
            {
              std::lock_guard<std::mutex> lock(metrics_mutex_);
              if (count_plan_failure) {
                metrics_.plans_failed++;
              }
              if (count_validation_failure) {
                metrics_.validations_failed++;
              }
            }
            sm_.set_last_error(error_code_from_dto(error));
            if (prepared_resume) {
              if (prepared_session) {
                {
                  std::lock_guard session_lock(prepared_session->mutex);
                  prepared_session->error = error;
                  prepared_session->state =
                      ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
                  prepared_session->resume_cycle = false;
                }
                prepared_session->changed.notify_all();
              }
              auto prev_state = sm_.status().state;
              auto terminal_commit =
                  commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
              auto failed_snapshot = latest_backend_snapshot;
              failed_snapshot.timestamp_ns = clock_.now();
              failed_snapshot.execution_id = exec_id;
              failed_snapshot.move_id = request.move_id;
              failed_snapshot.state = terminal_commit.committed;
              failed_snapshot.error = error;
              publish_execution_snapshot(std::move(failed_snapshot));
              emit_event({KernelEventType::kStateTransition,
                          exec_id,
                          clock_.now(),
                          prev_state,
                          terminal_commit.committed,
                          error,
                          {{"reason", outcome_reap_reason}}});
              finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                                error, std::nullopt, outcome_reap_reason);
              return;
            }
            (void)execution_backend_.stop();
            const bool stop_completed = drain_execution_handle_bounded(execution_handle);
            auto prev_state = sm_.status().state;
            auto terminal_state =
                stop_completed ? ExecutionState::kFailed : ExecutionState::kInterventionRequired;
            if (!stop_completed) {
              sm_.set_last_error(ErrorCode::kStopFailed);
            }
            auto terminal_commit = commit_terminal(terminal_state, "trip_stop_terminal");
            if (terminal_commit.committed != terminal_state) {
              if (finalize_overridden_terminal_authority(
                      terminal_commit.committed,
                      "resume_failure_stop_overridden_by_terminal_authority", error)) {
                return;
              }
            }
            terminal_state = terminal_commit.committed;
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = terminal_state;
            failed_snapshot.error =
                stop_completed ? error
                               : operator_intervention_error(ErrorCode::kStopFailed,
                                                             "Resume failed and software stop "
                                                             "completion was not observed",
                                                             "resume_failure_stop_drain",
                                                             prev_state, exec_id, request.move_id);
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition,
                        exec_id,
                        clock_.now(),
                        prev_state,
                        terminal_state,
                        std::nullopt,
                        {}});
            finalize_motion_execution_outcome(
                motion_state, MotionExecutionTerminalCause::kFailed,
                stop_completed ? error
                               : ErrorDTO{kErrorStopFailed,
                                          "Resume failed and stop drain did not complete",
                                          {{"reason", "resume_failure_stop_drain"}}},
                std::nullopt, outcome_reap_reason);
          };

          emit_event({KernelEventType::kPlanStarted,
                      exec_id,
                      clock_.now(),
                      {},
                      {},
                      std::nullopt,
                      {{"reason", "resume_replan"}, {"resume_phase", "planning"}}});
          auto resume_scene_result = retain_current_scene_snapshot_lease_inventory(
              exec_id, request.move_id + "_resume_" + std::to_string(clock_.now()));
          if (!resume_scene_result.ok()) {
            fail_resume_closed(resume_scene_result.error(), "resume scene retention failed",
                               "resume_scene_retention_failed", true, false);
            return;
          }
          auto resume_scene = std::move(resume_scene_result.value());
          auto resume_scene_snapshot_lease =
              build_scene_snapshot_lease(resume_scene.inventory(), resume_request, exec_id);
          auto resume_plan =
              plan_resume_trajectory_with_evidence(resume_request, resume_scene.inventory());
          if (!resume_plan.ok()) {
            fail_resume_closed(resume_plan.error(), "resume planning failed",
                               "resume_planning_failed", true, false);
            return;
          }

          auto resumed_trajectory = *resume_plan.value().result.trajectory;
          emit_event({KernelEventType::kPlanCompleted,
                      exec_id,
                      clock_.now(),
                      {},
                      {},
                      std::nullopt,
                      {{"reason", "resume_replan"}, {"resume_phase", "planning"}}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.plans_succeeded++;
          }

          if (stop_policy_.stop_requested() || sm_.status().state == ExecutionState::kStopped) {
            finish_resume_interrupted(ExecutionState::kStopped,
                                      ErrorDTO{kErrorInternalError,
                                               "Resume planning interrupted by stop",
                                               {{"reason", "resume_planning_stop"}}},
                                      "resume_planning_stop");
            return;
          }
          if (sm_.status().state == ExecutionState::kIdle) {
            finish_resume_interrupted(ExecutionState::kIdle,
                                      ErrorDTO{kErrorInternalError,
                                               "Resume planning interrupted by reset",
                                               {{"reason", "resume_planning_reset"}}},
                                      "resume_planning_reset");
            return;
          }

          emit_event({KernelEventType::kValidationStarted,
                      exec_id,
                      clock_.now(),
                      {},
                      {},
                      std::nullopt,
                      {{"reason", "resume_replan"}, {"resume_phase", "validation"}}});

          Result<CollisionValidationSuccess> resume_validation =
              validate_trajectory(resumed_trajectory, resume_request,
                                  resume_scene.mutable_inventory(), resume_scene_snapshot_lease);
          if (!resume_validation.ok()) {
            if (validation_cancelled_by_interrupt(resume_validation.error()) &&
                sm_.status().state == ExecutionState::kInterrupted) {
              emit_event({KernelEventType::kValidationFailed,
                          exec_id,
                          clock_.now(),
                          {},
                          {},
                          std::nullopt,
                          {{"reason", "resume validation cancelled by interrupt"},
                           {"resume_phase", "validation"},
                           {"resume_replan", "true"}}});
              emit_event({KernelEventType::kErrorRaised,
                          exec_id,
                          clock_.now(),
                          {},
                          {},
                          resume_validation.error(),
                          {}});
              mark_active_interrupt_preemption_result(InterruptPreemptionResult::kSucceeded);
              auto interrupted_snapshot = latest_backend_snapshot;
              interrupted_snapshot.timestamp_ns = clock_.now();
              interrupted_snapshot.execution_id = exec_id;
              interrupted_snapshot.move_id = request.move_id;
              interrupted_snapshot.state = ExecutionState::kInterrupted;
              interrupted_snapshot.error = resume_validation.error();
              publish_execution_snapshot(std::move(interrupted_snapshot));
              finalize_motion_execution_outcome(
                  motion_state, MotionExecutionTerminalCause::kInterruptedFault, std::nullopt,
                  std::nullopt, "resume_validation_cancelled_by_interrupt");
              return;
            }
            fail_resume_closed(resume_validation.error(), "resume validation failed",
                               "resume_validation_failed", false, true);
            return;
          }
          auto resume_projection = build_runtime_collision_state_projection(
              resumed_trajectory, resume_validation.value().collision_trajectory);
          if (!resume_projection.ok()) {
            fail_resume_closed(resume_projection.error(),
                               "resume runtime collision projection failed",
                               "resume_runtime_collision_projection_failed", false, true);
            return;
          }
          set_runtime_collision_state_projection(std::move(resume_projection.value()));
          auto resume_collision_trajectory = std::make_shared<const TrajectoryDTO>(
              std::move(resume_validation.value().collision_trajectory));

          const auto& resume_validation_telemetry = resume_validation.value().telemetry;
          emit_event(
              {KernelEventType::kValidationPassed,
               exec_id,
               clock_.now(),
               {},
               {},
               std::nullopt,
               {{"reason", "resume_replan"},
                {"resume_phase", "validation"},
                {"collision_workers_requested",
                 std::to_string(resume_validation_telemetry.workers_requested)},
                {"collision_workers_effective",
                 std::to_string(resume_validation_telemetry.workers_effective)},
                {"collision_workers_used",
                 std::to_string(resume_validation_telemetry.workers_actually_used)},
                {"collision_parallel_fallback_reason", resume_validation_telemetry.fallback_reason},
                {"collision_ordered_contact_summary_used",
                 resume_validation_telemetry.ordered_contact_summary_used ? "true" : "false"},
                {"collision_validation_wall_ns",
                 std::to_string(resume_validation_telemetry.total_wall_ns)},
                {"dynamically_discovered_start_contact_count",
                 std::to_string(
                     resume_validation.value().dynamically_discovered_start_contact_count)}}});
          {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            metrics_.validations_passed++;
          }

          if (stop_policy_.stop_requested() || sm_.status().state == ExecutionState::kStopped) {
            finish_resume_interrupted(ExecutionState::kStopped,
                                      ErrorDTO{kErrorInternalError,
                                               "Resume validation interrupted by stop",
                                               {{"reason", "resume_validation_stop"}}},
                                      "resume_validation_stop");
            return;
          }
          if (sm_.status().state == ExecutionState::kIdle) {
            finish_resume_interrupted(ExecutionState::kIdle,
                                      ErrorDTO{kErrorInternalError,
                                               "Resume validation interrupted by reset",
                                               {{"reason", "resume_validation_reset"}}},
                                      "resume_validation_reset");
            return;
          }

          std::shared_ptr<const PreparedOperationParticipantDTO> resume_participant;
          if (prepared_resume) {
            ExecuteWorkerPlanPhaseResult resume_plan_phase;
            resume_plan_phase.trajectory = resumed_trajectory;
            resume_plan_phase.plan_request = resume_plan.value().request;
            resume_plan_phase.planning_result = resume_plan.value().result;
            ExecuteWorkerValidationPhaseResult resume_validation_phase;
            resume_validation_phase.collision_world_revision =
                resume_validation.value().collision_world_revision;
            resume_validation_phase.validation_evidence = PreparedMotionValidationEvidenceDTO{
                config_.validation_revision, resume_validation.value().noncollision_result,
                resume_validation.value().collision_world_revision,
                resume_validation.value().collision_policy_revision,
                resume_validation.value().collision_decision};
            auto prepared = make_and_prepare_motion_participant(
                resume_request, resumed_trajectory, resume_plan_phase,
                resume_validation_phase, prepared_session);
            if (!prepared.ok()) {
              fail_resume_closed(prepared.error(), "resume backend preparation failed",
                                 "resume_backend_preparation_failed", false, false);
              return;
            }
            bool cancelled = false;
            {
              std::lock_guard session_lock(prepared_session->mutex);
              cancelled = prepared_session->cancel_requested;
              if (!cancelled) {
                prepared_session->participant = prepared.value();
                prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kPrepared;
                resume_participant = std::move(prepared.value());
              }
            }
            prepared_session->changed.notify_all();
            if (cancelled) {
              finish_resume_interrupted(
                  ExecutionState::kStopped,
                  ErrorDTO{kErrorInvalidRequest,
                           "Prepared Resume was cancelled before dispatch",
                           {{"reason", "prepared_resume_cancelled_pre_dispatch"}}},
                  "prepared_resume_cancelled_pre_dispatch");
              return;
            }
            emit_event({KernelEventType::kParticipantPrepared,
                        exec_id,
                        clock_.now(),
                        {},
                        {},
                        std::nullopt,
                        {{"prepared_contract_digest",
                          resume_participant->prepared_contract_digest.value()},
                         {"reason", "resume_reprepare"}}});
            {
              std::unique_lock session_lock(prepared_session->mutex);
              while (!prepared_session->dispatch_requested && !prepared_session->cancel_requested &&
                     !st.stop_requested() && !stop_policy_.stop_requested()) {
                prepared_session->changed.wait_for(session_lock, std::chrono::milliseconds(10));
              }
              cancelled = prepared_session->cancel_requested || st.stop_requested() ||
                          stop_policy_.stop_requested();
              if (cancelled) {
                prepared_session->cancel_requested = true;
                prepared_session->state =
                    ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
              }
            }
            if (cancelled) {
              finish_resume_interrupted(
                  ExecutionState::kStopped,
                  ErrorDTO{kErrorInvalidRequest,
                           "Prepared Resume was cancelled at the dispatch barrier",
                           {{"reason", "prepared_resume_cancelled_at_dispatch_barrier"}}},
                  "prepared_resume_cancelled_at_dispatch_barrier");
              return;
            }
          }

          motion_state.runtime_collision_protection_generation = start_collision_monitoring(
              "resume_dispatch", RuntimeCollisionProtectionPurpose::kMotion,
              &resume_scene.inventory());
          execution_handle =
              prepared_resume ? execution_backend_.DispatchResume(
                                    *resume_participant, resume_request.binding.controller_goal_id,
                                    resume_request.binding.controller_goal_generation)
                              : execution_backend_.resume(resumed_trajectory);
          if (prepared_resume) {
            if (const auto invalid = prepared_execution_handle_field_error(
                    execution_handle, *resume_participant,
                    resume_request.binding.controller_goal_id,
                    resume_request.binding.controller_goal_generation)) {
              fail_resume_closed(
                  ErrorDTO{kErrorInvariantViolation,
                           "Resume backend returned a handle outside the prepared goal",
                           {{"reason", std::string(*invalid)}}},
                  "prepared Resume dispatch membership failed",
                  "prepared_resume_dispatch_membership_failed", false, false);
              return;
            }
            {
              std::lock_guard session_lock(prepared_session->mutex);
              prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kDispatched;
              prepared_session->resume_cycle = false;
            }
            prepared_session->changed.notify_all();
          }

          auto prev_state = sm_.status().state;
          auto resume_transition = sm_.try_transition(ExecutionState::kExecuting);
          if (!resume_transition.ok()) {
            sm_.set_last_error(ErrorCode::kResumeFailed);
            ErrorDTO resume_error{kErrorResumeFailed, "Resume transition to Executing failed", {}};
            emit_event(
                {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, resume_error, {}});
            auto terminal_commit = commit_terminal(ExecutionState::kFailed, "terminal_commit");
            if (terminal_commit.committed != ExecutionState::kFailed) {
              if (finalize_overridden_terminal_authority(
                      terminal_commit.committed,
                      "resume_transition_overridden_by_terminal_authority", resume_error)) {
                return;
              }
            }
            auto failed_snapshot = latest_backend_snapshot;
            failed_snapshot.timestamp_ns = clock_.now();
            failed_snapshot.execution_id = exec_id;
            failed_snapshot.move_id = request.move_id;
            failed_snapshot.state = terminal_commit.committed;
            failed_snapshot.error = resume_error;
            publish_execution_snapshot(std::move(failed_snapshot));
            emit_event({KernelEventType::kStateTransition,
                        exec_id,
                        clock_.now(),
                        prev_state,
                        terminal_commit.committed,
                        std::nullopt,
                        {}});
            finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                              resume_error, std::nullopt,
                                              "resume_transition_failed");
            return;
          }

          emit_event({KernelEventType::kStateTransition,
                      exec_id,
                      clock_.now(),
                      prev_state,
                      ExecutionState::kExecuting,
                      std::nullopt,
                      {}});
          emit_event(
              {KernelEventType::kExecuteStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});

          auto resumed_snapshot = latest_backend_snapshot;
          resumed_snapshot.timestamp_ns = clock_.now();
          resumed_snapshot.execution_id = exec_id;
          resumed_snapshot.move_id = request.move_id;
          resumed_snapshot.state = ExecutionState::kExecuting;
          resumed_snapshot.error.reset();
          publish_execution_snapshot(std::move(resumed_snapshot));

          motion_state.execution_progress_generation = execution_handle.execution_generation;
          arm_runtime_collision_lookahead(exec_id, execution_handle.execution_generation,
                                          std::move(resume_collision_trajectory),
                                          resume_request.contact_policy_id,
                                          resume_scene.inventory());
        }

        if (completed) {
          // Some real backends implement pause by cancelling the active
          // controller goal. Their execution handle is then terminal even
          // though the kernel state is intentionally kPaused. Keep the kernel
          // worker alive so a later resume() can replan, validate, and
          // fresh-dispatch the original motion intent; otherwise the state
          // machine can advance to kValidating with no worker left to consume
          // resume intent.
          //
          // Audit 6D (finding 2.19): condvar wait instead of a bare sleep —
          // resume()/stop()/reset() notify `worker_signals_`, so the worker
          // wakes immediately on intent instead of up to one poll period
          // later. The poll period remains only as the deadline fallback.
          (void)worker_signals_.wait_for_resume_or(
              std::chrono::nanoseconds(kExecutionPollNs), [this, &st]() {
                return lifecycle_stop_requested_while_busy_.load(std::memory_order_seq_cst) ||
                       lifecycle_reset_requested_while_busy_.load(std::memory_order_acquire) ||
                       st.stop_requested();
              });
        }
        continue;
      }

      if (completed) {
        break;
      }
    }

    execute_worker_finalize_phase(request, exec_id, execution_handle, motion_state);
    return;

  } catch (const std::exception& e) {
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                              motion_state.runtime_collision_protection_generation);
    sm_.set_last_error(ErrorCode::kInternalError);
    auto prev_state = sm_.status().state;
    auto failure_transition = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    const ErrorDTO exception_error{kErrorInternalError, e.what(), {{"reason", "worker_exception"}}};
    if (failure_transition.committed != ExecutionState::kFailed) {
      if (finalize_overridden_terminal_authority(
              failure_transition.committed, "worker_exception_overridden_by_terminal_authority",
              exception_error)) {
        return;
      }
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = failure_transition.committed;
    snapshot.error = exception_error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                prev_state,
                failure_transition.committed,
                std::nullopt,
                {}});
    {
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.executions_failed++;
    }
    finalize_motion_execution_outcome(motion_state, MotionExecutionTerminalCause::kFailed,
                                      exception_error, std::nullopt, "worker_exception");
  }
}

Result<RobotStateDTO> ExecutionKernel::current_preview_state(const std::int64_t now_ns) const {
  return observed_state_.current_state(now_ns);
}

Result<std::vector<Pose3DTO>> ExecutionKernel::resolve_preview_reference_poses(
    ExecuteRequestDTO request, std::vector<ReferenceDTO> references,
    const CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& start_state) {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  const auto reject = [](const ErrorCode code, std::string message, std::string reason) {
    return Result<std::vector<Pose3DTO>>::Err(ErrorDTO{
        std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
  };
  if (sm_.status().state != ExecutionState::kIdle) {
    return reject(ErrorCode::kExecutionRejected,
                  "Reference resolution requires an idle participant",
                  "motion_preview_reference_participant_not_idle");
  }
  if (references.empty() || references.size() > 2U ||
      std::any_of(references.begin(), references.end(), [](const auto& reference) {
        return !reference_identity_matches_type(reference) ||
               (reference.type == ReferenceDTO::Type::kPose) != reference.pose.has_value();
      })) {
    return reject(ErrorCode::kInvalidRequest,
                  "Reference resolution received an invalid portable reference",
                  "motion_preview_reference_invalid");
  }
  if (!projection_inventory.projection_binding.valid() || !projection_inventory.logical_scene ||
      !projection_inventory.logical_scene->locally_valid() ||
      projection_inventory.logical_scene->revision !=
          projection_inventory.projection_binding.projection_revision.scene_revision ||
      projection_inventory.projection_binding.projection_revision.retained_bundle_id !=
          request.binding.retained_bundle_id ||
      projection_inventory.projection_binding.projection_revision.graph_revision !=
          request.binding.graph_revision) {
    return reject(ErrorCode::kExecutionRejected,
                  "Reference resolution requires one exact retained scene projection",
                  "motion_preview_reference_projection_invalid");
  }

  auto selected_start = controlled_state_for_binding(request, start_state);
  if (!selected_start.ok()) {
    return Result<std::vector<Pose3DTO>>::Err(selected_start.error());
  }
  const auto capture_time_ns = clock_.now();
  auto observed_publication = observed_state_.addressed_publication(
      request.binding.control_binding_id, request.binding.control_binding_revision);
  if (!observed_publication.ok()) {
    return Result<std::vector<Pose3DTO>>::Err(observed_publication.error());
  }
  auto published_start =
      controlled_state_for_publication(request, observed_publication.value(), capture_time_ns);
  if (!published_start.ok()) {
    return Result<std::vector<Pose3DTO>>::Err(published_start.error());
  }
  if (published_start.value() != selected_start.value()) {
    return reject(ErrorCode::kStartStateMismatch,
                  "Reference state changed after the linked preview state was captured",
                  "motion_preview_reference_start_state_changed");
  }

  ProgramFrameResolutionCaptureDTO capture{request.binding,
                                           published_start.value(),
                                           observed_publication.value(),
                                           projection_inventory.projection_binding,
                                           projection_inventory.logical_scene,
                                           request.runtime_object_bindings,
                                           references,
                                           capture_time_ns};
  auto context = reference_resolver_.capture_program_frame_resolution_context(capture);
  if (!context.ok()) {
    return Result<std::vector<Pose3DTO>>::Err(context.error());
  }
  if (!context.value().valid() || context.value().binding != capture.binding ||
      context.value().start_state != capture.start_state ||
      context.value().observed_start_publication != capture.observed_start_publication ||
      context.value().projection_binding != capture.projection_binding ||
      context.value().logical_scene != capture.logical_scene ||
      context.value().runtime_object_bindings != capture.runtime_object_bindings ||
      context.value().captured_at_ns != capture.captured_at_ns) {
    return reject(ErrorCode::kExecutionRejected,
                  "Reference context differs from its retained preview admission",
                  "motion_preview_reference_context_invalid");
  }
  std::vector<Pose3DTO> resolved;
  resolved.reserve(references.size());
  for (const auto& reference : references) {
    auto pose = reference_resolver_.resolve_reference_pose(reference, context.value());
    if (!pose.ok()) {
      return Result<std::vector<Pose3DTO>>::Err(pose.error());
    }
    resolved.push_back(std::move(pose.value()));
  }
  return Result<std::vector<Pose3DTO>>::Ok(std::move(resolved));
}

Result<MotionPreviewResultDTO> ExecutionKernel::preview_motion(
    ExecuteRequestDTO request, CollisionWorldLeaseInventory projection_inventory,
    RobotStateDTO start_state, const std::uint32_t planning_attempt_index,
    const std::stop_token stop_token) {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  const auto reject = [](const ErrorCode code, std::string message, std::string reason) {
    return Result<MotionPreviewResultDTO>::Err(ErrorDTO{
        std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
  };
  if (sm_.status().state != ExecutionState::kIdle) {
    return reject(ErrorCode::kExecutionRejected, "Motion preview requires an idle participant",
                  "motion_preview_participant_not_idle");
  }
  if (auto admission_error = collision_validation_admission_error(); admission_error.has_value()) {
    return Result<MotionPreviewResultDTO>::Err(std::move(*admission_error));
  }
  if (!std::isfinite(request.timeouts.planning_timeout_s) ||
      request.timeouts.planning_timeout_s <= 0.0) {
    return reject(ErrorCode::kInvalidRequest,
                  "Motion preview planning timeout must be finite and "
                  "strictly positive",
                  "motion_preview_planning_timeout_invalid");
  }
  if (request.target_mode != MotionTargetModeDTO::kAbsolute ||
      request.target_reference.has_value() || request.offset_reference.has_value()) {
    return reject(ErrorCode::kInvalidRequest,
                  "Motion preview requires an already-resolved absolute "
                  "motion target",
                  "motion_preview_target_not_resolved");
  }
  if (!projection_inventory.projection_binding.valid() || !projection_inventory.logical_scene ||
      !projection_inventory.logical_scene->locally_valid() || !projection_inventory.bundle.world ||
      !projection_inventory.bundle.policy.has_value() ||
      projection_inventory.logical_scene->revision !=
          projection_inventory.projection_binding.projection_revision.scene_revision ||
      projection_inventory.projection_binding.projection_revision.retained_bundle_id !=
          request.binding.retained_bundle_id ||
      projection_inventory.projection_binding.projection_revision.graph_revision !=
          request.binding.graph_revision) {
    return reject(ErrorCode::kExecutionRejected,
                  "Motion preview requires one exact retained scene "
                  "projection",
                  "motion_preview_projection_invalid");
  }
  if (auto controlled = controlled_state_for_binding(request, start_state); !controlled.ok()) {
    return Result<MotionPreviewResultDTO>::Err(controlled.error());
  }
  if (stop_token.stop_requested()) {
    return reject(ErrorCode::kExecutionAborted, "Motion preview was cancelled before planning",
                  "motion_preview_cancelled");
  }

  auto grasp = install_resolved_contact_grasp_policy(request, projection_inventory);
  if (!grasp.ok()) {
    return Result<MotionPreviewResultDTO>::Err(grasp.error());
  }
  auto dynamic_contacts =
      discover_dynamic_carried_start_contacts(projection_inventory, start_state, 0U);
  if (!dynamic_contacts.ok()) {
    return Result<MotionPreviewResultDTO>::Err(dynamic_contacts.error());
  }
  request.dynamically_discovered_start_contact_count = dynamic_contacts.value();

  std::stop_callback cancellation(stop_token, [this]() { planner_.request_cancel(); });
  const auto planning_start_ns = clock_.now();
  const auto planning_timeout_ns =
      static_cast<std::int64_t>(request.timeouts.planning_timeout_s * 1.0e9);
  const auto planning_timeout_handle = scheduler_.schedule(
      planning_start_ns + planning_timeout_ns, [this]() { planner_.request_cancel(); },
      SchedulerPriority::Monitoring);
  struct ScheduleCancelGuard final {
    IScheduler& scheduler;
    ScheduleHandle handle;
    ~ScheduleCancelGuard() {
      if (handle != 0U) {
        scheduler.cancel(handle);
      }
    }
  } timeout_guard{scheduler_, planning_timeout_handle};

  try {
    auto planned =
        plan_trajectory(request, projection_inventory, planning_attempt_index, &start_state);
    const bool planning_timed_out = clock_.now() - planning_start_ns >= planning_timeout_ns;
    if (planning_timed_out) {
      return reject(ErrorCode::kPlanningTimeout, "Motion preview planning timed out",
                    "motion_preview_planning_timeout");
    }
    if (stop_token.stop_requested()) {
      return reject(ErrorCode::kExecutionAborted, "Motion preview was cancelled during planning",
                    "motion_preview_cancelled");
    }
    if (!planned.ok()) {
      return Result<MotionPreviewResultDTO>::Err(planned.error());
    }

    auto trajectory = std::move(planned.value());
    auto validated = validate_trajectory(trajectory, request, projection_inventory, std::nullopt,
                                         &start_state, stop_token);
    if (stop_token.stop_requested()) {
      return reject(ErrorCode::kExecutionAborted, "Motion preview was cancelled during validation",
                    "motion_preview_cancelled");
    }
    if (!validated.ok()) {
      return Result<MotionPreviewResultDTO>::Err(validated.error());
    }

    RobotStateDTO final_state = start_state;
    if (!trajectory.waypoints.empty()) {
      const auto& final_waypoint = trajectory.waypoints.back();
      final_state = RobotStateDTO{trajectory.joint_names, final_waypoint.positions_rad,
                                  final_waypoint.velocities_rad_s};
    }
    auto final_controlled_state = controlled_state_for_binding(request, final_state);
    if (!final_controlled_state.ok()) {
      return Result<MotionPreviewResultDTO>::Err(final_controlled_state.error());
    }
    auto final_tcp_pose = kinematics_.forward_kinematics(
        FkRequestDTO{final_state, compiled_frame_name(request.binding.tcp_frame),
                     compiled_frame_name(request.binding.base_frame)});
    if (!final_tcp_pose.ok()) {
      return Result<MotionPreviewResultDTO>::Err(final_tcp_pose.error());
    }
    return Result<MotionPreviewResultDTO>::Ok(MotionPreviewResultDTO{
        std::move(trajectory), std::move(start_state), std::move(final_state),
        std::move(final_controlled_state.value()), std::move(final_tcp_pose.value()),
        projection_inventory.projection_binding, validated.value().collision_world_revision,
        validated.value().dynamically_discovered_start_contact_count});
  } catch (const std::exception& error) {
    return Result<MotionPreviewResultDTO>::Err(
        ErrorDTO{kErrorPlanningFailed,
                 "Motion preview raised while planning or validating",
                 {{"reason", "motion_preview_exception"}, {"exception", error.what()}}});
  } catch (...) {
    return Result<MotionPreviewResultDTO>::Err(
        ErrorDTO{kErrorPlanningFailed,
                 "Motion preview raised a non-standard exception",
                 {{"reason", "motion_preview_exception"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
}

Result<uint64_t> ExecutionKernel::execute(const ExecuteRequestDTO& request) {
  return execute_impl(request, {});
}

Result<std::uint64_t> ExecutionKernel::PrepareMotion(
    ResolvedPreparedMotionExecutionRequestDTO request) {
  if (!request.ordinary_stop_plan) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared motion requires the retained ordinary Stop plan",
                 {{"reason", "ordinary_stop_plan_missing"}}});
  }
  const auto& binding = request.binding.value();
  const auto& resolved = request.execution_request.binding;
  if (request.operation_revision.value() == 0U || resolved.operation_id.empty() ||
      resolved.participant_id.empty() || resolved.controller_goal_id.empty() ||
      !resolved.controller_goal_generation.nonzero() || resolved.program_id != binding.program_id ||
      resolved.program_revision != binding.program_revision ||
      resolved.program_binding_id != binding.program_binding_id ||
      resolved.retained_bundle_id != binding.retained_bundle_id ||
      resolved.graph_revision != binding.graph_revision ||
      resolved.controllable_component_id != binding.controllable_component_id ||
      resolved.command_target_id != binding.command_target_id ||
      resolved.capability_id != binding.capability_id) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared motion authority differs from the resolved execution request",
                 {{"reason", "prepared_motion_authority_mismatch"}}});
  }
  auto verified_stop = VerifyPreparedParticipantStopPlan(
      request.stop_readiness.value().resolved_stop_base_plan, *request.ordinary_stop_plan,
      binding.resources, request.retained_default_stop_limits, request.stop_node_category);
  if (!verified_stop.ok() || request.ordinary_stop_plan->operation_id != resolved.operation_id ||
      request.ordinary_stop_plan->operation_revision != request.operation_revision ||
      request.ordinary_stop_plan->participant_id != resolved.participant_id ||
      request.ordinary_stop_plan->program_run_id != resolved.program_run_id) {
    return Result<std::uint64_t>::Err(
        verified_stop.ok()
            ? ErrorDTO{kErrorInvalidRequest,
                       "Prepared motion ordinary Stop plan crosses execution identity",
                       {{"reason", "ordinary_stop_plan_identity_mismatch"}}}
            : verified_stop.error());
  }

  std::shared_ptr<PreparedMotionSession> session;
  try {
    session = std::make_shared<PreparedMotionSession>(std::move(request));
  } catch (...) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared motion session allocation failed before admission",
                 {{"reason", "prepared_motion_session_allocation_failed"}}});
  }
  // Keep the owner stable while evaluating the request reference. Function
  // argument evaluation order must not allow moving `session` before the
  // request argument is formed.
  return execute_impl(session->authority.execution_request, session);
}

Result<ResolvedPreparedMotionExecutionSnapshotDTO> ExecutionKernel::ObservePreparedMotion(
    const std::uint64_t execution_id) const {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared motion session is not resident",
                 {{"reason", "prepared_motion_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  if (session->execution_id != execution_id) {
    return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared motion execution identity is stale",
                 {{"reason", "prepared_motion_execution_id_mismatch"}}});
  }
  return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(
      ResolvedPreparedMotionExecutionSnapshotDTO{session->execution_id, session->state,
                                                 session->participant,
                                                 session->backend_prepare_result, session->error});
}

Result<void> ExecutionKernel::DispatchPreparedMotion(
    const std::uint64_t execution_id, const ContentDigest& prepared_contract_digest) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->execution_id != execution_id) {
      return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                        "Prepared motion execution identity is stale",
                                        {{"reason", "prepared_motion_execution_id_mismatch"}}});
    }
    if (session->state != ResolvedPreparedMotionExecutionStateDTO::kPrepared ||
        !session->participant ||
        session->participant->prepared_contract_digest != prepared_contract_digest) {
      return Result<void>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Prepared motion dispatch does not name the exact ready contract",
                   {{"reason", "prepared_motion_dispatch_mismatch"}}});
    }
    session->dispatch_requested = true;
    session->state = ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested;
  }
  session->changed.notify_all();
  return Result<void>::Ok();
}

Result<void> ExecutionKernel::PrepareResumeMotion(const std::uint64_t execution_id,
                                                  ResolvedPreparedMotionResumeRequestDTO request) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  if (!request.operation_revision.nonzero() || request.operation_id.empty() ||
      request.participant_id.empty() || request.controller_goal_id.empty() ||
      !request.controller_goal_generation.nonzero() || !request.ordinary_stop_plan) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "Prepared Resume authority is incomplete",
                                      {{"reason", "prepared_resume_authority_invalid"}}});
  }

  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  std::lock_guard session_lock(session->mutex);
  if (session->execution_id != execution_id) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared Resume execution identity is stale",
                                      {{"reason", "prepared_resume_execution_id_mismatch"}}});
  }
  const auto& prior = session->authority;
  const auto& prior_binding = prior.execution_request.binding;
  if (session->state != ResolvedPreparedMotionExecutionStateDTO::kDispatched ||
      session->resume_cycle || request.operation_id != prior_binding.operation_id ||
      request.participant_id != prior_binding.participant_id ||
      request.operation_revision.value() <= prior.operation_revision.value() ||
      request.controller_goal_generation.value() <=
          prior_binding.controller_goal_generation.value() ||
      request.controller_goal_id == prior_binding.controller_goal_id ||
      request.ordinary_stop_plan->operation_id != request.operation_id ||
      request.ordinary_stop_plan->operation_revision != request.operation_revision ||
      request.ordinary_stop_plan->participant_id != request.participant_id ||
      request.ordinary_stop_plan->program_run_id != prior_binding.program_run_id) {
    return Result<void>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared Resume does not advance the resident participant authority",
                 {{"reason", "prepared_resume_authority_mismatch"}}});
  }
  const auto verified_stop = VerifyPreparedParticipantStopPlan(
      prior.stop_readiness.value().resolved_stop_base_plan, *request.ordinary_stop_plan,
      prior.binding.value().resources, prior.retained_default_stop_limits,
      prior.stop_node_category);
  if (!verified_stop.ok()) {
    return Result<void>::Err(verified_stop.error());
  }

  ResolvedPreparedMotionExecutionRequestDTO updated = prior;
  updated.operation_revision = request.operation_revision;
  updated.execution_request.binding.controller_goal_id = std::move(request.controller_goal_id);
  updated.execution_request.binding.controller_goal_generation = request.controller_goal_generation;
  updated.ordinary_stop_plan = std::move(request.ordinary_stop_plan);
  auto transition = stop_policy_.request_resume();
  if (!transition.ok()) {
    return Result<void>::Err(transition.error());
  }
  session->authority = std::move(updated);
  session->participant.reset();
  session->backend_prepare_result.reset();
  session->error.reset();
  session->dispatch_requested = false;
  session->cancel_requested = false;
  session->resume_cycle = true;
  session->state = ResolvedPreparedMotionExecutionStateDTO::kPreparing;
  worker_signals_.request_resume();
  return Result<void>::Ok();
}

Result<void> ExecutionKernel::DispatchPreparedResumeMotion(
    const std::uint64_t execution_id, const ContentDigest& prepared_contract_digest) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->execution_id != execution_id) {
      return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                        "Prepared Resume execution identity is stale",
                                        {{"reason", "prepared_resume_execution_id_mismatch"}}});
    }
    if (!session->resume_cycle ||
        session->state != ResolvedPreparedMotionExecutionStateDTO::kPrepared ||
        !session->participant ||
        session->participant->prepared_contract_digest != prepared_contract_digest) {
      return Result<void>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Prepared Resume dispatch does not name the exact ready contract",
                   {{"reason", "prepared_resume_dispatch_mismatch"}}});
    }
    session->dispatch_requested = true;
    session->state = ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested;
  }
  session->changed.notify_all();
  return Result<void>::Ok();
}

Result<void> ExecutionKernel::CancelPreparedResumeMotion(const std::uint64_t execution_id) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->execution_id != execution_id) {
      return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                        "Prepared Resume execution identity is stale",
                                        {{"reason", "prepared_resume_execution_id_mismatch"}}});
    }
    if (!session->resume_cycle ||
        session->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched) {
      return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                                        "No cancellable prepared Resume is resident",
                                        {{"reason", "prepared_resume_not_cancellable"}}});
    }
    if (session->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
        session->state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
      return Result<void>::Ok();
    }
    session->cancel_requested = true;
    session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
  }
  session->changed.notify_all();
  request_active_planning_cancel();
  request_active_validation_cancel("prepared_resume_cancelled_pre_dispatch");
  return Result<void>::Ok();
}

Result<void> ExecutionKernel::CancelPreparedMotion(const std::uint64_t execution_id) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->execution_id != execution_id) {
      return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                        "Prepared motion execution identity is stale",
                                        {{"reason", "prepared_motion_execution_id_mismatch"}}});
    }
    if (session->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched) {
      return Result<void>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                   "A dispatched participant requires the prepared Stop-stage path",
                   {{"reason", "prepared_motion_already_dispatched"}}});
    }
    if (session->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
        session->state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
      return Result<void>::Ok();
    }
    session->cancel_requested = true;
    session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
  }
  session->changed.notify_all();
  request_active_planning_cancel();
  request_active_validation_cancel("prepared_motion_cancelled_pre_dispatch");
  return Result<void>::Ok();
}

Result<std::uint64_t> ExecutionKernel::PrepareCapabilityCommand(
    ResolvedPreparedCapabilityCommandExecutionRequestDTO request) {
  if (!request.participant) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared capability command requires one immutable participant",
                 {{"reason", "prepared_command_participant_missing"}}});
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *request.participant, request.binding, request.stop_readiness, nullptr,
      request.retained_default_stop_limits, request.stop_node_category);
  if (!verified.ok()) {
    return Result<std::uint64_t>::Err(verified.error());
  }
  if (!std::holds_alternative<PreparedCapabilityCommandDTO>(request.participant->payload) ||
      !prepared_command_matches_private_request(*request.participant, request.binding.value(),
                                                request.command_request)) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Prepared capability command differs from its compiled private request",
                 {{"reason", "prepared_command_authority_mismatch"}}});
  }
  auto validated = validate_gripper_command(request.command_request);
  if (!validated.ok()) {
    return Result<std::uint64_t>::Err(validated.error());
  }
  auto prepared = MakePrepareParticipantResult(*request.participant,
                                               PrepareParticipantResultStateDTO::kPrepared);
  if (!prepared.ok()) {
    return Result<std::uint64_t>::Err(prepared.error());
  }

  std::shared_ptr<PreparedCapabilityCommandSession> session;
  try {
    session = std::make_shared<PreparedCapabilityCommandSession>(std::move(request));
  } catch (...) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared capability-command session allocation failed",
                 {{"reason", "prepared_command_session_allocation_failed"}}});
  }
  const auto preparation_id = next_execution_id_.fetch_add(1U, std::memory_order_relaxed);
  if (preparation_id == 0U || preparation_id == std::numeric_limits<std::uint64_t>::max()) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared capability-command identity space is exhausted",
                 {{"reason", "prepared_command_identity_exhausted"}}});
  }
  session->preparation_id = preparation_id;
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kPrepared;
  session->backend_prepare_result = std::move(prepared.value());

  std::lock_guard active_lock(prepared_command_session_mutex_);
  if (active_prepared_command_session_) {
    std::lock_guard prior_lock(active_prepared_command_session_->mutex);
    const auto& prior = *active_prepared_command_session_;
    if (prior.authority.participant &&
        prior.authority.participant->prepared_contract_digest ==
            session->authority.participant->prepared_contract_digest) {
      return Result<std::uint64_t>::Ok(prior.preparation_id);
    }
    bool prior_effect_active =
        prior.state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested;
    if (prior.state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched &&
        prior.dispatch_result.has_value() && prior.dispatch_result->accepted) {
      const auto status =
          get_gripper_status(prior.authority.participant->command_target_id.value());
      prior_effect_active = status.state == GripperCommandState::kExecuting &&
                            status.command_id == prior.dispatch_result->command_id &&
                            status.generation == prior.dispatch_result->generation;
    }
    if (prior.state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
        prior_effect_active) {
      return Result<std::uint64_t>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Another prepared capability command is still resident",
                   {{"reason", "prepared_command_session_busy"}}});
    }
  }
  active_prepared_command_session_ = std::move(session);
  return Result<std::uint64_t>::Ok(preparation_id);
}

Result<ResolvedPreparedCapabilityCommandSnapshotDTO>
ExecutionKernel::ObservePreparedCapabilityCommand(const std::uint64_t preparation_id) const {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
  }
  if (!session) {
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command session is not resident",
                 {{"reason", "prepared_command_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  if (session->preparation_id != preparation_id) {
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command identity is stale",
                 {{"reason", "prepared_command_identity_mismatch"}}});
  }
  return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Ok(
      ResolvedPreparedCapabilityCommandSnapshotDTO{
          session->preparation_id, session->state, session->authority.participant,
          session->backend_prepare_result, session->controller_goal_id,
          session->controller_goal_generation, session->dispatch_result, session->error});
}

Result<GripperCommandResultDTO> ExecutionKernel::DispatchPreparedCapabilityCommand(
    const std::uint64_t preparation_id, const ContentDigest& prepared_contract_digest,
    const ControllerGoalId& controller_goal_id,
    const ControllerGoalGeneration controller_goal_generation) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
  }
  if (!session) {
    return Result<GripperCommandResultDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Prepared capability-command session is not resident",
                 {{"reason", "prepared_command_session_missing"}}});
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->preparation_id != preparation_id) {
      return Result<GripperCommandResultDTO>::Err(
          ErrorDTO{kErrorMissingReference,
                   "Prepared capability-command identity is stale",
                   {{"reason", "prepared_command_identity_mismatch"}}});
    }
    const bool exact_dispatch =
        session->authority.participant &&
        session->authority.participant->prepared_contract_digest == prepared_contract_digest &&
        session->controller_goal_id == controller_goal_id &&
        session->controller_goal_generation == controller_goal_generation;
    if ((session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched ||
         session->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed) &&
        exact_dispatch && session->dispatch_result.has_value()) {
      return Result<GripperCommandResultDTO>::Ok(*session->dispatch_result);
    }
    if (session->state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
        !session->authority.participant ||
        session->authority.participant->prepared_contract_digest != prepared_contract_digest ||
        controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
      return Result<GripperCommandResultDTO>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Prepared capability-command dispatch identity is invalid",
                   {{"reason", "prepared_command_dispatch_mismatch"}}});
    }
    session->controller_goal_id = controller_goal_id;
    session->controller_goal_generation = controller_goal_generation;
    session->state = ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested;
  }

  GripperCommandResultDTO dispatched;
  try {
    dispatched = command_gripper(session->authority.command_request);
  } catch (const std::exception& exception) {
    dispatched = GripperCommandResultDTO{
        false, 0U, 0U,
        ErrorDTO{
            kErrorExecutionRejected,
            "Capability-command backend threw during dispatch",
            {{"reason", "prepared_command_dispatch_exception"}, {"exception", exception.what()}}}};
  } catch (...) {
    dispatched =
        GripperCommandResultDTO{false, 0U, 0U,
                                ErrorDTO{kErrorExecutionRejected,
                                         "Capability-command backend threw during dispatch",
                                         {{"reason", "prepared_command_dispatch_exception"},
                                          {"exception", reason_codes::kUnknownNonStdException}}}};
  }
  {
    std::lock_guard lock(session->mutex);
    session->dispatch_result = dispatched;
    session->error = dispatched.error;
    session->state = dispatched.accepted && !dispatched.error.has_value()
                         ? ResolvedPreparedCapabilityCommandStateDTO::kDispatched
                         : ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed;
  }
  return Result<GripperCommandResultDTO>::Ok(std::move(dispatched));
}

Result<void> ExecutionKernel::CancelPreparedCapabilityCommand(const std::uint64_t preparation_id) {
  std::shared_ptr<PreparedCapabilityCommandSession> session;
  {
    std::lock_guard lock(prepared_command_session_mutex_);
    session = active_prepared_command_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared capability-command session is not resident",
                                      {{"reason", "prepared_command_session_missing"}}});
  }
  std::lock_guard lock(session->mutex);
  if (session->preparation_id != preparation_id) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared capability-command identity is stale",
                                      {{"reason", "prepared_command_identity_mismatch"}}});
  }
  if (session->state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
    return Result<void>::Ok();
  }
  if (session->state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared &&
      session->state != ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed) {
    return Result<void>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                                      "A released capability command requires the Stop-stage path",
                                      {{"reason", "prepared_command_already_dispatched"}}});
  }
  session->state = ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
  return Result<void>::Ok();
}

Result<uint64_t> ExecutionKernel::execute_impl(
    const ExecuteRequestDTO& request, std::shared_ptr<PreparedMotionSession> prepared_session) {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
  lifecycle_stop_requested_event_emitted_.store(false, std::memory_order_seq_cst);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
  clear_runtime_collision_state_projection();
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire)) {
    return Result<uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                          "Execution worker retirement is in progress",
                                          {{"reason_code", "worker_retirement_in_progress"}}});
  }
  if (lifecycle_reset_waiting_for_stop_.load(std::memory_order_acquire)) {
    return Result<uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                          "Reset is waiting for stop completion",
                                          {{"reason_code", "reset_waiting_for_stop"}}});
  }

  switch (sm_.status().state) {
    using enum ExecutionState;
    case kIdle:
    case kCompleted:
    case kStopped:
    case kFailed:
    case kInterventionRequired:
      if (!retire_execute_worker(lifecycle_lock)) {
        return Result<uint64_t>::Err(
            ErrorDTO{kErrorExecutionRejected,
                     "Execution cannot be started from the active execution worker",
                     {{"reason_code", "worker_retirement_self_thread"}}});
      }
      break;
    default:
      break;
  }

  switch (sm_.status().state) {
    using enum ExecutionState;
    case kIdle:
    case kCompleted:
    case kStopped:
    case kFailed:
    case kInterventionRequired:
      planning_reset_requested_.store(false, std::memory_order_release);
      break;
    default:
      break;
  }

  // `kInterventionRequired` MUST be cleared by an explicit recovery action,
  // not silently by `execute()`. An implicit clear here would erase the
  // operator intervention boundary: any caller could re-issue `execute` to
  // bypass the intervention gate with no audit event.
  if (sm_.status().state == ExecutionState::kInterventionRequired) {
    return Result<uint64_t>::Err(ErrorDTO{kErrorInvalidRequest,
                                          "Execution is in InterventionRequired; explicit recovery "
                                          "action required",
                                          {{"requires_operator_intervention", "true"},
                                           {"reason_code", "intervention_required"},
                                           {"state", to_string(sm_.status().state)}}});
  }

  if (sm_.status().state == ExecutionState::kFailed ||
      sm_.status().state == ExecutionState::kStopped) {
    const auto prev_state = sm_.status().state;
    auto reset_result = stop_policy_.request_reset();
    if (!reset_result.ok()) {
      return Result<uint64_t>::Err(reset_result.error());
    }
    clear_active_request_correlation();
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                prev_state,
                ExecutionState::kIdle,
                std::nullopt,
                {}});
  }

  if (sm_.status().state != ExecutionState::kIdle) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }

  if (!std::isfinite(request.timeouts.planning_timeout_s) ||
      request.timeouts.planning_timeout_s <= 0.0) {
    return Result<uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "planning_timeout_s must be finite and strictly positive",
                 {{"reason_code", "invalid_planning_timeout"},
                  {"planning_timeout_s", std::to_string(request.timeouts.planning_timeout_s)}}});
  }
  if (!std::isfinite(request.timeouts.execution_timeout_s) ||
      request.timeouts.execution_timeout_s <= 0.0) {
    return Result<uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "execution_timeout_s must be finite and strictly positive",
                 {{"reason_code", "invalid_execution_timeout"},
                  {"execution_timeout_s", std::to_string(request.timeouts.execution_timeout_s)}}});
  }

  terminal_stop_authority_asserted_.store(false, std::memory_order_release);
  terminal_fault_authority_asserted_.store(false, std::memory_order_release);
  uint64_t exec_id = next_execution_id_++;
  auto prev_state = sm_.status().state;
  sm_.set_execution_id(exec_id);
  set_active_request_correlation(request);
  if (request.initial_runtime_scale.has_value()) {
    auto scale_acceptance = set_runtime_scale(*request.initial_runtime_scale);
    if (!scale_acceptance.ok()) {
      clear_active_request_correlation();
      sm_.set_execution_id(0);
      return Result<uint64_t>::Err(scale_acceptance.error());
    }
  }
  auto transition = sm_.try_transition(ExecutionState::kPlanning);
  if (!transition.ok()) {
    clear_active_request_correlation();
    sm_.set_execution_id(0);
    return Result<uint64_t>::Err(ErrorCode::kInternalError);
  }
  authorize_gripper_authority();

  worker_signals_.mark_started();
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown);

  emit_event({KernelEventType::kStateTransition,
              exec_id,
              clock_.now(),
              prev_state,
              ExecutionState::kPlanning,
              std::nullopt,
              {}});

  ExecutionSnapshot admission_snapshot;
  admission_snapshot.timestamp_ns = clock_.now();
  admission_snapshot.execution_id = exec_id;
  admission_snapshot.move_id = request.move_id;
  admission_snapshot.state = ExecutionState::kPlanning;
  admission_snapshot.progress = 0.0;
  publish_execution_snapshot(std::move(admission_snapshot));

  const auto finish_admission_without_worker = [&]() {
    execute_admission_interruptible_.store(false, std::memory_order_seq_cst);
    finish_execute_worker();
  };

  std::string admitted_contact_policy_id = request.contact_policy_id;

  const auto finalize_admitted_without_worker = [&](MotionExecutionTerminalCause terminal_cause,
                                                    std::string reap_reason) {
    ActiveMotionExecution motion_state;
    motion_state.execution_id = exec_id;
    motion_state.move_id = request.move_id;
    motion_state.had_scopes = !request.motion_interrupt_scopes.empty();
    motion_state.correlation_captured = true;
    motion_state.action_span = request.action_span;
    motion_state.contact_policy_id = admitted_contact_policy_id;
    finalize_motion_execution_outcome(motion_state, terminal_cause, std::nullopt, std::nullopt,
                                      std::move(reap_reason));
  };

  const auto fail_admission = [&](ErrorDTO error) -> Result<uint64_t> {
    sm_.set_last_error(error_code_from_dto(error));
    const auto failed_from_state = sm_.status().state;
    auto fail_transition = commit_terminal(ExecutionState::kFailed, "terminal_commit");
    auto failed_snapshot = get_execution_snapshot();
    failed_snapshot.timestamp_ns = clock_.now();
    failed_snapshot.execution_id = exec_id;
    failed_snapshot.move_id = request.move_id;
    failed_snapshot.state = sm_.status().state;
    failed_snapshot.error = error;
    publish_execution_snapshot(std::move(failed_snapshot));
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, error, {}});
    if (fail_transition.ok()) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  failed_from_state,
                  ExecutionState::kFailed,
                  std::nullopt,
                  {{"reason", "execute_admission_failed"}}});
    }
    finish_admission_without_worker();
    return Result<uint64_t>::Err(std::move(error));
  };

  const auto reject_admission = [&](ErrorDTO error) -> Result<uint64_t> {
    const auto rejected_from_state = sm_.status().state;
    (void)stop_policy_.request_reset();
    clear_active_request_correlation();
    sm_.set_execution_id(0);
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    if (rejected_from_state != ExecutionState::kIdle) {
      emit_event({KernelEventType::kStateTransition,
                  exec_id,
                  clock_.now(),
                  rejected_from_state,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {{"reason", "execute_admission_rejected"}}});
    }
    finish_admission_without_worker();
    return Result<uint64_t>::Err(std::move(error));
  };

  execute_admission_interruptible_.store(true, std::memory_order_seq_cst);

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return reject_admission(sync_result.error());
  }

  auto retained_scene_result =
      retain_current_scene_snapshot_lease_inventory(exec_id, request.move_id);
  if (!retained_scene_result.ok()) {
    return reject_admission(std::move(retained_scene_result.error()));
  }
  auto retained_scene = std::move(retained_scene_result.value());
  const auto projection_binding = retained_scene.binding();
  if (!projection_binding.valid()) {
    return reject_admission(ErrorDTO{kErrorExecutionRejected,
                                     "Retained planner projection binding is invalid",
                                     {{"reason", "planner_projection_binding_invalid"}}});
  }
  ExecuteRequestDTO resolved_request(request);
  auto contact_grasp_binding =
      install_resolved_contact_grasp_policy(resolved_request, retained_scene.mutable_inventory());
  if (!contact_grasp_binding.ok()) {
    return reject_admission(contact_grasp_binding.error());
  }
  admitted_contact_policy_id = resolved_request.contact_policy_id;
  set_active_request_correlation(resolved_request);
  std::optional<SceneSnapshotLease> scene_snapshot_lease;
  scene_snapshot_lease =
      build_scene_snapshot_lease(retained_scene.inventory(), resolved_request, exec_id);

  std::optional<RobotStateDTO> synced_state;
  auto observed_start = observed_state_.current_state(clock_.now());
  if (observed_start.ok()) {
    synced_state = std::move(observed_start.value());
  }

  std::optional<ProgramFrameResolutionContextDTO> resolution_context;
  if (request.target_mode == MotionTargetModeDTO::kRelative) {
    if (!synced_state.has_value()) {
      return reject_admission(ErrorDTO{kErrorStartStateMismatch,
                                       "Relative motion requires an authority-selected start state",
                                       {{"reason", "relative_start_state_unavailable"}}});
    }
    const auto capture_time_ns = clock_.now();
    auto observed_publication = observed_state_.addressed_publication(
        request.binding.control_binding_id, request.binding.control_binding_revision);
    if (!observed_publication.ok()) {
      return reject_admission(observed_publication.error());
    }
    auto controlled_start =
        controlled_state_for_publication(request, observed_publication.value(), capture_time_ns);
    if (!controlled_start.ok()) {
      return reject_admission(controlled_start.error());
    }
    std::vector<ReferenceDTO> requested_references;
    if (request.target_reference.has_value()) {
      requested_references.push_back(*request.target_reference);
    }
    if (request.offset_reference.has_value()) {
      requested_references.push_back(*request.offset_reference);
    }
    ProgramFrameResolutionCaptureDTO capture{request.binding,
                                             controlled_start.value(),
                                             observed_publication.value(),
                                             projection_binding,
                                             retained_scene.inventory().logical_scene,
                                             request.runtime_object_bindings,
                                             std::move(requested_references),
                                             capture_time_ns};
    auto captured = reference_resolver_.capture_program_frame_resolution_context(capture);
    if (!captured.ok()) {
      return reject_admission(captured.error());
    }
    resolution_context.emplace(std::move(captured.value()));
    if (!resolution_context->valid() || resolution_context->binding != capture.binding ||
        resolution_context->start_state != capture.start_state ||
        resolution_context->observed_start_publication != capture.observed_start_publication ||
        resolution_context->projection_binding != capture.projection_binding ||
        resolution_context->logical_scene != capture.logical_scene ||
        resolution_context->runtime_object_bindings != capture.runtime_object_bindings ||
        resolution_context->captured_at_ns != capture.captured_at_ns) {
      return reject_admission(
          ErrorDTO{kErrorExecutionRejected,
                   "Program reference context differs from the retained admission",
                   {{"reason", "program_frame_resolution_context_invalid"}}});
    }
  }
  auto resolved_motion = resolve_motion_target(
      request, resolution_context.has_value() ? &*resolution_context : nullptr,
      reference_resolver_);
  if (!resolved_motion.ok()) {
    const auto cache_key = trajectory_cache_key(request);
    if (request.cache_mode != CacheMode::kDisable && cache_.contains(cache_key)) {
      cache_.invalidate(cache_key);
      emit_event({KernelEventType::kCacheInvalidated,
                  0,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id}, {"reason", "goal_unverifiable"}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_invalidations++;
    }
    return reject_admission(resolved_motion.error());
  }
  resolved_request.motion = std::move(resolved_motion.value());

  if (!synced_state.has_value()) {
    return reject_admission(ErrorDTO{kErrorStartStateMismatch,
                                     "Planning requires an authority-selected observed start state",
                                     {{"reason", "planning_start_state_unavailable"}}});
  }
  auto dynamic_start_contacts = discover_dynamic_carried_start_contacts(
      retained_scene.mutable_inventory(), *synced_state, exec_id);
  if (!dynamic_start_contacts.ok()) {
    return reject_admission(dynamic_start_contacts.error());
  }
  resolved_request.dynamically_discovered_start_contact_count = dynamic_start_contacts.value();

  PreparedExecutionStart prepared;
  const auto active_collision_world_revision = retained_scene.inventory().bundle.world->revision;
  const TrajectoryCacheRevisionEvidence active_cache_evidence{
      projection_binding.projection_revision, active_collision_world_revision};
  const auto cache_key = trajectory_cache_key(resolved_request);
  if (request.cache_mode != CacheMode::kDisable) {
    // Exact-revision entries are cache hits. A same-intent older-revision entry
    // is only a current-snapshot revalidation candidate; it carries no stale
    // proof and cannot dispatch until the ordinary validation gate restamps it.
    const auto intent_identity = trajectory_cache_intent_identity(resolved_request);
    auto cache_lookup =
        cache_.lookup_with_revision(cache_key, active_cache_evidence, intent_identity);
    const bool exact_cache_hit = cache_lookup.disposition == TrajectoryCacheLookupDisposition::kHit;
    const bool revalidation_candidate =
        cache_lookup.disposition == TrajectoryCacheLookupDisposition::kRevalidationCandidate;
    if ((exact_cache_hit || revalidation_candidate) && cache_lookup.trajectory.has_value()) {
      auto& cached = *cache_lookup.trajectory;
      const auto goal_match =
          cached_trajectory_matches_request_goal(kinematics_, cached, resolved_request);
      const auto start_match =
          synced_state.has_value()
              ? cached_trajectory_matches_current_start(cached, *synced_state,
                                                        request.tolerances.joint_position_tol_rad)
              : incompatible_cache("start_state_changed");
      if (goal_match.compatible && start_match.compatible) {
        prepared.cache_hit = exact_cache_hit;
        prepared.cache_revalidation_candidate = revalidation_candidate;
        prepared.trajectory = std::move(cached);
      } else {
        cache_.invalidate(cache_key);
        prepared.cache_invalidated = true;
        prepared.cache_invalidation_reason =
            !start_match.compatible ? start_match.reason : goal_match.reason;
      }
    } else if (cache_lookup.disposition == TrajectoryCacheLookupDisposition::kIntentMismatch) {
      prepared.cache_invalidated = true;
      prepared.cache_invalidation_reason = "planner_intent_changed";
    } else if (cache_lookup.disposition == TrajectoryCacheLookupDisposition::kRevisionMismatch) {
      prepared.cache_invalidated = true;
      prepared.cache_invalidation_reason = "collision_world_revision_changed";
    } else {
      prepared.cache_miss = true;
    }
  }

  execute_admission_interruptible_.store(false, std::memory_order_seq_cst);

  const bool pending_reset =
      lifecycle_reset_requested_while_busy_.exchange(false, std::memory_order_acq_rel);
  const bool pending_stop =
      lifecycle_stop_requested_while_busy_.exchange(false, std::memory_order_seq_cst);
  if (pending_reset) {
    // Execute admission has not launched a motion worker or operator-stop
    // worker yet; reset can clear the admission without touching any selected
    // stop profile. Active operator stops are handled by reset() in Stopping.
    invalidate_gripper_authority(GripperInvalidationReason::kReset);
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return fail_admission(backend_reset.error.value_or(
          ErrorDTO{kErrorResetFailed,
                   "Execution backend reset failed during pending reset",
                   {{"reason", "pending_reset_backend_failed"}}}));
    }
    auto result = stop_policy_.request_reset();
    if (!result.ok()) {
      return fail_admission(result.error());
    }
    clear_active_request_correlation();
    sm_.set_execution_id(0);
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    authorize_gripper_authority();
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kPlanning,
                ExecutionState::kIdle,
                std::nullopt,
                {{"reason", "pending_reset_during_execute_admission"}}});
    finalize_admitted_without_worker(MotionExecutionTerminalCause::kStopped,
                                     "pending_reset_during_execute_admission");
    finish_admission_without_worker();
    return Result<uint64_t>::Ok(exec_id);
  }
  if (pending_stop) {
    auto result = stop_policy_.request_hard_stop();
    if (!result.ok()) {
      return fail_admission(result.error());
    }
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = exec_id;
    snapshot.move_id = request.move_id;
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    const bool stop_requested_already_emitted =
        lifecycle_stop_requested_event_emitted_.exchange(false, std::memory_order_seq_cst);
    if (!stop_requested_already_emitted) {
      emit_event({KernelEventType::kStopRequested,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "pending_stop_during_execute_admission"}}});
    }
    emit_event({KernelEventType::kStopIssued, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kPlanning,
                sm_.status().state,
                std::nullopt,
                {{"reason", "pending_stop_during_execute_admission"}}});
    finalize_admitted_without_worker(MotionExecutionTerminalCause::kStopped,
                                     "pending_stop_during_execute_admission");
    finish_admission_without_worker();
    return Result<uint64_t>::Ok(exec_id);
  }

  const auto initial_state = (prepared.cache_hit || prepared.cache_revalidation_candidate)
                                 ? ExecutionState::kValidating
                                 : ExecutionState::kPlanning;
  if (initial_state != ExecutionState::kPlanning) {
    auto cache_hit_transition = sm_.try_transition(initial_state);
    if (!cache_hit_transition.ok()) {
      clear_active_request_correlation();
      sm_.set_execution_id(0);
      finish_admission_without_worker();
      return Result<uint64_t>::Err(ErrorCode::kInternalError);
    }
    emit_event({KernelEventType::kStateTransition,
                exec_id,
                clock_.now(),
                ExecutionState::kPlanning,
                initial_state,
                std::nullopt,
                {}});
  }

  if (prepared.cache_invalidated) {
    emit_event({KernelEventType::kCacheInvalidated,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"move_id", request.move_id}, {"reason", prepared.cache_invalidation_reason}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_invalidations++;
  }

  if (prepared.cache_hit) {
    emit_event({KernelEventType::kCacheHit, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_hits++;
  } else if (prepared.cache_revalidation_candidate) {
    emit_event(
        {KernelEventType::kCacheRevalidationCandidate,
         exec_id,
         clock_.now(),
         {},
         {},
         std::nullopt,
         {{"move_id", request.move_id},
          {"current_revision_id", std::to_string(active_collision_world_revision.revision_id)}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.cache_revalidation_candidates++;
  } else {
    if (prepared.cache_miss) {
      emit_event({KernelEventType::kCacheMiss,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"move_id", request.move_id}}});
      std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
      metrics_.cache_misses++;
    }
    emit_event({KernelEventType::kPlanStarted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
  }

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = request.move_id;
  snapshot.state = initial_state;
  snapshot.progress = 0.0;
  auto snapshot_state = observed_state_.current_state(clock_.now());
  if (snapshot_state.ok()) {
    snapshot.joint_positions_rad = std::move(snapshot_state.value().positions_rad);
  }
  publish_execution_snapshot(std::move(snapshot));

  if (prepared_session) {
    {
      std::lock_guard session_lock(prepared_session->mutex);
      prepared_session->execution_id = exec_id;
    }
    std::lock_guard active_lock(prepared_motion_session_mutex_);
    active_prepared_motion_session_ = prepared_session;
  }

  auto worker_start_gate = std::make_shared<WorkerStartGate>();
  const auto worker_generation = ++execute_worker_generation_;
  worker_thread_ = std::jthread(
      [this, request_copy = std::move(resolved_request), exec_id,
       cached_trajectory = (prepared.cache_hit || prepared.cache_revalidation_candidate)
                               ? std::make_optional(std::move(prepared.trajectory))
                               : std::nullopt,
       cache_revalidation_candidate = prepared.cache_revalidation_candidate,
       retained_scene = std::move(retained_scene),
       scene_snapshot_lease = std::move(scene_snapshot_lease),
       prepared_session = std::move(prepared_session),
       worker_start_gate](std::stop_token st) mutable {
        if (!worker_start_gate->wait(st)) {
          return;
        }
        async_execute_worker(st, std::move(request_copy), exec_id, std::move(cached_trajectory),
                             cache_revalidation_candidate, std::move(retained_scene),
                             std::move(scene_snapshot_lease), std::move(prepared_session));
      });

  execute_admission_interruptible_.store(false, std::memory_order_seq_cst);
  lifecycle_lock.unlock();
  event_scope.flush();
  bool finalize_pre_dispatch_stop = false;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const auto status = sm_.status();
    const bool should_start_worker =
        status.execution_id == exec_id &&
        (status.state == ExecutionState::kPlanning || status.state == ExecutionState::kValidating);
    if (!should_start_worker) {
      const bool same_worker = worker_generation == execute_worker_generation_;
      if (same_worker && worker_thread_.joinable()) {
        worker_thread_.request_stop();
      }
      if (same_worker) {
        worker_signals_.mark_complete();
      }
      finalize_pre_dispatch_stop = true;
    }
    worker_start_gate->release();
  }
  if (finalize_pre_dispatch_stop) {
    finalize_admitted_without_worker(MotionExecutionTerminalCause::kStopped,
                                     "execute_admission_cancelled_before_worker_start");
  }
  return Result<uint64_t>::Ok(exec_id);
}

std::uint64_t ExecutionKernel::control_command_epoch() const {
  return control_command_epoch_.load(std::memory_order_acquire);
}

std::uint64_t ExecutionKernel::bump_control_command_epoch() {
  return control_command_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool ExecutionKernel::control_command_epoch_matches(
    std::optional<std::uint64_t> expected_epoch) const {
  return !expected_epoch.has_value() ||
         *expected_epoch == control_command_epoch_.load(std::memory_order_acquire);
}

StopResultDTO ExecutionKernel::stop() {
  EventDeferralScope event_scope(*this);
  // Operator stop intent outranks future-trajectory warning work even if the
  // lifecycle mutex is busy or the backend later rejects the stop command.
  // The session invalidates lock-free first and only try-locks its leaf state.
  revoke_runtime_collision_lookahead_clear_permits();
  bool stop_epoch_bumped = false;
  const auto accepted_stop = [this, &stop_epoch_bumped](bool lifecycle_held = true) {
    if (!stop_epoch_bumped) {
      bump_control_command_epoch();
      stop_epoch_bumped = true;
    }
    invalidate_gripper_authority(GripperInvalidationReason::kStop);
    if (lifecycle_held) {
      // Rebind the published recovery assessment to the post-stop epoch
      // while still inside stop()'s lifecycle hold: commit-coupled, and a
      // request bound to the returned assessment admits without an epoch
      // mismatch. The busy path (try-lock failed) skips this — it only
      // records deferred stop intent and owns no authority facts to
      // publish; the deferred stop's own terminal path refreshes.
      (void)refresh_recovery_assessment_locked("stop_epoch_bumped");
    }
    return StopResultDTO{true, std::nullopt};
  };
  join_finished_operator_stop_worker();
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      lifecycle_stop_requested_while_busy_.store(true, std::memory_order_seq_cst);
      lifecycle_stop_requested_event_emitted_.store(true, std::memory_order_seq_cst);
      emit_event({KernelEventType::kStopRequested,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {{"reason", "lifecycle_busy"}}});
      if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
        return accepted_stop(/*lifecycle_held=*/false);
      }
    }
    lifecycle_lock.lock();
  }
  if (lifecycle_stop_requested_event_emitted_.exchange(false, std::memory_order_seq_cst) &&
      !lifecycle_stop_requested_while_busy_.load(std::memory_order_seq_cst) &&
      sm_.status().state == ExecutionState::kStopped) {
    return accepted_stop();
  }
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);

  // Per spec/230 §6.1 Stop chain:
  //   stop_requested → state_transition(Stopping) → stop_issued
  //                  → state_transition(Stopped)
  // kStopRequested marks operator intent at the API boundary; the
  // intermediate kStopping covers the controlled-stop deceleration
  // window (1-2 s for soft-on-path), making the "stopping…" period
  // observable to the UI and to trace reconstruction.
  const auto prev_state = sm_.status().state;
  emit_event({KernelEventType::kStopRequested,
              sm_.status().execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {}});

  // Pre-transition to kStopping when the source state has a
  // deceleration window. The state machine allows kStopping from
  // Executing/Paused; Planning/Validating cancel direct to Stopped
  // (no intermediate to observe). stop_policy_.request_hard_stop()
  // handles the final Stopping → Stopped step.
  const bool transitioned_to_stopping =
      (prev_state == ExecutionState::kExecuting || prev_state == ExecutionState::kPaused) &&
      sm_.try_transition(ExecutionState::kStopping).ok();
  if (transitioned_to_stopping) {
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                prev_state,
                ExecutionState::kStopping,
                std::nullopt,
                {}});
  }
  // `pre_completion_state` is the from-state for any subsequent
  // state-transition event so the trace remains causal.
  const ExecutionState pre_completion_state =
      transitioned_to_stopping ? ExecutionState::kStopping : prev_state;

  const bool operator_profile_applicable =
      config_.operator_stop_profile && operator_controlled_stop_allowed(prev_state);
  const auto make_hard_stop_preterminal =
      [this, &stop_epoch_bumped](std::uint64_t stop_owner_generation) {
        return [this, &stop_epoch_bumped, stop_owner_generation]() {
          if (!stop_epoch_bumped) {
            bump_control_command_epoch();
            stop_epoch_bumped = true;
          }
          KernelStopCertificateDTO certificate;
          certificate.completion_result = StopCertificateOutcome::kSucceeded;
          certificate.stop_owner = StopOwnerKind::kOperatorStopWorker;
          certificate.stop_owner_generation = stop_owner_generation;
          certificate.execution_id = sm_.status().execution_id;
          certificate.control_command_epoch = control_command_epoch();
          certificate.requested_stop_mode = "hard_stop";
          certificate.executed_stop_mode = "hard_stop";
          certificate.command_source = "operator_stop_hard";
          certificate.preemption_result = InterruptPreemptionResult::kNoneRequired;
          certificate.backend_stop_confirmed = true;
          certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
          certificate.completed_at_ns = clock_.now();
          publish_stop_certificate(std::move(certificate));
          (void)publish_terminal_recovery_assessment_locked(ExecutionState::kStopped,
                                                            "operator_hard_stop_preterminal");
        };
      };

  // Repeated Stop while an operator controlled stop is already in flight is
  // idempotent. Escalation to hard stop is an explicit reset/timeout path, not
  // an accidental semantic change from a second Stop button press.
  if (prev_state == ExecutionState::kStopping) {
    if (operator_stop_worker_.active()) {
      lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
      return accepted_stop();
    }
    if (motion_trip_stop_.active()) {
      (void)motion_trip_stop_.request_stop();
      lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
      return accepted_stop();
    }
  }

  if (operator_profile_applicable) {
    const auto stop_owner_generation =
        begin_operator_stop_owner_generation(sm_.status().execution_id);
    const auto runtime_collision_generation = start_collision_monitoring(
        "operator_stop", RuntimeCollisionProtectionPurpose::kControlledStop);
    cache_.invalidate_all();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    if (!start_operator_stop_worker(*config_.operator_stop_profile, pre_completion_state,
                                    runtime_collision_generation, stop_owner_generation)) {
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                runtime_collision_generation);
      auto result =
          stop_policy_.request_hard_stop(make_hard_stop_preterminal(stop_owner_generation));
      if (!result.ok()) {
        return StopResultDTO{false, result.error()};
      }
      auto hard_snapshot = get_execution_snapshot();
      hard_snapshot.timestamp_ns = clock_.now();
      hard_snapshot.state = sm_.status().state;
      publish_execution_snapshot(std::move(hard_snapshot));
      emit_event({KernelEventType::kStopIssued,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  std::nullopt,
                  {}});
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  pre_completion_state,
                  sm_.status().state,
                  std::nullopt,
                  {{"reason", "operator_stop_worker_unavailable_hard_stop"}}});
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.stops_issued++;
    }
    lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
    return accepted_stop();
  }

  const auto stop_owner_generation =
      begin_operator_stop_owner_generation(sm_.status().execution_id);
  auto result = stop_policy_.request_hard_stop(make_hard_stop_preterminal(stop_owner_generation));
  if (!result.ok()) {
    return StopResultDTO{false, result.error()};
  }
  if (prev_state == ExecutionState::kPlanning) {
    request_active_planning_cancel();
  } else if (prev_state == ExecutionState::kValidating) {
    request_active_validation_cancel("operator_stop");
  }

  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced);
  auto snapshot = get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = sm_.status().state;
  publish_execution_snapshot(std::move(snapshot));
  // Emit kStopIssued event
  emit_event({KernelEventType::kStopIssued,
              sm_.status().execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {}});
  // Emit terminal state transition. `pre_completion_state` is kStopping
  // when the source state had a deceleration window (Executing/Paused),
  // matching the spec/230 §6.1 stop chain; for Planning/Validating
  // cancellations it is the original prev_state and the transition goes
  // direct to kStopped.
  auto new_state = sm_.status().state;
  emit_event({KernelEventType::kStateTransition,
              sm_.status().execution_id,
              clock_.now(),
              pre_completion_state,
              new_state,
              std::nullopt,
              {}});
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.stops_issued++;
  }
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
  return accepted_stop();
}

PauseResultDTO ExecutionKernel::pause() {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      return PauseResultDTO{false, ErrorDTO{kErrorInvalidRequest,
                                            "Pause is only valid during Executing",
                                            {{"reason_code", "pause_not_executing"},
                                             {"state", to_string(sm_.status().state)}}}};
    }
    lifecycle_lock.lock();
  }

  // Per spec/230 §6.1 Pause chain:
  //   pause_requested → state_transition(Stopping) → state_transition(Paused)
  // The Executing → Stopping transition is synchronous inside
  // stop_policy_.request_pause(); the Stopping → Paused transition fires
  // asynchronously via the scheduled lambda there. The
  // pause_completed_callback (wired in the constructor) emits the
  // terminal kStateTransition(Paused) event when that lambda runs.
  const auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_pause();
  if (result.ok()) {
    auto backend_pause = execution_backend_.pause();
    if (!backend_pause.accepted) {
      auto error = backend_pause.error.value_or(ErrorDTO{kErrorStopFailed,
                                                         "Execution backend refused pause",
                                                         {{"reason", "backend_pause_refused"}}});
      if (!error.details.contains("reason")) {
        error.details["reason"] = "backend_pause_refused";
      }
      sm_.set_last_error(error_code_from_dto(error));
      auto transition = commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");
      auto snapshot = get_execution_snapshot();
      snapshot.timestamp_ns = clock_.now();
      snapshot.state = sm_.status().state;
      snapshot.error = error;
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kErrorRaised,
                  sm_.status().execution_id,
                  clock_.now(),
                  {},
                  {},
                  error,
                  error.details});
      if (transition.ok()) {
        emit_event({KernelEventType::kStateTransition,
                    sm_.status().execution_id,
                    clock_.now(),
                    ExecutionState::kStopping,
                    ExecutionState::kInterventionRequired,
                    std::nullopt,
                    {{"reason", "backend_pause_refused"}}});
      }
      return PauseResultDTO{false, error};
    }
    invalidate_gripper_authority(GripperInvalidationReason::kPause);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kPauseRequested,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                prev_state,
                ExecutionState::kStopping,
                std::nullopt,
                {}});
    return PauseResultDTO{true, std::nullopt};
  }
  auto error = result.error();
  if (error.code == to_string(ErrorCode::kInvalidStateTransition)) {
    error = ErrorDTO{kErrorInvalidRequest,
                     "Pause is only valid during Executing",
                     {{"reason_code", "pause_not_executing"}, {"state", to_string(prev_state)}}};
  }
  return PauseResultDTO{false, error};
}

ResumeResultDTO ExecutionKernel::resume(std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  if (!control_command_epoch_matches(expected_epoch)) {
    return ResumeResultDTO{false, stale_control_command_error()};
  }
  auto prev_state = sm_.status().state;
  auto result = stop_policy_.request_resume();
  if (result.ok()) {
    authorize_gripper_authority();
    // Audit 6D: wake the paused worker immediately (it waits on
    // worker_signals_ keyed on resume intent instead of sleep-polling).
    worker_signals_.request_resume();
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kValidating;
    snapshot.error.reset();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    // Emit kResumeRequested event
    emit_event({KernelEventType::kResumeRequested,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                prev_state,
                sm_.status().state,
                std::nullopt,
                {}});
    (void)refresh_recovery_assessment_locked("resume_accepted");
    return ResumeResultDTO{true, std::nullopt};
  }
  return ResumeResultDTO{false, result.error()};
}

MotionInterruptStopCapabilityDTO ExecutionKernel::get_motion_interrupt_stop_capability() const {
  return execution_backend_.get_motion_interrupt_stop_capability();
}

ResetResultDTO ExecutionKernel::reset(std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  const auto reject_if_stale = [&]() -> std::optional<ResetResultDTO> {
    if (!control_command_epoch_matches(expected_epoch)) {
      return ResetResultDTO{false, stale_control_command_error()};
    }
    return std::nullopt;
  };
  if (auto stale = reject_if_stale()) {
    return *stale;
  }
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
      lifecycle_reset_requested_while_busy_.store(true, std::memory_order_release);
      if (execute_admission_interruptible_.load(std::memory_order_seq_cst)) {
        // The admission thread cannot consume the pending-reset flag until it
        // first publishes execute_admission_interruptible_=false. Seeing true
        // here therefore establishes the reset boundary before that thread can
        // process it, and makes immediate gripper invalidation race-free.
        revoke_runtime_collision_lookahead_clear_permits();
        invalidate_gripper_authority(GripperInvalidationReason::kReset);
        return ResetResultDTO{true, std::nullopt};
      }
    }
    lifecycle_lock.lock();
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
  }
  if (auto stale = reject_if_stale()) {
    return *stale;
  }
  revoke_runtime_collision_lookahead_clear_permits();
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
  lifecycle_stop_requested_event_emitted_.store(false, std::memory_order_seq_cst);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire)) {
    return ResetResultDTO{false, ErrorDTO{kErrorInvalidRequest,
                                          "Execution worker retirement is already in progress",
                                          {{"reason_code", "worker_retirement_in_progress"}}}};
  }
  // This reset intent is now admitted. Fence gripper callbacks and dispatch
  // before any cancellation, worker retirement, controlled-stop wait, or
  // backend reset can block. Later reset failure deliberately leaves admission
  // closed; only a successful reset/resume/new execute reauthorizes commands.
  invalidate_gripper_authority(GripperInvalidationReason::kReset);
  ExecutionState prev_state;
  {
    // Linearize the reset claim with the worker's post-plan classification.
    // If Reset wins, the worker observes the claim and performs no competing
    // state transition. If the worker wins, Reset observes its resulting state
    // and follows that state's normal reset path.
    std::lock_guard<std::mutex> planning_result_lock(planning_result_mutex_);
    prev_state = sm_.status().state;
    if (prev_state == ExecutionState::kPlanning) {
      planning_reset_requested_.store(true, std::memory_order_release);
    }
  }
  if (prev_state == ExecutionState::kPlanning) {
    request_active_planning_cancel();
  } else if (prev_state == ExecutionState::kValidating) {
    request_active_validation_cancel("reset");
  }

  request_fault_preempt_controlled_stop();
  fault_preempt_worker_.cancel(/*invalidate_generation=*/true);

  if (get_active_interrupt_latch()) {
    auto clear = interrupt_manager_.clear_active_latch("reset", clock_.now());
    if (clear.ok()) {
      record_terminal_fault_authority(clear.value());
      emit_interrupt_events(clear.value());
    } else {
      return ResetResultDTO{false, clear.error()};
    }
  }

  const auto clear_operator_stop_errors = [&]() { operator_stop_worker_.clear_errors(); };

  const auto retire_worker_if_quiescent = [&]() -> bool {
    switch (sm_.status().state) {
      using enum ExecutionState;
      case kIdle:
      case kCompleted:
      case kStopped:
      case kFailed:
      case kInterventionRequired:
      case kInterrupted:
        return retire_execute_worker(lifecycle_lock);
      default:
        break;
    }
    return true;
  };

  const auto enter_reset_intervention = [&](ErrorDTO error,
                                            ExecutionState from_state) -> ResetResultDTO {
    error = ensure_operator_intervention_details(std::move(error), "backend_reset", from_state,
                                                 sm_.status().execution_id, "");
    sm_.set_last_error(error_code_from_dto(error));
    commit_terminal(ExecutionState::kInterventionRequired, "terminal_commit");

    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = ExecutionState::kInterventionRequired;
    snapshot.error = error;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kErrorRaised,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                error,
                {}});
    if (from_state != ExecutionState::kInterventionRequired) {
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  from_state,
                  ExecutionState::kInterventionRequired,
                  std::nullopt,
                  {}});
    }
    return ResetResultDTO{false, error};
  };

  const auto publish_idle_after_reset = [&](ExecutionState from_state,
                                            bool emit_transition) -> ResetResultDTO {
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    clear_active_request_correlation();
    auto snapshot = execution_backend_.get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.execution_id = 0;
    snapshot.move_id.clear();
    snapshot.state = ExecutionState::kIdle;
    snapshot.progress = 0.0;
    snapshot.error.reset();
    publish_execution_snapshot(std::move(snapshot));
    clear_operator_stop_errors();
    lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
    lifecycle_stop_requested_event_emitted_.store(false, std::memory_order_seq_cst);
    lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
    if (emit_transition) {
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  from_state,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {}});
    }
    // Reset revokes every recovery boundary: cancel the freshness deadline,
    // reset the observation qualifier, and publish the none-assessment under
    // the lifecycle hold (generation fencing via the epoch bump above).
    if (recovery_freshness_deadline_handle_ != 0) {
      scheduler_.cancel(recovery_freshness_deadline_handle_);
      recovery_freshness_deadline_handle_ = 0;
      recovery_freshness_deadline_ns_ = 0;
    }
    recovery_readiness_tracker_.reset();
    (void)refresh_recovery_assessment_locked("reset_accepted");
    authorize_gripper_authority();
    return ResetResultDTO{true, std::nullopt};
  };

  const auto reset_backend_and_state = [&](ExecutionState from_state,
                                           bool emit_transition) -> ResetResultDTO {
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    if (!retire_worker_if_quiescent()) {
      return ResetResultDTO{false,
                            ErrorDTO{kErrorInvalidRequest,
                                     "Execution worker cannot be retired from its own callback",
                                     {{"reason_code", "worker_retirement_self_thread"}}}};
    }
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return enter_reset_intervention(
          backend_reset.error.value_or(operator_intervention_error(
              ErrorCode::kResetFailed, "Execution backend reset failed", "backend_reset",
              from_state, sm_.status().execution_id, "")),
          from_state);
    }

    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    if (sm_.status().state != ExecutionState::kIdle) {
      auto result = stop_policy_.request_reset();
      if (!result.ok()) {
        return ResetResultDTO{false, result.error()};
      }
    }
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    sm_.set_execution_id(0);
    return publish_idle_after_reset(from_state, emit_transition);
  };

  if (prev_state == ExecutionState::kIdle) {
    if (!retire_worker_if_quiescent()) {
      return ResetResultDTO{false,
                            ErrorDTO{kErrorInvalidRequest,
                                     "Execution worker cannot be retired from its own callback",
                                     {{"reason_code", "worker_retirement_self_thread"}}}};
    }
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    auto backend_reset = execution_backend_.reset();
    if (!backend_reset.accepted) {
      return backend_reset;
    }
    if (auto stale = reject_if_stale()) {
      return *stale;
    }
    return publish_idle_after_reset(prev_state, /*emit_transition=*/false);
  }

  worker_signals_.clear_resume_request();

  const auto operator_stop_timeout = [&]() {
    if (config_.operator_stop_profile) {
      return effective_controlled_stop_deadline_duration(
          *config_.operator_stop_profile, config_.complete_move_on_signal_timeout_ns);
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1));
  };

  const auto hard_stop_before_reset = [&](ExecutionState from_state,
                                          std::string reason) -> std::optional<ErrorDTO> {
    auto result = stop_policy_.request_hard_stop();
    if (!result.ok()) {
      return result.error();
    }
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown);
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    emit_event({KernelEventType::kStopIssued,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {}});
    emit_event({KernelEventType::kStateTransition,
                sm_.status().execution_id,
                clock_.now(),
                from_state,
                sm_.status().state,
                std::nullopt,
                {{"reason", reason}}});
    std::lock_guard<std::mutex> metrics_lock(metrics_mutex_);
    metrics_.stops_issued++;
    return std::nullopt;
  };

  const auto wait_for_selected_stop = [&](ExecutionState from_state) -> std::optional<ErrorDTO> {
    lifecycle_reset_waiting_for_stop_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    const bool stopped = wait_for_operator_stop_worker(operator_stop_timeout());
    lifecycle_lock.lock();
    lifecycle_reset_waiting_for_stop_.store(false, std::memory_order_release);
    join_finished_operator_stop_worker();

    if (!stopped) {
      auto hard_stop_error =
          hard_stop_before_reset(from_state, "reset_operator_stop_timeout_hard_stop");
      cancel_operator_stop_worker(/*invalidate_generation=*/true);
      lifecycle_reset_waiting_for_stop_.store(true, std::memory_order_release);
      lifecycle_lock.unlock();
      const bool drained = wait_for_operator_stop_worker(std::chrono::milliseconds(100));
      lifecycle_lock.lock();
      lifecycle_reset_waiting_for_stop_.store(false, std::memory_order_release);
      join_finished_operator_stop_worker();
      if (!drained) {
        return ErrorDTO{kErrorStopFailed,
                        "Operator stop worker did not quiesce after reset hard-stop "
                        "escalation",
                        {{"reason", "operator_stop_worker_stuck_after_hard_stop"}}};
      }
      return hard_stop_error;
    }
    if (sm_.status().state == ExecutionState::kInterventionRequired) {
      // Reset is an explicit teardown/recovery command. If the operator-stop
      // worker reaches InterventionRequired while reset is already waiting on
      // it, preserve the worker's emitted diagnostics but let reset proceed to
      // the backend reset gate. Otherwise a race between reset's wait timeout
      // and the worker's own stop-failure terminal can strand the operator in
      // InterventionRequired even though Reset was requested.
      return std::nullopt;
    }
    if (sm_.status().state != ExecutionState::kStopped) {
      return hard_stop_before_reset(from_state, "reset_operator_stop_unexpected_state_hard_stop");
    }
    return std::nullopt;
  };

  if ((prev_state == ExecutionState::kExecuting || prev_state == ExecutionState::kPaused) &&
      config_.operator_stop_profile && operator_controlled_stop_allowed(prev_state)) {
    emit_event({KernelEventType::kStopRequested,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "reset_stop_before_reset"}}});
    const bool transitioned_to_stopping = sm_.try_transition(ExecutionState::kStopping).ok();
    if (transitioned_to_stopping) {
      emit_event({KernelEventType::kStateTransition,
                  sm_.status().execution_id,
                  clock_.now(),
                  prev_state,
                  ExecutionState::kStopping,
                  std::nullopt,
                  {{"reason", "reset_stop_before_reset"}}});
    }
    auto snapshot = get_execution_snapshot();
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = sm_.status().state;
    publish_execution_snapshot(std::move(snapshot));
    const auto runtime_collision_generation = start_collision_monitoring(
        "reset_operator_stop", RuntimeCollisionProtectionPurpose::kControlledStop);
    const auto stop_owner_generation =
        begin_operator_stop_owner_generation(sm_.status().execution_id);
    if (start_operator_stop_worker(
            *config_.operator_stop_profile,
            transitioned_to_stopping ? ExecutionState::kStopping : prev_state,
            runtime_collision_generation, stop_owner_generation)) {
      if (auto error = wait_for_selected_stop(transitioned_to_stopping ? ExecutionState::kStopping
                                                                       : prev_state)) {
        return enter_reset_intervention(*error, prev_state);
      }
    } else {
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                runtime_collision_generation);
      auto hard_stop_error =
          hard_stop_before_reset(transitioned_to_stopping ? ExecutionState::kStopping : prev_state,
                                 "reset_operator_stop_worker_unavailable_hard_stop");
      if (operator_stop_worker_.active()) {
        return enter_reset_intervention(
            ErrorDTO{kErrorStopFailed,
                     "Operator stop worker remained active after reset hard-stop "
                     "fallback",
                     {{"reason", "operator_stop_worker_active_after_hard_stop"}}},
            prev_state);
      }
      if (hard_stop_error) {
        return enter_reset_intervention(*hard_stop_error, prev_state);
      }
    }
  } else if (prev_state == ExecutionState::kStopping && operator_stop_worker_.active()) {
    if (auto error = wait_for_selected_stop(ExecutionState::kStopping)) {
      return enter_reset_intervention(*error, prev_state);
    }
  } else if (prev_state == ExecutionState::kExecuting || prev_state == ExecutionState::kPaused ||
             prev_state == ExecutionState::kStopping) {
    emit_event({KernelEventType::kStopRequested,
                sm_.status().execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "reset_hard_stop_before_reset"}}});
    if (auto error = hard_stop_before_reset(
            prev_state == ExecutionState::kStopping ? ExecutionState::kStopping : prev_state,
            "reset_hard_stop_before_reset")) {
      return enter_reset_intervention(*error, prev_state);
    }
  }

  const auto reset_from_state = sm_.status().state;
  return reset_backend_and_state(reset_from_state, /*emit_transition=*/true);
}

ResetResultDTO ExecutionKernel::reset_after_certified_containment(
    const std::uint64_t execution_id, std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (!control_command_epoch_matches(expected_epoch)) {
    return ResetResultDTO{false, stale_control_command_error()};
  }
  const auto before = sm_.status();
  if (execution_id == 0U || before.execution_id != execution_id ||
      before.state == ExecutionState::kIdle) {
    return ResetResultDTO{
        false, ErrorDTO{kErrorInvalidRequest,
                        "Certified-containment reset names a stale subordinate execution",
                        {{"reason_code", "certified_containment_execution_mismatch"},
                         {"current_execution_id", std::to_string(before.execution_id)},
                         {"requested_execution_id", std::to_string(execution_id)}}}};
  }
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire)) {
    return ResetResultDTO{false, ErrorDTO{kErrorInvalidRequest,
                                          "Execution worker retirement is already in progress",
                                          {{"reason_code", "worker_retirement_in_progress"}}}};
  }

  // The addressed owner has already certified containment. This path only
  // retires local execution machinery and resets the backend; it must never
  // issue another Stop command or wait for another Stop policy decision.
  revoke_runtime_collision_lookahead_clear_permits();
  invalidate_gripper_authority(GripperInvalidationReason::kReset);
  planning_reset_requested_.store(true, std::memory_order_release);
  request_active_planning_cancel();
  request_active_validation_cancel("certified_containment_reset");
  stop_policy_.cancel_pending_callbacks();
  request_fault_preempt_controlled_stop();
  fault_preempt_worker_.stop_and_join();
  operator_stop_worker_.stop_and_join();
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown);
  if (!retire_execute_worker(lifecycle_lock)) {
    return ResetResultDTO{false, ErrorDTO{kErrorInvalidRequest,
                                          "Execution worker cannot retire from its own callback",
                                          {{"reason_code", "worker_retirement_self_thread"}}}};
  }
  if (!control_command_epoch_matches(expected_epoch)) {
    return ResetResultDTO{false, stale_control_command_error()};
  }
  auto backend_reset = execution_backend_.reset();
  if (!backend_reset.accepted) {
    return backend_reset;
  }
  if (sm_.status().state != ExecutionState::kIdle) {
    auto reset_state = stop_policy_.request_reset();
    if (!reset_state.ok()) {
      return ResetResultDTO{false, reset_state.error()};
    }
  }
  sm_.set_execution_id(0U);
  clear_active_request_correlation();
  auto snapshot = execution_backend_.get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0U;
  snapshot.move_id.clear();
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  snapshot.error.reset();
  publish_execution_snapshot(std::move(snapshot));
  emit_event({KernelEventType::kStateTransition,
              execution_id,
              clock_.now(),
              before.state,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "reset_after_certified_containment"}}});
  planning_reset_requested_.store(false, std::memory_order_release);
  lifecycle_stop_requested_while_busy_.store(false, std::memory_order_seq_cst);
  lifecycle_stop_requested_event_emitted_.store(false, std::memory_order_seq_cst);
  lifecycle_reset_requested_while_busy_.store(false, std::memory_order_release);
  operator_stop_worker_.clear_errors();
  authorize_gripper_authority();
  return ResetResultDTO{true, std::nullopt};
}

AcknowledgeCompletedResultDTO ExecutionKernel::acknowledge_completed(
    uint64_t execution_id, std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

  if (!control_command_epoch_matches(expected_epoch)) {
    return AcknowledgeCompletedResultDTO{false, stale_control_command_error()};
  }
  const auto status = sm_.status();
  if (status.state != ExecutionState::kCompleted) {
    return AcknowledgeCompletedResultDTO{
        false, ErrorDTO{kErrorInvalidRequest,
                        "Can only acknowledge a Completed execution",
                        {{"reason_code", "wrong_state"}, {"state", to_string(status.state)}}}};
  }
  if (status.execution_id != execution_id) {
    return AcknowledgeCompletedResultDTO{
        false, ErrorDTO{kErrorInvalidRequest,
                        "Execution acknowledgement id mismatch",
                        {{"reason_code", "mismatched_execution_id"},
                         {"current_execution_id", std::to_string(status.execution_id)},
                         {"requested_execution_id", std::to_string(execution_id)}}}};
  }
  if (completion_publication_execution_id_.load(std::memory_order_acquire) == execution_id) {
    return AcknowledgeCompletedResultDTO{
        false, ErrorDTO{kErrorExecutionRejected,
                        "Completed execution evidence is still being published",
                        {{"reason_code", reason_codes::kTerminalPublicationInProgress},
                         {"retryable", "true"},
                         {"execution_id", std::to_string(execution_id)}}}};
  }

  auto result = sm_.reset();
  if (!result.ok()) {
    return AcknowledgeCompletedResultDTO{false, result.error()};
  }
  ExecutionSnapshot last_completed_snapshot = snapshot_publisher_.latest();
  sm_.set_execution_id(0);
  active_correlation_.clear_move_id();

  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  snapshot.joint_positions_rad = std::move(last_completed_snapshot.joint_positions_rad);
  publish_execution_snapshot(std::move(snapshot));

  emit_event({KernelEventType::kCompletedAcknowledged,
              execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"acknowledged_execution_id", std::to_string(execution_id)}}});
  emit_event({KernelEventType::kStateTransition,
              execution_id,
              clock_.now(),
              ExecutionState::kCompleted,
              ExecutionState::kIdle,
              std::nullopt,
              {}});
  {
    // Cleared AFTER the acknowledgement events: they are the closing
    // bracket of this execution's story and must still carry its
    // request-correlation stamp.
    active_correlation_.clear_program_run_and_action_span();
  }
  { runtime_collision_monitor_.clear_active_contact_policy_id(); }

  return AcknowledgeCompletedResultDTO{true, std::nullopt};
}

Result<void> ExecutionKernel::apply_acknowledge_and_continue_locked(const std::string& principal_id,
                                                                    const std::string& reason) {
  const auto status = sm_.status();
  if (principal_id.empty()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "acknowledge_and_continue requires principal_id",
                                      {{"state", to_string(status.state)}}});
  }
  if (status.state != ExecutionState::kInterventionRequired) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "acknowledge_and_continue is valid only from "
                                      "InterventionRequired",
                                      {{"state", to_string(status.state)}}});
  }

  // apply_recovery_action_impl is the sole caller. Its coherent assessment
  // transaction has already classified the intervention and checked the
  // qualified observed-state source. Do not re-read the backend here: that
  // would create a second authority and could block while lifecycle_mutex_ is
  // held. This helper owns mutation only.
  auto snapshot = snapshot_publisher_.latest();

  if (interrupt_manager_.active_latch()) {
    auto clear =
        interrupt_manager_.clear_active_latch("intervention_retry_acknowledged", clock_.now());
    if (clear.ok()) {
      record_terminal_fault_authority(clear.value());
      emit_interrupt_events(clear.value());
    } else {
      return Result<void>::Err(clear.error());
    }
    if (interrupt_manager_.active_latch()) {
      return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                        "Interrupt acknowledgement replayed another active latch; "
                                        "retry is not yet admissible",
                                        {{"reason", "intervention_retry_replayed_latch"}}});
    }
  }

  // Clear/replay is decided while the kernel is still held in
  // InterventionRequired. Resetting the stop-policy state machine first would
  // expose false Idle if the disposition revealed a queued latch.
  auto reset_result = stop_policy_.request_reset();
  if (!reset_result.ok()) {
    return Result<void>::Err(reset_result.error());
  }

  const auto prev_state = status.state;
  const auto prev_execution_id = status.execution_id;
  sm_.set_execution_id(0);
  active_correlation_.clear_move_id();

  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = 0;
  snapshot.move_id.clear();
  snapshot.state = ExecutionState::kIdle;
  snapshot.progress = 0.0;
  snapshot.error.reset();
  publish_execution_snapshot(std::move(snapshot));
  {
    operator_stop_worker_.clear_errors();
    clear_fault_preempt_intervention_error();
    clear_fault_preempt_stop_path_collision();
  }

  emit_event({KernelEventType::kInterventionAcknowledged,
              prev_execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"principal_id", principal_id},
               {"reason", reason},
               {"prev_state", to_string(prev_state)},
               {"recovery_mode", "acknowledge_and_continue"},
               {"backend_reset_performed", "false"}}});
  emit_event({KernelEventType::kStateTransition,
              prev_execution_id,
              clock_.now(),
              prev_state,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "intervention_acknowledged_for_retry"}}});

  {
    // Cleared AFTER the acknowledgement/recovery boundary events: they are
    // part of this run's incident story and must carry request correlation.
    active_correlation_.clear_program_run_and_action_span();
  }
  { runtime_collision_monitor_.clear_active_contact_policy_id(); }
  authorize_gripper_authority();
  return Result<void>::Ok();
}

namespace {

bool is_retryable_failed_validation_error(const ErrorDTO& error) {
  if (error.code == kErrorSceneChangedDuringValidation || error.code == kErrorValidationCollision ||
      error.code == kErrorContactPolicyViolation) {
    return true;
  }
  const auto reason_it = error.details.find("reason");
  const std::string reason = reason_it != error.details.end() ? reason_it->second : std::string{};
  const auto reason_code_it = error.details.find("reason_code");
  const std::string reason_code =
      reason_code_it != error.details.end() ? reason_code_it->second : std::string{};
  return reason == "validation_failed" || reason_code == "validation_failed";
}

}  // namespace

Result<void> ExecutionKernel::apply_retry_after_scene_repair_locked(const std::string& principal_id,
                                                                    const std::string& reason) {
  const auto status = sm_.status();
  if (principal_id.empty()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "retry_after_scene_repair requires principal_id",
                                      {{"state", to_string(status.state)}}});
  }
  if (status.state != ExecutionState::kFailed) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "retry_after_scene_repair is valid only from Failed",
                                      {{"state", to_string(status.state)}}});
  }

  // The assessment transaction already classified the published failure as a
  // retryable validation boundary. Consume that immutable publication rather
  // than issuing a backend RPC under lifecycle_mutex_.
  const auto failed_snapshot = snapshot_publisher_.latest();
  if (!failed_snapshot.error.has_value()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "Cannot retry after scene repair: missing classified "
                                      "validation error",
                                      {{"reason", "failed_validation_error_missing"}}});
  }
  if (!is_retryable_failed_validation_error(*failed_snapshot.error)) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "Cannot retry after scene repair: failure is not a retryable "
                                      "pre-dispatch validation boundary",
                                      {{"reason", "failed_validation_not_retryable"},
                                       {"error_code", failed_snapshot.error->code}}});
  }

  auto backend_snapshot = failed_snapshot;

  auto reset_result = stop_policy_.request_reset();
  if (!reset_result.ok()) {
    return Result<void>::Err(reset_result.error());
  }

  const auto prev_execution_id = status.execution_id;
  sm_.set_execution_id(0);
  active_correlation_.clear_move_id();

  backend_snapshot.timestamp_ns = clock_.now();
  backend_snapshot.execution_id = 0;
  backend_snapshot.move_id.clear();
  backend_snapshot.state = ExecutionState::kIdle;
  backend_snapshot.progress = 0.0;
  backend_snapshot.error.reset();
  publish_execution_snapshot(std::move(backend_snapshot));

  emit_event({KernelEventType::kFailedValidationAcknowledged,
              prev_execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"principal_id", principal_id},
               {"reason", reason},
               {"prev_state", to_string(status.state)},
               {"error_code", failed_snapshot.error->code}}});
  emit_event({KernelEventType::kStateTransition,
              prev_execution_id,
              clock_.now(),
              ExecutionState::kFailed,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "failed_validation_retry_acknowledged"}}});

  { active_correlation_.clear_program_run_and_action_span(); }
  { runtime_collision_monitor_.clear_active_contact_policy_id(); }
  authorize_gripper_authority();
  return Result<void>::Ok();
}

KernelRecoveryAssessmentDTO ExecutionKernel::get_recovery_assessment() const {
  std::lock_guard<std::mutex> lock(recovery_assessment_mutex_);
  return recovery_assessment_;
}

std::vector<KernelStopCertificateDTO> ExecutionKernel::stop_certificate_evidence() const {
  std::lock_guard<std::mutex> lock(recovery_certificate_mutex_);
  return {recovery_certificates_.evidence_ring().begin(),
          recovery_certificates_.evidence_ring().end()};
}

bool ExecutionKernel::publish_stop_certificate(KernelStopCertificateDTO certificate,
                                               bool coalesce_owner_session) {
  apply_observed_state_to_stop_certificate(
      execution_backend_.observed_robot_state_publication(),
      certificate.completed_at_ns > 0 ? certificate.completed_at_ns : clock_.now(),
      config_.recovery_at_rest_velocity_threshold_rad_s, certificate);
  if (certificate.certificate_id.empty()) {
    certificate.certificate_id = std::string("stopcert-") +
                                 std::string(stop_owner_kind_name(certificate.stop_owner)) + "-g" +
                                 std::to_string(certificate.stop_owner_generation) + "-x" +
                                 std::to_string(certificate.execution_id) + "-o" +
                                 stop_certificate_outcome_name(certificate.completion_result) +
                                 "-t" + std::to_string(certificate.completed_at_ns);
  }
  {
    std::lock_guard<std::mutex> lock(recovery_certificate_mutex_);
    if (coalesce_owner_session) {
      const auto revision = recovery_certificates_.publish_owner_session_progress(certificate);
      if (!revision.has_value()) {
        return false;
      }
      certificate.certificate_revision = *revision;
    } else {
      certificate.certificate_revision = recovery_certificates_.publish(certificate);
    }
  }
  KernelEvent event;
  event.type = KernelEventType::kStopCertificatePublished;
  event.execution_id = certificate.execution_id;
  event.timestamp_ns = certificate.completed_at_ns;
  event.details = {
      {"certificate_id", certificate.certificate_id},
      {"certificate_revision", std::to_string(certificate.certificate_revision)},
      {"completion_result", stop_certificate_outcome_name(certificate.completion_result)},
      {"stop_owner", stop_owner_kind_name(certificate.stop_owner)},
      {"stop_owner_generation", std::to_string(certificate.stop_owner_generation)},
      {"latch_id", certificate.latch_id},
      {"command_source", certificate.command_source},
      {"backend_stop_confirmed", certificate.backend_stop_confirmed ? "true" : "false"},
      {"observed_state_fresh", certificate.observed_state_fresh ? "true" : "false"},
      {"observed_state_at_rest", certificate.observed_state_at_rest ? "true" : "false"},
      {"at_rest_evidence", at_rest_evidence_name(certificate.at_rest_evidence)},
      {"reason_code", "stop_certificate_published"},
  };
  // Evidence after the authority fact is durable (act-before-emit).
  emit_event(std::move(event));
  return true;
}

std::uint64_t ExecutionKernel::begin_operator_stop_owner_generation(std::uint64_t execution_id) {
  std::lock_guard<std::mutex> lock(operator_stop_identity_mutex_);
  return operator_stop_identity_.begin(execution_id);
}

std::uint64_t ExecutionKernel::ensure_operator_stop_owner_generation(std::uint64_t execution_id) {
  std::lock_guard<std::mutex> lock(operator_stop_identity_mutex_);
  return operator_stop_identity_.ensure(execution_id);
}

std::uint64_t ExecutionKernel::operator_stop_owner_generation_for_execution(
    std::uint64_t execution_id) const {
  std::lock_guard<std::mutex> lock(operator_stop_identity_mutex_);
  return operator_stop_identity_.generation_for_execution(execution_id);
}

void ExecutionKernel::publish_operator_stop_certificate(std::uint64_t execution_id,
                                                        StopCertificateOutcome completion_result,
                                                        bool backend_stop_confirmed,
                                                        std::string command_source,
                                                        std::string requested_stop_mode,
                                                        std::string executed_stop_mode) {
  KernelStopCertificateDTO certificate;
  certificate.completion_result = completion_result;
  certificate.stop_owner = StopOwnerKind::kOperatorStopWorker;
  certificate.stop_owner_generation = ensure_operator_stop_owner_generation(execution_id);
  certificate.execution_id = execution_id;
  certificate.control_command_epoch = control_command_epoch();
  certificate.requested_stop_mode = std::move(requested_stop_mode);
  certificate.executed_stop_mode = std::move(executed_stop_mode);
  certificate.command_source = std::move(command_source);
  certificate.preemption_result = InterruptPreemptionResult::kNoneRequired;
  certificate.backend_stop_confirmed = backend_stop_confirmed;
  certificate.at_rest_evidence = AtRestEvidence::kUnavailable;
  certificate.completed_at_ns = clock_.now();
  (void)publish_stop_certificate(std::move(certificate),
                                 /*coalesce_owner_session=*/true);
}

RecoveryReadinessPolicy ExecutionKernel::recovery_readiness_policy() const {
  RecoveryReadinessPolicy policy;
  policy.require_measured_observation = config_.recovery_require_measured_observation;
  policy.ready_dwell_ns = config_.recovery_ready_dwell_ns;
  policy.ready_min_samples = config_.recovery_ready_min_samples;
  return policy;
}

RecoveryAuthorityCapture ExecutionKernel::capture_recovery_authority_locked() {
  // Caller holds lifecycle_mutex_: every fact below is read once, in the
  // documented lock order, and cannot be replaced by a concurrent lifecycle
  // command while captured (spec/230 §3.7.1 coherent capture).
  RecoveryAuthorityCapture capture;
  capture.control_command_epoch = control_command_epoch();
  const auto status = sm_.status();
  capture.execution_id = status.execution_id;
  capture.execution_state = status.state;
  capture.active_latch = interrupt_manager_.active_latch();
  capture.queued_latch_present = interrupt_manager_.queued_count() != 0;
  capture.fault_preempt_stop_active = fault_preempt_stop_active();
  capture.fault_preempt_worker_active = fault_preempt_worker_.active();
  capture.operator_stop_active = operator_stop_command_active_.load(std::memory_order_acquire);
  capture.motion_trip_stop_active = motion_trip_stop_.active();
  capture.fault_preempt_owner_generation = fault_preempt_stop_generation();
  capture.operator_stop_owner_generation =
      operator_stop_owner_generation_for_execution(capture.execution_id);
  capture.stronger_authority_active =
      lifecycle_stop_requested_while_busy_.load(std::memory_order_acquire) ||
      lifecycle_reset_requested_while_busy_.load(std::memory_order_acquire) ||
      lifecycle_reset_waiting_for_stop_.load(std::memory_order_acquire) ||
      lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(recovery_certificate_mutex_);
    capture.fault_preempt_certificate =
        recovery_certificates_.authoritative(StopOwnerKind::kFaultPreemptWorker);
    capture.operator_stop_certificate =
        recovery_certificates_.authoritative(StopOwnerKind::kOperatorStopWorker);
  }
  if (status.state == ExecutionState::kInterventionRequired) {
    std::optional<ErrorDTO> intervention_error = operator_stop_worker_.intervention_error();
    if (!intervention_error.has_value()) {
      intervention_error = fault_preempt_intervention_error_for_active_latch();
    }
    if (!intervention_error.has_value()) {
      intervention_error = snapshot_publisher_.latest().error;
    }
    capture.stop_fault_intervention_classified =
        intervention_error.has_value() &&
        is_classified_stop_intervention(intervention_error->details);
  }
  if (status.state == ExecutionState::kFailed) {
    const auto snapshot = snapshot_publisher_.latest();
    capture.failed_validation_retryable =
        snapshot.error.has_value() && is_retryable_failed_validation_error(*snapshot.error);
    if (snapshot.error.has_value()) {
      auto it = snapshot.error->details.find("reason");
      if (it != snapshot.error->details.end()) {
        capture.failed_validation_reason = it->second;
      }
    }
  }
  const auto observed_sample = capture_observed_state_evidence(
      execution_backend_.observed_robot_state_publication(), clock_.now(),
      config_.recovery_at_rest_velocity_threshold_rad_s);
  if (observed_sample.has_value()) {
    capture.observed_state = *observed_sample;
    capture.observed_state_qualified = recovery_readiness_tracker_.update(
        *observed_sample, config_.recovery_observed_joint_order_token, recovery_readiness_policy(),
        clock_.now());
  } else {
    recovery_readiness_tracker_.reset();
  }
  capture.captured_at_ns = clock_.now();
  return capture;
}

KernelRecoveryAssessmentDTO ExecutionKernel::refresh_recovery_assessment_locked(
    const char* trigger) {
  recovery_assessment_dirty_.store(false, std::memory_order_release);
  return publish_recovery_assessment_locked(capture_recovery_authority_locked(), trigger);
}

KernelRecoveryAssessmentDTO ExecutionKernel::publish_terminal_recovery_assessment_locked(
    ExecutionState terminal_state, const char* trigger) {
  auto capture = capture_recovery_authority_locked();
  capture.execution_state = terminal_state;
  return publish_recovery_assessment_locked(std::move(capture), trigger);
}

KernelRecoveryAssessmentDTO ExecutionKernel::publish_recovery_assessment_locked(
    RecoveryAuthorityCapture capture, const char* trigger) {
  KernelRecoveryAssessmentDTO previous;
  {
    std::lock_guard<std::mutex> lock(recovery_assessment_mutex_);
    previous = recovery_assessment_;
  }
  auto publication = prepare_recovery_assessment_publication(
      previous, capture, recovery_readiness_policy(),
      trigger != nullptr ? std::string_view(trigger) : std::string_view{});
  auto& next = publication.candidate;
  if (!publication.changed) {
    // Equally admissible facts do not churn the published revision
    // (spec/210 §3.12 offer stability), but a newer equivalent observation
    // extends its own freshness deadline. Keep the expiry schedule current
    // without changing the operator-visible assessment revision.
    schedule_recovery_freshness_deadline_locked(next);
    schedule_recovery_observation_poll_locked(next);
    return previous;
  }
  {
    std::lock_guard<std::mutex> lock(recovery_assessment_mutex_);
    recovery_assessment_ = next;
  }
  schedule_recovery_freshness_deadline_locked(next);
  schedule_recovery_observation_poll_locked(next);
  KernelEvent event;
  event.type = KernelEventType::kRecoveryAssessmentChanged;
  event.execution_id = next.execution_id;
  event.timestamp_ns = clock_.now();
  event.details = std::move(publication.event_details);
  emit_event(std::move(event));
  return next;
}

void ExecutionKernel::try_refresh_recovery_assessment(const char* trigger) {
  // ONLY callable from contexts that provably never own lifecycle_mutex_
  // (collision-tick ingest tail, stop-worker lambdas): std::mutex try_lock
  // by the owning thread is UB.
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    recovery_assessment_dirty_.store(true, std::memory_order_release);
    schedule_recovery_assessment_refresh(trigger);
    return;
  }
  EventDeferralScope event_scope(*this);
  (void)refresh_recovery_assessment_locked(trigger);
}

void ExecutionKernel::schedule_recovery_assessment_refresh(const char* trigger) noexcept {
  std::lock_guard<std::mutex> refresh_lock(recovery_refresh_mutex_);
  if (recovery_refresh_scheduled_) {
    return;
  }
  recovery_refresh_scheduled_ = true;
  try {
    recovery_refresh_handle_ = scheduler_.schedule(
        clock_.now(),
        [this, trigger]() {
          {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
            EventDeferralScope event_scope(*this);
            (void)refresh_recovery_assessment_locked(trigger);
          }
          {
            std::lock_guard<std::mutex> lock(recovery_refresh_mutex_);
            recovery_refresh_handle_ = 0;
            recovery_refresh_scheduled_ = false;
          }
          // A terminal commit can mark dirty while this callback is between
          // capture and clearing the scheduled flag. Recheck after clearing;
          // a concurrent later mark either schedules itself or is seen here.
          if (recovery_assessment_dirty_.load(std::memory_order_acquire)) {
            schedule_recovery_assessment_refresh("coalesced_recovery_authority_change");
          }
        },
        SchedulerPriority::Monitoring);
  } catch (...) {
    recovery_refresh_scheduled_ = false;
    recovery_refresh_handle_ = 0;
    // The final action path rebuilds under lifecycle authority and remains
    // fail-closed. A telemetry refresh allocation failure must not escape a
    // terminal commit after the physical stop has already succeeded.
  }
}

void ExecutionKernel::schedule_recovery_freshness_deadline_locked(
    const KernelRecoveryAssessmentDTO& assessment) {
  const bool observation_dependent =
      assessment.recovery_class == RecoveryClass::kRuntimeCollision ||
      assessment.recovery_class == RecoveryClass::kStopFaultIntervention;
  const bool actionable = assessment.status == RecoveryAssessmentStatus::kRetryReady ||
                          assessment.status == RecoveryAssessmentStatus::kActionRequired;
  if (recovery_freshness_deadline_handle_ != 0) {
    if (observation_dependent && actionable && assessment.observed_state_valid_until_ns > 0 &&
        recovery_freshness_deadline_ns_ == assessment.observed_state_valid_until_ns) {
      return;
    }
    scheduler_.cancel(recovery_freshness_deadline_handle_);
    recovery_freshness_deadline_handle_ = 0;
    recovery_freshness_deadline_ns_ = 0;
  }
  if (!observation_dependent || !actionable || !assessment.observed_state_available ||
      !assessment.observed_state_fresh || !config_.recovery_require_measured_observation) {
    return;
  }
  const std::int64_t deadline_ns = assessment.observed_state_valid_until_ns;
  if (deadline_ns <= 0) {
    return;
  }
  // Kernel-owned expiry: publishes the non-ready revision even when no new
  // sample or observer read arrives. Monitoring role; the callback briefly
  // takes lifecycle_mutex_ (documented allowance, bounded).
  recovery_freshness_deadline_handle_ = scheduler_.schedule(
      deadline_ns,
      [this]() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        EventDeferralScope event_scope(*this);
        (void)refresh_recovery_assessment_locked("observed_state_freshness_deadline");
      },
      SchedulerPriority::Monitoring);
  recovery_freshness_deadline_ns_ = deadline_ns;
}

void ExecutionKernel::schedule_recovery_observation_poll_locked(
    const KernelRecoveryAssessmentDTO& assessment) {
  const bool observation_dependent =
      assessment.recovery_class == RecoveryClass::kRuntimeCollision ||
      assessment.recovery_class == RecoveryClass::kStopFaultIntervention;
  if (!observation_dependent) {
    if (recovery_observation_poll_handle_ != 0) {
      scheduler_.cancel(recovery_observation_poll_handle_);
      recovery_observation_poll_handle_ = 0;
    }
    return;
  }
  if (recovery_observation_poll_handle_ != 0) {
    return;
  }
  recovery_observation_poll_handle_ = scheduler_.schedule(
      clock_.now() + config_.recovery_observation_poll_period_ns,
      [this]() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        recovery_observation_poll_handle_ = 0;
        EventDeferralScope event_scope(*this);
        (void)refresh_recovery_assessment_locked("observed_state_publication_poll");
      },
      SchedulerPriority::Monitoring);
}

KernelRecoveryActionResultDTO ExecutionKernel::apply_recovery_action(
    const KernelRecoveryActionRequestDTO& request) {
  return apply_recovery_action_impl(request);
}

KernelRecoveryActionResultDTO ExecutionKernel::apply_recovery_action_impl(
    KernelRecoveryActionRequestDTO request) {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  RecoveryAuthorityCapture capture = capture_recovery_authority_locked();
  auto assessment = publish_recovery_assessment_locked(capture, "recovery_action_admission");
  const auto reject_with_assessment =
      [&](const KernelRecoveryAssessmentDTO& rejected_assessment,
          std::string_view reason) -> KernelRecoveryActionResultDTO {
    // The authority decision is complete and no mutation follows a rejection.
    // Release lifecycle authority before allocating the structured error/event
    // payload; EventDeferralScope still preserves post-lock dispatch ordering.
    if (lifecycle_lock.owns_lock()) {
      lifecycle_lock.unlock();
    }
    auto error = recovery_action_rejection_error(request, rejected_assessment, reason);
    KernelEvent event;
    event.type = KernelEventType::kRecoveryActionRejected;
    event.execution_id = rejected_assessment.execution_id;
    event.timestamp_ns = clock_.now();
    event.error = error;
    event.details = error.details;
    event.details["request_id"] = request.request_id;
    event.details["principal_id"] = request.principal_id;
    emit_event(std::move(event));
    return KernelRecoveryActionResultDTO{request.request_id, request.action, false,
                                         rejected_assessment, std::move(error)};
  };
  const auto reject = [&](std::string_view reason) {
    return reject_with_assessment(assessment, reason);
  };

  const auto decision = evaluate_recovery_admission(request, capture, assessment);
  if (!decision.admitted) {
    return reject(decision.rejection_reason);
  }

  std::optional<ExecutionState> accepted_from_state;

  switch (request.action) {
    case RecoveryAction::kRetryMotionFromCurrentState: {
      // Transplanted RT-8P0 accept path: certificate admission already
      // proven above; clear the latch and publish Idle without any backend
      // reset or motion command.
      InterruptDisposition disposition;
      disposition.latch_id = assessment.latch_id;
      disposition.action = InterruptDispositionAction::kAcknowledge;
      disposition.principal_id = request.principal_id;
      disposition.reason = request.reason;
      auto disposition_result = interrupt_manager_.apply_disposition(disposition, clock_.now());
      if (!disposition_result.ok()) {
        return reject(disposition_result.error().details.count("reason")
                          ? disposition_result.error().details.at("reason")
                          : std::string("interrupt_disposition_failed"));
      }
      stamp_interrupt_latch_collision_world_revision(disposition_result.value());
      apply_interrupt_ingest_effects(disposition_result.value(),
                                     /*apply_preemption_inline=*/true);
      if (disposition_result.value().rejected) {
        return reject(disposition_result.value().reason.empty()
                          ? std::string("interrupt_disposition_rejected")
                          : disposition_result.value().reason);
      }
      if (interrupt_manager_.active_latch()) {
        // Queued-latch replay: transactional rejection. The replayed latch
        // is now authoritative; no Idle is published and the refreshed
        // assessment carries the new latch identity.
        auto replay_assessment = refresh_recovery_assessment_locked("queued_latch_replayed");
        return reject_with_assessment(replay_assessment,
                                      reason_codes::kRecoveryQueuedLatchReplayed);
      }
      auto reset_result = stop_policy_.request_reset();
      if (!reset_result.ok()) {
        return reject(std::string("stop_policy_reset_rejected"));
      }
      const auto prev_execution_id = capture.execution_id;
      sm_.set_execution_id(0);
      active_correlation_.clear_move_id();
      auto snapshot = snapshot_publisher_.latest();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = 0;
      snapshot.move_id.clear();
      snapshot.state = ExecutionState::kIdle;
      snapshot.progress = 0.0;
      snapshot.error.reset();
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition,
                  prev_execution_id,
                  clock_.now(),
                  ExecutionState::kInterrupted,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {{"reason", "interrupted_retry_acknowledged"}}});
      accepted_from_state = ExecutionState::kInterrupted;
      active_correlation_.clear_program_run_and_action_span();
      runtime_collision_monitor_.clear_active_contact_policy_id();
      authorize_gripper_authority();
      break;
    }
    case RecoveryAction::kReplanFromStoppedState: {
      // Non-resetting internal stop-policy cleanup ONLY. This is
      // stop_policy_.request_reset() — the stop state machine returning to
      // Idle — and is deliberately distinct from execution_backend_.reset(),
      // which is never called on any accepted recovery action.
      auto reset_result = stop_policy_.request_reset();
      if (!reset_result.ok()) {
        return reject(std::string("stop_policy_reset_rejected"));
      }
      const auto prev_execution_id = capture.execution_id;
      sm_.set_execution_id(0);
      active_correlation_.clear_move_id();
      auto snapshot = snapshot_publisher_.latest();
      snapshot.timestamp_ns = clock_.now();
      snapshot.execution_id = 0;
      snapshot.move_id.clear();
      snapshot.state = ExecutionState::kIdle;
      snapshot.progress = 0.0;
      snapshot.error.reset();
      publish_execution_snapshot(std::move(snapshot));
      emit_event({KernelEventType::kStateTransition,
                  prev_execution_id,
                  clock_.now(),
                  ExecutionState::kStopped,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {{"reason", "stopped_replan_acknowledged"}}});
      accepted_from_state = ExecutionState::kStopped;
      active_correlation_.clear_program_run_and_action_span();
      authorize_gripper_authority();
      break;
    }
    case RecoveryAction::kAcknowledgeAndContinue: {
      auto result = apply_acknowledge_and_continue_locked(request.principal_id, request.reason);
      if (!result.ok()) {
        return reject(result.error().details.count("reason")
                          ? result.error().details.at("reason")
                          : std::string("intervention_ack_rejected"));
      }
      accepted_from_state = ExecutionState::kInterventionRequired;
      break;
    }
    case RecoveryAction::kRetryAfterSceneRepair: {
      auto result = apply_retry_after_scene_repair_locked(request.principal_id, request.reason);
      if (!result.ok()) {
        return reject(result.error().details.count("reason")
                          ? result.error().details.at("reason")
                          : std::string("failed_validation_ack_rejected"));
      }
      accepted_from_state = ExecutionState::kFailed;
      break;
    }
    case RecoveryAction::kResetCycle:
      return reject(reason_codes::kRecoveryActionNotAllowed);
  }
  auto final_assessment = refresh_recovery_assessment_locked("recovery_action_accepted");
  lifecycle_lock.unlock();
  KernelEvent accepted_event;
  accepted_event.type = KernelEventType::kRecoveryActionAccepted;
  accepted_event.execution_id = assessment.execution_id;
  accepted_event.timestamp_ns = clock_.now();
  accepted_event.from_state = accepted_from_state.value_or(assessment.execution_state);
  accepted_event.to_state = ExecutionState::kIdle;
  accepted_event.details = {
      {"request_id", request.request_id},
      {"action", recovery_action_name(request.action)},
      {"principal_id", request.principal_id},
      {"assessment_revision", std::to_string(assessment.assessment_revision)},
      {"stop_certificate_id", assessment.stop_certificate_id},
      {"latch_id", assessment.latch_id},
      {"backend_reset_performed", "false"},
      {"reason_code", "recovery_action_accepted"},
  };
  emit_event(std::move(accepted_event));
  return KernelRecoveryActionResultDTO{request.request_id, request.action, true, final_assessment,
                                       std::nullopt};
}

ExecutionStatus ExecutionKernel::get_status() const {
  auto status = sm_.status();
  status.active_move_id = active_correlation_.move_id();
  return status;
}

ExecutionSnapshot ExecutionKernel::get_execution_snapshot() const {
  const auto kernel_status = sm_.status();
  const auto kernel_state = kernel_status.state;
  // Read the live backend while idle so continuously observed physical state
  // is available before the first plan, and during kStopping/kExecuting where
  // serving the published cache would hide safety-relevant or moving state.
  // The kernel remains the lifecycle authority: state and execution identity
  // are stamped from the state machine, never trusted from the backend's view.
  if (kernel_state == ExecutionState::kStopping || kernel_state == ExecutionState::kExecuting ||
      kernel_state == ExecutionState::kIdle) {
    auto backend_snapshot = execution_backend_.get_execution_snapshot();
    // An idle backend may not have observed its first physical sample yet.
    // Preserve the kernel's published fallback until it has useful positions.
    if (kernel_state != ExecutionState::kIdle || !backend_snapshot.joint_positions_rad.empty()) {
      backend_snapshot.state = kernel_state;
      backend_snapshot.execution_id = kernel_status.execution_id;
      backend_snapshot.move_id = active_correlation_.move_id();
      if (backend_snapshot.timestamp_ns == 0) {
        backend_snapshot.timestamp_ns = clock_.now();
      }
      if (backend_snapshot.timestamp_ns > 0) {
        const auto now_ns = clock_.now();
        if (now_ns >= backend_snapshot.timestamp_ns) {
          const auto lag_ns = static_cast<uint64_t>(now_ns - backend_snapshot.timestamp_ns);
          std::lock_guard<std::mutex> lock(metrics_mutex_);
          metrics_.observer_lag_max_ns = std::max(metrics_.observer_lag_max_ns, lag_ns);
        }
      }
      stamp_active_request_correlation(backend_snapshot);
      return backend_snapshot;
    }
  }

  ExecutionSnapshot snapshot = snapshot_publisher_.latest();
  if (kernel_state == ExecutionState::kStopped || kernel_state == ExecutionState::kCompleted ||
      kernel_state == ExecutionState::kFailed ||
      kernel_state == ExecutionState::kInterventionRequired ||
      kernel_state == ExecutionState::kInterrupted || kernel_state == ExecutionState::kIdle) {
    snapshot.state = kernel_state;
    snapshot.execution_id = kernel_status.execution_id;
    snapshot.move_id = active_correlation_.move_id();
  }
  if (!snapshot.error.has_value() && (kernel_state == ExecutionState::kStopped ||
                                      kernel_state == ExecutionState::kInterventionRequired)) {
    if (kernel_state == ExecutionState::kStopped) {
      snapshot.error = operator_stop_worker_.terminal_error();
    } else if (kernel_state == ExecutionState::kInterventionRequired) {
      snapshot.error = operator_stop_worker_.intervention_error();
    }
  }
  if (snapshot.timestamp_ns > 0) {
    const auto now_ns = clock_.now();
    if (now_ns >= snapshot.timestamp_ns) {
      const auto lag_ns = static_cast<uint64_t>(now_ns - snapshot.timestamp_ns);
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.observer_lag_max_ns = std::max(metrics_.observer_lag_max_ns, lag_ns);
    }
  }
  stamp_active_request_correlation(snapshot);
  return snapshot;
}

Result<void> ExecutionKernel::refresh_observed_state_snapshot() {
  auto synchronized = execution_backend_.sync_observed_state_before_planning();
  if (!synchronized.ok()) {
    return synchronized;
  }
  auto observed = observed_state_.current_state(clock_.now());
  if (!observed.ok()) {
    return Result<void>::Err(observed.error());
  }
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = sm_.status().execution_id;
  snapshot.move_id = active_correlation_.move_id();
  snapshot.state = sm_.status().state;
  snapshot.progress = sm_.status().progress;
  snapshot.joint_positions_rad = std::move(observed.value().positions_rad);
  stamp_active_request_correlation(snapshot);
  publish_execution_snapshot(std::move(snapshot));
  return Result<void>::Ok();
}

MetricsSnapshot ExecutionKernel::get_metrics() const {
  MetricsSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    snapshot = metrics_;
  }
  snapshot.events_emitted = event_sequence_.load(std::memory_order_relaxed);
  snapshot.flight_recorder_records = flight_recorder_.recorded_total();
  snapshot.validation_collision_workers_requested =
      static_cast<std::uint64_t>(config_.validation_collision_workers);
  snapshot.validation_collision_workers_effective =
      validation_collision_workers_effective_.load(std::memory_order_relaxed);
  const auto validation_pool_status = collision_validation_worker_pool_->status();
  snapshot.validation_collision_jobs_queued = validation_pool_status.queued_jobs;
  snapshot.validation_collision_workers_active = validation_pool_status.active_workers;
  snapshot.validation_collision_jobs_completed = validation_pool_status.completed_jobs_total;
  snapshot.validation_collision_jobs_cancelled = validation_pool_status.cancelled_jobs_total;
  snapshot.validation_collision_jobs_dropped = validation_pool_status.dropped_jobs_total;
  snapshot.validation_collision_jobs_abandoned = validation_pool_status.abandoned_jobs_total;
  snapshot.runtime_collision_ticks_started =
      runtime_collision_ticks_started_.load(std::memory_order_relaxed);
  snapshot.runtime_collision_ticks_completed =
      runtime_collision_ticks_completed_.load(std::memory_order_relaxed);
  snapshot.runtime_collision_ticks_faulted =
      runtime_collision_ticks_faulted_.load(std::memory_order_relaxed);
  if (const auto* observed = execution_backend_.observed_robot_state_publication();
      observed != nullptr) {
    snapshot.observed_state_reader_retry_exhausted_total =
        observed->metrics().reader_retry_exhausted_total;
  }
  snapshot.runtime_collision_tick_max_ns =
      runtime_collision_tick_max_ns_.load(std::memory_order_relaxed);
  const auto runtime_timing = runtime_collision_monitor_.timing_metrics();
  snapshot.runtime_collision_tick_start_jitter_max_ns =
      runtime_timing.scheduled_to_start_jitter_max_ns;
  snapshot.runtime_collision_tick_late_periods = runtime_timing.late_periods;
  snapshot.runtime_collision_tick_missed_periods = runtime_timing.missed_periods;
  snapshot.runtime_collision_tick_max_consecutive_late_periods =
      runtime_timing.max_consecutive_late_periods;
  const auto coverage = runtime_collision_stop_coverage_.metrics();
  snapshot.runtime_collision_stop_windows_started = coverage.windows_started;
  snapshot.runtime_collision_stop_windows_completed = coverage.windows_completed;
  snapshot.runtime_collision_stop_ticks_attempted = coverage.ticks_attempted;
  snapshot.runtime_collision_stop_queries_executed = coverage.queries_executed;
  snapshot.runtime_collision_stop_unavailable_ticks = coverage.unavailable_ticks;
  snapshot.runtime_collision_stop_longest_unavailable_ns = coverage.longest_unavailable_ns;
  snapshot.runtime_collision_stop_zero_check_windows = coverage.zero_check_windows;
  snapshot.runtime_collision_last_stop_ticks_attempted = coverage.last_window.ticks_attempted;
  snapshot.runtime_collision_last_stop_queries_executed = coverage.last_window.queries_executed;
  snapshot.runtime_collision_last_stop_unavailable_ticks = coverage.last_window.unavailable_ticks;
  snapshot.runtime_collision_last_stop_longest_unavailable_ns =
      coverage.last_window.longest_unavailable_ns;
  snapshot.runtime_collision_freshest_observed_source_age_ns = coverage.freshest_source_age_ns;
  const int source_kind = coverage.freshest_source_kind;
  if (source_kind >= static_cast<int>(ObservedRobotStateSourceKind::kActionFeedback) &&
      source_kind <= static_cast<int>(ObservedRobotStateSourceKind::kPredictedState)) {
    snapshot.runtime_collision_freshest_observed_source_id = observed_robot_state_source_kind_name(
        static_cast<ObservedRobotStateSourceKind>(source_kind));
  }
  snapshot.runtime_collision_unavailable_events_suppressed = coverage.unavailable_events_suppressed;
  snapshot.validation_deferred_runtime_monitor_active =
      validation_deferred_runtime_monitor_active_.load(std::memory_order_relaxed);
  snapshot.runtime_collision_monitor_period_ns =
      static_cast<std::uint64_t>(config_.collision_monitor_period_ns);
  snapshot.runtime_collision_immediate_query_budget_ns =
      static_cast<std::uint64_t>(config_.runtime_collision_immediate_query_budget_ns);
  snapshot.runtime_collision_lookahead_cycle_budget_ns =
      static_cast<std::uint64_t>(config_.runtime_collision_lookahead_cycle_budget_ns);
  snapshot.runtime_collision_lookahead_query_budget_ns =
      static_cast<std::uint64_t>(config_.runtime_collision_lookahead_query_budget_ns);
  snapshot.runtime_collision_lookahead_horizon_ns =
      static_cast<std::uint64_t>(config_.runtime_collision_lookahead_horizon_ns);
  snapshot.runtime_collision_lookahead_max_samples =
      config_.runtime_collision_lookahead_max_samples;
  snapshot.runtime_collision_lookahead_workers_requested =
      static_cast<std::uint64_t>(config_.runtime_collision_lookahead_workers);
  if (const auto lookahead = runtime_collision_lookahead_session_.load(std::memory_order_acquire)) {
    const auto lookahead_status = lookahead->status();
    snapshot.runtime_collision_lookahead_workers_effective =
        static_cast<std::uint64_t>(lookahead_status.effective_workers);
    snapshot.runtime_collision_lookahead_armed = lookahead_status.armed;
    snapshot.runtime_collision_lookahead_job_active = lookahead_status.job_active;
    snapshot.runtime_collision_lookahead_permit_pending = lookahead_status.permit_pending;
    snapshot.runtime_collision_lookahead_execution_id = lookahead_status.execution_id;
    snapshot.runtime_collision_lookahead_execution_generation =
        lookahead_status.execution_generation;
    snapshot.runtime_collision_lookahead_cursor_valid = lookahead_status.cursor_valid;
    snapshot.runtime_collision_lookahead_cursor_source_kind = lookahead_status.cursor_source_kind;
    snapshot.runtime_collision_lookahead_cursor_authority_profile =
        lookahead_status.cursor_authority_profile;
    snapshot.runtime_collision_lookahead_cursor_age_ns =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, lookahead_status.cursor_age_ns));
    snapshot.runtime_collision_lookahead_jobs_started = lookahead_status.jobs_started_total;
    snapshot.runtime_collision_lookahead_jobs_clear = lookahead_status.jobs_clear_total;
    snapshot.runtime_collision_lookahead_jobs_collision = lookahead_status.jobs_collision_total;
    snapshot.runtime_collision_lookahead_jobs_inconclusive =
        lookahead_status.jobs_inconclusive_total;
    snapshot.runtime_collision_lookahead_jobs_query_timeout =
        lookahead_status.jobs_query_timeout_total;
    snapshot.runtime_collision_lookahead_jobs_cycle_timeout =
        lookahead_status.jobs_cycle_timeout_total;
    snapshot.runtime_collision_lookahead_jobs_superseded = lookahead_status.jobs_superseded_total;
    snapshot.runtime_collision_lookahead_reruns_coalesced = lookahead_status.reruns_coalesced_total;
    snapshot.runtime_collision_lookahead_clear_permits_invalidated =
        lookahead_status.clear_permits_invalidated_total;
    snapshot.runtime_collision_lookahead_clear_permit_try_lock_misses =
        lookahead_status.clear_permit_try_lock_misses_total;
    snapshot.runtime_collision_lookahead_stale_results =
        lookahead_status.stale_generation_results_total;
    snapshot.runtime_collision_lookahead_last_cycle_duration_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.last_cycle_duration_ns));
    snapshot.runtime_collision_lookahead_max_cycle_duration_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.max_cycle_duration_ns));
    snapshot.runtime_collision_lookahead_last_worker_active_duration_ns =
        static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, lookahead_status.last_worker_active_duration_ns));
    snapshot.runtime_collision_lookahead_max_worker_active_duration_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.max_worker_active_duration_ns));
    snapshot.runtime_collision_lookahead_max_start_jitter_ns =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, lookahead_status.max_start_jitter_ns));
    snapshot.runtime_collision_lookahead_last_inter_job_gap_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.last_inter_job_gap_ns));
    snapshot.runtime_collision_lookahead_active_time_total_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.active_time_total_ns));
    snapshot.runtime_collision_lookahead_duty_window_ns =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, lookahead_status.duty_window_ns));
    snapshot.runtime_collision_lookahead_duty_cycle_ppm = lookahead_status.duty_cycle_ppm;
    snapshot.runtime_collision_lookahead_last_cursor_time_ns =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, lookahead_status.last_cursor_time_ns));
    snapshot.runtime_collision_lookahead_last_covered_until_time_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, lookahead_status.last_covered_until_time_ns));
    snapshot.runtime_collision_lookahead_last_checked_samples =
        lookahead_status.last_checked_samples;
    snapshot.runtime_collision_lookahead_last_eligible_source_waypoints =
        lookahead_status.last_eligible_source_waypoints;
    snapshot.runtime_collision_lookahead_last_sampling_reduced =
        lookahead_status.last_sampling_reduced;
    snapshot.runtime_collision_lookahead_last_sample_times_ns =
        lookahead_status.last_sample_times_ns;
    snapshot.runtime_collision_lookahead_last_workers_used =
        static_cast<std::uint64_t>(std::max(0, lookahead_status.last_workers_used));
    snapshot.runtime_collision_lookahead_last_termination =
        runtime_collision_lookahead_termination_name(lookahead_status.last_termination);
  }
  return snapshot;
}

Result<void> ExecutionKernel::collision_check_tick() {
  auto prev_state = sm_.status().state;
  const bool trip_stop_active = motion_trip_stop_.active();
  const bool operator_stop_active = operator_stop_worker_.active();
  const bool fault_preempt_stop_active = this->fault_preempt_stop_active();
  const bool stop_path_active =
      trip_stop_active || operator_stop_active || fault_preempt_stop_active;
  if (stop_path_active) {
    // Defensive backstop for stop paths that become active between API
    // admission and this monitor tick. Repeated revocation is idempotent for
    // authority and keeps the lower-priority worker out of the deceleration
    // window.
    revoke_runtime_collision_lookahead_clear_permits();
  }
  const auto stop_coverage_generation =
      stop_path_active ? runtime_collision_stop_coverage_.active_generation() : 0;
  // The monitor runs over the robot during normal execution, while paused,
  // and over decelerating stop paths owned by motion-trip/operator/fault
  // preempt stop.
  if (!stop_path_active && prev_state != ExecutionState::kExecuting &&
      prev_state != ExecutionState::kPaused) {
    return Result<void>::Ok();
  }

  const auto tick_start_ns = clock_.now();
  runtime_collision_ticks_started_.fetch_add(1, std::memory_order_relaxed);
  runtime_collision_stop_coverage_.record_tick(stop_coverage_generation);
  const auto record_tick_metric = [this, tick_start_ns](bool faulted) {
    const auto now_ns = clock_.now();
    std::uint64_t duration_ns = 0;
    if (now_ns > tick_start_ns) {
      duration_ns = static_cast<std::uint64_t>(now_ns - tick_start_ns);
    }
    if (faulted) {
      runtime_collision_ticks_faulted_.fetch_add(1, std::memory_order_relaxed);
    } else {
      runtime_collision_ticks_completed_.fetch_add(1, std::memory_order_relaxed);
    }
    update_atomic_max(runtime_collision_tick_max_ns_, duration_ns);
  };

  auto publish_runtime_monitor_fault =
      [this, &record_tick_metric, trip_stop_active, operator_stop_active, fault_preempt_stop_active,
       stop_path_active,
       stop_coverage_generation](CollisionCheckResultDTO check,
                                 std::map<std::string, std::string> details) -> Result<void> {
    // A future check is authorized only by the most recent clear immediate
    // tick. Withdraw that authority before any fault/stop evidence is
    // emitted; permit generations make queued and in-flight work stale.
    revoke_runtime_collision_lookahead_clear_permits();
    record_tick_metric(true);
    details["surface"] = "observed_runtime_monitor";
    emit_contact_policy_violation_event(
        sm_.status().execution_id, details,
        stop_path_active ? std::optional<std::string_view>{"signal_active_stop"}
                         : std::optional<std::string_view>{"preempt_trajectory"});
    const auto stop_owner = [&]() -> const char* {
      if (trip_stop_active) {
        return "motion_trip_stop";
      }
      if (operator_stop_active) {
        return "operator_stop";
      }
      return "fault_preempt_stop";
    };

    // Stop-path collision: during an active motion-trip, operator, or
    // fault-preempt controlled stop, a collision must not latch a new fault or
    // race terminal state authority. The monitor signals the owning stop path;
    // that worker owns cancellation, hard-stop escalation, and terminal commit.
    if (stop_path_active) {
      // Only an actual detected collision escalates the in-flight stop. A
      // monitor availability/config fault (no contact) must not disrupt a
      // stop already in progress, but it must be auditable.
      if (!check.in_collision) {
        const auto now_ns = clock_.now();
        details["reason_during_stop"] = "stop_monitor_unavailable";
        details["stop_owner"] = stop_owner();
        const auto reason_it = details.find("reason");
        const auto reason = reason_it != details.end() ? reason_it->second
                                                       : std::string("runtime_monitor_unavailable");
        const auto unavailable_reason = classify_runtime_collision_unavailable_reason(reason);
        runtime_collision_stop_coverage_.record_unavailable(stop_coverage_generation,
                                                            unavailable_reason, now_ns);
        const auto reason_hash = static_cast<std::uint64_t>(std::hash<std::string>{}(reason));
        if (!runtime_collision_stop_coverage_.should_emit_unavailable(reason_hash, now_ns)) {
          return Result<void>::Ok();
        }
        KernelEvent monitor_fault_event;
        monitor_fault_event.type = KernelEventType::kErrorRaised;
        monitor_fault_event.execution_id = sm_.status().execution_id;
        monitor_fault_event.timestamp_ns = clock_.now();
        monitor_fault_event.details = details;
        monitor_fault_event.error =
            ErrorDTO{std::string(kErrorExecutionAborted),
                     "runtime collision monitor unavailable during controlled stop", details};
        emit_event(monitor_fault_event);
        return Result<void>::Ok();
      }
      details["reason_during_stop"] = "stop_path_collision";
      details["stop_owner"] = stop_owner();
      if (check.contact_body_a) {
        details["contact_a"] = *check.contact_body_a;
      }
      if (check.contact_body_b) {
        details["contact_b"] = *check.contact_body_b;
      }
      if (trip_stop_active) {
        motion_trip_stop_.signal_collision_and_cancel();
      }
      if (operator_stop_active) {
        const bool cancel_requested = signal_operator_stop_path_collision(details);
        details["operator_stop_cancel_requested"] = cancel_requested ? "true" : "false";
      }
      if (fault_preempt_stop_active) {
        const bool cancel_requested = signal_fault_preempt_stop_path_collision(details);
        details["fault_preempt_stop_cancel_requested"] = cancel_requested ? "true" : "false";
      }
      KernelEvent stop_collision_event;
      stop_collision_event.type = KernelEventType::kCollisionDetected;
      stop_collision_event.execution_id = sm_.status().execution_id;
      stop_collision_event.timestamp_ns = clock_.now();
      stop_collision_event.details = details;
      emit_event(stop_collision_event);
      return Result<void>::Ok();
    }

    KernelEvent collision_event;
    collision_event.type = KernelEventType::kCollisionDetected;
    collision_event.execution_id = sm_.status().execution_id;
    collision_event.timestamp_ns = clock_.now();
    collision_event.details = details;
    if (check.contact_body_a) {
      collision_event.details["contact_a"] = *check.contact_body_a;
    }
    if (check.contact_body_b) {
      collision_event.details["contact_b"] = *check.contact_body_b;
    }
    emit_event(collision_event);
    {
      std::lock_guard<std::mutex> collision_metrics_lock(metrics_mutex_);
      metrics_.collision_detections++;
    }
    cache_.invalidate_all();
    auto interrupt = publish_runtime_collision_fault_interrupt(check, collision_event.details);
    if (interrupt.rejected) {
      return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                        "Runtime collision interrupt signal rejected",
                                        {{"reason", interrupt.reason}}});
    }
    return Result<void>::Ok();
  };

  const auto query_session = runtime_collision_query_session_.load(std::memory_order_acquire);
  if (!query_session) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "runtime_collision_query_session_unavailable"}};
    {
      std::lock_guard<std::mutex> error_lock(runtime_collision_query_prepare_error_mutex_);
      if (runtime_collision_query_prepare_error_) {
        for (const auto& [key, value] : runtime_collision_query_prepare_error_->details) {
          details[key] = value;
        }
      }
    }
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }

  RuntimeCollisionQueryBundlePtr published_bundle;
  try {
    published_bundle = query_session->acquire_bundle();
  } catch (const std::exception& e) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "published_runtime_collision_snapshot_exception"},
        {"exception", e.what()}};
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.runtime_collision_snapshot_refresh_failures++;
    }
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  } catch (...) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "published_runtime_collision_snapshot_exception"},
        {"exception", reason_codes::kUnknownNonStdException}};
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.runtime_collision_snapshot_refresh_failures++;
    }
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }
  if (!published_bundle || !published_bundle->world || !published_bundle->policy ||
      !published_bundle->policy_index) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "runtime_collision_snapshot_not_published"}};
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }
  const auto& world_snapshot = published_bundle->world;
  const auto& policy_snapshot = published_bundle->policy;
  const auto& runtime_capability = query_session->engine_capability();
  if (world_snapshot->bodies.size() > runtime_capability.runtime_query_max_world_bodies ||
      published_bundle->body_freshness.size() > runtime_capability.runtime_query_max_world_bodies) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "runtime_collision_bundle_capacity_exceeded"},
        {"body_count", std::to_string(world_snapshot->bodies.size())},
        {"freshness_count", std::to_string(published_bundle->body_freshness.size())},
        {"body_bound", std::to_string(runtime_capability.runtime_query_max_world_bodies)}};
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }
  if (!published_bundle->lookup_order_valid) {
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "runtime_collision_bundle_lookup_order_invalid"}};
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }

  const auto dynamic_body_now_ns = clock_.now();
  const auto freshness = check_runtime_dynamic_body_freshness(
      *world_snapshot, published_bundle->body_freshness, dynamic_body_now_ns,
      config_.runtime_dynamic_body_max_age_cap_ns, published_bundle->lookup_order_valid);
  if (freshness.status != RuntimeDynamicBodyFreshnessStatus::kFresh) {
    std::map<std::string, std::string> details{{"reason_code", "runtime_collision_monitor"}};
    const auto reason = validate_dynamic_body_freshness(
        *world_snapshot, published_bundle->body_freshness, dynamic_body_now_ns,
        config_.runtime_dynamic_body_max_age_cap_ns, details);
    details["reason"] = reason.value_or(runtime_dynamic_body_freshness_reason(freshness.status));
    details["collision_world_revision_id"] = std::to_string(world_snapshot->revision.revision_id);
    details["collision_world_content_hash"] = world_snapshot->revision.content_hash;
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(details));
  }

  const auto* observed_publication = execution_backend_.observed_robot_state_publication();
  if (observed_publication == nullptr) {
    std::map<std::string, std::string> monitor_details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "observed_state_unavailable"},
        {"observed_state_unavailable_reason", "backend_observed_state_publication_unconfigured"}};
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(monitor_details));
  }
  const auto observed_now_ns = clock_.now();
  auto observed_view = observed_publication->acquire(observed_now_ns);
  if (!observed_view) {
    std::map<std::string, std::string> monitor_details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "observed_state_unavailable"},
        {"observed_state_unavailable_reason", "no_observed_state_sample_published"}};
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(monitor_details));
  }
  const auto publication_budget_ns = observed_view.freshness_budget_ns();
  const auto observed_age_ns =
      observed_now_ns >= observed_view.received_timestamp_ns()
          ? static_cast<std::uint64_t>(observed_now_ns - observed_view.received_timestamp_ns())
          : 0;
  runtime_collision_stop_coverage_.record_observed_source(observed_view.source_kind(),
                                                          observed_age_ns);
  const auto effective_budget_ns =
      config_.runtime_observed_state_max_age_ns == 0 ? 0
      : publication_budget_ns > 0
          ? std::min<std::int64_t>(publication_budget_ns, config_.runtime_observed_state_max_age_ns)
          : config_.runtime_observed_state_max_age_ns;
  const bool observed_timestamp_invalid =
      effective_budget_ns > 0 && (observed_view.received_timestamp_ns() < 0 ||
                                  observed_view.received_timestamp_ns() > observed_now_ns);
  const bool observed_timestamp_stale =
      effective_budget_ns > 0 && !observed_timestamp_invalid &&
      observed_now_ns - observed_view.received_timestamp_ns() > effective_budget_ns;
  auto append_observed_details = [&](auto& details) {
    details["observed_state_source_id"] = std::string(observed_view.source_id());
    details["observed_state_source_kind"] =
        observed_robot_state_source_kind_name(observed_view.source_kind());
    details["observed_state_authority_profile"] = std::string(observed_view.authority_profile_id());
    details["observed_state_sequence"] = std::to_string(observed_view.sequence());
    details["observed_state_source_timestamp_ns"] =
        std::to_string(observed_view.source_timestamp_ns());
    details["observed_state_received_timestamp_ns"] =
        std::to_string(observed_view.received_timestamp_ns());
    details["observed_state_age_ns"] = std::to_string(observed_age_ns);
    details["observed_state_freshness_budget_ns"] = std::to_string(effective_budget_ns);
  };
  if (observed_timestamp_invalid || observed_timestamp_stale) {
    std::map<std::string, std::string> monitor_details{{"reason_code", "runtime_collision_monitor"},
                                                       {"reason", "stale_observed_robot_state"}};
    append_observed_details(monitor_details);
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(monitor_details));
  }
  if (!observed_view.complete() || observed_view.positions_rad().empty()) {
    std::map<std::string, std::string> monitor_details{{"reason_code", "runtime_collision_monitor"},
                                                       {"reason", "observed_state_incomplete"}};
    append_observed_details(monitor_details);
    return publish_runtime_monitor_fault(CollisionCheckResultDTO{}, std::move(monitor_details));
  }

  const auto outcome = query_session->tick(published_bundle, observed_view.positions_rad(),
                                           observed_view.source_timestamp_ns());
  if (outcome.query_executed) {
    runtime_collision_stop_coverage_.record_query(stop_coverage_generation, observed_now_ns);
  }
  if (outcome.status == RuntimeCollisionTickStatus::kClear) {
    if (!stop_path_active) {
      if (const auto lookahead =
              runtime_collision_lookahead_session_.load(std::memory_order_acquire)) {
        // The immediate current-state query is always first. Only its clear
        // outcome may open one lower-priority, capacity-one future check.
        // Start the lower-priority cycle only after the reserved immediate
        // query has cleared. Its cycle budget must not charge time owned by
        // the higher-priority monitor.
        lookahead->request(clock_.now(), std::move(published_bundle),
                           query_session->contact_release_state_snapshot());
      }
    }
    record_tick_metric(false);
    return Result<void>::Ok();
  }

  std::map<std::string, std::string> monitor_details{
      {"reason_code", "runtime_collision_monitor"},
      {"reason", runtime_collision_tick_reason_name(outcome.reason)},
      {"collision_world_revision_id", std::to_string(world_snapshot->revision.revision_id)},
      {"collision_world_content_hash", world_snapshot->revision.content_hash},
      {"published_collision_policy_revision_id",
       std::to_string(policy_snapshot->revision.revision_id)},
      {"published_collision_policy_content_hash", policy_snapshot->revision.content_hash},
      {"runtime_bundle_epoch", std::to_string(published_bundle->epoch)},
      {"runtime_query_engine_id", query_session->engine_capability().engine_id},
      {"runtime_query_engine_version", query_session->engine_capability().engine_version},
      {"runtime_query_duration_ns", std::to_string(outcome.query_duration_ns)}};
  append_observed_details(monitor_details);
  if (outcome.engine_failure_reason != RuntimeCollisionQueryFailureReason::kNone) {
    monitor_details["runtime_query_failure_reason"] =
        runtime_collision_query_failure_reason_name(outcome.engine_failure_reason);
  }
  monitor_details["runtime_query_engine_status"] =
      runtime_collision_engine_status_name(outcome.engine_status);
  CollisionCheckResultDTO check;
  check.in_collision = outcome.status == RuntimeCollisionTickStatus::kCollision;
  if (outcome.pair != nullptr) {
    check.contact_body_a = outcome.pair->body_a;
    check.contact_body_b = outcome.pair->body_b;
    monitor_details["contact_a"] = outcome.pair->body_a;
    monitor_details["contact_b"] = outcome.pair->body_b;
    monitor_details["collision_result_first_pair"] =
        outcome.pair->body_a + " <-> " + outcome.pair->body_b;
    monitor_details["collision_result_first_failure_pair"] =
        monitor_details["collision_result_first_pair"];
    monitor_details["collision_result_first_failure_classification"] =
        outcome.pair_classification == CollisionPairClassification::kBlockingCollision
            ? "blocking_collision"
            : "diagnostic_incomplete";
  }
  if (outcome.contact_grasp_violation != RuntimeContactGraspViolation::kNone) {
    monitor_details["collision_result_raw_contact_grasp_policy_id"] =
        query_session->active_contact_policy_id();
    monitor_details["collision_result_raw_contact_grasp_violation"] =
        outcome.contact_grasp_violation == RuntimeContactGraspViolation::kMissingClearance
            ? "missing_grasp_contact_clearance"
            : "grasp_contact_penetration_exceeded";
  }
  if (outcome.signed_clearance_m) {
    monitor_details["runtime_signed_clearance_m"] = format_double(*outcome.signed_clearance_m);
  }
  if (outcome.initial_penetration_m) {
    monitor_details["contact_release_initial_penetration_m"] =
        format_double(*outcome.initial_penetration_m);
  }
  if (outcome.previous_penetration_m) {
    monitor_details["contact_release_previous_penetration_m"] =
        format_double(*outcome.previous_penetration_m);
  }
  if (outcome.current_penetration_m) {
    const auto current = format_double(*outcome.current_penetration_m);
    if (outcome.max_contact_penetration_m) {
      monitor_details["contact_policy_mode"] = "bounded_penetration";
      monitor_details["current_penetration_m"] = current;
      monitor_details["max_contact_penetration_m"] =
          format_double(*outcome.max_contact_penetration_m);
      if (outcome.reason == RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded &&
          std::isfinite(*outcome.current_penetration_m)) {
        monitor_details["within_penetration_bound"] = "false";
      }
    } else {
      monitor_details["contact_release_current_penetration_m"] = current;
    }
  }
  const auto contact_violation_reason = [&]() -> const char* {
    switch (outcome.reason) {
      case RuntimeCollisionTickReason::kContactReleaseMissingClearance:
        return "missing_contact_release_clearance";
      case RuntimeCollisionTickReason::kContactReleaseNewContact:
        return "new_active_contact_after_clear_start";
      case RuntimeCollisionTickReason::kContactReleaseInitialPenetrationExceeded:
        return "initial_penetration_exceeds_policy";
      case RuntimeCollisionTickReason::kContactReleaseRecontact:
        return "recontact_after_release";
      case RuntimeCollisionTickReason::kContactReleaseRecontactPenetrationExceeded:
        return "recontact_penetration_exceeds_policy";
      case RuntimeCollisionTickReason::kContactReleasePenetrationIncreased:
        return "penetration_increased";
      case RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded:
        return "contact_penetration_exceeds_policy";
      default:
        return nullptr;
    }
  }();
  if (contact_violation_reason != nullptr && outcome.pair != nullptr &&
      outcome.contact_policy_mode.has_value()) {
    const auto& pair = *outcome.pair;
    monitor_details["contact_policy_violation"] = contact_violation_reason;
    monitor_details["contact_policy_id"] =
        policy_snapshot->policy_id.empty()
            ? "support_contacts@" + std::to_string(policy_snapshot->revision.revision_id)
            : policy_snapshot->policy_id;
    monitor_details["policy_mode"] =
        *outcome.contact_policy_mode == CollisionContactPolicyMode::kBoundedPenetration
            ? "bounded_penetration"
            : "contact_release";
    monitor_details["zone_id"] = "support_pair:" + pair.body_a + "<->" + pair.body_b;
    monitor_details["contact_body_a"] = pair.body_a;
    monitor_details["contact_body_b"] = pair.body_b;
    monitor_details["contact_present"] = outcome.contact_present ? "true" : "false";
    monitor_details["initial_contact_pair_count"] =
        std::to_string(outcome.initial_contact_pair_count);
    monitor_details["current_contact_pair_count"] =
        std::to_string(outcome.current_contact_pair_count);
    monitor_details["distance_request_mode"] =
        collision_distance_request_mode_name(outcome.distance_request_mode);
    monitor_details["evaluated_distance_pair_count"] =
        std::to_string(outcome.evaluated_distance_pair_count);
    monitor_details["response"] = "preempt_trajectory";
    monitor_details["classification"] =
        outcome.pair_classification == CollisionPairClassification::kDiagnosticIncomplete
            ? "diagnostic_incomplete"
            : "blocking_collision";
    if (*outcome.contact_policy_mode == CollisionContactPolicyMode::kContactRelease &&
        outcome.release_evidence_mode.has_value()) {
      monitor_details["release_evidence_mode"] =
          *outcome.release_evidence_mode == ContactReleaseEvidenceMode::kBinaryContact
              ? "binary_contact"
              : "signed_penetration";
    }
    if (outcome.contact_present && outcome.current_penetration_m &&
        (*outcome.contact_policy_mode == CollisionContactPolicyMode::kBoundedPenetration ||
         outcome.release_evidence_mode == ContactReleaseEvidenceMode::kSignedPenetration)) {
      monitor_details["penetration_depth_m"] = format_double(*outcome.current_penetration_m);
    }
    if (outcome.max_contact_penetration_m) {
      monitor_details["max_contact_penetration_m"] =
          format_double(*outcome.max_contact_penetration_m);
      if (outcome.reason == RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded &&
          outcome.current_penetration_m.has_value() &&
          std::isfinite(*outcome.current_penetration_m)) {
        monitor_details["within_penetration_bound"] = "false";
      }
    }
    if (outcome.reason == RuntimeCollisionTickReason::kContactReleaseInitialPenetrationExceeded) {
      const auto policy_entry = published_bundle->policy_index->support_contact_index.find(pair);
      if (policy_entry != published_bundle->policy_index->support_contact_index.end() &&
          policy_entry->second.enforce_max_start_penetration) {
        monitor_details["max_penetration_m"] =
            format_double(policy_entry->second.max_start_penetration_m);
      }
    }
  }
  if (outcome.contact_grasp_violation != RuntimeContactGraspViolation::kNone &&
      outcome.pair != nullptr) {
    const auto active_policy =
        std::find_if(policy_snapshot->contact_grasps.begin(), policy_snapshot->contact_grasps.end(),
                     [&](const ContactGraspPolicyEntry& candidate) {
                       return candidate.policy_id == query_session->active_contact_policy_id();
                     });
    if (active_policy != policy_snapshot->contact_grasps.end()) {
      auto pair = *outcome.pair;
      if (pair.body_b < pair.body_a) {
        std::swap(pair.body_a, pair.body_b);
      }
      const bool missing_clearance =
          outcome.contact_grasp_violation == RuntimeContactGraspViolation::kMissingClearance;
      monitor_details["contact_policy_violation"] =
          missing_clearance ? reason_codes::kMissingGraspContactClearance
                            : reason_codes::kGraspContactPenetrationExceeded;
      monitor_details["contact_policy_id"] = active_policy->policy_id;
      monitor_details["policy_mode"] = "contact_grasp";
      monitor_details["zone_id"] = "contact_grasp_pair:" + pair.body_a + "<->" + pair.body_b;
      monitor_details["contact_body_a"] = pair.body_a;
      monitor_details["contact_body_b"] = pair.body_b;
      monitor_details["contact_present"] = outcome.contact_present ? "true" : "false";
      monitor_details["distance_request_mode"] =
          collision_distance_request_mode_name(outcome.distance_request_mode);
      monitor_details["evaluated_distance_pair_count"] =
          std::to_string(outcome.evaluated_distance_pair_count);
      monitor_details["max_penetration_m"] = format_double(active_policy->max_penetration_m);
      monitor_details["response"] = "preempt_trajectory";
      monitor_details["classification"] =
          outcome.pair_classification == CollisionPairClassification::kDiagnosticIncomplete
              ? "diagnostic_incomplete"
              : "blocking_collision";
      if (!missing_clearance && outcome.signed_clearance_m.has_value() &&
          std::isfinite(*outcome.signed_clearance_m) && *outcome.signed_clearance_m <= 0.0) {
        monitor_details["penetration_depth_m"] =
            format_double(std::max(0.0, -*outcome.signed_clearance_m));
      }
    }
  }
  return publish_runtime_monitor_fault(check, std::move(monitor_details));
}

Result<std::size_t> ExecutionKernel::discover_dynamic_carried_start_contacts(
    CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& observed_start,
    const std::uint64_t execution_id) {
  const auto& world = projection_inventory.bundle.world;
  const auto& policy = projection_inventory.bundle.policy;
  if (!world || !policy.has_value()) {
    return Result<std::size_t>::Err(authoritative_collision_validation_config_error(
        !world ? "snapshot_unavailable" : "missing_collision_policy_snapshot"));
  }

  bool has_carried_scene_body = false;
  bool has_detached_scene_body = false;
  for (const auto& body : world->bodies) {
    if (body.owner_kind != CollisionOwnerKind::kSceneResource) {
      continue;
    }
    has_carried_scene_body = has_carried_scene_body || body.parent_body_id.has_value();
    has_detached_scene_body = has_detached_scene_body || !body.parent_body_id.has_value();
  }
  if (!has_carried_scene_body || !has_detached_scene_body) {
    return Result<std::size_t>::Ok(0U);
  }

  const auto collision_resources = collision_resources_.snapshot();
  const auto runtime_protection = runtime_collision_protection_snapshot();
  const auto admission =
      CollisionResourceManager::validate_admission(collision_resources, runtime_protection.active);
  if (!admission.admitted()) {
    return Result<std::size_t>::Err(validation_admission_error(
        sm_.status().state, collision_resources, admission, runtime_protection));
  }
  const auto engine = std::find_if(collision_resources.validation_engines.begin(),
                                   collision_resources.validation_engines.end(),
                                   [](const auto& candidate) { return candidate != nullptr; });
  if (engine == collision_resources.validation_engines.end()) {
    return Result<std::size_t>::Err(
        authoritative_collision_validation_config_error("missing_collision_query_engines"));
  }

  CollisionQuery query;
  query.query_id = "dynamic-carried-start-contact/" + std::to_string(execution_id);
  query.surface = CollisionSurface::kPreExecutionValidation;
  query.collision_world_revision = world->revision;
  query.policy_revision = policy->revision.content_hash;
  query.robot_state = observed_start;
  query.waypoint_index = 0;
  query.state_time_ns = 0;
  query.sampling_mode = CollisionSamplingMode::kWaypointOnly;
  query.timeout_budget_ns =
      static_cast<std::int64_t>(config_.validation_collision_query_timeout_ns);
  query.distance_request_mode = CollisionDistanceRequestMode::kNone;
  CollisionCancellationToken cancellation;
  auto result = (*engine)->check(*world, query, *policy, cancellation);
  if (result.engine_status != CollisionEngineStatus::kOk ||
      !result.collision_world_revision.has_value() ||
      result.collision_world_revision->revision_id != world->revision.revision_id ||
      result.collision_world_revision->content_hash != world->revision.content_hash) {
    return Result<std::size_t>::Err(dynamic_carried_start_contact_query_error(result));
  }
  const auto discovered =
      dynamic_carried_start_contact_pairs(*world, result.normalized_pairs, *policy);
  if (discovered.empty()) {
    return Result<std::size_t>::Ok(0U);
  }
  auto overlay = with_dynamic_carried_start_contacts(*policy, discovered);
  if (!overlay.ok()) {
    return Result<std::size_t>::Err(overlay.error());
  }
  projection_inventory.bundle.policy = std::move(overlay.value());
  return Result<std::size_t>::Ok(discovered.size());
}

Result<PlanRequestDTO> ExecutionKernel::make_plan_request(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
    std::uint32_t planning_attempt_index, const RobotStateDTO* start_state_override) {
  const auto& projection_binding = projection_inventory.projection_binding;
  if (!projection_binding.valid()) {
    return Result<PlanRequestDTO>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Planning requires a valid retained scene projection binding",
                 {{"reason", "planner_projection_binding_invalid"}}});
  }
  RobotStateDTO current_state;
  if (start_state_override != nullptr) {
    current_state = *start_state_override;
  } else {
    auto current_state_result = observed_state_.current_state(clock_.now());
    if (!current_state_result.ok()) {
      return Result<PlanRequestDTO>::Err(current_state_result.error());
    }
    current_state = std::move(current_state_result.value());
  }
  auto controlled_state = controlled_state_for_binding(request, current_state);
  if (!controlled_state.ok()) {
    return Result<PlanRequestDTO>::Err(controlled_state.error());
  }

  const auto& binding = request.binding;
  return Result<PlanRequestDTO>::Ok(PlanRequestDTO{
      binding.operation_id,
      binding.participant_id,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      binding.program_run_id,
      MoveId(request.move_id),
      binding.controllable_component_id,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      binding.planner_capability_binding,
      binding.model_bindings,
      binding.model_group_binding,
      binding.control_domain_id,
      binding.control_domain_revision,
      binding.control_binding_id,
      binding.control_binding_revision,
      binding.control_interface_contract_id,
      binding.control_interface_revision,
      binding.controller_topology_id,
      binding.topology_revision,
      binding.control_route_id,
      binding.activation_set_id,
      binding.active_control_configuration_id,
      binding.active_control_configuration_generation,
      binding.provider_instance_id,
      binding.provider_manifest_id,
      binding.provider_manifest_revision,
      binding.provider_runtime_epoch,
      binding.runtime_unit_bindings,
      binding.resources,
      request.action_span,
      binding.base_frame,
      binding.tcp_frame,
      std::move(controlled_state.value()),
      request.motion,
      request.tolerances,
      request.speed_profile,
      request.planning_profile,
      request.contact_policy_id.empty() ? std::nullopt
                                        : std::optional<std::string>{request.contact_policy_id},
      request.per_resource_dynamics_limits,
      request.timeouts.planning_timeout_s,
      planning_attempt_index,
      projection_binding,
      binding.component_attachment_revisions,
      binding.carried_object_attachment_revisions,
      projection_inventory.bundle.world,
      projection_inventory.bundle.policy.has_value()
          ? std::make_shared<const CollisionPolicySnapshot>(*projection_inventory.bundle.policy)
          : nullptr});
}

Result<ExecutionKernel::PlanningSuccess> ExecutionKernel::plan_trajectory_with_evidence(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
    std::uint32_t planning_attempt_index, const RobotStateDTO* start_state_override) {
  auto plan_request = make_plan_request(request, projection_inventory, planning_attempt_index,
                                        start_state_override);
  if (!plan_request.ok()) {
    return Result<PlanningSuccess>::Err(plan_request.error());
  }

  auto result = planner_.plan(plan_request.value());

  if (result.trajectory.has_value()) {
    const auto& projection_binding = projection_inventory.projection_binding;
    const auto& binding = request.binding;
    if (result.projection_revision != projection_binding.projection_revision) {
      return Result<PlanningSuccess>::Err(ErrorDTO{
          kErrorInvariantViolation,
          "Planner result did not acknowledge the retained projection revision",
          {{"reason", "planner_result_projection_revision_mismatch"},
           {"requested_revision_id",
            std::to_string(projection_binding.projection_revision.scene_revision.revision_id)},
           {"returned_revision_id",
            std::to_string(result.projection_revision.scene_revision.revision_id)}}});
    }
    if (result.trajectory->resources != binding.resources ||
        result.trajectory->joint_names != binding.model_group_binding.compiled_joint_names) {
      return Result<PlanningSuccess>::Err(
          ErrorDTO{kErrorInvariantViolation,
                   "Planner result changed the exact resolved resource projection",
                   {{"reason", "planner_result_resource_projection_mismatch"}}});
    }
    return Result<PlanningSuccess>::Ok(
        PlanningSuccess{std::move(plan_request.value()), std::move(result)});
  }

  if (result.error.has_value()) {
    return Result<PlanningSuccess>::Err(std::move(*result.error));
  }

  return Result<PlanningSuccess>::Err(ErrorCode::kPlanningFailed);
}

Result<TrajectoryDTO> ExecutionKernel::plan_trajectory(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
    std::uint32_t planning_attempt_index, const RobotStateDTO* start_state_override) {
  auto planned = plan_trajectory_with_evidence(request, projection_inventory,
                                               planning_attempt_index, start_state_override);
  if (!planned.ok()) {
    return Result<TrajectoryDTO>::Err(planned.error());
  }
  return Result<TrajectoryDTO>::Ok(std::move(*planned.value().result.trajectory));
}

Result<TrajectoryDTO> ExecutionKernel::plan_resume_trajectory(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory) {
  auto planned = plan_resume_trajectory_with_evidence(request, projection_inventory);
  if (!planned.ok()) {
    return Result<TrajectoryDTO>::Err(planned.error());
  }
  return Result<TrajectoryDTO>::Ok(std::move(*planned.value().result.trajectory));
}

Result<ExecutionKernel::PlanningSuccess> ExecutionKernel::plan_resume_trajectory_with_evidence(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory) {
  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    auto error = sync_result.error();
    error.details["resume_phase"] = "state_sync";
    return Result<PlanningSuccess>::Err(std::move(error));
  }

  try {
    auto planned = plan_trajectory_with_evidence(request, projection_inventory);
    if (!planned.ok()) {
      auto error = planned.error();
      error.details["resume_phase"] = "planning";
      return Result<PlanningSuccess>::Err(std::move(error));
    }
    return planned;
  } catch (const std::exception& e) {
    return Result<PlanningSuccess>::Err(
        ErrorDTO{kErrorPlanningFailed,
                 "Resume planning could not read the fresh current state",
                 {{"reason", "resume_current_state_unavailable"},
                  {"resume_phase", "current_state"},
                  {"exception", e.what()}}});
  } catch (...) {
    return Result<PlanningSuccess>::Err(
        ErrorDTO{kErrorPlanningFailed,
                 "Resume planning could not read the fresh current state",
                 {{"reason", "resume_current_state_unavailable"},
                  {"resume_phase", "current_state"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
}

Result<ExecutionKernel::CollisionValidationSuccess> ExecutionKernel::validate_trajectory(
    const TrajectoryDTO& trajectory, const ExecuteRequestDTO& request,
    CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
    const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
    const RobotStateDTO* start_state_override, const std::stop_token external_stop_token) {
  // Start-state gate
  RobotStateDTO current_state;
  if (start_state_override != nullptr) {
    current_state = *start_state_override;
  } else {
    auto current_state_result = observed_state_.current_state(clock_.now());
    if (!current_state_result.ok()) {
      return Result<CollisionValidationSuccess>::Err(current_state_result.error());
    }
    current_state = std::move(current_state_result.value());
  }
  if (!trajectory.waypoints.empty()) {
    const auto start_match = cached_trajectory_matches_current_start(
        trajectory, current_state, request.tolerances.joint_position_tol_rad);
    if (!start_match.compatible) {
      return Result<CollisionValidationSuccess>::Err(ErrorDTO{
          kErrorStartStateMismatch, "Start state mismatch", {{"reason", start_match.reason}}});
    }
  }

  std::shared_ptr<const CollisionPolicySnapshot> snapshot;
  std::shared_ptr<const CollisionPolicyIndex> snapshot_index;
  PoseSourceRetryConfig retry;
  std::int64_t per_state_timeout_ns = 0;
  {
    std::scoped_lock lock(coordinator_mutex_);
    snapshot = coordinator_policy_snapshot_;
    snapshot_index = coordinator_policy_index_;
    retry = coordinator_retry_config_;
    per_state_timeout_ns = coordinator_per_state_timeout_ns_;
  }
  const auto collision_resources = collision_resources_.snapshot();
  std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines =
      collision_resources.validation_engines;
  const auto runtime_protection = runtime_collision_protection_snapshot();
  const auto validation_admission =
      CollisionResourceManager::validate_admission(collision_resources, runtime_protection.active);
  if (!validation_admission.admitted()) {
    if (validation_admission.status ==
        CollisionValidationAdmissionStatus::kRuntimeMonitorLeaseActiveSharedResource) {
      validation_deferred_runtime_monitor_active_.fetch_add(1, std::memory_order_relaxed);
    }
    return Result<CollisionValidationSuccess>::Err(validation_admission_error(
        sm_.status().state, collision_resources, validation_admission, runtime_protection));
  }

  auto cancel = install_validation_cancel_token();
  std::stop_callback external_cancel(
      external_stop_token, [cancel]() { cancel->request_cancel("motion_preview_cancelled"); });
  if (external_stop_token.stop_requested()) {
    cancel->request_cancel("motion_preview_cancelled");
  }
  struct ValidationCancelTokenGuard {
    ExecutionKernel* kernel = nullptr;
    std::shared_ptr<CollisionCancellationToken> token;
    ~ValidationCancelTokenGuard() {
      if (kernel) {
        kernel->clear_validation_cancel_token(token);
      }
    }
  } cancel_guard{this, cancel};

  // Full non-collision trajectory validation (including TCP speed limit per
  // spec 230 §3.3). The manyforge_collision pipeline is the only
  // collision authority, so the backend validator must not run its legacy
  // collision sweep first and reject contacts the active policy would allow.
  ValidationOptionsDTO opts;
  opts.tcp_frame = compiled_frame_name(request.binding.tcp_frame);
  opts.base_frame = compiled_frame_name(request.binding.base_frame);
  opts.max_tcp_linear_speed_mps = request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
  opts.run_collision_checks = false;
  auto val_result = trajectory_validator_.validate_non_collision(trajectory, opts, *cancel);
  if (!val_result.valid) {
    if (val_result.error.has_value()) {
      return Result<CollisionValidationSuccess>::Err(std::move(*val_result.error));
    }
    return Result<CollisionValidationSuccess>::Err(ErrorCode::kValidationCollision);
  }

  auto collision_trajectory_result =
      collision_projector_.expand_for_collision(trajectory, current_state);
  if (!collision_trajectory_result.ok()) {
    return Result<CollisionValidationSuccess>::Err(std::move(collision_trajectory_result.error()));
  }
  auto collision_trajectory = std::move(collision_trajectory_result.value());

  int available_validation_engines = 0;
  for (const auto& engine : validation_engines) {
    if (engine) {
      ++available_validation_engines;
    }
  }
  if (available_validation_engines == 0) {
    return Result<CollisionValidationSuccess>::Err(
        authoritative_collision_validation_config_error("missing_collision_query_engines"));
  }

  struct AuthoritativeValidationAttempt {
    CollisionWorldSnapshotPtr world;
    std::shared_ptr<const CollisionPolicySnapshot> policy;
    CollisionValidationDecisionBundle bundle;
    ParallelValidationTelemetry telemetry;
    std::optional<std::map<std::string, std::string>> contact_policy_violation;
  };

  std::optional<SceneSnapshotLease> effective_scene_snapshot_lease = scene_snapshot_lease;
  if (!effective_scene_snapshot_lease.has_value() && scene_snapshot_lease_inventory.bundle.world) {
    effective_scene_snapshot_lease = build_scene_snapshot_lease(scene_snapshot_lease_inventory,
                                                                request, sm_.status().execution_id);
  }

  auto run_authoritative_validation_once = [&]() -> Result<AuthoritativeValidationAttempt> {
    CollisionWorldPolicySnapshotBundle world_policy_bundle;
    world_policy_bundle = scene_snapshot_lease_inventory.bundle;
    CollisionWorldSnapshotPtr world_snapshot = world_policy_bundle.world;
    if (!world_snapshot) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error("snapshot_unavailable"));
    }

    auto active_policy = snapshot;
    auto active_policy_index = snapshot_index;
    if (world_policy_bundle.policy) {
      active_policy =
          std::make_shared<CollisionPolicySnapshot>(std::move(*world_policy_bundle.policy));
      active_policy_index = build_collision_policy_index(*active_policy);
    }
    if (!active_policy) {
      return Result<AuthoritativeValidationAttempt>::Err(
          authoritative_collision_validation_config_error("missing_collision_policy_snapshot"));
    }
    if (!active_policy_index) {
      active_policy_index = build_collision_policy_index(*active_policy);
    }

    CollisionValidationPassConfig cfg;
    cfg.policy_snapshot_ref = active_policy;
    cfg.policy_index = active_policy_index;
    cfg.pose_source_retry = retry;
    cfg.per_state_timeout_ns =
        per_state_timeout_ns > 0
            ? per_state_timeout_ns
            : static_cast<std::int64_t>(config_.validation_collision_query_timeout_ns);
    cfg.surface = CollisionSurface::kPreExecutionValidation;
    cfg.contact_policy_id = request.contact_policy_id;

    AuthoritativeValidationAttempt attempt;
    attempt.world = world_snapshot;
    attempt.policy = active_policy;
    attempt.bundle = collision_validation_worker_pool_->validate_trajectory(
        world_snapshot, validation_engines, collision_trajectory, cancel, cfg);
    attempt.telemetry = collision_validation_worker_pool_->last_telemetry();
    attempt.contact_policy_violation =
        contact_policy_violation_details(attempt.bundle, *active_policy, "block_dispatch");
    return Result<AuthoritativeValidationAttempt>::Ok(std::move(attempt));
  };

  auto attempt_result = run_authoritative_validation_once();
  if (!attempt_result.ok()) {
    return Result<CollisionValidationSuccess>::Err(attempt_result.error());
  }

  auto attempt = std::move(attempt_result.value());
  std::size_t dynamically_discovered_start_contact_count =
      request.dynamically_discovered_start_contact_count;
  if (!attempt.bundle.decision.accepted && !attempt.bundle.cancelled && attempt.world &&
      attempt.policy) {
    const auto discovered =
        dynamic_carried_start_contact_pairs(*attempt.world, attempt.bundle, *attempt.policy);
    if (!discovered.empty()) {
      auto overlay = with_dynamic_carried_start_contacts(*attempt.policy, discovered);
      if (!overlay.ok()) {
        return Result<CollisionValidationSuccess>::Err(overlay.error());
      }
      scene_snapshot_lease_inventory.bundle.policy = std::move(overlay.value());
      dynamically_discovered_start_contact_count += discovered.size();
      attempt_result = run_authoritative_validation_once();
      if (!attempt_result.ok()) {
        return Result<CollisionValidationSuccess>::Err(attempt_result.error());
      }
      attempt = std::move(attempt_result.value());
    }
  }
  auto bundle = std::move(attempt.bundle);
  auto telemetry = attempt.telemetry;
  auto contact_policy_violation = std::move(attempt.contact_policy_violation);

  if (bundle.decision.accepted) {
    return Result<CollisionValidationSuccess>::Ok(CollisionValidationSuccess{
        bundle.borrowed_world_revision, attempt.policy->revision, std::move(val_result),
        std::move(bundle.decision), std::move(collision_trajectory),
        std::move(effective_scene_snapshot_lease), telemetry,
        dynamically_discovered_start_contact_count});
  }
  std::map<std::string, std::string> details;
  if (bundle.decision.first_failing_waypoint) {
    details["first_failing_waypoint"] = std::to_string(*bundle.decision.first_failing_waypoint);
    if (*bundle.decision.first_failing_waypoint >= 0 &&
        static_cast<std::size_t>(*bundle.decision.first_failing_waypoint) <
            collision_trajectory.waypoints.size()) {
      const auto first_failure_index =
          static_cast<std::size_t>(*bundle.decision.first_failing_waypoint);
      const auto& waypoint = collision_trajectory.waypoints[first_failure_index];
      details["collision_result_first_failure_joint_names"] =
          comma_join_strings(collision_trajectory.joint_names);
      details["collision_result_first_failure_positions_rad"] =
          comma_join_doubles(waypoint.positions_rad);
      details["collision_result_first_failure_time_from_start_s"] =
          std::to_string(waypoint.time_from_start_s);
    }
  }
  if (bundle.decision.first_failing_time_ns) {
    details["first_failing_time_ns"] = std::to_string(*bundle.decision.first_failing_time_ns);
  }
  details["collision_world_revision_id"] =
      std::to_string(bundle.borrowed_world_revision.revision_id);
  details["collision_world_content_hash"] = bundle.borrowed_world_revision.content_hash;
  details["workers_used"] = std::to_string(telemetry.workers_actually_used);
  details["workers_requested"] = std::to_string(telemetry.workers_requested);
  details["workers_effective"] = std::to_string(telemetry.workers_effective);
  details["ordered_contact_summary_used"] =
      telemetry.ordered_contact_summary_used ? "true" : "false";
  details["dynamically_discovered_start_contact_count"] =
      std::to_string(dynamically_discovered_start_contact_count);
  if (!telemetry.fallback_reason.empty()) {
    details["parallel_fallback_reason"] = telemetry.fallback_reason;
  }
  append_collision_failure_diagnostics(details, bundle);
  if (contact_policy_violation.has_value()) {
    contact_policy_violation->emplace("surface", "pre_dispatch_validation");
    for (auto& [key, value] : *contact_policy_violation) {
      details[key] = std::move(value);
    }
  }
  const char* rejection_error_code = contact_policy_violation.has_value()
                                         ? kErrorContactPolicyViolation
                                         : kErrorValidationCollision;
  if (bundle.cancelled) {
    if (bundle.cancelled_at_waypoint_index) {
      details["cancelled_at_waypoint_index"] = std::to_string(*bundle.cancelled_at_waypoint_index);
    }
    if (bundle.cancellation_reason) {
      details["cancellation_reason"] = *bundle.cancellation_reason;
    }
    details["reason"] = bundle.cancellation_reason &&
                                *bundle.cancellation_reason == "collision_validation_wall_timeout"
                            ? "collision_validation_wall_timeout"
                            : "collision_validation_cancelled";
    return Result<CollisionValidationSuccess>::Err(ErrorDTO{
        kErrorValidationCollision,
        pre_dispatch_collision_validation_message(details, CollisionDecisionAction::kInconclusive),
        std::move(details)});
  }
  if (bundle.decision.action == CollisionDecisionAction::kInconclusive) {
    return Result<CollisionValidationSuccess>::Err(
        ErrorDTO{rejection_error_code,
                 pre_dispatch_collision_validation_message(details, bundle.decision.action),
                 std::move(details)});
  }
  if (config_.enable_collision_diagnostic_compare && collision_diagnostic_comparator_ != nullptr) {
    append_collision_compare_diagnostics(details, bundle, trajectory,
                                         *collision_diagnostic_comparator_);
  }
  return Result<CollisionValidationSuccess>::Err(
      ErrorDTO{rejection_error_code,
               pre_dispatch_collision_validation_message(details, bundle.decision.action),
               std::move(details)});
}

void ExecutionKernel::set_collision_validation_policy_snapshot(
    std::optional<CollisionPolicySnapshot> snapshot) {
  std::scoped_lock lock(coordinator_mutex_);
  if (snapshot) {
    auto shared_snapshot = std::make_shared<CollisionPolicySnapshot>(std::move(*snapshot));
    coordinator_policy_index_ = build_collision_policy_index(*shared_snapshot);
    coordinator_policy_snapshot_ = std::move(shared_snapshot);
    return;
  }
  coordinator_policy_snapshot_.reset();
  coordinator_policy_index_.reset();
}

void ExecutionKernel::set_collision_resources(
    ICollisionWorldSnapshotPublisher& publisher,
    std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines) {
  const auto capacity =
      CollisionResourceManager::inspect_validation_worker_capacity(validation_engines);
  if (config_.validation_collision_workers_declared &&
      (capacity.mismatch_reason ||
       capacity.effective_contexts !=
           static_cast<std::size_t>(config_.validation_collision_workers))) {
    throw std::invalid_argument(
        "declared validation collision worker capacity is not backed by "
        "independent contexts: requested=" +
        std::to_string(config_.validation_collision_workers) +
        " effective=" + std::to_string(capacity.effective_contexts) +
        " reason=" + capacity.mismatch_reason.value_or("count_mismatch"));
  }
  const auto lookahead_capacity =
      CollisionResourceManager::inspect_validation_worker_capacity(runtime_lookahead_engines);
  if ((!runtime_lookahead_engines.empty() &&
       (lookahead_capacity.mismatch_reason ||
        lookahead_capacity.effective_contexts !=
            static_cast<std::size_t>(config_.runtime_collision_lookahead_workers))) ||
      (config_.runtime_collision_lookahead_workers_declared && runtime_lookahead_engines.empty())) {
    throw std::invalid_argument(
        "declared runtime collision lookahead capacity is not backed by "
        "independent contexts: requested=" +
        std::to_string(config_.runtime_collision_lookahead_workers) +
        " effective=" + std::to_string(lookahead_capacity.effective_contexts) + " reason=" +
        lookahead_capacity.mismatch_reason.value_or(
            runtime_lookahead_engines.empty() ? "missing_contexts" : "count_mismatch"));
  }

  CollisionPipelineResources candidate;
  candidate.snapshot_publisher = &publisher;
  candidate.runtime_monitor_engine = runtime_monitor_engine;
  candidate.validation_engines = validation_engines;
  candidate.runtime_lookahead_engines = runtime_lookahead_engines;
  if (const auto conflict =
          CollisionResourceManager::runtime_lookahead_resource_conflict(candidate)) {
    throw std::invalid_argument("runtime collision lookahead resource ownership conflict: " +
                                *conflict);
  }

  std::shared_ptr<RuntimeCollisionLookaheadSession> next_lookahead;
  if (!runtime_lookahead_engines.empty()) {
    const auto* progress = execution_backend_.execution_progress_publication();
    if (progress == nullptr) {
      throw std::invalid_argument(
          "runtime collision lookahead requires backend progress authority");
    }
    RuntimeCollisionLookaheadSession::Config lookahead_config;
    lookahead_config.cycle_budget_ns = config_.runtime_collision_lookahead_cycle_budget_ns;
    lookahead_config.horizon_ns = config_.runtime_collision_lookahead_horizon_ns;
    lookahead_config.max_samples = config_.runtime_collision_lookahead_max_samples;
    lookahead_config.workers = config_.runtime_collision_lookahead_workers;
    lookahead_config.min_waypoints_for_parallel =
        config_.runtime_collision_lookahead_min_waypoints_for_parallel;
    lookahead_config.query_budget_ns = config_.runtime_collision_lookahead_query_budget_ns;
    lookahead_config.dynamic_body_max_age_cap_ns = config_.runtime_dynamic_body_max_age_cap_ns;
    next_lookahead = std::make_shared<RuntimeCollisionLookaheadSession>(
        lookahead_config, clock_, scheduler_, publisher, *progress, runtime_lookahead_engines,
        [this](const RuntimeCollisionLookaheadSession* source_session,
               RuntimeCollisionLookaheadResult result) {
          (void)source_session;
          emit_runtime_collision_lookahead_evidence(std::move(result));
        },
        [this](const RuntimeCollisionLookaheadSession* source_session,
               RuntimeCollisionLookaheadResult& result) {
          return apply_runtime_collision_lookahead_authority(result, source_session);
        });
  }

  collision_resources_.configure(publisher, std::move(runtime_monitor_engine),
                                 std::move(validation_engines),
                                 std::move(runtime_lookahead_engines));
  validation_collision_workers_effective_.store(capacity.effective_contexts,
                                                std::memory_order_relaxed);
  auto previous_lookahead = runtime_collision_lookahead_session_.exchange(
      std::move(next_lookahead), std::memory_order_acq_rel);
  if (previous_lookahead) {
    previous_lookahead->disarm();
  }
  const auto protection = runtime_collision_protection_snapshot();
  if (protection.active) {
    runtime_collision_query_session_.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> error_lock(runtime_collision_query_prepare_error_mutex_);
    runtime_collision_query_prepare_error_ =
        ErrorDTO{kErrorValidationCollision,
                 "Runtime collision resources changed while monitoring was active",
                 {{"reason", "runtime_collision_resources_changed_while_active"}}};
  }
  std::map<std::string, std::string> ignored_details;
  (void)refresh_published_runtime_collision_snapshot(&ignored_details);
}

std::optional<ErrorDTO> ExecutionKernel::collision_validation_admission_error() const {
  const auto resources = collision_resources_.snapshot();
  const auto protection = runtime_collision_protection_snapshot();
  const auto admission = CollisionResourceManager::validate_admission(resources, protection.active);
  if (admission.admitted()) {
    return std::nullopt;
  }
  return validation_admission_error(sm_.status().state, resources, admission, protection);
}

void ExecutionKernel::set_collision_validation_retry_config(PoseSourceRetryConfig retry,
                                                            std::int64_t per_state_timeout_ns) {
  std::scoped_lock lock(coordinator_mutex_);
  coordinator_retry_config_ = retry;
  coordinator_per_state_timeout_ns_ = per_state_timeout_ns;
}

void ExecutionKernel::begin_runtime_velocity_scale_freeze() {
  std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
  ++runtime_velocity_scale_freeze_depth_;
}

void ExecutionKernel::end_runtime_velocity_scale_freeze() {
  std::optional<double> deferred_scale;
  std::optional<std::string> deferred_principal_id;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (runtime_velocity_scale_freeze_depth_ <= 0) {
      return;
    }
    --runtime_velocity_scale_freeze_depth_;
    if (runtime_velocity_scale_freeze_depth_ == 0 && deferred_runtime_velocity_scale_.has_value()) {
      deferred_scale = deferred_runtime_velocity_scale_;
      deferred_principal_id = deferred_runtime_velocity_scale_principal_id_;
      deferred_runtime_velocity_scale_.reset();
      deferred_runtime_velocity_scale_principal_id_.reset();
    }
  }
  if (deferred_scale.has_value()) {
    set_runtime_velocity_scale(*deferred_scale, deferred_principal_id);
  }
}

void ExecutionKernel::set_runtime_velocity_scale(double scale,
                                                 std::optional<std::string> principal_id) {
  // ADR-0018: UI runtime velocity scale → backend runtime register.
  // Validate finiteness here; clamp to [0, 1]; forward to the backend's
  // atomic store. No lifecycle_mutex_: this is a high-rate operator surface
  // and the backend's atomic store handles concurrency.
  if (!std::isfinite(scale)) {
    // K2-style defense in depth: refuse NaN/Inf rather than
    // propagating into the worker.
    return;
  }
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  const auto status = sm_.status();
  double previous = 1.0;
  bool deferred = false;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    previous = runtime_velocity_scale_.load(std::memory_order_acquire);
    if (runtime_velocity_scale_freeze_depth_ > 0) {
      deferred_runtime_velocity_scale_ = scale;
      deferred_runtime_velocity_scale_principal_id_ = principal_id;
      deferred = true;
    } else {
      runtime_velocity_scale_.store(scale, std::memory_order_release);
    }
  }
  if (!deferred) {
    execution_backend_.set_runtime_velocity_scale(scale);
  }
  std::map<std::string, std::string> details{
      {"previous_scale", format_double(previous)},
      {"new_scale", format_double(scale)},
      {"applied_scale", format_double(deferred ? previous : scale)},
      {"backend_status", deferred ? "deferred_controlled_stop_freeze" : "forwarded"},
      {"source", "kernel_api"},
      {"state", to_string(status.state)}};
  if (principal_id.has_value() && !principal_id->empty()) {
    details["principal_id"] = *principal_id;
  }
  emit_event({KernelEventType::kRuntimeVelocityScaleChanged,
              status.execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              std::move(details)});
}

namespace {

[[nodiscard]] bool resolved_scale_target_matches(const ResolvedRuntimeScaleCommandDTO& command,
                                                 const ResolvedExecutionBindingDTO& binding) {
  if (!binding.runtime_scale_qualification.has_value()) {
    return false;
  }
  const auto& qualification = *binding.runtime_scale_qualification;
  return command.operation_id == binding.operation_id &&
         command.participant_id == binding.participant_id &&
         command.controllable_component_id == binding.controllable_component_id &&
         command.command_target_id == binding.command_target_id &&
         command.capability_id == binding.capability_id &&
         command.control_domain_id == binding.control_domain_id &&
         command.control_domain_revision == binding.control_domain_revision &&
         command.control_binding_id == binding.control_binding_id &&
         command.control_binding_revision == binding.control_binding_revision &&
         command.controller_goal_id == binding.controller_goal_id &&
         command.controller_goal_generation == binding.controller_goal_generation &&
         command.controller_topology_id == binding.controller_topology_id &&
         command.topology_revision == binding.topology_revision &&
         command.control_route_id == binding.control_route_id &&
         command.control_route_id == qualification.control_route_id &&
         command.scaling_domain_id == qualification.scaling_domain_id &&
         command.coordinated_barrier_id == qualification.coordinated_barrier_id &&
         command.resources == binding.resources &&
         command.active_control_configuration_id == binding.active_control_configuration_id &&
         command.active_control_configuration_generation ==
             binding.active_control_configuration_generation;
}

[[nodiscard]] bool resolved_scale_command_well_formed(
    const ResolvedRuntimeScaleCommandDTO& command) {
  return !command.operation_id.empty() && command.operation_revision.nonzero() &&
         !command.participant_id.empty() && !command.controllable_component_id.empty() &&
         !command.command_target_id.empty() && !command.capability_id.empty() &&
         !command.control_domain_id.empty() && !command.control_domain_revision.empty() &&
         !command.control_binding_id.empty() && !command.control_binding_revision.empty() &&
         !command.controller_goal_id.empty() && command.controller_goal_generation.nonzero() &&
         !command.controller_topology_id.empty() && !command.topology_revision.empty() &&
         !command.control_route_id.empty() && !command.scaling_domain_id.empty() &&
         !command.resources.empty() && !command.active_control_configuration_id.empty() &&
         command.active_control_configuration_generation.nonzero() &&
         std::isfinite(command.requested_scale) && command.requested_scale >= 0.0 &&
         command.requested_scale <= 1.0;
}

[[nodiscard]] ResolvedRuntimeScaleAcceptedDTO make_scale_acceptance(
    const ResolvedRuntimeScaleCommandDTO& command, std::uint64_t sequence,
    std::int64_t accepted_at_ns) {
  return ResolvedRuntimeScaleAcceptedDTO{command.operation_id,
                                         command.operation_revision,
                                         command.participant_id,
                                         command.controllable_component_id,
                                         command.command_target_id,
                                         command.capability_id,
                                         command.control_domain_id,
                                         command.control_domain_revision,
                                         command.control_binding_id,
                                         command.control_binding_revision,
                                         command.controller_goal_id,
                                         command.controller_goal_generation,
                                         command.controller_topology_id,
                                         command.topology_revision,
                                         command.control_route_id,
                                         command.scaling_domain_id,
                                         command.coordinated_barrier_id,
                                         command.resources,
                                         command.active_control_configuration_id,
                                         command.active_control_configuration_generation,
                                         command.requested_runtime_scale_generation,
                                         command.requested_scale,
                                         sequence,
                                         accepted_at_ns};
}

}  // namespace

Result<ResolvedRuntimeScaleAcceptedDTO> ExecutionKernel::set_runtime_scale(
    const ResolvedRuntimeScaleCommandDTO& command) {
  std::lock_guard submission_lock(resolved_runtime_scale_submission_mutex_);
  if (!resolved_scale_command_well_formed(command)) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "resolved runtime-scale command is malformed",
                 {{"reason", "resolved_runtime_scale_command_invalid"}}});
  }
  const auto binding = active_correlation_.resolved_binding();
  if (!binding.has_value() || !resolved_scale_target_matches(command, *binding)) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                 "resolved runtime-scale command does not match the active binding",
                 {{"reason", "resolved_runtime_scale_binding_mismatch"}}});
  }
  const auto state = sm_.status().state;
  if (state == ExecutionState::kStopping || state == ExecutionState::kStopped ||
      state == ExecutionState::kCompleted || state == ExecutionState::kFailed ||
      state == ExecutionState::kInterrupted || state == ExecutionState::kInterventionRequired) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                 "runtime scale is not admissible in the current execution state",
                 {{"reason", "runtime_scale_state_invalid"}, {"state", to_string(state)}}});
  }
  const auto mechanism = execution_backend_.runtime_velocity_scale_status();
  if (!mechanism.supported) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kUnsupportedCapability)),
                 "execution mechanism has no qualified runtime-scale status",
                 {{"reason", "runtime_scale_mechanism_unsupported"}}});
  }
  if (mechanism.error.has_value()) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(*mechanism.error);
  }

  ResolvedRuntimeScaleAcceptedDTO accepted = make_scale_acceptance(command, 0U, 0);
  {
    std::lock_guard scale_lock(resolved_runtime_scale_mutex_);
    if (accepted_resolved_runtime_scale_command_.has_value() &&
        accepted_resolved_runtime_scale_command_->operation_id == command.operation_id) {
      if (*accepted_resolved_runtime_scale_command_ == command &&
          accepted_resolved_runtime_scale_result_.has_value()) {
        return Result<ResolvedRuntimeScaleAcceptedDTO>::Ok(
            *accepted_resolved_runtime_scale_result_);
      }
      const auto previous_generation =
          accepted_resolved_runtime_scale_command_->requested_runtime_scale_generation;
      if (command.expected_runtime_scale_generation != previous_generation ||
          previous_generation.value() == std::numeric_limits<std::uint64_t>::max() ||
          command.requested_runtime_scale_generation.value() != previous_generation.value() + 1U ||
          command.operation_revision <
              accepted_resolved_runtime_scale_command_->operation_revision) {
        return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
            ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                     "resolved runtime-scale generation or operation revision is stale",
                     {{"reason", "resolved_runtime_scale_generation_stale"}}});
      }
    } else if (command.expected_runtime_scale_generation.value() != 0U ||
               command.requested_runtime_scale_generation.value() != 0U) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                   "a new operation must initialize runtime scale at generation zero",
                   {{"reason", "resolved_runtime_scale_initial_generation_invalid"}}});
    }
    if (runtime_scale_acceptance_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInternalError)),
                   "runtime-scale acceptance sequence is exhausted",
                   {{"reason", "runtime_scale_acceptance_sequence_exhausted"}}});
    }
    accepted = make_scale_acceptance(command, ++runtime_scale_acceptance_sequence_, clock_.now());
    accepted_resolved_runtime_scale_command_ = command;
    accepted_resolved_runtime_scale_result_ = accepted;
  }

  set_runtime_velocity_scale(command.requested_scale);
  return Result<ResolvedRuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
}

Result<ResolvedRuntimeScaleStatusDTO> ExecutionKernel::runtime_scale_status(
    const OperationId& operation_id, const ControlBindingId& control_binding_id) const {
  if (operation_id.empty() || control_binding_id.empty()) {
    return Result<ResolvedRuntimeScaleStatusDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "runtime-scale status query has an empty exact identity",
                 {{"reason", "runtime_scale_status_query_invalid"}}});
  }
  const auto binding = active_correlation_.resolved_binding();
  if (!binding.has_value() || binding->operation_id != operation_id ||
      binding->control_binding_id != control_binding_id) {
    return Result<ResolvedRuntimeScaleStatusDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                 "runtime-scale status query does not name the active binding",
                 {{"reason", "runtime_scale_status_binding_not_active"}}});
  }
  std::optional<ResolvedRuntimeScaleCommandDTO> command;
  {
    std::lock_guard scale_lock(resolved_runtime_scale_mutex_);
    if (!accepted_resolved_runtime_scale_command_.has_value() ||
        accepted_resolved_runtime_scale_command_->operation_id != operation_id ||
        accepted_resolved_runtime_scale_command_->control_binding_id != control_binding_id) {
      return Result<ResolvedRuntimeScaleStatusDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                   "active binding has no accepted runtime-scale generation",
                   {{"reason", "runtime_scale_status_not_initialized"}}});
    }
    command = *accepted_resolved_runtime_scale_command_;
  }

  auto mechanism = execution_backend_.runtime_velocity_scale_status();
  RuntimeScaleApplicationState application_state = RuntimeScaleApplicationState::kPending;
  if (!mechanism.supported) {
    application_state = RuntimeScaleApplicationState::kRejected;
    if (!mechanism.error.has_value()) {
      mechanism.error = ErrorDTO{std::string(to_string(ErrorCode::kUnsupportedCapability)),
                                 "execution mechanism no longer reports runtime-scale support",
                                 {{"reason", "runtime_scale_mechanism_unsupported"}}};
    }
  } else if (mechanism.error.has_value()) {
    application_state = RuntimeScaleApplicationState::kFailed;
  } else if (mechanism.active && mechanism.acknowledgement_sequence != 0U &&
             std::abs(mechanism.commanded_scale - command->requested_scale) <= 1e-12 &&
             std::abs(mechanism.applied_scale - command->requested_scale) <= 1e-9) {
    application_state = RuntimeScaleApplicationState::kApplied;
  }

  return Result<ResolvedRuntimeScaleStatusDTO>::Ok(
      ResolvedRuntimeScaleStatusDTO{command->operation_id,
                                    command->operation_revision,
                                    command->participant_id,
                                    command->controllable_component_id,
                                    command->command_target_id,
                                    command->capability_id,
                                    command->control_domain_id,
                                    command->control_domain_revision,
                                    command->control_binding_id,
                                    command->control_binding_revision,
                                    command->controller_goal_id,
                                    command->controller_goal_generation,
                                    command->controller_topology_id,
                                    command->topology_revision,
                                    command->control_route_id,
                                    command->scaling_domain_id,
                                    command->coordinated_barrier_id,
                                    command->resources,
                                    command->active_control_configuration_id,
                                    command->active_control_configuration_generation,
                                    command->requested_runtime_scale_generation,
                                    mechanism.commanded_scale,
                                    mechanism.applied_scale,
                                    mechanism.virtual_trajectory_time_s,
                                    binding->provider_runtime_epoch,
                                    mechanism.acknowledgement_sequence,
                                    mechanism.acknowledgement_timestamp_ns,
                                    application_state,
                                    mechanism.error});
}

}  // namespace manyforge::core
