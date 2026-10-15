#include <algorithm>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

using EdgeKey = std::pair<ControllerRoleId, ControllerRoleId>;
using RoleSet = std::set<ControllerRoleId>;
using EdgeSet = std::set<EdgeKey>;

template <typename T>
[[nodiscard]] Result<T> reject(std::string reason, std::string message) {
  return Result<T>::Err(ErrorDTO{
      "controller_lifecycle_invalid", std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] EdgeKey edge_key(const ControllerChainEdgeRefDTO& edge) {
  return {edge.upstream_controller_role_id, edge.downstream_controller_role_id};
}

[[nodiscard]] EdgeKey edge_key(const ControllerChainEdgeDTO& edge) {
  return {edge.upstream_controller_role_id, edge.downstream_controller_role_id};
}

[[nodiscard]] ControllerChainEdgeRefDTO edge_ref(const EdgeKey& edge) {
  return {edge.first, edge.second};
}

[[nodiscard]] const ControllerActivationSetDTO* find_selected_activation(
    const ControllerLifecycleExpectedDomainDTO& expected) noexcept {
  const ControllerActivationSetDTO* result = nullptr;
  for (const auto& activation : expected.controller_topology.activation_sets) {
    if (activation.activation_set_id == expected.desired_configuration.activation_set_id) {
      if (result != nullptr) {
        return nullptr;
      }
      result = &activation;
    }
  }
  return result;
}

[[nodiscard]] const ControllerActivationSetDTO* find_activation(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ActivationSetId& activation_set_id) noexcept {
  const ControllerActivationSetDTO* result = nullptr;
  for (const auto& activation : expected.controller_topology.activation_sets) {
    if (activation.activation_set_id != activation_set_id) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &activation;
  }
  return result;
}

[[nodiscard]] bool route_is_expected(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ActiveControlConfigurationBindingRouteDTO& observed_route,
    const ActivationSetId& activation_set_id) noexcept {
  const auto binding = std::find_if(
      expected.expected_bindings.begin(), expected.expected_bindings.end(),
      [&](const auto& candidate) {
        return candidate.control_binding_id == observed_route.control_binding_id &&
               candidate.control_binding_revision == observed_route.control_binding_revision;
      });
  if (binding == expected.expected_bindings.end()) {
    return false;
  }
  const auto route =
      std::find_if(expected.controller_topology.legal_routes.begin(),
                   expected.controller_topology.legal_routes.end(), [&](const auto& candidate) {
                     return candidate.control_route_id == observed_route.control_route_id;
                   });
  return route != expected.controller_topology.legal_routes.end() &&
         route->control_interface_contract_id == binding->control_interface_contract_id &&
         route->control_interface_revision == binding->control_interface_revision &&
         std::find(route->compatible_activation_set_ids.begin(),
                   route->compatible_activation_set_ids.end(),
                   activation_set_id) != route->compatible_activation_set_ids.end() &&
         std::any_of(expected.expected_endpoints.begin(), expected.expected_endpoints.end(),
                     [&](const auto& endpoint) {
                       return endpoint.control_binding_id == observed_route.control_binding_id &&
                              endpoint.control_binding_revision ==
                                  observed_route.control_binding_revision &&
                              endpoint.control_route_id == observed_route.control_route_id;
                     });
}

[[nodiscard]] Result<void> verify_topology(const ControllerLifecycleExpectedDomainDTO& expected) {
  RoleSet roles;
  for (const auto& controller : expected.controller_topology.controllers) {
    if (controller.controller_role_id.empty() ||
        !roles.insert(controller.controller_role_id).second) {
      return reject<void>("controller_role_duplicate_or_empty",
                          "controller topology roles must be complete and unique");
    }
  }

  EdgeSet edges;
  std::map<ControllerRoleId, std::size_t> indegree;
  std::map<ControllerRoleId, std::vector<ControllerRoleId>> downstream;
  for (const auto& role : roles) {
    indegree.emplace(role, 0U);
    downstream.emplace(role, std::vector<ControllerRoleId>{});
  }
  for (const auto& edge : expected.controller_topology.chain_edges) {
    const auto key = edge_key(edge);
    if (key.first == key.second || !roles.contains(key.first) || !roles.contains(key.second) ||
        !edges.insert(key).second) {
      return reject<void>("controller_chain_edge_invalid",
                          "controller topology contains an unknown or duplicate chain edge");
    }
    ++indegree.at(key.second);
    downstream.at(key.first).push_back(key.second);
  }

  RoleSet ready;
  for (const auto& [role, count] : indegree) {
    if (count == 0U) {
      ready.insert(role);
    }
  }
  std::size_t visited = 0U;
  while (!ready.empty()) {
    const auto role = *ready.begin();
    ready.erase(ready.begin());
    ++visited;
    for (const auto& child : downstream.at(role)) {
      auto& count = indegree.at(child);
      --count;
      if (count == 0U) {
        ready.insert(child);
      }
    }
  }
  if (visited != roles.size()) {
    return reject<void>("controller_topology_cycle",
                        "controller topology contains a dependency cycle");
  }

  const auto* selected = find_selected_activation(expected);
  if (selected == nullptr || (selected->active_controller_role_ids.empty() &&
      expected.physical_expectation != ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone)) {
    return reject<void>("desired_activation_set_missing",
                        "desired activation set is missing or ambiguous");
  }
  RoleSet selected_roles;
  for (const auto& role : selected->active_controller_role_ids) {
    if (!roles.contains(role) || !selected_roles.insert(role).second) {
      return reject<void>("desired_activation_role_invalid",
                          "desired activation set contains an unknown or duplicate role");
    }
  }
  EdgeSet selected_edges;
  for (const auto& edge : selected->active_chain_edges) {
    const auto key = edge_key(edge);
    if (!edges.contains(key) || !selected_roles.contains(key.first) ||
        !selected_roles.contains(key.second) || !selected_edges.insert(key).second) {
      return reject<void>("desired_activation_edge_invalid",
                          "desired activation set contains an invalid chain edge");
    }
  }
  return Result<void>::Ok();
}

[[nodiscard]] Result<void> verify_exact_observation(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ControllerLifecycleObservationDTO& observation, std::int64_t now_ns) {
  if (!expected.locally_valid()) {
    return reject<void>("expected_closure_invalid",
                        "controller lifecycle expected closure is invalid");
  }
  if (!observation.locally_valid()) {
    return reject<void>("observation_invalid", "controller lifecycle observation is invalid");
  }
  if (now_ns < 0 || now_ns < observation.received_at_ns || now_ns >= observation.valid_until_ns) {
    return reject<void>("observation_not_fresh",
                        "controller lifecycle observation is expired or future-dated");
  }
  if (observation.query != MakeControllerLifecycleObservationQuery(expected)) {
    return reject<void>("observation_identity_mismatch",
                        "controller lifecycle observation does not echo the expected identity");
  }
  const auto topology = verify_topology(expected);
  if (!topology.ok()) {
    return topology;
  }
  if (observation.controllers.size() != expected.expected_controllers.size() ||
      observation.endpoints.size() != expected.expected_endpoints.size()) {
    return reject<void>("observation_incomplete",
                        "controller lifecycle observation is not the complete expected closure");
  }
  for (std::size_t index = 0U; index < expected.expected_controllers.size(); ++index) {
    if (observation.controllers[index].expected_controller !=
        expected.expected_controllers[index]) {
      return reject<void>("observed_controller_mismatch",
                          "observed controller projection differs from the retained closure");
    }
  }
  for (std::size_t index = 0U; index < expected.expected_endpoints.size(); ++index) {
    if (observation.endpoints[index].expected_endpoint != expected.expected_endpoints[index]) {
      return reject<void>("observed_endpoint_mismatch",
                          "observed endpoint projection differs from the retained closure");
    }
  }

  EdgeSet topology_edges;
  for (const auto& edge : expected.controller_topology.chain_edges) {
    topology_edges.insert(edge_key(edge));
  }
  std::map<ControllerRoleId, ControllerLifecycleActivityDTO> activities;
  for (const auto& controller : observation.controllers) {
    activities.emplace(controller.expected_controller.controller_role_id, controller.activity);
  }
  for (const auto& edge : observation.active_chain_edges) {
    const auto key = edge_key(edge);
    const auto upstream = activities.find(key.first);
    const auto downstream = activities.find(key.second);
    if (!topology_edges.contains(key) || upstream == activities.end() ||
        downstream == activities.end() ||
        upstream->second != ControllerLifecycleActivityDTO::kActive ||
        downstream->second != ControllerLifecycleActivityDTO::kActive) {
      return reject<void>("observed_active_edge_invalid",
                          "observed active edge is not an active edge of the exact topology");
    }
  }
  if (observation.active_configuration.has_value()) {
    const auto& active = *observation.active_configuration;
    const auto* activation = find_activation(expected, active.activation_set_id);
    if (activation == nullptr ||
        active.binding_routes.size() != expected.expected_bindings.size() ||
        !std::all_of(active.binding_routes.begin(), active.binding_routes.end(),
                     [&](const auto& route) {
                       return route_is_expected(expected, route, active.activation_set_id);
                     })) {
      return reject<void>("observed_configuration_invalid",
                          "observed configuration is not a complete legal retained configuration");
    }
  }
  return Result<void>::Ok();
}

[[nodiscard]] std::vector<ControllerRoleId> sorted_roles(const RoleSet& roles) {
  return {roles.begin(), roles.end()};
}

[[nodiscard]] std::vector<ControllerChainEdgeRefDTO> sorted_edges(const EdgeSet& edges) {
  std::vector<ControllerChainEdgeRefDTO> result;
  result.reserve(edges.size());
  for (const auto& edge : edges) {
    result.push_back(edge_ref(edge));
  }
  return result;
}

[[nodiscard]] Result<std::vector<ControllerLifecycleTransitionStepDTO>> make_waves(
    const RoleSet& selected_roles, const EdgeSet& topology_edges,
    ControllerLifecycleTransitionKindDTO kind) {
  RoleSet remaining = selected_roles;
  std::vector<ControllerLifecycleTransitionStepDTO> steps;
  while (!remaining.empty()) {
    std::vector<ControllerRoleId> wave;
    for (const auto& role : remaining) {
      const bool blocked =
          std::any_of(topology_edges.begin(), topology_edges.end(), [&](const EdgeKey& edge) {
            if (kind == ControllerLifecycleTransitionKindDTO::kDeactivate) {
              return edge.second == role && remaining.contains(edge.first);
            }
            return edge.first == role && remaining.contains(edge.second);
          });
      if (!blocked) {
        wave.push_back(role);
      }
    }
    if (wave.empty()) {
      return reject<std::vector<ControllerLifecycleTransitionStepDTO>>(
          "controller_topology_cycle", "controller transition dependencies contain a cycle");
    }
    for (const auto& role : wave) {
      remaining.erase(role);
    }
    steps.push_back({kind, std::move(wave)});
  }
  return Result<std::vector<ControllerLifecycleTransitionStepDTO>>::Ok(std::move(steps));
}

}  // namespace

Result<ControllerLifecycleReconciliationDTO> ReconcileControllerLifecycle(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ControllerLifecycleObservationDTO& observation, std::int64_t now_ns) {
  const auto verified = verify_exact_observation(expected, observation, now_ns);
  if (!verified.ok()) {
    return Result<ControllerLifecycleReconciliationDTO>::Err(verified.error());
  }

  const auto* desired = find_selected_activation(expected);
  RoleSet desired_roles(desired->active_controller_role_ids.begin(),
                        desired->active_controller_role_ids.end());
  EdgeSet desired_edges;
  for (const auto& edge : desired->active_chain_edges) {
    desired_edges.insert(edge_key(edge));
  }
  EdgeSet observed_edges;
  for (const auto& edge : observation.active_chain_edges) {
    observed_edges.insert(edge_key(edge));
  }

  std::set<std::string> reasons;
  if (!observation.active_configuration.has_value()) {
    reasons.insert("active_configuration_unavailable");
  } else {
    std::vector<ActiveControlConfigurationBindingRouteDTO> desired_routes;
    desired_routes.reserve(expected.desired_configuration.binding_routes.size());
    for (const auto& route : expected.desired_configuration.binding_routes) {
      desired_routes.push_back(
          {route.control_binding_id, route.control_binding_revision, route.control_route_id});
    }
    if (observation.active_configuration->activation_set_id !=
            expected.desired_configuration.activation_set_id ||
        observation.active_configuration->binding_routes != desired_routes) {
      reasons.insert("active_configuration_mismatch");
    }
  }
  for (const auto& controller : observation.controllers) {
    const auto role = controller.expected_controller.controller_role_id;
    const auto required = desired_roles.contains(role) ? ControllerLifecycleActivityDTO::kActive
                                                       : ControllerLifecycleActivityDTO::kInactive;
    if (controller.activity != required) {
      reasons.insert("controller_activity_mismatch");
    }
  }
  if (observed_edges != desired_edges) {
    reasons.insert("active_chain_edges_mismatch");
  }
  if (std::any_of(
          observation.endpoints.begin(), observation.endpoints.end(), [&](const auto& endpoint) {
            const auto selected = std::find_if(
                expected.desired_configuration.binding_routes.begin(),
                expected.desired_configuration.binding_routes.end(), [&](const auto& route) {
                  return route.control_binding_id ==
                             endpoint.expected_endpoint.control_binding_id &&
                         route.control_binding_revision ==
                             endpoint.expected_endpoint.control_binding_revision &&
                         route.control_route_id == endpoint.expected_endpoint.control_route_id;
                });
            return selected != expected.desired_configuration.binding_routes.end() &&
                   !endpoint.ready;
          })) {
    reasons.insert("endpoint_not_ready");
  }

  std::vector<std::string> reason_codes(reasons.begin(), reasons.end());
  ControllerLifecycleReconciliationDTO result{
      observation.query,
      observation.source_sequence,
      reason_codes.empty()
          ? ControllerLifecycleReconciliationDispositionDTO::kReconciled
          : ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled,
      expected.desired_configuration.activation_set_id,
      expected.desired_configuration.binding_routes,
      std::move(reason_codes),
  };
  if (!result.locally_valid()) {
    return reject<ControllerLifecycleReconciliationDTO>(
        "reconciliation_result_invalid", "controller lifecycle reconciliation result is invalid");
  }
  return Result<ControllerLifecycleReconciliationDTO>::Ok(std::move(result));
}

Result<ControllerLifecycleTransitionPlanDTO> BuildControllerLifecycleTransitionPlan(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const ControllerLifecycleObservationDTO& observation, std::int64_t now_ns) {
  const auto verified = verify_exact_observation(expected, observation, now_ns);
  if (!verified.ok()) {
    return Result<ControllerLifecycleTransitionPlanDTO>::Err(verified.error());
  }

  RoleSet observed_active_roles;
  for (const auto& controller : observation.controllers) {
    if (controller.activity != ControllerLifecycleActivityDTO::kInactive &&
        controller.activity != ControllerLifecycleActivityDTO::kActive) {
      return reject<ControllerLifecycleTransitionPlanDTO>(
          "controller_activity_not_plannable",
          "transition planning requires exact inactive or active controller activity");
    }
    if (controller.activity == ControllerLifecycleActivityDTO::kActive) {
      observed_active_roles.insert(controller.expected_controller.controller_role_id);
    }
  }

  const auto* desired = find_selected_activation(expected);
  RoleSet desired_roles(desired->active_controller_role_ids.begin(),
                        desired->active_controller_role_ids.end());
  EdgeSet observed_edges;
  for (const auto& edge : observation.active_chain_edges) {
    observed_edges.insert(edge_key(edge));
  }
  EdgeSet desired_edges;
  for (const auto& edge : desired->active_chain_edges) {
    desired_edges.insert(edge_key(edge));
  }
  EdgeSet topology_edges;
  for (const auto& edge : expected.controller_topology.chain_edges) {
    topology_edges.insert(edge_key(edge));
  }

  RoleSet deactivate;
  std::set_difference(observed_active_roles.begin(), observed_active_roles.end(),
                      desired_roles.begin(), desired_roles.end(),
                      std::inserter(deactivate, deactivate.end()));
  EdgeSet changed_edges;
  std::set_symmetric_difference(observed_edges.begin(), observed_edges.end(), desired_edges.begin(),
                                desired_edges.end(),
                                std::inserter(changed_edges, changed_edges.end()));
  for (const auto& edge : changed_edges) {
    if (observed_active_roles.contains(edge.first)) {
      deactivate.insert(edge.first);
    }
  }

  RoleSet remaining_active = observed_active_roles;
  for (const auto& role : deactivate) {
    remaining_active.erase(role);
  }
  RoleSet activate;
  std::set_difference(desired_roles.begin(), desired_roles.end(), remaining_active.begin(),
                      remaining_active.end(), std::inserter(activate, activate.end()));

  const auto deactivation_steps =
      make_waves(deactivate, topology_edges, ControllerLifecycleTransitionKindDTO::kDeactivate);
  if (!deactivation_steps.ok()) {
    return Result<ControllerLifecycleTransitionPlanDTO>::Err(deactivation_steps.error());
  }
  const auto activation_steps =
      make_waves(activate, topology_edges, ControllerLifecycleTransitionKindDTO::kActivate);
  if (!activation_steps.ok()) {
    return Result<ControllerLifecycleTransitionPlanDTO>::Err(activation_steps.error());
  }

  std::vector<ControllerLifecycleTransitionStepDTO> steps = deactivation_steps.value();
  steps.insert(steps.end(), activation_steps.value().begin(), activation_steps.value().end());
  if (steps.size() > kMaxControllerLifecycleTransitionSteps) {
    return reject<ControllerLifecycleTransitionPlanDTO>(
        "controller_transition_plan_over_bound",
        "controller transition plan exceeds its closed wave bound");
  }

  ControllerLifecycleTransitionPlanDTO result{
      observation.query,
      observation.source_sequence,
      sorted_roles(observed_active_roles),
      sorted_edges(observed_edges),
      desired->activation_set_id,
      sorted_roles(desired_roles),
      sorted_edges(desired_edges),
      std::move(steps),
  };
  if (!result.locally_valid()) {
    return reject<ControllerLifecycleTransitionPlanDTO>(
        "controller_transition_plan_invalid",
        "controller transition plan failed closed local validation");
  }
  return Result<ControllerLifecycleTransitionPlanDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
