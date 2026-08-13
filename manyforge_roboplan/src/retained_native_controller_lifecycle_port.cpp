#include <algorithm>
#include <limits>
#include <manyforge_roboplan/retained_native_controller_lifecycle_port.hpp>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

template <typename T>
[[nodiscard]] core::Result<T> reject(std::string reason, std::string message) {
  return core::Result<T>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool selected_route(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::ControllerLifecycleExpectedEndpointDTO &endpoint) noexcept {
  return std::any_of(
      expected.desired_configuration.binding_routes.begin(),
      expected.desired_configuration.binding_routes.end(),
      [&](const auto &route) {
        return route.control_binding_id == endpoint.control_binding_id &&
               route.control_binding_revision ==
                   endpoint.control_binding_revision &&
               route.control_route_id == endpoint.control_route_id;
      });
}

[[nodiscard]] const core::ControllerActivationSetDTO *selected_activation(
    const core::ControllerLifecycleExpectedDomainDTO &expected) noexcept {
  const core::ControllerActivationSetDTO *result = nullptr;
  for (const auto &activation : expected.controller_topology.activation_sets) {
    if (activation.activation_set_id !=
        expected.desired_configuration.activation_set_id) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &activation;
  }
  return result;
}

[[nodiscard]] std::set<core::ResourceId>
binding_resources(const core::ControllerLifecycleExpectedBindingDTO &binding) {
  std::set<core::ResourceId> result;
  for (const auto &interface : binding.claimed_command_interfaces) {
    result.insert(interface.resource_id);
  }
  for (const auto &interface : binding.claimed_state_interfaces) {
    result.insert(interface.resource_id);
  }
  return result;
}

[[nodiscard]] std::set<core::ResourceId>
participant_resources(const core::RuntimeParticipantBindingDTO &binding) {
  std::set<core::ResourceId> result;
  for (const auto &resource : binding.resources) {
    result.insert(resource.resource_id);
  }
  return result;
}

[[nodiscard]] bool exact_binding_match(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::ControllerLifecycleExpectedBindingDTO &expected_binding,
    const core::DesiredControlBindingRouteDTO &desired_route,
    const core::RuntimeParticipantBindingDTO &physical) {
  if (!physical.control_path.has_value()) {
    return false;
  }
  const auto &path = *physical.control_path;
  return physical.retained_bundle_id.value().size() > 0U &&
         physical.graph_revision.value().size() > 0U &&
         path.control_domain_id == expected.control_domain_id &&
         path.control_domain_revision == expected.control_domain_revision &&
         path.control_binding_id == expected_binding.control_binding_id &&
         path.control_binding_revision ==
             expected_binding.control_binding_revision &&
         path.control_interface_contract_id ==
             expected_binding.control_interface_contract_id &&
         path.control_interface_revision ==
             expected_binding.control_interface_revision &&
         path.controller_topology_id == expected.controller_topology_id &&
         path.topology_revision == expected.topology_revision &&
         path.control_route_id == desired_route.control_route_id &&
         path.activation_set_id ==
             expected.desired_configuration.activation_set_id &&
         path.provider_runtime_unit_id == expected.source_runtime_unit_id &&
         path.provider_runtime_epoch == expected.source_runtime_epoch &&
         participant_resources(physical) == binding_resources(expected_binding);
}

[[nodiscard]] core::Result<void> verify_fixed_direct_runtime(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  const auto *activation = selected_activation(expected);
  if (!expected.locally_valid() || activation == nullptr ||
      expected.controller_topology.controllers.size() != 1U ||
      expected.expected_controllers.size() != 1U ||
      activation->active_controller_role_ids.size() != 1U ||
      activation->active_controller_role_ids.front() !=
          expected.expected_controllers.front().controller_role_id ||
      !activation->active_chain_edges.empty() ||
      !expected.controller_topology.chain_edges.empty()) {
    return reject<void>("native_controller_topology_not_fixed_direct",
                        "native controller lifecycle adapter requires one "
                        "immutable direct controller");
  }
  if (physical_bindings.size() != expected.expected_bindings.size()) {
    return reject<void>("native_controller_binding_coverage_incomplete",
                        "native controller lifecycle adapter requires one "
                        "physical representative per binding");
  }

  std::set<std::pair<core::ControlBindingId, core::ControlBindingRevision>>
      matched;
  for (const auto &expected_binding : expected.expected_bindings) {
    const auto desired =
        std::find_if(expected.desired_configuration.binding_routes.begin(),
                     expected.desired_configuration.binding_routes.end(),
                     [&](const auto &route) {
                       return route.control_binding_id ==
                                  expected_binding.control_binding_id &&
                              route.control_binding_revision ==
                                  expected_binding.control_binding_revision;
                     });
    if (desired == expected.desired_configuration.binding_routes.end()) {
      return reject<void>(
          "native_controller_desired_binding_missing",
          "native controller binding has no exact desired route");
    }
    std::size_t matches = 0U;
    for (const auto &physical : physical_bindings) {
      if (exact_binding_match(expected, expected_binding, *desired,
                              physical.value())) {
        ++matches;
      }
    }
    if (matches != 1U ||
        !matched
             .emplace(expected_binding.control_binding_id,
                      expected_binding.control_binding_revision)
             .second) {
      return reject<void>("native_controller_binding_identity_mismatch",
                          "native controller binding does not resolve one "
                          "exact physical representative");
    }
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] core::ErrorDTO transition_error(std::string reason,
                                              std::string message) {
  return {std::string(core::to_string(core::ErrorCode::kUnsupportedCapability)),
          std::move(message),
          {{"reason", std::move(reason)}}};
}

} // namespace

core::Result<std::shared_ptr<RetainedNativeControllerLifecyclePort>>
RetainedNativeControllerLifecyclePort::Create(
    core::ControllerLifecycleExpectedDomain expected_domain,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IClock &clock, const core::Duration freshness_duration_ns) {
  if (freshness_duration_ns <= 0) {
    return reject<std::shared_ptr<RetainedNativeControllerLifecyclePort>>(
        "native_controller_freshness_invalid",
        "native controller observation freshness must be positive");
  }
  const auto &expected = expected_domain.value();
  const auto verified =
      verify_fixed_direct_runtime(expected, physical_bindings);
  if (!verified.ok()) {
    return core::
        Result<std::shared_ptr<RetainedNativeControllerLifecyclePort>>::Err(
            verified.error());
  }

  const auto *activation = selected_activation(expected);
  std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
  routes.reserve(expected.desired_configuration.binding_routes.size());
  for (const auto &route : expected.desired_configuration.binding_routes) {
    routes.push_back({route.control_binding_id, route.control_binding_revision,
                      route.control_route_id});
  }
  std::vector<core::ControllerLifecycleObservedControllerDTO> controllers;
  controllers.reserve(expected.expected_controllers.size());
  for (const auto &controller : expected.expected_controllers) {
    controllers.push_back(
        {controller, core::ControllerLifecycleActivityDTO::kActive});
  }
  std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints;
  endpoints.reserve(expected.expected_endpoints.size());
  for (const auto &endpoint : expected.expected_endpoints) {
    endpoints.push_back({endpoint, selected_route(expected, endpoint)});
  }

  auto port = std::shared_ptr<RetainedNativeControllerLifecyclePort>(
      new RetainedNativeControllerLifecyclePort(
          std::move(expected_domain),
          {activation->activation_set_id, std::move(routes)},
          std::move(controllers), activation->active_chain_edges,
          std::move(endpoints), clock, freshness_duration_ns));
  return core::Result<std::shared_ptr<RetainedNativeControllerLifecyclePort>>::
      Ok(std::move(port));
}

RetainedNativeControllerLifecyclePort::RetainedNativeControllerLifecyclePort(
    core::ControllerLifecycleExpectedDomain expected_domain,
    core::ControllerLifecycleObservedConfigurationDTO active_configuration,
    std::vector<core::ControllerLifecycleObservedControllerDTO> controllers,
    std::vector<core::ControllerChainEdgeRefDTO> active_chain_edges,
    std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints,
    core::IClock &clock, const core::Duration freshness_duration_ns)
    : expected_domain_(std::move(expected_domain)),
      active_configuration_(std::move(active_configuration)),
      controllers_(std::move(controllers)),
      active_chain_edges_(std::move(active_chain_edges)),
      endpoints_(std::move(endpoints)), clock_(clock),
      freshness_duration_ns_(freshness_duration_ns) {}

core::Result<core::ControllerLifecycleObservationDTO>
RetainedNativeControllerLifecyclePort::Observe(
    const core::ControllerLifecycleObservationQueryDTO &query) {
  const auto exact_query =
      core::MakeControllerLifecycleObservationQuery(expected_domain_.value());
  if (!query.locally_valid() || query != exact_query) {
    return core::Result<core::ControllerLifecycleObservationDTO>::Err(
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kMissingReference)),
            "native controller lifecycle query does not name the exact "
            "retained source",
            {{"reason", "native_controller_query_mismatch"}}});
  }
  const auto now = clock_.now();
  if (now < 0 || now > std::numeric_limits<core::TimePoint>::max() -
                           freshness_duration_ns_) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "native_controller_observation_time_invalid",
        "native controller observation time cannot produce a bounded freshness "
        "window");
  }

  auto sequence = next_source_sequence_.load(std::memory_order_acquire);
  for (;;) {
    if (sequence == 0U ||
        sequence == std::numeric_limits<std::uint64_t>::max()) {
      return reject<core::ControllerLifecycleObservationDTO>(
          "native_controller_source_sequence_exhausted",
          "native controller observation sequence is exhausted");
    }
    if (next_source_sequence_.compare_exchange_weak(
            sequence, sequence + 1U, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      break;
    }
  }

  core::ControllerLifecycleObservationDTO observation{
      exact_query,
      sequence,
      now,
      now,
      now + freshness_duration_ns_,
      active_configuration_,
      controllers_,
      active_chain_edges_,
      endpoints_,
  };
  if (!observation.locally_valid()) {
    return reject<core::ControllerLifecycleObservationDTO>(
        "native_controller_observation_invalid",
        "native controller construction produced an invalid observation");
  }
  return core::Result<core::ControllerLifecycleObservationDTO>::Ok(
      std::move(observation));
}

core::ControllerLifecycleTransitionResultDTO
RetainedNativeControllerLifecyclePort::ApplyTransitionStep(
    const core::ControllerLifecycleTransitionIntentDTO &intent,
    const std::stop_token cancellation) {
  const auto exact_query =
      core::MakeControllerLifecycleObservationQuery(expected_domain_.value());
  if (!intent.locally_valid() || intent.query != exact_query) {
    return {intent,
            core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
            std::nullopt,
            transition_error(
                "native_controller_transition_invalid",
                "native controller transition intent is invalid or stale")};
  }
  if (cancellation.stop_requested()) {
    return {intent,
            core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
            std::nullopt,
            transition_error(
                "native_controller_transition_cancelled",
                "native controller transition was cancelled before dispatch")};
  }
  return {intent,
          core::ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched,
          std::nullopt,
          transition_error(
              "native_controller_topology_immutable",
              "in-process native controller topology cannot be reconfigured")};
}

} // namespace manyforge::roboplan_backend
