#include <algorithm>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <string_view>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
}

[[nodiscard]] bool error_valid(const std::optional<ErrorDTO>& error) noexcept {
  if (!error.has_value()) {
    return true;
  }
  if (error->code.empty() || error->code.size() > kMaxLifecycleErrorCodeBytes ||
      error->message.size() > kMaxLifecycleErrorMessageBytes ||
      error->details.size() > kMaxLifecycleErrorDetails) {
    return false;
  }
  return std::all_of(error->details.begin(), error->details.end(), [](const auto& detail) {
    return !detail.first.empty() && detail.first.size() <= kMaxLifecycleErrorDetailKeyBytes &&
           detail.second.size() <= kMaxLifecycleErrorDetailValueBytes;
  });
}

[[nodiscard]] bool portable_token(const std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxLifecycleFactTokenBytes) {
    return false;
  }
  const auto alphanumeric = [](const unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return alphanumeric(byte) || character == '.' || character == '_' ||
           character == '-';
  });
}

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value& left, const Value& right) {
                              return !less(left, right) && !less(right, left);
                            }) == values.end();
}

[[nodiscard]] bool known(ComponentLifecycleStateDTO value) noexcept {
  switch (value) {
    case ComponentLifecycleStateDTO::kUndeclared:
    case ComponentLifecycleStateDTO::kDeclared:
    case ComponentLifecycleStateDTO::kPrepared:
    case ComponentLifecycleStateDTO::kActive:
    case ComponentLifecycleStateDTO::kDegraded:
    case ComponentLifecycleStateDTO::kFaulted:
    case ComponentLifecycleStateDTO::kDisabling:
    case ComponentLifecycleStateDTO::kDisabled:
      return true;
  }
  return false;
}

[[nodiscard]] bool active_configuration_bounded(
    const ActiveControlConfigurationDTO& configuration) noexcept {
  if (!bounded_identity(configuration.active_control_configuration_id) ||
      !bounded_identity(configuration.control_domain_id) ||
      !bounded_identity(configuration.control_domain_revision) ||
      !bounded_identity(configuration.activation_set_id) ||
      !bounded_identity(configuration.controller_topology_id) ||
      !bounded_identity(configuration.topology_revision) ||
      !bounded_identity(configuration.source_runtime_unit_id) ||
      !bounded_identity(configuration.source_runtime_epoch) ||
      configuration.kinematic_resource_source_projections.size() >
          kMaxLifecycleKinematicSourceProjections) {
    return false;
  }
  for (const auto& route : configuration.binding_routes) {
    if (!bounded_identity(route.control_binding_id) ||
        !bounded_identity(route.control_binding_revision) ||
        !bounded_identity(route.control_route_id)) {
      return false;
    }
  }
  std::size_t contributing_count = 0U;
  for (const auto& projection : configuration.kinematic_resource_source_projections) {
    if (!bounded_identity(projection.resource_id) ||
        !bounded_identity(projection.provider_instance_id) ||
        !bounded_identity(projection.provider_manifest_id) ||
        !bounded_identity(projection.provider_manifest_revision) ||
        !bounded_identity(projection.state_authority_id) ||
        contributing_count > kMaxLifecycleKinematicContributingBindings -
                                 projection.contributing_control_bindings.size()) {
      return false;
    }
    contributing_count += projection.contributing_control_bindings.size();
    for (const auto& source : projection.contributing_control_bindings) {
      if (!bounded_identity(source.control_binding_id) ||
          !bounded_identity(source.control_binding_revision)) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool normalized_health_valid(
    ComponentLifecycleStateDTO state, const std::vector<LifecycleFactDTO>& facts,
    const std::optional<ErrorDTO>& diagnostic) noexcept {
  if (!known(state) || facts.size() > kMaxLifecycleFacts || !error_valid(diagnostic)) {
    return false;
  }
  const auto less = [](const LifecycleFactDTO& left, const LifecycleFactDTO& right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return sorted_unique(facts, less) &&
         std::all_of(facts.begin(), facts.end(),
                     [](const LifecycleFactDTO& fact) { return fact.locally_valid(); });
}

}  // namespace


bool ExpectedActiveControlConfigurationDTO::locally_valid() const noexcept {
  switch (presence) {
    case ExpectedActiveControlConfigurationPresenceDTO::kAbsent:
      return !active_control_configuration_id.has_value() &&
             !active_control_configuration_generation.has_value();
    case ExpectedActiveControlConfigurationPresenceDTO::kPresent:
      return active_control_configuration_id.has_value() &&
             bounded_identity(*active_control_configuration_id) &&
             active_control_configuration_generation.has_value() &&
             active_control_configuration_generation->nonzero();
  }
  return false;
}

bool ExpectedControlBindingRevisionDTO::locally_valid() const noexcept {
  return bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(control_route_id);
}

bool LifecycleFactValueDTO::locally_valid() const noexcept {
  switch (kind) {
  case LifecycleFactValueKindDTO::kUnknown:
    return !boolean_value.has_value() && !integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kBoolean:
    return boolean_value.has_value() && !integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kInteger:
    return !boolean_value.has_value() && integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kToken:
    return !boolean_value.has_value() && !integer_value.has_value() &&
           token_value.has_value() && portable_token(*token_value);
  }
  return false;
}

bool LifecycleFactDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_fact_id) && value.locally_valid();
}

bool BrandLifecycleObservationDTO::locally_valid() const noexcept {
  if (!bounded_identity(lifecycle_transaction_id) || !lifecycle_transaction_generation.nonzero() ||
      !bounded_identity(controllable_component_id) || !bounded_identity(control_domain_id) ||
      !bounded_identity(control_domain_revision) || !bounded_identity(provider_instance_id) ||
      !bounded_identity(provider_manifest_id) || !bounded_identity(provider_manifest_revision) ||
      !bounded_identity(provider_runtime_unit_id) || !bounded_identity(provider_runtime_epoch) ||
      source_sequence == 0U || observation_generation == 0U || source_timestamp_ns < 0 ||
      received_timestamp_ns < 0 || fresh_until_ns < 0 ||
      source_timestamp_ns > received_timestamp_ns || received_timestamp_ns >= fresh_until_ns ||
      !normalized_health_valid(observed_state, facts, diagnostic)) {
    return false;
  }
  return true;
}

bool ProviderControlDomainDTO::locally_valid() const noexcept {
  if (!bounded_identity(controllable_component_id) || !bounded_identity(provider_runtime_unit_id) ||
      !bounded_identity(provider_instance_id) || !bounded_identity(provider_manifest_id) ||
      !IsExactLowercaseSha256Digest(provider_manifest_revision.value()) ||
      !IsExactLowercaseSha256Digest(lifecycle_plan_catalog_digest.value()) ||
      !IsExactLowercaseSha256Digest(signal_configuration_digest.value()) ||
      lifecycle_command_timeout_ns <= 0 ||
      lifecycle_command_timeout_ns > kMaxLifecycleTransactionTimeoutNs || signal_names.empty() ||
      signal_names.size() > kMaxLifecycleCurrentResources ||
      !std::is_sorted(signal_names.begin(), signal_names.end()) ||
      std::adjacent_find(signal_names.begin(), signal_names.end()) != signal_names.end())
    return false;
  return std::all_of(signal_names.begin(), signal_names.end(), [](const auto& name) {
    return !name.empty() && name.size() <= kMaxLifecycleIdentityBytes;
  });
}

bool ResolvedLifecycleProviderOccurrenceDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && bounded_identity(control_domain_revision) &&
         bounded_identity(provider_instance_id) && bounded_identity(provider_manifest_id) &&
         bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
         bounded_identity(lifecycle_plan_catalog_digest);
}

bool ResolvedLifecycleControlDomainScopeDTO::locally_valid() const noexcept {
  if (payload.valueless_by_exception() || !bounded_identity(control_domain_id) ||
      !bounded_identity(control_domain_revision))
    return false;
  if (const auto* value = provider()) {
    if (!value->provider.locally_valid() || !bounded_identity(value->provider_runtime_epoch))
      return false;
    if (value->output_resources.size() > kMaxLifecycleCurrentResources ||
        !std::is_sorted(value->output_resources.begin(), value->output_resources.end()) ||
        std::adjacent_find(value->output_resources.begin(), value->output_resources.end()) !=
            value->output_resources.end() ||
        std::any_of(value->output_resources.begin(), value->output_resources.end(),
                    [](const auto& id) { return !bounded_identity(id); }))
      return false;
    if (!value->active_control_configuration) return true;
    const auto& configuration = *value->active_control_configuration;
    return configuration.locally_valid() &&
           configuration.source_runtime_unit_id == value->provider.provider_runtime_unit_id &&
           configuration.source_runtime_epoch == value->provider_runtime_epoch &&
           configuration.signal_configuration_digest == value->provider.signal_configuration_digest;
  }
  const auto& [controller_topology_id, topology_revision, active_control_configuration,
               current_control_bindings, current_resources] = controller();
  if (!bounded_identity(controller_topology_id) || !bounded_identity(topology_revision) ||
      current_control_bindings.empty() ||
      current_control_bindings.size() > kMaxLifecycleResolvedBindings ||
      current_resources.empty() || current_resources.size() > kMaxLifecycleCurrentResources) {
    return false;
  }
  const auto binding_less = [](const ExpectedControlBindingRevisionDTO& left,
                               const ExpectedControlBindingRevisionDTO& right) {
    return left.control_binding_id < right.control_binding_id;
  };
  if (!sorted_unique(current_control_bindings, binding_less) ||
      !std::all_of(current_control_bindings.begin(), current_control_bindings.end(),
                   [](const ExpectedControlBindingRevisionDTO& binding) {
                     return binding.locally_valid();
                   }) ||
      !std::is_sorted(current_resources.begin(), current_resources.end()) ||
      std::adjacent_find(current_resources.begin(), current_resources.end()) !=
          current_resources.end() ||
      std::any_of(current_resources.begin(), current_resources.end(),
                  [](const ResourceId& resource) { return !bounded_identity(resource); })) {
    return false;
  }
  if (!active_control_configuration.has_value()) {
    return true;
  }
  const auto& configuration = *active_control_configuration;
  if (!configuration.locally_valid() || !active_configuration_bounded(configuration) ||
      configuration.control_domain_id != control_domain_id ||
      configuration.control_domain_revision != control_domain_revision ||
      configuration.controller_topology_id != controller_topology_id ||
      configuration.topology_revision != topology_revision ||
      configuration.binding_routes.size() != current_control_bindings.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < current_control_bindings.size(); ++index) {
    const auto& current = current_control_bindings[index];
    const auto& configured = configuration.binding_routes[index];
    if (current.control_binding_id != configured.control_binding_id ||
        current.control_binding_revision != configured.control_binding_revision ||
        current.control_route_id != configured.control_route_id) {
      return false;
    }
  }
  return true;
}

bool ProviderDomainActiveConfigurationDTO::locally_valid() const noexcept {
  return bounded_identity(active_control_configuration_id) &&
         active_control_configuration_generation.nonzero() &&
         bounded_identity(source_runtime_unit_id) && bounded_identity(source_runtime_epoch) &&
         source_sequence != 0U && IsExactLowercaseSha256Digest(signal_configuration_digest.value());
}

bool ComponentLifecycleCurrentSnapshotDTO::locally_valid() const noexcept {
  if (!bounded_identity(snapshot_digest) || !bounded_identity(controllable_component_id) ||
      !component_lifecycle_generation.nonzero() || !known(component_lifecycle_state) ||
      !active_publication_receipt.valid() || domains.empty() ||
      domains.size() > kMaxLifecycleResolvedDomains ||
      provider_occurrences.size() != domains.size() ||
      provider_occurrences.size() > kMaxLifecycleProviderOccurrences) {
    return false;
  }
  const auto domain_less = [](const ResolvedLifecycleControlDomainScopeDTO& left,
                              const ResolvedLifecycleControlDomainScopeDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  const auto provider_less = [](const ResolvedLifecycleProviderOccurrenceDTO& left,
                                const ResolvedLifecycleProviderOccurrenceDTO& right) {
    return left.control_domain_id < right.control_domain_id;
  };
  if (!sorted_unique(domains, domain_less) || !sorted_unique(provider_occurrences, provider_less)) {
    return false;
  }
  std::size_t binding_count = 0U;
  std::size_t resource_count = 0U;
  for (std::size_t index = 0U; index < domains.size(); ++index) {
    const auto& domain = domains[index];
    const auto& provider = provider_occurrences[index];
    if (!domain.locally_valid() || !provider.locally_valid() ||
        domain.control_domain_id != provider.control_domain_id ||
        domain.control_domain_revision != provider.control_domain_revision ||
        binding_count > kMaxLifecycleResolvedBindings - domain.control_bindings().size() ||
        resource_count > kMaxLifecycleCurrentResources - domain.resource_ids().size()) {
      return false;
    }
    if (const auto* driver = domain.provider();
        driver && (driver->provider.controllable_component_id != controllable_component_id ||
                   !ProviderDomainMatchesOccurrence(
                       domain.control_domain_id, domain.control_domain_revision, driver->provider,
                       driver->provider_runtime_epoch, provider)))
      return false;
    binding_count += domain.control_bindings().size();
    resource_count += domain.resource_ids().size();
  }
  return true;
}

}  // namespace manyforge::core
