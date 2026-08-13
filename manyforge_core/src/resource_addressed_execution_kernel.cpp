#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

template <typename T>
[[nodiscard]] Result<T> reject(std::string message, std::string reason,
                               ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<T>::Err(error(std::string(to_string(code)), std::move(message), std::move(reason)));
}

[[nodiscard]] std::vector<RuntimeParticipantBinding> extract_bindings(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<RuntimeParticipantBinding> bindings;
  bindings.reserve(registrations.size());
  for (const auto& registration : registrations) {
    if (registration.engine == nullptr) {
      throw std::invalid_argument("runtime participant registration has no execution engine");
    }
    if (registration.stop_readiness.has_value() && registration.stop_execution_port == nullptr) {
      throw std::invalid_argument(
          "Stop-qualified runtime participant registration has no Stop-stage port");
    }
    if (registration.stop_readiness.has_value() &&
        registration.stop_readiness->value().controllable_component_id !=
            registration.binding.value().controllable_component_id) {
      throw std::invalid_argument(
          "runtime participant registration Stop readiness crosses its binding");
    }
    bindings.push_back(registration.binding);
  }
  return bindings;
}

[[nodiscard]] std::size_t qualified_execution_engine_count(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::set<const IResolvedParticipantExecutionEngine*> engines;
  for (const auto& registration : registrations) {
    if (registration.stop_readiness.has_value() && registration.stop_execution_port != nullptr) {
      engines.insert(registration.engine);
    }
  }
  return engines.size();
}

[[nodiscard]] std::vector<DomainActiveControlConfigurationDTO>
extract_active_control_configurations(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<DomainActiveControlConfigurationDTO> configurations;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value()) {
      continue;
    }
    const auto& control = *binding.control_path;
    configurations.push_back(DomainActiveControlConfigurationDTO{
        control.control_domain_id, control.active_control_configuration_id,
        control.active_control_configuration_generation});
  }
  std::sort(configurations.begin(), configurations.end(), [](const auto& left, const auto& right) {
    if (left.control_domain_id != right.control_domain_id) {
      return left.control_domain_id < right.control_domain_id;
    }
    if (left.active_control_configuration_id != right.active_control_configuration_id) {
      return left.active_control_configuration_id < right.active_control_configuration_id;
    }
    return left.active_control_configuration_generation <
           right.active_control_configuration_generation;
  });
  const auto unique = std::unique(configurations.begin(), configurations.end());
  configurations.erase(unique, configurations.end());
  if (std::adjacent_find(configurations.begin(), configurations.end(),
                         [](const auto& left, const auto& right) {
                           return left.control_domain_id == right.control_domain_id;
                         }) != configurations.end()) {
    throw std::invalid_argument(
        "runtime participants disagree on one active control configuration");
  }
  return configurations;
}

[[nodiscard]] Result<std::vector<RouteReadinessPublicationRowDTO>>
derive_route_readiness_projection(
    const ActiveControlConfigurationDTO& configuration,
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  if (!configuration.locally_valid() ||
      configuration.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<std::vector<RouteReadinessPublicationRowDTO>>(
        "route readiness requires one reconciled active configuration",
        "runtime_route_configuration_unreconciled", ErrorCode::kInvalidRequest);
  }

  std::map<ControlBindingId, RouteReadinessPublicationRowDTO> rows_by_binding;
  const auto desired_routes_match = [&](const ControllerLifecycleExpectedDomainDTO& expected) {
    if (expected.desired_configuration.binding_routes.size() !=
        configuration.binding_routes.size()) {
      return false;
    }
    for (std::size_t index = 0U; index < configuration.binding_routes.size(); ++index) {
      const auto& desired = expected.desired_configuration.binding_routes[index];
      const auto& active = configuration.binding_routes[index];
      if (desired.control_binding_id != active.control_binding_id ||
          desired.control_binding_revision != active.control_binding_revision ||
          desired.control_route_id != active.control_route_id) {
        return false;
      }
    }
    return true;
  };
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value() ||
        binding.control_path->control_domain_id != configuration.control_domain_id) {
      continue;
    }
    const auto& path = *binding.control_path;
    const auto selected = std::find_if(
        configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& route) { return route.control_binding_id == path.control_binding_id; });
    if (selected == configuration.binding_routes.end() ||
        selected->control_binding_revision != path.control_binding_revision ||
        selected->control_route_id != path.control_route_id ||
        path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id ||
        path.topology_revision != configuration.topology_revision ||
        path.activation_set_id != configuration.activation_set_id ||
        path.active_control_configuration_id != configuration.active_control_configuration_id ||
        path.active_control_configuration_generation !=
            configuration.active_control_configuration_generation ||
        !registration.stop_readiness.has_value()) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime registration differs from the published route configuration",
          "runtime_route_registration_mismatch", ErrorCode::kInvariantViolation);
    }
    const auto& readiness = registration.stop_readiness->value();
    const auto& expected = readiness.expected_controller_domain;
    const auto& plan = readiness.resolved_stop_base_plan;
    if (expected.control_domain_id != configuration.control_domain_id ||
        expected.control_domain_revision != configuration.control_domain_revision ||
        expected.controller_topology_id != configuration.controller_topology_id ||
        expected.topology_revision != configuration.topology_revision ||
        expected.desired_configuration.activation_set_id != configuration.activation_set_id ||
        !desired_routes_match(expected) ||
        expected.source_runtime_unit_id != configuration.source_runtime_unit_id ||
        expected.source_runtime_epoch != configuration.source_runtime_epoch ||
        plan.active_control_configuration_id != configuration.active_control_configuration_id ||
        plan.active_control_configuration_generation !=
            configuration.active_control_configuration_generation ||
        plan.source_runtime_unit_id != configuration.source_runtime_unit_id ||
        plan.source_runtime_epoch != configuration.source_runtime_epoch ||
        plan.source_sequence != configuration.source_sequence) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime Stop readiness differs from the published route configuration",
          "runtime_route_readiness_mismatch", ErrorCode::kInvariantViolation);
    }

    RouteReadinessPublicationRowDTO row{
        binding.controllable_component_id, readiness.component_lifecycle_generation,
        path.control_binding_id, path.control_binding_revision, plan.resolved_stop_base_plan_id};
    const auto found = rows_by_binding.find(path.control_binding_id);
    if (found == rows_by_binding.end()) {
      rows_by_binding.emplace(path.control_binding_id, std::move(row));
    } else if (found->second != row) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime aliases disagree on one selected binding readiness row",
          "runtime_route_alias_readiness_mismatch", ErrorCode::kInvariantViolation);
    }
  }

  std::vector<RouteReadinessPublicationRowDTO> rows;
  rows.reserve(rows_by_binding.size());
  for (const auto& selected : configuration.binding_routes) {
    const auto found = rows_by_binding.find(selected.control_binding_id);
    if (found == rows_by_binding.end() ||
        found->second.control_binding_revision != selected.control_binding_revision) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime readiness does not cover every selected control binding",
          "runtime_route_readiness_incomplete", ErrorCode::kMissingReference);
    }
    rows.push_back(found->second);
  }
  std::sort(rows.begin(), rows.end(), CanonicalRouteReadinessPublicationRowLess);
  if (rows.size() != configuration.binding_routes.size() ||
      !std::all_of(rows.begin(), rows.end(), [](const auto& row) { return row.locally_valid(); })) {
    return reject<std::vector<RouteReadinessPublicationRowDTO>>(
        "runtime readiness projection has an invalid closed shape",
        "runtime_route_readiness_invalid", ErrorCode::kInvariantViolation);
  }
  return Result<std::vector<RouteReadinessPublicationRowDTO>>::Ok(std::move(rows));
}

[[nodiscard]] bool registration_truth_equal(
    const RuntimeParticipantExecutionRegistration& left,
    const RuntimeParticipantExecutionRegistration& right) noexcept {
  const bool readiness_equal =
      left.stop_readiness.has_value() == right.stop_readiness.has_value() &&
      (!left.stop_readiness.has_value() ||
       left.stop_readiness->value() == right.stop_readiness->value());
  return left.binding.value() == right.binding.value() && readiness_equal &&
         left.retained_default_stop_limits == right.retained_default_stop_limits &&
         left.stop_node_category == right.stop_node_category && left.engine == right.engine &&
         left.stop_execution_port == right.stop_execution_port;
}

[[nodiscard]] std::set<ControlDomainId> changed_registration_domains(
    const std::vector<RuntimeParticipantExecutionRegistration>& current,
    const std::vector<RuntimeParticipantExecutionRegistration>& candidate) {
  std::set<ControlDomainId> domains;
  for (const auto* registrations : {&current, &candidate}) {
    for (const auto& registration : *registrations) {
      if (registration.binding.value().control_path.has_value()) {
        domains.insert(registration.binding.value().control_path->control_domain_id);
      }
    }
  }
  std::set<ControlDomainId> changed;
  for (const auto& domain : domains) {
    const auto current_count = std::count_if(current.begin(), current.end(), [&](const auto& row) {
      return row.binding.value().control_path.has_value() &&
             row.binding.value().control_path->control_domain_id == domain;
    });
    const auto candidate_count =
        std::count_if(candidate.begin(), candidate.end(), [&](const auto& row) {
          return row.binding.value().control_path.has_value() &&
                 row.binding.value().control_path->control_domain_id == domain;
        });
    if (current_count != candidate_count ||
        std::any_of(current.begin(), current.end(), [&](const auto& existing) {
          if (!existing.binding.value().control_path.has_value() ||
              existing.binding.value().control_path->control_domain_id != domain) {
            return false;
          }
          return std::count_if(candidate.begin(), candidate.end(), [&](const auto& replacement) {
                   return registration_truth_equal(existing, replacement);
                 }) != 1;
        })) {
      changed.insert(domain);
    }
  }
  return changed;
}

struct StopStaticFacts final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::optional<StopDeadlineBudgetDTO> deadline_budget;
  std::vector<StopScopeComponentSnapshotDTO> components;
};

[[nodiscard]] ConfigurationFenceScopeDTO derive_disable_handoff_scope(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations,
    const std::optional<ControllableComponentId>& selected_component) {
  std::set<ControlDomainId> selected_domains;
  if (selected_component.has_value()) {
    for (const auto& registration : registrations) {
      const auto& binding = registration.binding.value();
      if (binding.controllable_component_id == *selected_component &&
          binding.control_path.has_value()) {
        selected_domains.insert(binding.control_path->control_domain_id);
      }
    }
    if (selected_domains.empty()) {
      return {};
    }
  }

  std::set<ControllableComponentId> components;
  std::map<ControlDomainId, ControlDomainRevision> domains;
  std::map<ControlBindingId, ControlBindingRevision> bindings;
  std::set<ResourceId> resources;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value()) {
      continue;
    }
    const auto& control = *binding.control_path;
    if (selected_component.has_value() && !selected_domains.contains(control.control_domain_id)) {
      continue;
    }
    if (!selected_component.has_value()) {
      components.insert(binding.controllable_component_id);
    }
    const auto [domain, domain_inserted] =
        domains.emplace(control.control_domain_id, control.control_domain_revision);
    if (!domain_inserted && domain->second != control.control_domain_revision) {
      throw std::invalid_argument("runtime Disable scope has conflicting control-domain revisions");
    }
    const auto [control_binding, binding_inserted] =
        bindings.emplace(control.control_binding_id, control.control_binding_revision);
    if (!binding_inserted && control_binding->second != control.control_binding_revision) {
      throw std::invalid_argument(
          "runtime Disable scope has conflicting control-binding revisions");
    }
    for (const auto& resource : binding.resources) {
      resources.insert(resource.resource_id);
    }
  }
  if (selected_component.has_value()) {
    components.insert(*selected_component);
  }
  if (components.empty() || domains.empty() || bindings.empty() || resources.empty()) {
    return {};
  }

  ConfigurationFenceScopeDTO scope;
  scope.controllable_component_ids.assign(components.begin(), components.end());
  scope.domains.reserve(domains.size());
  for (const auto& [domain_id, revision] : domains) {
    scope.domains.push_back({domain_id, revision, std::nullopt});
  }
  scope.bindings.reserve(bindings.size());
  for (const auto& [binding_id, revision] : bindings) {
    scope.bindings.push_back({binding_id, revision, std::nullopt});
  }
  scope.resources.assign(resources.begin(), resources.end());
  if (!scope.valid()) {
    throw std::invalid_argument("runtime Disable scope is not canonical");
  }
  return scope;
}

[[nodiscard]] std::vector<CurrentDomainActiveControlConfigurationDTO>
select_disable_active_configurations(
    const ConfigurationFenceScopeDTO& scope,
    const std::vector<DomainActiveControlConfigurationDTO>& available) {
  std::vector<CurrentDomainActiveControlConfigurationDTO> result;
  result.reserve(scope.domains.size());
  for (const auto& domain : scope.domains) {
    const auto found =
        std::lower_bound(available.begin(), available.end(), domain.control_domain_id,
                         [](const auto& configuration, const ControlDomainId& domain_id) {
                           return configuration.control_domain_id < domain_id;
                         });
    if (found == available.end() || found->control_domain_id != domain.control_domain_id) {
      result.push_back(
          {domain.control_domain_id,
           {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}});
    } else {
      result.push_back({domain.control_domain_id,
                        {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
                         found->active_control_configuration_id,
                         found->active_control_configuration_generation}});
    }
  }
  return result;
}

[[nodiscard]] StopStaticFacts derive_stop_static_facts(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations,
    const ResourceAddressedExecutionKernelConfig& config) {
  std::optional<GraphRevision> graph_revision = config.graph_revision;
  std::optional<RetainedBundleId> retained_bundle_id;
  std::optional<StopDeadlineBudgetDTO> deadline_budget = config.cell_stop_deadline_budget;
  if (deadline_budget.has_value()) {
    deadline_budget->phases.clear();
  }

  std::map<ControllableComponentId, StopScopeComponentSnapshotDTO> components;
  std::map<ResourceId, ControllableComponentId> resource_owners;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!retained_bundle_id.has_value()) {
      retained_bundle_id = binding.retained_bundle_id;
    }
    if (binding.retained_bundle_id != *retained_bundle_id) {
      throw std::invalid_argument(
          "runtime participants do not share one retained bundle for Stop terminal truth");
    }
    if (!graph_revision.has_value()) {
      graph_revision = binding.graph_revision;
    }
    if (binding.graph_revision != *graph_revision) {
      throw std::invalid_argument(
          "runtime participants do not share one current graph revision for Stop resolution");
    }
    if (!registration.stop_readiness.has_value()) {
      // Unqualified registrations remain observable and may later be replaced
      // under a configuration fence, but Execute cannot admit them. They do
      // not self-attest component lifecycle membership or a Stop plan here.
      continue;
    }
    const auto& readiness = registration.stop_readiness->value();
    const auto& budget = readiness.resolved_stop_base_plan.deadline_budget;
    if (!deadline_budget.has_value()) {
      deadline_budget =
          StopDeadlineBudgetDTO{budget.outer_duration_ns, budget.kernel_completion_reserve_ns, {}};
    }
    if (budget.outer_duration_ns != deadline_budget->outer_duration_ns ||
        budget.kernel_completion_reserve_ns != deadline_budget->kernel_completion_reserve_ns) {
      throw std::invalid_argument(
          "runtime participants do not share one cell-wide Stop deadline envelope");
    }

    auto [component, inserted] = components.try_emplace(
        readiness.controllable_component_id,
        StopScopeComponentSnapshotDTO{
            readiness.controllable_component_id, readiness.component_lifecycle_generation, {}});
    if (!inserted && component->second.component_lifecycle_generation !=
                         readiness.component_lifecycle_generation) {
      throw std::invalid_argument(
          "runtime participant Stop readiness disagrees on component lifecycle generation");
    }
    for (const auto& resource : binding.resources) {
      const auto [owner, owner_inserted] =
          resource_owners.emplace(resource.resource_id, readiness.controllable_component_id);
      if (!owner_inserted && owner->second != readiness.controllable_component_id) {
        throw std::invalid_argument(
            "runtime Stop readiness assigns one resource to multiple components");
      }
      component->second.resources.push_back(resource.resource_id);
    }
  }

  if (!graph_revision.has_value() || graph_revision->empty()) {
    throw std::invalid_argument("runtime Stop resolution has no current graph revision");
  }
  if (deadline_budget.has_value() &&
      (deadline_budget->outer_duration_ns <= 0 ||
       deadline_budget->outer_duration_ns > kMaximumStopDurationNs ||
       deadline_budget->kernel_completion_reserve_ns <= 0 ||
       deadline_budget->kernel_completion_reserve_ns >= deadline_budget->outer_duration_ns)) {
    throw std::invalid_argument("runtime Stop resolution has no valid cell-wide deadline envelope");
  }
  if (!deadline_budget.has_value() && !components.empty()) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants have no cell-wide deadline envelope");
  }

  if (!retained_bundle_id.has_value() || retained_bundle_id->empty()) {
    throw std::invalid_argument("runtime Stop terminal truth has no retained-bundle identity");
  }

  StopStaticFacts facts{
      std::move(*retained_bundle_id), std::move(*graph_revision), std::move(deadline_budget), {}};
  facts.components.reserve(components.size());
  for (auto& [component_id, component] : components) {
    (void)component_id;
    std::sort(component.resources.begin(), component.resources.end());
    component.resources.erase(std::unique(component.resources.begin(), component.resources.end()),
                              component.resources.end());
    facts.components.push_back(std::move(component));
  }
  return facts;
}

[[nodiscard]] const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const FrameRoleId& role) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameRoleBindingDTO& candidate) { return candidate.frame_role_id == role; });
  return found == binding.frame_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] const RuntimeResourceBindingDTO* find_source_joint(
    const RuntimeParticipantBindingDTO& binding, const std::string& source_joint_name) noexcept {
  const auto found = std::find_if(binding.resources.begin(), binding.resources.end(),
                                  [&](const RuntimeResourceBindingDTO& candidate) {
                                    return candidate.source_joint_name.has_value() &&
                                           *candidate.source_joint_name == source_joint_name;
                                  });
  return found == binding.resources.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramModelRoleBindingDTO* find_model(
    const RuntimeParticipantBindingDTO& binding,
    const ModelInstanceId& model_instance_id) noexcept {
  const auto found = std::find_if(binding.model_bindings.begin(), binding.model_bindings.end(),
                                  [&](const ProgramModelRoleBindingDTO& candidate) {
                                    return candidate.model_instance_id == model_instance_id;
                                  });
  return found == binding.model_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] CapabilityId required_planner_capability(const MotionSpecDTO& motion) {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JointGoalDTO>) {
          return CapabilityId("planning.motion.joint_goal");
        } else if constexpr (std::is_same_v<Value, PoseGoalDTO>) {
          return CapabilityId("planning.motion.pose_goal");
        } else if constexpr (std::is_same_v<Value, NamedTargetDTO>) {
          return CapabilityId("planning.motion.named_target");
        } else {
          return CapabilityId("planning.motion.cartesian_path");
        }
      },
      motion);
}

[[nodiscard]] bool same_linked_preview_participant(const RuntimeParticipantBindingDTO& left,
                                                   const RuntimeParticipantBindingDTO& right) {
  auto left_stable_identity = left;
  auto right_stable_identity = right;
  // Supporting providers are selected for each motion payload. A linked
  // pose/Cartesian sequence may therefore use different exact planners while
  // retaining the same robot, resources, control path, epochs, and execution
  // engine. Each complete binding is still resolved and validated before use.
  left_stable_identity.supporting_capability_provider_paths.clear();
  right_stable_identity.supporting_capability_provider_paths.clear();
  return left_stable_identity == right_stable_identity;
}

[[nodiscard]] Result<std::optional<ResolvedContactGraspBindingDTO>> resolve_contact_grasp_binding(
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding) {
  if (!motion.contact_policy_id.has_value()) {
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(std::nullopt);
  }
  const auto policy = std::lower_bound(
      binding.contact_grasp_bindings.begin(), binding.contact_grasp_bindings.end(),
      *motion.contact_policy_id, [](const auto& candidate, const ContactPolicyId& id) {
        return candidate.contact_policy_id < id;
      });
  if (policy == binding.contact_grasp_bindings.end() ||
      policy->contact_policy_id != *motion.contact_policy_id) {
    // Other contact-policy modes remain selected from the retained scene
    // policy. Only Program-owned contact_grasp requires this late binding.
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(std::nullopt);
  }
  const auto object = std::lower_bound(
      motion.runtime_object_bindings.begin(), motion.runtime_object_bindings.end(),
      policy->runtime_object_id, [](const auto& candidate, const RuntimeObjectId& id) {
        return candidate.runtime_object_id < id;
      });
  if (object == motion.runtime_object_bindings.end() ||
      object->runtime_object_id != policy->runtime_object_id) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy has no selected runtime object",
        "contact_grasp_runtime_object_unresolved", ErrorCode::kMissingReference);
  }
  std::vector<CollisionBodyId> body_ids;
  for (const auto& body : policy->allowed_body_bindings) {
    body_ids.insert(body_ids.end(), body.collision_body_ids.begin(), body.collision_body_ids.end());
  }
  std::sort(body_ids.begin(), body_ids.end());
  body_ids.erase(std::unique(body_ids.begin(), body_ids.end()), body_ids.end());
  if (body_ids.empty()) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy resolved no robot collision bodies",
        "contact_grasp_body_binding_empty", ErrorCode::kInvariantViolation);
  }
  return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(
      std::optional<ResolvedContactGraspBindingDTO>{ResolvedContactGraspBindingDTO{
          policy->contact_policy_id, policy->contact_policy_revision, policy->runtime_object_id,
          object->scene_resource_id, std::move(body_ids), policy->max_penetration_m,
          policy->terminal_window_start_fraction}});
}

[[nodiscard]] ResolvedFrameDTO resolved_component_frame(const RuntimeParticipantBindingDTO& binding,
                                                        const ProgramFrameRoleBindingDTO& frame) {
  return ResolvedComponentModelFrameDTO{binding.controllable_component_id, frame.model_instance_id,
                                        frame.frame_role_id, frame.compiled_frame_name,
                                        frame.model_revision};
}

[[nodiscard]] Result<ExecuteRequestDTO> build_motion_request(
    const OperationIntentDTO& operation, const OperationParticipantIntentDTO& participant,
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding,
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) {
  if (binding.model_groups.size() != 1U) {
    return reject<ExecuteRequestDTO>("I6 motion requires exactly one retained model group",
                                     "i6_motion_model_group_cardinality_invalid");
  }
  const auto* base = find_frame(binding, motion.base_frame_role_id);
  const auto* tcp = find_frame(binding, motion.tcp_frame_role_id);
  if (base == nullptr || tcp == nullptr) {
    return reject<ExecuteRequestDTO>("motion frame roles do not resolve through the exact binding",
                                     "motion_frame_role_unresolved");
  }

  const auto& model_group = binding.model_groups.front();
  const auto* group_model = find_model(binding, model_group.model_instance_id);
  if (group_model == nullptr || !binding.control_path.has_value()) {
    return reject<ExecuteRequestDTO>(
        "motion model group or control path is absent from the exact binding",
        "motion_runtime_binding_incomplete", ErrorCode::kInvariantViolation);
  }
  std::vector<std::string> compiled_joint_names;
  compiled_joint_names.reserve(binding.resources.size());
  for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
    const auto& resource = binding.resources[index];
    if (resource.coordinate_kind != CoordinateKind::kAngular ||
        std::count(model_group.expanded_compiled_joint_names.begin(),
                   model_group.expanded_compiled_joint_names.end(),
                   resource.compiled_resource_name) != 1) {
      return reject<ExecuteRequestDTO>(
          "motion requires all-angular resources belonging to the exact retained model group",
          "motion_resource_projection_not_all_angular", ErrorCode::kInvariantViolation);
    }
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  const auto& control = *binding.control_path;
  const auto planner_capability = required_planner_capability(motion.motion);
  const auto* planner_definition = find_builtin_capability_definition(planner_capability);
  if (planner_definition == nullptr) {
    return reject<ExecuteRequestDTO>("motion kind has no registered planning capability",
                                     "motion_planner_capability_unregistered",
                                     ErrorCode::kInvariantViolation);
  }
  const CapabilityDefinitionRevision planner_revision(
      std::string(planner_definition->capability_definition_revision));
  const auto planner_path =
      std::find_if(binding.supporting_capability_provider_paths.begin(),
                   binding.supporting_capability_provider_paths.end(),
                   [&](const RuntimeCapabilityProviderPathDTO& candidate) {
                     return candidate.capability_id == planner_capability &&
                            candidate.capability_definition_revision == planner_revision;
                   });
  if (planner_path == binding.supporting_capability_provider_paths.end()) {
    return reject<ExecuteRequestDTO>(
        "motion planner capability does not resolve through the exact binding",
        "motion_planner_capability_unresolved", ErrorCode::kMissingReference);
  }
  std::vector<PlannerModelBindingDTO> models;
  models.reserve(binding.model_bindings.size());
  for (const auto& model : binding.model_bindings) {
    models.push_back(PlannerModelBindingDTO{model.model_instance_id, model.model_revision});
  }
  std::vector<PlannerRuntimeUnitBindingDTO> runtime_units;
  runtime_units.reserve(binding.runtime_unit_epochs.size());
  for (const auto& runtime : binding.runtime_unit_epochs) {
    runtime_units.push_back(
        PlannerRuntimeUnitBindingDTO{runtime.runtime_unit_id, runtime.runtime_epoch});
  }
  std::vector<ResourceId> bound_resources;
  bound_resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    bound_resources.push_back(resource.resource_id);
  }

  ResolvedExecutionBindingDTO exact_binding{
      operation_id,
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      operation.program_run_id,
      binding.controllable_component_id,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      PlannerCapabilityBindingDTO{
          planner_path->capability_id, planner_path->capability_definition_revision,
          planner_path->capability_provider_binding_id,
          planner_path->capability_provider_binding_revision, planner_path->provider_instance_id,
          planner_path->provider_manifest_id, planner_path->provider_manifest_revision,
          planner_path->provider_runtime_unit_id, planner_path->provider_runtime_epoch},
      std::move(models),
      PlannerModelGroupBindingDTO{model_group.model_group_role_id, model_group.model_instance_id,
                                  group_model->model_revision, model_group.compiled_group_name,
                                  std::move(compiled_joint_names)},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      control.control_interface_contract_id,
      control.control_interface_revision,
      control.controller_topology_id,
      control.topology_revision,
      control.control_route_id,
      control.activation_set_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      control.provider_instance_id,
      control.provider_manifest_id,
      control.provider_manifest_revision,
      control.provider_runtime_epoch,
      std::move(runtime_units),
      std::move(bound_resources),
      resolved_component_frame(binding, *base),
      resolved_component_frame(binding, *tcp),
      {},
      {},
      control.runtime_scale_qualification,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.target_role_id,
      binding.frame_bindings,
      binding.collision_body_bindings};

  ExecuteRequestDTO request(std::move(exact_binding));
  request.move_id = motion.move_id.value();
  request.tolerances = motion.tolerances;
  request.speed_profile = motion.speed_profile;
  request.timeouts = motion.timeouts;
  request.cache_mode = motion.cache_mode;
  request.planning_profile = motion.planning_profile;
  request.target_mode = motion.target_mode;
  request.target_reference = motion.target_reference;
  request.offset_reference = motion.offset_reference;
  request.motion_interrupt_scopes = motion.motion_interrupt_scopes;
  request.action_span = participant.action_span;
  request.contact_policy_id =
      motion.contact_policy_id.has_value() ? motion.contact_policy_id->value() : std::string{};
  request.runtime_object_bindings = motion.runtime_object_bindings;
  auto contact_grasp = resolve_contact_grasp_binding(motion, binding);
  if (!contact_grasp.ok()) {
    return Result<ExecuteRequestDTO>::Err(contact_grasp.error());
  }
  request.resolved_contact_grasp = std::move(contact_grasp.value());
  if (control.runtime_scale_qualification.has_value()) {
    const auto& qualification = *control.runtime_scale_qualification;
    request.initial_runtime_scale =
        ResolvedRuntimeScaleCommandDTO{operation_id,
                                       operation_revision,
                                       participant_id,
                                       binding.controllable_component_id,
                                       binding.command_target_id,
                                       binding.capability_id,
                                       control.control_domain_id,
                                       control.control_domain_revision,
                                       control.control_binding_id,
                                       control.control_binding_revision,
                                       controller_goal_id,
                                       controller_goal_generation,
                                       control.controller_topology_id,
                                       control.topology_revision,
                                       control.control_route_id,
                                       qualification.scaling_domain_id,
                                       qualification.coordinated_barrier_id,
                                       request.binding.resources,
                                       control.active_control_configuration_id,
                                       control.active_control_configuration_generation,
                                       RuntimeScaleGeneration(0U),
                                       RuntimeScaleGeneration(0U),
                                       1.0};
  }

  if (const auto* goal = std::get_if<JointGoalDTO>(&motion.motion)) {
    std::set<std::string, std::less<>> seen_joint_names;
    std::vector<ResourceId> resolved_resources;
    std::vector<std::string> compiled_names;
    resolved_resources.reserve(goal->joint_names.size());
    compiled_names.reserve(goal->joint_names.size());
    for (const auto& joint_name : goal->joint_names) {
      const auto* resource = find_source_joint(binding, joint_name);
      if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular ||
          !seen_joint_names.insert(joint_name).second) {
        return reject<ExecuteRequestDTO>(
            "joint goal source joints are missing, duplicated, or non-angular",
            "joint_goal_source_joint_unresolved");
      }
      resolved_resources.push_back(resource->resource_id);
      compiled_names.push_back(resource->compiled_resource_name);
    }
    request.motion = ResolvedJointGoalDTO{std::move(resolved_resources), std::move(compiled_names),
                                          goal->positions_rad};
  } else if (const auto* pose = std::get_if<PoseGoalDTO>(&motion.motion)) {
    request.motion = *pose;
  } else if (const auto* path = std::get_if<CartesianPathDTO>(&motion.motion)) {
    request.motion = *path;
  } else {
    return reject<ExecuteRequestDTO>(
        "named targets require the retained Program target map before planning",
        "named_target_resolution_unavailable", ErrorCode::kMissingReference);
  }

  for (const auto& [joint_name, limit] : motion.per_joint_dynamics_limits) {
    const auto* resource = find_source_joint(binding, joint_name);
    if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular) {
      return reject<ExecuteRequestDTO>(
          "motion dynamics source joint does not resolve to an angular resource",
          "motion_dynamics_source_joint_unresolved");
    }
    request.per_resource_dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
        resource->resource_id, resource->compiled_resource_name, limit});
  }
  std::sort(request.per_resource_dynamics_limits.begin(),
            request.per_resource_dynamics_limits.end(),
            [&](const ResolvedResourceDynamicsLimitDTO& lhs,
                const ResolvedResourceDynamicsLimitDTO& rhs) {
              const auto left = std::find(request.binding.resources.begin(),
                                          request.binding.resources.end(), lhs.resource_id);
              const auto right = std::find(request.binding.resources.begin(),
                                           request.binding.resources.end(), rhs.resource_id);
              return left < right;
            });
  return Result<ExecuteRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] Result<GripperCommandRequestDTO> build_command_request(
    const CapabilityCommandIntentDTO& command, const RuntimeParticipantBindingDTO& binding) {
  if (binding.model_groups.size() > 1U || binding.resources.size() != 1U) {
    return reject<GripperCommandRequestDTO>(
        "the retained I6 gripper mechanism requires one exact command resource",
        "i6_gripper_command_scope_unsupported");
  }
  const auto& resource = binding.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  const bool exact_model_scope =
      model_backed && binding.model_groups.size() == 1U &&
      std::count(binding.model_groups.front().expanded_compiled_joint_names.begin(),
                 binding.model_groups.front().expanded_compiled_joint_names.end(),
                 resource.compiled_resource_name) == 1;
  const bool exact_component_local_scope = !model_backed && binding.model_groups.empty();
  if (!exact_model_scope && !exact_component_local_scope) {
    return reject<GripperCommandRequestDTO>(
        "the retained gripper resource has no exact model-backed or component-local scope",
        "i6_gripper_command_scope_unsupported");
  }
  if (command.command_schema_id.value() != kGripperPositionCommandSchemaId ||
      command.command_schema_revision.value() != kGripperPositionCommandSchemaRevision) {
    return reject<GripperCommandRequestDTO>("the command is not the qualified I6 gripper schema",
                                            "i6_command_schema_unsupported",
                                            ErrorCode::kUnsupportedCapability);
  }

  double command_duration_s = 0.0;
  for (const auto& option : command.caller_options) {
    if (option.option_id.value() != "command_duration_s") {
      return reject<GripperCommandRequestDTO>(
          "the command carries an option not declared by its schema", "command_option_unsupported",
          ErrorCode::kInvalidRequest);
    }
    command_duration_s = option.value_s;
  }

  const auto& value = std::get<GripperPositionCommandValueDTO>(command.command_value);
  const auto kind = coordinate_kind(value.position);
  if (kind != binding.resources.front().coordinate_kind) {
    return reject<GripperCommandRequestDTO>(
        "gripper command unit differs from its retained resource",
        "gripper_command_coordinate_kind_mismatch", ErrorCode::kInvalidRequest);
  }
  const double target_position = kind == CoordinateKind::kAngular
                                     ? std::get<AngularPositionDTO>(value.position).value_rad
                                     : std::get<LinearPositionDTO>(value.position).value_m;
  std::optional<double> max_effort;
  if (value.max_effort.has_value()) {
    max_effort = kind == CoordinateKind::kAngular
                     ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                     : std::get<LinearEffortDTO>(*value.max_effort).value_n;
  }

  GripperCommandRequestDTO request;
  request.gripper_id = binding.command_target_id.value();
  request.group_id =
      model_backed ? binding.model_groups.front().compiled_group_name : std::string{};
  request.joint_name = resource.compiled_resource_name;
  request.target_position = target_position;
  request.duration_s = command_duration_s;
  request.max_effort = max_effort;
  request.commanded_state = "position";
  request.actuation_kind = kind == CoordinateKind::kAngular ? "angular" : "linear";
  request.state_authority = binding.control_path->state_authority_id.value();
  request.completion_policy = "goal_fenced_target_reached";
  request.grasp_confirmation_policy = "binding_feedback_required";
  request.transport_kind = "resolved_binding";
  request.evidence = {"command_terminal"};
  request.evidence_state = "required_pending";
  request.completion_timeout_s = command.completion_timeout_s;
  return Result<GripperCommandRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] OperationStateKind operation_state(ExecutionState state) noexcept {
  switch (state) {
    case ExecutionState::kIdle:
      return OperationStateKind::kPrepared;
    case ExecutionState::kPlanning:
      return OperationStateKind::kPlanning;
    case ExecutionState::kValidating:
      return OperationStateKind::kValidating;
    case ExecutionState::kExecuting:
      return OperationStateKind::kExecuting;
    case ExecutionState::kInterrupted:
      return OperationStateKind::kInterrupted;
    case ExecutionState::kPaused:
      return OperationStateKind::kPaused;
    case ExecutionState::kStopping:
      return OperationStateKind::kStopping;
    case ExecutionState::kStopped:
      return OperationStateKind::kStopped;
    case ExecutionState::kCompleted:
      return OperationStateKind::kCompleted;
    case ExecutionState::kFailed:
      return OperationStateKind::kFailed;
    case ExecutionState::kInterventionRequired:
      return OperationStateKind::kInterventionRequired;
  }
  return OperationStateKind::kInterventionRequired;
}

[[nodiscard]] bool terminal(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool control_admissible(OperationControlKind control,
                                      OperationStateKind state) noexcept {
  switch (control) {
    case OperationControlKind::kPause:
      return state == OperationStateKind::kExecuting;
    case OperationControlKind::kResume:
      return state == OperationStateKind::kPaused;
    case OperationControlKind::kReset:
      return state != OperationStateKind::kCompleted;
    case OperationControlKind::kAcknowledgeCompleted:
      return state == OperationStateKind::kCompleted;
  }
  return false;
}

[[nodiscard]] const char* control_name(OperationControlKind control) noexcept {
  switch (control) {
    case OperationControlKind::kPause:
      return "pause";
    case OperationControlKind::kResume:
      return "resume";
    case OperationControlKind::kReset:
      return "reset";
    case OperationControlKind::kAcknowledgeCompleted:
      return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] std::optional<ResourceSlotState> slot_state_for(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kDispatching:
    case OperationStateKind::kExecuting:
    case OperationStateKind::kPaused:
      return ResourceSlotState::kDispatched;
    case OperationStateKind::kStopping:
      return ResourceSlotState::kStopping;
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return ResourceSlotState::kTerminal;
    case OperationStateKind::kQueued:
    case OperationStateKind::kResolving:
    case OperationStateKind::kPlanning:
    case OperationStateKind::kValidating:
    case OperationStateKind::kPrepared:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] ErrorDTO status_error(ErrorCode code) {
  return ErrorDTO{std::string(to_string(code)),
                  "subordinate participant reached a terminal error state",
                  {{"reason", "subordinate_terminal_error"}}};
}

}  // namespace

struct ResourceAddressedExecutionKernel::ReplayEntry final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::ControlReplayEntry final {
  OperationControlRequestDTO request;
  OperationControlAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::StopReplayEntry final {
  StopIntentDTO intent;
  StopAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::FullStopReplayEntry final {
  FullStopIntentDTO intent;
  StopAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::RuntimeScaleReplayEntry final {
  RuntimeScaleRequestDTO request;
  RuntimeScaleAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::StopFaultRecoveryReplayEntry final {
  StopFaultRecoveryIntentDTO intent;
  StopFaultRecoveryResultDTO result;
};

struct ResourceAddressedExecutionKernel::ActiveOperation final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness stop_readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  std::optional<PreparedOperationParticipantDTO> prepared_participant;
  IResolvedParticipantExecutionEngine* engine{};
  IControlDomainExecutionPort* stop_execution_port{};
  ResourceSlotOwnerDTO slot_owner;
  std::vector<ResourceId> resources;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationStateDTO state;
  ResourceSlotState slot_state{ResourceSlotState::kReserved};
  std::uint64_t subordinate_id{};
  std::uint64_t subordinate_generation{};
  std::optional<OperationControlKind> pending_control;
  bool reconciliation_in_flight{};
  bool runtime_scale_effect_in_flight{};
  bool stop_effect_in_flight{};
  bool ordinary_acceptance_pending{};
  bool ordinary_acceptance_durable{};
  bool dispatch_effect_in_flight{};
  std::optional<ErrorDTO> last_subordinate_error;
  std::optional<RuntimeScaleStatusDTO> runtime_scale_status;
};

struct StopParticipantAttempt final {
  bool started{};
  bool superseded{};
  bool result_consumed{};
  std::optional<StopStageAttemptHandle> handle;
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const RemainingStopBudgetDTO> budget;
  std::int64_t stage_deadline_ns{};
};

struct StopParticipantWork final {
  OperationId operation_id;
  OperationRevision accepted_operation_revision{0U};
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation{0U};
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory node_category{MotionInterruptNodeCategory::kGeneric};
  PreparedParticipantStopPlanDTO plan;
  std::vector<ResourceId> resources;
  IControlDomainExecutionPort* port{};
  bool cancelled_pre_dispatch{};
  bool terminal{};
  bool contained{};
  std::size_t current_stage_index{};
  std::uint64_t next_result_sequence{1U};
  std::optional<ErrorDTO> terminal_error;
  std::array<StopParticipantAttempt, kMaxStopEscalationStages> attempts;
  std::vector<StopAttemptedStageResultDTO> retained_results;
};

struct ResourceAddressedExecutionKernel::StopTransaction final {
  bool active{};
  StopControlWorkToken token;
  std::optional<StopId> stop_id;
  StopGeneration stop_generation{0U};
  std::int64_t outer_deadline_ns{};
  bool full_stop{};
  bool persistence_degraded{};
  EmergencyStopProjectionStateDTO acceptance_projection_state{
      EmergencyStopProjectionStateDTO::kPending};
  bool progress_in_flight{};
  bool fresh_scope_complete{};
  bool terminal{};
  std::optional<StopAcceptedDTO> accepted;
  EmergencyStopProjectedIntentDTO projected_intent;
  std::vector<StopAffectedOperationRevisionDTO> newly_fenced_operation_revisions;
  std::vector<ResourceId> newly_captured_resources;
  /// Only scope first fenced by this transaction. Predecessor operations keep
  /// progressing under their original work tokens and are never redispatched.
  std::vector<OperationId> operations;
  std::vector<ResourceId> resources;
  std::vector<StopControlWorkToken> predecessor_tokens;
  std::vector<StopCertificateDTO> predecessor_certificates;
  bool participant_work_initialized{};
  std::vector<StopParticipantWork> participants;
  std::optional<StopCertificateDTO> certificate;
  std::optional<StopControlWorkToken> terminal_projection_token;
};

struct ResourceAddressedExecutionKernel::StopTerminalRecord final {
  StopStatusDTO status;
  StopCertificateDTO certificate;
  std::optional<StopFaultLatchDTO> fault_latch;
  std::optional<StopControlWorkToken> terminal_projection_token;
  std::unique_ptr<StopSurveillanceReducerState> surveillance_state;
  std::optional<ScheduleHandle> surveillance_schedule_handle;
  std::uint64_t surveillance_schedule_generation{};
  bool surveillance_callback_pending{};
  std::optional<StopControlWorkToken> surveillance_projection_token;
  std::int64_t next_surveillance_sample_ns{};
  std::int64_t next_surveillance_checkpoint_ns{};
};

struct ResourceAddressedExecutionKernel::StopProjectionJob final {
  StopControlWorkToken token;
  bool active{};
  std::optional<EmergencyStopAcceptanceRecordDTO> acceptance;
  std::optional<EmergencyStopProjectedIntentDTO> intent;
  std::optional<StopAcceptedDTO> accepted;
  std::optional<EmergencyStopAffectedStateSnapshotDTO> affected_state;
  std::optional<StopStatusDTO> terminal_status;
  std::optional<StopCertificateDTO> terminal_certificate;
  std::optional<StopFaultLatchDTO> terminal_fault_latch;
  std::optional<StopPostTerminalSurveillanceDTO> surveillance_checkpoint;
  std::optional<StopFaultLatchDTO> surveillance_fault_latch;
};

namespace {

constexpr std::uint64_t kMaximumKernelJsonInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaximumResidentStopTerminalRecords = 64U;

[[nodiscard]] std::int64_t checked_deadline(std::int64_t now, std::int64_t duration) noexcept {
  if (now < 0 || duration <= 0 || now > kMaximumStopDurationNs - duration) {
    return -1;
  }
  return now + duration;
}

[[nodiscard]] RemainingStopBudgetDTO remaining_stop_budget(
    const PreparedParticipantStopPlanDTO& plan, const StopId& stop_id,
    const StopGeneration stop_generation, const std::int64_t now,
    const std::int64_t outer_deadline_ns) {
  const auto remaining_outer = now >= outer_deadline_ns ? 0 : outer_deadline_ns - now;
  RemainingStopBudgetDTO budget{stop_id, stop_generation, remaining_outer, {}};
  budget.phases.reserve(plan.deadline_budget.phases.size());
  const auto elapsed = plan.deadline_budget.outer_duration_ns - remaining_outer;
  for (const auto& phase : plan.deadline_budget.phases) {
    const auto remaining =
        remaining_outer <= plan.deadline_budget.kernel_completion_reserve_ns
            ? 0
            : std::max<std::int64_t>(
                  0, std::min(phase.deadline_offset_ns - elapsed,
                              remaining_outer - plan.deadline_budget.kernel_completion_reserve_ns));
    budget.phases.push_back({phase.phase_id, remaining});
  }
  return budget;
}

[[nodiscard]] std::optional<std::int64_t> remaining_phase_offset(
    const RemainingStopBudgetDTO& budget, const StopBudgetPhaseId& phase_id) noexcept {
  const auto found = std::find_if(budget.phases.begin(), budget.phases.end(),
                                  [&](const auto& phase) { return phase.phase_id == phase_id; });
  return found == budget.phases.end() ? std::nullopt
                                      : std::optional(found->remaining_deadline_offset_ns);
}

[[nodiscard]] std::optional<std::int64_t> stage_watchdog_deadline(
    const PreparedStopStageCommandDTO& command, const RemainingStopBudgetDTO& budget,
    const std::int64_t now) noexcept {
  auto maximum =
      remaining_phase_offset(budget, command.resolved_stage.containment_command.command_phase_id);
  if (!maximum.has_value() || *maximum <= 0) {
    return std::nullopt;
  }
  for (const auto& evidence : command.resolved_stage.evidence_requirements) {
    const auto remaining = remaining_phase_offset(budget, evidence.requirement.phase_id);
    if (!remaining.has_value()) {
      return std::nullopt;
    }
    maximum = std::max(*maximum, *remaining);
  }
  return checked_deadline(now, *maximum);
}

[[nodiscard]] Result<ResolvedParticipantStopStageDTO> resolve_participant_stop_stage(
    const StopParticipantWork& item, const StopId& stop_id, const StopGeneration stop_generation,
    const std::size_t stage_index, const RemainingStopBudgetDTO& budget) {
  const auto& binding = item.binding.value();
  const auto& path = *binding.control_path;
  const auto& selected = item.plan.stage_commands[stage_index].resolved_stage;
  std::optional<CapabilityProviderBindingId> provider_binding_id;
  std::optional<CapabilityProviderBindingRevision> provider_binding_revision;
  const auto provider_matches = [&](const RuntimeCapabilityProviderPathDTO& provider) {
    return provider.provider_instance_id == selected.command_target.provider_instance_id &&
           provider.provider_manifest_id == selected.command_target.provider_manifest_id &&
           provider.provider_manifest_revision ==
               selected.command_target.provider_manifest_revision &&
           provider.provider_runtime_unit_id == selected.command_target.runtime_unit_id &&
           provider.provider_runtime_epoch == selected.command_target.provider_runtime_epoch;
  };
  const auto select_provider = [&](const RuntimeCapabilityProviderPathDTO& provider) {
    provider_binding_id = provider.capability_provider_binding_id;
    provider_binding_revision = provider.capability_provider_binding_revision;
  };
  switch (selected.containment_command.command_kind) {
    case StopContainmentCommandKind::kControllerGoalCancel:
    case StopContainmentCommandKind::kMotionControlledStop:
      break;
    case StopContainmentCommandKind::kCapabilityCommandCancel:
    case StopContainmentCommandKind::kSafeOutputCommand:
    case StopContainmentCommandKind::kLogicalCancel:
    case StopContainmentCommandKind::kRegistered:
      if (binding.capability_provider_path.has_value() &&
          provider_matches(*binding.capability_provider_path)) {
        select_provider(*binding.capability_provider_path);
      } else {
        const auto supporting =
            std::find_if(binding.supporting_capability_provider_paths.begin(),
                         binding.supporting_capability_provider_paths.end(), provider_matches);
        if (supporting != binding.supporting_capability_provider_paths.end()) {
          select_provider(*supporting);
        }
      }
      break;
  }

  ResolvedParticipantStopStageDTO candidate{stop_id,
                                            stop_generation,
                                            item.operation_id,
                                            item.plan.operation_revision,
                                            item.plan.participant_id,
                                            item.controller_goal_id,
                                            item.controller_goal_generation,
                                            binding.controllable_component_id,
                                            binding.command_target_id,
                                            binding.capability_id,
                                            path.control_domain_id,
                                            path.control_domain_revision,
                                            path.control_binding_id,
                                            path.control_binding_revision,
                                            path.active_control_configuration_id,
                                            path.active_control_configuration_generation,
                                            path.controller_topology_id,
                                            path.topology_revision,
                                            path.control_route_id,
                                            std::move(provider_binding_id),
                                            std::move(provider_binding_revision),
                                            selected.command_target.provider_runtime_epoch,
                                            item.resources,
                                            selected.containment_granularity,
                                            path.stop_evidence_policy_id,
                                            path.stop_evidence_policy_revision,
                                            item.plan,
                                            stage_index,
                                            ContentDigest("pending")};
  return MakeResolvedParticipantStopStage(
      std::move(candidate), budget, item.readiness.value().resolved_stop_base_plan, item.binding,
      item.retained_default_stop_limits, item.node_category, item.resources);
}

[[nodiscard]] StopRequestedSelectorSnapshotDTO requested_selector_snapshot(
    const EmergencyStopProjectedIntentDTO& intent, const ContentDigest& digest) {
  if (const auto* ordinary = std::get_if<StopIntentDTO>(&intent)) {
    const auto kind = std::visit(
        [](const auto& rows) {
          using Rows = std::decay_t<decltype(rows)>;
          if constexpr (std::is_same_v<Rows, std::vector<StopOperationTargetDTO>>) {
            return StopRequestedSelectorKindDTO::kOrdinaryOperations;
          } else if constexpr (std::is_same_v<Rows, std::vector<StopComponentTargetDTO>>) {
            return StopRequestedSelectorKindDTO::kOrdinaryComponents;
          } else {
            return StopRequestedSelectorKindDTO::kOrdinaryResources;
          }
        },
        ordinary->selector);
    return StopRequestedSelectorSnapshotDTO{kind, digest, ordinary->selector, std::nullopt};
  }
  if (std::holds_alternative<FullStopIntentDTO>(intent)) {
    return StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull, digest,
                                            std::nullopt, std::nullopt};
  }
  return StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kMalformedIngress, digest,
                                          std::nullopt, "malformed_stop_ingress"};
}

[[nodiscard]] StopEscalationStageKind stage_kind(const PreparedParticipantStopPlanDTO& plan,
                                                 const std::size_t index) noexcept {
  return plan.stage_commands[index].resolved_stage.stage_kind;
}

[[nodiscard]] StopParticipantScopeFactsDTO participant_scope_facts(
    const StopParticipantWork& item) {
  const auto& binding = item.binding.value();
  const auto& path = *binding.control_path;
  return StopParticipantScopeFactsDTO{
      item.accepted_operation_revision,
      binding.controllable_component_id,
      binding.command_target_id,
      binding.capability_id,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      item.controller_goal_id,
      item.controller_goal_generation,
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      path.provider_runtime_epoch,
      item.resources,
      item.readiness.value().resolved_stop_base_plan.containment_granularity};
}

[[nodiscard]] StopScopeEntryDTO participant_scope_entry(
    const StopParticipantWork& item, const StopId& stop_id, const StopGeneration stop_generation,
    const std::int64_t accepted_at_ns, const std::int64_t remaining_outer_duration_ns) {
  std::vector<StopAcceptedPhaseBudgetDTO> phases;
  phases.reserve(item.plan.deadline_budget.phases.size());
  const auto elapsed = item.plan.deadline_budget.outer_duration_ns - remaining_outer_duration_ns;
  for (const auto& phase : item.plan.deadline_budget.phases) {
    phases.push_back(
        StopAcceptedPhaseBudgetDTO{phase.phase_id, phase.deadline_offset_ns,
                                   std::max<std::int64_t>(0, phase.deadline_offset_ns - elapsed)});
  }
  return StopScopeEntryDTO{
      StopScopeEntryKeyDTO{stop_id, stop_generation,
                           StopParticipantScopeKeyDTO{item.operation_id, item.plan.participant_id}},
      participant_scope_facts(item),
      StopScopeEntryDispositionDTO::kStoppable,
      item.cancelled_pre_dispatch ? StopScopeDispatchStateDTO::kNeverDispatchedCancelled
                                  : StopScopeDispatchStateDTO::kDispatched,
      item.readiness.value().resolved_stop_base_plan.stop_evidence_policy_id,
      item.readiness.value().resolved_stop_base_plan.stop_evidence_policy_revision,
      item.plan.participant_stop_invocation_plan_digest,
      accepted_at_ns,
      remaining_outer_duration_ns,
      std::move(phases)};
}

[[nodiscard]] bool stop_scope_entry_key_less(const StopScopeEntryKeyDTO& left,
                                             const StopScopeEntryKeyDTO& right) noexcept {
  if (left.origin_stop_id != right.origin_stop_id) {
    return left.origin_stop_id < right.origin_stop_id;
  }
  if (left.origin_stop_generation != right.origin_stop_generation) {
    return left.origin_stop_generation < right.origin_stop_generation;
  }
  if (left.subject.index() != right.subject.index()) {
    return left.subject.index() < right.subject.index();
  }
  if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&left.subject)) {
    const auto& other = std::get<StopParticipantScopeKeyDTO>(right.subject);
    return std::tie(participant->operation_id, participant->participant_id) <
           std::tie(other.operation_id, other.participant_id);
  }
  return std::get<ResourceId>(left.subject) < std::get<ResourceId>(right.subject);
}

[[nodiscard]] bool stop_surveillance_source_plan_less(
    const StopSurveillanceSourcePlanDTO& left,
    const StopSurveillanceSourcePlanDTO& right) noexcept {
  if (stop_scope_entry_key_less(left.scope_key, right.scope_key)) {
    return true;
  }
  if (stop_scope_entry_key_less(right.scope_key, left.scope_key)) {
    return false;
  }
  return left.evidence_requirement.evidence_item_id < right.evidence_requirement.evidence_item_id;
}

[[nodiscard]] Result<std::vector<StopSurveillanceSourcePlanDTO>> stop_surveillance_source_plans(
    const StopCertificateDTO& certificate) {
  std::vector<StopSurveillanceSourcePlanDTO> plans;
  for (const auto& attempt : certificate.attempted_stage_results) {
    if (attempt.authority != StopStageResultAuthorityDTO::kAuthoritative) {
      continue;
    }
    const auto stage_index = attempt.result.request.stage_index;
    const auto& stages = attempt.result.request.participant_stop_plan.stage_commands;
    if (stage_index >= stages.size()) {
      return reject<std::vector<StopSurveillanceSourcePlanDTO>>(
          "terminal Stop result names an invalid prepared stage", "stop_surveillance_stage_invalid",
          ErrorCode::kInvariantViolation);
    }
    for (const auto& resolved : stages[stage_index].resolved_stage.evidence_requirements) {
      plans.push_back(StopSurveillanceSourcePlanDTO{attempt.scope_key, resolved.requirement,
                                                    resolved.resolved_source});
    }
  }
  std::sort(plans.begin(), plans.end(), stop_surveillance_source_plan_less);
  plans.erase(std::unique(plans.begin(), plans.end()), plans.end());
  if (plans.empty() || plans.size() > kMaximumStopSurveillanceSources ||
      !std::all_of(plans.begin(), plans.end(), [](const auto& plan) {
        return !stop_surveillance_source_plan_field_error(plan).has_value();
      })) {
    return reject<std::vector<StopSurveillanceSourcePlanDTO>>(
        "terminal Stop has no canonical surveillance evidence sources",
        "stop_surveillance_sources_unavailable", ErrorCode::kMissingReference);
  }
  return Result<std::vector<StopSurveillanceSourcePlanDTO>>::Ok(std::move(plans));
}

[[nodiscard]] bool stop_requires_surveillance(const StopCertificateDTO& certificate,
                                              const StopFaultLatchDTO& latch) noexcept {
  return certificate.result == StopCertificateResultDTO::kPartial ||
         certificate.result == StopCertificateResultDTO::kFailedUncontained ||
         latch.trigger == StopFaultTriggerDTO::kPhysicalRestUnproven;
}

[[nodiscard]] StopFaultTriggerDTO primary_fault_trigger(const StopCertificateDTO& certificate,
                                                        const bool persistence_degraded) noexcept {
  if (certificate.result == StopCertificateResultDTO::kFailedUncontained) {
    return StopFaultTriggerDTO::kFailedUncontained;
  }
  if (certificate.result == StopCertificateResultDTO::kPartial) {
    return StopFaultTriggerDTO::kPartial;
  }
  if (certificate.result == StopCertificateResultDTO::kSucceededEscalated) {
    return StopFaultTriggerDTO::kEscalated;
  }
  if (certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
    return StopFaultTriggerDTO::kScopePromotion;
  }
  return persistence_degraded ? StopFaultTriggerDTO::kPersistenceDegraded
                              : StopFaultTriggerDTO::kPhysicalRestUnproven;
}

[[nodiscard]] std::string fault_trigger_reason(const StopFaultTriggerDTO trigger) {
  switch (trigger) {
    case StopFaultTriggerDTO::kScopePromotion:
      return "stop_scope_promoted";
    case StopFaultTriggerDTO::kEscalated:
      return "stop_escalated";
    case StopFaultTriggerDTO::kPartial:
      return "stop_partial";
    case StopFaultTriggerDTO::kFailedUncontained:
      return "stop_failed_uncontained";
    case StopFaultTriggerDTO::kPhysicalRestUnproven:
      return "stop_physical_rest_unproven";
    case StopFaultTriggerDTO::kPersistenceDegraded:
      return "stop_persistence_degraded";
  }
  return "stop_fault";
}

[[nodiscard]] bool terminal_fault_required(const StopCertificateDTO& certificate,
                                           const bool persistence_degraded) noexcept {
  return persistence_degraded ||
         certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull ||
         certificate.result != StopCertificateResultDTO::kSucceededNominal;
}

[[nodiscard]] StopFaultLatchDTO persistence_fault_latch(
    const CellId& cell_id, const RequestOwnerEpoch& owner_epoch,
    const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
    const StopCertificateDTO& certificate, const StopFaultLatchGeneration generation,
    StopDurableProjectionStatusDTO terminal_projection) {
  std::vector<OperationId> operations;
  std::vector<ResourceId> resources;
  operations.reserve(certificate.scope_entries.size());
  resources.reserve(certificate.scope_entries.size());
  for (const auto& entry : certificate.scope_entries) {
    if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&entry.key.subject)) {
      operations.push_back(participant->operation_id);
      const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry.facts);
      resources.insert(resources.end(), facts.resources.begin(), facts.resources.end());
    } else {
      resources.push_back(std::get<ResourceId>(entry.key.subject));
    }
  }
  std::sort(operations.begin(), operations.end());
  operations.erase(std::unique(operations.begin(), operations.end()), operations.end());
  std::sort(resources.begin(), resources.end());
  resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
  const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                        certificate.stop_certificate_revision, certificate.stop_id,
                                        certificate.stop_generation};
  return StopFaultLatchDTO{
      cell_id,
      certificate.stop_id,
      certificate.stop_generation,
      reference,
      StopFaultLatchId("stop-fault." + owner_epoch.value() + "." +
                       std::to_string(generation.value())),
      generation,
      StopFaultTriggerDTO::kPersistenceDegraded,
      true,
      std::move(operations),
      std::move(resources),
      retained_bundle_id,
      graph_revision,
      std::move(terminal_projection),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForDurableRecord,
      "stop_persistence_degraded",
      {}};
}

}  // namespace

class ResourceAddressedExecutionKernel::StopProjectionWorker final {
 public:
  StopProjectionWorker(const std::size_t capacity, ResourceAddressedExecutionKernel& owner)
      : capacity_(capacity),
        owner_(owner),
        requested_(std::make_unique<std::atomic_uint64_t[]>(capacity)),
        delivered_(std::make_unique<std::atomic_uint64_t[]>(capacity)),
        worker_([this](std::stop_token token) noexcept { Run(token); }) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return ready_.load(std::memory_order_acquire); });
  }

  ~StopProjectionWorker() {
    Close();
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Publish(const StopControlWorkToken token) noexcept {
    if (!accepting_.load(std::memory_order_acquire) || token.slot >= capacity_ ||
        !token.locally_valid()) {
      return;
    }
    auto retained = requested_[token.slot].load(std::memory_order_relaxed);
    while (token.generation > retained &&
           !requested_[token.slot].compare_exchange_weak(
               retained, token.generation, std::memory_order_release, std::memory_order_relaxed)) {
    }
    changed_.notify_all();
  }

  void Close() noexcept {
    accepting_.store(false, std::memory_order_release);
    changed_.notify_all();
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    try {
      std::unique_lock lock(mutex_);
      return changed_.wait_for(lock, timeout, [this] {
        if (callback_in_flight_.load(std::memory_order_acquire)) {
          return false;
        }
        for (std::size_t index = 0U; index < capacity_; ++index) {
          if (requested_[index].load(std::memory_order_acquire) !=
              delivered_[index].load(std::memory_order_acquire)) {
            return false;
          }
        }
        return true;
      });
    } catch (...) {
      return false;
    }
  }

 private:
  void Run(const std::stop_token stop_token) noexcept {
    ready_.store(true, std::memory_order_release);
    changed_.notify_all();
    while (!stop_token.stop_requested()) {
      bool progressed = false;
      for (std::size_t index = 0U; index < capacity_; ++index) {
        const auto requested = requested_[index].load(std::memory_order_acquire);
        if (requested <= delivered_[index].load(std::memory_order_acquire)) {
          continue;
        }
        callback_in_flight_.store(true, std::memory_order_release);
        owner_.ProjectStop(
            StopControlWorkToken{static_cast<StopControlSlotIndex>(index), requested});
        callback_in_flight_.store(false, std::memory_order_release);
        delivered_[index].store(requested, std::memory_order_release);
        progressed = true;
        changed_.notify_all();
      }
      if (progressed) {
        continue;
      }
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] {
        if (stop_token.stop_requested()) {
          return true;
        }
        for (std::size_t index = 0U; index < capacity_; ++index) {
          if (requested_[index].load(std::memory_order_acquire) >
              delivered_[index].load(std::memory_order_acquire)) {
            return true;
          }
        }
        return false;
      });
    }
  }

  std::size_t capacity_{};
  ResourceAddressedExecutionKernel& owner_;
  std::unique_ptr<std::atomic_uint64_t[]> requested_;
  std::unique_ptr<std::atomic_uint64_t[]> delivered_;
  std::atomic_bool accepting_{true};
  std::atomic_bool ready_{};
  std::atomic_bool callback_in_flight_{};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::jthread worker_;
};

ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

const ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) const noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

StopScopeResolverSnapshotDTO ResourceAddressedExecutionKernel::capture_stop_scope_locked(
    const ResourceSlotTableSnapshotDTO& slots) const {
  StopScopeResolverSnapshotDTO snapshot{config_.request_owner_epoch,
                                        *stop_graph_revision_,
                                        slots.operation_resource_generation,
                                        cell_stop_generation_,
                                        {},
                                        stop_component_scope_,
                                        {}};
  snapshot.operations.reserve(active_operations_.size() + historical_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    const bool effect_uncertain =
        operation->stop_effect_in_flight || operation->dispatch_effect_in_flight ||
        operation->state.state == OperationStateKind::kInterrupted ||
        operation->state.state == OperationStateKind::kInterventionRequired;
    snapshot.operations.push_back(StopScopeOperationSnapshotDTO{
        operation_id,
        operation->state.operation_revision,
        operation->state.active_stop_generation.value_or(StopGeneration(0U)),
        operation->state.state,
        true,
        effect_uncertain,
        {operation->binding.value().controllable_component_id},
        operation->resources});
  }
  for (const auto& operation : historical_operations_) {
    if (active_operations_.contains(operation.operation_id)) {
      continue;
    }
    snapshot.operations.push_back(
        StopScopeOperationSnapshotDTO{operation.operation_id,
                                      operation.operation_revision,
                                      operation.active_stop_generation.value_or(StopGeneration(0U)),
                                      operation.state,
                                      false,
                                      false,
                                      {},
                                      {}});
  }
  std::sort(
      snapshot.operations.begin(), snapshot.operations.end(),
      [](const auto& left, const auto& right) { return left.operation_id < right.operation_id; });

  snapshot.resources.reserve(slots.slots.size());
  for (const auto& slot : slots.slots) {
    bool effect_uncertain = slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal;
    if (slot.owner.has_value()) {
      const auto* operation = find_active_operation_locked(slot.owner->operation_id);
      effect_uncertain = effect_uncertain || operation == nullptr ||
                         operation->stop_effect_in_flight || operation->dispatch_effect_in_flight ||
                         operation->state.state == OperationStateKind::kInterrupted ||
                         operation->state.state == OperationStateKind::kInterventionRequired;
    }
    snapshot.resources.push_back(
        StopScopeResourceSnapshotDTO{slot.resource_id, slot.stop_generation, effect_uncertain});
  }
  return snapshot;
}

std::optional<OperationId> ResourceAddressedExecutionKernel::find_event_operation_locked(
    const IResolvedParticipantExecutionEngine* engine, const std::uint64_t subordinate_id) const {
  if (engine == nullptr || subordinate_id == 0U) {
    return std::nullopt;
  }
  std::optional<OperationId> result;
  for (const auto& [operation_id, operation] : active_operations_) {
    if (operation->engine != engine || operation->subordinate_id != subordinate_id) {
      continue;
    }
    if (result.has_value()) {
      // Admission prevents one engine instance from owning two operations.
      // Fail closed if corrupted state ever violates that invariant.
      return std::nullopt;
    }
    result = operation_id;
  }
  return result;
}

struct ResourceAddressedExecutionKernel::EngineSubscription final {
  IResolvedParticipantExecutionEngine* engine{};
  SubscriptionHandle handle{};
};

ResourceAddressedExecutionKernel::ResourceAddressedExecutionKernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config)
    : registry_(extract_bindings(registrations)),
      admission_mutex_(config.operation_admission_gate != nullptr
                           ? config.operation_admission_gate->shared_admission_mutex()
                           : std::make_shared<std::recursive_mutex>()),
      resource_slots_(std::make_shared<ResourceSlotTable>(
          registry_.resource_inventory(), extract_active_control_configurations(registrations),
          admission_mutex_)),
      registrations_(std::move(registrations)),
      clock_(clock),
      journal_(journal),
      config_(std::move(config)),
      emergency_stop_reserve_(config_.emergency_stop_correlation_capacity),
      stop_transactions_(config_.emergency_stop_correlation_capacity + 1U),
      stop_correlations_(config_.emergency_stop_correlation_capacity + 1U),
      stop_projection_jobs_(config_.stop_forensic_projection_capacity == 0U
                                ? 2U * (config_.emergency_stop_correlation_capacity + 1U) +
                                      kMaximumResidentStopTerminalRecords
                                : config_.stop_forensic_projection_capacity),
      stop_projection_generations_(stop_projection_jobs_.size()) {
  if (config_.request_owner_epoch.empty() || config_.cell_id.empty() ||
      config_.request_replay_capacity == 0U ||
      config_.request_replay_capacity > kMaxI6OperationReplayEntries) {
    throw std::invalid_argument(
        "addressed Kernel request-owner epoch, cell identity, or replay bound is invalid");
  }
  if (config_.operation_admission_gate != nullptr &&
      !resource_slots_->shares_admission_mutex(
          config_.operation_admission_gate->shared_admission_mutex())) {
    throw std::invalid_argument(
        "addressed Kernel admission gate does not share its resource mutex");
  }
  const auto maximum_projection_capacity =
      2U * (config_.emergency_stop_correlation_capacity + 1U) + kMaximumResidentStopTerminalRecords;
  if (stop_projection_jobs_.empty() || stop_projection_jobs_.size() > maximum_projection_capacity) {
    throw std::invalid_argument("addressed Kernel forensic projection bound is invalid");
  }
  if (config_.stop_surveillance.has_value()) {
    const auto& surveillance = *config_.stop_surveillance;
    if (surveillance.scheduler == nullptr || surveillance.observer == nullptr ||
        surveillance.policy.sample_cadence_ns <= 0 ||
        surveillance.policy.checkpoint_interval_ns < surveillance.policy.sample_cadence_ns ||
        surveillance.policy.retained_checkpoint_limit == 0U ||
        surveillance.policy.retained_checkpoint_limit > kMaximumStopSurveillanceCheckpoints) {
      throw std::invalid_argument("addressed Kernel Stop-surveillance owner is invalid");
    }
  }
  auto stop_facts = derive_stop_static_facts(registrations_, config_);
  stop_retained_bundle_id_ = std::move(stop_facts.retained_bundle_id);
  stop_graph_revision_ = std::move(stop_facts.graph_revision);
  cell_stop_deadline_budget_ = std::move(stop_facts.deadline_budget);
  stop_component_scope_ = std::move(stop_facts.components);
  for (const auto& configuration : extract_active_control_configurations(registrations_)) {
    const bool fully_qualified =
        std::all_of(registrations_.begin(), registrations_.end(), [&](const auto& registration) {
          return !registration.binding.value().control_path.has_value() ||
                 registration.binding.value().control_path->control_domain_id !=
                     configuration.control_domain_id ||
                 registration.stop_readiness.has_value();
        });
    if (!fully_qualified) {
      runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
    }
  }
  replay_ledger_.reserve(config_.request_replay_capacity);
  control_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_replay_ledger_.reserve(config_.request_replay_capacity);
  full_stop_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_fault_recovery_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_terminal_history_.reserve(kMaximumResidentStopTerminalRecords);
  for (std::size_t index = 0U; index < stop_transactions_.size(); ++index) {
    stop_transactions_[index] = std::make_unique<StopTransaction>();
  }
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    stop_projection_jobs_[index] = std::make_unique<StopProjectionJob>();
  }
  stop_projection_worker_ =
      std::make_unique<StopProjectionWorker>(stop_projection_jobs_.size(), *this);
  stop_control_executor_ = std::make_unique<BoundedStopControlExecutor>(
      emergency_stop_reserve_.correlation_capacity() + 1U, clock_,
      static_cast<IStopControlWorkReceiver&>(*this));
  if (!stop_control_executor_->prequalified_ready() ||
      stop_control_executor_->capacity() != emergency_stop_reserve_.correlation_capacity() + 1U) {
    throw std::runtime_error("StopControl executor failed its reserve-capacity admission join");
  }
  const auto initially_qualified_participants = qualified_execution_engine_count(registrations_);
  prequalified_max_live_stop_participants_ =
      config_.prequalified_max_live_stop_participants == 0U
          ? std::max<std::size_t>(1U, initially_qualified_participants)
          : config_.prequalified_max_live_stop_participants;
  if (initially_qualified_participants > prequalified_max_live_stop_participants_ ||
      prequalified_max_live_stop_participants_ >
          kMaximumPrequalifiedStopStageAttemptLanes / kMaxStopEscalationStages) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants exceed the prequalified attempt-isolation bound");
  }
  stop_stage_attempt_executor_ = std::make_unique<BoundedStopStageAttemptExecutor>(
      prequalified_max_live_stop_participants_ * kMaxStopEscalationStages,
      prequalified_max_live_stop_participants_,
      static_cast<IStopStageAttemptCompletionReceiver&>(*this));
  if (!stop_stage_attempt_executor_->prequalified_ready() ||
      stop_stage_attempt_executor_->prequalified_max_live_participants() !=
          prequalified_max_live_stop_participants_) {
    throw std::runtime_error("Stop-stage attempt isolation failed its admission prequalification");
  }

  auto recovery =
      journal_.BeginOwnerEpoch(config_.request_owner_epoch, clock_.now(),
                               config_.request_replay_capacity, registry_.resource_inventory());
  if (!recovery.ok()) {
    throw std::runtime_error("addressed Kernel durable owner admission failed: " +
                             recovery.error().code + ": " + recovery.error().message);
  }
  historical_operations_ = std::move(recovery.value().historical_operations);
  if (recovery.value().recovered_stops.size() > kMaximumResidentStopTerminalRecords) {
    throw std::runtime_error("recovered Stop terminal history exceeds its fixed bound");
  }
  for (const auto& recovered : recovery.value().recovered_stops) {
    cell_stop_generation_ = std::max(cell_stop_generation_, recovered.accepted.stop_generation);
    if (!recovered.terminal_certificate.has_value()) {
      // An accepted Stop with no projected terminal certificate is never
      // reconstructed or redispatched. BeginOwnerEpoch supplies the complete
      // current inventory fence for an unclosed predecessor; keep motion
      // admission closed until the separate reconciliation owner clears it.
      motion_admission_closed_ = true;
      motion_admission_unreconciled_ = true;
      continue;
    }
    const auto& certificate = *recovered.terminal_certificate;
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                          certificate.stop_certificate_revision,
                                          certificate.stop_id, certificate.stop_generation};
    StopStatusDTO status{
        recovered.accepted,
        terminal_state,
        certificate.scope_entries,
        {},
        certificate.attempted_stage_results,
        certificate.evidence_items,
        certificate.unsettled,
        certificate.failed,
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        recovered.surveillance.has_value()
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                             std::nullopt, std::nullopt},
        reference,
        recovered.fault_latch,
        recovered.surveillance};
    if (stop_terminal_projection_field_error(status, certificate, recovered.fault_latch)
            .has_value()) {
      throw std::runtime_error("recovered Stop terminal projection is invalid");
    }
    std::unique_ptr<StopSurveillanceReducerState> recovered_surveillance_state;
    std::int64_t next_surveillance_sample_ns = 0;
    std::int64_t next_surveillance_checkpoint_ns = 0;
    if (recovered.surveillance.has_value()) {
      if (!config_.stop_surveillance.has_value() || !recovered.fault_latch.has_value()) {
        motion_admission_closed_ = true;
      } else {
        auto source_plans = stop_surveillance_source_plans(certificate);
        if (!source_plans.ok()) {
          throw std::runtime_error("recovered Stop surveillance sources are unavailable");
        }
        auto adopted = AdoptStopSurveillanceReducerState(
            config_.request_owner_epoch, certificate, *recovered.fault_latch,
            *recovered.surveillance, std::move(source_plans.value()));
        if (!adopted.ok()) {
          throw std::runtime_error("recovered Stop surveillance checkpoint is invalid");
        }
        recovered_surveillance_state =
            std::make_unique<StopSurveillanceReducerState>(std::move(adopted.value()));
        const auto now = clock_.now();
        const auto& policy = config_.stop_surveillance->policy;
        if (now > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns ||
            now > std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
          throw std::runtime_error("recovered Stop surveillance deadline is exhausted");
        }
        next_surveillance_sample_ns = now + policy.sample_cadence_ns;
        next_surveillance_checkpoint_ns = now + policy.checkpoint_interval_ns;
      }
    }
    stop_terminal_history_.push_back(std::make_unique<StopTerminalRecord>(StopTerminalRecord{
        std::move(status), certificate, recovered.fault_latch, std::nullopt,
        std::move(recovered_surveillance_state), std::nullopt, 0U, false, std::nullopt,
        next_surveillance_sample_ns, next_surveillance_checkpoint_ns}));
    if (recovered.fault_latch.has_value()) {
      cell_stop_fault_latch_generation_ =
          std::max(cell_stop_fault_latch_generation_, recovered.fault_latch->latch_generation);
      if (recovered.fault_latch->state != StopFaultLatchStateDTO::kCleared &&
          (!active_stop_fault_latch_.has_value() ||
           active_stop_fault_latch_->latch_generation < recovered.fault_latch->latch_generation)) {
        active_stop_fault_latch_ = recovered.fault_latch;
        motion_admission_closed_ = true;
      }
    }
    if (recovered.surveillance.has_value() &&
        recovered.surveillance->state == StopPostTerminalSurveillanceStateDTO::kMonitoring) {
      motion_admission_closed_ = true;
    }
  }
  if (recovery.value().replaced_unclosed_owner_epoch.has_value()) {
    motion_admission_closed_ = true;
    motion_admission_unreconciled_ = true;
  }
  if (!recovery.value().unreconciled_resources.empty()) {
    motion_admission_closed_ = true;
    motion_admission_unreconciled_ = true;
    auto marked = resource_slots_->mark_unreconciled_external_or_orphan_goal(
        recovery.value().unreconciled_resources);
    if (!marked.ok()) {
      throw std::runtime_error("addressed Kernel durable orphan recovery failed: " +
                               marked.error().code + ": " + marked.error().message);
    }
  }

  for (const auto& registration : registrations_) {
    if (std::any_of(
            engine_subscriptions_.begin(), engine_subscriptions_.end(),
            [&](const EngineSubscription& item) { return item.engine == registration.engine; })) {
      continue;
    }
    auto* engine = registration.engine;
    const auto handle = engine->subscribe_events(
        [this, engine](const KernelEvent& event) { on_engine_event(engine, event); });
    engine_subscriptions_.push_back(EngineSubscription{engine, handle});
  }
  for (const auto& record : stop_terminal_history_) {
    if (record != nullptr && record->surveillance_state != nullptr &&
        record->surveillance_state->projection.state ==
            StopPostTerminalSurveillanceStateDTO::kMonitoring) {
      ScheduleStopSurveillance(record->certificate.stop_certificate_id,
                               record->next_surveillance_sample_ns);
    }
  }
}

ResourceAddressedExecutionKernel::~ResourceAddressedExecutionKernel() {
  std::vector<ScheduleHandle> surveillance_handles;
  if (config_.stop_surveillance.has_value()) {
    {
      std::unique_lock state_lock(state_mutex_);
      stop_surveillance_shutting_down_ = true;
      if (!stop_surveillance_schedule_cv_.wait_for(state_lock, std::chrono::seconds(5), [&] {
            return stop_surveillance_schedule_publications_ == 0U;
          })) {
        std::terminate();
      }
      for (const auto& record : stop_terminal_history_) {
        if (record != nullptr && record->surveillance_schedule_handle.has_value()) {
          surveillance_handles.push_back(*record->surveillance_schedule_handle);
          record->surveillance_schedule_handle.reset();
          record->surveillance_callback_pending = false;
        }
      }
    }
    for (const auto handle : surveillance_handles) {
      try {
        config_.stop_surveillance->scheduler->cancel(handle);
      } catch (...) {
        std::terminate();
      }
    }
    try {
      if (!config_.stop_surveillance->scheduler->drain_for(SchedulerPriority::Monitoring,
                                                           std::chrono::seconds(5))) {
        std::terminate();
      }
    } catch (...) {
      std::terminate();
    }
  }
  if (stop_stage_attempt_executor_ != nullptr) {
    stop_stage_attempt_executor_->CloseIngress();
    if (!stop_stage_attempt_executor_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
  }
  if (stop_control_executor_ != nullptr) {
    // Attempt completion callbacks remain allowed to rearm/publish their
    // owning StopControl slot until every isolated provider call has drained.
    // Drain those causal wakeups before closing StopControl ingress.
    if (!stop_control_executor_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
    stop_control_executor_->CloseIngress();
    stop_control_executor_.reset();
  }
  stop_stage_attempt_executor_.reset();
  if (stop_projection_worker_ != nullptr) {
    stop_projection_worker_->Close();
    if (!stop_projection_worker_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
    stop_projection_worker_.reset();
  }
  bool clean_owner = false;
  {
    std::lock_guard state_lock(state_mutex_);
    clean_owner =
        active_operations_.empty() && !motion_admission_unreconciled_ &&
        std::none_of(stop_projection_jobs_.begin(), stop_projection_jobs_.end(),
                     [](const auto& job) { return job != nullptr && job->active; }) &&
        std::none_of(stop_transactions_.begin(), stop_transactions_.end(),
                     [](const auto& transaction) {
                       return transaction != nullptr && transaction->active &&
                              !transaction->terminal;
                     }) &&
        std::all_of(stop_terminal_history_.begin(), stop_terminal_history_.end(),
                    [](const auto& record) {
                      return record != nullptr &&
                             record->status.acceptance_projection.state ==
                                 StopDurableProjectionStateDTO::kRecorded &&
                             record->status.terminal_projection.state ==
                                 StopDurableProjectionStateDTO::kRecorded &&
                             (record->status.surveillance_projection.state ==
                                  StopDurableProjectionStateDTO::kNotApplicable ||
                              (record->status.surveillance_projection.state ==
                                   StopDurableProjectionStateDTO::kRecorded &&
                               record->status.surveillance.has_value() &&
                               record->status.surveillance->state ==
                                   StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved)) &&
                             (!record->fault_latch.has_value() ||
                              record->fault_latch->state == StopFaultLatchStateDTO::kCleared);
                    });
  }
  if (clean_owner) {
    static_cast<void>(journal_.CloseOwnerEpoch(config_.request_owner_epoch, clock_.now()));
  }
  for (const auto& subscription : engine_subscriptions_) {
    subscription.engine->unsubscribe_events(subscription.handle);
  }
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_participants(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (qualified_execution_engine_count(registrations) > prequalified_max_live_stop_participants_) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              "candidate runtime participants exceed prequalified Stop-stage isolation",
              "stop_stage_attempt_prequalification_exceeded"));
  }
  std::optional<RuntimeParticipantRegistry> candidate_registry;
  try {
    candidate_registry.emplace(extract_bindings(registrations));
  } catch (const std::exception& exception) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              std::string("candidate runtime participant registry is invalid: ") + exception.what(),
              "runtime_participant_reconfiguration_invalid"));
  }
  std::vector<DomainActiveControlConfigurationDTO> candidate_configurations;
  std::optional<StopStaticFacts> candidate_stop_facts;
  std::set<ControlDomainId> changed_domains;
  try {
    candidate_configurations = extract_active_control_configurations(registrations);
    changed_domains = changed_registration_domains(registrations_, registrations);
    if (registrations.empty()) {
      candidate_stop_facts.emplace(StopStaticFacts{
          *stop_retained_bundle_id_, *stop_graph_revision_, cell_stop_deadline_budget_, {}});
    } else {
      candidate_stop_facts.emplace(derive_stop_static_facts(registrations, config_));
    }
  } catch (const std::exception& exception) {
    return Result<void>::Err(error(
        std::string(to_string(ErrorCode::kInvalidRequest)),
        std::string("candidate runtime control configuration is invalid: ") + exception.what(),
        "runtime_participant_reconfiguration_invalid"));
  }

  std::lock_guard submit_lock(submission_mutex_);
  std::vector<EngineSubscription> added_subscriptions;
  try {
    for (const auto& registration : registrations) {
      const auto already_subscribed = std::any_of(
          engine_subscriptions_.begin(), engine_subscriptions_.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      const auto newly_subscribed = std::any_of(
          added_subscriptions.begin(), added_subscriptions.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      if (already_subscribed || newly_subscribed) {
        continue;
      }
      auto* engine = registration.engine;
      const auto handle = engine->subscribe_events(
          [this, engine](const KernelEvent& event) { on_engine_event(engine, event); });
      added_subscriptions.push_back(EngineSubscription{engine, handle});
    }
  } catch (const std::exception& exception) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInternalError)),
              std::string("candidate runtime event subscription failed: ") + exception.what(),
              "runtime_participant_subscription_failed"));
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime event subscription failed",
                                   "runtime_participant_subscription_failed"));
  }

  try {
    engine_subscriptions_.reserve(engine_subscriptions_.size() + added_subscriptions.size());
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime subscription table reached capacity",
                                   "runtime_participant_subscription_capacity_unavailable"));
  }

  std::optional<ErrorDTO> table_error;
  {
    std::lock_guard state_lock(state_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    auto table_result = resource_slots_->reconfigure_runtime_inventory(
        candidate_registry->resource_inventory(), candidate_configurations);
    if (!table_result.ok()) {
      table_error = table_result.error();
    } else {
      for (auto readback = runtime_route_configuration_readbacks_.begin();
           readback != runtime_route_configuration_readbacks_.end();) {
        if (!changed_domains.contains(readback->first)) {
          ++readback;
          continue;
        }
        const auto candidate = std::find_if(
            candidate_configurations.begin(), candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == readback->first; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback->second.active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback->second.active_configuration.active_control_configuration_generation) {
          readback = runtime_route_configuration_readbacks_.erase(readback);
        } else {
          ++readback;
        }
      }
      for (auto required = runtime_route_configuration_publication_required_.begin();
           required != runtime_route_configuration_publication_required_.end();) {
        if (!changed_domains.contains(*required)) {
          ++required;
          continue;
        }
        const auto present =
            std::any_of(candidate_configurations.begin(), candidate_configurations.end(),
                        [&](const auto& item) { return item.control_domain_id == *required; });
        required = present ? std::next(required)
                           : runtime_route_configuration_publication_required_.erase(required);
      }
      for (const auto& configuration : candidate_configurations) {
        if (!changed_domains.contains(configuration.control_domain_id)) {
          continue;
        }
        const auto current =
            runtime_route_configuration_readbacks_.find(configuration.control_domain_id);
        if (current == runtime_route_configuration_readbacks_.end()) {
          runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
        }
      }
      registry_ = std::move(*candidate_registry);
      registrations_ = std::move(registrations);
      stop_retained_bundle_id_ = std::move(candidate_stop_facts->retained_bundle_id);
      stop_graph_revision_ = std::move(candidate_stop_facts->graph_revision);
      cell_stop_deadline_budget_ = std::move(candidate_stop_facts->deadline_budget);
      stop_component_scope_ = std::move(candidate_stop_facts->components);
      config_.participant_resolver = std::move(participant_resolver);
      engine_subscriptions_.insert(engine_subscriptions_.end(),
                                   std::make_move_iterator(added_subscriptions.begin()),
                                   std::make_move_iterator(added_subscriptions.end()));
    }
  }
  if (table_error.has_value()) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(std::move(*table_error));
  }
  return Result<void>::Ok();
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::publish_runtime_route_configuration(
    ActiveControlConfigurationDTO configuration,
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!configuration.locally_valid()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route publication has an invalid active configuration",
        "runtime_route_configuration_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  const bool reconciled = configuration.state == ActiveControlConfigurationStateDTO::kReconciled;
  if (reconciled) {
    auto projected = derive_route_readiness_projection(configuration, registrations);
    if (!projected.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(projected.error());
    }
    readiness_rows = std::move(projected.value());
  } else if (configuration.state !=
                 ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled ||
             !registrations.empty() || participant_resolver) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "unreconciled route publication cannot install runtime participants",
        "runtime_route_unreconciled_registration_present", ErrorCode::kInvalidRequest);
  }

  RuntimeRouteConfigurationReadbackDTO candidate{std::move(configuration),
                                                 std::move(readiness_rows), reconciled};
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO> staged_readback;
  try {
    staged_readback.emplace(candidate.active_configuration.control_domain_id, candidate);
  } catch (...) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback could not be staged before publication",
        "runtime_route_readback_capacity_unavailable", ErrorCode::kInternalError);
  }
  auto staged_node = staged_readback.extract(staged_readback.begin());
  if (reconciled) {
    auto installed =
        reconfigure_runtime_participants(std::move(registrations), std::move(participant_resolver));
    if (!installed.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(installed.error());
    }
  }
  std::lock_guard submit_lock(submission_mutex_);
  if (reconciled) {
    auto current_rows =
        derive_route_readiness_projection(candidate.active_configuration, registrations_);
    if (!current_rows.ok() || current_rows.value() != candidate.readiness_rows) {
      return reject<RuntimeRouteConfigurationReadbackDTO>(
          "runtime registration changed before route publication readback",
          "runtime_route_publication_raced", ErrorCode::kInvariantViolation);
    }
  }
  {
    std::lock_guard state_lock(state_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    runtime_route_configuration_readbacks_.erase(candidate.active_configuration.control_domain_id);
    runtime_route_configuration_readbacks_.insert(std::move(staged_node));
    runtime_route_configuration_publication_required_.erase(
        candidate.active_configuration.control_domain_id);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(std::move(candidate));
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::runtime_route_configuration_readback(
    const ControlDomainId& control_domain_id,
    const ActiveControlConfigurationId& active_configuration_id,
    const ActiveControlConfigurationGeneration active_configuration_generation) const {
  if (control_domain_id.empty() || active_configuration_id.empty() ||
      !active_configuration_generation.nonzero()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback query has an invalid identity",
        "runtime_route_readback_query_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end() ||
      found->second.active_configuration.active_control_configuration_id !=
          active_configuration_id ||
      found->second.active_configuration.active_control_configuration_generation !=
          active_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route configuration readback is absent or stale", "runtime_route_readback_missing",
        ErrorCode::kMissingReference);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(found->second);
}

bool ResourceAddressedExecutionKernel::participant_engine_in_use(
    const IResolvedParticipantExecutionEngine* engine) const {
  if (engine == nullptr) {
    return false;
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  return std::any_of(active_operations_.begin(), active_operations_.end(),
                     [&](const auto& item) { return item.second->engine == engine; }) ||
         std::any_of(registrations_.begin(), registrations_.end(),
                     [&](const auto& registration) { return registration.engine == engine; });
}

Result<void> ResourceAddressedExecutionKernel::release_retired_engine(
    IResolvedParticipantExecutionEngine* engine) {
  if (engine == nullptr) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInvalidRequest)),
                                   "retired participant engine is absent",
                                   "retired_participant_engine_absent"));
  }
  std::lock_guard submit_lock(submission_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    if (std::any_of(active_operations_.begin(), active_operations_.end(),
                    [&](const auto& item) { return item.second->engine == engine; }) ||
        std::any_of(registrations_.begin(), registrations_.end(),
                    [&](const auto& registration) { return registration.engine == engine; })) {
      return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                     "retired participant engine remains in use",
                                     "retired_participant_engine_in_use"));
    }
  }
  const auto subscription =
      std::find_if(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                   [&](const EngineSubscription& item) { return item.engine == engine; });
  if (subscription == engine_subscriptions_.end()) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kMissingReference)),
                                   "retired participant engine has no subscription",
                                   "retired_participant_engine_subscription_absent"));
  }
  engine->unsubscribe_events(subscription->handle);
  engine_subscriptions_.erase(subscription);
  return Result<void>::Ok();
}

struct ResourceAddressedExecutionKernel::PreviewSession final : IAddressedMotionPreviewSession {
  PreviewSession(ResourceAddressedExecutionKernel& owner, RuntimeParticipantBinding primary_binding,
                 IResolvedParticipantExecutionEngine& primary_engine, IMotionPreview& motion,
                 std::unique_lock<std::mutex> submission_lock)
      : owner_(owner),
        primary_binding_(std::move(primary_binding)),
        primary_engine_(primary_engine),
        motion_(motion),
        submission_lock_(std::move(submission_lock)) {}

  [[nodiscard]] Result<RuntimeParticipantBindingDTO> resolve_binding(
      const OperationIntentDTO& intent) const override {
    auto validation = validate_operation_intent(intent);
    if (!validation.ok()) {
      return Result<RuntimeParticipantBindingDTO>::Err(validation.error());
    }
    if (intent.request_context.request_owner_epoch != owner_.config_.request_owner_epoch) {
      return Result<RuntimeParticipantBindingDTO>::Err(
          error("stale_request_owner_epoch", "preview request names a stale Kernel owner epoch",
                "stale_request_owner_epoch"));
    }
    if (intent.participants.size() != 1U ||
        intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
      return reject<RuntimeParticipantBindingDTO>("I6 preview admits one independent participant",
                                                  "i6_preview_single_participant_only");
    }
    auto resolved = owner_.resolve_registration(intent, 0U);
    if (!resolved.ok()) {
      return Result<RuntimeParticipantBindingDTO>::Err(resolved.error());
    }
    if (resolved.value().engine != &primary_engine_) {
      return reject<RuntimeParticipantBindingDTO>(
          "linked preview motions cross their exact execution engine",
          "motion_preview_engine_changed", ErrorCode::kInvariantViolation);
    }
    return Result<RuntimeParticipantBindingDTO>::Ok(resolved.value().binding.value());
  }

  [[nodiscard]] Result<ExecuteRequestDTO> prepare_motion(
      const OperationIntentDTO& intent) override {
    auto resolved = resolve_binding(intent);
    if (!resolved.ok()) {
      return Result<ExecuteRequestDTO>::Err(resolved.error());
    }
    if (!same_linked_preview_participant(resolved.value(), primary_binding_.value())) {
      return reject<ExecuteRequestDTO>(
          "linked preview motions cross their exact runtime participant",
          "motion_preview_binding_changed");
    }
    if (!std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)) {
      return reject<ExecuteRequestDTO>("preview preparation requires a motion participant",
                                       "motion_preview_payload_not_motion",
                                       ErrorCode::kInvalidRequest);
    }
    if (next_preview_id_ == 0U || next_preview_id_ == std::numeric_limits<std::uint64_t>::max()) {
      return reject<ExecuteRequestDTO>("preview identity space is exhausted",
                                       "motion_preview_id_exhausted", ErrorCode::kInternalError);
    }

    const std::string prefix = "preview:" + owner_.config_.request_owner_epoch.value() + ":" +
                               std::to_string(next_preview_id_++);
    const OperationId operation_id(prefix);
    const OperationParticipantId participant_id(prefix + ":participant:1");
    const ControllerGoalId controller_goal_id(prefix + ":controller-goal:1");
    return build_motion_request(
        intent, intent.participants.front(),
        std::get<MotionParticipantIntentDTO>(intent.participants.front().payload), resolved.value(),
        operation_id, OperationRevision(1U), participant_id, controller_goal_id,
        ControllerGoalGeneration(1U));
  }

  [[nodiscard]] Result<RobotStateDTO> current_state() const override {
    return motion_.current_preview_state(owner_.clock_.now());
  }

  [[nodiscard]] Result<std::vector<Pose3DTO>> resolve_reference_poses(
      const OperationIntentDTO& motion_context, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& start_state,
      std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings) override {
    auto prepared = prepare_motion(motion_context);
    if (!prepared.ok()) {
      return Result<std::vector<Pose3DTO>>::Err(prepared.error());
    }
    prepared.value().runtime_object_bindings = std::move(runtime_object_bindings);
    return motion_.resolve_preview_reference_poses(
        std::move(prepared.value()), std::move(references), projection_inventory, start_state);
  }

  [[nodiscard]] Result<MotionPreviewResultDTO> preview_motion(
      OperationIntentDTO intent, CollisionWorldLeaseInventory projection_inventory,
      RobotStateDTO start_state, const std::uint32_t planning_attempt_index,
      const std::stop_token stop_token) override {
    if (!intent.expected_scene_revision.has_value() ||
        *intent.expected_scene_revision !=
            projection_inventory.projection_binding.projection_revision.scene_revision) {
      return reject<MotionPreviewResultDTO>(
          "preview intent does not name the exact retained scene revision",
          "motion_preview_scene_revision_mismatch", ErrorCode::kSceneChangedDuringValidation);
    }
    auto prepared = prepare_motion(intent);
    if (!prepared.ok()) {
      return Result<MotionPreviewResultDTO>::Err(prepared.error());
    }
    return motion_.preview_motion(std::move(prepared.value()), std::move(projection_inventory),
                                  std::move(start_state), planning_attempt_index, stop_token);
  }

 private:
  ResourceAddressedExecutionKernel& owner_;
  RuntimeParticipantBinding primary_binding_;
  IResolvedParticipantExecutionEngine& primary_engine_;
  IMotionPreview& motion_;
  // This is intentionally retained for the complete multi-leg candidate
  // transaction. The independent control path never takes this mutex.
  std::unique_lock<std::mutex> submission_lock_;
  std::uint64_t next_preview_id_{1U};
};

Result<std::unique_ptr<IAddressedMotionPreviewSession>>
ResourceAddressedExecutionKernel::begin_motion_preview_session(
    const OperationIntentDTO& primary_motion) {
  std::unique_lock submission_lock(submission_mutex_);
  auto validation = validate_operation_intent(primary_motion);
  if (!validation.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(validation.error());
  }
  if (primary_motion.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
        error("stale_request_owner_epoch", "preview request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  if (primary_motion.participants.size() != 1U ||
      primary_motion.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<MotionParticipantIntentDTO>(
          primary_motion.participants.front().payload)) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "I6 preview requires one independent motion participant",
        "i6_preview_primary_motion_invalid", ErrorCode::kInvalidRequest);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(primary_motion.request_context.request_id.value())
            .has_value()) {
      return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    if (!active_operations_.empty()) {
      return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
          "motion preview cannot begin while an operation is unsettled",
          "motion_preview_operation_busy");
    }
  }
  auto resolved = resolve_registration(primary_motion, 0U);
  if (!resolved.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(resolved.error());
  }
  auto* engine = resolved.value().engine;
  auto* motion = dynamic_cast<IMotionPreview*>(engine);
  if (motion == nullptr) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "resolved participant has no non-dispatching motion preview",
        "motion_preview_engine_unavailable", ErrorCode::kUnsupportedCapability);
  }
  std::unique_ptr<IAddressedMotionPreviewSession> session = std::make_unique<PreviewSession>(
      *this, std::move(resolved.value().binding), *engine, *motion, std::move(submission_lock));
  return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Ok(std::move(session));
}

Result<CapabilityCommandPreviewResultDTO>
ResourceAddressedExecutionKernel::preview_capability_command(const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);
  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(error(
        "stale_request_owner_epoch", "capability-command preview names a stale Kernel owner epoch",
        "stale_request_owner_epoch"));
  }
  if (intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<CapabilityCommandIntentDTO>(intent.participants.front().payload)) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "I6 command preview requires one independent capability command",
        "i6_preview_capability_command_invalid", ErrorCode::kInvalidRequest);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (!active_operations_.empty()) {
      return reject<CapabilityCommandPreviewResultDTO>(
          "capability-command preview cannot begin while an operation is "
          "unsettled",
          "capability_command_preview_operation_busy");
    }
  }
  auto resolved = resolve_registration(intent, 0U);
  if (!resolved.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(resolved.error());
  }
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "resolved command participant has no registered subordinate engine",
        "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
  }
  const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
  auto request = build_command_request(command, resolved.value().binding.value());
  if (!request.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(request.error());
  }
  auto preflight = engine->validate_gripper_command(request.value());
  if (!preflight.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(preflight.error());
  }
  const auto status = engine->get_gripper_status(request.value().gripper_id);
  if (status.state == GripperCommandState::kExecuting) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "gripper command preview cannot validate a busy target", "gripper_busy");
  }
  return Result<CapabilityCommandPreviewResultDTO>::Ok(CapabilityCommandPreviewResultDTO{
      resolved.value().binding.value(), std::move(request.value())});
}

const RuntimeParticipantExecutionRegistration* ResourceAddressedExecutionKernel::registration_for(
    const RuntimeParticipantBinding& binding) const noexcept {
  const auto found = std::find_if(registrations_.begin(), registrations_.end(),
                                  [&](const RuntimeParticipantExecutionRegistration& registration) {
                                    return registration.binding.value() == binding.value();
                                  });
  return found == registrations_.end() ? nullptr : &*found;
}

Result<RuntimeParticipantExecutionRegistration>
ResourceAddressedExecutionKernel::resolve_registration(const OperationIntentDTO& operation,
                                                       const std::size_t participant_index) const {
  if (!config_.participant_resolver) {
    auto binding = registry_.resolve(operation, participant_index);
    if (!binding.ok()) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(binding.error());
    }
    const auto* registration = registration_for(binding.value());
    if (registration == nullptr || registration->engine == nullptr) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "resolved participant has no registered subordinate engine",
          "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
    }
    return Result<RuntimeParticipantExecutionRegistration>::Ok(*registration);
  }

  Result<RuntimeParticipantExecutionRegistration> resolved =
      Result<RuntimeParticipantExecutionRegistration>::Err(
          error(std::string(to_string(ErrorCode::kInternalError)),
                "product-backed participant resolver did not return a result",
                "runtime_participant_resolver_failed"));
  try {
    resolved = config_.participant_resolver(operation, participant_index);
  } catch (const std::exception& exception) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("product-backed participant resolution failed: ") + exception.what(),
              "runtime_participant_resolver_failed"));
  } catch (...) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(error(
        std::string(to_string(ErrorCode::kInvariantViolation)),
        "product-backed participant resolution failed", "runtime_participant_resolver_failed"));
  }
  if (!resolved.ok()) {
    return resolved;
  }
  auto validated = RuntimeParticipantRegistry::validate_resolved(operation, participant_index,
                                                                 resolved.value().binding);
  if (!validated.ok()) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(validated.error());
  }
  const auto engine_is_pinned =
      std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& item) {
        return item.engine != nullptr && item.engine == resolved.value().engine;
      });
  if (!engine_is_pinned) {
    return reject<RuntimeParticipantExecutionRegistration>(
        "product-backed participant selected an unpinned subordinate engine",
        "runtime_participant_engine_unpinned", ErrorCode::kInvariantViolation);
  }
  const auto& inventory = registry_.resource_inventory();
  for (const auto& resource : resolved.value().binding.value().resources) {
    if (!std::binary_search(inventory.begin(), inventory.end(), resource.resource_id)) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "product-backed participant widened the active resource inventory",
          "runtime_participant_resource_outside_inventory", ErrorCode::kInvariantViolation);
    }
  }
  return resolved;
}

Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute(
    const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);

  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<OperationAcceptedDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  if (intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
    return reject<OperationAcceptedDTO>("I6 admits one independent participant per operation",
                                        "i6_single_participant_only");
  }

  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<OperationAcceptedDTO>(
          "new motion admission is closed by an active emergency Stop or fault",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto replay =
        std::find_if(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<OperationAcceptedDTO>::Err(
            error("request_id_conflict", "operation request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationAcceptedDTO>::Ok(std::move(accepted));
    }
    const auto control_request = std::find_if(
        control_replay_ledger_.begin(), control_replay_ledger_.end(),
        [&](const ControlReplayEntry& entry) {
          return entry.request.request_context.request_id == intent.request_context.request_id;
        });
    if (control_request != control_replay_ledger_.end()) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by an operation control",
          "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    })) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by another Kernel command",
          "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
  }

  auto resolved = resolve_registration(intent, 0U);
  if (!resolved.ok()) {
    return Result<OperationAcceptedDTO>::Err(resolved.error());
  }
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no registered subordinate engine",
                                        "runtime_participant_engine_missing",
                                        ErrorCode::kInvariantViolation);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto engine_in_use =
        std::any_of(active_operations_.begin(), active_operations_.end(),
                    [&](const auto& item) { return item.second->engine == engine; });
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine already owns an unsettled operation",
          "participant_engine_busy");
    }
  }
  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }

  const auto& binding = resolved.value().binding.value();
  if (!binding.control_path.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no active control path",
                                        "runtime_participant_control_path_missing",
                                        ErrorCode::kMissingReference);
  }
  const auto& control = *binding.control_path;
  if (runtime_route_configuration_publication_required_.contains(control.control_domain_id)) {
    return reject<OperationAcceptedDTO>(
        "runtime route configuration has not been published after reconfiguration",
        "runtime_route_publication_required", ErrorCode::kInvalidStateTransition);
  }
  const auto route_readback =
      runtime_route_configuration_readbacks_.find(control.control_domain_id);
  if (route_readback != runtime_route_configuration_readbacks_.end() &&
      (!route_readback->second.operation_admission_open ||
       route_readback->second.active_configuration.active_control_configuration_id !=
           control.active_control_configuration_id ||
       route_readback->second.active_configuration.active_control_configuration_generation !=
           control.active_control_configuration_generation)) {
    return reject<OperationAcceptedDTO>("runtime route configuration is unreconciled or stale",
                                        "runtime_route_configuration_not_ready",
                                        ErrorCode::kInvalidStateTransition);
  }
  if (!resolved.value().stop_readiness.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no verified Stop readiness",
                                        "runtime_participant_stop_readiness_missing",
                                        ErrorCode::kMissingReference);
  }
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() + "." +
                                        std::to_string(next_operation_id_));
  const auto participant_id = OperationParticipantId(operation_id.value() + ".participant.1");
  const auto controller_goal_id = ControllerGoalId(operation_id.value() + ".controller-goal.1");
  const ControllerGoalGeneration controller_goal_generation(1U);
  const OperationRevision operation_revision(1U);
  const auto payload_kind =
      std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
          ? OperationParticipantPayloadKind::kMotion
          : OperationParticipantPayloadKind::kCapabilityCommand;
  ResourceSlotOwnerDTO slot_owner{operation_id,
                                  operation_revision,
                                  participant_id,
                                  control.control_domain_id,
                                  control.control_domain_revision,
                                  control.control_binding_id,
                                  control.control_binding_revision,
                                  control.controller_topology_id,
                                  control.topology_revision,
                                  control.control_route_id,
                                  control.active_control_configuration_id,
                                  control.active_control_configuration_generation,
                                  control.containment_granularity};
  std::optional<ExecuteRequestDTO> motion_request;
  std::optional<GripperCommandRequestDTO> command_request;
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    const auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
    auto built = build_motion_request(intent, intent.participants.front(), motion, binding,
                                      operation_id, operation_revision, participant_id,
                                      controller_goal_id, controller_goal_generation);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    motion_request = std::move(built.value());
  } else {
    const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    auto built = build_command_request(command, binding);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    command_request = std::move(built.value());
    auto private_validation = engine->validate_gripper_command(*command_request);
    if (!private_validation.ok()) {
      return Result<OperationAcceptedDTO>::Err(private_validation.error());
    }
  }
  const auto accepted_at_ns = clock_.now();
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      {OperationParticipantAssignmentDTO{0U, participant_id}},
                                      accepted_at_ns};
  std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      control.runtime_scale_qualification.has_value()) {
    const auto& qualification = *control.runtime_scale_qualification;
    initial_runtime_scale_status = RuntimeScaleStatusDTO{
        operation_id,
        operation_revision,
        RuntimeScaleGeneration(0U),
        1.0,
        1.0,
        {RuntimeScaleParticipantStatusDTO{participant_id,
                                          binding.controllable_component_id,
                                          binding.command_target_id,
                                          binding.capability_id,
                                          control.control_domain_id,
                                          control.control_domain_revision,
                                          control.control_binding_id,
                                          control.control_binding_revision,
                                          controller_goal_id,
                                          controller_goal_generation,
                                          control.controller_topology_id,
                                          control.topology_revision,
                                          control.control_route_id,
                                          qualification.scaling_domain_id,
                                          qualification.coordinated_barrier_id,
                                          resources,
                                          control.active_control_configuration_id,
                                          control.active_control_configuration_generation,
                                          RuntimeScaleGeneration(0U),
                                          1.0,
                                          1.0,
                                          0.0,
                                          control.provider_runtime_epoch,
                                          0U,
                                          0,
                                          true,
                                          RuntimeScaleApplicationState::kPending,
                                          std::nullopt}},
        RuntimeScaleApplicationState::kPending,
        accepted_at_ns};
  }
  OperationParticipantStateDTO participant_state{
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.controllable_component_id,
      binding.command_target_id,
      binding.capability_id,
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_id)
          : std::nullopt,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_revision)
          : std::nullopt,
      payload_kind,
      OperationStateKind::kResolving,
      0.0,
      resources,
      std::nullopt};
  OperationStateDTO operation_state_dto{operation_id,
                                        operation_revision,
                                        intent.program_binding_id,
                                        intent.program_run_id,
                                        intent.coordination_policy,
                                        OperationStateKind::kResolving,
                                        {std::move(participant_state)},
                                        accepted_at_ns,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
  if (initial_runtime_scale_status.has_value()) {
    operation_state_dto.runtime_scale_generation = RuntimeScaleGeneration(0U);
  }

  PreparedParticipantStopPlanContextDTO stop_context{operation_id,
                                                     operation_revision,
                                                     participant_id,
                                                     intent.program_id,
                                                     intent.program_revision,
                                                     intent.program_binding_id,
                                                     intent.program_run_id,
                                                     std::nullopt,
                                                     ParticipantStopProfilePurposeDTO::kOrdinary,
                                                     std::nullopt,
                                                     resolved.value().stop_node_category};
  auto ordinary_stop_plan = MakePreparedParticipantStopPlan(
      resolved.value().stop_readiness->value().resolved_stop_base_plan, std::move(stop_context),
      binding.resources, resolved.value().retained_default_stop_limits);
  if (!ordinary_stop_plan.ok()) {
    return Result<OperationAcceptedDTO>::Err(ordinary_stop_plan.error());
  }

  std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_stop_plan;
  std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type prepared_operation;
  ReplayEntry prepared_replay{intent, accepted};
  try {
    retained_stop_plan = std::make_shared<const PreparedParticipantStopPlanDTO>(
        std::move(ordinary_stop_plan.value()));
    std::map<OperationId, std::unique_ptr<ActiveOperation>> staging;
    staging.emplace(operation_id, std::make_unique<ActiveOperation>(
                                      ActiveOperation{intent,
                                                      accepted,
                                                      resolved.value().binding,
                                                      *resolved.value().stop_readiness,
                                                      resolved.value().retained_default_stop_limits,
                                                      resolved.value().stop_node_category,
                                                      retained_stop_plan,
                                                      std::nullopt,
                                                      engine,
                                                      resolved.value().stop_execution_port,
                                                      slot_owner,
                                                      resources,
                                                      payload_kind,
                                                      operation_state_dto,
                                                      ResourceSlotState::kReserved,
                                                      0U,
                                                      0U,
                                                      std::nullopt,
                                                      false,
                                                      false,
                                                      false,
                                                      true,
                                                      false,
                                                      false,
                                                      std::nullopt,
                                                      std::move(initial_runtime_scale_status)}));
    prepared_operation = staging.extract(operation_id);
  } catch (...) {
    return reject<OperationAcceptedDTO>(
        "the bounded operation admission state could not be allocated before reservation",
        "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
  }

  {
    // This is the only Execute/Stop ordering section: publish the reserved
    // pending operation before any ordinary journal or provider call. A Stop
    // can now cancel/fence it without waiting for those calls.
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<OperationAcceptedDTO>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    const bool engine_in_use =
        std::any_of(active_operations_.begin(), active_operations_.end(),
                    [&](const auto& item) { return item.second->engine == engine; });
    const bool request_id_used =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(full_stop_replay_ledger_.begin(), full_stop_replay_ledger_.end(),
                    [&](const FullStopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    });
    if (request_id_used) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID was used during admission",
                "request_id_conflict"));
    }
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine became busy during admission",
          "participant_engine_busy");
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    auto reservation =
        config_.operation_admission_gate != nullptr
            ? config_.operation_admission_gate->reserve(*resource_slots_, slot_owner, resources)
            : resource_slots_->reserve(slot_owner, resources);
    if (!reservation.ok()) {
      return Result<OperationAcceptedDTO>::Err(reservation.error());
    }
    const auto inserted = active_operations_.insert(std::move(prepared_operation));
    if (!inserted.inserted) {
      const auto released = resource_slots_->release(slot_owner, resources);
      if (!released.ok()) {
        return Result<OperationAcceptedDTO>::Err(released.error());
      }
      return reject<OperationAcceptedDTO>("operation identity is already resident",
                                          "operation_id_conflict", ErrorCode::kInvariantViolation);
    }
    ++next_operation_id_;
  }

  auto durable_acceptance = journal_.RecordOperationAccepted(config_.request_owner_epoch, intent,
                                                             accepted, operation_state_dto);
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      active_operation->ordinary_acceptance_pending = false;
      if (!active_operation->state.active_stop_id.has_value()) {
        const auto released =
            resource_slots_->release(active_operation->slot_owner, active_operation->resources);
        if (!released.ok()) {
          return Result<OperationAcceptedDTO>::Err(released.error());
        }
        active_operations_.erase(operation_id);
      }
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  bool dispatch_allowed = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation == nullptr) {
      return reject<OperationAcceptedDTO>("operation disappeared after durable acceptance",
                                          "active_operation_lost", ErrorCode::kInvariantViolation);
    }
    active_operation->ordinary_acceptance_pending = false;
    active_operation->ordinary_acceptance_durable = true;
    replay_ledger_.push_back(std::move(prepared_replay));
    dispatch_allowed = !active_operation->state.active_stop_id.has_value() &&
                       active_operation->slot_state == ResourceSlotState::kReserved;
    active_operation->dispatch_effect_in_flight = dispatch_allowed;
  }
  if (!dispatch_allowed) {
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }

  Result<void> dispatch_result = Result<void>::Ok();
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    auto dispatch = engine->execute(*motion_request);
    if (!dispatch.ok()) {
      dispatch_result = Result<void>::Err(dispatch.error());
    } else {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(operation_id);
      if (active_operation == nullptr) {
        dispatch_result = Result<void>::Err(error(
            std::string(to_string(ErrorCode::kInvariantViolation)),
            "active operation disappeared during subordinate admission", "active_operation_lost"));
      } else {
        active_operation->dispatch_effect_in_flight = false;
      }
      if (active_operation != nullptr && active_operation->subordinate_id != 0U &&
          active_operation->subordinate_id != dispatch.value()) {
        dispatch_result =
            Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                    "subordinate event and execute result identities disagree",
                                    "subordinate_execution_id_mismatch"));
      } else if (active_operation != nullptr) {
        active_operation->subordinate_id = dispatch.value();
        if (active_operation->state.state == OperationStateKind::kResolving) {
          auto planning_state = active_operation->state;
          planning_state.state = OperationStateKind::kPlanning;
          planning_state.participants.front().state = OperationStateKind::kPlanning;
          planning_state.updated_at_ns = clock_.now();
          auto durable_state =
              journal_.RecordOperationState(config_.request_owner_epoch, planning_state,
                                            active_operation->runtime_scale_status.has_value()
                                                ? &*active_operation->runtime_scale_status
                                                : nullptr,
                                            "subordinate_motion_admitted");
          if (durable_state.ok()) {
            active_operation->state = std::move(planning_state);
          } else {
            dispatch_result = Result<void>::Err(durable_state.error());
          }
        }
      }
    }
  } else {
    const auto dispatch = engine->command_gripper(*command_request);
    if (!dispatch.accepted || dispatch.error.has_value()) {
      dispatch_result = Result<void>::Err(dispatch.error.value_or(
          error(std::string(to_string(ErrorCode::kExecutionRejected)),
                "subordinate gripper command was rejected", "subordinate_command_rejected")));
    } else {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(operation_id);
      if (active_operation == nullptr) {
        dispatch_result = Result<void>::Err(error(
            std::string(to_string(ErrorCode::kInvariantViolation)),
            "active command disappeared during subordinate admission", "active_operation_lost"));
      } else {
        active_operation->dispatch_effect_in_flight = false;
        active_operation->subordinate_id = dispatch.command_id;
        active_operation->subordinate_generation = dispatch.generation;
        if (!active_operation->state.active_stop_id.has_value()) {
          auto executing_state = active_operation->state;
          executing_state.state = OperationStateKind::kExecuting;
          executing_state.participants.front().state = OperationStateKind::kExecuting;
          executing_state.updated_at_ns = clock_.now();
          auto durable_state =
              journal_.RecordOperationState(config_.request_owner_epoch, executing_state,
                                            active_operation->runtime_scale_status.has_value()
                                                ? &*active_operation->runtime_scale_status
                                                : nullptr,
                                            "subordinate_command_admitted");
          if (durable_state.ok()) {
            auto transition =
                resource_slots_->transition(slot_owner, resources, ResourceSlotState::kDispatched);
            if (!transition.ok()) {
              dispatch_result = transition;
            } else {
              active_operation->slot_state = ResourceSlotState::kDispatched;
              active_operation->state = std::move(executing_state);
            }
          } else {
            dispatch_result = Result<void>::Err(durable_state.error());
          }
        }
      }
    }
  }

  if (!dispatch_result.ok()) {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      active_operation->dispatch_effect_in_flight = false;
      if (active_operation->state.active_stop_id.has_value()) {
        active_operation->last_subordinate_error = dispatch_result.error();
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      auto failed_state = active_operation->state;
      failed_state.state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().terminal_error = dispatch_result.error();
      failed_state.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, failed_state,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "subordinate_dispatch_failed_after_acceptance");
      if (durable_failure.ok()) {
        if (active_operation->slot_state != ResourceSlotState::kTerminal) {
          const auto transition =
              resource_slots_->transition(active_operation->slot_owner, active_operation->resources,
                                          ResourceSlotState::kTerminal);
          if (transition.ok()) {
            active_operation->slot_state = ResourceSlotState::kTerminal;
          }
        }
        active_operation->state = std::move(failed_state);
      }
    }
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::pause(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kPause);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::resume(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kResume);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::reset(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kReset);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::acknowledge_completed(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kAcknowledgeCompleted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::apply_control(
    const OperationControlRequestDTO& request, OperationControlKind control) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() ||
      (request.reason.has_value() && request.reason->empty()) ||
      !request.attribution.locally_valid()) {
    return reject<OperationControlAcceptedDTO>("operation control request is malformed",
                                               "operation_control_request_invalid",
                                               ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation control names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::uint64_t subordinate_id = 0U;
  std::uint64_t subordinate_generation = 0U;
  std::string command_target;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationControlAcceptedDTO accepted{RequestId("pending"),
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       OperationId("pending"),
                                       OperationRevision(0U),
                                       control,
                                       OperationRevision(0U),
                                       0};
  const bool settles_operation = control == OperationControlKind::kReset ||
                                 control == OperationControlKind::kAcknowledgeCompleted;
  {
    std::lock_guard state_lock(state_mutex_);
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto replay = std::find_if(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                                     [&](const ControlReplayEntry& entry) {
                                       return entry.request.request_context.request_id ==
                                              request.request_context.request_id;
                                     });
    if (replay != control_replay_ledger_.end()) {
      if (replay->request != request || replay->accepted.control != control) {
        return Result<OperationControlAcceptedDTO>::Err(error(
            "request_id_conflict", "operation control request ID was reused with different intent",
            "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationControlAcceptedDTO>::Ok(std::move(accepted));
    }
    if (std::any_of(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == request.request_context.request_id;
        })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID was already used by Execute",
                "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             request.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             request.request_context.request_id;
                    })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "operation control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<OperationControlAcceptedDTO>("operation control names a non-resident operation",
                                                 "operation_not_resident",
                                                 ErrorCode::kMissingReference);
    }
    const auto current_revision = active_operation->state.operation_revision;
    if (current_revision != request.expected_operation_revision) {
      return reject<OperationControlAcceptedDTO>("operation control expected revision is stale",
                                                 "operation_revision_stale");
    }
    if (!control_admissible(control, active_operation->state.state)) {
      return reject<OperationControlAcceptedDTO>(
          std::string(control_name(control)) + " is invalid for the current operation state",
          "operation_control_state_invalid", ErrorCode::kInvalidStateTransition);
    }
    if (active_operation->pending_control.has_value()) {
      return reject<OperationControlAcceptedDTO>(
          "another operation control is already being admitted", "operation_control_busy");
    }
    if (settles_operation && (active_operation->reconciliation_in_flight ||
                              active_operation->runtime_scale_effect_in_flight ||
                              active_operation->stop_effect_in_flight)) {
      return reject<OperationControlAcceptedDTO>(
          "operation settlement cannot overtake an in-flight subordinate effect",
          "operation_effect_in_flight");
    }
    engine = active_operation->engine;
    subordinate_id = active_operation->subordinate_id;
    subordinate_generation = active_operation->subordinate_generation;
    command_target = active_operation->binding.value().command_target_id.value();
    payload_kind = active_operation->payload_kind;
    if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
        (control == OperationControlKind::kPause || control == OperationControlKind::kResume)) {
      return reject<OperationControlAcceptedDTO>(
          "the qualified I6 command participant has no pause or resume realization",
          control == OperationControlKind::kPause ? "command_pause_unsupported"
                                                  : "command_resume_unsupported",
          ErrorCode::kUnsupportedCapability);
    }
    if (current_revision.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<OperationControlAcceptedDTO>("operation revision space is exhausted",
                                                 "operation_revision_exhausted",
                                                 ErrorCode::kInternalError);
    }
    const OperationRevision next_revision(current_revision.value() + 1U);
    const auto accepted_at_ns = clock_.now();
    accepted = OperationControlAcceptedDTO{request.request_context.request_id,
                                           RequestReplayStatusDTO::kAcceptedNew,
                                           request.operation_id,
                                           current_revision,
                                           control,
                                           next_revision,
                                           accepted_at_ns};
    auto accepted_state = active_operation->state;
    accepted_state.operation_revision = next_revision;
    accepted_state.updated_at_ns = accepted_at_ns;
    auto durable_acceptance = journal_.RecordControlAccepted(config_.request_owner_epoch, request,
                                                             accepted, accepted_state);
    if (!durable_acceptance.ok()) {
      return Result<OperationControlAcceptedDTO>::Err(durable_acceptance.error());
    }

    auto revised = settles_operation && active_operation->slot_state != ResourceSlotState::kTerminal
                       ? resource_slots_->transition_and_revise_owner(
                             active_operation->slot_owner, active_operation->resources,
                             ResourceSlotState::kTerminal, next_revision)
                       : resource_slots_->revise_owner(active_operation->slot_owner,
                                                       active_operation->resources, next_revision);
    control_replay_ledger_.push_back(ControlReplayEntry{request, accepted});
    if (!revised.ok()) {
      auto intervention = accepted_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = revised.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention, nullptr,
                                        "resource_revision_failed_after_control_acceptance");
      if (durable_failure.ok()) {
        active_operation->state = std::move(intervention);
      }
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    active_operation->slot_owner = revised.value();
    if (settles_operation) {
      active_operation->slot_state = ResourceSlotState::kTerminal;
    }
    active_operation->state = std::move(accepted_state);
    if (active_operation->runtime_scale_status.has_value()) {
      active_operation->runtime_scale_status->operation_revision = next_revision;
      active_operation->runtime_scale_status->updated_at_ns = accepted_at_ns;
    }
    active_operation->pending_control = control;
  }

  bool accepted_by_subordinate = false;
  std::optional<ErrorDTO> subordinate_error;
  switch (control) {
    case OperationControlKind::kPause: {
      const auto result = engine->pause();
      accepted_by_subordinate = result.accepted;
      subordinate_error = result.error;
      break;
    }
    case OperationControlKind::kResume: {
      const auto result = engine->resume(std::nullopt);
      accepted_by_subordinate = result.accepted;
      subordinate_error = result.error;
      break;
    }
    case OperationControlKind::kReset: {
      if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
        const auto result =
            engine->reset_gripper(command_target, subordinate_id, subordinate_generation);
        accepted_by_subordinate = result.ok();
        if (!result.ok()) {
          subordinate_error = result.error();
        }
      } else {
        const auto result = engine->reset(std::nullopt);
        accepted_by_subordinate = result.accepted;
        subordinate_error = result.error;
      }
      break;
    }
    case OperationControlKind::kAcknowledgeCompleted:
      if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
        accepted_by_subordinate = true;
      } else {
        const auto result = engine->acknowledge_completed(subordinate_id, std::nullopt);
        accepted_by_subordinate = result.accepted;
        subordinate_error = result.error;
      }
      break;
  }
  if (!accepted_by_subordinate) {
    const auto failure = subordinate_error.value_or(
        error(std::string(to_string(ErrorCode::kExecutionRejected)),
              std::string("subordinate rejected accepted ") + control_name(control),
              "subordinate_control_rejected"));
    (void)apply_subordinate_state(request.operation_id, engine, subordinate_id,
                                  OperationStateKind::kInterventionRequired, 0.0, failure);
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr && active_operation->pending_control == control) {
      active_operation->pending_control.reset();
    }
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  std::lock_guard state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(request.operation_id);
  if (active_operation == nullptr) {
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }
  active_operation->pending_control.reset();
  if (settles_operation) {
    auto release =
        resource_slots_->release(active_operation->slot_owner, active_operation->resources);
    if (!release.ok()) {
      auto intervention = active_operation->state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = release.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "resource_release_failed_after_control_effect");
      if (durable_failure.ok()) {
        active_operation->state = std::move(intervention);
      }
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    historical_operations_.push_back(active_operation->state);
    active_operations_.erase(request.operation_id);
  }
  return Result<OperationControlAcceptedDTO>::Ok(accepted);
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::stop(const StopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::full_stop(
    const FullStopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

void ResourceAddressedExecutionKernel::ScheduleStopSurveillance(
    StopCertificateId certificate_id, const std::int64_t deadline_ns) noexcept {
  if (!config_.stop_surveillance.has_value()) {
    return;
  }
  std::uint64_t generation = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_shutting_down_) {
      return;
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
        (*found)->surveillance_state->projection.state !=
            StopPostTerminalSurveillanceStateDTO::kMonitoring ||
        (*found)->surveillance_callback_pending ||
        (*found)->surveillance_schedule_generation == std::numeric_limits<std::uint64_t>::max()) {
      return;
    }
    generation = ++(*found)->surveillance_schedule_generation;
    (*found)->surveillance_callback_pending = true;
    ++stop_surveillance_schedule_publications_;
  }

  std::optional<ScheduleHandle> handle;
  try {
    handle = config_.stop_surveillance->scheduler->schedule(
        deadline_ns,
        [this, certificate_id, generation] { RunStopSurveillance(certificate_id, generation); },
        SchedulerPriority::Monitoring);
  } catch (...) {
    // The publication boundary itself is infrastructure, not source truth.
  }

  bool cancel_late_handle = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_schedule_publications_ == 0U) {
      std::terminate();
    }
    --stop_surveillance_schedule_publications_;
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (!handle.has_value() || found == stop_terminal_history_.end() ||
        (*found)->surveillance_schedule_generation != generation ||
        !(*found)->surveillance_callback_pending || stop_surveillance_shutting_down_) {
      cancel_late_handle = handle.has_value();
      if (found != stop_terminal_history_.end() &&
          (*found)->surveillance_schedule_generation == generation) {
        (*found)->surveillance_callback_pending = false;
        (*found)->surveillance_schedule_handle.reset();
      }
      if (!handle.has_value()) {
        motion_admission_closed_ = true;
      }
    } else {
      (*found)->surveillance_schedule_handle = *handle;
    }
    stop_surveillance_schedule_cv_.notify_all();
  }
  if (cancel_late_handle) {
    try {
      config_.stop_surveillance->scheduler->cancel(*handle);
    } catch (...) {
      std::terminate();
    }
  }
}

void ResourceAddressedExecutionKernel::RunStopSurveillance(
    StopCertificateId certificate_id, const std::uint64_t schedule_generation) noexcept {
  try {
    std::int64_t next_deadline_ns = 0;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() ||
          (*found)->surveillance_schedule_generation != schedule_generation ||
          !(*found)->surveillance_callback_pending) {
        return;
      }
      (*found)->surveillance_callback_pending = false;
      (*found)->surveillance_schedule_handle.reset();
      if (stop_surveillance_shutting_down_ || (*found)->surveillance_state == nullptr ||
          (*found)->surveillance_state->projection.state !=
              StopPostTerminalSurveillanceStateDTO::kMonitoring) {
        return;
      }
    }

    const auto now = clock_.now();
    if (now < 0) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }
    std::size_t source_count = 0U;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      source_count = (*found)->surveillance_state->source_plans.size();
    }
    for (std::size_t source_index = 0U; source_index < source_count; ++source_index) {
      std::optional<StopSurveillanceObservationQueryDTO> query;
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto made =
            MakeStopSurveillanceObservationQuery(*(*found)->surveillance_state, source_index, now);
        if (!made.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        query = std::move(made.value());
      }
      auto sample = config_.stop_surveillance->observer->Observe(*query);
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto reduced =
            ReduceStopSurveillanceObservation(*(*found)->surveillance_state, *query, sample);
        if (!reduced.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        *(*found)->surveillance_state = std::move(reduced.value());
        (*found)->status.surveillance = (*found)->surveillance_state->projection;
        if ((*found)->fault_latch.has_value()) {
          (*found)->status.fault_latch = (*found)->fault_latch;
        }
      }
    }

    std::optional<StopControlWorkToken> checkpoint_token;
    bool reschedule = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      auto& record = **found;
      const auto& policy = record.surveillance_state->projection.policy;
      if (now >= record.next_surveillance_checkpoint_ns && record.fault_latch.has_value() &&
          !record.surveillance_projection_token.has_value()) {
        auto checkpoint = MakeStopSurveillanceCheckpoint(*record.surveillance_state);
        if (checkpoint.ok()) {
          checkpoint_token = allocate_stop_projection_job_locked();
          if (checkpoint_token.has_value()) {
            auto& projection = *stop_projection_jobs_[checkpoint_token->slot];
            projection.surveillance_checkpoint = std::move(checkpoint.value());
            projection.surveillance_fault_latch = *record.fault_latch;
            record.surveillance_projection_token = checkpoint_token;
            if (now <= std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
              record.next_surveillance_checkpoint_ns = now + policy.checkpoint_interval_ns;
            }
          } else {
            motion_admission_closed_ = true;
          }
        }
      }
      reschedule = record.surveillance_state->projection.state ==
                   StopPostTerminalSurveillanceStateDTO::kMonitoring;
      if (reschedule) {
        if (now > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns) {
          motion_admission_closed_ = true;
          reschedule = false;
        } else {
          const auto elapsed = std::max<std::int64_t>(0, now - record.next_surveillance_sample_ns);
          const auto skipped = elapsed / policy.sample_cadence_ns + 1;
          if (skipped >
              (std::numeric_limits<std::int64_t>::max() - record.next_surveillance_sample_ns) /
                  policy.sample_cadence_ns) {
            motion_admission_closed_ = true;
            reschedule = false;
          } else {
            record.next_surveillance_sample_ns += skipped * policy.sample_cadence_ns;
            next_deadline_ns = record.next_surveillance_sample_ns;
          }
        }
      }
    }
    if (checkpoint_token.has_value()) {
      stop_projection_worker_->Publish(*checkpoint_token);
    }
    if (reschedule) {
      ScheduleStopSurveillance(std::move(certificate_id), next_deadline_ns);
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

struct ResourceAddressedExecutionKernel::DisableRuntimeExpectation final {
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeRequestKindDTO request_kind{
      DisableHandoffScopeRequestKindDTO::kFullStopRequired};
  std::optional<RetainedBundleId> retained_bundle_id;
  std::optional<GraphRevision> graph_revision;
  std::optional<ComponentLifecycleGeneration> component_lifecycle_generation;
  std::optional<ConfigurationFenceScopeDTO> component_scope;
  std::optional<std::vector<CurrentDomainActiveControlConfigurationDTO>>
      active_control_configurations;
};

Result<KernelDisableHandoffRuntimeSnapshotDTO>
ResourceAddressedExecutionKernel::capture_disable_handoff_runtime_snapshot_impl(
    const DisableRuntimeExpectation& expectation) const {
  try {
    std::lock_guard state_lock(state_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (!stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value()) {
      return reject<KernelDisableHandoffRuntimeSnapshotDTO>(
          "Kernel Disable admission has no current runtime identity",
          "disable_handoff_runtime_identity_unavailable", ErrorCode::kInvariantViolation);
    }

    const auto full_scope = derive_disable_handoff_scope(registrations_, std::nullopt);
    if (full_scope.controllable_component_ids.empty()) {
      return reject<KernelDisableHandoffRuntimeSnapshotDTO>(
          "Kernel Disable admission has no current controllable runtime scope",
          "disable_handoff_runtime_scope_unavailable", ErrorCode::kMissingReference);
    }

    auto resolution = DisableHandoffScopeResolutionDTO::kPromotedToFull;
    auto current_scope = full_scope;
    std::optional<ComponentLifecycleGeneration> current_component_generation;
    if (expectation.request_kind == DisableHandoffScopeRequestKindDTO::kResolvedComponentScope) {
      const auto component_scope =
          derive_disable_handoff_scope(registrations_, expectation.controllable_component_id);
      const auto component = std::lower_bound(
          stop_component_scope_.begin(), stop_component_scope_.end(),
          expectation.controllable_component_id,
          [](const StopScopeComponentSnapshotDTO& row, const ControllableComponentId& id) {
            return row.controllable_component_id < id;
          });
      if (!component_scope.controllable_component_ids.empty() &&
          component != stop_component_scope_.end() &&
          component->controllable_component_id == expectation.controllable_component_id) {
        current_scope = component_scope;
        current_component_generation = component->component_lifecycle_generation;
        const auto observation = resource_slots_->admission_snapshot(current_scope.resources);
        const auto current_configurations = select_disable_active_configurations(
            current_scope, observation.active_control_configurations);
        resolution =
            expectation.retained_bundle_id == stop_retained_bundle_id_ &&
                    expectation.graph_revision == stop_graph_revision_ &&
                    expectation.component_lifecycle_generation == current_component_generation &&
                    expectation.component_scope == current_scope &&
                    expectation.active_control_configurations == current_configurations
                ? DisableHandoffScopeResolutionDTO::kExact
                : DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved;
      }
    }

    const auto observation = resource_slots_->admission_snapshot(current_scope.resources);
    const auto slot_snapshot = resource_slots_->coherent_snapshot();
    std::set<OperationId> queued_operations;
    std::set<OperationId> active_operations;
    bool orphan_present = false;
    for (const auto& slot : slot_snapshot.slots) {
      if (!std::binary_search(current_scope.resources.begin(), current_scope.resources.end(),
                              slot.resource_id)) {
        continue;
      }
      if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
        orphan_present = true;
        continue;
      }
      if (!slot.owner.has_value()) {
        continue;
      }
      const auto* operation = find_active_operation_locked(slot.owner->operation_id);
      if (operation == nullptr || terminal(operation->state.state)) {
        continue;
      }
      if (slot.state == ResourceSlotState::kReserved || operation->ordinary_acceptance_pending) {
        queued_operations.insert(slot.owner->operation_id);
      } else {
        active_operations.insert(slot.owner->operation_id);
      }
    }
    KernelDisableHandoffRuntimeSnapshotDTO snapshot{
        expectation.controllable_component_id,
        expectation.request_kind,
        config_.request_owner_epoch,
        resolution,
        std::move(current_scope),
        *stop_retained_bundle_id_,
        *stop_graph_revision_,
        observation.operation_resource_generation,
        {},
        current_component_generation,
        cell_stop_generation_,
        std::vector<OperationId>(queued_operations.begin(), queued_operations.end()),
        static_cast<std::uint64_t>(active_operations.size()) + (orphan_present ? 1U : 0U)};
    snapshot.current_active_control_configurations = select_disable_active_configurations(
        snapshot.current_scope, observation.active_control_configurations);
    return Result<KernelDisableHandoffRuntimeSnapshotDTO>::Ok(std::move(snapshot));
  } catch (const std::exception& exception) {
    return reject<KernelDisableHandoffRuntimeSnapshotDTO>(
        std::string("Kernel Disable runtime snapshot failed: ") + exception.what(),
        "disable_handoff_runtime_snapshot_failed", ErrorCode::kInvariantViolation);
  } catch (...) {
    return reject<KernelDisableHandoffRuntimeSnapshotDTO>("Kernel Disable runtime snapshot failed",
                                                          "disable_handoff_runtime_snapshot_failed",
                                                          ErrorCode::kInvariantViolation);
  }
}

Result<KernelDisableHandoffRuntimeSnapshotDTO>
ResourceAddressedExecutionKernel::capture_disable_handoff_runtime_snapshot(
    const DisableHandoffAdmissionQueryDTO& query) const {
  if (!query.locally_valid()) {
    return reject<KernelDisableHandoffRuntimeSnapshotDTO>(
        "Kernel Disable admission query is malformed", "disable_handoff_query_invalid",
        ErrorCode::kInvalidRequest);
  }
  DisableRuntimeExpectation expectation{query.controllable_component_id,
                                        query.request_kind,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
  if (query.component_expectation.has_value()) {
    expectation.retained_bundle_id = query.component_expectation->retained_bundle_id;
    expectation.graph_revision = query.component_expectation->graph_revision;
    expectation.component_lifecycle_generation =
        query.component_expectation->component_lifecycle_generation;
    expectation.component_scope = query.component_expectation->component_scope;
    expectation.active_control_configurations =
        query.component_expectation->current_active_control_configurations;
  }
  return capture_disable_handoff_runtime_snapshot_impl(expectation);
}

Result<KernelDisableHandoffRuntimeSnapshotDTO>
ResourceAddressedExecutionKernel::revalidate_disable_handoff_request(
    const DisableHandoffRequestDTO& request) const {
  if (!request.locally_valid()) {
    return reject<KernelDisableHandoffRuntimeSnapshotDTO>(
        "Kernel Disable handoff request is malformed", "disable_handoff_request_invalid",
        ErrorCode::kInvalidRequest);
  }
  DisableRuntimeExpectation expectation{
      request.controllable_component_id,
      request.scope_resolution == DisableHandoffScopeResolutionDTO::kPromotedToFull
          ? DisableHandoffScopeRequestKindDTO::kFullStopRequired
          : DisableHandoffScopeRequestKindDTO::kResolvedComponentScope,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  if (expectation.request_kind == DisableHandoffScopeRequestKindDTO::kResolvedComponentScope) {
    expectation.retained_bundle_id = request.expected_retained_bundle_id;
    expectation.graph_revision = request.expected_graph_revision;
    expectation.component_lifecycle_generation = request.expected_component_lifecycle_generation;
    expectation.component_scope = request.affected_scope;
    expectation.active_control_configurations = request.expected_active_control_configurations;
  }
  return capture_disable_handoff_runtime_snapshot_impl(expectation);
}

Result<StopStatusDTO> ResourceAddressedExecutionKernel::get_stop_status(
    const StopId& stop_id) const {
  if (stop_id.empty()) {
    return reject<StopStatusDTO>("Stop status identity is empty", "stop_id_invalid",
                                 ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_id == stop_id;
      });
  if (terminal != stop_terminal_history_.end()) {
    return Result<StopStatusDTO>::Ok((*terminal)->status);
  }
  const auto active = std::find_if(
      stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& transaction) {
        return transaction != nullptr && transaction->active && transaction->stop_id == stop_id &&
               transaction->accepted.has_value();
      });
  if (active == stop_transactions_.end()) {
    return reject<StopStatusDTO>("Stop status is not resident", "stop_status_not_resident",
                                 ErrorCode::kMissingReference);
  }
  const auto& transaction = **active;
  std::vector<StopScopeEntryDTO> entries;
  std::vector<StopAttemptedStageResultDTO> attempts;
  entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
  for (const auto& participant : transaction.participants) {
    entries.push_back(participant_scope_entry(
        participant, *transaction.stop_id, transaction.stop_generation,
        transaction.accepted->accepted_at_ns, transaction.accepted->remaining_outer_duration_ns));
    attempts.insert(attempts.end(), participant.retained_results.begin(),
                    participant.retained_results.end());
  }
  for (const auto& resource : transaction.newly_captured_resources) {
    entries.push_back(StopScopeEntryDTO{
        StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
        resource,
        StopScopeEntryDispositionDTO::kNotStoppable,
        StopScopeDispatchStateDTO::kEffectUncertain,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        transaction.accepted->accepted_at_ns,
        transaction.accepted->remaining_outer_duration_ns,
        {}});
  }
  const auto projection = [&] {
    switch (transaction.acceptance_projection_state) {
      case EmergencyStopProjectionStateDTO::kPending:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                              std::nullopt};
      case EmergencyStopProjectionStateDTO::kRecorded:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                              std::nullopt, std::nullopt};
      case EmergencyStopProjectionStateDTO::kFailed:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                              ErrorDTO{"operation_journal_stop_projection_failed",
                                                       "Stop acceptance forensic projection failed",
                                                       {}},
                                              1U};
    }
    return StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kFailed,
        ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
  }();
  StopStatusDTO status{*transaction.accepted,
                       transaction.fresh_scope_complete ? StopStatusStateDTO::kCertifying
                                                        : StopStatusStateDTO::kStopping,
                       std::move(entries),
                       {},
                       std::move(attempts),
                       {},
                       {},
                       {},
                       projection,
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                                      std::nullopt, std::nullopt},
                       std::nullopt,
                       std::nullopt,
                       std::nullopt};
  return Result<StopStatusDTO>::Ok(std::move(status));
}

Result<StopCertificateDTO> ResourceAddressedExecutionKernel::get_stop_certificate(
    const StopCertificateId& certificate_id) const {
  if (certificate_id.empty()) {
    return reject<StopCertificateDTO>("Stop certificate identity is empty",
                                      "stop_certificate_id_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
      });
  if (found == stop_terminal_history_.end()) {
    return reject<StopCertificateDTO>("Stop certificate is not resident",
                                      "stop_certificate_not_resident",
                                      ErrorCode::kMissingReference);
  }
  return Result<StopCertificateDTO>::Ok((*found)->certificate);
}

std::optional<StopFaultLatchDTO> ResourceAddressedExecutionKernel::get_stop_fault_latch() const {
  std::lock_guard state_lock(state_mutex_);
  return active_stop_fault_latch_;
}

Result<StopFaultRecoveryResultDTO> ResourceAddressedExecutionKernel::recover_stop_fault(
    const StopFaultRecoveryIntentDTO& intent) {
  if (stop_fault_recovery_intent_field_error(intent).has_value()) {
    return reject<StopFaultRecoveryResultDTO>("Stop fault-recovery intent is invalid",
                                              "stop_fault_recovery_intent_invalid",
                                              ErrorCode::kInvalidRequest);
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<StopFaultRecoveryResultDTO>::Err(
        error("stale_request_owner_epoch", "Stop fault recovery names a stale owner epoch",
              "stale_request_owner_epoch"));
  }

  // Recovery is an ordinary, durable control path. Serialize it independently
  // from emergency Stop so two clear attempts cannot both evaluate one latch
  // and race their post-commit publication.
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);

  std::optional<StopStatusDTO> status;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        stop_fault_recovery_replay_ledger_.begin(), stop_fault_recovery_replay_ledger_.end(),
        [&](const auto& row) {
          return row.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != stop_fault_recovery_replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<StopFaultRecoveryResultDTO>::Err(
            error("request_id_conflict", "Stop fault-recovery request ID conflicts",
                  "request_id_conflict"));
      }
      auto result = replay->result;
      result.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<StopFaultRecoveryResultDTO>::Ok(std::move(result));
    }
    const auto request_id = intent.request_context.request_id;
    const bool request_id_used_elsewhere =
        emergency_stop_reserve_.find(request_id.value()).has_value() ||
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& row) {
                      return row.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    });
    if (request_id_used_elsewhere) {
      return Result<StopFaultRecoveryResultDTO>::Err(
          error("request_id_conflict", "Stop fault-recovery request ID is already retained",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() + stop_fault_recovery_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<StopFaultRecoveryResultDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    if (!active_stop_fault_latch_.has_value() ||
        active_stop_fault_latch_->latch_id != intent.expected_latch_id ||
        active_stop_fault_latch_->latch_generation != intent.expected_latch_generation ||
        !active_stop_fault_latch_->stop_certificate.has_value()) {
      return reject<StopFaultRecoveryResultDTO>("Stop fault latch is not current",
                                                "stop_fault_recovery_latch_stale",
                                                ErrorCode::kInvalidStateTransition);
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch.has_value() &&
                 record->fault_latch->latch_id == intent.expected_latch_id &&
                 record->fault_latch->latch_generation == intent.expected_latch_generation;
        });
    if (found == stop_terminal_history_.end()) {
      return reject<StopFaultRecoveryResultDTO>("Stop terminal truth is not resident",
                                                "stop_fault_recovery_terminal_missing",
                                                ErrorCode::kMissingReference);
    }
    status = (*found)->status;
  }
  if (!config_.stop_fault_recovery_facts_resolver) {
    return reject<StopFaultRecoveryResultDTO>("Stop recovery facts owner is not installed",
                                              "stop_fault_recovery_facts_unavailable",
                                              ErrorCode::kMissingReference);
  }
  auto facts = config_.stop_fault_recovery_facts_resolver(*status, clock_.now());
  if (!facts.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(facts.error());
  }
  auto evaluated = EvaluateStopFaultRecovery(intent, *status, facts.value());
  if (!evaluated.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(evaluated.error());
  }
  if (!evaluated.value().result.accepted) {
    std::lock_guard state_lock(state_mutex_);
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch.has_value() &&
                 record->fault_latch->latch_id == intent.expected_latch_id &&
                 record->fault_latch->latch_generation == intent.expected_latch_generation;
        });
    if (found == stop_terminal_history_.end() || !active_stop_fault_latch_.has_value() ||
        active_stop_fault_latch_->latch_id != intent.expected_latch_id ||
        active_stop_fault_latch_->latch_generation != intent.expected_latch_generation) {
      motion_admission_closed_ = true;
      return reject<StopFaultRecoveryResultDTO>("Stop fault truth changed during assessment",
                                                "stop_fault_recovery_assessment_conflict",
                                                ErrorCode::kInvariantViolation);
    }
    // A current-facts assessment is owner truth even when the caller's
    // revision is stale. Publish the newest non-cleared latch so the next
    // exact request can name it; this performs no clear and no external
    // effect.
    (*found)->fault_latch = evaluated.value().result.latch;
    (*found)->status.fault_latch = evaluated.value().result.latch;
    active_stop_fault_latch_ = evaluated.value().result.latch;
    stop_fault_recovery_replay_ledger_.push_back(
        StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
    return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
  }
  const auto persisted = journal_.RecordStopFaultCleared(
      config_.request_owner_epoch, intent, evaluated.value().result, evaluated.value().result.latch,
      *evaluated.value().resulting_stop_status);
  if (!persisted.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(persisted.error());
  }
  std::lock_guard state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->fault_latch.has_value() &&
               record->fault_latch->latch_id == intent.expected_latch_id &&
               record->fault_latch->latch_generation == intent.expected_latch_generation;
      });
  if (found == stop_terminal_history_.end() || !active_stop_fault_latch_.has_value() ||
      active_stop_fault_latch_->latch_id != intent.expected_latch_id ||
      active_stop_fault_latch_->latch_generation != intent.expected_latch_generation) {
    motion_admission_closed_ = true;
    return reject<StopFaultRecoveryResultDTO>("Stop fault truth changed after durable clear",
                                              "stop_fault_recovery_postcommit_conflict",
                                              ErrorCode::kInvariantViolation);
  }
  (*found)->fault_latch = evaluated.value().result.latch;
  (*found)->status = *evaluated.value().resulting_stop_status;
  (*found)->surveillance_state.reset();
  active_stop_fault_latch_ = evaluated.value().result.latch;
  stop_fault_recovery_replay_ledger_.push_back(
      StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
  motion_admission_closed_ = motion_admission_unreconciled_;
  return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::accept_stop(
    EmergencyStopProjectedIntentDTO projected_intent) {
  struct PostReserveInvariantGuard final {
    bool armed{};
    ~PostReserveInvariantGuard() {
      if (armed) {
        // Once the emergency reserve has accepted a recognized Stop, returning
        // a normal rejection or unwinding would lose its already-linearized
        // containment authority. Every remaining failure is an owner bug and
        // terminates fail-closed rather than pretending the Stop was rejected.
        std::terminate();
      }
    }
  } post_reserve_guard;
  const auto* ordinary = std::get_if<StopIntentDTO>(&projected_intent);
  const auto* full = std::get_if<FullStopIntentDTO>(&projected_intent);
  if (ordinary == nullptr && full == nullptr) {
    return reject<StopAcceptedDTO>("Stop ingress has no normalized request identity",
                                   "stop_ingress_not_normalized", ErrorCode::kInvalidRequest);
  }
  const auto& context = ordinary != nullptr ? ordinary->request_context : full->request_context;
  const auto& attribution = ordinary != nullptr ? ordinary->attribution : full->attribution;
  if (!attribution.locally_valid()) {
    return reject<StopAcceptedDTO>("Stop ingress attribution is invalid",
                                   "stop_request_attribution_invalid", ErrorCode::kInvalidRequest);
  }
  const auto normalized_digest = ordinary != nullptr ? ComputeNormalizedStopIntentDigest(*ordinary)
                                                     : ComputeNormalizedStopIntentDigest(*full);
  if (!normalized_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(normalized_digest.error());
  }
  const auto requested_selector_digest = ordinary != nullptr
                                             ? ComputeRequestedStopSelectorDigest(*ordinary)
                                             : ComputeRequestedStopSelectorDigest(*full);
  if (!requested_selector_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(requested_selector_digest.error());
  }

  const auto accepted_at_ns = clock_.now();
  std::int64_t outer_deadline_ns{};

  EmergencyStopAcceptanceOutcome outcome;
  std::optional<StopAcceptedDTO> accepted;
  StopControlWorkToken work_token;
  std::optional<StopControlWorkToken> projection_token;
  bool handoff_work = false;
  {
    std::lock_guard state_lock(state_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (!cell_stop_deadline_budget_.has_value()) {
      return reject<StopAcceptedDTO>("Stop deadline referent is unavailable",
                                     "stop_deadline_budget_unavailable",
                                     ErrorCode::kInvariantViolation);
    }
    outer_deadline_ns =
        checked_deadline(accepted_at_ns, cell_stop_deadline_budget_->outer_duration_ns);
    if (outer_deadline_ns < 0) {
      return reject<StopAcceptedDTO>("Stop deadline cannot be represented monotonically",
                                     "stop_deadline_overflow", ErrorCode::kInternalError);
    }
    const auto proposed_number = next_stop_id_;
    if (proposed_number == 0U || proposed_number > kMaximumKernelJsonInteger ||
        cell_stop_generation_.value() >= kMaximumKernelJsonInteger) {
      return reject<StopAcceptedDTO>("Stop identity or generation space is exhausted",
                                     "stop_counter_exhausted", ErrorCode::kInternalError);
    }
    const auto proposed_stop_id = StopId("stop." + config_.request_owner_epoch.value() + "." +
                                         std::to_string(proposed_number));
    const auto proposed_generation = StopGeneration(cell_stop_generation_.value() + 1U);
    const auto slot_snapshot = resource_slots_->coherent_snapshot();
    auto resolver_snapshot = capture_stop_scope_locked(slot_snapshot);
    auto resolution = ordinary != nullptr ? resolve_stop_scope(*ordinary, resolver_snapshot)
                                          : resolve_full_stop_scope(*full, resolver_snapshot);
    if (!resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(resolution.error());
    }
    auto full_resolution =
        ordinary != nullptr
            ? resolve_full_stop_scope(
                  FullStopIntentDTO{context, cell_stop_generation_, attribution}, resolver_snapshot)
            : resolution;
    if (!full_resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(full_resolution.error());
    }
    const bool request_id_used_by_ordinary_command =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == context.request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id == context.request_id;
                    });
    if (request_id_used_by_ordinary_command) {
      auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
      resolution = full_resolution;
      resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
      resolution.value().reason = StopScopeResolutionReasonDTO::kRequestIdentityConflict;
      resolution.value().stale_diagnostics = std::move(retained_diagnostics);
    }
    const bool request_id_retained_by_stop =
        emergency_stop_reserve_.find(context.request_id.value()).has_value();
    auto captured_digest = ComputeCapturedStopScopeDigest(resolution.value());
    auto full_digest = ComputeCapturedStopScopeDigest(full_resolution.value());
    if (!captured_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(captured_digest.error());
    }
    if (!full_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(full_digest.error());
    }

    const auto transaction_intersects = [](const StopTransaction& transaction,
                                           const StopScopeResolutionDTO& scope) {
      const bool operation_overlap =
          transaction.accepted.has_value() &&
          std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                      transaction.accepted->affected_operation_revisions.end(),
                      [&](const auto& affected) {
                        return std::binary_search(scope.operations.begin(), scope.operations.end(),
                                                  affected.operation_id);
                      });
      const bool resource_overlap = std::any_of(
          transaction.resources.begin(), transaction.resources.end(), [&](const auto& resource) {
            return std::binary_search(scope.resources.begin(), scope.resources.end(), resource);
          });
      return operation_overlap || resource_overlap;
    };
    const auto transaction_covers = [](const StopTransaction& transaction,
                                       const StopScopeResolutionDTO& scope) {
      if (!transaction.accepted.has_value()) {
        return false;
      }
      return std::all_of(scope.operations.begin(), scope.operations.end(),
                         [&](const auto& operation_id) {
                           const auto found = std::lower_bound(
                               transaction.accepted->affected_operation_revisions.begin(),
                               transaction.accepted->affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
                           return found !=
                                      transaction.accepted->affected_operation_revisions.end() &&
                                  found->operation_id == operation_id;
                         }) &&
             std::all_of(scope.resources.begin(), scope.resources.end(), [&](const auto& resource) {
               return std::binary_search(transaction.resources.begin(), transaction.resources.end(),
                                         resource);
             });
    };
    const auto resident_transactions = [&](const StopScopeResolutionDTO& scope) {
      std::vector<StopTransaction*> result;
      result.reserve(stop_transactions_.size());
      for (const auto& candidate : stop_transactions_) {
        if (candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
            candidate->accepted.has_value() && transaction_intersects(*candidate, scope)) {
          result.push_back(candidate.get());
        }
      }
      std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return *left->stop_id < *right->stop_id;
      });
      return result;
    };
    const auto transaction_matches_selector = [&](const StopTransaction& transaction) {
      if (ordinary == nullptr || !transaction.accepted.has_value()) {
        return false;
      }
      return std::visit(
          [&](const auto& targets) {
            using Targets = std::decay_t<decltype(targets)>;
            if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                                   transaction.accepted->affected_operation_revisions.end(),
                                   [&](const auto& affected) {
                                     return affected.operation_id == target.operation_id;
                                   });
              });
            } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::binary_search(transaction.resources.begin(),
                                          transaction.resources.end(), target.resource_id);
              });
            } else {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(
                    transaction.accepted->affected_operation_revisions.begin(),
                    transaction.accepted->affected_operation_revisions.end(),
                    [&](const auto& affected) {
                      const auto found = std::lower_bound(
                          resolver_snapshot.operations.begin(), resolver_snapshot.operations.end(),
                          affected.operation_id, [](const auto& operation, const OperationId& id) {
                            return operation.operation_id < id;
                          });
                      return found != resolver_snapshot.operations.end() &&
                             found->operation_id == affected.operation_id &&
                             std::binary_search(found->controllable_components.begin(),
                                                found->controllable_components.end(),
                                                target.controllable_component_id);
                    });
              });
            }
          },
          ordinary->selector);
    };

    std::optional<std::string_view> overlapping_stop_id;
    if (ordinary != nullptr && !resolution.value().full_stop) {
      auto resident = resident_transactions(resolution.value());
      if (resident.empty() && resolution.value().disposition ==
                                  StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
        for (const auto& candidate : stop_transactions_) {
          if (candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
              transaction_matches_selector(*candidate)) {
            resident.push_back(candidate.get());
          }
        }
        std::sort(resident.begin(), resident.end(), [](const auto* left, const auto* right) {
          return *left->stop_id < *right->stop_id;
        });
      }
      const auto covering_full =
          std::find_if(resident.begin(), resident.end(), [&](const auto* tx) {
            return tx->full_stop && transaction_covers(*tx, resolution.value());
          });
      if (covering_full != resident.end()) {
        overlapping_stop_id = (*covering_full)->stop_id->value();
      } else if (resident.size() == 1U &&
                 transaction_covers(*resident.front(), resolution.value())) {
        overlapping_stop_id = resident.front()->stop_id->value();
      } else if (!resident.empty()) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = full_resolution;
        resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
        resolution.value().reason = StopScopeResolutionReasonDTO::kOverlappingStopTransactions;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
        captured_digest = ComputeCapturedStopScopeDigest(resolution.value());
        if (!captured_digest.ok()) {
          return Result<StopAcceptedDTO>::Err(captured_digest.error());
        }
      }
    }
    const auto full_predecessors = resident_transactions(full_resolution.value());
    auto full_outer_deadline_ns = outer_deadline_ns;
    std::vector<StopAcceptedPredecessorInput> full_predecessor_inputs;
    std::vector<StopControlWorkToken> full_predecessor_tokens;
    std::vector<StopCertificateDTO> full_predecessor_certificates;
    full_predecessor_inputs.reserve(full_predecessors.size());
    full_predecessor_tokens.reserve(full_predecessors.size());
    full_predecessor_certificates.reserve(full_predecessors.size());
    for (const auto* predecessor : full_predecessors) {
      full_outer_deadline_ns = std::min(full_outer_deadline_ns, predecessor->outer_deadline_ns);
      const auto remaining = std::min(predecessor->accepted->remaining_outer_duration_ns,
                                      predecessor->outer_deadline_ns <= accepted_at_ns
                                          ? 0
                                          : predecessor->outer_deadline_ns - accepted_at_ns);
      if (remaining <= 0) {
        motion_admission_closed_ = true;
        return reject<StopAcceptedDTO>("resident predecessor exhausted before aggregation",
                                       "stop_predecessor_deadline_exhausted",
                                       ErrorCode::kInvariantViolation);
      }
      full_predecessor_inputs.push_back({*predecessor->accepted, remaining});
      if (predecessor->certificate.has_value()) {
        full_predecessor_certificates.push_back(*predecessor->certificate);
      } else {
        full_predecessor_tokens.push_back(predecessor->token);
      }
    }

    const EmergencyStopAcceptanceCandidate candidate{
        context.request_id.value(),
        normalized_digest.value().value(),
        attribution.transport,
        attribution.assurance,
        attribution.principal_id.has_value()
            ? std::optional<std::string_view>(*attribution.principal_id)
            : std::nullopt,
        std::nullopt,
        proposed_stop_id.value(),
        proposed_generation,
        accepted_at_ns,
        outer_deadline_ns,
        full_outer_deadline_ns,
        resolution.value().disposition,
        captured_digest.value().value(),
        full_digest.value().value(),
        overlapping_stop_id,
        resolution.value().full_stop};
    outcome = emergency_stop_reserve_.reserve_or_join(candidate);
    if (!outcome.accepted() || outcome.work_token.slot >= stop_transactions_.size() ||
        outcome.correlation_slot >= stop_correlations_.size()) {
      return reject<StopAcceptedDTO>("emergency Stop reserve rejected its prepared candidate",
                                     "emergency_stop_reserve_invariant",
                                     ErrorCode::kInvariantViolation);
    }
    post_reserve_guard.armed = true;

    std::optional<EmergencyStopAffectedStateSnapshotDTO> projection_state;
    if (outcome.replay_status == RequestReplayStatusDTO::kReplayedExact) {
      const auto& retained = stop_correlations_[outcome.correlation_slot];
      if (!retained.has_value()) {
        return reject<StopAcceptedDTO>("Stop reserve replay has no retained result",
                                       "stop_replay_result_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *retained;
      accepted->replay_status = RequestReplayStatusDTO::kReplayedExact;
      work_token = outcome.work_token;
    } else if (outcome.acceptance_path == EmergencyStopAcceptancePathDTO::kJoinedResident ||
               (stop_transactions_[outcome.work_token.slot]->active &&
                stop_transactions_[outcome.work_token.slot]->token == outcome.work_token)) {
      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      if (!transaction.active || transaction.token != outcome.work_token ||
          !transaction.accepted.has_value() || !transaction.stop_id.has_value()) {
        return reject<StopAcceptedDTO>("joined Stop transaction is not resident",
                                       "stop_join_transaction_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *transaction.accepted;
      accepted->request_id = context.request_id;
      accepted->replay_status = RequestReplayStatusDTO::kAcceptedNew;
      accepted->requested_selector_digest = requested_selector_digest.value();
      if (!transaction.full_stop) {
        if (resolution.value().disposition ==
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
          accepted->resolution_disposition =
              resolution.value().stale_diagnostics.empty()
                  ? StopResolutionDispositionDTO::kExact
                  : StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
          accepted->resolution_reason =
              resolution.value().stale_diagnostics.empty()
                  ? StopScopeResolutionReasonDTO::kExactCurrentScope
                  : StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
        } else {
          accepted->resolution_disposition = resolution.value().disposition;
          accepted->resolution_reason = resolution.value().reason;
        }
        accepted->stale_diagnostics = std::move(resolution.value().stale_diagnostics);
      }
      accepted->remaining_outer_duration_ns =
          std::max<std::int64_t>(0, transaction.outer_deadline_ns - accepted_at_ns);
      accepted->accepted_at_ns = accepted_at_ns;
      for (auto& participant : accepted->participant_budget_allocations) {
        const auto elapsed =
            accepted->fixed_outer_duration_ns - accepted->remaining_outer_duration_ns;
        for (auto& phase : participant.phases) {
          phase.remaining_deadline_offset_ns =
              accepted->remaining_outer_duration_ns <= accepted->fixed_kernel_completion_reserve_ns
                  ? 0
                  : std::max<std::int64_t>(
                        0, std::min(phase.fixed_deadline_offset_ns - elapsed,
                                    accepted->remaining_outer_duration_ns -
                                        accepted->fixed_kernel_completion_reserve_ns));
        }
      }
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state.emplace();
      projection_state->operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          projection_state->operations.push_back(operation->state);
        }
      }
      projection_state->resources = transaction.resources;
      work_token = outcome.work_token;
    } else {
      if (outcome.full_stop && !resolution.value().full_stop) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = std::move(full_resolution);
        resolution.value().disposition = outcome.disposition;
        resolution.value().reason = request_id_retained_by_stop
                                        ? StopScopeResolutionReasonDTO::kRequestIdentityConflict
                                        : StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
      }
      auto acceptance_resolution = resolution.value();
      const bool aggregate = acceptance_resolution.full_stop && !full_predecessor_inputs.empty();
      if (aggregate) {
        for (const auto& predecessor : full_predecessor_inputs) {
          for (const auto& affected : predecessor.acceptance.affected_operation_revisions) {
            acceptance_resolution.operations.push_back(affected.operation_id);
          }
        }
        for (const auto* predecessor : full_predecessors) {
          acceptance_resolution.resources.insert(acceptance_resolution.resources.end(),
                                                 predecessor->resources.begin(),
                                                 predecessor->resources.end());
        }
        std::sort(acceptance_resolution.operations.begin(), acceptance_resolution.operations.end());
        acceptance_resolution.operations.erase(std::unique(acceptance_resolution.operations.begin(),
                                                           acceptance_resolution.operations.end()),
                                               acceptance_resolution.operations.end());
        std::sort(acceptance_resolution.resources.begin(), acceptance_resolution.resources.end());
        acceptance_resolution.resources.erase(std::unique(acceptance_resolution.resources.begin(),
                                                          acceptance_resolution.resources.end()),
                                              acceptance_resolution.resources.end());
      }
      std::vector<StopAffectedOperationRevisionDTO> affected_revisions;
      std::vector<PreparedParticipantStopPlanDTO> plans;
      std::vector<ResourceSlotStopFenceOwnerDTO> owner_rows;
      std::vector<OperationId> newly_fenced_operations;
      EmergencyStopAffectedStateSnapshotDTO affected_state;
      affected_revisions.reserve(resolution.value().operations.size());
      plans.reserve(resolution.value().operations.size());
      owner_rows.reserve(resolution.value().operations.size());
      newly_fenced_operations.reserve(resolution.value().operations.size());
      affected_state.operations.reserve(resolution.value().operations.size());
      for (const auto& operation_id : resolution.value().operations) {
        const bool inherited =
            aggregate &&
            std::any_of(full_predecessor_inputs.begin(), full_predecessor_inputs.end(),
                        [&](const auto& predecessor) {
                          const auto found = std::lower_bound(
                              predecessor.acceptance.affected_operation_revisions.begin(),
                              predecessor.acceptance.affected_operation_revisions.end(),
                              operation_id, [](const auto& affected, const OperationId& id) {
                                return affected.operation_id < id;
                              });
                          return found !=
                                     predecessor.acceptance.affected_operation_revisions.end() &&
                                 found->operation_id == operation_id;
                        });
        if (inherited) {
          continue;
        }
        auto* operation = find_active_operation_locked(operation_id);
        if (operation == nullptr ||
            operation->state.operation_revision.value() >= kMaximumKernelJsonInteger) {
          return reject<StopAcceptedDTO>("resolved Stop operation is not resident or exhausted",
                                         "stop_operation_not_fenceable",
                                         ErrorCode::kInvariantViolation);
        }
        const OperationRevision next_revision(operation->state.operation_revision.value() + 1U);
        affected_revisions.push_back({operation_id, next_revision});
        plans.push_back(*operation->ordinary_stop_plan);
        newly_fenced_operations.push_back(operation_id);
        owner_rows.push_back(ResourceSlotStopFenceOwnerDTO{operation->slot_owner,
                                                           operation->resources, next_revision});
      }
      std::sort(owner_rows.begin(), owner_rows.end(), [](const auto& left, const auto& right) {
        return std::tie(left.expected_owner.operation_id, left.expected_owner.participant_id) <
               std::tie(right.expected_owner.operation_id, right.expected_owner.participant_id);
      });
      std::vector<ResourceId> orphan_resources;
      for (const auto& resource : resolution.value().resources) {
        if (aggregate && std::any_of(full_predecessors.begin(), full_predecessors.end(),
                                     [&](const auto* predecessor) {
                                       return std::binary_search(predecessor->resources.begin(),
                                                                 predecessor->resources.end(),
                                                                 resource);
                                     })) {
          continue;
        }
        const auto found = std::lower_bound(
            slot_snapshot.slots.begin(), slot_snapshot.slots.end(), resource,
            [](const auto& slot, const ResourceId& id) { return slot.resource_id < id; });
        if (found != slot_snapshot.slots.end() && found->resource_id == resource &&
            !found->owner.has_value() &&
            found->state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
          orphan_resources.push_back(resource);
        }
      }
      const auto remaining_outer_duration_ns =
          std::max<std::int64_t>(0, outcome.outer_deadline_ns - accepted_at_ns);
      auto made =
          aggregate
              ? (ordinary != nullptr
                     ? MakeAggregatedStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, *cell_stop_deadline_budget_,
                                                  remaining_outer_duration_ns, accepted_at_ns,
                                                  plans, full_predecessor_inputs)
                     : MakeAggregatedStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, *cell_stop_deadline_budget_,
                                                  remaining_outer_duration_ns, accepted_at_ns,
                                                  plans, full_predecessor_inputs))
              : (ordinary != nullptr
                     ? MakeStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, *cell_stop_deadline_budget_,
                                        remaining_outer_duration_ns, accepted_at_ns, plans)
                     : MakeStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, *cell_stop_deadline_budget_,
                                        remaining_outer_duration_ns, accepted_at_ns, plans));
      if (!made.ok()) {
        return Result<StopAcceptedDTO>::Err(made.error());
      }
      accepted = std::move(made.value());
      auto fenced =
          owner_rows.empty() && orphan_resources.empty()
              ? Result<ResourceSlotStopFenceResultDTO>::Ok(ResourceSlotStopFenceResultDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation, {}, {}})
              : resource_slots_->stop_fence_batch(ResourceSlotStopFenceRequestDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation,
                    std::move(owner_rows), orphan_resources});
      if (!fenced.ok()) {
        motion_admission_closed_ = true;
        return Result<StopAcceptedDTO>::Err(fenced.error());
      }
      for (const auto& affected : affected_revisions) {
        auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation == nullptr) {
          continue;
        }
        operation->state.operation_revision = affected.operation_revision_after_acceptance;
        operation->state.state = operation->ordinary_acceptance_pending
                                     ? OperationStateKind::kCancelledPreDispatch
                                     : OperationStateKind::kStopping;
        operation->state.active_stop_id = StopId(std::string(outcome.stop_id()));
        operation->state.active_stop_generation = outcome.stop_generation;
        operation->state.updated_at_ns = accepted_at_ns;
        operation->state.participants.front().state = operation->state.state;
        operation->slot_owner.operation_revision = affected.operation_revision_after_acceptance;
        operation->slot_state = ResourceSlotState::kStopping;
        operation->stop_effect_in_flight = true;
        affected_state.operations.push_back(operation->state);
      }
      affected_state.operations.clear();
      affected_state.operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          affected_state.operations.push_back(operation->state);
        }
      }
      affected_state.resources = acceptance_resolution.resources;

      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      transaction = StopTransaction{
          true,
          outcome.work_token,
          StopId(std::string(outcome.stop_id())),
          outcome.stop_generation,
          outcome.outer_deadline_ns,
          outcome.full_stop,
          false,
          EmergencyStopProjectionStateDTO::kPending,
          false,
          false,
          false,
          *accepted,
          projected_intent,
          affected_revisions,
          fenced.value().orphan_resources,
          std::move(newly_fenced_operations),
          acceptance_resolution.resources,
          aggregate ? full_predecessor_tokens : std::vector<StopControlWorkToken>{},
          aggregate ? full_predecessor_certificates : std::vector<StopCertificateDTO>{},
          false,
          {},
          std::nullopt,
          std::nullopt};
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state = std::move(affected_state);
      work_token = outcome.work_token;
      handoff_work = true;
      // All deadline mutation for one physical Stop slot is serialized by
      // state_mutex_. A predecessor may terminalize as soon as this aggregate
      // becomes resident and rearm it at `now`; publishing the outer watchdog
      // later must never overwrite that wakeup.
      stop_control_executor_->ArmDeadline(work_token, outcome.outer_deadline_ns);
      next_stop_id_ = proposed_number + 1U;
      cell_stop_generation_ = std::max(cell_stop_generation_, outcome.stop_generation);
      if (outcome.full_stop ||
          accepted->resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
        motion_admission_closed_ = true;
      }
    }

    if (outcome.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        outcome.correlation_retained && accepted.has_value() && projection_state.has_value()) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        // The containment effect is already linearized and handed to the
        // prequalified StopControl owner. Forensic projection capacity may
        // therefore degrade admission, but it must never reject, widen, pin,
        // or terminate an accepted Stop.
        auto& transaction = *stop_transactions_[outcome.work_token.slot];
        transaction.persistence_degraded = true;
        transaction.acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        motion_admission_closed_ = true;
      } else {
        auto& projection = *stop_projection_jobs_[projection_token->slot];
        projection.intent = projected_intent;
        projection.affected_state = std::move(*projection_state);
        projection.acceptance =
            EmergencyStopAcceptanceRecordDTO{config_.request_owner_epoch,
                                             context.request_id,
                                             normalized_digest.value(),
                                             std::nullopt,
                                             attribution,
                                             accepted->replay_status,
                                             accepted->stop_id,
                                             accepted->stop_generation,
                                             accepted_at_ns,
                                             outcome.outer_deadline_ns,
                                             accepted->resolution_disposition,
                                             accepted->captured_scope_digest,
                                             EmergencyStopEntryStateDTO::kActive,
                                             outcome.acceptance_path,
                                             EmergencyStopProjectionStateDTO::kPending};
        projection.accepted = *accepted;
      }
    }
  }

  if (handoff_work) {
    stop_control_executor_->Publish(work_token);
  }
  if (projection_token.has_value()) {
    stop_projection_worker_->Publish(*projection_token);
  }
  if (!accepted.has_value()) {
    return reject<StopAcceptedDTO>("Stop acceptance result was not retained",
                                   "stop_acceptance_result_missing",
                                   ErrorCode::kInvariantViolation);
  }
  post_reserve_guard.armed = false;
  return Result<StopAcceptedDTO>::Ok(std::move(*accepted));
}

void ResourceAddressedExecutionKernel::ProjectStop(const StopControlWorkToken token) noexcept {
  StopProjectionJob job;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_projection_jobs_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& retained = *stop_projection_jobs_[token.slot];
      if (!retained.active || retained.token != token) {
        return;
      }
      job = retained;
      retained.active = false;
    }

    if (job.terminal_status.has_value()) {
      if (!job.terminal_certificate.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      if (!job.terminal_fault_latch.has_value() &&
          job.terminal_status->acceptance_projection.state ==
              StopDurableProjectionStateDTO::kFailed) {
        std::lock_guard state_lock(state_mutex_);
        if (!stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value() ||
            cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
          motion_admission_closed_ = true;
          return;
        }
        cell_stop_fault_latch_generation_ =
            StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
        job.terminal_fault_latch = persistence_fault_latch(
            config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
            *stop_graph_revision_, *job.terminal_certificate, cell_stop_fault_latch_generation_,
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                           std::nullopt});
        job.terminal_status->fault_latch = job.terminal_fault_latch;
        active_stop_fault_latch_ = job.terminal_fault_latch;
        motion_admission_closed_ = true;
      }
      const auto projected =
          journal_.RecordStopTerminal(config_.request_owner_epoch, *job.terminal_status,
                                      *job.terminal_certificate, job.terminal_fault_latch);
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.terminal_certificate->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.terminal_certificate->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->terminal_projection_token == token) {
        (*record)->terminal_projection_token.reset();
      }
      if (job.terminal_fault_latch.has_value() && !(*record)->fault_latch.has_value()) {
        (*record)->fault_latch = job.terminal_fault_latch;
        (*record)->status.fault_latch = job.terminal_fault_latch;
      }
      if (projected.state == StopForensicProjectionStateDTO::kRecorded) {
        (*record)->status.terminal_projection = StopDurableProjectionStatusDTO{
            StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection = StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      } else {
        motion_admission_closed_ = true;
        const auto failure =
            projected.error.value_or(ErrorDTO{"operation_journal_stop_terminal_projection_failed",
                                              "Stop terminal forensic projection failed",
                                              {}});
        (*record)->status.terminal_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if (!(*record)->fault_latch.has_value() && stop_retained_bundle_id_.has_value() &&
            stop_graph_revision_.has_value() &&
            cell_stop_fault_latch_generation_.value() < kMaximumKernelJsonInteger) {
          cell_stop_fault_latch_generation_ =
              StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
          (*record)->fault_latch = persistence_fault_latch(
              config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
              *stop_graph_revision_, (*record)->certificate, cell_stop_fault_latch_generation_,
              StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U});
          (*record)->status.fault_latch = (*record)->fault_latch;
          active_stop_fault_latch_ = (*record)->fault_latch;
        }
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection =
              StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      }
      return;
    }

    if (job.surveillance_checkpoint.has_value()) {
      if (!job.surveillance_fault_latch.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      const auto projected = journal_.RecordStopSurveillanceCheckpoint(
          config_.request_owner_epoch, *job.surveillance_checkpoint, *job.surveillance_fault_latch);
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->surveillance_projection_token == token) {
        (*record)->surveillance_projection_token.reset();
      }
      if ((*record)->surveillance_state == nullptr) {
        motion_admission_closed_ = true;
        return;
      }
      auto checkpoint_status = StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
      if (projected.state != StopForensicProjectionStateDTO::kRecorded) {
        const auto failure = projected.error.value_or(
            ErrorDTO{"operation_journal_stop_surveillance_projection_failed",
                     "Stop surveillance checkpoint projection failed",
                     {}});
        checkpoint_status =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        motion_admission_closed_ = true;
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      auto& current = (*record)->surveillance_state->projection;
      if (current.surveillance_generation == job.surveillance_checkpoint->surveillance_generation &&
          current.checkpoint_generation == job.surveillance_checkpoint->checkpoint_generation) {
        current.checkpoint_projection = checkpoint_status;
      }
      (*record)->status.surveillance = current;
      (*record)->status.surveillance_projection = current.checkpoint_projection;
      if ((*record)->fault_latch.has_value()) {
        (*record)->status.fault_latch = (*record)->fault_latch;
      }
      return;
    }

    if (!job.acceptance.has_value() || !job.intent.has_value() || !job.accepted.has_value() ||
        !job.affected_state.has_value()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }

    const auto projected =
        journal_.RecordStopAccepted(config_.request_owner_epoch, *job.acceptance, *job.intent,
                                    *job.accepted, *job.affected_state);
    const auto state = projected.projection_state == EmergencyStopProjectionStateDTO::kRecorded
                           ? EmergencyStopProjectionStateDTO::kRecorded
                           : EmergencyStopProjectionStateDTO::kFailed;
    const auto durable_status =
        state == EmergencyStopProjectionStateDTO::kRecorded
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{
                  StopDurableProjectionStateDTO::kFailed,
                  projected.error.value_or(ErrorDTO{"operation_journal_stop_projection_failed",
                                                    "Stop acceptance forensic projection failed",
                                                    {}}),
                  1U};
    const bool marked =
        emergency_stop_reserve_.mark_projection(job.acceptance->stop_id.value(), state);
    std::optional<StopControlWorkToken> terminal_projection;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end()) {
        (*transaction)->acceptance_projection_state = state;
        if ((*transaction)->terminal_projection_token.has_value()) {
          auto& terminal_job =
              *stop_projection_jobs_[(*transaction)->terminal_projection_token->slot];
          if (terminal_job.terminal_status.has_value()) {
            terminal_job.terminal_status->acceptance_projection = durable_status;
          }
        }
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end()) {
        (*terminal_record)->status.acceptance_projection = durable_status;
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection = durable_status;
              terminal_projection = retained_token;
            }
          }
        }
      }
      if (state == EmergencyStopProjectionStateDTO::kFailed ||
          (!marked && transaction == stop_transactions_.end() &&
           terminal_record == stop_terminal_history_.end())) {
        motion_admission_closed_ = true;
        if (transaction != stop_transactions_.end()) {
          (*transaction)->persistence_degraded = true;
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  } catch (...) {
    std::optional<StopControlWorkToken> terminal_projection;
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (job.surveillance_checkpoint.has_value()) {
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record != stop_terminal_history_.end()) {
        if ((*record)->surveillance_projection_token == token) {
          (*record)->surveillance_projection_token.reset();
        }
        const ErrorDTO failure{"operation_journal_stop_surveillance_projection_failed",
                               "Stop surveillance checkpoint projection failed",
                               {}};
        (*record)->status.surveillance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*record)->surveillance_state != nullptr) {
          (*record)->surveillance_state->projection.checkpoint_projection =
              (*record)->status.surveillance_projection;
          (*record)->status.surveillance = (*record)->surveillance_state->projection;
        }
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      return;
    }
    if (job.acceptance.has_value()) {
      static_cast<void>(emergency_stop_reserve_.mark_projection(
          job.acceptance->stop_id.value(), EmergencyStopProjectionStateDTO::kFailed));
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end()) {
        (*transaction)->acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        (*transaction)->persistence_degraded = true;
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end()) {
        const ErrorDTO failure{"operation_journal_stop_projection_failed",
                               "Stop acceptance forensic projection failed",
                               {}};
        (*terminal_record)->status.acceptance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection =
                  (*terminal_record)->status.acceptance_projection;
              terminal_projection = retained_token;
            }
          }
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  }
}

void ResourceAddressedExecutionKernel::StageAttemptCompleted(
    const StopStageAttemptHandle handle) noexcept {
  std::optional<StopControlWorkToken> owner_token;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      for (const auto& transaction : stop_transactions_) {
        if (transaction == nullptr || !transaction->active || transaction->terminal) {
          continue;
        }
        const bool owns = std::any_of(
            transaction->participants.begin(), transaction->participants.end(),
            [&](const auto& participant) {
              return std::any_of(participant.attempts.begin(), participant.attempts.end(),
                                 [&](const auto& attempt) {
                                   return attempt.handle.has_value() && *attempt.handle == handle &&
                                          !attempt.result_consumed;
                                 });
            });
        if (owns) {
          owner_token = transaction->token;
          // Deadline mutation for one physical Stop slot is serialized by
          // state_mutex_. This cannot race with ProgressStop rearming the
          // participant phase watchdog after it publishes an attempt.
          stop_control_executor_->ArmDeadline(*owner_token, clock_.now());
          break;
        }
      }
    }
    if (owner_token.has_value()) {
      // Publish coalesces an already-delivered generation. Rearming its fixed
      // deadline at the current instant under the Kernel state lock is the
      // allocation-free wakeup path for an asynchronously completed attempt.
      stop_control_executor_->Publish(*owner_token);
      return;
    }
    // A terminal transaction may already have handed off its immutable
    // certificate. Consume and release a late isolated result so one hung
    // predecessor can never exhaust future Stop-stage capacity. Later P5d
    // projection owns the bounded late-audit sink.
    const auto late = stop_stage_attempt_executor_->TryTake(handle);
    if (late.status != StopStageAttemptTakeStatus::kPending) {
      static_cast<void>(stop_stage_attempt_executor_->Release(handle));
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

std::optional<StopControlWorkToken>
ResourceAddressedExecutionKernel::allocate_stop_projection_job_locked() noexcept {
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    auto& job = *stop_projection_jobs_[index];
    if (job.active) {
      continue;
    }
    auto& generation = stop_projection_generations_[index];
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
      continue;
    }
    ++generation;
    const StopControlWorkToken token{static_cast<StopControlSlotIndex>(index), generation};
    job = StopProjectionJob{};
    job.token = token;
    job.active = true;
    return token;
  }
  return std::nullopt;
}

bool ResourceAddressedExecutionKernel::commit_stop_terminal_locked(
    StopTransaction& transaction, const std::int64_t completed_at_ns) noexcept {
  try {
    if (!transaction.accepted.has_value() || !transaction.stop_id.has_value() ||
        transaction.certificate.has_value()) {
      return false;
    }
    std::vector<StopCertificateDTO> predecessors = transaction.predecessor_certificates;
    predecessors.reserve(predecessors.size() + transaction.predecessor_tokens.size());
    for (const auto& predecessor_token : transaction.predecessor_tokens) {
      if (predecessor_token.slot >= stop_transactions_.size()) {
        return false;
      }
      const auto& predecessor = *stop_transactions_[predecessor_token.slot];
      if (!predecessor.active || predecessor.token != predecessor_token ||
          !predecessor.certificate.has_value()) {
        return false;
      }
      predecessors.push_back(*predecessor.certificate);
    }
    std::sort(predecessors.begin(), predecessors.end(),
              [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
    if (std::adjacent_find(predecessors.begin(), predecessors.end(),
                           [](const auto& left, const auto& right) {
                             return left.stop_id == right.stop_id;
                           }) != predecessors.end() ||
        predecessors.size() != transaction.accepted->joined_predecessor_stops.size()) {
      return false;
    }

    std::vector<StopScopeEntryDTO> entries;
    std::vector<StopAttemptedStageResultDTO> attempts;
    entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
    for (const auto& participant : transaction.participants) {
      entries.push_back(participant_scope_entry(
          participant, *transaction.stop_id, transaction.stop_generation,
          transaction.accepted->accepted_at_ns, transaction.accepted->remaining_outer_duration_ns));
      attempts.insert(attempts.end(), participant.retained_results.begin(),
                      participant.retained_results.end());
    }
    for (const auto& resource : transaction.newly_captured_resources) {
      entries.push_back(StopScopeEntryDTO{
          StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
          resource,
          StopScopeEntryDispositionDTO::kNotStoppable,
          StopScopeDispatchStateDTO::kEffectUncertain,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          transaction.accepted->accepted_at_ns,
          transaction.accepted->remaining_outer_duration_ns,
          {}});
    }
    // Fresh operation IDs and orphan resources are already canonical. The key
    // variant orders participant subjects before resource-only subjects.
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
      const auto& left_key = left.key;
      const auto& right_key = right.key;
      if (left_key.origin_stop_id != right_key.origin_stop_id) {
        return left_key.origin_stop_id < right_key.origin_stop_id;
      }
      if (left_key.origin_stop_generation != right_key.origin_stop_generation) {
        return left_key.origin_stop_generation < right_key.origin_stop_generation;
      }
      if (left_key.subject.index() != right_key.subject.index()) {
        return left_key.subject.index() < right_key.subject.index();
      }
      if (const auto* left_participant =
              std::get_if<StopParticipantScopeKeyDTO>(&left_key.subject)) {
        const auto& right_participant = std::get<StopParticipantScopeKeyDTO>(right_key.subject);
        return std::tie(left_participant->operation_id, left_participant->participant_id) <
               std::tie(right_participant.operation_id, right_participant.participant_id);
      }
      return std::get<ResourceId>(left_key.subject) < std::get<ResourceId>(right_key.subject);
    });
    std::sort(attempts.begin(), attempts.end(), [](const auto& left, const auto& right) {
      const auto key = [](const auto& attempt) {
        const auto& participant = std::get<StopParticipantScopeKeyDTO>(attempt.scope_key.subject);
        return std::tie(attempt.scope_key.origin_stop_id, attempt.scope_key.origin_stop_generation,
                        participant.operation_id, participant.participant_id,
                        attempt.result.request.stage_index, attempt.result_sequence);
      };
      return key(left) < key(right);
    });

    std::optional<ErrorDTO> terminal_error;
    for (const auto& participant : transaction.participants) {
      if (!participant.contained && participant.terminal_error.has_value()) {
        terminal_error = participant.terminal_error;
        break;
      }
    }
    const bool known_latch_required =
        transaction.persistence_degraded ||
        transaction.accepted->resolution_disposition ==
            StopResolutionDispositionDTO::kPromotedToFull ||
        std::any_of(attempts.begin(), attempts.end(),
                    [](const auto& attempt) {
                      return attempt.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                             (stage_kind(attempt.result.request.participant_stop_plan,
                                         attempt.result.request.stage_index) !=
                                  StopEscalationStageKind::kNominal ||
                              attempt.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded);
                    }) ||
        !transaction.newly_captured_resources.empty();
    std::optional<StopFaultLatchId> latch_id;
    std::optional<StopFaultLatchGeneration> latch_generation;
    const auto assign_latch_identity = [&]() {
      if (latch_id.has_value()) {
        return true;
      }
      if (cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
        return false;
      }
      latch_generation = StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
      latch_id = StopFaultLatchId("stop-fault." + config_.request_owner_epoch.value() + "." +
                                  std::to_string(latch_generation->value()));
      return true;
    };
    if (known_latch_required && !assign_latch_identity()) {
      return false;
    }
    StopTerminalReductionInputDTO reduction{
        StopCertificateId("stop-certificate." + transaction.stop_id->value()),
        StopCertificateRevision(1U),
        *transaction.stop_id,
        transaction.stop_generation,
        requested_selector_snapshot(transaction.projected_intent,
                                    transaction.accepted->requested_selector_digest),
        transaction.accepted->resolution_disposition,
        transaction.accepted->resolution_reason,
        transaction.accepted->stale_diagnostics,
        {StopScopeCaptureHistoryDTO{
            1U, transaction.accepted->resolution_disposition,
            transaction.accepted->resolution_reason, transaction.accepted->captured_scope_digest,
            transaction.accepted->joined_predecessor_stops,
            transaction.newly_fenced_operation_revisions, transaction.newly_captured_resources}},
        std::move(predecessors),
        std::move(entries),
        std::move(attempts),
        std::nullopt,
        latch_id,
        latch_generation,
        completed_at_ns,
        terminal_error};
    auto reduced = ReduceStopTerminal(reduction);
    // The reducer owns the authoritative nominal/escalated/partial outcome.
    // Pre-classification above avoids a second reduction in the common case,
    // while this exact retry closes outcomes (for example evidence-derived
    // partial containment) that cannot be guessed before reduction.
    if (!reduced.ok() && reduced.error().code == "stop_terminal_fault_latch_required" &&
        !latch_id.has_value() && assign_latch_identity()) {
      reduction.fault_latch_id = latch_id;
      reduction.fault_latch_generation = latch_generation;
      reduced = ReduceStopTerminal(reduction);
    }
    if (!reduced.ok()) {
      return false;
    }
    auto certificate = std::move(reduced.value());
    bool projection_capacity_available = false;
    for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
      if (!stop_projection_jobs_[index]->active &&
          stop_projection_generations_[index] != std::numeric_limits<std::uint64_t>::max()) {
        projection_capacity_available = true;
        break;
      }
    }
    std::optional<ErrorDTO> projection_capacity_error;
    if (!projection_capacity_available) {
      projection_capacity_error =
          ErrorDTO{"operation_journal_stop_terminal_projection_capacity_exhausted",
                   "Stop terminal forensic projection capacity is exhausted",
                   {}};
      transaction.persistence_degraded = true;
      motion_admission_closed_ = true;
      if (!assign_latch_identity()) {
        return false;
      }
    }
    const auto terminal_projection_status =
        projection_capacity_error.has_value()
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                             projection_capacity_error, 1U}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt};
    std::optional<StopFaultLatchDTO> latch;
    if (terminal_fault_required(certificate, transaction.persistence_degraded)) {
      if (!latch_id.has_value() || !latch_generation.has_value() ||
          !stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value()) {
        return false;
      }
      const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                            certificate.stop_certificate_revision,
                                            certificate.stop_id, certificate.stop_generation};
      std::vector<OperationId> operations;
      std::vector<ResourceId> resources;
      operations.reserve(certificate.scope_entries.size());
      for (const auto& entry : certificate.scope_entries) {
        if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&entry.key.subject)) {
          operations.push_back(participant->operation_id);
          const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry.facts);
          resources.insert(resources.end(), facts.resources.begin(), facts.resources.end());
        } else {
          resources.push_back(std::get<ResourceId>(entry.key.subject));
        }
      }
      std::sort(operations.begin(), operations.end());
      operations.erase(std::unique(operations.begin(), operations.end()), operations.end());
      std::sort(resources.begin(), resources.end());
      resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
      const auto trigger = primary_fault_trigger(certificate, transaction.persistence_degraded);
      latch = StopFaultLatchDTO{
          config_.cell_id,
          certificate.stop_id,
          certificate.stop_generation,
          reference,
          *latch_id,
          *latch_generation,
          trigger,
          transaction.persistence_degraded,
          std::move(operations),
          std::move(resources),
          *stop_retained_bundle_id_,
          *stop_graph_revision_,
          terminal_projection_status,
          StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                         std::nullopt, std::nullopt},
          StopFaultLatchStateDTO::kLatched,
          1U,
          StopFaultRecoveryStatusDTO::kWaitingForObservation,
          fault_trigger_reason(trigger),
          {}};
      if (stop_fault_latch_field_error(*latch).has_value()) {
        return false;
      }
    }
    std::unique_ptr<StopSurveillanceReducerState> surveillance_state;
    if (latch.has_value() && stop_requires_surveillance(certificate, *latch)) {
      if (!config_.stop_surveillance.has_value()) {
        motion_admission_closed_ = true;
      } else {
        auto source_plans = stop_surveillance_source_plans(certificate);
        if (!source_plans.ok()) {
          motion_admission_closed_ = true;
        } else {
          auto seeded = MakeStopSurveillanceReducerState(
              config_.request_owner_epoch, 1U, certificate, *latch,
              config_.stop_surveillance->policy, std::move(source_plans.value()));
          if (!seeded.ok()) {
            return false;
          }
          surveillance_state =
              std::make_unique<StopSurveillanceReducerState>(std::move(seeded.value()));
        }
      }
    }
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const auto acceptance_projection = [&] {
      switch (transaction.acceptance_projection_state) {
        case EmergencyStopProjectionStateDTO::kPending:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kRecorded:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kFailed:
          return StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kFailed,
              ErrorDTO{"operation_journal_stop_projection_failed",
                       "Stop acceptance forensic projection failed",
                       {}},
              1U};
      }
      return StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kFailed,
          ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
    }();
    const auto surveillance_projection =
        surveillance_state != nullptr
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                             std::nullopt, std::nullopt};
    StopStatusDTO status{*transaction.accepted,
                         terminal_state,
                         certificate.scope_entries,
                         {},
                         certificate.attempted_stage_results,
                         certificate.evidence_items,
                         certificate.unsettled,
                         certificate.failed,
                         acceptance_projection,
                         terminal_projection_status,
                         surveillance_projection,
                         StopCertificateRefDTO{certificate.stop_certificate_id,
                                               certificate.stop_certificate_revision,
                                               certificate.stop_id, certificate.stop_generation},
                         latch,
                         surveillance_state != nullptr
                             ? std::optional(surveillance_state->projection)
                             : std::nullopt};
    if (stop_terminal_projection_field_error(status, certificate, latch).has_value()) {
      return false;
    }
    if (stop_terminal_history_.size() == kMaximumResidentStopTerminalRecords) {
      const auto referenced_by_live_aggregate = [&](const StopTerminalRecord& record) {
        return std::any_of(
            stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
              return candidate != nullptr && candidate->active && !candidate->terminal &&
                     candidate->accepted.has_value() &&
                     std::any_of(candidate->accepted->joined_predecessor_stops.begin(),
                                 candidate->accepted->joined_predecessor_stops.end(),
                                 [&](const auto& predecessor) {
                                   return predecessor.predecessor_stop_id ==
                                              record.certificate.stop_id &&
                                          predecessor.predecessor_stop_generation ==
                                              record.certificate.stop_generation;
                                 });
            });
      };
      const auto compactable = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr &&
                   record->status.terminal_projection.state ==
                       StopDurableProjectionStateDTO::kRecorded &&
                   (!record->fault_latch.has_value() ||
                    record->fault_latch->state == StopFaultLatchStateDTO::kCleared) &&
                   (!record->status.surveillance.has_value() ||
                    record->status.surveillance->state ==
                        StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved) &&
                   !referenced_by_live_aggregate(*record);
          });
      if (compactable == stop_terminal_history_.end()) {
        return false;
      }
      stop_terminal_history_.erase(compactable);
    }
    std::optional<StopControlWorkToken> projection_token;
    if (projection_capacity_available) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        return false;
      }
      auto& projection = *stop_projection_jobs_[projection_token->slot];
      projection.terminal_status = status;
      projection.terminal_certificate = certificate;
      projection.terminal_fault_latch = latch;
    }
    std::int64_t first_surveillance_sample_ns = 0;
    std::int64_t first_surveillance_checkpoint_ns = 0;
    if (surveillance_state != nullptr) {
      const auto& policy = config_.stop_surveillance->policy;
      if (completed_at_ns > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns ||
          completed_at_ns >
              std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
        return false;
      }
      first_surveillance_sample_ns = completed_at_ns + policy.sample_cadence_ns;
      first_surveillance_checkpoint_ns = completed_at_ns + policy.checkpoint_interval_ns;
    }
    auto record = std::make_unique<StopTerminalRecord>(StopTerminalRecord{
        status, certificate, latch, projection_token, std::move(surveillance_state), std::nullopt,
        0U, false, std::nullopt, first_surveillance_sample_ns, first_surveillance_checkpoint_ns});
    transaction.certificate = certificate;
    transaction.terminal_projection_token = projection_token;
    stop_terminal_history_.push_back(std::move(record));
    if (latch.has_value()) {
      cell_stop_fault_latch_generation_ = *latch_generation;
      active_stop_fault_latch_ = *latch;
      motion_admission_closed_ = true;
    }
    static_cast<void>(
        emergency_stop_reserve_.release_after_terminal_handoff(transaction.stop_id->value()));
    return true;
  } catch (...) {
    return false;
  }
}

void ResourceAddressedExecutionKernel::ProgressStop(const StopControlWorkToken token) noexcept {
  try {
    std::optional<std::int64_t> next_deadline;
    std::vector<StopControlWorkToken> dependents;
    std::optional<StopControlWorkToken> terminal_projection;
    std::optional<std::pair<StopCertificateId, std::int64_t>> surveillance_schedule;
    bool terminal = false;
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token || transaction.terminal ||
          transaction.progress_in_flight || !transaction.stop_id.has_value()) {
        return;
      }
      transaction.progress_in_flight = true;
      const auto stop_id = *transaction.stop_id;
      const auto stop_generation = transaction.stop_generation;
      const auto now = clock_.now();

      if (!transaction.participant_work_initialized) {
        transaction.participants.reserve(transaction.operations.size());
        for (const auto& operation_id : transaction.operations) {
          const auto* operation = find_active_operation_locked(operation_id);
          if (operation == nullptr || operation->state.participants.size() != 1U ||
              operation->ordinary_stop_plan == nullptr) {
            motion_admission_closed_ = true;
            continue;
          }
          const auto& participant = operation->state.participants.front();
          StopParticipantWork work{
              operation_id,
              operation->state.operation_revision,
              participant.controller_goal_id,
              participant.controller_goal_generation,
              operation->binding,
              operation->stop_readiness,
              operation->retained_default_stop_limits,
              operation->stop_node_category,
              *operation->ordinary_stop_plan,
              operation->resources,
              operation->stop_execution_port,
              operation->state.state == OperationStateKind::kCancelledPreDispatch,
              false,
              false,
              0U,
              1U,
              std::nullopt,
              {},
              {}};
          work.retained_results.reserve(kMaxStopEscalationStages * 2U);
          transaction.participants.push_back(std::move(work));
        }
        transaction.participant_work_initialized = true;
      }

      for (auto& item : transaction.participants) {
        const auto retain_first_error = [&](ErrorDTO failure) {
          if (!item.terminal_error.has_value()) {
            item.terminal_error = std::move(failure);
          }
        };
        if (item.terminal) {
          continue;
        }
        if (item.cancelled_pre_dispatch) {
          item.contained = true;
          item.terminal = true;
        }

        for (auto& attempt : item.attempts) {
          if (!attempt.started || attempt.result_consumed || !attempt.handle.has_value()) {
            continue;
          }
          auto result = stop_stage_attempt_executor_->TryTake(*attempt.handle);
          if (result.status == StopStageAttemptTakeStatus::kPending) {
            continue;
          }
          attempt.result_consumed = true;
          const auto released = stop_stage_attempt_executor_->Release(*attempt.handle);
          if (released != StopStageAttemptReleaseStatus::kReleased) {
            motion_admission_closed_ = true;
          }
          if (result.status == StopStageAttemptTakeStatus::kCompleted &&
              result.result.has_value()) {
            auto valid = ValidateParticipantStopStageResult(
                *result.result, item.readiness.value().resolved_stop_base_plan, item.binding,
                item.retained_default_stop_limits, item.node_category, item.resources);
            if (valid.ok()) {
              item.retained_results.push_back(StopAttemptedStageResultDTO{
                  StopScopeEntryKeyDTO{
                      stop_id, stop_generation,
                      StopParticipantScopeKeyDTO{item.operation_id, item.plan.participant_id}},
                  item.next_result_sequence++, now,
                  attempt.superseded ? StopStageResultAuthorityDTO::kSupersededLate
                                     : StopStageResultAuthorityDTO::kAuthoritative,
                  std::move(*result.result)});
              if (!attempt.superseded) {
                const auto& retained = item.retained_results.back().result;
                if (retained.error.has_value()) {
                  retain_first_error(*retained.error);
                }
                if (retained.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
                  item.contained = true;
                  item.terminal = true;
                  item.terminal_error.reset();
                } else {
                  ++item.current_stage_index;
                }
              }
            } else {
              motion_admission_closed_ = true;
              if (!attempt.superseded) {
                retain_first_error(valid.error());
                ++item.current_stage_index;
              }
            }
          } else {
            motion_admission_closed_ = true;
            if (!attempt.superseded) {
              retain_first_error(error(std::string(to_string(ErrorCode::kInternalError)),
                                       "Stop-stage execution lane failed without a result",
                                       "stop_stage_attempt_infrastructure_failure"));
              ++item.current_stage_index;
            }
          }
        }

        while (!item.terminal) {
          if (item.current_stage_index >= item.plan.stage_commands.size()) {
            item.terminal = true;
            item.contained = false;
            if (!item.terminal_error.has_value()) {
              item.terminal_error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                          "Stop cascade exhausted without proof of containment",
                                          "participant_stop_cascade_exhausted");
            }
            break;
          }
          auto& attempt = item.attempts[item.current_stage_index];
          if (attempt.started && !attempt.result_consumed) {
            if (now < attempt.stage_deadline_ns) {
              next_deadline = !next_deadline.has_value()
                                  ? std::optional(attempt.stage_deadline_ns)
                                  : std::min(*next_deadline, attempt.stage_deadline_ns);
              break;
            }
            attempt.superseded = true;
            if (attempt.handle.has_value()) {
              static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*attempt.handle));
            }
            retain_first_error(
                error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                      "Stop stage did not prove containment before its phase deadline",
                      "participant_stop_stage_deadline_expired"));
            ++item.current_stage_index;
            continue;
          }
          if (attempt.started) {
            ++item.current_stage_index;
            continue;
          }
          auto budget = remaining_stop_budget(item.plan, stop_id, stop_generation, now,
                                              transaction.outer_deadline_ns);
          const auto watchdog = stage_watchdog_deadline(
              item.plan.stage_commands[item.current_stage_index], budget, now);
          if (!watchdog.has_value() || budget.remaining_outer_duration_ns <= 0) {
            retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                     "Stop stage command phase expired before dispatch",
                                     "participant_stop_stage_command_phase_expired"));
            ++item.current_stage_index;
            continue;
          }
          if (item.port == nullptr) {
            motion_admission_closed_ = true;
            retain_first_error(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                     "prepared Stop participant has no execution port",
                                     "participant_stop_execution_port_missing"));
            ++item.current_stage_index;
            continue;
          }
          auto stage = resolve_participant_stop_stage(item, stop_id, stop_generation,
                                                      item.current_stage_index, budget);
          if (!stage.ok()) {
            motion_admission_closed_ = true;
            retain_first_error(stage.error());
            ++item.current_stage_index;
            continue;
          }
          attempt.stage =
              std::make_shared<const ResolvedParticipantStopStageDTO>(std::move(stage.value()));
          attempt.budget = std::make_shared<const RemainingStopBudgetDTO>(std::move(budget));
          const auto started = stop_stage_attempt_executor_->TryStart(
              StopStageAttemptRequest{item.port, attempt.stage, attempt.budget});
          if (started.status != StopStageAttemptStartStatus::kStarted ||
              !started.handle.has_value()) {
            motion_admission_closed_ = true;
            item.terminal = true;
            item.contained = false;
            item.terminal_error =
                error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "prequalified Stop-stage isolation could not accept prepared work",
                      "stop_stage_attempt_prequalification_failed");
            break;
          }
          attempt.started = true;
          attempt.handle = *started.handle;
          attempt.stage_deadline_ns = *watchdog;
          next_deadline = !next_deadline.has_value()
                              ? watchdog
                              : std::optional(std::min(*next_deadline, *watchdog));
          break;
        }

        if (!item.terminal) {
          continue;
        }
        auto* operation = find_active_operation_locked(item.operation_id);
        if (operation == nullptr || operation->state.active_stop_id != stop_id ||
            operation->state.active_stop_generation != stop_generation) {
          motion_admission_closed_ = true;
          continue;
        }
        operation->state.state = item.contained ? OperationStateKind::kStopped
                                                : OperationStateKind::kInterventionRequired;
        operation->state.participants.front().state = operation->state.state;
        operation->state.participants.front().terminal_error =
            item.contained ? std::nullopt : item.terminal_error;
        operation->state.updated_at_ns = now;
        const auto transitioned = resource_slots_->transition(
            operation->slot_owner, operation->resources, ResourceSlotState::kTerminal);
        if (transitioned.ok()) {
          operation->slot_state = ResourceSlotState::kTerminal;
        } else {
          motion_admission_closed_ = true;
          operation->state.state = OperationStateKind::kInterventionRequired;
          operation->state.participants.front().state = OperationStateKind::kInterventionRequired;
          operation->state.participants.front().terminal_error = transitioned.error();
        }
        operation->stop_effect_in_flight = false;
      }

      transaction.fresh_scope_complete =
          std::all_of(transaction.participants.begin(), transaction.participants.end(),
                      [](const auto& participant) { return participant.terminal; });
      if (transaction.participants.size() != transaction.operations.size()) {
        transaction.fresh_scope_complete = false;
        motion_admission_closed_ = true;
      }
      if (transaction.fresh_scope_complete) {
        const bool predecessors_terminal = std::all_of(
            transaction.predecessor_tokens.begin(), transaction.predecessor_tokens.end(),
            [&](const auto& predecessor_token) {
              return predecessor_token.slot < stop_transactions_.size() &&
                     stop_transactions_[predecessor_token.slot]->active &&
                     stop_transactions_[predecessor_token.slot]->token == predecessor_token &&
                     stop_transactions_[predecessor_token.slot]->certificate.has_value();
            });
        terminal = transaction.fresh_scope_complete && predecessors_terminal;
        if (terminal) {
          // The immutable local certificate is the terminal handoff boundary.
          // Do not expose terminal state, release the emergency reserve slot,
          // or wake dependent aggregates until the complete predecessor and
          // fresh-scope truth has been reduced and retained locally.
          if (!commit_stop_terminal_locked(transaction, now)) {
            transaction.progress_in_flight = false;
            motion_admission_closed_ = true;
            return;
          }
          transaction.terminal = true;
          if (transaction.acceptance_projection_state !=
              EmergencyStopProjectionStateDTO::kPending) {
            terminal_projection = transaction.terminal_projection_token;
          }
          const auto terminal_record = std::find_if(
              stop_terminal_history_.begin(), stop_terminal_history_.end(),
              [&](const auto& record) {
                return record != nullptr &&
                       record->certificate.stop_id == transaction.certificate->stop_id &&
                       record->certificate.stop_generation ==
                           transaction.certificate->stop_generation;
              });
          if (terminal_record != stop_terminal_history_.end() &&
              (*terminal_record)->surveillance_state != nullptr) {
            surveillance_schedule = std::pair((*terminal_record)->certificate.stop_certificate_id,
                                              (*terminal_record)->next_surveillance_sample_ns);
          }
          for (const auto& candidate : stop_transactions_) {
            if (candidate == nullptr || !candidate->active || candidate->terminal) {
              continue;
            }
            const auto predecessor = std::find(candidate->predecessor_tokens.begin(),
                                               candidate->predecessor_tokens.end(), token);
            if (predecessor != candidate->predecessor_tokens.end()) {
              candidate->predecessor_certificates.push_back(*transaction.certificate);
              std::sort(
                  candidate->predecessor_certificates.begin(),
                  candidate->predecessor_certificates.end(),
                  [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
              candidate->predecessor_tokens.erase(predecessor);
              stop_control_executor_->ArmDeadline(candidate->token, now);
              dependents.push_back(candidate->token);
            }
          }
        }
      }
      transaction.progress_in_flight = false;
      if (terminal) {
        stop_control_executor_->CancelDeadline(token);
      } else if (next_deadline.has_value()) {
        stop_control_executor_->ArmDeadline(token, *next_deadline);
      }
    }
    if (terminal) {
      if (terminal_projection.has_value()) {
        stop_projection_worker_->Publish(*terminal_projection);
      }
      for (const auto& dependent : dependents) {
        stop_control_executor_->Publish(dependent);
      }
      if (surveillance_schedule.has_value()) {
        ScheduleStopSurveillance(std::move(surveillance_schedule->first),
                                 surveillance_schedule->second);
      }
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (token.locally_valid() && token.slot < stop_transactions_.size()) {
      auto& transaction = *stop_transactions_[token.slot];
      if (transaction.active && transaction.token == token) {
        transaction.progress_in_flight = false;
      }
    }
  }
}

Result<RuntimeScaleAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale(
    const RuntimeScaleRequestDTO& request) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() || !std::isfinite(request.scale) ||
      request.scale < 0.0 || request.scale > 1.0 || !request.attribution.locally_valid()) {
    return reject<RuntimeScaleAcceptedDTO>("runtime-scale request is malformed",
                                           "runtime_scale_request_invalid",
                                           ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "runtime-scale request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::optional<ResolvedRuntimeScaleCommandDTO> resolved_command;
  RuntimeScaleAcceptedDTO accepted{request.request_context.request_id,
                                   RequestReplayStatusDTO::kAcceptedNew,
                                   request.operation_id,
                                   request.expected_operation_revision,
                                   request.scale,
                                   RuntimeScaleGeneration(0U),
                                   0};
  {
    std::lock_guard state_lock(state_mutex_);
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict", "runtime-scale request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto replay = std::find_if(
        runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
        [&](const RuntimeScaleReplayEntry& entry) {
          return entry.request.request_context.request_id == request.request_context.request_id;
        });
    if (replay != runtime_scale_replay_ledger_.end()) {
      if (replay->request != request) {
        return Result<RuntimeScaleAcceptedDTO>::Err(error(
            "request_id_conflict", "runtime-scale request ID was reused with different intent",
            "request_id_conflict"));
      }
      auto replayed = replay->accepted;
      replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(replayed));
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    });
    if (used_elsewhere) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    if (replay_ledger_.size() + control_replay_ledger_.size() + stop_replay_ledger_.size() +
            runtime_scale_replay_ledger_.size() >=
        config_.request_replay_capacity) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_ledger_full", "operation replay ledger reached its fixed bound",
                "request_ledger_full"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale names a non-resident operation",
                                             "operation_not_resident",
                                             ErrorCode::kMissingReference);
    }
    if (active_operation->state.operation_revision != request.expected_operation_revision) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale expected operation revision is stale",
                                             "operation_revision_stale");
    }
    if (active_operation->payload_kind != OperationParticipantPayloadKind::kMotion ||
        !active_operation->binding.value().control_path.has_value() ||
        !active_operation->binding.value().control_path->runtime_scale_qualification.has_value() ||
        !active_operation->runtime_scale_status.has_value()) {
      return reject<RuntimeScaleAcceptedDTO>(
          "the exact operation binding has no qualified runtime scaling",
          "runtime_scale_unsupported", ErrorCode::kUnsupportedCapability);
    }
    if (active_operation->pending_control.has_value() ||
        active_operation->runtime_scale_effect_in_flight) {
      return reject<RuntimeScaleAcceptedDTO>("another operation effect is already being admitted",
                                             "operation_control_busy");
    }
    const auto current_generation =
        active_operation->runtime_scale_status->runtime_scale_generation;
    if (current_generation != request.expected_runtime_scale_generation) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation is stale",
                                             "runtime_scale_generation_stale");
    }
    const auto state = active_operation->state.state;
    if (state != OperationStateKind::kPlanning && state != OperationStateKind::kValidating &&
        state != OperationStateKind::kPrepared && state != OperationStateKind::kDispatching &&
        state != OperationStateKind::kExecuting && state != OperationStateKind::kPaused) {
      return reject<RuntimeScaleAcceptedDTO>(
          "runtime scale is invalid for the current operation state", "runtime_scale_state_invalid",
          ErrorCode::kInvalidStateTransition);
    }
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation space is exhausted",
                                             "runtime_scale_generation_exhausted",
                                             ErrorCode::kInternalError);
    }

    const RuntimeScaleGeneration next_generation(current_generation.value() + 1U);
    accepted = RuntimeScaleAcceptedDTO{request.request_context.request_id,
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       request.operation_id,
                                       request.expected_operation_revision,
                                       request.scale,
                                       next_generation,
                                       clock_.now()};
    auto accepted_status = *active_operation->runtime_scale_status;
    accepted_status.runtime_scale_generation = next_generation;
    accepted_status.requested_scale = request.scale;
    accepted_status.state = RuntimeScaleApplicationState::kPending;
    accepted_status.updated_at_ns = accepted.accepted_at_ns;
    auto& participant = accepted_status.participants.front();
    participant.accepted_generation = next_generation;
    participant.commanded_scale = request.scale;
    participant.state = RuntimeScaleApplicationState::kPending;
    participant.error.reset();
    auto accepted_state = active_operation->state;
    accepted_state.runtime_scale_generation = next_generation;
    accepted_state.updated_at_ns = accepted.accepted_at_ns;
    auto durable_acceptance = journal_.RecordRuntimeScaleAccepted(
        config_.request_owner_epoch, request, accepted, accepted_state, accepted_status);
    if (!durable_acceptance.ok()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(durable_acceptance.error());
    }
    active_operation->state = std::move(accepted_state);
    active_operation->runtime_scale_status = std::move(accepted_status);
    runtime_scale_replay_ledger_.push_back(RuntimeScaleReplayEntry{request, accepted});
    engine = active_operation->engine;
    const auto& binding = active_operation->binding.value();
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    const auto& operation_participant = active_operation->state.participants.front();
    resolved_command =
        ResolvedRuntimeScaleCommandDTO{request.operation_id,
                                       request.expected_operation_revision,
                                       operation_participant.participant_id,
                                       binding.controllable_component_id,
                                       binding.command_target_id,
                                       binding.capability_id,
                                       control.control_domain_id,
                                       control.control_domain_revision,
                                       control.control_binding_id,
                                       control.control_binding_revision,
                                       operation_participant.controller_goal_id,
                                       operation_participant.controller_goal_generation,
                                       control.controller_topology_id,
                                       control.topology_revision,
                                       control.control_route_id,
                                       qualification.scaling_domain_id,
                                       qualification.coordinated_barrier_id,
                                       active_operation->resources,
                                       control.active_control_configuration_id,
                                       control.active_control_configuration_generation,
                                       current_generation,
                                       next_generation,
                                       request.scale};
    active_operation->runtime_scale_effect_in_flight = true;
  }

  const auto commanded = engine->set_runtime_scale(*resolved_command);
  if (!commanded.ok()) {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr && active_operation->runtime_scale_status.has_value() &&
        active_operation->runtime_scale_status->runtime_scale_generation ==
            accepted.runtime_scale_generation) {
      auto status = *active_operation->runtime_scale_status;
      status.state = RuntimeScaleApplicationState::kFailed;
      status.updated_at_ns = clock_.now();
      status.participants.front().state = RuntimeScaleApplicationState::kFailed;
      status.participants.front().error = commanded.error();
      auto state = active_operation->state;
      auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                   "runtime_scale_effect_rejected");
      if (durable.ok()) {
        active_operation->state = std::move(state);
        active_operation->runtime_scale_status = std::move(status);
      }
    }
  } else {
    const auto& exact = commanded.value();
    const bool acceptance_matches =
        exact.operation_id == resolved_command->operation_id &&
        exact.operation_revision == resolved_command->operation_revision &&
        exact.participant_id == resolved_command->participant_id &&
        exact.controllable_component_id == resolved_command->controllable_component_id &&
        exact.command_target_id == resolved_command->command_target_id &&
        exact.capability_id == resolved_command->capability_id &&
        exact.control_domain_id == resolved_command->control_domain_id &&
        exact.control_domain_revision == resolved_command->control_domain_revision &&
        exact.control_binding_id == resolved_command->control_binding_id &&
        exact.control_binding_revision == resolved_command->control_binding_revision &&
        exact.controller_goal_id == resolved_command->controller_goal_id &&
        exact.controller_goal_generation == resolved_command->controller_goal_generation &&
        exact.controller_topology_id == resolved_command->controller_topology_id &&
        exact.topology_revision == resolved_command->topology_revision &&
        exact.control_route_id == resolved_command->control_route_id &&
        exact.scaling_domain_id == resolved_command->scaling_domain_id &&
        exact.coordinated_barrier_id == resolved_command->coordinated_barrier_id &&
        exact.resources == resolved_command->resources &&
        exact.active_control_configuration_id ==
            resolved_command->active_control_configuration_id &&
        exact.active_control_configuration_generation ==
            resolved_command->active_control_configuration_generation &&
        exact.accepted_generation == resolved_command->requested_runtime_scale_generation &&
        std::abs(exact.commanded_scale - resolved_command->requested_scale) <= 1e-12;
    if (!acceptance_matches) {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(request.operation_id);
      if (active_operation != nullptr && active_operation->runtime_scale_status.has_value()) {
        auto status = *active_operation->runtime_scale_status;
        status.state = RuntimeScaleApplicationState::kFailed;
        status.updated_at_ns = clock_.now();
        status.participants.front().state = RuntimeScaleApplicationState::kFailed;
        status.participants.front().error =
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "participant engine returned mismatched runtime-scale acceptance",
                  "runtime_scale_acceptance_mismatch");
        auto state = active_operation->state;
        auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                     "runtime_scale_acceptance_mismatch");
        if (durable.ok()) {
          active_operation->state = std::move(state);
          active_operation->runtime_scale_status = std::move(status);
        }
      }
    } else {
      const auto observed = engine->runtime_scale_status(resolved_command->operation_id,
                                                         resolved_command->control_binding_id);
      if (!observed.ok()) {
        std::lock_guard state_lock(state_mutex_);
        auto* active_operation = find_active_operation_locked(request.operation_id);
        if (active_operation != nullptr && active_operation->runtime_scale_status.has_value()) {
          auto status = *active_operation->runtime_scale_status;
          status.state = RuntimeScaleApplicationState::kFailed;
          status.updated_at_ns = clock_.now();
          status.participants.front().state = RuntimeScaleApplicationState::kFailed;
          status.participants.front().error = observed.error();
          auto state = active_operation->state;
          auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                       "runtime_scale_status_unavailable");
          if (durable.ok()) {
            active_operation->state = std::move(state);
            active_operation->runtime_scale_status = std::move(status);
          }
        }
      } else {
        (void)apply_runtime_scale_status(engine, observed.value());
      }
    }
  }
  {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr) {
      active_operation->runtime_scale_effect_in_flight = false;
    }
  }
  return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
}

Result<void> ResourceAddressedExecutionKernel::apply_runtime_scale_status(
    IResolvedParticipantExecutionEngine* engine,
    const ResolvedRuntimeScaleStatusDTO& resolved_status) {
  std::lock_guard state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(resolved_status.operation_id);
  if (active_operation == nullptr || active_operation->engine != engine ||
      !active_operation->runtime_scale_status.has_value()) {
    return Result<void>::Ok();
  }
  auto status = *active_operation->runtime_scale_status;
  auto& participant = status.participants.front();
  const auto& binding = active_operation->binding.value();
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const bool exact_target_matches =
      resolved_status.operation_id == status.operation_id &&
      resolved_status.operation_revision == status.operation_revision &&
      resolved_status.participant_id == participant.participant_id &&
      resolved_status.controllable_component_id == participant.controllable_component_id &&
      resolved_status.command_target_id == participant.command_target_id &&
      resolved_status.capability_id == participant.capability_id &&
      resolved_status.control_domain_id == participant.control_domain_id &&
      resolved_status.control_domain_revision == participant.control_domain_revision &&
      resolved_status.control_binding_id == participant.control_binding_id &&
      resolved_status.control_binding_revision == participant.control_binding_revision &&
      resolved_status.controller_goal_id == participant.controller_goal_id &&
      resolved_status.controller_goal_generation == participant.controller_goal_generation &&
      resolved_status.controller_topology_id == participant.controller_topology_id &&
      resolved_status.topology_revision == participant.topology_revision &&
      resolved_status.control_route_id == participant.control_route_id &&
      resolved_status.scaling_domain_id == participant.scaling_domain_id &&
      resolved_status.coordinated_barrier_id == participant.coordinated_barrier_id &&
      resolved_status.resources == participant.resources &&
      resolved_status.active_control_configuration_id ==
          participant.active_control_configuration_id &&
      resolved_status.active_control_configuration_generation ==
          participant.active_control_configuration_generation &&
      resolved_status.acknowledgement_source_epoch == control.provider_runtime_epoch;
  if (!exact_target_matches) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error =
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "participant engine returned runtime-scale status for another binding",
              "runtime_scale_status_identity_mismatch");
    status.updated_at_ns = clock_.now();
    auto state = active_operation->state;
    auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                 "runtime_scale_status_identity_mismatch");
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    active_operation->state = std::move(state);
    active_operation->runtime_scale_status = std::move(status);
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation < status.runtime_scale_generation) {
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation != status.runtime_scale_generation) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine advanced an unrequested runtime-scale generation",
                              "runtime_scale_status_generation_mismatch");
    status.updated_at_ns = clock_.now();
    auto state = active_operation->state;
    auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                                 "runtime_scale_status_generation_mismatch");
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    active_operation->state = std::move(state);
    active_operation->runtime_scale_status = std::move(status);
    return Result<void>::Ok();
  }
  participant.commanded_scale = resolved_status.commanded_scale;
  participant.applied_scale = resolved_status.applied_scale;
  participant.virtual_trajectory_time_s = resolved_status.virtual_trajectory_time_s;
  participant.acknowledgement_source_epoch = resolved_status.acknowledgement_source_epoch;
  participant.acknowledgement_sequence = resolved_status.acknowledgement_sequence;
  participant.acknowledgement_timestamp_ns = resolved_status.acknowledgement_timestamp_ns;
  participant.supported = resolved_status.state != RuntimeScaleApplicationState::kRejected;
  participant.error = resolved_status.error;
  status.effective_scale = resolved_status.applied_scale;
  status.updated_at_ns = clock_.now();

  const bool stale = resolved_status.acknowledgement_timestamp_ns > 0 &&
                     status.updated_at_ns > resolved_status.acknowledgement_timestamp_ns &&
                     status.updated_at_ns - resolved_status.acknowledgement_timestamp_ns >
                         qualification.freshness_budget_ns;
  if (resolved_status.error.has_value() || stale) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    if (stale && !participant.error.has_value()) {
      participant.error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                "runtime-scale applied-state acknowledgement is stale",
                                "runtime_scale_acknowledgement_stale");
    }
  } else {
    participant.state = resolved_status.state;
    status.state = participant.state;
  }
  auto state = active_operation->state;
  auto durable = journal_.RecordOperationState(config_.request_owner_epoch, state, &status,
                                               "runtime_scale_status_observed");
  if (!durable.ok()) {
    return Result<void>::Err(durable.error());
  }
  active_operation->state = std::move(state);
  active_operation->runtime_scale_status = std::move(status);
  return Result<void>::Ok();
}

Result<RuntimeScaleStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<RuntimeScaleStatusDTO>("operation identity is empty", "operation_id_empty",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation == nullptr) {
    return reject<RuntimeScaleStatusDTO>("operation is not resident", "operation_not_resident",
                                         ErrorCode::kMissingReference);
  }
  if (!active_operation->runtime_scale_status.has_value()) {
    return reject<RuntimeScaleStatusDTO>("operation binding has no qualified runtime scaling",
                                         "runtime_scale_unsupported",
                                         ErrorCode::kUnsupportedCapability);
  }
  return Result<RuntimeScaleStatusDTO>::Ok(*active_operation->runtime_scale_status);
}

Result<OperationStateDTO> ResourceAddressedExecutionKernel::get_operation_state(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationStateDTO>("operation identity is empty", "operation_id_empty",
                                     ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation != nullptr) {
    return Result<OperationStateDTO>::Ok(active_operation->state);
  }
  const auto historical = std::find_if(
      historical_operations_.begin(), historical_operations_.end(),
      [&](const OperationStateDTO& state) { return state.operation_id == operation_id; });
  if (historical != historical_operations_.end()) {
    return Result<OperationStateDTO>::Ok(*historical);
  }
  return reject<OperationStateDTO>("operation is not resident or retained",
                                   "operation_not_resident", ErrorCode::kMissingReference);
}

Result<void> ResourceAddressedExecutionKernel::apply_subordinate_state(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    std::uint64_t subordinate_id, OperationStateKind state, double progress,
    std::optional<ErrorDTO> terminal_error) {
  std::lock_guard state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation == nullptr || active_operation->engine != engine) {
    return Result<void>::Ok();
  }
  if (state == OperationStateKind::kPrepared &&
      (active_operation->pending_control == OperationControlKind::kReset ||
       active_operation->pending_control == OperationControlKind::kAcknowledgeCompleted)) {
    // The subordinate Idle transition confirms the settling effect. The
    // durable owner retains the terminal state named by the accepted control;
    // it does not republish that private FSM reset as a new active operation.
    return Result<void>::Ok();
  }
  if (terminal(active_operation->state.state) && state != active_operation->state.state) {
    // A settled participant cannot be revived by late subordinate feedback.
    return Result<void>::Ok();
  }
  if (active_operation->state.state == OperationStateKind::kStopping &&
      active_operation->state.active_stop_id.has_value() &&
      state != OperationStateKind::kStopping && state != OperationStateKind::kStopped &&
      state != OperationStateKind::kFailed && state != OperationStateKind::kInterrupted &&
      state != OperationStateKind::kInterventionRequired) {
    // Once Stop owns the terminal boundary, stale progress, Pause, or natural
    // completion cannot steal its outcome. Containment evidence may still
    // settle to one of the explicit Stop/failure outcomes.
    return Result<void>::Ok();
  }
  if (active_operation->payload_kind == OperationParticipantPayloadKind::kMotion) {
    if (subordinate_id == 0U) {
      return reject<void>("subordinate execution identity is empty",
                          "subordinate_execution_id_empty", ErrorCode::kInvariantViolation);
    }
    // The execute result establishes event ownership. Events observed before
    // that result are ignored rather than guessed from an engine-global slot;
    // reconcile_subordinate_state() closes the resulting observation window.
    if (active_operation->subordinate_id == 0U ||
        active_operation->subordinate_id != subordinate_id) {
      return Result<void>::Ok();
    }
  }

  auto desired_slot = slot_state_for(state);
  if (state == OperationStateKind::kStopping &&
      active_operation->pending_control == OperationControlKind::kPause) {
    // Pause retains the reservation as dispatched while the subordinate
    // mechanism reaches its controlled Paused state. ResourceSlot::kStopping
    // is reserved for terminal containment/Reset paths.
    desired_slot.reset();
  }
  const auto desired_slot_value = desired_slot.value_or(active_operation->slot_state);
  const bool slot_changed =
      desired_slot.has_value() && active_operation->slot_state != desired_slot_value;
  const auto& participant = active_operation->state.participants.front();
  const double bounded_progress = std::clamp(progress, 0.0, 1.0);
  const bool failure_terminal = state == OperationStateKind::kFailed ||
                                state == OperationStateKind::kStopped ||
                                state == OperationStateKind::kInterrupted ||
                                state == OperationStateKind::kInterventionRequired;
  if (failure_terminal) {
    const bool generic_subordinate_error =
        terminal_error.has_value() && terminal_error->details.contains("reason") &&
        terminal_error->details.at("reason") == "subordinate_terminal_error";
    if ((!terminal_error.has_value() || generic_subordinate_error) &&
        active_operation->last_subordinate_error.has_value()) {
      terminal_error = active_operation->last_subordinate_error;
    }
  }
  const bool same_terminal_error =
      participant.terminal_error.has_value() == terminal_error.has_value() &&
      (!terminal_error.has_value() ||
       (participant.terminal_error->code == terminal_error->code &&
        participant.terminal_error->message == terminal_error->message &&
        participant.terminal_error->details == terminal_error->details));
  if (!slot_changed && active_operation->state.state == state && participant.state == state &&
      participant.progress == bounded_progress && (!terminal(state) || same_terminal_error)) {
    return Result<void>::Ok();
  }

  auto observed_state = active_operation->state;
  auto& observed_participant = observed_state.participants.front();
  observed_state.state = state;
  observed_participant.state = state;
  observed_participant.progress = bounded_progress;
  if (terminal(state)) {
    observed_participant.terminal_error = std::move(terminal_error);
  }
  observed_state.updated_at_ns = clock_.now();
  auto durable_state = journal_.RecordOperationState(
      config_.request_owner_epoch, observed_state,
      active_operation->runtime_scale_status.has_value() ? &*active_operation->runtime_scale_status
                                                         : nullptr,
      "subordinate_state_observed");
  if (!durable_state.ok()) {
    return Result<void>::Err(durable_state.error());
  }
  if (slot_changed) {
    auto transition = resource_slots_->transition(active_operation->slot_owner,
                                                  active_operation->resources, desired_slot_value);
    if (!transition.ok()) {
      auto intervention = observed_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().state = OperationStateKind::kInterventionRequired;
      intervention.participants.front().terminal_error = transition.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          journal_.RecordOperationState(config_.request_owner_epoch, intervention,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "resource_transition_failed_after_state_observation");
      if (durable_failure.ok()) {
        active_operation->state = std::move(intervention);
      }
      return transition;
    }
    active_operation->slot_state = desired_slot_value;
  }
  active_operation->state = std::move(observed_state);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::on_engine_event(IResolvedParticipantExecutionEngine* engine,
                                                       const KernelEvent& event) {
  if (event.type == KernelEventType::kErrorRaised) {
    if (!event.error.has_value()) {
      return;
    }
    std::lock_guard state_lock(state_mutex_);
    const auto operation_id = find_event_operation_locked(engine, event.execution_id);
    if (operation_id.has_value()) {
      auto* active_operation = find_active_operation_locked(*operation_id);
      if (active_operation != nullptr) {
        active_operation->last_subordinate_error = event.error;
      }
    }
    return;
  }
  if (event.type != KernelEventType::kStateTransition &&
      event.type != KernelEventType::kExecuteStarted &&
      event.type != KernelEventType::kExecuteCompleted) {
    return;
  }
  auto state = event.type == KernelEventType::kExecuteStarted     ? OperationStateKind::kExecuting
               : event.type == KernelEventType::kExecuteCompleted ? OperationStateKind::kCompleted
                                                                  : operation_state(event.to_state);
  std::optional<ErrorDTO> terminal_error = event.error;
  if (!terminal_error.has_value() &&
      (state == OperationStateKind::kFailed || state == OperationStateKind::kStopped ||
       state == OperationStateKind::kInterventionRequired)) {
    const auto status = engine->get_status();
    terminal_error = status_error(status.last_error);
  }
  std::optional<OperationId> operation_id;
  {
    std::lock_guard state_lock(state_mutex_);
    operation_id = find_event_operation_locked(engine, event.execution_id);
  }
  if (!operation_id.has_value()) {
    return;
  }
  const auto status = engine->get_status();
  (void)apply_subordinate_state(*operation_id, engine, event.execution_id, state, status.progress,
                                std::move(terminal_error));
}

Result<void> ResourceAddressedExecutionKernel::reconcile_subordinate_state() {
  struct ReconcileTarget final {
    OperationId operation_id;
    IResolvedParticipantExecutionEngine* engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t subordinate_id{};
    std::uint64_t subordinate_generation{};
    std::string command_target;
    OperationStateKind public_state{OperationStateKind::kPrepared};
    std::optional<ControlBindingId> runtime_scale_control_binding_id;
  };
  std::vector<ReconcileTarget> targets;
  {
    std::lock_guard state_lock(state_mutex_);
    targets.reserve(active_operations_.size());
    for (const auto& [operation_id, operation] : active_operations_) {
      // Operation controls own their subordinate transition until their
      // accepted effect returns. A scheduler pass skips, rather than waits on,
      // that exact operation; disjoint operations still reconcile. The claim
      // also coalesces concurrent scheduler calls without holding a lock over
      // subordinate I/O.
      if (operation->pending_control.has_value() || operation->reconciliation_in_flight ||
          operation->runtime_scale_effect_in_flight || operation->stop_effect_in_flight) {
        continue;
      }
      operation->reconciliation_in_flight = true;
      std::optional<ControlBindingId> runtime_scale_control_binding_id;
      if (operation->runtime_scale_status.has_value()) {
        runtime_scale_control_binding_id =
            operation->binding.value().control_path->control_binding_id;
      }
      targets.push_back(ReconcileTarget{
          operation_id, operation->engine, operation->payload_kind, operation->subordinate_id,
          operation->subordinate_generation, operation->binding.value().command_target_id.value(),
          operation->state.state, std::move(runtime_scale_control_binding_id)});
    }
  }

  std::optional<ErrorDTO> first_error;
  for (const auto& target : targets) {
    Result<void> applied = Result<void>::Ok();
    if (target.payload_kind == OperationParticipantPayloadKind::kMotion) {
      const auto status = target.engine->get_status();
      std::optional<ErrorDTO> terminal_error;
      const auto state = operation_state(status.state);
      if (state == OperationStateKind::kFailed || state == OperationStateKind::kStopped ||
          state == OperationStateKind::kInterventionRequired) {
        terminal_error = status_error(status.last_error);
      }
      applied = apply_subordinate_state(target.operation_id, target.engine, target.subordinate_id,
                                        state, status.progress, std::move(terminal_error));
      if (applied.ok() && target.runtime_scale_control_binding_id.has_value()) {
        const auto scale_status = target.engine->runtime_scale_status(
            target.operation_id, *target.runtime_scale_control_binding_id);
        if (scale_status.ok()) {
          applied = apply_runtime_scale_status(target.engine, scale_status.value());
        } else {
          std::lock_guard state_lock(state_mutex_);
          auto* active_operation = find_active_operation_locked(target.operation_id);
          if (active_operation != nullptr && active_operation->runtime_scale_status.has_value()) {
            auto public_status = *active_operation->runtime_scale_status;
            public_status.state = RuntimeScaleApplicationState::kFailed;
            public_status.updated_at_ns = clock_.now();
            public_status.participants.front().state = RuntimeScaleApplicationState::kFailed;
            public_status.participants.front().error = scale_status.error();
            auto operation_state = active_operation->state;
            auto durable = journal_.RecordOperationState(config_.request_owner_epoch,
                                                         operation_state, &public_status,
                                                         "runtime_scale_reconcile_unavailable");
            if (!durable.ok()) {
              applied = Result<void>::Err(durable.error());
            } else {
              active_operation->state = std::move(operation_state);
              active_operation->runtime_scale_status = std::move(public_status);
            }
          }
        }
      }
    } else {
      const auto status = target.engine->get_gripper_status(target.command_target);
      switch (status.state) {
        case GripperCommandState::kIdle:
          if (target.public_state == OperationStateKind::kStopped &&
              status.command_id == target.subordinate_id &&
              status.generation == target.subordinate_generation) {
            applied = Result<void>::Ok();
          } else {
            applied = apply_subordinate_state(
                target.operation_id, target.engine, target.subordinate_id,
                OperationStateKind::kInterventionRequired, status.progress,
                error("invariant_violation",
                      "accepted gripper command became idle without terminal evidence",
                      "gripper_command_lost"));
          }
          break;
        case GripperCommandState::kExecuting:
          applied = apply_subordinate_state(target.operation_id, target.engine,
                                            target.subordinate_id, OperationStateKind::kExecuting,
                                            status.progress, std::nullopt);
          break;
        case GripperCommandState::kCompleted:
          applied = apply_subordinate_state(target.operation_id, target.engine,
                                            target.subordinate_id, OperationStateKind::kCompleted,
                                            status.progress, std::nullopt);
          break;
        case GripperCommandState::kFailed:
          applied = apply_subordinate_state(
              target.operation_id, target.engine, target.subordinate_id,
              OperationStateKind::kFailed, status.progress,
              status.error.value_or(error("execution_aborted", "gripper command failed",
                                          "gripper_command_failed_without_error")));
          break;
      }
    }
    if (!applied.ok() && !first_error.has_value()) {
      first_error = applied.error();
    }
    {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(target.operation_id);
      if (active_operation != nullptr) {
        active_operation->reconciliation_in_flight = false;
      }
    }
  }
  return first_error.has_value() ? Result<void>::Err(*first_error) : Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceAddressedExecutionKernel::resource_snapshot() const {
  return resource_slots_->snapshot();
}

}  // namespace manyforge::core
