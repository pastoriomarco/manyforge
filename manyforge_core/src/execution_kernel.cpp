#include <manyforge_core/io_rule_stop_response.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/cache/trajectory_cache_compatibility.hpp>
#include <manyforge_core/cache/trajectory_cache_intent.hpp>
#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/execution_admission_support.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/motion_target_resolution.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/spring_back_return_point.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/trajectory/trajectory_splice.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>
#include <manyforge_core/types/kernel_event_factories.hpp>
#include <manyforge_core/types/reason_codes.hpp>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace manyforge::core {
namespace {

[[nodiscard]] bool resolved_scale_target_matches(const ResolvedRuntimeScaleCommandDTO& command,
                                                 const ResolvedExecutionBindingDTO& binding);
[[nodiscard]] bool resolved_scale_command_well_formed(
    const ResolvedRuntimeScaleCommandDTO& command);
[[nodiscard]] ResolvedRuntimeScaleAcceptedDTO make_scale_acceptance(
    const ResolvedRuntimeScaleCommandDTO& command, std::uint64_t sequence,
    std::int64_t accepted_at_ns);

std::optional<std::int64_t> motion_replacement_elapsed_ns(std::int64_t now,
                                                          std::int64_t started) noexcept {
  if (now < started) return std::nullopt;
  const auto elapsed = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(started);
  if (elapsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  return static_cast<std::int64_t>(elapsed);
}

void trace_moving_core_phase(const MovingReplacementInputDTO* replacement,
                             const char* phase, std::int64_t started,
                             std::int64_t finished, std::size_t points) {
  if (!replacement) return;
  const auto written = std::fprintf(stderr,
      "manyforge_moving_core_phase source_goal=%.160s phase=%s started_ns=%lld finished_ns=%lld elapsed_ns=%lld points=%zu\n",
      replacement->source_goal.controller_goal_id.value().c_str(), phase,
      static_cast<long long>(started), static_cast<long long>(finished),
      static_cast<long long>(finished - started), points);
  static_cast<void>(written);
}

void resolve_motion_execution_timeout(ExecuteRequestDTO& request, const TrajectoryDTO& trajectory) {
  if (!request.trajectory_relative_timeout) return;
  const double duration =
      trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
  const double budget =
      std::max({10.0, request.timeouts.execution_timeout_s, 1.5 * duration + 2.0});
  if (!std::isfinite(duration) || duration < 0.0 || !std::isfinite(budget) ||
      budget >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1e9) {
    throw std::invalid_argument("Manual trajectory completion budget is not representable");
  }
  request.timeouts.execution_timeout_s = budget;
}

ErrorDTO stale_control_command_error() {
  return ErrorDTO{kErrorInvalidRequest,
                  "Control command was superseded",
                  {{"reason_code", "stale_control_command"}}};
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

void update_atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
  }
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
      !valid_program_contact_envelope(binding.envelope) || !inventory.logical_scene ||
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
  const auto* grasp = std::get_if<ContactGraspEnvelopeDTO>(&binding.envelope);
  if (grasp != nullptr) {
    effective.contact_grasps.push_back(ContactGraspPolicyEntry{
        effective_id, binding.selected_object_id.value(), std::move(allowed_body_ids),
        grasp->max_penetration_m, grasp->terminal_window_start_fraction});
  } else {
    const auto& release = std::get<ContactReleaseEnvelopeDTO>(binding.envelope);
    for (const auto& body_id : allowed_body_ids) {
      for (std::size_t index = 0; index < object->collision_shapes.size(); ++index) {
        SupportContactPolicyEntry entry;
        entry.pair = {body_id, make_scene_collision_body_id(binding.selected_object_id, index)};
        if (entry.pair.body_b < entry.pair.body_a)
          std::swap(entry.pair.body_a, entry.pair.body_b);
        entry.release_evidence_mode =
            release.release_evidence_mode == ContactReleaseEvidenceModeDTO::kSignedPenetration
                ? ContactReleaseEvidenceMode::kSignedPenetration
                : ContactReleaseEvidenceMode::kBinaryContact;
        entry.enforce_max_start_penetration = release.enforce_max_start_penetration;
        entry.max_start_penetration_m = release.max_start_penetration_m;
        entry.release_tolerance_m = release.release_tolerance_m;
        entry.min_release_progress_m = release.min_release_progress_m;
        entry.require_separation_by_end = release.require_separation_by_end;
        entry.require_strictly_decreasing_penetration =
            release.require_strictly_decreasing_penetration;
        entry.allow_recontact = release.allow_recontact;
        const auto existing = std::find_if(
            effective.support_contacts.begin(), effective.support_contacts.end(),
            [&](const auto& candidate) { return candidate.pair == entry.pair; });
        if (existing != effective.support_contacts.end()) {
          if (*existing == entry) continue;
          return Result<void>::Err(ErrorDTO{kErrorInvalidContactPolicy,
              "Resolved release conflicts with an existing contact pair",
              {{"reason", "resolved_contact_release_pair_conflict"}}});
        }
        if (effective.support_contacts.size() >= RuntimeCollisionQuerySession::kMaxSupportContacts)
          return Result<void>::Err(ErrorDTO{kErrorInvalidContactPolicy,
              "Resolved release exceeds the runtime contact bound",
              {{"reason", "resolved_contact_release_capacity_exceeded"}}});
        effective.support_contacts.push_back(std::move(entry));
      }
    }
    std::sort(effective.support_contacts.begin(), effective.support_contacts.end(),
              [](const auto& left, const auto& right) { return left.pair < right.pair; });
  }
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
  // This copied request field selects only a grasp overlay. Release uses the
  // exact support-contact pairs above; authored identity remains in the resolved
  // binding and original operation intent / prepared contact-policy reference.
  request.contact_policy_id = grasp != nullptr ? effective_id : std::string{};
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
      {"execution_generation", std::to_string(result.execution_generation)}};
  details.insert(result.checked_input_details.begin(), result.checked_input_details.end());
  if (result.job_context_observed) {
    details["lookahead_requested_at_ns"] = std::to_string(result.requested_at_ns);
    details["lookahead_cycle_deadline_ns"] = std::to_string(result.cycle_deadline_ns);
    details["lookahead_start_jitter_ns"] = std::to_string(result.start_jitter_ns);
    details["lookahead_cycle_duration_ns"] = std::to_string(result.cycle_duration_ns);
    details["lookahead_cycle_deadline_exceeded"] =
        result.cycle_deadline_exceeded ? "true" : "false";
  }
  if (result.selection_computed) {
    details["lookahead_covered_until_time_ns"] = std::to_string(result.covered_until_time_ns);
    details["lookahead_checked_samples"] = std::to_string(result.checked_samples);
    details["lookahead_termination"] =
        runtime_collision_lookahead_termination_name(result.termination);
  }
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

struct ExecutionKernel::ValidatedMotionPlan final {
  PlanningSuccess planning;
  CollisionValidationSuccess validation;
};

enum class PreparedMotionDispatchGate : std::uint8_t {
  kOpen = 0,
  kFencedBeforeDispatch,
  kCommitted,
};

struct ExecutionKernel::PreparedMotionSession final {
  explicit PreparedMotionSession(ResolvedPreparedMotionExecutionRequestDTO supplied)
      : authority(std::move(supplied)) {}

  ResolvedPreparedMotionExecutionRequestDTO authority;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::uint64_t execution_id{};
  std::uint64_t backend_execution_generation{};
  std::shared_ptr<const PreparedOperationParticipantDTO> last_dispatched_participant;
  ResolvedPreparedMotionExecutionStateDTO state{
      ResolvedPreparedMotionExecutionStateDTO::kPreparing};
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ErrorDTO> error;
  std::atomic<PreparedMotionDispatchGate> dispatch_gate{PreparedMotionDispatchGate::kOpen};
  bool dispatch_requested{};
  bool cancel_requested{};
  bool conclusive_collision_failure{};

  // Only a replacement session uses these staged values. The existing worker
  // remains the sole physical observer/presenter throughout computation.
  std::int64_t preparation_started_ns{};
  std::int64_t preparation_deadline_ns{};
  bool preparation_completed{};
  bool worker_parked{};
  bool replacement_failure_notified{};
  std::stop_source replacement_cancellation;
  std::shared_ptr<CollisionCancellationToken> replacement_validation_cancellation;
  std::unique_ptr<ActiveMotionExecution> replacement_execution;
  std::unique_ptr<ExecutionCorrelationTracker> replacement_correlation;
  std::shared_ptr<const TrajectoryDTO> replacement_collision_trajectory;
  std::optional<RuntimeCollisionLookaheadSession::PreparedArm> replacement_lookahead_arm;
  std::optional<RuntimeCollisionStateProjection> replacement_projection;
  std::optional<PreparedMotionReplacementObservationDTO> replacement_observation;
  std::optional<StopCertificateRefDTO> spring_activation_certificate;

  // Plain work/result under this session's existing task and cancellation.
  // None of these fields are backend membership or physical effect proof.
  struct SpringColdProduct final {
    ResolvedPreparedMotionExecutionRequestDTO authority;
    std::uint64_t reserved_execution_id{};
    ResolvedPreparedMotionExecutionStateDTO state{
        ResolvedPreparedMotionExecutionStateDTO::kPreparing};
    std::shared_ptr<const PreparedOperationParticipantDTO> participant;
    std::unique_ptr<ActiveMotionExecution> execution;
    std::shared_ptr<const TrajectoryDTO> collision_trajectory;
    std::optional<ErrorDTO> error;
    bool cancelled{};
  };
  std::optional<SpringColdProduct> spring_cold_product;
  struct SpringPromotionReceipt final {
    std::uint64_t abandoned_id{};
    std::uint64_t promoted_id{};
    StopCertificateRefDTO certificate;
  };
  std::optional<SpringPromotionReceipt> spring_promotion_receipt;

  /// Caller holds mutex. The failed execution fact is checked by the caller.
  [[nodiscard]] bool never_dispatched_for(std::uint64_t expected_execution_id) const {
    return execution_id == expected_execution_id && !dispatch_requested &&
           !last_dispatched_participant &&
           (state == ResolvedPreparedMotionExecutionStateDTO::kPreparing ||
            state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed);
  }
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
      planning_scene_synchronizer_(ports.planning_scene_synchronizer),
      execution_backend_(execution_backend),
      clock_(clock),
      scheduler_(scheduler),
      cache_(cache),
      config_(std::move(config)),
      sm_(clock),
      runtime_collision_monitor_(
          clock_, scheduler_, RuntimeCollisionMonitor::Config{config_.collision_monitor_period_ns},
          [this]() { return collision_check_tick(); },
          [this]() {
            const auto status = sm_.status();
            return status.state == ExecutionState::kExecuting && !status.fact.has_value();
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
  if (replacement_preparation_thread_.joinable()) {
    replacement_preparation_thread_.request_stop();
    replacement_preparation_thread_.join();
  }
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

  // Scheduled collision callbacks capture `this`; cancel them before any
  // member teardown so destruction cannot race a monitor tick.
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown, std::nullopt,
                            /*drain_callbacks=*/true);
  drain_scheduler_or_terminate("collision_monitor_stop");

  // Join before member destruction so worker callbacks cannot notify the
  // condition variable after it has been destroyed. This is intentionally
  // explicit instead of relying on std::jthread member teardown order.
  if (worker_thread_.joinable()) {
    worker_thread_.request_stop();
    worker_signals_.abandon();
    worker_thread_.join();
  }

  // RT-8B freshness deadlines capture `this`. A pending deadline is not
  // covered by scheduler drain (drain only waits for already-dequeued work),
  // and the execute worker may have replaced the handle after the earlier
  // drain. Cancel after every kernel-owned worker is joined, then drain once
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

bool ExecutionKernel::settle_execution(const std::uint64_t exec_id, const std::string& move_id,
                                       ParticipantExecutionFact fact, const char* reason,
                                       std::optional<ExecutionSnapshot> final_snapshot) {
  const auto phase = sm_.status().state;
  const auto kind = fact.kind;
  const auto effect = fact.effect;
  const auto error = fact.error;
  if (!sm_.settle(fact).ok()) {
    return false;
  }
  // RT-8B: an ended execution changes the recovery inputs. This function is
  // reached both with and without lifecycle_mutex_ held, and std::mutex
  // try_lock by the owning thread is UB; enqueue a coalesced refresh.
  recovery_assessment_dirty_.store(true, std::memory_order_release);
  schedule_recovery_assessment_refresh("participant_settled");
  auto snapshot = final_snapshot.has_value() ? std::move(*final_snapshot) : get_execution_snapshot();
  snapshot.timestamp_ns = clock_.now();
  snapshot.execution_id = exec_id;
  snapshot.move_id = move_id;
  snapshot.state = phase;
  snapshot.error = error;
  if (kind == ParticipantExecutionFactKind::kComputedCompletion) {
    snapshot.progress = 1.0;
  }
  publish_execution_snapshot(std::move(snapshot));
  {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    if (kind == ParticipantExecutionFactKind::kComputedCompletion) {
      metrics_.executions_completed++;
    } else if (kind == ParticipantExecutionFactKind::kLocalFailure && phase == ExecutionState::kExecuting) {
      metrics_.executions_failed++;
    }
  }
  KernelEvent event{KernelEventType::kParticipantSettled,
                    exec_id,
                    clock_.now(),
                    phase,
                    phase,
                    error,
                    {{"reason", reason},
                     {"participant_fact", to_string(kind)},
                     {"participant_effect", to_string(effect)}}};
  if (kind == ParticipantExecutionFactKind::kLocalFailure) {
    RecoveryAuthorityCapture failed;
    failed.execution_id = exec_id;
    failed.execution_state = ExecutionState::kFailed;
    failed.failed_execution_effect = effect;
    if (error.has_value()) failed.failed_validation_code = error->code;
    std::shared_ptr<PreparedMotionSession> session;
    {
      std::lock_guard lock(prepared_motion_session_mutex_);
      session = active_prepared_motion_session_;
    }
    if (session) {
      std::lock_guard lock(session->mutex);
      failed.failed_preparation_never_dispatched = session->never_dispatched_for(exec_id);
      failed.failed_validation_conclusive_collision =
          failed.failed_preparation_never_dispatched && session->conclusive_collision_failure;
    }
    failed.failed_validation_retryable = failed.failed_preparation_never_dispatched;
    event.retry_eligibility = planning_retry_eligibility(failed);
  }
  event.participant_fact = std::move(fact);
  emit_event(std::move(event));
  return true;
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

Result<std::optional<ProjectionRevisionDTO>> ExecutionKernel::synchronize_scene_before_planning() {
  if (planning_scene_synchronizer_ == nullptr) {
    return Result<std::optional<ProjectionRevisionDTO>>::Ok(std::nullopt);
  }
  try {
    auto synchronized = planning_scene_synchronizer_->SynchronizeBeforePlanning();
    if (!synchronized.ok()) {
      return Result<std::optional<ProjectionRevisionDTO>>::Err(synchronized.error());
    }
    if (!synchronized.value().valid()) {
      return Result<std::optional<ProjectionRevisionDTO>>::Err(
          ErrorDTO{kErrorExecutionRejected,
                   "Planning-scene synchronization returned an invalid projection",
                   {{"reason", "planning_scene_synchronization_invalid"}}});
    }
    return Result<std::optional<ProjectionRevisionDTO>>::Ok(std::move(synchronized.value()));
  } catch (const std::exception& error) {
    return Result<std::optional<ProjectionRevisionDTO>>::Err(ErrorDTO{
        kErrorExecutionRejected,
        "Planning-scene synchronization raised an exception",
        {{"reason", "planning_scene_synchronization_exception"}, {"exception", error.what()}}});
  } catch (...) {
    return Result<std::optional<ProjectionRevisionDTO>>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Planning-scene synchronization raised a non-standard exception",
                 {{"reason", "planning_scene_synchronization_exception"},
                  {"exception", reason_codes::kUnknownNonStdException}}});
  }
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
    const CollisionWorldLeaseInventory& inventory, std::uint64_t exec_id) const {
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
  const auto body_hashes = body_hashes_by_id(inventory.bundle.world);
  lease.objects.reserve(inventory.live_objects.size());
  for (const auto& live : inventory.live_objects) {
    if (live.object_id.empty() || live.collision_body_ids.empty()) {
      continue;
    }
    SceneSnapshotLeaseObject object;
    object.object_id = live.object_id;
    object.collision_body_ids = live.collision_body_ids;
    object.object_revision_token = live.object_revision_token;
    object.pose = live.pose;
    tighten_lease_age(lease.lease_max_age_ns, live.lease_max_age_ns);
    for (const auto& body_id : object.collision_body_ids) {
      const auto it = body_hashes.find(body_id);
      if (it != body_hashes.end()) {
        object.body_content_hashes[body_id] = it->second;
      }
    }
    lease.objects.push_back(std::move(object));
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
    context.record_object_divergence = config_.record_scene_divergence_detail;
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

ExecutionKernel::CollisionMonitoringStart ExecutionKernel::start_collision_monitoring(
    std::string reason, const CollisionWorldLeaseInventory* admission_inventory) {
  std::uint64_t reused_generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    const auto active_session = runtime_collision_query_session_.load(std::memory_order_acquire);
    if (runtime_collision_protection_.active && active_session) {
      // An active protection window stays continuous: reuse the prepared
      // session so there is no unmonitored engine-rebuild gap and
      // contact-release history cannot be re-baselined.
      runtime_collision_protection_.generation = ++runtime_collision_protection_next_generation_;
      runtime_collision_protection_.execution_id = sm_.status().execution_id;
      runtime_collision_protection_.state_at_acquire = sm_.status().state;
      runtime_collision_protection_.reason = std::move(reason);
      reused_generation = runtime_collision_protection_.generation;
      runtime_collision_monitor_.start();
    }
  }
  if (reused_generation != 0) {
    return {reused_generation, std::nullopt};
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
    // A failed preparation never arms the monitor: the caller stops this
    // generation fail-closed and does not dispatch.
    if (!query_prepare_error) {
      runtime_collision_monitor_.start();
    }
  }
  return {generation, std::move(query_prepare_error)};
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
  bool invalid_authority = false;
  {
    std::lock_guard<std::mutex> lock(runtime_collision_protection_mutex_);
    // Select the session under the same authority lock as the arm decision.
    // Teardown retires the session under this lock, so copying it beforehand
    // could let a dispatch arm a session after retirement had begun.
    const auto lookahead = runtime_collision_lookahead_session_.load(std::memory_order_acquire);
    if (!lookahead) {
      return;
    }
    const auto status = sm_.status();
    if (!runtime_collision_protection_.active ||
        runtime_collision_protection_.execution_id != execution_id ||
        status.state != ExecutionState::kExecuting || status.fact.has_value()) {
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
    const auto status = sm_.status();
    if (!runtime_collision_protection_.active ||
        runtime_collision_protection_.execution_id != result.execution_id ||
        addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == result.execution_id ||
        status.state != ExecutionState::kExecuting || status.fact.has_value()) {
      return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
    }
    if (result.arm_generation != 0 &&
        execution_backend_.claim_execution_progress_hazard(result.execution_generation) !=
            ExecutionProgressHazardClaimResult::kClaimed) {
      return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
    }
    cache_.invalidate_all();
    // The collision is a participant fact. The addressed Kernel resolves and
    // accepts the containment Stop (or escalates an active one) from this
    // synchronous event; the participant keeps no local fault latch.
    auto handoff_details = authority_details;
    handoff_details["surface"] = "addressed_runtime_collision_authority";
    handoff_details["addressed_stop_authority_handoff"] = "true";
    handoff_details["in_collision"] = check.in_collision ? "true" : "false";
    KernelEvent handoff_event;
    handoff_event.type = KernelEventType::kCollisionDetected;
    handoff_event.execution_id = result.execution_id;
    handoff_event.timestamp_ns = clock_.now();
    handoff_event.details = std::move(handoff_details);
    emit_event(std::move(handoff_event));
    lock.unlock();
    event_scope.flush();
  }

  // The synchronous handoff linearized the addressed Stop when the owner
  // accepted it; this result then belongs to the replaced trajectory and is
  // not repeated as telemetry evidence.
  if (addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == result.execution_id) {
    return RuntimeCollisionLookaheadAuthorityDisposition::kStale;
  }
  result.stop_authority_signalled = true;
  result.authority_rejected = false;
  result.authority_rejection_reason.clear();
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
  if (result.selection_computed) {
    details["lookahead_eligible_source_waypoints"] = std::to_string(result.eligible_source_waypoints);
    details["lookahead_sampling_reduced"] = result.sampling_reduced ? "true" : "false";
    details["lookahead_sample_times_ns"] = std::move(sampled_times_ns);
  }
  if (result.validation_completed) {
    details["lookahead_workers_used"] = std::to_string(result.workers_used);
  }
  if (result.job_context_observed) {
    details["lookahead_worker_active_duration_ns"] = std::to_string(result.worker_active_duration_ns);
  }
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
  schedule_recovery_assessment_refresh("terminal_cleanup_completed");
}

Result<void> ExecutionKernel::retire_prepared_motion_membership(
    const PreparedMotionSession* preserve_pending) {
  // Called with lifecycle serialization after the execution worker has joined
  // and the owning cleanup has established settlement. Never a Stop callback.
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  std::shared_ptr<PreparedMotionSession> pending;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    pending = pending_prepared_motion_session_;
  }
  if (pending && pending.get() != preserve_pending) {
    if (pending->backend_prepare_result &&
        pending->backend_prepare_result->state == PrepareParticipantResultStateDTO::kPrepared) {
      const auto retired_pending = execution_backend_.RetirePreparedExecution(pending->execution_id);
      if (!retired_pending.ok()) return retired_pending;
    }
    const auto released =
        execution_backend_.EndReplacementPreparationScaleHold(pending->execution_id);
    if (!released.ok()) return released;
    std::lock_guard lock(prepared_motion_session_mutex_);
    if (pending_prepared_motion_session_ == pending) pending_prepared_motion_session_.reset();
  }
  if (!session) return Result<void>::Ok();
  const auto retired = execution_backend_.RetirePreparedExecution(session->execution_id);
  if (!retired.ok()) return retired;
  const auto released =
      execution_backend_.EndReplacementPreparationScaleHold(session->execution_id);
  if (!released.ok()) return released;
  std::lock_guard lock(prepared_motion_session_mutex_);
  if (active_prepared_motion_session_ == session) active_prepared_motion_session_.reset();
  return Result<void>::Ok();
}

void ExecutionKernel::notify_execution_waiters() {
  worker_signals_.notify();
}

bool ExecutionKernel::retire_execute_worker(std::unique_lock<std::mutex>& lifecycle_lock,
                                            const bool containment_already_certified) {
  if (!worker_thread_.joinable()) {
    return true;
  }
  certified_containment_retirement_.store(containment_already_certified, std::memory_order_release);
  if (worker_thread_.get_id() == std::this_thread::get_id()) {
    worker_thread_.request_stop();
    worker_signals_.notify();
    return false;
  }

  lifecycle_worker_retirement_in_progress_.store(true, std::memory_order_release);
  {
    std::lock_guard owner_lock(prepared_motion_session_mutex_);
    if (pending_prepared_motion_session_) {
      pending_prepared_motion_session_->replacement_cancellation.request_stop();
    }
  }
  if (replacement_preparation_thread_.joinable()) replacement_preparation_thread_.request_stop();
  worker_thread_.request_stop();
  worker_signals_.notify();
  lifecycle_lock.unlock();
  if (replacement_preparation_thread_.joinable()) replacement_preparation_thread_.join();
  worker_thread_.join();
  lifecycle_lock.lock();
  certified_containment_retirement_.store(false, std::memory_order_release);
  lifecycle_worker_retirement_in_progress_.store(false, std::memory_order_release);
  return true;
}

bool ExecutionKernel::finalize_prepared_motion_cancellation_if_requested(
    const std::shared_ptr<PreparedMotionSession>& prepared_session,
    const ExecuteRequestDTO& request, const std::uint64_t exec_id,
    ActiveMotionExecution& motion_state, const char* reason) {
  if (!prepared_session) {
    return false;
  }
  {
    std::lock_guard lock(prepared_session->mutex);
    if (!prepared_session->cancel_requested) {
      return false;
    }
  }
  motion_state.record_terminal(std::nullopt);
  (void)settle_execution(exec_id, request.move_id,
                         ParticipantExecutionFact{
                             ParticipantExecutionFactKind::kCancelledBeforeDispatch,
                             ParticipantEffectKnowledge::kNoEffect, std::nullopt},
                         reason);
  return true;
}

void ExecutionKernel::settle_local_failure(const ExecuteRequestDTO& request,
                                           const std::uint64_t exec_id,
                                           ActiveMotionExecution& motion_state, ErrorDTO error,
                                           const ParticipantEffectKnowledge effect,
                                           const char* reason,
                                           std::optional<ExecutionSnapshot> final_snapshot) {
  motion_state.record_terminal(error);
  (void)settle_execution(
      exec_id, request.move_id,
      ParticipantExecutionFact{ParticipantExecutionFactKind::kLocalFailure, effect, std::move(error)},
      reason, std::move(final_snapshot));
}

void ExecutionKernel::expire_planning_deadline() {
  planning_deadline_expired_.store(true, std::memory_order_release);
  request_active_planning_cancel();
}

ExecutionKernel::ExecuteWorkerPlanPhaseResult ExecutionKernel::execute_worker_plan_phase(
    std::stop_token st, ExecuteRequestDTO& request,
    const CollisionWorldLeaseInventory& projection_inventory, std::uint64_t exec_id,
    std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
    ActiveMotionExecution& motion_state,
    const std::shared_ptr<PreparedMotionSession>& prepared_session) {
  ExecuteWorkerPlanPhaseResult result;

  if (st.stop_requested()) {
    motion_state.record_terminal(std::nullopt);
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }
  if (finalize_prepared_motion_cancellation_if_requested(
          prepared_session, request, exec_id, motion_state,
          "prepared_motion_cancelled_before_planning")) {
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (prepared_session && prepared_session->authority.replacement &&
      prepared_session->authority.replacement->spring_return) {
    std::lock_guard lock(prepared_session->mutex);
    const auto& motion = std::get<PreparedMotionExecutionDTO>(prepared_session->participant->payload);
    result.trajectory = motion.trajectory;
    result.plan_request = motion.plan_request;
    result.planning_result = motion.planning_evidence.planning_result;
    return result;
  }
  const bool cache_hit = cached_trajectory.has_value();
  result.active_trajectory_from_cache = cache_hit;
  result.cache_revalidation_candidate = cache_hit && cache_revalidation_candidate;
  if (cache_hit) {
    auto cached_request = make_plan_request(request, projection_inventory, 0U, nullptr);
    if (!cached_request.ok()) {
      if (finalize_prepared_motion_cancellation_if_requested(
              prepared_session, request, exec_id, motion_state,
              "prepared_motion_cancelled_during_cached_plan_validation")) {
        result.status = ExecuteWorkerPhaseStatus::kTerminal;
        return result;
      }
      const auto error = cached_request.error();
      emit_event(make_error_raised_event(exec_id, clock_.now(), error));
      settle_local_failure(request, exec_id, motion_state, error,
                           ParticipantEffectKnowledge::kNoEffect, "cached_plan_request_failed");
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
  planning_deadline_expired_.store(false, std::memory_order_release);
  if (planning_timeout_ns > 0) {
    const int64_t deadline = planning_start_ns + planning_timeout_ns;
    // Monitoring lane: the watchdog must fire even when the planner
    // and collision-monitor are blocked behind the scene lock. See
    // spec/220 threading invariants and ADR-0019. It only records the
    // expiry and cancels planning; this worker reports the fact.
    planning_timeout_handle = scheduler_.schedule(
        deadline, [this]() { expire_planning_deadline(); }, SchedulerPriority::Monitoring);
  }

  auto plan_result = prepared_session && prepared_session->authority.continuation
                         ? plan_motion_continuation(request, projection_inventory,
                                                    *prepared_session->authority.continuation)
                         : plan_trajectory_with_evidence(request, projection_inventory);
  const bool planning_timed_out =
      planning_timeout_ns > 0 && (planning_deadline_expired_.load(std::memory_order_acquire) ||
                                  (clock_.now() - planning_start_ns) >= planning_timeout_ns);

  if (planning_timeout_handle != 0) {
    scheduler_.cancel(planning_timeout_handle);
  }

  if (finalize_prepared_motion_cancellation_if_requested(
          prepared_session, request, exec_id, motion_state,
          "prepared_motion_cancelled_during_planning")) {
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (planning_reset_requested_.load(std::memory_order_acquire) ||
      sm_.status().state == ExecutionState::kIdle) {
    // Certified-containment retirement owns this execution. The planner
    // cancel can complete before retirement returns the participant to Idle,
    // so the cancellation result is not a planning failure.
    motion_state.record_terminal(std::nullopt);
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }

  if (planning_timed_out || !plan_result.ok()) {
    // A planning deadline that expires before dispatch is a planning failure
    // `planning_timeout`, whichever timer noticed it first (this Kernel's
    // deadline or the planner backend); it is never a Stop.
    const ErrorDTO planning_error =
        planning_timed_out ? ErrorDTO{kErrorPlanningTimeout, "Planning timed out", {}}
                           : plan_result.error();
    emit_event(make_error_raised_event(exec_id, clock_.now(), planning_error));
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.plans_failed++;
    }
    settle_local_failure(request, exec_id, motion_state, planning_error,
                         ParticipantEffectKnowledge::kNoEffect, "planning_failed");
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
    settle_local_failure(request, exec_id, motion_state,
                         ErrorDTO{kErrorInternalError, "Failed to transition to Validating", {}},
                         ParticipantEffectKnowledge::kNoEffect, "validating_transition_rejected");
    result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return result;
  }
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
    ActiveMotionExecution& motion_state,
    const std::shared_ptr<PreparedMotionSession>& prepared_session) {
  ExecuteWorkerValidationPhaseResult phase_result;
  if (prepared_session && prepared_session->authority.replacement &&
      prepared_session->authority.replacement->spring_return) {
    {
      std::lock_guard lock(prepared_session->mutex);
      const auto& motion = std::get<PreparedMotionExecutionDTO>(prepared_session->participant->payload);
      phase_result.collision_world_revision = motion.validation_evidence.collision_world_revision;
      phase_result.validation_evidence = motion.validation_evidence;
      phase_result.scene_snapshot_lease = scene_snapshot_lease;
      phase_result.collision_trajectory = prepared_session->replacement_collision_trajectory;
    }
    const TrajectoryDTO missing_curve;
    auto projection = build_runtime_collision_state_projection(
        trajectory, phase_result.collision_trajectory ? *phase_result.collision_trajectory
                                                     : missing_curve);
    if (!projection.ok()) {
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {},
                  projection.error(), {}});
      settle_local_failure(request, exec_id, motion_state, projection.error(),
                           ParticipantEffectKnowledge::kNoEffect, "validation_failed");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    set_runtime_collision_state_projection(std::move(projection.value()));
    return phase_result;
  }
  const int max_authoritative_validation_attempts =
      std::max(cache_revalidation_candidate ? 2 : 1,
               1 + config_.authoritative_validation_collision_replan_attempts);
  int authoritative_validation_attempt = 0;
  Result<CollisionValidationSuccess> val_result =
      Result<CollisionValidationSuccess>::Err(ErrorCode::kInternalError);
  bool conclusive_collision_failure = false;
  while (true) {
    emit_event({KernelEventType::kValidationStarted,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"attempt_index", std::to_string(authoritative_validation_attempt)},
                 {"max_attempts", std::to_string(max_authoritative_validation_attempts)}}});

    val_result =
        validate_trajectory(trajectory, request, scene_snapshot_lease_inventory,
                            scene_snapshot_lease, nullptr, {}, &conclusive_collision_failure);
    if (finalize_prepared_motion_cancellation_if_requested(
            prepared_session, request, exec_id, motion_state,
            "prepared_motion_cancelled_during_validation")) {
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
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
        sm_.status().state == ExecutionState::kValidating && !st.stop_requested();
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
    planning_deadline_expired_.store(false, std::memory_order_release);
    if (replan_timeout_ns > 0) {
      const int64_t deadline = replan_start_ns + replan_timeout_ns;
      replan_timeout_handle = scheduler_.schedule(
          deadline, [this]() { expire_planning_deadline(); }, SchedulerPriority::Monitoring);
    }
    auto replan_result =
        plan_trajectory_with_evidence(request, scene_snapshot_lease_inventory,
                                      static_cast<std::uint32_t>(authoritative_validation_attempt));
    const bool replan_timed_out =
        replan_timeout_ns > 0 && (planning_deadline_expired_.load(std::memory_order_acquire) ||
                                  (clock_.now() - replan_start_ns) >= replan_timeout_ns);
    if (replan_timeout_handle != 0) {
      scheduler_.cancel(replan_timeout_handle);
    }

    if (finalize_prepared_motion_cancellation_if_requested(
            prepared_session, request, exec_id, motion_state,
            "prepared_motion_cancelled_during_validation_replan")) {
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }

    const auto post_replan_status = sm_.status();
    if (post_replan_status.state == ExecutionState::kIdle || post_replan_status.fact.has_value()) {
      // Retired, or an exact cancellation already reported its fact.
      motion_state.record_terminal(std::nullopt);
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
    if (replan_timed_out || !replan_result.ok()) {
      // An expired replan deadline is the same planning failure
      // `planning_timeout` as one the planner reports; never a Stop.
      const ErrorDTO replan_error =
          replan_timed_out ? ErrorDTO{kErrorPlanningTimeout, "Planning timed out", {}}
                           : replan_result.error();
      emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, replan_error, {}});
      {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.plans_failed++;
      }
      settle_local_failure(request, exec_id, motion_state, replan_error,
                           ParticipantEffectKnowledge::kNoEffect,
                           "authoritative_validation_replan_failed");
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
      settle_local_failure(
          request, exec_id, motion_state,
          ErrorDTO{kErrorInternalError, "Failed to transition retry to Validating", {}},
          ParticipantEffectKnowledge::kNoEffect, "authoritative_replan_validating_failed");
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
    if (prepared_session) {
      std::lock_guard lock(prepared_session->mutex);
      prepared_session->conclusive_collision_failure = conclusive_collision_failure;
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
    settle_local_failure(request, exec_id, motion_state, val_result.error(),
                         ParticipantEffectKnowledge::kNoEffect, "validation_failed");
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

  if (sm_.status().state == ExecutionState::kIdle) {
    // Retired while validating: retirement owns this execution.
    motion_state.record_terminal(std::nullopt);
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

void ExecutionKernel::store_validated_candidate(const ExecuteRequestDTO& request,
                                                std::uint64_t exec_id,
                                                const TrajectoryDTO& trajectory,
                                                const ProjectionRevisionDTO& projection_revision,
                                                const CollisionWorldRevision& validation_revision,
                                                bool revalidated_cache_candidate) {
  // The exact resolved intent and validation revision remain cache authority.
  const auto key = trajectory_cache_key(request);
  if (request.cache_mode == CacheMode::kDisable || !key.valid() ||
      !collision_world_revision_is_set(validation_revision))
    return;
  cache_.store(key, trajectory,
               TrajectoryCacheRevisionEvidence{projection_revision, validation_revision},
               trajectory_cache_intent_identity(request));
  if (revalidated_cache_candidate) {
    emit_event({KernelEventType::kCacheRevalidated,
                exec_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"move_id", request.move_id},
                 {"revision_id", std::to_string(validation_revision.revision_id)}}});
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.cache_revalidations_succeeded++;
  }
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

  const auto fail_scene_snapshot_gate = [&](ErrorDTO stale_error, const char* reason) -> bool {
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, stale_error, {}});
    settle_local_failure(request, exec_id, motion_state, std::move(stale_error),
                         ParticipantEffectKnowledge::kNoEffect, reason);
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

        ErrorDTO stale_error{kErrorSceneChangedDuringValidation,
                             "Collision world changed after validation and before dispatch",
                             std::move(details)};
        if (fail_scene_snapshot_gate(std::move(stale_error),
                                     "dispatch_scene_snapshot_lease_rejected")) {
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
        if (fail_scene_snapshot_gate(std::move(stale_error), "dispatch_revision_changed")) {
          return phase_result;
        }
      }
    }
  }

  if (!active_trajectory_from_cache || cache_revalidation_candidate)
    store_validated_candidate(request, exec_id, trajectory, projection_binding.projection_revision,
                              validation_world_revision, cache_revalidation_candidate);

  auto prev_state3 = sm_.status().state;
  auto r3 = sm_.try_transition(ExecutionState::kExecuting);
  if (!r3.ok()) {
    settle_local_failure(request, exec_id, motion_state,
                         ErrorDTO{kErrorInternalError, "Failed to transition to Executing", {}},
                         ParticipantEffectKnowledge::kNoEffect, "executing_transition_rejected");
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
  auto monitoring = start_collision_monitoring("execute_dispatch", &scene_snapshot_lease_inventory);
  motion_state.runtime_collision_protection_generation = monitoring.generation;
  if (monitoring.prepare_error) {
    // Never dispatch without a prepared runtime monitor. This is a bug, not an
    // operating condition: a critical fault with no effect, recovered by Reset.
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                              monitoring.generation);
    auto details = std::move(monitoring.prepare_error->details);
    details["reason_code"] = kErrorRuntimeCollisionMonitorPrepareFailed;
    details["prepare_error_code"] = std::move(monitoring.prepare_error->code);
    ErrorDTO prepare_error{kErrorRuntimeCollisionMonitorPrepareFailed,
                           "Runtime collision monitor preparation failed: " +
                               monitoring.prepare_error->message,
                           std::move(details)};
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, prepare_error, {}});
    settle_local_failure(request, exec_id, motion_state, std::move(prepare_error),
                         ParticipantEffectKnowledge::kNoEffect, "execution_critical_fault");
    phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
    return phase_result;
  }
  phase_result.execution_handle =
      execution_backend_.Dispatch(*prepared_participant, request.binding.controller_goal_id,
                                  request.binding.controller_goal_generation);
  {
    if (const auto invalid = prepared_execution_handle_field_error(
            phase_result.execution_handle, *prepared_participant,
            request.binding.controller_goal_id, request.binding.controller_goal_generation)) {
      ErrorDTO dispatch_error{std::string(to_string(ErrorCode::kInvariantViolation)),
                              "Backend returned a handle outside the prepared dispatch authority",
                              {{"reason", std::string(*invalid)}}};
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                motion_state.runtime_collision_protection_generation);
      emit_event(
          {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, dispatch_error, {}});
      settle_local_failure(request, exec_id, motion_state, dispatch_error,
                           ParticipantEffectKnowledge::kEffectUncertain,
                           "prepared_dispatch_membership_invalid");
      phase_result.status = ExecuteWorkerPhaseStatus::kTerminal;
      return phase_result;
    }
  }
  // A generation-less handle is also the backend's explicit rejected-dispatch
  // representation.  Inspect a terminal handle before arming lookahead so the
  // backend's exact rejection (stale state, admission drift, action rejection,
  // progress-authority failure, ...) remains the operation-visible cause.
  // Otherwise lookahead correctly rejects generation 0 but masks the earlier,
  // much more actionable error as `execution_progress_generation_unavailable`.
  if (phase_result.execution_handle.execution_generation == 0 &&
      phase_result.execution_handle.get_snapshot) {
    auto rejected_snapshot = phase_result.execution_handle.get_snapshot();
    const bool backend_rejected = rejected_snapshot.state == ExecutionState::kFailed ||
                                  rejected_snapshot.state == ExecutionState::kInterventionRequired;
    if (backend_rejected) {
      auto dispatch_error = rejected_snapshot.error.value_or(
          ErrorDTO{kErrorExecutionRejected,
                   "Execution backend rejected dispatch without an error",
                   {{"reason", "backend_dispatch_rejected_without_error"}}});
      dispatch_error.details.try_emplace("dispatch_boundary", "execution_backend_handle");
      dispatch_error.details.try_emplace("execution_generation", "0");
      stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                motion_state.runtime_collision_protection_generation);
      emit_event({KernelEventType::kErrorRaised,
                  exec_id,
                  clock_.now(),
                  {},
                  {},
                  dispatch_error,
                  dispatch_error.details});
      // A rejected dispatch has no effect unless the backend itself could not
      // establish settlement of what it may already have sent.
      settle_local_failure(request, exec_id, motion_state, dispatch_error,
                           rejected_snapshot.state == ExecutionState::kInterventionRequired
                               ? ParticipantEffectKnowledge::kEffectUncertain
                               : ParticipantEffectKnowledge::kNoEffect,
                           "backend_dispatch_rejected", std::move(rejected_snapshot));
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

  const auto status = sm_.status();
  if (status.execution_id != exec_id || status.state == ExecutionState::kIdle ||
      status.fact.has_value()) {
    // Retired, or the addressed Stop settlement already recorded the fact.
    motion_state.record_terminal(status.fact.has_value() ? status.fact->error : std::nullopt);
    return;
  }

  core::ExecutionSnapshot final_snapshot;
  if (execution_handle.get_snapshot) {
    final_snapshot = execution_handle.get_snapshot();
    final_snapshot.execution_id = exec_id;
    final_snapshot.move_id = request.move_id;
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

  if (addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == exec_id) {
    // The addressed owner's Stop ended this execution after its fence; the
    // owner holds the containment evidence and the outcome.
    motion_state.record_terminal(std::nullopt);
    (void)settle_execution(exec_id, request.move_id,
                           ParticipantExecutionFact{
                               ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                               ParticipantEffectKnowledge::kEffectSettled, std::nullopt},
                           "addressed_stop_fenced_execution_ended", final_snapshot);
    return;
  }

  if (final_snapshot.state == ExecutionState::kFailed ||
      final_snapshot.state == ExecutionState::kInterventionRequired ||
      final_snapshot.state == ExecutionState::kStopped) {
    const bool uncertain = final_snapshot.state == ExecutionState::kInterventionRequired;
    auto error = final_snapshot.error.value_or(
        uncertain ? ErrorDTO{kErrorStopFailed,
                             "Execution required intervention without captured error",
                             {{"reason", "missing_error_on_intervention"}}}
        : final_snapshot.state == ExecutionState::kStopped
            ? ErrorDTO{kErrorExecutionAborted,
                       "Execution backend stopped without an addressed Stop",
                       {{"reason", "backend_stopped_without_addressed_stop"}}}
            : ErrorDTO{kErrorInternalError,
                       "Execution failed without captured error",
                       {{"reason", "missing_error_on_failed"}}});
    emit_event({KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, error, {}});
    settle_local_failure(request, exec_id, motion_state, std::move(error),
                         uncertain ? ParticipantEffectKnowledge::kEffectUncertain
                                   : ParticipantEffectKnowledge::kEffectSettled,
                         "execution_backend_failed", std::move(final_snapshot));
    return;
  }

  // Trajectory reached its computed end and the backend reported natural
  // completion: the participant's completion fact, with the final snapshot as
  // evidence. Its events are deferred until the publication guard is released
  // so an observer reacting to the fact cannot acknowledge ahead of it.
  EventDeferralScope completion_events(*this);
  {
    {
      std::lock_guard<std::mutex> lock(completion_publication_status_mutex_);
      completion_publication_previous_status_ = sm_.status();
    }
    CompletionPublicationGuard completion_publication{completion_publication_execution_id_,
                                                      exec_id};
    motion_state.record_terminal(std::nullopt);
    // The fact is masked from observers by the publication guard until its
    // evidence is published; kExecuteCompleted is dispatched inside the guard
    // and the deferred kParticipantSettled only after it.
    if (settle_execution(exec_id, request.move_id,
                         ParticipantExecutionFact{
                             ParticipantExecutionFactKind::kComputedCompletion,
                             ParticipantEffectKnowledge::kEffectSettled, std::nullopt},
                         "computed_completion", std::move(final_snapshot))) {
      emit_event_immediate(
          {KernelEventType::kExecuteCompleted, exec_id, clock_.now(), {}, {}, std::nullopt, {}});
    }
  }
  completion_events.flush();
}

Result<std::shared_ptr<const PreparedOperationParticipantDTO>>
ExecutionKernel::assemble_prepared_motion_participant(
    const ResolvedPreparedMotionExecutionRequestDTO& authority,
    const ExecuteRequestDTO& request, const TrajectoryDTO& trajectory,
    const PlanRequestDTO& plan_request, const PlanResultDTO& planning_result,
    const PreparedMotionValidationEvidenceDTO& validation_evidence) {
  const auto reject = [](std::string reason, std::string message) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(
        ErrorDTO{kErrorExecutionRejected, std::move(message), {{"reason", std::move(reason)}}});
  };
  if (!authority.ordinary_stop_plan) {
    return reject("ordinary_stop_plan_missing",
                  "Prepared motion requires the retained ordinary Stop plan");
  }
  const auto& binding = authority.binding.value();
  if (!binding.control_path) {
    return reject("prepared_motion_control_path_missing",
                  "Prepared motion requires one sealed control path");
  }
  const auto& path = *binding.control_path;

  PlanRequestDTO canonical_plan_request = plan_request;
  canonical_plan_request.retained_collision_world.reset();
  canonical_plan_request.retained_collision_policy.reset();

  std::vector<PreparedMotionInterruptScopeDTO> prepared_scopes;
  prepared_scopes.reserve(request.motion_interrupt_scopes.size());
  if (authority.interrupt_scope_actions.size() != request.motion_interrupt_scopes.size())
    return reject("prepared_motion_interrupt_action_missing",
                  "Every prepared interrupt scope requires its admitted sealed action");
  for (const auto& scope : request.motion_interrupt_scopes) {
    const auto action = std::find_if(
        authority.interrupt_scope_actions.begin(), authority.interrupt_scope_actions.end(),
        [&](const auto& value) { return value.scope_id.value() == scope.scope_id; });
    if (action == authority.interrupt_scope_actions.end() ||
        std::count_if(
            authority.interrupt_scope_actions.begin(), authority.interrupt_scope_actions.end(),
            [&](const auto& value) { return value.scope_id.value() == scope.scope_id; }) != 1)
      return reject("prepared_motion_interrupt_action_missing",
                    "Interrupt action membership is invalid");
    const bool desired =
        action->preemption_action == InterruptPreemptionAction::kCompleteMoveOnSignal;
    const bool fault = action->preemption_action == InterruptPreemptionAction::kPreemptTrajectory;
    if ((!desired && !fault) || (desired != scope.stop_profile.has_value()) ||
        (fault && action->success_through_stage_id))
      return reject("prepared_motion_interrupt_stop_profile_invalid",
                    "Interrupt profile does not match its admitted action");
    auto stop_plan = [&]() -> Result<PreparedParticipantStopPlanDTO> {
      if (fault) {
        if (!authority.ordinary_stop_plan)
          return Result<PreparedParticipantStopPlanDTO>::Err(
              ErrorDTO{"invalid_request",
                       "Fault scope has no ordinary Stop plan",
                       {{"reason", "prepared_motion_interrupt_stop_plan_missing"}}});
        return Result<PreparedParticipantStopPlanDTO>::Ok(*authority.ordinary_stop_plan);
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
      return MakePreparedParticipantStopPlan(
          authority.stop_readiness.value().resolved_stop_base_plan, std::move(stop_context),
          binding.resources, authority.retained_default_stop_limits);
    }();
    if (!stop_plan.ok())
      return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(stop_plan.error());
    const auto entry =
        ResolvePreparedIoRuleStopEntry(stop_plan.value(), action->stop_entry_stage_id);
    if (!entry.ok())
      return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(entry.error());
    const auto success_through = ResolvePreparedIoRuleSuccessThrough(
        stop_plan.value(), entry.value(), action->success_through_stage_id);
    if (!success_through.ok())
      return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(
          success_through.error());
    prepared_scopes.push_back(PreparedMotionInterruptScopeDTO{
        MotionInterruptScopeId(scope.scope_id), InterruptPolicyId(scope.policy_id),
        scope.activation_mode, scope.arm_timeout_ns, scope.required, scope.stop_profile,
        scope.node_category, scope.metadata, std::move(stop_plan.value()),
        action->preemption_action, entry.value(), success_through.value()});
  }
  std::sort(prepared_scopes.begin(), prepared_scopes.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.scope_id < rhs.scope_id; });

  std::optional<PreparedContactPolicyRefDTO> contact_policy;
  if (request.resolved_contact_grasp) {
    // Admission rewrote request.contact_policy_id to the operation-local bound
    // policy; the resolved grasp must match the policy the owner admitted.
    if (authority.execution_request.contact_policy_id !=
        request.resolved_contact_grasp->contact_policy_id.value()) {
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
      plan_request.start_state,
      PreparedMotionPlanningEvidenceDTO{planning_result, plan_request.planning_attempt_index,
                                        plan_request.planner_capability_binding},
      validation_evidence,
      std::move(contact_policy),
      authority.requested_tcp_linear_speed_mps,
      authority.deployment_maximum_tcp_linear_speed_mps,
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
      std::move(payload),
      authority.fault_stop_plan
          ? std::optional<PreparedParticipantStopPlanDTO>{*authority.fault_stop_plan}
          : std::nullopt};

  auto made = MakePreparedOperationParticipant(
      std::move(candidate), authority.binding, authority.stop_readiness,
      authority.retained_default_stop_limits, authority.stop_node_category);
  if (!made.ok()) {
    return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Err(made.error());
  }
  if (made.value().controller().ordinary_stop_plan != *authority.ordinary_stop_plan) {
    return reject("prepared_motion_ordinary_stop_plan_mismatch",
                  "Prepared participant changed the owner-issued ordinary Stop plan");
  }
  if (made.value().controller().fault_stop_plan !=
      (authority.fault_stop_plan
           ? std::optional<PreparedParticipantStopPlanDTO>{*authority.fault_stop_plan}
           : std::nullopt)) {
    return reject("prepared_motion_fault_stop_plan_mismatch",
                  "Prepared participant changed the owner-issued fault Stop plan");
  }

  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  try {
    participant = std::make_shared<const PreparedOperationParticipantDTO>(std::move(made.value()));
  } catch (...) {
    return reject("prepared_motion_product_allocation_failed",
                  "Prepared participant allocation failed before backend Prepare");
  }

  return Result<std::shared_ptr<const PreparedOperationParticipantDTO>>::Ok(std::move(participant));
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
  const bool trace_preparation = prepared_session->authority.replacement &&
                                 !prepared_session->authority.replacement->spring_return;
  const auto assembly_started = trace_preparation ? clock_.now() : 0;
  auto assembled = assemble_prepared_motion_participant(
      prepared_session->authority, request, trajectory, *plan_phase.plan_request,
      *plan_phase.planning_result, *validation_phase.validation_evidence);
  const auto assembly_finished = trace_preparation ? clock_.now() : 0;
  if (!assembled.ok()) return assembled;
  auto participant = std::move(assembled.value());
  const bool cold_reuse = prepared_session->authority.replacement &&
                         prepared_session->authority.replacement->spring_return;
  if (cold_reuse && (!prepared_session->participant ||
      prepared_session->participant->prepared_contract_digest != participant->prepared_contract_digest))
    return reject("spring_return_sealed_product_changed",
                  "Cold activation changed its retained planning or validation evidence");

  const auto backend_started = trace_preparation ? clock_.now() : 0;
  Result<PrepareParticipantResultDTO> backend_result = [&]() {
    try {
      return Result<PrepareParticipantResultDTO>::Ok(
          prepared_session->authority.replacement && !cold_reuse
              ? execution_backend_.PrepareReplacement(
                    *participant, prepared_session->execution_id,
                    prepared_session->authority.replacement->source)
              : execution_backend_.Prepare(*participant, prepared_session->execution_id));
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
  const auto backend_finished = trace_preparation ? clock_.now() : 0;
  if (trace_preparation)
    (void)std::fprintf(stderr,
        "manyforge_moving_core_phase source_goal=%.160s phase=participant_prepare "
        "execution_id=%llu started_ns=%lld finished_ns=%lld assembly_wall_ns=%lld backend_wall_ns=%lld\n",
        prepared_session->authority.replacement->source.goal.controller_goal_id.value().c_str(),
        static_cast<unsigned long long>(prepared_session->execution_id),
        static_cast<long long>(assembly_started), static_cast<long long>(backend_finished),
        static_cast<long long>(assembly_finished - assembly_started),
        static_cast<long long>(backend_finished - backend_started));
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
    std::stop_token st, ExecuteRequestDTO initial_request, uint64_t initial_exec_id,
    std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
    RetainedSceneSnapshotLease initial_retained_scene,
    std::optional<SceneSnapshotLease> initial_scene_snapshot_lease,
    std::shared_ptr<PreparedMotionSession> initial_prepared_session) {
  // The existing physical worker owns one current context. Candidate planning
  // never replaces this context until the worker reaches a handover safe point.
  ActiveMotionExecution motion_state(std::move(initial_request));
  motion_state.execution_id = initial_exec_id;
  motion_state.retained_scene = std::move(initial_retained_scene);
  motion_state.scene_snapshot_lease = std::move(initial_scene_snapshot_lease);
  motion_state.prepared_session = std::move(initial_prepared_session);
  auto& request = motion_state.request;
  auto& exec_id = motion_state.execution_id;
  auto& retained_scene = motion_state.retained_scene;
  auto& scene_snapshot_lease = motion_state.scene_snapshot_lease;
  auto& prepared_session = motion_state.prepared_session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    active_motion_execution_ = &motion_state;
  }
  struct WorkerCompletionGuard {
    ExecutionKernel* self;
    ActiveMotionExecution& motion_state;
    RetainedSceneSnapshotLease& retained_scene;
    std::optional<SceneSnapshotLease>& scene_snapshot_lease;
    std::shared_ptr<PreparedMotionSession>& prepared_session;
    ~WorkerCompletionGuard() {
      // Defensive: an unexpected return path missed the explicit terminal
      // record. Fail with a diagnostic reason rather than dropping the cause.
      motion_state.record_terminal(ErrorDTO{kErrorInternalError,
                                            "Worker exited without explicit motion terminal",
                                            {{"reason", "worker_exit_without_outcome"}}});
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
              // Reuse the exact recorded failure when events preceded the
              // owner's Prepare handoff.
              prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
              prepared_session->error =
                  motion_state.terminal_error.has_value()
                      ? motion_state.terminal_error
                      : std::optional<ErrorDTO>{ErrorDTO{
                            kErrorInternalError,
                            "Prepared motion worker exited before dispatch",
                            {{"reason", "prepared_motion_worker_exited_before_dispatch"}}}};
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
      {
        std::lock_guard lock(self->prepared_motion_session_mutex_);
        if (self->active_motion_execution_ == &motion_state)
          self->active_motion_execution_ = nullptr;
      }
      // Completion is the quiescence boundary observed by reset, tests, and
      // callers. Release every worker-owned scene authority handle before
      // publishing it; otherwise a waiter can race retained-lease cleanup.
      scene_snapshot_lease.reset();
      retained_scene = RetainedSceneSnapshotLease{};
      self->finish_execute_worker();
    }
  };
  WorkerCompletionGuard completion_guard{this, motion_state, retained_scene, scene_snapshot_lease,
                                         prepared_session};
  try {
    const auto projection_binding = retained_scene.binding();
    if (!projection_binding.valid()) {
      throw std::runtime_error("execute worker received an invalid retained projection binding");
    }
    auto plan_phase = execute_worker_plan_phase(
        st, request, retained_scene.inventory(), exec_id, std::move(cached_trajectory),
        cache_revalidation_candidate, motion_state, prepared_session);
    if (plan_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    TrajectoryDTO trajectory = std::move(plan_phase.trajectory);
    bool active_trajectory_from_cache = plan_phase.active_trajectory_from_cache;
    cache_revalidation_candidate = plan_phase.cache_revalidation_candidate;

    auto validation_phase = execute_worker_validate_replan_phase(
        st, request, exec_id, trajectory, plan_phase.plan_request, plan_phase.planning_result,
        active_trajectory_from_cache, cache_revalidation_candidate,
        retained_scene.mutable_inventory(), scene_snapshot_lease, motion_state, prepared_session);
    if (validation_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    const CollisionWorldRevision validation_world_revision =
        validation_phase.collision_world_revision;

    // Owner policy, resolved only after all planning/validation retiming. The
    // prepared product and the dispatch watchdog must carry the same budget.
    resolve_motion_execution_timeout(request, trajectory);
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_participant;
    auto prepared = make_and_prepare_motion_participant(request, trajectory, plan_phase,
                                                        validation_phase, prepared_session);
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
      if (cancelled) {
        (void)finalize_prepared_motion_cancellation_if_requested(
            prepared_session, request, exec_id, motion_state,
            "prepared_motion_cancelled_pre_dispatch");
      } else {
        const auto error = prepared.error();
        emit_event(make_error_raised_event(exec_id, clock_.now(), error));
        settle_local_failure(request, exec_id, motion_state, error,
                             ParticipantEffectKnowledge::kNoEffect,
                             "prepared_motion_prepare_failed");
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
             !st.stop_requested()) {
        prepared_session->changed.wait_for(lock, std::chrono::milliseconds(10));
      }
      cancelled = prepared_session->cancel_requested || st.stop_requested();
      if (!cancelled) {
        auto expected = PreparedMotionDispatchGate::kOpen;
        if (!prepared_session->dispatch_gate.compare_exchange_strong(
                expected, PreparedMotionDispatchGate::kCommitted, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          cancelled = expected == PreparedMotionDispatchGate::kFencedBeforeDispatch;
        }
      }
      if (cancelled) {
        prepared_session->cancel_requested = true;
        prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
      }
    }
    if (cancelled) {
      (void)finalize_prepared_motion_cancellation_if_requested(
          prepared_session, request, exec_id, motion_state,
          "prepared_motion_cancelled_before_dispatch");
      return;
    }

    auto dispatch_phase = execute_worker_dispatch_phase(
        request, exec_id, trajectory, active_trajectory_from_cache, cache_revalidation_candidate,
        projection_binding, retained_scene.inventory(), validation_world_revision,
        validation_phase.scene_snapshot_lease, std::move(validation_phase.collision_trajectory),
        prepared_participant, motion_state);
    if (dispatch_phase.status == ExecuteWorkerPhaseStatus::kTerminal) {
      return;
    }
    {
      std::lock_guard lock(prepared_session->mutex);
      prepared_session->backend_execution_generation =
          dispatch_phase.execution_handle.execution_generation;
      prepared_session->last_dispatched_participant = prepared_session->participant;
      prepared_session->state = ResolvedPreparedMotionExecutionStateDTO::kDispatched;
    }
    motion_state.execution_handle = std::move(dispatch_phase.execution_handle);
    auto& execution_handle = motion_state.execution_handle;
    motion_state.execution_progress_generation = execution_handle.execution_generation;
    motion_state.exec_start_ns = dispatch_phase.exec_start_ns;
    motion_state.exec_timeout_ns = dispatch_phase.exec_timeout_ns;
    motion_state.latest_backend_snapshot = std::move(dispatch_phase.latest_backend_snapshot);
    auto& exec_start_ns = motion_state.exec_start_ns;
    auto& exec_timeout_ns = motion_state.exec_timeout_ns;
    auto& latest_backend_snapshot = motion_state.latest_backend_snapshot;

    motion_state.last_runtime_scale_sample_ns = exec_start_ns;
    auto& last_runtime_scale_sample_ns = motion_state.last_runtime_scale_sample_ns;
    auto& runtime_scaled_execution_ns = motion_state.runtime_scaled_execution_ns;
    // Optional user-set zero-speed hold timeout (item 40). Unset keeps
    // `main`: a hold at zero speed never ends the motion.
    motion_state.zero_speed_hold_timeout_ns =
        request.timeouts.zero_speed_hold_timeout_s.has_value()
            ? static_cast<int64_t>(*request.timeouts.zero_speed_hold_timeout_s * 1e9)
            : 0;
    auto& zero_speed_hold_timeout_ns = motion_state.zero_speed_hold_timeout_ns;
    // Start of the current continuous zero-speed hold, when holding.
    auto& zero_speed_holding = motion_state.zero_speed_holding;
    auto& zero_speed_hold_started_ns = motion_state.zero_speed_hold_started_ns;
    auto& zero_speed_hold_stop_requested = motion_state.zero_speed_hold_stop_requested;
    while (true) {
      if (st.stop_requested()) {
        // Retirement after certified containment (or Kernel teardown).
        stop_collision_monitoring(RuntimeCollisionMonitorRelease::kWorkerRetirement,
                                  motion_state.runtime_collision_protection_generation);
        if (certified_containment_retirement_.load(std::memory_order_acquire)) {
          // The addressed owner already proved containment. Let the
          // contained handle quiesce, bounded, so membership retirement
          // finds the mechanism idle; never issue another command.
          (void)drain_execution_handle_bounded(execution_handle);
        } else {
          // Kernel teardown with motion still dispatched.
          (void)execution_backend_.stop();
        }
        motion_state.record_terminal(std::nullopt);
        return;
      }
      if (service_motion_replacement(st, motion_state)) continue;
      bool completed = true;
      if (execution_handle.wait_for_completion) {
        completed = execution_handle.wait_for_completion(kExecutionPollNs);
      }

      if (execution_handle.get_snapshot) {
        latest_backend_snapshot = execution_handle.get_snapshot();
        latest_backend_snapshot.execution_id = exec_id;
        latest_backend_snapshot.move_id = request.move_id;
        if (!sm_.settled()) {
          publish_execution_snapshot(latest_backend_snapshot);
          sm_.set_progress(latest_backend_snapshot.progress);
        }
      }

      const auto now_ns = clock_.now();
      if (!completed) {
        // As `main` (item 40): the timeout clock is elapsed time x scale, so
        // a zero-scale hold never trips it. The applied factor is used when
        // the mechanism reports a valid one; otherwise the accepted slider
        // value (`main`'s own quantity, 1.0 when none was set). Timing
        // feedback is never itself a failure.
        const int64_t dt_ns = std::max<int64_t>(0, now_ns - last_runtime_scale_sample_ns);
        double scale = 1.0;
        {
          std::lock_guard scale_lock(resolved_runtime_scale_mutex_);
          if (accepted_resolved_runtime_scale_command_.has_value()) {
            scale = accepted_resolved_runtime_scale_command_->requested_scale;
          }
        }
        try {
          const auto mechanism = execution_backend_.runtime_velocity_scale_status();
          if (mechanism.supported && !mechanism.error && std::isfinite(mechanism.applied_scale) &&
              mechanism.applied_scale >= 0.0 && mechanism.applied_scale <= 1.0) {
            scale = mechanism.applied_scale;
          }
        } catch (...) {
          // Keep the accepted slider value.
        }
        scale = std::isfinite(scale) ? std::clamp(scale, 0.0, 1.0) : 1.0;
        runtime_scaled_execution_ns += static_cast<double>(dt_ns) * scale;
        if (scale > 0.0) {
          zero_speed_holding = false;
        } else if (!zero_speed_holding) {
          zero_speed_holding = true;
          zero_speed_hold_started_ns = last_runtime_scale_sample_ns;
        }
      } else {
        zero_speed_holding = false;
      }
      last_runtime_scale_sample_ns = now_ns;

      const auto loop_status = sm_.status();
      if (loop_status.state == ExecutionState::kIdle || loop_status.execution_id != exec_id) {
        // Retired underneath this worker.
        motion_state.record_terminal(std::nullopt);
        return;
      }
      if (loop_status.fact.has_value() ||
          addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == exec_id) {
        // The addressed owner's Stop owns this execution: neither a timing
        // fault nor the local timeout may issue a competing command. Keep
        // observing until the contained handle ends.
        if (completed) {
          break;
        }
        continue;
      }

      if (!completed && zero_speed_hold_timeout_ns > 0 && !zero_speed_hold_stop_requested &&
          zero_speed_holding && now_ns - zero_speed_hold_started_ns >= zero_speed_hold_timeout_ns) {
        // The slider held the motion at zero for the user-set time: the
        // addressed owner ends it Stopped (no error), resumable like an
        // operator Stop, through its own Stop. This worker keeps observing
        // until that Stop fences and ends the execution.
        zero_speed_hold_stop_requested = true;
        emit_event({KernelEventType::kStopRequested,
                    exec_id,
                    clock_.now(),
                    {},
                    {},
                    std::nullopt,
                    {{"reason", "zero_speed_hold_timeout"},
                     {"zero_speed_hold_timeout_s",
                      std::to_string(static_cast<double>(zero_speed_hold_timeout_ns) / 1e9)},
                     {"held_s",
                      std::to_string(static_cast<double>(now_ns - zero_speed_hold_started_ns) /
                                     1e9)}}});
      }

      if (!completed && exec_timeout_ns > 0 &&
          runtime_scaled_execution_ns >= static_cast<double>(exec_timeout_ns)) {
        const ErrorDTO execution_error{
            kErrorExecutionTimeout,
            "Execution timeout exceeded",
            {{"execution_budget_s", std::to_string(static_cast<double>(exec_timeout_ns) / 1e9)},
             {"scaled_elapsed_s", std::to_string(runtime_scaled_execution_ns / 1e9)}}};
        stop_collision_monitoring(RuntimeCollisionMonitorRelease::kFailClosedTerminal,
                                  motion_state.runtime_collision_protection_generation);
        (void)execution_backend_.stop();
        const bool stop_completed = drain_execution_handle_bounded(execution_handle);
        ErrorDTO terminal_error =
            stop_completed ? execution_error
                           : operator_intervention_error(ErrorCode::kStopFailed,
                                                         execution_error.message +
                                                             " and software stop "
                                                             "completion was not observed",
                                                         "execution_timeout_stop_drain",
                                                         loop_status.state, exec_id,
                                                         request.move_id);
        emit_event(
            {KernelEventType::kErrorRaised, exec_id, clock_.now(), {}, {}, terminal_error, {}});
        settle_local_failure(request, exec_id, motion_state, std::move(terminal_error),
                             stop_completed ? ParticipantEffectKnowledge::kEffectSettled
                                            : ParticipantEffectKnowledge::kEffectUncertain,
                             "execution_timeout", latest_backend_snapshot);
        return;
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
    // A backend's reported critical fault (Q52) keeps its own error; any
    // other exception is the worker's (item 17). Both take this path.
    const auto* critical = dynamic_cast<const ExecutionCriticalFault*>(&e);
    const ErrorDTO exception_error =
        critical != nullptr
            ? critical->error()
            : ErrorDTO{kErrorInternalError, e.what(), {{"reason", "worker_exception"}}};
    bool dispatched = false;
    if (prepared_session) {
      std::lock_guard lock(prepared_session->mutex);
      dispatched = prepared_session->dispatch_gate.load(std::memory_order_acquire) ==
                   PreparedMotionDispatchGate::kCommitted;
    }
    if (dispatched) {
      // Item 17: an exception after dispatch leaves the command's settlement
      // unknown. Issue the lane's hard stop (the primitive Reset uses) and
      // end InterventionRequired, unless an addressed Stop already owns the
      // execution.
      const auto status = sm_.status();
      if (!status.fact.has_value() &&
          addressed_stop_fence_execution_id_.load(std::memory_order_acquire) != exec_id) {
        try {
          (void)execution_backend_.stop();
        } catch (...) {
          // The outcome stays InterventionRequired either way.
        }
      }
    }
    settle_local_failure(request, exec_id, motion_state, exception_error,
                         dispatched ? ParticipantEffectKnowledge::kEffectUncertain
                                    : ParticipantEffectKnowledge::kNoEffect,
                         critical != nullptr ? "execution_critical_fault" : "worker_exception");
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
  if (const auto status = sm_.status();
      status.state != ExecutionState::kIdle || status.fact.has_value()) {
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

std::shared_ptr<ExecutionKernel::PreparedMotionSession>
ExecutionKernel::failed_preparation_never_dispatched_locked() {
  // Caller holds lifecycle_mutex_. A Failed terminal whose exact prepared
  // session never requested or reached dispatch commanded nothing: the
  // dispatch barrier opens only for a Prepared product, and this execution
  // committed its failure before one existed. Such a failure has no physical
  // effect, so it is retryable like a pre-dispatch validation failure.
  const auto status = sm_.status();
  if (!status.fact.has_value() || status.fact->kind != ParticipantExecutionFactKind::kLocalFailure ||
      status.execution_id == 0U)
    return nullptr;
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) return nullptr;
  std::lock_guard lock(session->mutex);
  return session->never_dispatched_for(status.execution_id) ? session : nullptr;
}

bool ExecutionKernel::failed_preparation_settled_locked() {
  // Caller holds lifecycle_mutex_. Once the worker of a never-dispatched
  // failed preparation has finished, an effect-free preview may plan from the
  // same idle robot. This grants no execution, Reset or recovery authority.
  const auto session = failed_preparation_never_dispatched_locked();
  if (!session) return false;
  {
    std::lock_guard lock(session->mutex);
    if (session->state != ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed) {
      return false;
    }
  }
  return worker_signals_.wait_for_completion(1);
}

struct ExecutionKernel::LifecyclePreemption final {
  explicit LifecyclePreemption(ExecutionKernel& owner) : owner_(owner) {
    owner_.lifecycle_preemption_waiters_.fetch_add(1U, std::memory_order_seq_cst);
    std::lock_guard lock(owner_.preview_cancellation_mutex_);
    if (owner_.active_preview_cancellation_.has_value()) {
      owner_.active_preview_cancellation_->request_stop();
    }
  }
  LifecyclePreemption(const LifecyclePreemption&) = delete;
  LifecyclePreemption& operator=(const LifecyclePreemption&) = delete;
  ~LifecyclePreemption() {
    owner_.lifecycle_preemption_waiters_.fetch_sub(1U, std::memory_order_seq_cst);
  }

 private:
  ExecutionKernel& owner_;
};

Result<ExecutionKernel::ValidatedMotionPlan> ExecutionKernel::compute_validated_motion_plan(
    const ExecuteRequestDTO& request, CollisionWorldLeaseInventory& projection_inventory,
    const RobotStateDTO& start_state, const std::uint32_t planning_attempt_index,
    const std::int64_t planning_start_ns, const std::int64_t planning_timeout_ns,
    const std::stop_token stop_token,
    std::shared_ptr<CollisionCancellationToken> validation_cancellation,
    const std::optional<std::uint64_t> validation_execution_id,
    const MovingReplacementInputDTO* replacement,
    const std::optional<std::int64_t> suffix_planning_deadline_ns) {
  const auto reject = [](const ErrorCode code, std::string message, std::string reason) {
    return Result<ValidatedMotionPlan>::Err(ErrorDTO{
        std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
  };
  const auto failure = [&](ErrorDTO error, const char* phase) {
    if (replacement) {
      error.details.try_emplace("takeover_compute_phase", phase);
      error.details.try_emplace("takeover_preparation_budget_ns",
                                std::to_string(planning_timeout_ns));
      error.details.try_emplace("takeover_preparation_started_ns",
                                std::to_string(planning_start_ns));
      if (suffix_planning_deadline_ns) {
        error.details.try_emplace("takeover_suffix_planning_deadline_ns",
                                  std::to_string(*suffix_planning_deadline_ns));
        error.details.try_emplace("takeover_suffix_planning_budget_ns",
                                  std::to_string(*suffix_planning_deadline_ns - planning_start_ns));
      }
    }
    return Result<ValidatedMotionPlan>::Err(std::move(error));
  };
  auto planned =
      plan_trajectory_with_evidence(request, projection_inventory, planning_attempt_index,
                                    &start_state, replacement, suffix_planning_deadline_ns);
  const auto planning_finished_ns = clock_.now();
  const auto planning_elapsed_ns = planning_finished_ns - planning_start_ns;
  trace_moving_core_phase(replacement, "candidate_return", planning_start_ns,
      planning_finished_ns, planned.ok() && planned.value().result.trajectory
          ? planned.value().result.trajectory->waypoints.size() : 0U);
  if (planning_elapsed_ns >= planning_timeout_ns) {
    ErrorDTO error{kErrorPlanningTimeout, "Motion preview planning timed out", {}};
    if (!planned.ok()) {
      const auto &underlying = planned.error();
      error.details = underlying.details;
      error.details.try_emplace("underlying_error_code", underlying.code);
      error.details.try_emplace("underlying_error_message", underlying.message);
      if (const auto reason = underlying.details.find("reason");
          reason != underlying.details.end()) {
        error.details.try_emplace("underlying_error_reason", reason->second);
      }
    }
    error.details["reason"] = "motion_preview_planning_timeout";
    error.details["planning_elapsed_ns"] =
        std::to_string(std::max<std::int64_t>(0, planning_elapsed_ns));
    error.details["planning_budget_ns"] = std::to_string(planning_timeout_ns);
    return failure(std::move(error), "candidate_compute");
  }
  if (stop_token.stop_requested()) {
    return reject(ErrorCode::kExecutionAborted, "Motion preview was cancelled during planning",
                  "motion_preview_cancelled");
  }
  if (!planned.ok()) return failure(planned.error(), "candidate_compute");

  auto validated = validate_trajectory(
      *planned.value().result.trajectory, request, projection_inventory, std::nullopt,
      &start_state, stop_token, nullptr, std::move(validation_cancellation), validation_execution_id, replacement);
  if (stop_token.stop_requested()) {
    return reject(ErrorCode::kExecutionAborted, "Motion preview was cancelled during validation",
                  "motion_preview_cancelled");
  }
  if (!validated.ok()) return failure(validated.error(), "core_validation");
  return Result<ValidatedMotionPlan>::Ok(
      ValidatedMotionPlan{std::move(planned.value()), std::move(validated.value())});
}

Result<MotionPreviewResultDTO> ExecutionKernel::preview_motion(
    ExecuteRequestDTO request, CollisionWorldLeaseInventory projection_inventory,
    RobotStateDTO start_state, const std::uint32_t planning_attempt_index,
    const std::stop_token caller_stop_token) {
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  // Exclusive planner access stays with lifecycle_mutex_; a waiting Stop,
  // Reset or recovery cancels this advisory preview instead of waiting for
  // its planning to finish or time out. Register before reading the waiter
  // count so either side observes the other.
  std::stop_source preview_cancellation;
  {
    std::lock_guard lock(preview_cancellation_mutex_);
    active_preview_cancellation_ = preview_cancellation;
  }
  struct ActivePreviewRegistration final {
    ExecutionKernel& owner;
    ~ActivePreviewRegistration() {
      std::lock_guard lock(owner.preview_cancellation_mutex_);
      owner.active_preview_cancellation_.reset();
    }
  } active_preview_registration{*this};
  std::stop_callback caller_cancellation(
      caller_stop_token, [&preview_cancellation]() { preview_cancellation.request_stop(); });
  if (lifecycle_preemption_waiters_.load(std::memory_order_seq_cst) != 0U) {
    preview_cancellation.request_stop();
  }
  const auto stop_token = preview_cancellation.get_token();
  const auto reject = [](const ErrorCode code, std::string message, std::string reason) {
    return Result<MotionPreviewResultDTO>::Err(ErrorDTO{
        std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
  };
  if (const auto status = sm_.status();
      (status.state != ExecutionState::kIdle || status.fact.has_value()) &&
      !failed_preparation_settled_locked()) {
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
      request.target_reference.has_value() || request.offset_reference.has_value() ||
      request.goal_frame_reference.has_value()) {
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
    auto computed = compute_validated_motion_plan(
        request, projection_inventory, start_state, planning_attempt_index,
        planning_start_ns, planning_timeout_ns, stop_token);
    if (!computed.ok()) return Result<MotionPreviewResultDTO>::Err(computed.error());
    auto trajectory = std::move(*computed.value().planning.result.trajectory);
    auto validated = std::move(computed.value().validation);

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
        projection_inventory.projection_binding, validated.collision_world_revision,
        validated.dynamically_discovered_start_contact_count});
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
      request.stop_readiness.value().resolved_stop_base_plan, *request.ordinary_stop_plan, binding.resources,
      request.retained_default_stop_limits, request.stop_node_category);
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
  if (request.fault_stop_plan) {
    const auto verified_fault = VerifyPreparedParticipantStopPlan(
        request.stop_readiness.value().resolved_stop_base_plan, *request.fault_stop_plan, binding.resources,
        request.retained_default_stop_limits, request.stop_node_category);
    if (!verified_fault.ok() || request.fault_stop_plan->operation_id != resolved.operation_id ||
        request.fault_stop_plan->operation_revision != request.operation_revision ||
        request.fault_stop_plan->participant_id != resolved.participant_id ||
        request.fault_stop_plan->program_run_id != resolved.program_run_id ||
        request.fault_stop_plan->participant_stop_profile_purpose !=
            ParticipantStopProfilePurposeDTO::kFault ||
        request.fault_stop_plan->motion_interrupt_scope_id.has_value()) {
      return Result<std::uint64_t>::Err(
          verified_fault.ok() ? ErrorDTO{kErrorInvalidRequest,
                                         "Prepared fault Stop plan crosses execution identity",
                                         {{"reason", "fault_stop_plan_identity_mismatch"}}}
                              : verified_fault.error());
    }
  }

  if (request.replacement && request.replacement->spring_return &&
      request.replacement->spring_return->abandoned_preparation_id != 0U)
    return prepare_spring_motion_attachment(std::move(request));

  std::shared_ptr<PreparedMotionSession> session;
  try {
    session = std::make_shared<PreparedMotionSession>(std::move(request));
  } catch (...) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{kErrorInternalError,
                 "Prepared motion session allocation failed before admission",
                 {{"reason", "prepared_motion_session_allocation_failed"}}});
  }
  if (session->authority.replacement) return prepare_motion_replacement(std::move(session));
  // Keep the owner stable while evaluating the request reference. Function
  // argument evaluation order must not allow moving `session` before the
  // request argument is formed.
  return execute_impl(session->authority.execution_request, session);
}

Result<void> ExecutionKernel::qualify_spring_motion_replacement(
    ResolvedPreparedMotionExecutionRequestDTO& candidate, const PreparedMotionSession& source) {
  const auto refuse = [](const char* reason) {
    return Result<void>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Spring return source is not the exact resident Stop occurrence",
                 {{"reason", reason}}});
  };
  auto& context = *candidate.replacement;
  if (!context.spring_return || !context.spring_return->stage || !context.spring_return->point ||
      !source.last_dispatched_participant || source.backend_execution_generation == 0U ||
      source.state != ResolvedPreparedMotionExecutionStateDTO::kDispatched)
    return refuse("spring_return_source_unavailable");
  const auto& stage = *context.spring_return->stage;
  const auto& point = *context.spring_return->point;
  const auto& binding = source.authority.execution_request.binding;
  const auto* motion =
      std::get_if<PreparedMotionExecutionDTO>(&source.last_dispatched_participant->payload);
  const auto status = sm_.status();
  const auto scope_digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  if (!motion || !scope_digest.ok() ||
      scope_digest.value() != stage.resolved_stop_scope_policy_digest || stage.stage_index != 0U ||
      stage.operation_id != binding.operation_id ||
      stage.participant_id != binding.participant_id ||
      stage.controller_goal_id != binding.controller_goal_id ||
      stage.controller_goal_generation != binding.controller_goal_generation ||
      stage.control_binding_id != binding.control_binding_id ||
      stage.control_binding_revision != binding.control_binding_revision ||
      stage.participant_stop_plan != source.last_dispatched_participant->controller().ordinary_stop_plan ||
      point.source_goal !=
          ExecutionGoalKey{binding.controller_goal_id, binding.controller_goal_generation} ||
      point.binding_epoch != point.trip.binding_epoch ||
      status.execution_id != source.execution_id ||
      addressed_stop_fence_execution_id_.load(std::memory_order_acquire) != source.execution_id ||
      (!status.fact && status.state != ExecutionState::kExecuting &&
       status.state != ExecutionState::kStopping))
    return refuse("spring_return_source_identity_mismatch");
  if (status.fact &&
      (status.fact->kind != ParticipantExecutionFactKind::kPreemptedByAddressedStop ||
       status.fact->effect != ParticipantEffectKnowledge::kEffectSettled))
    return refuse("spring_return_source_effects_unqualified");
  const auto& initial = point.initial;
  const auto& rest = initial.spring_back_return_point;
  const auto joints = motion->trajectory.joint_names.size();
  if (initial.joint_names != motion->trajectory.joint_names || joints == 0U ||
      rest.positions_rad.size() != joints || rest.velocities_rad_s.size() != joints ||
      rest.accelerations_rad_s2.size() != joints || rest.time_from_start_s != 0.0 ||
      !std::isfinite(initial.command_path_time_s) || !point.canonical_return_path_time_s ||
      !std::isfinite(*point.canonical_return_path_time_s) ||
      *point.canonical_return_path_time_s < 0.0 || motion->trajectory.waypoints.empty() ||
      *point.canonical_return_path_time_s > motion->trajectory.waypoints.back().time_from_start_s ||
      !std::isfinite(initial.runtime_velocity_scale_at_trip) ||
      initial.runtime_velocity_scale_at_trip <= 0.0 ||
      initial.runtime_velocity_scale_at_trip > 1.0 || !std::isfinite(initial.path_time_rate) ||
      initial.path_time_rate <= 0.0 ||
      !std::all_of(rest.positions_rad.begin(), rest.positions_rad.end(),
                   [](double q) { return std::isfinite(q); }) ||
      !std::all_of(rest.velocities_rad_s.begin(), rest.velocities_rad_s.end(),
                   [](double v) { return v == 0.0; }) ||
      !std::all_of(rest.accelerations_rad_s2.begin(), rest.accelerations_rad_s2.end(),
                   [](double a) { return a == 0.0; }))
    return refuse("spring_return_point_invalid");
  context.source = PreparedMotionReplacementSourceDTO{
      source.execution_id,
      source.backend_execution_generation,
      point.source_goal,
      point.binding_epoch,
      source.last_dispatched_participant->prepared_contract_digest,
      point.trip,
      {},
      0};
  return Result<void>::Ok();
}

Result<void> ExecutionKernel::compute_spring_motion_replacement(
    const std::stop_token stop_token, const std::shared_ptr<PreparedMotionSession>& session) {
  const auto refuse = [](const char* reason) {
    return Result<void>::Err(ErrorDTO{
        kErrorExecutionRejected, "Spring successor cold preparation failed", {{"reason", reason}}});
  };
  // Attachment is installed before this one shared task starts and is retained
  // until the task is joined; physical session fields stay untouched.
  auto* attached = session->spring_cold_product ? &*session->spring_cold_product : nullptr;
  const auto& authority = attached ? attached->authority : session->authority;
  const auto execution_id = attached ? attached->reserved_execution_id : session->execution_id;
  const auto& context = *authority.replacement;
  if (!context.spring_return || !context.spring_return->point || !context.spring_return->stage)
    return refuse("spring_return_context_missing");
  const auto& initial = context.spring_return->point->initial;
  auto staged = std::make_unique<ActiveMotionExecution>(authority.execution_request);
  staged->execution_id = execution_id;
  auto& request = staged->request;
  if (request.target_mode != MotionTargetModeDTO::kAbsolute || request.target_reference ||
      request.offset_reference || request.goal_frame_reference)
    return refuse("spring_return_target_not_resolved");
  auto resolved = resolve_motion_target(request, nullptr, reference_resolver_);
  if (!resolved.ok()) return Result<void>::Err(resolved.error());
  request.motion = std::move(resolved.value());
  RobotStateDTO rest{initial.joint_names, initial.spring_back_return_point.positions_rad,
                     initial.spring_back_return_point.velocities_rad_s};
  auto start = controlled_state_for_binding(request, rest);
  if (!start.ok()) return Result<void>::Err(start.error());
  auto retained = retain_current_scene_snapshot_lease_inventory(execution_id, request.move_id);
  if (!retained.ok()) return Result<void>::Err(retained.error());
  staged->retained_scene = std::move(retained.value());
  auto& inventory = staged->retained_scene.mutable_inventory();
  if (inventory.projection_binding.projection_revision.retained_bundle_id !=
          request.binding.retained_bundle_id ||
      inventory.projection_binding.projection_revision.graph_revision !=
          request.binding.graph_revision)
    return refuse("spring_return_projection_mismatch");
  if (auto admission = collision_validation_admission_error()) return Result<void>::Err(*admission);
  auto grasp = install_resolved_contact_grasp_policy(request, inventory);
  if (!grasp.ok()) return grasp;
  if (clock_.now() >= session->preparation_deadline_ns || stop_token.stop_requested())
    return refuse("spring_return_preparation_budget_expired");
  std::stop_callback cancellation(stop_token, [this, session] {
    session->replacement_validation_cancellation->request_cancel("spring_return_cancelled");
    planner_.request_cancel();
  });
  const auto deadline = scheduler_.schedule(
      session->preparation_deadline_ns,
      [this, session, cancellation = session->replacement_validation_cancellation] {
        std::lock_guard lock(session->mutex);
        if (session->replacement_validation_cancellation != cancellation ||
            session->preparation_completed)
          return;
        cancellation->request_cancel("spring_return_budget_expired");
        planner_.request_cancel();
      },
      SchedulerPriority::Monitoring);
  struct DeadlineGuard {
    IScheduler& scheduler;
    ScheduleHandle handle;
    ~DeadlineGuard() {
      if (handle != 0U) scheduler.cancel(handle);
    }
  } guard{scheduler_, deadline};
  const auto remaining_ns = session->preparation_deadline_ns - clock_.now();
  if (remaining_ns <= 0 || stop_token.stop_requested())
    return refuse("spring_return_preparation_budget_expired");
  // Bound the actual candidate's PlanRequest without rewriting the authored
  // execution request. The successor's existing full-B start and outer
  // watchdog remain the cancellation/containment authority.
  auto candidate_request = request;
  candidate_request.timeouts.planning_timeout_s =
      std::min(request.timeouts.planning_timeout_s, static_cast<double>(remaining_ns) / 1.0e9);
  auto computed = compute_validated_motion_plan(
      candidate_request, inventory, rest, 0U, session->preparation_started_ns,
      context.preparation_budget_ns, stop_token, session->replacement_validation_cancellation,
      execution_id);
  if (!computed.ok()) return Result<void>::Err(computed.error());
  if (clock_.now() >= session->preparation_deadline_ns || stop_token.stop_requested())
    return refuse("spring_return_preparation_budget_expired");
  auto& trajectory = *computed.value().planning.result.trajectory;
  resolve_motion_execution_timeout(request, trajectory);
  const auto& validation = computed.value().validation;
  PreparedMotionValidationEvidenceDTO evidence{
      config_.validation_revision, validation.noncollision_result,
      validation.collision_world_revision, validation.collision_policy_revision,
      validation.collision_decision};
  auto product = assemble_prepared_motion_participant(authority, request, trajectory,
                                                      computed.value().planning.request,
                                                      computed.value().planning.result, evidence);
  if (!product.ok()) return Result<void>::Err(product.error());
  auto collision_curve = std::make_shared<const TrajectoryDTO>(
      std::move(computed.value().validation.collision_trajectory));
  staged->scene_snapshot_lease = std::move(computed.value().validation.scene_snapshot_lease);
  std::lock_guard lock(session->mutex);
  if (session->cancel_requested || (attached && attached->cancelled) || stop_token.stop_requested())
    return refuse("spring_return_cancelled");
  if (clock_.now() >= session->preparation_deadline_ns)
    return refuse("spring_return_preparation_budget_expired");
  if (attached) {
    attached->participant = std::move(product.value());
    attached->execution = std::move(staged);
    attached->collision_trajectory = std::move(collision_curve);
    attached->state = ResolvedPreparedMotionExecutionStateDTO::kColdPrepared;
  } else {
    session->participant = std::move(product.value());
    session->replacement_execution = std::move(staged);
    session->replacement_collision_trajectory = std::move(collision_curve);
    session->state = ResolvedPreparedMotionExecutionStateDTO::kColdPrepared;
  }
  return Result<void>::Ok();
}

Result<bool> ExecutionKernel::PrepareColdMotionAfterStop(const std::uint64_t candidate_id,
                                                         const StopCertificateDTO& certificate) {
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire))
    return Result<bool>::Ok(false);
  // An event may be delivered on the preparation thread itself. Existing
  // reconciliation retries the read; never join that same thread here.
  if (replacement_preparation_thread_.joinable() &&
      replacement_preparation_thread_.get_id() == std::this_thread::get_id())
    return Result<bool>::Ok(false);
  std::shared_ptr<PreparedMotionSession> candidate;
  std::shared_ptr<PreparedMotionSession> source;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    candidate = pending_prepared_motion_session_;
    source = active_prepared_motion_session_;
  }
  if (!candidate && source && source->execution_id == candidate_id) {
    std::lock_guard lock(source->mutex);
    const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                          certificate.stop_certificate_revision,
                                          certificate.stop_id, certificate.stop_generation};
    if (source->spring_activation_certificate == reference) return Result<bool>::Ok(true);
  }
  const auto refuse = [](const char* reason) {
    return Result<bool>::Err(ErrorDTO{kErrorExecutionRejected,
                                      "Spring successor activation lacks exact settled authority",
                                      {{"reason", reason}}});
  };
  if (!candidate || !source || candidate->execution_id != candidate_id)
    return refuse("spring_return_candidate_stale");
  std::optional<ExecuteRequestDTO> request;
  {
    std::lock_guard lock(candidate->mutex);
    if (!candidate->authority.replacement || !candidate->authority.replacement->spring_return ||
        !candidate->authority.replacement->spring_return->stage ||
        !candidate->authority.replacement->spring_return->point || candidate->cancel_requested)
      return refuse("spring_return_context_missing");
    if (candidate->state != ResolvedPreparedMotionExecutionStateDTO::kColdPrepared ||
        !candidate->preparation_completed)
      return Result<bool>::Ok(false);
    if (!candidate->participant || !candidate->replacement_execution ||
        candidate->backend_prepare_result)
      return refuse("spring_return_cold_product_unavailable");
    const auto& stage = *candidate->authority.replacement->spring_return->stage;
    const auto& captured = candidate->authority.replacement->source;
    const auto status = sm_.status();
    const auto* local =
        addressed_participant_stop_settlement_ ? &*addressed_participant_stop_settlement_ : nullptr;
    if (certificate.stop_id != stage.stop_id ||
        certificate.stop_generation != stage.stop_generation ||
        certificate.result != StopCertificateResultDTO::kSucceededNominal ||
        certificate.terminal_error || certificate.fault_latch_id ||
        !certificate.unsettled.empty() || !certificate.failed.empty() || !local ||
        local->execution_id != captured.execution_id ||
        local->aggregate_stop_certificate !=
            StopCertificateRefDTO{certificate.stop_certificate_id,
                                  certificate.stop_certificate_revision, certificate.stop_id,
                                  certificate.stop_generation} ||
        status.execution_id != captured.execution_id ||
        recovery_execution_state_locked(status) != ExecutionState::kStopped || !status.fact ||
        status.fact->kind != ParticipantExecutionFactKind::kPreemptedByAddressedStop ||
        status.fact->effect != ParticipantEffectKnowledge::kEffectSettled)
      return refuse("spring_return_stop_not_settled");
    const bool returned = std::any_of(
        certificate.attempted_stage_results.begin(), certificate.attempted_stage_results.end(),
        [&](const auto& attempted) {
          const auto& result = attempted.result;
          return attempted.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                 result.request == stage &&
                 result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
                 result.attempt_disposition == StopStageAttemptDispositionDTO::kDispatched &&
                 !result.error &&
                 result.actual_provider_contract_mode == MotionControlledStopMode::kSpringBackHold;
        });
    if (!returned) return refuse("spring_return_not_the_completed_stage");
    std::lock_guard source_lock(source->mutex);
    if (source->execution_id != captured.execution_id ||
        source->backend_execution_generation != captured.execution_generation ||
        !source->last_dispatched_participant ||
        source->last_dispatched_participant->prepared_contract_digest !=
            captured.prepared_contract_digest)
      return refuse("spring_return_source_stale");
    request = candidate->replacement_execution->request;
    candidate->spring_activation_certificate = StopCertificateRefDTO{
        certificate.stop_certificate_id, certificate.stop_certificate_revision, certificate.stop_id,
        certificate.stop_generation};
  }
  // execute_impl retains ordinary admission and dispatch gates; its spring-only
  // reuse branch preserves this exact sealed product, projection and identity.
  lifecycle_lock.unlock();
  const auto activated = execute_impl(*request, candidate);
  if (!activated.ok()) {
    std::lock_guard lock(candidate->mutex);
    candidate->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
    candidate->error = activated.error();
    return Result<bool>::Err(activated.error());
  }
  return activated.value() == candidate_id ? Result<bool>::Ok(true)
                                           : refuse("spring_return_candidate_identity_changed");
}
Result<std::uint64_t> ExecutionKernel::prepare_motion_replacement(
    std::shared_ptr<PreparedMotionSession> session) {
  const auto reject = [](const char* reason) {
    return Result<std::uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                               "Motion replacement preparation was refused",
                                               {{"reason", reason}}});
  };
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire))
    return reject("worker_retirement_in_progress");
  const auto& context = *session->authority.replacement;
  if (context.preparation_budget_ns <= 0 || session->authority.continuation ||
      (context.planning_budget_ns &&
       (*context.planning_budget_ns <= 0 ||
        *context.planning_budget_ns >= context.preparation_budget_ns)))
    return reject("motion_replacement_context_invalid");
  const auto& timeouts = session->authority.execution_request.timeouts;
  const double maximum_timeout_s =
      static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1e9;
  if (!std::isfinite(timeouts.execution_timeout_s) || timeouts.execution_timeout_s <= 0.0 ||
      timeouts.execution_timeout_s >= maximum_timeout_s ||
      (timeouts.zero_speed_hold_timeout_s &&
       (!std::isfinite(*timeouts.zero_speed_hold_timeout_s) ||
        *timeouts.zero_speed_hold_timeout_s <= 0.0 ||
        *timeouts.zero_speed_hold_timeout_s >= maximum_timeout_s)))
    return reject("motion_replacement_timeout_invalid");
  std::shared_ptr<PreparedMotionSession> source;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    source = active_prepared_motion_session_;
    if (pending_prepared_motion_session_) return reject("motion_replacement_already_pending");
  }
  if (!source) return reject("motion_replacement_source_missing");
  {
    std::lock_guard lock(source->mutex);
    const auto status = sm_.status();
    const auto& incoming = session->authority.execution_request;
    const auto& incumbent = source->authority.execution_request;
    if (context.spring_return) {
      const auto qualified = qualify_spring_motion_replacement(session->authority, *source);
      if (!qualified.ok()) return Result<std::uint64_t>::Err(qualified.error());
    }
    if (!context.spring_return &&
        (source->execution_id != context.source.execution_id ||
         source->backend_execution_generation != context.source.execution_generation ||
         source->state != ResolvedPreparedMotionExecutionStateDTO::kDispatched ||
         !source->last_dispatched_participant ||
         source->last_dispatched_participant->prepared_contract_digest !=
             context.source.prepared_contract_digest ||
         incumbent.binding.controller_goal_id != context.source.goal.controller_goal_id ||
         incumbent.binding.controller_goal_generation !=
             context.source.goal.controller_goal_generation ||
         status.execution_id != source->execution_id || status.fact ||
         status.state != ExecutionState::kExecuting ||
         addressed_stop_fence_execution_id_.load(std::memory_order_acquire) ==
             source->execution_id))
      return reject("motion_replacement_source_stale");
    if (!incoming.action_span || !incumbent.action_span ||
        incoming.action_span->action_source != ActionSource::kProgramNode ||
        incumbent.action_span->action_source != ActionSource::kProgramNode ||
        incoming.binding.program_run_id.empty() ||
        incoming.binding.program_run_id != incumbent.binding.program_run_id ||
        incoming.binding.control_binding_id != incumbent.binding.control_binding_id ||
        incoming.binding.control_binding_revision != incumbent.binding.control_binding_revision ||
        incoming.binding.resources != incumbent.binding.resources)
      return reject("motion_replacement_scope_mismatch");
  }
  if (replacement_preparation_thread_.joinable()) {
    if (replacement_preparation_thread_.get_id() == std::this_thread::get_id())
      return reject("motion_replacement_computation_retiring");
    lifecycle_worker_retirement_in_progress_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    replacement_preparation_thread_.join();
    lifecycle_lock.lock();
    lifecycle_worker_retirement_in_progress_.store(false, std::memory_order_release);
    // Joining is never an admission fence; recheck the incumbent after it.
    if (context.spring_return) {
      std::lock_guard lock(source->mutex);
      const auto qualified = qualify_spring_motion_replacement(session->authority, *source);
      if (!qualified.ok()) return Result<std::uint64_t>::Err(qualified.error());
    }
    const auto status = sm_.status();
    if (!context.spring_return && (status.execution_id != context.source.execution_id ||
                                   status.fact || status.state != ExecutionState::kExecuting))
      return reject("motion_replacement_source_stale");
  }
  const auto id = next_execution_id_.load(std::memory_order_relaxed);
  if (id == std::numeric_limits<std::uint64_t>::max()) return reject("execution_id_exhausted");
  session->execution_id = id;
  session->preparation_started_ns = clock_.now();
  if (session->preparation_started_ns >
      std::numeric_limits<std::int64_t>::max() - context.preparation_budget_ns)
    return reject("motion_replacement_budget_unrepresentable");
  session->preparation_deadline_ns =
      session->preparation_started_ns + context.preparation_budget_ns;
  session->replacement_validation_cancellation = std::make_shared<CollisionCancellationToken>();
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    if (active_prepared_motion_session_ != source || pending_prepared_motion_session_)
      return reject("motion_replacement_source_stale");
    next_execution_id_.store(id + 1U, std::memory_order_relaxed);
    pending_prepared_motion_session_ = session;
  }
  const auto launched = launch_motion_replacement_preparation(session);
  if (!launched.ok()) {
    std::lock_guard lock(prepared_motion_session_mutex_);
    if (pending_prepared_motion_session_ == session) pending_prepared_motion_session_.reset();
    return Result<std::uint64_t>::Err(launched.error());
  }
  return Result<std::uint64_t>::Ok(id);
}

Result<std::uint64_t> ExecutionKernel::prepare_spring_motion_attachment(
    ResolvedPreparedMotionExecutionRequestDTO request) {
  const auto reject = [](const char* reason) {
    return Result<std::uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                               "Sent replacement cold computation was refused",
                                               {{"reason", reason}}});
  };
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire))
    return reject("worker_retirement_in_progress");
  const auto abandoned_id = request.replacement->spring_return->abandoned_preparation_id;
  const auto budget = request.replacement->preparation_budget_ns;
  if (budget <= 0 || request.continuation) return reject("motion_replacement_context_invalid");
  const auto& timeouts = request.execution_request.timeouts;
  const double maximum_timeout_s =
      static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1e9;
  if (!std::isfinite(timeouts.execution_timeout_s) || timeouts.execution_timeout_s <= 0.0 ||
      timeouts.execution_timeout_s >= maximum_timeout_s ||
      (timeouts.zero_speed_hold_timeout_s &&
       (!std::isfinite(*timeouts.zero_speed_hold_timeout_s) ||
        *timeouts.zero_speed_hold_timeout_s <= 0.0 ||
        *timeouts.zero_speed_hold_timeout_s >= maximum_timeout_s)))
    return reject("motion_replacement_timeout_invalid");
  std::shared_ptr<PreparedMotionSession> source;
  std::shared_ptr<PreparedMotionSession> pending;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    source = active_prepared_motion_session_;
    pending = pending_prepared_motion_session_;
  }
  if (!source || !pending) return reject("spring_return_candidate_stale");
  const auto qualify = [&]() -> Result<void> {
    std::scoped_lock lock(source->mutex, pending->mutex);
    if (pending->execution_id != abandoned_id || !pending->authority.replacement ||
        pending->spring_cold_product || pending->cancel_requested ||
        !pending->preparation_completed || !pending->backend_prepare_result ||
        pending->backend_prepare_result->state != PrepareParticipantResultStateDTO::kPrepared ||
        pending->dispatch_gate.load(std::memory_order_acquire) !=
            PreparedMotionDispatchGate::kCommitted)
      return Result<void>::Err(ErrorCode::kExecutionRejected);
    const auto& old = pending->authority.execution_request;
    const auto& next = request.execution_request;
    const auto& incumbent = source->authority.execution_request;
    auto same_binding = next.binding;
    same_binding.controller_goal_generation = old.binding.controller_goal_generation;
    if (!next.action_span || !old.action_span || !incumbent.action_span ||
        next.action_span->action_source != ActionSource::kProgramNode ||
        old.action_span->action_source != ActionSource::kProgramNode ||
        incumbent.action_span->action_source != ActionSource::kProgramNode ||
        next.action_span != old.action_span || same_binding != old.binding ||
        request.operation_revision != pending->authority.operation_revision ||
        next.binding.operation_id != old.binding.operation_id ||
        next.binding.participant_id != old.binding.participant_id ||
        next.binding.program_run_id.empty() ||
        next.binding.program_run_id != old.binding.program_run_id ||
        next.binding.program_run_id != incumbent.binding.program_run_id ||
        next.binding.controller_goal_id != old.binding.controller_goal_id ||
        old.binding.controller_goal_generation.value() >= 9'007'199'254'740'991ULL ||
        next.binding.controller_goal_generation.value() !=
            old.binding.controller_goal_generation.value() + 1U ||
        next.binding.control_binding_id != old.binding.control_binding_id ||
        next.binding.control_binding_revision != old.binding.control_binding_revision ||
        next.binding.resources != old.binding.resources ||
        next.binding.control_binding_id != incumbent.binding.control_binding_id ||
        next.binding.control_binding_revision != incumbent.binding.control_binding_revision ||
        next.binding.resources != incumbent.binding.resources)
      return Result<void>::Err(ErrorCode::kInvalidRequest);
    return qualify_spring_motion_replacement(request, *source);
  };
  if (const auto qualified = qualify(); !qualified.ok())
    return Result<std::uint64_t>::Err(qualified.error());
  if (replacement_preparation_thread_.joinable()) {
    if (replacement_preparation_thread_.get_id() == std::this_thread::get_id())
      return reject("motion_replacement_computation_retiring");
    lifecycle_worker_retirement_in_progress_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    replacement_preparation_thread_.join();
    lifecycle_lock.lock();
    lifecycle_worker_retirement_in_progress_.store(false, std::memory_order_release);
    if (const auto qualified = qualify(); !qualified.ok())
      return Result<std::uint64_t>::Err(qualified.error());
  }
  const auto id = next_execution_id_.load(std::memory_order_relaxed);
  const auto started = clock_.now();
  if (id == std::numeric_limits<std::uint64_t>::max()) return reject("execution_id_exhausted");
  if (started > std::numeric_limits<std::int64_t>::max() - budget)
    return reject("motion_replacement_budget_unrepresentable");
  auto validation_cancellation = std::make_shared<CollisionCancellationToken>();
  PreparedMotionSession::SpringColdProduct cold{
      std::move(request), id,   ResolvedPreparedMotionExecutionStateDTO::kPreparing, {}, {}, {},
      std::nullopt,       false};
  {
    std::lock_guard owner_lock(prepared_motion_session_mutex_);
    if (active_prepared_motion_session_ != source || pending_prepared_motion_session_ != pending)
      return reject("spring_return_candidate_stale");
    std::lock_guard lock(pending->mutex);
    pending->spring_cold_product.emplace(std::move(cold));
    pending->preparation_started_ns = started;
    pending->preparation_deadline_ns = started + budget;
    pending->preparation_completed = false;
    pending->replacement_cancellation = std::stop_source{};
    pending->replacement_validation_cancellation = std::move(validation_cancellation);
    next_execution_id_.store(id + 1U, std::memory_order_relaxed);
  }
  const auto launched = launch_motion_replacement_preparation(pending);
  if (!launched.ok()) {
    std::lock_guard lock(pending->mutex);
    pending->spring_cold_product->state =
        ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
    pending->spring_cold_product->error = launched.error();
    pending->preparation_completed = true;
    return Result<std::uint64_t>::Err(launched.error());
  }
  return Result<std::uint64_t>::Ok(abandoned_id);
}

Result<void> ExecutionKernel::launch_motion_replacement_preparation(
    const std::shared_ptr<PreparedMotionSession>& session) {
  try {
    replacement_preparation_thread_ = std::jthread([this, session](std::stop_token token) {
      std::stop_callback cancellation(token,
                                      [&]() { session->replacement_cancellation.request_stop(); });
      compute_motion_replacement(session->replacement_cancellation.get_token(), session);
    });
    return Result<void>::Ok();
  } catch (...) {
    return Result<void>::Err(ErrorDTO{kErrorExecutionRejected,
                                      "Motion replacement computation could not start",
                                      {{"reason", "motion_replacement_computation_start_failed"}}});
  }
}

void ExecutionKernel::compute_motion_replacement(const std::stop_token stop_token,
                                                 std::shared_ptr<PreparedMotionSession> session) {
  const auto failed = [](const char* reason) {
    return ErrorDTO{
        kErrorExecutionRejected, "Moving replacement preparation failed", {{"reason", reason}}};
  };
  std::optional<std::int64_t> backend_prepared_at;
  auto compute = [&]() -> Result<void> {
    if (session->spring_cold_product) return compute_spring_motion_replacement(stop_token, session);
    const auto& context = *session->authority.replacement;
    if (context.spring_return) return compute_spring_motion_replacement(stop_token, session);
    const auto& captured = context.source;
    // H already includes acquisition and confirmation of the incumbent scale.
    // Keep the same hold through preparation failure for the existing Stop owner.
    const auto held = execution_backend_.BeginReplacementPreparationScaleHold(
        captured, session->execution_id, session->preparation_deadline_ns, stop_token);
    if (!held.ok()) return held;
    std::shared_ptr<const PreparedOperationParticipantDTO> source_product;
    {
      std::lock_guard owner_lock(prepared_motion_session_mutex_);
      if (!active_prepared_motion_session_ ||
          active_prepared_motion_session_->execution_id != captured.execution_id ||
          pending_prepared_motion_session_ != session)
        return Result<void>::Err(failed("motion_replacement_source_stale"));
      std::lock_guard source_lock(active_prepared_motion_session_->mutex);
      source_product = active_prepared_motion_session_->last_dispatched_participant;
    }
    const auto* source = source_product
                             ? std::get_if<PreparedMotionExecutionDTO>(&source_product->payload)
                             : nullptr;
    if (!source || source_product->prepared_contract_digest != captured.prepared_contract_digest ||
        captured.binding_epoch != captured.controller_sample.binding_epoch ||
        !captured.controller_sample.applied_factor ||
        !std::isfinite(*captured.controller_sample.applied_factor) ||
        *captured.controller_sample.applied_factor <= 0.0 ||
        *captured.controller_sample.applied_factor > 1.0 || !captured.progress.valid ||
        captured.progress.terminal ||
        captured.progress.execution_generation != captured.execution_generation ||
        captured.progress.trajectory_time_ns < 0 ||
        captured.progress.received_timestamp_ns !=
            captured.controller_sample.local_receipt_stamp_ns ||
        captured.progress.source_timestamp_ns != captured.controller_sample.source_stamp_ns ||
        !std::isfinite(captured.controller_sample.cursor_s) ||
        captured.controller_sample.cursor_s < 0.0)
      return Result<void>::Err(failed("motion_replacement_source_sample_invalid"));
    // Actual cursor and remaining lead are rechecked at send and adoption;
    // the backend owns the confirmed scale hold through the connector.
    // The controller commands one unscaled period P ahead of its cursor. With
    // P added, a takeover tick T at most budget + P after this sample's stamp
    // S finds every earlier source command at or before D (nominal clock).
    const double cursor = static_cast<double>(captured.progress.trajectory_time_ns) / 1e9;
    const double divergence = cursor + static_cast<double>(context.preparation_budget_ns) / 1e9 *
                                           *captured.controller_sample.applied_factor +
                              static_cast<double>(captured.controller_period_ns) / 1e9;
    if (!std::isfinite(divergence) || divergence <= cursor ||
        source->trajectory.waypoints.empty() ||
        divergence >= source->trajectory.waypoints.back().time_from_start_s)
      return Result<void>::Err(failed("motion_replacement_future_lead_unavailable"));
    auto prefix = slice_future_trajectory(source->trajectory, cursor);
    auto cut = sample_trajectory(source->trajectory, divergence);
    auto start = sample_trajectory(source->trajectory, cursor);
    if (!prefix.ok()) return Result<void>::Err(prefix.error());
    if (!cut.ok()) return Result<void>::Err(cut.error());
    if (!start.ok()) return Result<void>::Err(start.error());
    auto& points = prefix.value().waypoints;
    const double lead = divergence - cursor;
    points.erase(std::lower_bound(
                     points.begin(), points.end(), lead,
                     [](const auto& point, double time) { return point.time_from_start_s < time; }),
                 points.end());
    points.push_back(TrajectoryWaypointDTO{lead, cut.value().positions_rad,
                                           cut.value().velocities_rad_s,
                                           cut.value().accelerations_rad_s2});
    // Carry the incumbent's existing evidence only across the same accepted
    // span-equivalence contract as the shared splice. The final cut is newly
    // sampled; all earlier translated intervals were checked by the slice.
    if (source->trajectory.runtime_scale_bounds) {
      const auto cut_knot = std::lower_bound(
          source->trajectory.waypoints.begin(), source->trajectory.waypoints.end(), divergence,
          [](const auto& point, double time) { return point.time_from_start_s < time; });
      if (cut_knot == source->trajectory.waypoints.begin() || points.size() < 2U)
        return Result<void>::Err(failed("trajectory_splice_time_precision_lost"));
      const double old_span = divergence - std::max(cursor, std::prev(cut_knot)->time_from_start_s);
      const double new_span = lead - points[points.size() - 2U].time_from_start_s;
      if (!(old_span > 0.0) || !(new_span > 0.0) ||
          std::abs(old_span - new_span) >
              std::min(1e-9, std::sqrt(std::numeric_limits<double>::epsilon()) * old_span))
        return Result<void>::Err(failed("trajectory_splice_time_precision_lost"));
      prefix.value().runtime_scale_bounds = source->trajectory.runtime_scale_bounds;
    }
    // Source milestones are not successor-authored obligations. A Cartesian
    // source requiring a skipped milestone was already refused by the shared
    // slice; never fabricate a new fidelity certificate for this prefix.
    prefix.value().cartesian_waypoint_times_s.clear();
    auto prepared = std::make_unique<ActiveMotionExecution>(session->authority.execution_request);
    prepared->execution_id = session->execution_id;
    auto& request = prepared->request;
    if (request.target_mode != MotionTargetModeDTO::kAbsolute || request.target_reference ||
        request.offset_reference || request.goal_frame_reference)
      return Result<void>::Err(failed("motion_replacement_target_not_resolved"));
    auto resolved = resolve_motion_target(request, nullptr, reference_resolver_);
    if (!resolved.ok()) return Result<void>::Err(resolved.error());
    request.motion = std::move(resolved.value());
    RobotStateDTO start_state{source->trajectory.joint_names, start.value().positions_rad,
                              start.value().velocities_rad_s};
    RobotStateDTO suffix_seed{source->trajectory.joint_names, cut.value().positions_rad,
                              std::vector<double>(source->trajectory.joint_names.size(), 0.0)};
    auto suffix_state = controlled_state_for_binding(request, suffix_seed);
    if (!suffix_state.ok()) return Result<void>::Err(suffix_state.error());
    const auto elapsed =
        motion_replacement_elapsed_ns(clock_.now(), session->preparation_started_ns);
    if (!elapsed || *elapsed >= context.preparation_budget_ns || stop_token.stop_requested())
      return Result<void>::Err(failed("motion_replacement_preparation_budget_expired"));
    MovingReplacementInputDTO moving{
        captured.goal,
        captured.binding_epoch,
        cursor,
        std::move(prefix.value()),
        divergence,
        std::move(suffix_state.value()),
        source->plan_request.base_frame,
        source->plan_request.tcp_frame,
        source->plan_request.speed_profile,
        source->plan_request.per_resource_dynamics_limits,
        static_cast<double>(context.preparation_budget_ns - *elapsed) / 1e9};
    auto retained =
        retain_current_scene_snapshot_lease_inventory(session->execution_id, request.move_id);
    if (!retained.ok()) return Result<void>::Err(retained.error());
    prepared->retained_scene = std::move(retained.value());
    auto& inventory = prepared->retained_scene.mutable_inventory();
    if (inventory.projection_binding.projection_revision.retained_bundle_id !=
            request.binding.retained_bundle_id ||
        inventory.projection_binding.projection_revision.graph_revision !=
            request.binding.graph_revision)
      return Result<void>::Err(failed("motion_replacement_projection_mismatch"));
    if (auto admission = collision_validation_admission_error(); admission)
      return Result<void>::Err(std::move(*admission));
    auto grasp = install_resolved_contact_grasp_policy(request, inventory);
    if (!grasp.ok()) return grasp;
    std::stop_callback cancellation(stop_token, [this, session]() {
      session->replacement_validation_cancellation->request_cancel("motion_replacement_cancelled");
      planner_.request_cancel();
    });
    const auto start_ns = session->preparation_started_ns;
    if (start_ns > std::numeric_limits<std::int64_t>::max() - context.preparation_budget_ns)
      return Result<void>::Err(failed("motion_replacement_budget_unrepresentable"));
    const auto deadline = scheduler_.schedule(
        session->preparation_deadline_ns,
        [this, session, cancellation = session->replacement_validation_cancellation]() {
          std::lock_guard lock(session->mutex);
          if (session->replacement_validation_cancellation != cancellation ||
              session->preparation_completed)
            return;
          cancellation->request_cancel("motion_replacement_budget_expired");
          planner_.request_cancel();
        },
        SchedulerPriority::Monitoring);
    struct DeadlineGuard {
      IScheduler& scheduler;
      ScheduleHandle handle;
      ~DeadlineGuard() {
        if (handle != 0U) scheduler.cancel(handle);
      }
    } deadline_guard{scheduler_, deadline};
    auto computed = compute_validated_motion_plan(
        request, inventory, start_state, 0U, start_ns, context.preparation_budget_ns, stop_token,
        session->replacement_validation_cancellation, session->execution_id, &moving,
        context.planning_budget_ns
            ? std::optional<std::int64_t>(start_ns + *context.planning_budget_ns)
            : std::nullopt);
    if (!computed.ok()) return Result<void>::Err(computed.error());
    const auto completion_elapsed = motion_replacement_elapsed_ns(clock_.now(), start_ns);
    if (!completion_elapsed || *completion_elapsed >= context.preparation_budget_ns ||
        stop_token.stop_requested())
      return Result<void>::Err(failed("motion_replacement_preparation_budget_expired"));
    resolve_motion_execution_timeout(request, *computed.value().planning.result.trajectory);
    ExecuteWorkerPlanPhaseResult planning;
    planning.plan_request = std::move(computed.value().planning.request);
    planning.planning_result = std::move(computed.value().planning.result);
    auto& trajectory = *planning.planning_result->trajectory;
    auto& validation = computed.value().validation;
    ExecuteWorkerValidationPhaseResult validated;
    validated.validation_evidence = PreparedMotionValidationEvidenceDTO{
        config_.validation_revision, validation.noncollision_result,
        validation.collision_world_revision, validation.collision_policy_revision,
        validation.collision_decision};
    auto product =
        make_and_prepare_motion_participant(request, trajectory, planning, validated, session);
    backend_prepared_at = clock_.now();
    trace_moving_core_phase(&moving, "backend_prepared", start_ns, *backend_prepared_at, trajectory.waypoints.size());
    if (!product.ok()) return Result<void>::Err(product.error());
    auto projection =
        build_runtime_collision_state_projection(trajectory, validation.collision_trajectory);
    if (!projection.ok()) return Result<void>::Err(projection.error());
    auto correlation = std::make_unique<ExecutionCorrelationTracker>();
    correlation->set_from_request(request);
    auto collision_curve =
        std::make_shared<const TrajectoryDTO>(std::move(validation.collision_trajectory));
    prepared->scene_snapshot_lease = std::move(validation.scene_snapshot_lease);
    auto arm = RuntimeCollisionLookaheadSession::prepare_arm(
        session->execution_id, 0U, collision_curve, request.contact_policy_id,
        prepared->retained_scene.binding().projection_revision);
    std::lock_guard lock(session->mutex);
    if (session->cancel_requested || stop_token.stop_requested())
      return Result<void>::Err(failed("motion_replacement_cancelled"));
    session->participant = std::move(product.value());
    session->replacement_execution = std::move(prepared);
    session->replacement_correlation = std::move(correlation);
    session->replacement_collision_trajectory = std::move(collision_curve);
    session->replacement_projection = std::move(projection.value());
    session->replacement_lookahead_arm = std::move(arm);
    session->state = ResolvedPreparedMotionExecutionStateDTO::kPrepared;
    return Result<void>::Ok();
  };
  auto outcome = [&]() -> Result<void> {
    try {
      return compute();
    } catch (const std::exception& error) {
      return Result<void>::Err(ErrorDTO{
          kErrorPlanningFailed, "Replacement preparation raised", {{"exception", error.what()}}});
    } catch (...) {
      return Result<void>::Err(failed("motion_replacement_preparation_exception"));
    }
  }();
  {
    std::lock_guard lock(session->mutex);
    session->preparation_completed = true;
    if (!outcome.ok()) {
      if (session->spring_cold_product) {
        auto& cold = *session->spring_cold_product;
        cold.state = cold.cancelled || stop_token.stop_requested()
                         ? ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch
                         : ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
        cold.error = outcome.error();
      } else {
        session->state = session->cancel_requested
                             ? ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch
                             : ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
        session->error = outcome.error();
      }
    }
  }
  if (!outcome.ok() && !session->spring_cold_product && session->backend_prepare_result &&
      session->backend_prepare_result->state == PrepareParticipantResultStateDTO::kPrepared) {
    // Keep the original preparation error. A later explicit cancellation or
    // owner cleanup observes any retirement refusal and retains membership.
    (void)execution_backend_.RetirePreparedExecution(session->execution_id);
  }
  if (backend_prepared_at)
    (void)std::fprintf(stderr,
        "manyforge_moving_core_phase source_goal=%.160s phase=preparation_finalized "
        "execution_id=%llu started_ns=%lld finished_ns=%lld\n",
        session->authority.replacement->source.goal.controller_goal_id.value().c_str(),
        static_cast<unsigned long long>(session->execution_id),
        static_cast<long long>(*backend_prepared_at), static_cast<long long>(clock_.now()));
  session->changed.notify_all();
  KernelEvent event;
  event.type = KernelEventType::kParticipantPrepared;
  event.execution_id = session->execution_id;
  event.timestamp_ns = clock_.now();
  if (!outcome.ok()) event.error = outcome.error();
  event.details = {{"phase", "replacement_preparation"}};
  ExecutionCorrelationTracker correlation;
  correlation.set_from_request(session->authority.execution_request);
  correlation.stamp_details(event.details);
  emit_event(std::move(event));
}

Result<void> ExecutionKernel::check_motion_replacement_scene(
    const PreparedMotionSession& session) const {
  const auto* motion = session.participant
                           ? std::get_if<PreparedMotionExecutionDTO>(&session.participant->payload)
                           : nullptr;
  if (!motion || !session.replacement_execution)
    return Result<void>::Err(ErrorCode::kInvalidRequest);
  const auto& next = *session.replacement_execution;
  if (next.scene_snapshot_lease) {
    const auto validation = current_scene_snapshot_lease_validation_context();
    if (validation) {
      if (validate_scene_snapshot_lease(*next.scene_snapshot_lease, *validation).accepted)
        return Result<void>::Ok();
    }
  } else if (collision_world_revisions_equal(
                 current_authoritative_collision_world_revision(),
                 motion->validation_evidence.collision_world_revision)) {
    return Result<void>::Ok();
  }
  return Result<void>::Err(ErrorDTO{kErrorSceneChangedDuringValidation,
                                    "Replacement collision evidence is no longer current",
                                    {{"reason", "motion_replacement_scene_stale"}}});
}

bool ExecutionKernel::service_motion_replacement(const std::stop_token stop_token,
                                                 ActiveMotionExecution& current) {
  std::shared_ptr<PreparedMotionSession> pending;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    pending = pending_prepared_motion_session_;
  }
  if (!pending) return false;
  const auto& source = pending->authority.replacement->source;
  bool notify_owner = false;
  bool should_observe = false;
  const auto notify = [&]() {
    KernelEvent event;
    event.type = KernelEventType::kParticipantPrepared;
    event.execution_id = pending->execution_id;
    event.timestamp_ns = clock_.now();
    event.details = {{"phase", "replacement_commit_safe_point"}};
    ExecutionCorrelationTracker correlation;
    correlation.set_from_request(pending->authority.execution_request);
    correlation.stamp_details(event.details);
    emit_event(std::move(event));
  };
  {
    std::lock_guard lock(pending->mutex);
    if (pending->cancel_requested || stop_token.stop_requested() ||
        source.execution_id != current.execution_id ||
        addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == current.execution_id)
      return false;
    if (pending->state == ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested &&
        pending->dispatch_gate.load(std::memory_order_acquire) ==
            PreparedMotionDispatchGate::kOpen) {
      const auto elapsed =
          motion_replacement_elapsed_ns(clock_.now(), pending->preparation_started_ns);
      const auto scene = check_motion_replacement_scene(*pending);
      if (!scene.ok() || !elapsed ||
          *elapsed >= pending->authority.replacement->preparation_budget_ns) {
        pending->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
        pending->error = !scene.ok()
                             ? scene.error()
                             : ErrorDTO{kErrorPlanningTimeout,
                                        "Replacement preparation budget expired before send",
                                        {}};
        pending->replacement_observation = PreparedMotionReplacementObservationDTO{
            PreparedMotionReplacementPhaseDTO::kNoEffect, {}, {}, {}, pending->error};
        notify_owner = true;
      } else {
        std::lock_guard protection_lock(runtime_collision_protection_mutex_);
        const auto* progress = execution_backend_.execution_progress_publication();
        const auto hazard = progress
                                ? progress->hazard_authority_claimed(source.execution_generation)
                                : std::nullopt;
        if (!runtime_collision_protection_.active ||
            runtime_collision_protection_.execution_id != source.execution_id || !hazard ||
            *hazard) {
          pending->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
          pending->error = ErrorDTO{kErrorExecutionRejected,
                                    "Source runtime protection already owns containment",
                                    {{"reason", "motion_replacement_source_hazard_authority"}}};
          pending->replacement_observation = PreparedMotionReplacementObservationDTO{
              PreparedMotionReplacementPhaseDTO::kNoEffect, {}, {}, {}, pending->error};
          notify_owner = true;
        } else {
          auto expected = PreparedMotionDispatchGate::kOpen;
          if (!pending->dispatch_gate.compare_exchange_strong(
                  expected, PreparedMotionDispatchGate::kCommitted, std::memory_order_acq_rel))
            return false;
          const auto& binding = pending->authority.execution_request.binding;
          auto receipt = execution_backend_.DispatchReplacement(
              source, pending->execution_id, *pending->participant,
              ExecutionGoalKey{binding.controller_goal_id, binding.controller_goal_generation},
              pending->preparation_deadline_ns);
          if (receipt.command.dispatch == ExecutionPortDispatch::kNotDispatched) {
            pending->replacement_observation = PreparedMotionReplacementObservationDTO{
                PreparedMotionReplacementPhaseDTO::kNoEffect, {}, {}, {}, receipt.command.error};
            pending->state = ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed;
            pending->error = receipt.command.error.value_or(
                ErrorDTO{kErrorExecutionRejected, "Replacement was not dispatched", {}});
            notify_owner = true;
          } else {
            pending->replacement_observation = PreparedMotionReplacementObservationDTO{
                PreparedMotionReplacementPhaseDTO::kPossiblySent,
                {},
                {},
                {},
                receipt.command.error};
            if (receipt.submitted_at_ns &&
                *receipt.submitted_at_ns >= pending->preparation_started_ns &&
                *receipt.submitted_at_ns < pending->preparation_deadline_ns &&
                *receipt.submitted_at_ns <= clock_.now()) {
              pending->replacement_execution->exec_start_ns = *receipt.submitted_at_ns;
              pending->replacement_execution->last_runtime_scale_sample_ns =
                  *receipt.submitted_at_ns;
            } else {
              pending->error = ErrorDTO{
                  kErrorInvariantViolation, "Replacement submission time is unavailable", {}};
            }
            if (receipt.handle) {
              if (const auto invalid = prepared_execution_handle_field_error(
                      *receipt.handle, *pending->participant, binding.controller_goal_id,
                      binding.controller_goal_generation)) {
                pending->error = ErrorDTO{kErrorInvariantViolation,
                                          "Replacement handle crosses dispatch identity",
                                          {{"reason", std::string(*invalid)}}};
              } else {
                pending->replacement_execution->execution_handle = std::move(*receipt.handle);
                pending->backend_execution_generation =
                    pending->replacement_execution->execution_handle.execution_generation;
                pending->last_dispatched_participant = pending->participant;
                pending->replacement_lookahead_arm->execution_generation =
                    pending->backend_execution_generation;
              }
            } else {
              pending->error = receipt.command.error.value_or(
                  ErrorDTO{kErrorExecutionRejected, "Replacement send outcome is unresolved", {}});
            }
            notify_owner = pending->error.has_value();
          }
        }
      }
    }
    should_observe = pending->dispatch_gate.load(std::memory_order_acquire) ==
                         PreparedMotionDispatchGate::kCommitted &&
                     pending->state == ResolvedPreparedMotionExecutionStateDTO::kDispatchRequested;
    if (notify_owner) {
      notify_owner = !pending->replacement_failure_notified;
      pending->replacement_failure_notified = true;
    }
  }
  if (!should_observe) {
    if (notify_owner) notify();
    return false;
  }
  auto observed = execution_backend_.ObserveReplacement(source, pending->execution_id);
  {
    std::lock_guard lock(pending->mutex);
    if (observed.ok())
      pending->replacement_observation = std::move(observed.value());
    else {
      pending->error = observed.error();
      pending->replacement_observation = PreparedMotionReplacementObservationDTO{
          PreparedMotionReplacementPhaseDTO::kEffectUncertain, {}, {}, {}, observed.error()};
    }
    const auto& observation = *pending->replacement_observation;
    if (observation.phase == PreparedMotionReplacementPhaseDTO::kReadyToCommit &&
        pending->replacement_execution && pending->backend_execution_generation != 0U &&
        !pending->error) {
      // The ordinary worker cannot finalize its predecessor while this event
      // is dispatched. Parent callbacks are synchronous and must never wait
      // for this safe point while retaining addressed state.
      pending->worker_parked = true;
      notify_owner = true;
    } else if (observation.phase == PreparedMotionReplacementPhaseDTO::kEffectUncertain ||
               observation.phase == PreparedMotionReplacementPhaseDTO::kNoEffect) {
      notify_owner = !pending->replacement_failure_notified;
      pending->replacement_failure_notified = true;
    }
  }
  if (notify_owner) notify();
  std::unique_ptr<ActiveMotionExecution> retired;
  bool adopted = false;
  {
    std::lock_guard lock(pending->mutex);
    pending->worker_parked = false;
    adopted = pending->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched;
    if (adopted) retired = std::move(pending->replacement_execution);
  }
  if (retired && retired->execution_progress_generation != 0U)
    execution_backend_.retire_execution_progress_generation(retired->execution_progress_generation);
  if (adopted) {
    emit_event({KernelEventType::kExecuteStarted,
                current.execution_id,
                clock_.now(),
                {},
                {},
                std::nullopt,
                {{"reason", "motion_replacement_committed"}}});
  }
  return adopted;
}

Result<bool> ExecutionKernel::CommitPreparedMotionReplacement(
    const std::uint64_t candidate_id, const PreparedMotionReplacementSourceDTO& source,
    const PreparedMotionReplacementCommitDTO commit) {
  const auto reject = [](const char* reason) {
    return Result<bool>::Err(ErrorDTO{
        kErrorExecutionRejected, "Replacement adoption was refused", {{"reason", reason}}});
  };
  if (!commit.owner_commit) return reject("motion_replacement_commit_missing");
  // A scale submission may synchronously notify the addressed owner. Never
  // wait for it while that owner's state lock is held by this handover.
  std::unique_lock submission_lock(resolved_runtime_scale_submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock()) return Result<bool>::Ok(false);
  std::lock_guard owner_lock(prepared_motion_session_mutex_);
  auto pending = pending_prepared_motion_session_;
  auto incumbent = active_prepared_motion_session_;
  auto* current = active_motion_execution_;
  if (!pending || !incumbent || !current || pending->execution_id != candidate_id ||
      incumbent->execution_id != source.execution_id ||
      current->execution_id != source.execution_id)
    return reject("motion_replacement_source_stale");
  std::scoped_lock session_lock(pending->mutex, incumbent->mutex);
  if (!pending->worker_parked) return Result<bool>::Ok(false);
  const auto& retained = pending->authority.replacement->source;
  if (source.execution_id != retained.execution_id ||
      source.execution_generation != retained.execution_generation ||
      source.goal != retained.goal || source.binding_epoch != retained.binding_epoch ||
      source.prepared_contract_digest != retained.prepared_contract_digest ||
      pending->cancel_requested || !pending->replacement_execution ||
      !pending->replacement_correlation || !pending->replacement_projection ||
      !pending->replacement_lookahead_arm || pending->error || !pending->replacement_observation ||
      pending->replacement_observation->phase !=
          PreparedMotionReplacementPhaseDTO::kReadyToCommit ||
      addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == source.execution_id)
    return reject("motion_replacement_adoption_proof_incomplete");
  auto& next = *pending->replacement_execution;
  const auto* motion = std::get_if<PreparedMotionExecutionDTO>(&pending->participant->payload);
  if (!motion || !motion->plan_request.moving_replacement)
    return reject("motion_replacement_prepared_context_missing");
  const auto scene = check_motion_replacement_scene(*pending);
  if (!scene.ok()) return Result<bool>::Err(scene.error());
  std::lock_guard protection_lock(runtime_collision_protection_mutex_);
  const auto status = sm_.status();
  if (status.execution_id != source.execution_id || status.fact ||
      status.state != ExecutionState::kExecuting || !runtime_collision_protection_.active ||
      runtime_collision_protection_.execution_id != source.execution_id ||
      !runtime_collision_query_session_.load(std::memory_order_acquire))
    return reject("motion_replacement_runtime_protection_stale");
  {
    std::lock_guard projection_lock(runtime_collision_state_projection_mutex_);
    const auto& replacement = *pending->replacement_projection;
    if (!runtime_collision_state_projection_ ||
        runtime_collision_state_projection_->source_joint_names != replacement.source_joint_names ||
        runtime_collision_state_projection_->query_joint_names != replacement.query_joint_names ||
        runtime_collision_state_projection_->source_to_query_indices !=
            replacement.source_to_query_indices ||
        runtime_collision_state_projection_->template_positions_rad.size() !=
            replacement.template_positions_rad.size() ||
        current->request.contact_policy_id != next.request.contact_policy_id)
      return reject("motion_replacement_monitor_context_changed");
    for (std::size_t index = 0U; index < replacement.template_positions_rad.size(); ++index) {
      if (std::find(replacement.source_to_query_indices.begin(),
                    replacement.source_to_query_indices.end(),
                    index) == replacement.source_to_query_indices.end() &&
          runtime_collision_state_projection_->template_positions_rad[index] !=
              replacement.template_positions_rad[index])
        return reject("motion_replacement_monitor_context_changed");
    }
  }
  const auto* progress = execution_backend_.execution_progress_publication();
  const auto hazard =
      progress ? progress->hazard_authority_claimed(source.execution_generation) : std::nullopt;
  if (!hazard || *hazard) return reject("motion_replacement_source_hazard_authority");
  // All context copies and arm preparation happened before the CAS. The
  // backend validates its exact goal/Stop/association proofs, then calls this
  // immediate callback while holding its participant mutex.
  // The preparation budget ended at actual Replace submission. Recognition
  // and adoption retain the backend's existing effect-aware deadlines.
  // Prepare timeout values before backend adoption / owner CAS.
  next.exec_timeout_ns = static_cast<std::int64_t>(next.request.timeouts.execution_timeout_s * 1e9);
  next.zero_speed_hold_timeout_ns =
      next.request.timeouts.zero_speed_hold_timeout_s
          ? static_cast<std::int64_t>(*next.request.timeouts.zero_speed_hold_timeout_s * 1e9)
          : 0;
  next.execution_progress_generation = pending->backend_execution_generation;
  std::unique_lock scale_lock(resolved_runtime_scale_mutex_);
  std::optional<ResolvedRuntimeScaleCommandDTO> successor_scale;
  std::optional<ResolvedRuntimeScaleAcceptedDTO> successor_acceptance;
  if (next.request.initial_runtime_scale) {
    if (!accepted_resolved_runtime_scale_command_ ||
        !resolved_scale_target_matches(*accepted_resolved_runtime_scale_command_,
                                       current->request.binding) ||
        !resolved_scale_command_well_formed(*next.request.initial_runtime_scale) ||
        !resolved_scale_target_matches(*next.request.initial_runtime_scale, next.request.binding))
      return reject("motion_replacement_scale_authority_stale");
    if (runtime_scale_acceptance_sequence_ == std::numeric_limits<std::uint64_t>::max())
      return reject("runtime_scale_acceptance_sequence_exhausted");
    successor_scale = *next.request.initial_runtime_scale;
    successor_scale->expected_runtime_scale_generation = RuntimeScaleGeneration(0U);
    successor_scale->requested_runtime_scale_generation = RuntimeScaleGeneration(0U);
    successor_scale->requested_scale = accepted_resolved_runtime_scale_command_->requested_scale;
    successor_acceptance = make_scale_acceptance(
        *successor_scale, runtime_scale_acceptance_sequence_ + 1U, clock_.now());
  }
  const auto adopted = execution_backend_.AdoptReplacement(source, candidate_id, commit);
  if (!adopted.ok()) return Result<bool>::Err(adopted.error());
  static_assert(std::is_nothrow_swappable_v<ActiveMotionExecution>);
  std::swap(*current, next);
  current->prepared_session = pending;
  active_prepared_motion_session_ = pending;
  pending_prepared_motion_session_.reset();
  sm_.set_execution_id(candidate_id);
  if (successor_scale) {
    static_assert(std::is_nothrow_swappable_v<decltype(successor_scale)>);
    static_assert(std::is_nothrow_swappable_v<decltype(successor_acceptance)>);
    accepted_resolved_runtime_scale_command_.swap(successor_scale);
    accepted_resolved_runtime_scale_result_.swap(successor_acceptance);
    runtime_scale_acceptance_sequence_ = accepted_resolved_runtime_scale_result_->acceptance_sequence;
    current->request.initial_runtime_scale->requested_scale =
        accepted_resolved_runtime_scale_command_->requested_scale;
  }
  active_correlation_.swap_prepared(*pending->replacement_correlation);
  scale_lock.unlock();
  // Reuse the continuous immediate session/schedule: no teardown, no new
  // query engine and no contact-release re-baseline at handover.
  runtime_collision_protection_.execution_id = candidate_id;
  runtime_collision_protection_.generation = ++runtime_collision_protection_next_generation_;
  current->runtime_collision_protection_generation = runtime_collision_protection_.generation;
  if (auto lookahead = runtime_collision_lookahead_session_.load(std::memory_order_acquire))
    lookahead->publish_prepared_arm(std::move(*pending->replacement_lookahead_arm));
  pending->replacement_lookahead_arm.reset();
  pending->state = ResolvedPreparedMotionExecutionStateDTO::kDispatched;
  pending->replacement_observation->phase = PreparedMotionReplacementPhaseDTO::kCommitted;
  return Result<bool>::Ok(true);
}

Result<PreparedMotionReplacementSourceDTO> ExecutionKernel::CaptureMotionReplacementSource(
    const std::uint64_t execution_id) const {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<PreparedMotionReplacementSourceDTO>::Err(ErrorCode::kMissingReference);
  }
  std::lock_guard lock(session->mutex);
  const auto status = sm_.status();
  if (session->execution_id != execution_id || status.execution_id != execution_id ||
      status.state != ExecutionState::kExecuting || status.fact ||
      session->state != ResolvedPreparedMotionExecutionStateDTO::kDispatched ||
      !session->last_dispatched_participant || session->backend_execution_generation == 0U ||
      addressed_stop_fence_execution_id_.load(std::memory_order_acquire) == execution_id) {
    return Result<PreparedMotionReplacementSourceDTO>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Motion replacement source is no longer executing",
                 {{"reason", "motion_replacement_source_stale"}}});
  }
  const auto& request = session->authority.execution_request;
  const ExecutionGoalKey goal{request.binding.controller_goal_id,
                              request.binding.controller_goal_generation};
  auto captured = execution_backend_.CaptureReplacementSource(
      execution_id, session->backend_execution_generation, goal,
      session->last_dispatched_participant->prepared_contract_digest);
  if (!captured.ok()) return captured;
  const auto& source = captured.value();
  if (source.execution_id != execution_id ||
      source.execution_generation != session->backend_execution_generation || source.goal != goal ||
      source.prepared_contract_digest !=
          session->last_dispatched_participant->prepared_contract_digest ||
      source.binding_epoch != source.controller_sample.binding_epoch || !source.progress.valid ||
      source.progress.terminal ||
      source.progress.execution_generation != source.execution_generation ||
      source.progress.trajectory_time_ns < 0 ||
      source.progress.source_timestamp_ns != source.controller_sample.source_stamp_ns ||
      source.progress.received_timestamp_ns != source.controller_sample.local_receipt_stamp_ns)
    return Result<PreparedMotionReplacementSourceDTO>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Replacement source facts do not identify one sample",
                 {{"reason", "motion_replacement_source_sample_mismatch"}}});
  return captured;
}

Result<ResolvedPreparedMotionExecutionSnapshotDTO> ExecutionKernel::ObservePreparedMotion(
    const std::uint64_t execution_id) const {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
    if (!session || session->execution_id != execution_id)
      session = pending_prepared_motion_session_;
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
  if ((session->backend_execution_generation != 0U) !=
      static_cast<bool>(session->last_dispatched_participant))
    return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
        ErrorDTO{kErrorInvariantViolation,
                 "Dispatched motion source is inconsistent",
                 {{"reason", "prepared_motion_dispatch_source_invalid"}}});
  if (session->dispatch_gate.load(std::memory_order_acquire) ==
          PreparedMotionDispatchGate::kCommitted &&
      session->state != ResolvedPreparedMotionExecutionStateDTO::kDispatched &&
      !session->authority.replacement)
    return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
        ErrorDTO{kErrorInvariantViolation,
                 "A committed dispatch has not produced a verified motion source",
                 {{"reason", "prepared_motion_dispatch_outcome_unresolved"}}});
  const auto cursor =
      session->last_dispatched_participant
          ? execution_backend_.stopped_trajectory_time_s(session->backend_execution_generation)
          : std::nullopt;
  if (cursor) {
    const auto* motion = session->last_dispatched_participant
                             ? std::get_if<PreparedMotionExecutionDTO>(
                                   &session->last_dispatched_participant->payload)
                             : nullptr;
    if (session->backend_execution_generation == 0U || !std::isfinite(*cursor) || *cursor < 0.0 ||
        !motion || motion->trajectory.waypoints.empty() ||
        *cursor > motion->trajectory.waypoints.back().time_from_start_s)
      return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(
          ErrorDTO{kErrorInvariantViolation,
                   "Backend returned an invalid original trajectory cursor",
                   {{"reason", "prepared_motion_cursor_invalid"}}});
  }
  std::optional<ResolvedPreparedMotionSpringColdResultDTO> cold;
  if (session->spring_cold_product) {
    const auto& product = *session->spring_cold_product;
    cold = ResolvedPreparedMotionSpringColdResultDTO{
        product.reserved_execution_id,
        product.cancelled ? ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch
                          : product.state,
        product.participant, product.error};
  }
  return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(
      ResolvedPreparedMotionExecutionSnapshotDTO{
          session->execution_id, session->state, session->participant,
          session->backend_prepare_result, session->error, cursor,
          session->last_dispatched_participant, session->replacement_observation, std::move(cold)});
}

Result<void> ExecutionKernel::DispatchPreparedMotion(
    const std::uint64_t execution_id, const ContentDigest& prepared_contract_digest) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
    if (!session || session->execution_id != execution_id)
      session = pending_prepared_motion_session_;
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

Result<void> ExecutionKernel::CancelPreparedMotion(const std::uint64_t execution_id) {
  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
    if (!session || session->execution_id != execution_id)
      session = pending_prepared_motion_session_;
  }
  if (!session) {
    return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                      "Prepared motion session is not resident",
                                      {{"reason", "prepared_motion_session_missing"}}});
  }
  if (session->authority.replacement) {
    bool sent_refusal = false;
    {
      std::lock_guard lock(session->mutex);
      if (session->execution_id != execution_id)
        return Result<void>::Err(ErrorCode::kMissingReference);
      const bool no_effect =
          session->replacement_observation &&
          session->replacement_observation->phase == PreparedMotionReplacementPhaseDTO::kNoEffect;
      if (session->spring_cold_product) session->spring_cold_product->cancelled = true;
      if (session->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched ||
          (session->dispatch_gate.load(std::memory_order_acquire) ==
               PreparedMotionDispatchGate::kCommitted &&
           !no_effect))
        sent_refusal = true;
      if (!sent_refusal) {
        session->cancel_requested = true;
        session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
        auto open = PreparedMotionDispatchGate::kOpen;
        (void)session->dispatch_gate.compare_exchange_strong(
            open, PreparedMotionDispatchGate::kFencedBeforeDispatch, std::memory_order_acq_rel);
      }
    }
    session->replacement_cancellation.request_stop();
    session->changed.notify_all();
    if (sent_refusal)
      return Result<void>::Err(
          ErrorDTO{kErrorExecutionRejected,
                   "A possibly sent replacement requires the existing Stop owner",
                   {}});
    return Result<void>::Ok();
  }
  {
    std::lock_guard lock(session->mutex);
    if (session->execution_id != execution_id) {
      return Result<void>::Err(ErrorDTO{kErrorMissingReference,
                                        "Prepared motion execution identity is stale",
                                        {{"reason", "prepared_motion_execution_id_mismatch"}}});
    }
    if (session->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched ||
        session->dispatch_gate.load(std::memory_order_acquire) ==
            PreparedMotionDispatchGate::kCommitted) {
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

Result<bool> ExecutionKernel::RetireCancelledPreparedMotion(const std::uint64_t candidate_id) {
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire))
    return Result<bool>::Ok(false);
  std::shared_ptr<PreparedMotionSession> pending;
  std::shared_ptr<PreparedMotionSession> source;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    pending = pending_prepared_motion_session_;
    source = active_prepared_motion_session_;
  }
  // Replay acknowledges only this already completed old membership retirement;
  // it cannot cancel, dispatch or retire the promoted identity.
  for (const auto& resident : {pending, source}) {
    if (!resident) continue;
    std::lock_guard lock(resident->mutex);
    if (resident->spring_promotion_receipt &&
        resident->spring_promotion_receipt->abandoned_id == candidate_id &&
        resident->spring_promotion_receipt->promoted_id == resident->execution_id)
      return Result<bool>::Ok(true);
  }
  if (!pending || pending->execution_id != candidate_id)
    return Result<bool>::Err(ErrorCode::kMissingReference);
  std::optional<StopCertificateRefDTO> settled_certificate;
  {
    std::lock_guard lock(pending->mutex);
    const bool no_effect =
        pending->dispatch_gate.load(std::memory_order_acquire) ==
            PreparedMotionDispatchGate::kFencedBeforeDispatch ||
        (pending->replacement_observation &&
         pending->replacement_observation->phase == PreparedMotionReplacementPhaseDTO::kNoEffect);
    bool source_stop_settled = false;
    if ((!no_effect || pending->spring_cold_product) && source && pending->authority.replacement &&
        addressed_participant_stop_settlement_ && addressed_motion_stop_recovery_evidence_) {
      std::lock_guard source_lock(source->mutex);
      const auto& captured = pending->authority.replacement->source;
      const auto& settlement = *addressed_participant_stop_settlement_;
      const auto& evidence = *addressed_motion_stop_recovery_evidence_;
      const auto status = sm_.status();
      source_stop_settled =
          source->execution_id == captured.execution_id &&
          source->backend_execution_generation == captured.execution_generation &&
          source->participant &&
          source->participant->prepared_contract_digest == captured.prepared_contract_digest &&
          settlement.execution_id == captured.execution_id &&
          settlement.controller_goal_id == captured.goal.controller_goal_id &&
          settlement.controller_goal_generation == captured.goal.controller_goal_generation &&
          evidence.execution_id == captured.execution_id && evidence.backend_stop_confirmed &&
          evidence.aggregate_stop_certificate == settlement.aggregate_stop_certificate &&
          status.execution_id == captured.execution_id &&
          recovery_execution_state_locked(status) == ExecutionState::kStopped && status.fact &&
          status.fact->effect == ParticipantEffectKnowledge::kEffectSettled &&
          pending->backend_prepare_result &&
          pending->backend_prepare_result->state == PrepareParticipantResultStateDTO::kPrepared;
      if (source_stop_settled) settled_certificate = settlement.aggregate_stop_certificate;
    }
    if ((!pending->cancel_requested || !no_effect) && !source_stop_settled)
      return Result<bool>::Err(ErrorCode::kExecutionRejected);
    if (pending->spring_cold_product && !pending->spring_cold_product->cancelled &&
        !pending->cancel_requested) {
      const auto& spring = *pending->spring_cold_product->authority.replacement->spring_return;
      if (!settled_certificate || !spring.stage ||
          settled_certificate->stop_id != spring.stage->stop_id ||
          settled_certificate->stop_generation != spring.stage->stop_generation)
        return Result<bool>::Err(ErrorCode::kExecutionRejected);
    }
    if (!pending->preparation_completed) return Result<bool>::Ok(false);
    if (source_stop_settled && ((!worker_signals_.complete() && worker_thread_.joinable()) ||
                                completion_publication_execution_id_.load(
                                    std::memory_order_acquire) == source->execution_id))
      return Result<bool>::Ok(false);
  }
  if (replacement_preparation_thread_.joinable()) {
    if (replacement_preparation_thread_.get_id() == std::this_thread::get_id())
      return Result<bool>::Ok(false);
    lifecycle_worker_retirement_in_progress_.store(true, std::memory_order_release);
    lifecycle_lock.unlock();
    replacement_preparation_thread_.join();
    lifecycle_lock.lock();
    lifecycle_worker_retirement_in_progress_.store(false, std::memory_order_release);
  }
  if (pending->backend_prepare_result &&
      pending->backend_prepare_result->state == PrepareParticipantResultStateDTO::kPrepared) {
    // This also proves a sent candidate ended under the same scoped Stop;
    // source settlement alone never certifies the candidate's effect.
    const auto retired = execution_backend_.RetirePreparedExecution(candidate_id);
    if (!retired.ok()) return Result<bool>::Err(retired.error());
  }
  // Preparation may have acquired the hold before any backend product existed.
  // The worker has joined, so it cannot acquire a new hold after this release.
  const auto released = execution_backend_.EndReplacementPreparationScaleHold(candidate_id);
  if (!released.ok()) return Result<bool>::Err(released.error());
  std::lock_guard lock(prepared_motion_session_mutex_);
  if (pending_prepared_motion_session_ != pending)
    return Result<bool>::Err(ErrorCode::kStaleGeneration);
  std::lock_guard session_lock(pending->mutex);
  if (pending->spring_cold_product && !pending->spring_cold_product->cancelled &&
      !pending->cancel_requested && settled_certificate &&
      pending->spring_cold_product->state ==
          ResolvedPreparedMotionExecutionStateDTO::kColdPrepared &&
      pending->spring_cold_product->participant && pending->spring_cold_product->execution &&
      !pending->spring_cold_product->error) {
    auto cold = std::move(*pending->spring_cold_product);
    pending->spring_cold_product.reset();
    pending->spring_promotion_receipt.emplace(PreparedMotionSession::SpringPromotionReceipt{
        candidate_id, cold.reserved_execution_id, std::move(*settled_certificate)});
    pending->authority = std::move(cold.authority);
    pending->execution_id = cold.reserved_execution_id;
    pending->participant = std::move(cold.participant);
    pending->replacement_execution = std::move(cold.execution);
    pending->replacement_collision_trajectory = std::move(cold.collision_trajectory);
    pending->state = ResolvedPreparedMotionExecutionStateDTO::kColdPrepared;
    pending->backend_prepare_result.reset();
    pending->backend_execution_generation = 0U;
    pending->last_dispatched_participant.reset();
    pending->replacement_observation.reset();
    pending->replacement_correlation.reset();
    pending->replacement_lookahead_arm.reset();
    pending->replacement_projection.reset();
    pending->error.reset();
    pending->dispatch_requested = false;
    pending->worker_parked = false;
    pending->replacement_failure_notified = false;
    pending->dispatch_gate.store(PreparedMotionDispatchGate::kOpen, std::memory_order_release);
    return Result<bool>::Ok(true);
  }
  // Cancelled/failed data has no promotion authority. Its old physical debt
  // was nevertheless retired above by the same exact backend proof.
  pending_prepared_motion_session_.reset();
  return Result<bool>::Ok(true);
}

Result<std::uint64_t> ExecutionKernel::PrepareCapabilityCommand(
    ResolvedPreparedCapabilityCommandExecutionRequestDTO) {
  // Addressed commands have their own focused participant. Sharing this motion
  // lifecycle would make an arm Stop/Reset affect an unrelated gripper.
  return Result<std::uint64_t>::Err(ErrorCode::kUnsupportedCapability);
}
Result<ResolvedPreparedCapabilityCommandSnapshotDTO>
ExecutionKernel::ObservePreparedCapabilityCommand(std::uint64_t) const {
  return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(
      ErrorCode::kUnsupportedCapability);
}
Result<GripperCommandResultDTO> ExecutionKernel::DispatchPreparedCapabilityCommand(
    std::uint64_t, const ContentDigest&, const ControllerGoalId&, ControllerGoalGeneration) {
  return Result<GripperCommandResultDTO>::Err(ErrorCode::kUnsupportedCapability);
}
Result<void> ExecutionKernel::CancelPreparedCapabilityCommand(std::uint64_t) {
  return Result<void>::Err(ErrorCode::kUnsupportedCapability);
}

Result<uint64_t> ExecutionKernel::execute_impl(
    const ExecuteRequestDTO& request, std::shared_ptr<PreparedMotionSession> prepared_session) {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  const bool cold_reuse = prepared_session && prepared_session->authority.replacement &&
                         prepared_session->authority.replacement->spring_return;
  std::shared_ptr<const PreparedOperationParticipantDTO> cold_product;
  if (cold_reuse) {
    std::lock_guard owner_lock(prepared_motion_session_mutex_);
    std::lock_guard session_lock(prepared_session->mutex);
    if (pending_prepared_motion_session_ != prepared_session ||
        prepared_session->state != ResolvedPreparedMotionExecutionStateDTO::kColdPrepared ||
        !prepared_session->spring_activation_certificate || prepared_session->cancel_requested ||
        !prepared_session->participant || !prepared_session->replacement_execution)
      return Result<uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
          "Cold activation no longer owns its exact prepared product",
          {{"reason", "spring_return_activation_stale"}}});
    cold_product = prepared_session->participant;
  }
  clear_runtime_collision_state_projection();
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire)) {
    return Result<uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                          "Execution worker retirement is in progress",
                                          {{"reason_code", "worker_retirement_in_progress"}}});
  }

  const auto admission_status = sm_.status();
  if (admission_status.state != ExecutionState::kIdle && !admission_status.fact.has_value()) {
    return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
  }
  if (!retire_execute_worker(lifecycle_lock, cold_reuse)) {
    return Result<uint64_t>::Err(
        ErrorDTO{kErrorExecutionRejected,
                 "Execution cannot be started from the active execution worker",
                 {{"reason_code", "worker_retirement_self_thread"}}});
  }
  planning_reset_requested_.store(false, std::memory_order_release);

  if (const auto& fact = admission_status.fact; fact.has_value()) {
    // A computed completion is retired only by its acknowledgement. An
    // execution whose effect settlement is unknown requires an explicit
    // recovery action, never an implicit retirement by the next admission:
    // that would erase the operator intervention boundary with no audit event.
    if (fact->kind == ParticipantExecutionFactKind::kComputedCompletion) {
      return Result<uint64_t>::Err(ErrorCode::kExecutionRejected);
    }
    if (fact->effect == ParticipantEffectKnowledge::kEffectUncertain) {
      return Result<uint64_t>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Execution effect is uncertain; explicit recovery action required",
                   {{"requires_operator_intervention", "true"},
                    {"reason_code", "intervention_required"},
                    {"participant_fact", to_string(fact->kind)}}});
    }
    // Motion-free retirement of the previous ended execution.
    if (const auto retired = retire_prepared_motion_membership(
            cold_reuse ? prepared_session.get() : nullptr); !retired.ok()) {
      return Result<uint64_t>::Err(retired.error());
    }
    sm_.retire();
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
                admission_status.execution_id,
                clock_.now(),
                admission_status.state,
                ExecutionState::kIdle,
                std::nullopt,
                {{"reason", "ended_execution_retired_by_admission"}}});
  } else if (const auto retired = retire_prepared_motion_membership(
                 cold_reuse ? prepared_session.get() : nullptr); !retired.ok()) {
    return Result<uint64_t>::Err(retired.error());
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
  if (request.timeouts.zero_speed_hold_timeout_s.has_value() &&
      (!std::isfinite(*request.timeouts.zero_speed_hold_timeout_s) ||
       *request.timeouts.zero_speed_hold_timeout_s <= 0.0)) {
    return Result<uint64_t>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "zero_speed_hold_timeout_s must be finite and strictly positive when set",
                 {{"reason_code", "invalid_zero_speed_hold_timeout"},
                  {"zero_speed_hold_timeout_s",
                   std::to_string(*request.timeouts.zero_speed_hold_timeout_s)}}});
  }

  const uint64_t exec_id = cold_reuse ? prepared_session->execution_id
                                    : next_execution_id_.load(std::memory_order_relaxed);
  if (exec_id == std::numeric_limits<std::uint64_t>::max()) {
    return Result<uint64_t>::Err(ErrorDTO{kErrorExecutionRejected,
                                          "Execution identity space is exhausted",
                                          {{"reason", "execution_id_exhausted"}}});
  }
  // The previous prepared membership has retired. Its addressed Stop receipt
  // remains in the owner's history, but cannot own this new execution.
  addressed_motion_stop_recovery_evidence_.reset();
  addressed_participant_stop_settlement_.reset();
  addressed_stop_fence_execution_id_.store(0U, std::memory_order_release);
  if (!cold_reuse) next_execution_id_.store(exec_id + 1U, std::memory_order_relaxed);
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

  const auto finish_admission_without_worker = [&]() { finish_execute_worker(); };

  const auto reject_admission = [&](ErrorDTO error) -> Result<uint64_t> {
    const auto rejected_from_state = sm_.status().state;
    sm_.retire();
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

  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    return reject_admission(sync_result.error());
  }

  auto synchronized_scene = synchronize_scene_before_planning();
  if (!synchronized_scene.ok()) {
    return reject_admission(synchronized_scene.error());
  }

  auto retained_scene_result = [&]() -> Result<RetainedSceneSnapshotLease> {
    if (!cold_reuse) return retain_current_scene_snapshot_lease_inventory(exec_id, request.move_id);
    const auto current = current_scene_snapshot_lease_validation_context();
    std::lock_guard lock(prepared_session->mutex);
    const auto& staged = *prepared_session->replacement_execution;
    if (!current || current->live_projection_revision != staged.retained_scene.binding().projection_revision ||
        (staged.scene_snapshot_lease &&
         !validate_scene_snapshot_lease(*staged.scene_snapshot_lease, *current).accepted))
      return Result<RetainedSceneSnapshotLease>::Err(ErrorDTO{kErrorExecutionRejected,
          "Spring successor projection or retained lease changed before activation",
          {{"reason", "spring_return_projection_stale"}}});
    return Result<RetainedSceneSnapshotLease>::Ok(
        std::move(prepared_session->replacement_execution->retained_scene));
  }();
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
  if (synchronized_scene.value().has_value() &&
      !ProjectionIsSameOrForwardInLineage(projection_binding.projection_revision,
                                           *synchronized_scene.value())) {
    return reject_admission(
        ErrorDTO{kErrorExecutionRejected,
                 "Retained planner projection does not include the synchronized scene",
                 {{"reason", "planning_scene_synchronization_raced"}}});
  }
  ExecuteRequestDTO resolved_request(request);
  auto contact_grasp_binding = cold_reuse ? Result<void>::Ok()
      : install_resolved_contact_grasp_policy(resolved_request, retained_scene.mutable_inventory());
  if (!contact_grasp_binding.ok()) {
    return reject_admission(contact_grasp_binding.error());
  }
  set_active_request_correlation(resolved_request);
  std::optional<SceneSnapshotLease> scene_snapshot_lease;
  scene_snapshot_lease = cold_reuse ? prepared_session->replacement_execution->scene_snapshot_lease
      : build_scene_snapshot_lease(retained_scene.inventory(), exec_id);

  std::optional<RobotStateDTO> synced_state;
  auto observed_start = observed_state_.current_state(clock_.now());
  if (observed_start.ok()) {
    synced_state = std::move(observed_start.value());
  }

  std::optional<ProgramFrameResolutionContextDTO> resolution_context;
  const auto continuation = prepared_session ? prepared_session->authority.continuation : nullptr;
  if (!continuation && (request.target_mode == MotionTargetModeDTO::kRelative ||
                        request.goal_frame_reference.has_value())) {
    if (!synced_state.has_value()) {
      return reject_admission(
          ErrorDTO{kErrorStartStateMismatch,
                   "Referenced motion requires an authority-selected start state",
                   {{"reason", request.target_mode == MotionTargetModeDTO::kRelative
                                   ? "relative_start_state_unavailable"
                                   : "goal_frame_start_state_unavailable"}}});
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
    if (request.goal_frame_reference.has_value()) {
      requested_references.push_back(*request.goal_frame_reference);
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
  auto resolved_motion =
      continuation ? Result<ResolvedMotionSpecDTO>::Ok(continuation->motion)
                   : resolve_motion_target(
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
  if (continuation) resolved_request.cache_mode = CacheMode::kDisable;

  if (!synced_state.has_value()) {
    return reject_admission(ErrorDTO{kErrorStartStateMismatch,
                                     "Planning requires an authority-selected observed start state",
                                     {{"reason", "planning_start_state_unavailable"}}});
  }
  if (!cold_reuse) {
    auto dynamic_start_contacts = discover_dynamic_carried_start_contacts(
        retained_scene.mutable_inventory(), *synced_state, exec_id);
    if (!dynamic_start_contacts.ok()) return reject_admission(dynamic_start_contacts.error());
    resolved_request.dynamically_discovered_start_contact_count = dynamic_start_contacts.value();
  }

  PreparedExecutionStart prepared;
  if (cold_reuse) {
    const auto& motion = std::get<PreparedMotionExecutionDTO>(cold_product->payload);
    const auto start = cached_trajectory_matches_current_start(
        motion.trajectory, *synced_state, request.tolerances.joint_position_tol_rad);
    const auto goal = cached_trajectory_matches_request_goal(kinematics_, motion.trajectory, resolved_request);
    if (!start.compatible || !goal.compatible)
      return reject_admission(ErrorDTO{kErrorStartStateMismatch,
          "Spring successor no longer matches the settled start or admitted target",
          {{"reason", "spring_return_start_or_target_changed"}}});
    prepared.trajectory = motion.trajectory;
  }
  const auto active_collision_world_revision = retained_scene.inventory().bundle.world->revision;
  const TrajectoryCacheRevisionEvidence active_cache_evidence{
      projection_binding.projection_revision, active_collision_world_revision};
  const auto cache_key = trajectory_cache_key(resolved_request);
  if (!cold_reuse && resolved_request.cache_mode != CacheMode::kDisable) {
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

  const auto initial_state = (cold_reuse || prepared.cache_hit || prepared.cache_revalidation_candidate)
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
    if (cold_reuse && pending_prepared_motion_session_ == prepared_session)
      pending_prepared_motion_session_.reset();
  }

  auto worker_start_gate = std::make_shared<WorkerStartGate>();
  const auto worker_generation = ++execute_worker_generation_;
  worker_thread_ = std::jthread(
      [this, request_copy = std::move(resolved_request), exec_id,
       cached_trajectory = (cold_reuse || prepared.cache_hit || prepared.cache_revalidation_candidate)
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

  lifecycle_lock.unlock();
  event_scope.flush();
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    const auto status = sm_.status();
    const bool should_start_worker =
        status.execution_id == exec_id && !status.fact.has_value() &&
        (status.state == ExecutionState::kPlanning || status.state == ExecutionState::kValidating);
    if (!should_start_worker) {
      const bool same_worker = worker_generation == execute_worker_generation_;
      if (same_worker && worker_thread_.joinable()) {
        worker_thread_.request_stop();
      }
      if (same_worker) {
        worker_signals_.mark_complete();
      }
    }
    worker_start_gate->release();
  }
  return Result<uint64_t>::Ok(exec_id);
}

AddressedParticipantStopFenceDisposition ExecutionKernel::fence_addressed_participant_stop(
    const std::uint64_t execution_id) noexcept {
  if (execution_id == 0U) {
    return AddressedParticipantStopFenceDisposition::kUnknown;
  }
  addressed_stop_fence_execution_id_.store(execution_id, std::memory_order_release);
  revoke_runtime_collision_lookahead_clear_permits();

  std::shared_ptr<PreparedMotionSession> session;
  std::shared_ptr<PreparedMotionSession> pending;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
    pending = pending_prepared_motion_session_;
  }
  if (pending && pending->authority.replacement &&
      pending->authority.replacement->source.execution_id == execution_id) {
    auto open = PreparedMotionDispatchGate::kOpen;
    if (pending->dispatch_gate.compare_exchange_strong(
            open, PreparedMotionDispatchGate::kFencedBeforeDispatch, std::memory_order_acq_rel)) {
      {
        std::lock_guard pending_lock(pending->mutex);
        pending->cancel_requested = true;
        pending->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
      }
      pending->replacement_cancellation.request_stop();
      pending->changed.notify_all();
    }
  }
  if (!session || session->execution_id != execution_id) {
    return AddressedParticipantStopFenceDisposition::kUnknown;
  }

  auto expected = PreparedMotionDispatchGate::kOpen;
  if (session->dispatch_gate.compare_exchange_strong(
          expected, PreparedMotionDispatchGate::kFencedBeforeDispatch, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    {
      std::lock_guard session_lock(session->mutex);
      session->cancel_requested = true;
      session->state = ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    }
    session->changed.notify_all();
    try {
      request_active_planning_cancel();
      request_active_validation_cancel("addressed_stop_fenced_before_dispatch");
    } catch (...) {
      // The dispatch fence is already committed. Cancellation wakeups are
      // best-effort here and must not turn a noexcept Stop fence into process
      // termination.
    }
    return AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
  }
  return expected == PreparedMotionDispatchGate::kFencedBeforeDispatch
             ? AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch
             : AddressedParticipantStopFenceDisposition::kDispatchCommitted;
}

Result<AddressedParticipantStopSettlementDTO> ExecutionKernel::settle_addressed_participant_stop(
    const AddressedParticipantStopSettlementRequestDTO& request) {
  const auto& containment = request.containment;
  const auto& stop = request.aggregate_stop_certificate;
  const auto dispatched_containment_complete =
      !request.effect_prevented_before_dispatch &&
      containment.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
      !containment.error.has_value() &&
      containment.attempt_disposition != StopStageAttemptDispositionDTO::kNotDispatched &&
      std::all_of(
          containment.evidence_items.begin(), containment.evidence_items.end(),
          [](const auto& item) { return item.state == StopEvidenceItemStateDTO::kSatisfied; });
  const auto no_effect_cancellation_complete =
      request.effect_prevented_before_dispatch &&
      containment.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
      !containment.error.has_value() &&
      containment.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
      !containment.actual_provider_contract_mode.has_value() &&
      !containment.recoverable_substitution_reason.has_value() &&
      !containment.controller_acknowledgement_sequence.has_value() &&
      !containment.source_epoch.has_value() && !containment.source_sequence.has_value() &&
      containment.evidence_items.empty();
  if (request.execution_id == 0U || !stop.locally_valid() ||
      (!dispatched_containment_complete && !no_effect_cancellation_complete) ||
      containment.request.stop_id != stop.stop_id ||
      containment.request.stop_generation != stop.stop_generation) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "Addressed Stop settlement handoff is incomplete",
                 {{"reason", "addressed_participant_stop_settlement_handoff_invalid"}}});
  }

  if (request.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(ErrorCode::kUnsupportedCapability);
  }

  std::shared_ptr<PreparedMotionSession> session;
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    session = active_prepared_motion_session_;
  }
  if (!session) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Addressed Stop names no resident prepared motion",
                 {{"reason", "addressed_motion_stop_session_missing"}}});
  }

  EventDeferralScope event_scope(*this);
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard session_lock(session->mutex);
    const auto& binding = session->authority.execution_request.binding;
    const bool expected_session_state =
        request.effect_prevented_before_dispatch
            ? session->state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch
            : session->state == ResolvedPreparedMotionExecutionStateDTO::kDispatched;
    if (session->execution_id != request.execution_id || !expected_session_state ||
        containment.request.operation_id != binding.operation_id ||
        containment.request.participant_id != binding.participant_id ||
        containment.request.controller_goal_id != binding.controller_goal_id ||
        containment.request.controller_goal_generation != binding.controller_goal_generation) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(
          ErrorDTO{kErrorMissingReference,
                   "Addressed Stop names a stale prepared motion",
                   {{"reason", "addressed_motion_stop_execution_mismatch"},
                    {"requested_execution_id", std::to_string(request.execution_id)},
                    {"current_execution_id", std::to_string(session->execution_id)}}});
    }
    if (!request.effect_prevented_before_dispatch) {
      const auto valid_containment = ValidateParticipantStopStageResult(
          containment, session->authority.stop_readiness.value().resolved_stop_base_plan,
          session->authority.binding, session->authority.retained_default_stop_limits,
          session->authority.stop_node_category, binding.resources);
      if (!valid_containment.ok()) {
        return Result<AddressedParticipantStopSettlementDTO>::Err(valid_containment.error());
      }
    }
  }

  const auto before = sm_.status();
  if (before.execution_id != request.execution_id) {
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorMissingReference,
                 "Addressed Stop does not match the current motion execution",
                 {{"reason", "addressed_motion_stop_execution_mismatch"},
                  {"requested_execution_id", std::to_string(request.execution_id)},
                  {"current_execution_id", std::to_string(before.execution_id)}}});
  }
  if (addressed_participant_stop_settlement_.has_value()) {
    if (addressed_participant_stop_settlement_->execution_id == request.execution_id &&
        addressed_participant_stop_settlement_->aggregate_stop_certificate == stop) {
      return Result<AddressedParticipantStopSettlementDTO>::Ok(
          *addressed_participant_stop_settlement_);
    }
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorInvalidRequest,
                 "A different Stop already owns the current motion execution",
                 {{"reason", "addressed_motion_stop_identity_conflict"}}});
  }
  // The participant only records the addressed owner's containment for its
  // ended execution; the owner commits the operation outcome. A pre-dispatch
  // cancellation must already carry its cancelled-before-dispatch fact. A
  // dispatched execution may still be ending, may have ended by the fenced
  // Stop, or may have completed just before the Stop was accepted; a local
  // failure fact keeps its own cause rather than becoming an ordinary Stop.
  const auto& fact = before.fact;
  const bool contained_failure = fact && is_settled_execution_failure(*fact);
  if (request.effect_prevented_before_dispatch) {
    if (!fact.has_value() || fact->kind != ParticipantExecutionFactKind::kCancelledBeforeDispatch) {
      return Result<AddressedParticipantStopSettlementDTO>::Err(
          ErrorDTO{kErrorInvalidRequest,
                   "Pre-dispatch cancellation has not reported its fact",
                   {{"reason", "addressed_participant_stop_settlement_not_ready"},
                    {"participant_fact", fact.has_value() ? to_string(fact->kind) : "none"}}});
    }
  } else if (before.state != ExecutionState::kExecuting ||
             (fact.has_value() &&
              fact->kind != ParticipantExecutionFactKind::kComputedCompletion &&
              fact->kind != ParticipantExecutionFactKind::kPreemptedByAddressedStop &&
              !contained_failure)) {
    std::map<std::string, std::string> details{{"reason", "addressed_motion_stop_state_invalid"},
                                               {"state", to_string(before.state)}};
    if (fact.has_value()) {
      details["participant_fact"] = to_string(fact->kind);
      details["participant_effect"] = to_string(fact->effect);
    }
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorInvalidRequest, "Addressed Stop cannot settle the current motion execution",
                 std::move(details)});
  }

  control_command_epoch_.fetch_add(1, std::memory_order_acq_rel);
  revoke_runtime_collision_lookahead_clear_permits();
  const auto stop_owner_generation = begin_operator_stop_owner_generation(request.execution_id);
  // For a pre-dispatch cancellation, true means the participant backend
  // confirmed effect containment by preserving the dispatch fence; the
  // request's kNotDispatched disposition retains the distinction from a
  // physical Stop command in immutable aggregate history.
  addressed_motion_stop_recovery_evidence_ = AddressedMotionStopRecoveryEvidenceDTO{
      request.execution_id, stop_owner_generation, stop, true};
  if (!fact.has_value()) {
    stop_collision_monitoring(RuntimeCollisionMonitorRelease::kTerminalQuiesced);
    (void)settle_execution(request.execution_id, active_correlation_.move_id(),
                           ParticipantExecutionFact{
                               ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                               ParticipantEffectKnowledge::kEffectSettled, std::nullopt},
                           "addressed_stop_participant_settlement",
                           execution_backend_.get_execution_snapshot());
  }
  auto assessment = refresh_recovery_assessment_locked(
      request.effect_prevented_before_dispatch ? "addressed_cancelled_before_dispatch_settlement"
                                               : "addressed_stopped_participant_settlement");
  const bool recovery_matches = contained_failure
                                    ? assessment.execution_state == ExecutionState::kFailed &&
                                          !assessment.retry_admissible
                                    : assessment.execution_state == ExecutionState::kStopped &&
                                          assessment.recovery_class == RecoveryClass::kOrdinaryOperatorStop &&
                                          assessment.status == RecoveryAssessmentStatus::kRetryReady &&
                                          assessment.retry_admissible;
  if (assessment.execution_id != request.execution_id || !recovery_matches ||
      !assessment.backend_stop_confirmed ||
      assessment.stop_certificate_id != stop.stop_certificate_id.value() ||
      assessment.stop_certificate_revision != stop.stop_certificate_revision.value() ||
      assessment.stop_owner_generation != stop_owner_generation) {
    addressed_motion_stop_recovery_evidence_.reset();
    (void)refresh_recovery_assessment_locked("addressed_stopped_settlement_rejected");
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{kErrorInvariantViolation,
                 "Addressed Stop local recovery settlement is incomplete",
                 {{"reason", "addressed_motion_stop_recovery_settlement_invalid"}}});
  }
  addressed_participant_stop_settlement_ = AddressedParticipantStopSettlementDTO{
      request.execution_id,
      stop,
      containment.request.operation_id,
      containment.request.participant_id,
      containment.request.controller_goal_id,
      containment.request.controller_goal_generation,
      std::move(assessment), contained_failure ? fact : std::nullopt};
  return Result<AddressedParticipantStopSettlementDTO>::Ok(*addressed_participant_stop_settlement_);
}

bool ExecutionKernel::control_command_epoch_matches(
    std::optional<std::uint64_t> expected_epoch) const {
  return !expected_epoch.has_value() ||
         *expected_epoch == control_command_epoch_.load(std::memory_order_acquire);
}

ResetResultDTO ExecutionKernel::reset_after_certified_containment(
    const std::uint64_t execution_id, std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  const LifecyclePreemption preemption(*this);
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
  // retires local execution machinery and the prepared membership; it never
  // resets the backend, commands motion, issues another Stop or waits for
  // another Stop policy decision. Robot positions are preserved.
  revoke_runtime_collision_lookahead_clear_permits();
  planning_reset_requested_.store(true, std::memory_order_release);
  request_active_planning_cancel();
  request_active_validation_cancel("certified_containment_reset");
  stop_collision_monitoring(RuntimeCollisionMonitorRelease::kResetOrTeardown, std::nullopt,
                            /*drain_callbacks=*/true);
  if (!retire_execute_worker(lifecycle_lock, /*containment_already_certified=*/true)) {
    return ResetResultDTO{false, ErrorDTO{kErrorInvalidRequest,
                                          "Execution worker cannot retire from its own callback",
                                          {{"reason_code", "worker_retirement_self_thread"}}}};
  }
  if (!control_command_epoch_matches(expected_epoch)) {
    return ResetResultDTO{false, stale_control_command_error()};
  }
  auto retired = retire_prepared_motion_membership();
  if (!retired.ok() && retired.error().details.contains("reason") &&
      retired.error().details.at("reason") == "prepared_execution_not_quiescent") {
    // Item 12: Reset issues the lane's hard stop and then completes, as
    // `main`'s Reset did; it never leaves an unresolved retirement behind
    // while the backend is still winding down. The wait is `main`'s bounded
    // software-stop drain.
    try {
      (void)execution_backend_.stop();
    } catch (...) {
      // Retirement below reports whether the backend became quiescent.
    }
    for (int poll = 0; poll < kExecutionDrainMaxPolls && !retired.ok(); ++poll) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(kExecutionPollNs));
      retired = retire_prepared_motion_membership();
    }
  }
  if (!retired.ok()) {
    return ResetResultDTO{false, retired.error()};
  }
  sm_.retire();
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
  if (recovery_freshness_deadline_handle_ != 0) {
    scheduler_.cancel(recovery_freshness_deadline_handle_);
    recovery_freshness_deadline_handle_ = 0;
    recovery_freshness_deadline_ns_ = 0;
  }
  addressed_motion_stop_recovery_evidence_.reset();
  addressed_participant_stop_settlement_.reset();
  addressed_stop_fence_execution_id_.store(0U, std::memory_order_release);
  recovery_readiness_tracker_.reset();
  planning_reset_requested_.store(false, std::memory_order_release);
  (void)refresh_recovery_assessment_locked("certified_containment_reset_accepted");
  return ResetResultDTO{true, std::nullopt};
}

AcknowledgeCompletedResultDTO ExecutionKernel::acknowledge_completed(
    uint64_t execution_id, std::optional<std::uint64_t> expected_epoch) {
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);

  if (!control_command_epoch_matches(expected_epoch)) {
    return AcknowledgeCompletedResultDTO{false, stale_control_command_error()};
  }
  const auto status = sm_.status();
  if (!status.fact.has_value() ||
      status.fact->kind != ParticipantExecutionFactKind::kComputedCompletion) {
    return AcknowledgeCompletedResultDTO{
        false, ErrorDTO{kErrorInvalidRequest,
                        "Can only acknowledge a computed completion",
                        {{"reason_code", "wrong_state"},
                         {"state", to_string(status.state)},
                         {"participant_fact",
                          status.fact.has_value() ? to_string(status.fact->kind) : "none"}}}};
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

  if (!retire_execute_worker(lifecycle_lock)) {
    return AcknowledgeCompletedResultDTO{
        false, ErrorDTO{kErrorExecutionRejected,
                        "Completed worker has not retired",
                        {{"reason_code", reason_codes::kTerminalPublicationInProgress},
                         {"retryable", "true"}}}};
  }
  if (!control_command_epoch_matches(expected_epoch)) {
    return AcknowledgeCompletedResultDTO{false, stale_control_command_error()};
  }
  if (const auto retired = retire_prepared_motion_membership(); !retired.ok()) {
    return AcknowledgeCompletedResultDTO{false, retired.error()};
  }
  sm_.retire();
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
              status.state,
              ExecutionState::kIdle,
              std::nullopt,
              {{"reason", "computed_completion_acknowledged"}}});
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
  const auto recovery_state = recovery_execution_state_locked(status);
  if (principal_id.empty()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "acknowledge_and_continue requires principal_id",
                                      {{"state", to_string(recovery_state)}}});
  }
  if (recovery_state != ExecutionState::kInterventionRequired) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "acknowledge_and_continue is valid only from "
                                      "InterventionRequired",
                                      {{"state", to_string(recovery_state)}}});
  }

  // apply_recovery_action_impl is the sole caller. Its coherent assessment
  // transaction has already classified the intervention and checked the
  // qualified observed-state source. Do not re-read the backend here: that
  // would create a second authority and could block while lifecycle_mutex_ is
  // held. This helper owns mutation only.
  auto snapshot = snapshot_publisher_.latest();
  sm_.retire();

  const auto prev_state = recovery_state;
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
  const auto recovery_state = recovery_execution_state_locked(status);
  if (principal_id.empty()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "retry_after_scene_repair requires principal_id",
                                      {{"state", to_string(recovery_state)}}});
  }
  if (recovery_state != ExecutionState::kFailed) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "retry_after_scene_repair is valid only from Failed",
                                      {{"state", to_string(recovery_state)}}});
  }

  // The assessment transaction already classified the published failure as a
  // retryable validation boundary or a never-dispatched prepared failure.
  // Consume that immutable publication rather than issuing a backend RPC
  // under lifecycle_mutex_.
  const auto failed_snapshot = snapshot_publisher_.latest();
  if (!failed_snapshot.error.has_value()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "Cannot retry after scene repair: missing classified "
                                      "validation error",
                                      {{"reason", "failed_validation_error_missing"}}});
  }
  if (!is_retryable_failed_validation_error(*failed_snapshot.error) &&
      !failed_preparation_never_dispatched_locked()) {
    return Result<void>::Err(ErrorDTO{kErrorInvalidRequest,
                                      "Cannot retry after scene repair: failure is not a retryable "
                                      "pre-dispatch validation boundary",
                                      {{"reason", "failed_validation_not_retryable"},
                                       {"error_code", failed_snapshot.error->code}}});
  }

  auto backend_snapshot = failed_snapshot;
  sm_.retire();

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
               {"prev_state", to_string(recovery_state)},
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

std::uint64_t ExecutionKernel::begin_operator_stop_owner_generation(std::uint64_t execution_id) {
  std::lock_guard<std::mutex> lock(operator_stop_identity_mutex_);
  return operator_stop_identity_.begin(execution_id);
}

std::uint64_t ExecutionKernel::operator_stop_owner_generation_for_execution(
    std::uint64_t execution_id) const {
  std::lock_guard<std::mutex> lock(operator_stop_identity_mutex_);
  return operator_stop_identity_.generation_for_execution(execution_id);
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
  capture.control_command_epoch = control_command_epoch_.load(std::memory_order_acquire);
  const auto status = sm_.status();
  capture.execution_id = status.execution_id;
  capture.execution_state = recovery_execution_state_locked(status);
  if (status.fact) capture.terminal_effect = status.fact->effect;
  capture.terminal_cleanup_complete =
      (!worker_thread_.joinable() || worker_signals_.complete()) &&
      completion_publication_execution_id_.load(std::memory_order_acquire) != status.execution_id;
  capture.operator_stop_owner_generation =
      operator_stop_owner_generation_for_execution(capture.execution_id);
  capture.stronger_authority_active =
      lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(recovery_certificate_mutex_);
    capture.fault_preempt_certificate =
        recovery_certificates_.authoritative(StopOwnerKind::kFaultPreemptWorker);
    capture.operator_stop_certificate =
        recovery_certificates_.authoritative(StopOwnerKind::kOperatorStopWorker);
  }
  capture.addressed_motion_stop_evidence = addressed_motion_stop_recovery_evidence_;
  const auto& fact_error =
      status.fact.has_value() ? status.fact->error : std::optional<ErrorDTO>{};
  if (capture.execution_state == ExecutionState::kInterventionRequired) {
    capture.stop_fault_intervention_classified =
        fact_error.has_value() && is_classified_stop_intervention(fact_error->details);
  }
  if (capture.execution_state == ExecutionState::kFailed) {
    const auto never_dispatched = failed_preparation_never_dispatched_locked();
    capture.failed_preparation_never_dispatched = never_dispatched != nullptr;
    if (status.fact.has_value()) {
      capture.failed_execution_effect = status.fact->effect;
    }
    if (never_dispatched) {
      std::lock_guard session_lock(never_dispatched->mutex);
      capture.failed_validation_conclusive_collision =
          never_dispatched->conclusive_collision_failure;
    }
    capture.failed_validation_retryable =
        (fact_error.has_value() && is_retryable_failed_validation_error(*fact_error)) ||
        capture.failed_preparation_never_dispatched;
    if (fact_error.has_value()) {
      capture.failed_validation_code = fact_error->code;
      auto it = fact_error->details.find("reason");
      if (it != fact_error->details.end()) {
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

ExecutionState ExecutionKernel::recovery_execution_state_locked(
    const ExecutionStatus& status) const {
  // Recovery classification still lives here until it moves to the addressed
  // Kernel; it reads the participant facts, never an operation outcome the
  // participant committed.
  if (!status.fact.has_value()) {
    return status.state;
  }
  if (addressed_motion_stop_recovery_evidence_.has_value() &&
      addressed_motion_stop_recovery_evidence_->execution_id == status.execution_id &&
      status.fact->kind != ParticipantExecutionFactKind::kLocalFailure) {
    return ExecutionState::kStopped;
  }
  switch (status.fact->kind) {
    case ParticipantExecutionFactKind::kComputedCompletion:
      return ExecutionState::kCompleted;
    case ParticipantExecutionFactKind::kCancelledBeforeDispatch:
    case ParticipantExecutionFactKind::kPreemptedByAddressedStop:
    case ParticipantExecutionFactKind::kPreemptedByReplacement:
      return ExecutionState::kStopped;
    case ParticipantExecutionFactKind::kLocalFailure:
      return status.fact->effect == ParticipantEffectKnowledge::kEffectUncertain
                 ? ExecutionState::kInterventionRequired
                 : ExecutionState::kFailed;
  }
  return ExecutionState::kInterventionRequired;
}

KernelRecoveryAssessmentDTO ExecutionKernel::refresh_recovery_assessment_locked(
    const char* trigger) {
  recovery_assessment_dirty_.store(false, std::memory_order_release);
  return publish_recovery_assessment_locked(capture_recovery_authority_locked(), trigger);
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
    // extends its own freshness deadline. Publish that deadline and keep the
    // expiry schedule current without changing the operator-visible
    // assessment revision; readers must not see an elapsed deadline while
    // the observation is still fresh.
    if (next.observed_state_valid_until_ns != previous.observed_state_valid_until_ns) {
      std::lock_guard<std::mutex> lock(recovery_assessment_mutex_);
      if (recovery_assessment_.assessment_revision == previous.assessment_revision) {
        recovery_assessment_.observed_state_valid_until_ns = next.observed_state_valid_until_ns;
        previous.observed_state_valid_until_ns = next.observed_state_valid_until_ns;
      }
    }
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
  // Deferred callbacks may re-enter lifecycle methods. Destroy the lifecycle
  // lock before the event scope dispatches them, as in the other owner paths.
  EventDeferralScope event_scope(*this);
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
  if (!lifecycle_lock.owns_lock()) {
    recovery_assessment_dirty_.store(true, std::memory_order_release);
    schedule_recovery_assessment_refresh(trigger);
    return;
  }
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
            EventDeferralScope event_scope(*this);
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
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

namespace {

/// Retry classes whose published observation deadline the Kernel keeps
/// current (polling newer samples) and enforces (republishing at expiry).
/// A retry-admissible failed validation, including a never-dispatched
/// preparation failure, and an ordinary operator Stop follow the same
/// freshness rule as the other Stop-backed retry classes: a published
/// deadline is never left to elapse silently.
[[nodiscard]] bool recovery_observation_tracked(const KernelRecoveryAssessmentDTO& assessment) {
  return assessment.recovery_class == RecoveryClass::kRuntimeCollision ||
         assessment.recovery_class == RecoveryClass::kStopFaultIntervention ||
         assessment.recovery_class == RecoveryClass::kOrdinaryOperatorStop ||
         (assessment.recovery_class == RecoveryClass::kFailedValidation &&
          assessment.retry_admissible);
}

}  // namespace

void ExecutionKernel::schedule_recovery_freshness_deadline_locked(
    const KernelRecoveryAssessmentDTO& assessment) {
  const bool observation_dependent = recovery_observation_tracked(assessment);
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
        EventDeferralScope event_scope(*this);
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        (void)refresh_recovery_assessment_locked("observed_state_freshness_deadline");
      },
      SchedulerPriority::Monitoring);
  recovery_freshness_deadline_ns_ = deadline_ns;
}

void ExecutionKernel::schedule_recovery_observation_poll_locked(
    const KernelRecoveryAssessmentDTO& assessment) {
  const bool observation_dependent = recovery_observation_tracked(assessment);
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
        EventDeferralScope event_scope(*this);
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        recovery_observation_poll_handle_ = 0;
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
  const LifecyclePreemption preemption(*this);
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
    case RecoveryAction::kRetryMotionFromCurrentState:
      // The participant keeps no local fault latch any more; this action is
      // never offered by its assessment.
      return reject(reason_codes::kRecoveryActionNotAllowed);
    case RecoveryAction::kCloseOperation:
    case RecoveryAction::kReplanFromStoppedState: {
      if (request.action == RecoveryAction::kCloseOperation) {
        if (!retire_execute_worker(lifecycle_lock, /*containment_already_certified=*/true))
          return reject(reason_codes::kTerminalPublicationInProgress);
        capture = capture_recovery_authority_locked();
        assessment = publish_recovery_assessment_locked(capture, "close_operation_recheck");
        const auto close_decision = evaluate_recovery_admission(request, capture, assessment);
        if (!close_decision.admitted) return reject(close_decision.rejection_reason);
        const auto retired = retire_prepared_motion_membership();
        if (!retired.ok()) return reject("operation_close_cleanup_failed");
      }
      // Motion-free local retirement ONLY: the participant returns to Idle;
      // no backend reset or motion command is issued on any accepted action.
      sm_.retire();
      addressed_motion_stop_recovery_evidence_.reset();
      addressed_participant_stop_settlement_.reset();
      addressed_stop_fence_execution_id_.store(0U, std::memory_order_release);
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
                  capture.execution_state,
                  ExecutionState::kIdle,
                  std::nullopt,
                  {{"reason", request.action == RecoveryAction::kCloseOperation
                                  ? "operation_closed" : "stopped_replan_acknowledged"}}});
      accepted_from_state = capture.execution_state;
      active_correlation_.clear_program_run_and_action_span();
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
  if (status.fact.has_value() &&
      status.fact->kind == ParticipantExecutionFactKind::kComputedCompletion &&
      completion_publication_execution_id_.load(std::memory_order_acquire) == status.execution_id) {
    std::lock_guard<std::mutex> lock(completion_publication_status_mutex_);
    if (completion_publication_previous_status_.execution_id == status.execution_id) {
      status = completion_publication_previous_status_;
    }
  }
  status.active_move_id = active_correlation_.move_id();
  return status;
}

ExecutionSnapshot ExecutionKernel::get_execution_snapshot() const {
  const auto kernel_status = get_status();
  const auto kernel_state = kernel_status.state;
  // Read the live backend while idle so continuously observed physical state
  // is available before the first plan, and while executing where serving
  // the published cache would hide moving state. An ended execution serves
  // its published final snapshot. The participant remains the phase
  // authority: state and execution identity are stamped from its phase
  // machine, never trusted from the backend's view.
  if ((kernel_state == ExecutionState::kExecuting && !kernel_status.fact.has_value()) ||
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
  if (kernel_status.fact.has_value() || kernel_state == ExecutionState::kIdle) {
    snapshot.state = kernel_state;
    snapshot.execution_id = kernel_status.execution_id;
    snapshot.move_id = active_correlation_.move_id();
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
  if (const auto status = sm_.status();
      status.state != ExecutionState::kExecuting || status.fact.has_value()) {
    return Result<void>::Ok();
  }

  const auto tick_start_ns = clock_.now();
  runtime_collision_ticks_started_.fetch_add(1, std::memory_order_relaxed);
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
      [this, &record_tick_metric](CollisionCheckResultDTO check,
                                  std::map<std::string, std::string> details) -> Result<void> {
    // A future check is authorized only by the most recent clear immediate
    // tick. Withdraw that authority before any fault/stop evidence is
    // emitted; permit generations make queued and in-flight work stale.
    revoke_runtime_collision_lookahead_clear_permits();
    record_tick_metric(true);
    details["surface"] = "observed_runtime_monitor";
    // Contact versus monitor unavailability: the addressed Kernel escalates
    // an active Stop only on contact.
    details["in_collision"] = check.in_collision ? "true" : "false";
    emit_contact_policy_violation_event(sm_.status().execution_id, details, "preempt_trajectory");

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
    // The collision is reported as a fact only. The addressed Kernel selects
    // containment, or escalates an active Stop; the participant keeps no
    // local fault latch and issues no command.
    return Result<void>::Ok();
  };

  const auto query_session = runtime_collision_query_session_.load(std::memory_order_acquire);
  if (!query_session) {
    // Dispatch requires a prepared session, so an armed tick without one is
    // an invariant violation. It still fails closed through the monitor fault.
    std::map<std::string, std::string> details{
        {"reason_code", "runtime_collision_monitor"},
        {"reason", "runtime_collision_query_session_unavailable"},
        {"error_code", kErrorInvariantViolation}};
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
                                                       {"reason", "stale_observed_robot_state"},
                                                       {"cause", "stale_observed_robot_state"},
                                                       {"monitor_reason", observed_timestamp_invalid ?
                                                           "invalid_observed_robot_timestamp" : "stale_observed_robot_state"},
                                                       {"sample_age_ns", observed_timestamp_invalid ?
                                                           "unknown" : std::to_string(observed_age_ns)}};
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
  if (outcome.status == RuntimeCollisionTickStatus::kClear) {
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
    std::uint32_t planning_attempt_index, const RobotStateDTO* start_state_override,
    const MovingReplacementInputDTO* replacement) {
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
          : nullptr,
      replacement ? std::optional<MovingReplacementInputDTO>{*replacement} : std::nullopt});
}

Result<ExecutionKernel::PlanningSuccess> ExecutionKernel::plan_trajectory_with_evidence(
    const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
    std::uint32_t planning_attempt_index, const RobotStateDTO* start_state_override,
    const MovingReplacementInputDTO* replacement,
    const std::optional<std::int64_t> suffix_planning_deadline_ns) {
  auto plan_request = make_plan_request(request, projection_inventory, planning_attempt_index,
                                        start_state_override, replacement);
  if (!plan_request.ok()) {
    return Result<PlanningSuccess>::Err(plan_request.error());
  }

  if (suffix_planning_deadline_ns) {
    const auto remaining = *suffix_planning_deadline_ns - clock_.now();
    if (remaining <= 0 || !plan_request.value().moving_replacement)
      return Result<PlanningSuccess>::Err(
          ErrorDTO{kErrorPlanningTimeout,
                   "Moving suffix planning budget expired",
                   {{"reason", "motion_replacement_planning_budget_expired"}}});
    plan_request.value().moving_replacement->remaining_planning_budget_s =
        static_cast<double>(remaining) / 1e9;
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

Result<ExecutionKernel::PlanningSuccess> ExecutionKernel::plan_motion_continuation(
    ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
    const detail::MotionResumeIntent& continuation) {
  auto sync_result = execution_backend_.sync_observed_state_before_planning();
  if (!sync_result.ok()) {
    auto error = sync_result.error();
    error.details["resume_phase"] = "state_sync";
    return Result<PlanningSuccess>::Err(std::move(error));
  }

  try {
    auto fresh_start = observed_state_.current_state(clock_.now());
    if (!fresh_start.ok()) return Result<PlanningSuccess>::Err(fresh_start.error());
    request.motion = continuation.motion;
    if (std::holds_alternative<CartesianPathDTO>(continuation.motion)) {
      auto tcp = kinematics_.forward_kinematics(
          FkRequestDTO{fresh_start.value(), compiled_frame_name(request.binding.tcp_frame),
                       compiled_frame_name(request.binding.base_frame)});
      if (!tcp.ok()) return Result<PlanningSuccess>::Err(tcp.error());
      request.motion = detail::resume_motion_from_current_tcp(continuation, tcp.value());
    }
    auto planned =
        plan_trajectory_with_evidence(request, projection_inventory, 0U, &fresh_start.value());
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
    const RobotStateDTO* start_state_override, const std::stop_token external_stop_token,
    bool* conclusive_collision_failure,
    std::shared_ptr<CollisionCancellationToken> validation_cancellation,
    const std::optional<std::uint64_t> validation_execution_id,
    const MovingReplacementInputDTO* replacement) {
  if (conclusive_collision_failure != nullptr) {
    *conclusive_collision_failure = false;
  }
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

  const bool registers_active_cancellation = !validation_cancellation;
  auto cancel = validation_cancellation ? std::move(validation_cancellation)
                                        : install_validation_cancel_token();
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
  } cancel_guard{registers_active_cancellation ? this : nullptr, cancel};

  // Full non-collision trajectory validation (including TCP speed limit per
  // spec 230 §3.3). The manyforge_collision pipeline is the only
  // collision authority, so the backend validator must not run its legacy
  // collision sweep first and reject contacts the active policy would allow.
  ValidationOptionsDTO opts;
  opts.tcp_frame = compiled_frame_name(request.binding.tcp_frame);
  opts.base_frame = compiled_frame_name(request.binding.base_frame);
  opts.max_tcp_linear_speed_mps = request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
  opts.run_collision_checks = false;
  if (replacement) {
    opts.retained_prefix = RetainedTrajectoryPrefixValidationOptionsDTO{
        replacement->divergence_cursor_s - replacement->source_cursor_s,
        replacement->source_speed_profile.max_tcp_linear_speed_mps.value_or(0.0),
        compiled_frame_name(replacement->source_tcp_frame),
        compiled_frame_name(replacement->source_base_frame)};
  }
  const auto noncollision_started = replacement ? clock_.now() : 0;
  auto val_result = trajectory_validator_.validate_non_collision(trajectory, opts, *cancel);
  if (replacement) trace_moving_core_phase(replacement, "noncollision_return", noncollision_started,
                                          clock_.now(), trajectory.waypoints.size());
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
    const auto execution_id =
        validation_execution_id ? *validation_execution_id : sm_.status().execution_id;
    effective_scene_snapshot_lease =
        build_scene_snapshot_lease(scene_snapshot_lease_inventory, execution_id);
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

  const auto collision_started = replacement ? clock_.now() : 0;
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
  if (replacement) trace_moving_core_phase(replacement, "collision_return", collision_started,
                                          clock_.now(), collision_trajectory.waypoints.size());
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
  if (conclusive_collision_failure != nullptr && !contact_policy_violation.has_value() &&
      bundle.decision.action == CollisionDecisionAction::kReject &&
      bundle.decision.first_failing_waypoint.has_value()) {
    const auto failed_waypoint = std::find_if(
        bundle.waypoint_records.begin(), bundle.waypoint_records.end(), [&](const auto& record) {
          return record.waypoint_index == *bundle.decision.first_failing_waypoint;
        });
    if (failed_waypoint != bundle.waypoint_records.end()) {
      const auto& result = failed_waypoint->result;
      *conclusive_collision_failure =
          result.engine_status == CollisionEngineStatus::kOk && result.in_collision &&
          result.decision == CollisionDecisionResult::kFail &&
          std::any_of(result.normalized_pairs_classified.begin(),
                      result.normalized_pairs_classified.end(), [](const auto& pair) {
                        return pair.classification ==
                               CollisionPairClassification::kBlockingCollision;
                      });
    }
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

std::optional<ErrorDTO> ExecutionKernel::collision_resource_replacement_refusal_locked() {
  const auto reject = [](std::string reason, std::string message) {
    return std::optional<ErrorDTO>{ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidStateTransition)), std::move(message),
        {{"reason", std::move(reason)}}}};
  };
  if (sm_.status().state != ExecutionState::kIdle) {
    return reject("collision_resource_kernel_not_idle",
                  "Collision resources can change only while the Kernel is Idle");
  }
  if (runtime_collision_protection_snapshot().active ||
      runtime_collision_query_session_.load(std::memory_order_acquire) != nullptr) {
    return reject("collision_resource_runtime_monitor_active",
                  "Collision resources cannot change while runtime monitoring is active");
  }
  if (lifecycle_worker_retirement_in_progress_.load(std::memory_order_acquire) ||
      planning_reset_requested_.load(std::memory_order_acquire)) {
    return reject("collision_resource_kernel_transition_active",
                  "Collision resources cannot change during Kernel retirement or Reset");
  }
  {
    std::lock_guard lock(validation_cancel_mutex_);
    if (!active_validation_cancel_token_.expired()) {
      return reject("collision_resource_validation_active",
                    "Collision resources cannot change while validation work is active");
    }
  }
  {
    std::lock_guard lock(prepared_motion_session_mutex_);
    if (active_prepared_motion_session_ != nullptr) {
      return reject("collision_resource_prepared_operation_active",
                    "Collision resources cannot change while a prepared operation is retained");
    }
  }
  if (const auto lookahead =
          runtime_collision_lookahead_session_.load(std::memory_order_acquire);
      lookahead && !lookahead->quiescent_for_resource_replacement()) {
    return reject("collision_resource_lookahead_active",
                  "Collision resources cannot change while lookahead work is active");
  }
  return std::nullopt;
}

void ExecutionKernel::set_collision_resources(
    ICollisionWorldSnapshotPublisher& publisher,
    std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines) {
  // This API is intentionally limited to the first private-Kernel install.
  // A retained Kernel is replaced only by the exact Apply-fenced API below.
  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (const auto refusal = collision_resource_replacement_refusal_locked()) {
    throw std::logic_error(refusal->message + ": " + refusal->details.at("reason"));
  }
  std::unique_lock resource_set_lock(collision_resource_set_mutex_);
  if (collision_resource_set_generation_ != 0U) {
    throw std::logic_error(
        "collision resources are already installed; retained replacement requires an Apply fence");
  }
  if (runtime_collision_lookahead_session_.load(std::memory_order_acquire) != nullptr) {
    throw std::logic_error("initial collision resource install found an existing lookahead session");
  }

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
    std::terminate();
  }
  ++collision_resource_set_generation_;
  resource_set_lock.unlock();
  lifecycle_lock.unlock();
  std::map<std::string, std::string> ignored_details;
  (void)refresh_published_runtime_collision_snapshot(&ignored_details);
}

Result<CollisionPipelineResourceReadbackDTO>
ExecutionKernel::collision_resource_set_readback() const {
  std::lock_guard resource_set_lock(collision_resource_set_mutex_);
  const auto installed = collision_resources_.snapshot();
  if (collision_resource_set_generation_ == 0U) {
    return Result<CollisionPipelineResourceReadbackDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                 "Collision resources have not been installed",
                 {{"reason", "collision_resource_set_not_installed"}}});
  }
  if (!installed.snapshot_publisher_owner ||
      installed.snapshot_publisher_owner.get() != installed.snapshot_publisher) {
    return Result<CollisionPipelineResourceReadbackDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                 "Installed collision publisher is not owned by the Kernel",
                 {{"reason", "collision_resource_publisher_owner_missing"}}});
  }
  CollisionPipelineResourceOwners owners;
  owners.snapshot_publisher = installed.snapshot_publisher_owner;
  owners.runtime_monitor_engine = installed.runtime_monitor_engine;
  owners.validation_engines = installed.validation_engines;
  owners.runtime_lookahead_engines = installed.runtime_lookahead_engines;
  std::shared_ptr<const CollisionPolicySnapshot> policy_snapshot;
  std::shared_ptr<const CollisionPolicyIndex> policy_index;
  {
    std::lock_guard policy_lock(coordinator_mutex_);
    policy_snapshot = coordinator_policy_snapshot_;
    policy_index = coordinator_policy_index_;
  }
  return Result<CollisionPipelineResourceReadbackDTO>::Ok(
      CollisionPipelineResourceReadbackDTO{collision_resource_set_generation_,
                                           std::move(owners),
                                           std::move(policy_snapshot),
                                           std::move(policy_index)});
}

Result<CollisionPipelineResourceReadbackDTO>
ExecutionKernel::replace_collision_resources_under_apply_fence(
    const ConfigurationFenceId& apply_fence_id,
    const ConfigurationFenceScopeDTO& apply_fence_scope,
    const IResourceOperationAdmissionGate& admission_gate,
    CollisionPipelineResourceOwners candidate) {
  const auto refuse = [](ErrorCode code, std::string reason, std::string message,
                         std::map<std::string, std::string> additional = {}) {
    additional["reason"] = std::move(reason);
    return Result<CollisionPipelineResourceReadbackDTO>::Err(
        ErrorDTO{std::string(to_string(code)), std::move(message), std::move(additional)});
  };
  if (apply_fence_id.empty() || !apply_fence_scope.valid()) {
    return refuse(ErrorCode::kInvalidRequest, "configuration_fence_identity_invalid",
                  "Collision resource replacement requires a valid Apply fence identity");
  }
  std::shared_ptr<std::recursive_mutex> admission_mutex;
  try {
    admission_mutex = admission_gate.shared_admission_mutex();
  } catch (...) {
    return refuse(ErrorCode::kInvalidRequest, "configuration_fence_mutex_unavailable",
                  "Apply fence admission mutex is unavailable");
  }
  if (!admission_mutex) {
    return refuse(ErrorCode::kInvalidRequest, "configuration_fence_mutex_unavailable",
                  "Apply fence admission mutex is unavailable");
  }
  std::unique_lock admission_lock(*admission_mutex);
  try {
    if (!admission_gate.exact_fence_is_held(apply_fence_id, apply_fence_scope)) {
      return refuse(ErrorCode::kInvalidStateTransition, "configuration_fence_not_held",
                    "The exact Apply fence is not held for collision resource replacement");
    }
  } catch (...) {
    return refuse(ErrorCode::kInvalidStateTransition, "configuration_fence_verification_failed",
                  "The exact Apply fence could not be verified");
  }

  std::unique_lock lifecycle_lock(lifecycle_mutex_);
  if (const auto refusal = collision_resource_replacement_refusal_locked()) {
    return Result<CollisionPipelineResourceReadbackDTO>::Err(*refusal);
  }
  std::unique_lock resource_set_lock(collision_resource_set_mutex_);
  if (collision_resource_set_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return refuse(ErrorCode::kInvariantViolation, "collision_resource_generation_exhausted",
                  "Collision resource publication generation is exhausted");
  }
  if (!candidate.snapshot_publisher) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_publisher_missing",
                  "Candidate collision resources require an owned snapshot publisher");
  }
  if (!candidate.runtime_monitor_engine) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_engine_missing",
                  "Candidate collision resources require a runtime monitor engine",
                  {{"candidate_role", "runtime_monitor"}});
  }
  if (candidate.validation_engines.empty()) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_engine_missing",
                  "Candidate collision resources require at least one validation engine",
                  {{"candidate_role", "validation"}});
  }
  const auto contains_null_engine = [](const auto& engines) {
    return std::any_of(engines.begin(), engines.end(),
                       [](const auto& engine) { return !engine; });
  };
  if (contains_null_engine(candidate.validation_engines)) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_engine_missing",
                  "Candidate validation engine inventory contains a missing entry",
                  {{"candidate_role", "validation"}});
  }
  if (contains_null_engine(candidate.runtime_lookahead_engines)) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_engine_missing",
                  "Candidate lookahead engine inventory contains a missing entry",
                  {{"candidate_role", "runtime_lookahead"}});
  }

  CollisionPipelineResources prepared_resources;
  prepared_resources.snapshot_publisher = candidate.snapshot_publisher.get();
  prepared_resources.snapshot_publisher_owner = candidate.snapshot_publisher;
  prepared_resources.runtime_monitor_engine = candidate.runtime_monitor_engine;
  prepared_resources.validation_engines = candidate.validation_engines;
  prepared_resources.runtime_lookahead_engines = candidate.runtime_lookahead_engines;
  CollisionValidationAdmission admission;
  CollisionValidationWorkerCapacity validation_capacity;
  CollisionValidationWorkerCapacity lookahead_capacity;
  try {
    admission = CollisionResourceManager::validate_admission(prepared_resources, true);
    validation_capacity = CollisionResourceManager::inspect_validation_worker_capacity(
        candidate.validation_engines);
    lookahead_capacity = CollisionResourceManager::inspect_validation_worker_capacity(
        candidate.runtime_lookahead_engines);
  } catch (const std::exception& error) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_inspection_failed",
                  "Candidate collision engine capabilities could not be verified",
                  {{"exception", error.what()}});
  } catch (...) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_inspection_failed",
                  "Candidate collision engine capabilities could not be verified");
  }
  if (!admission.admitted()) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_invalid",
                  "Candidate collision resource roles are not independently usable",
                  {{"candidate_reason", admission.resource_conflict_reason}});
  }

  if (config_.validation_collision_workers_declared &&
      (validation_capacity.mismatch_reason ||
       validation_capacity.effective_contexts !=
           static_cast<std::size_t>(config_.validation_collision_workers))) {
    return refuse(
        ErrorCode::kInvalidRequest, "collision_resource_validation_capacity_mismatch",
        "Candidate validation engines do not satisfy the declared worker capacity",
        {{"requested", std::to_string(config_.validation_collision_workers)},
         {"effective", std::to_string(validation_capacity.effective_contexts)},
         {"capacity_reason", validation_capacity.mismatch_reason.value_or("count_mismatch")}});
  }
  if ((!candidate.runtime_lookahead_engines.empty() &&
       (lookahead_capacity.mismatch_reason ||
        lookahead_capacity.effective_contexts !=
            static_cast<std::size_t>(config_.runtime_collision_lookahead_workers))) ||
      (config_.runtime_collision_lookahead_workers_declared &&
       candidate.runtime_lookahead_engines.empty())) {
    return refuse(
        ErrorCode::kInvalidRequest, "collision_resource_lookahead_capacity_mismatch",
        "Candidate lookahead engines do not satisfy the declared worker capacity",
        {{"requested", std::to_string(config_.runtime_collision_lookahead_workers)},
         {"effective", std::to_string(lookahead_capacity.effective_contexts)},
         {"capacity_reason", lookahead_capacity.mismatch_reason.value_or(
                                 candidate.runtime_lookahead_engines.empty()
                                     ? "missing_contexts"
                                     : "count_mismatch")}});
  }

  auto published_bundle =
      load_published_runtime_collision_query_bundle(*candidate.snapshot_publisher);
  if (!published_bundle.ok()) {
    auto details = published_bundle.error().details;
    details["candidate_phase"] = "collision_resource_preflight";
    details["reason"] = "collision_resource_candidate_snapshot_unavailable";
    return Result<CollisionPipelineResourceReadbackDTO>::Err(ErrorDTO{
        published_bundle.error().code, published_bundle.error().message, std::move(details)});
  }
  const auto& bundle = published_bundle.value();
  if (!bundle || !bundle->world || !bundle->policy || !bundle->policy_index ||
      !bundle->lookup_order_valid || !bundle->distance_selection_order_valid) {
    return refuse(ErrorCode::kInvalidRequest, "collision_resource_candidate_snapshot_invalid",
                  "Candidate collision publisher did not provide a complete runtime snapshot");
  }

  // Prepare the complete readback before mutating Kernel state. The policy and
  // index are taken from the exact same immutable bundle as the candidate
  // world, rather than re-sampled from a second publisher surface later.
  const auto next_generation = collision_resource_set_generation_ + 1U;
  CollisionPipelineResourceReadbackDTO readback{
      next_generation, candidate, bundle->policy, bundle->policy_index};

  std::shared_ptr<RuntimeCollisionLookaheadSession> next_lookahead;
  if (!candidate.runtime_lookahead_engines.empty()) {
    const auto* progress = execution_backend_.execution_progress_publication();
    if (progress == nullptr) {
      return refuse(ErrorCode::kInvalidRequest, "collision_resource_progress_authority_missing",
                    "Candidate lookahead resources require backend progress authority");
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
    try {
      next_lookahead = std::make_shared<RuntimeCollisionLookaheadSession>(
          lookahead_config, clock_, scheduler_, *candidate.snapshot_publisher, *progress,
          candidate.runtime_lookahead_engines,
          [this](const RuntimeCollisionLookaheadSession* source_session,
                 RuntimeCollisionLookaheadResult result) {
            (void)source_session;
            emit_runtime_collision_lookahead_evidence(std::move(result));
          },
          [this](const RuntimeCollisionLookaheadSession* source_session,
                 RuntimeCollisionLookaheadResult& result) {
            return apply_runtime_collision_lookahead_authority(result, source_session);
          });
    } catch (const std::exception& error) {
      return refuse(ErrorCode::kInvalidRequest, "collision_resource_lookahead_prepare_failed",
                    "Candidate lookahead session could not be prepared",
                    {{"exception", error.what()}});
    } catch (...) {
      return refuse(ErrorCode::kInvalidRequest, "collision_resource_lookahead_prepare_failed",
                    "Candidate lookahead session could not be prepared");
    }
  }

  const auto previous_resources = collision_resources_.snapshot();
  std::shared_ptr<RuntimeCollisionLookaheadSession> previous_lookahead;
  {
    std::lock_guard policy_lock(coordinator_mutex_);
    collision_resources_.configure(std::move(prepared_resources));
    coordinator_policy_snapshot_ = bundle->policy;
    coordinator_policy_index_ = bundle->policy_index;
    previous_lookahead = runtime_collision_lookahead_session_.exchange(
        std::move(next_lookahead), std::memory_order_acq_rel);
    validation_collision_workers_effective_.store(validation_capacity.effective_contexts,
                                                  std::memory_order_relaxed);
    ++collision_resource_set_generation_;
  }
  resource_set_lock.unlock();
  lifecycle_lock.unlock();
  admission_lock.unlock();

  // The old worker borrows its publisher by reference. Keep the prior resource
  // snapshot alive until disarm and worker join finish, after all Kernel locks
  // have been released so a stale callback can exit without a lock cycle.
  if (previous_lookahead) {
    previous_lookahead->disarm();
    previous_lookahead.reset();
  }
  (void)previous_resources;
  return Result<CollisionPipelineResourceReadbackDTO>::Ok(std::move(readback));
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
  if (const auto status = sm_.status(); status.fact.has_value()) {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                 "runtime scale is not admissible for an ended execution",
                 {{"reason", "runtime_scale_state_invalid"},
                  {"state", to_string(status.state)},
                  {"participant_fact", to_string(status.fact->kind)}}});
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

  execution_backend_.set_runtime_velocity_scale(command.requested_scale);
  emit_event({KernelEventType::kRuntimeVelocityScaleChanged,
              sm_.status().execution_id,
              clock_.now(),
              {},
              {},
              std::nullopt,
              {{"new_scale", format_double(command.requested_scale)},
               {"runtime_scale_generation",
                std::to_string(command.requested_runtime_scale_generation.value())}}});
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
  std::optional<ResolvedExecutionBindingDTO> binding;
  std::optional<ResolvedRuntimeScaleCommandDTO> command;
  {
    std::lock_guard scale_lock(resolved_runtime_scale_mutex_);
    binding = active_correlation_.resolved_binding();
    if (!binding.has_value() || binding->operation_id != operation_id ||
        binding->control_binding_id != control_binding_id) {
      return Result<ResolvedRuntimeScaleStatusDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                   "runtime-scale status query does not name the active binding",
                   {{"reason", "runtime_scale_status_binding_not_active"}}});
    }
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
