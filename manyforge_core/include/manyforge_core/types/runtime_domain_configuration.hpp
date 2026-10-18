#pragma once

#include <manyforge_core/route_selection_owner.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <utility>
#include <variant>

namespace manyforge::core {

/// Current driver-domain fact retained by the same Kernel inventory as route
/// readbacks. CCLC and the installed provider supply these facts; receiving an
/// input sample never enables a component or creates a lifecycle observation.
struct RuntimeProviderDomainConfigurationReadbackDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ResolvedLifecycleControlDomainScopeDTO domain;
  ResolvedLifecycleProviderOccurrenceDTO provider_occurrence;
  bool operation_admission_open{};
  std::uint64_t driver_source_sequence{};
  std::uint64_t driver_observation_generation{};

  [[nodiscard]] bool locally_valid() const noexcept {
    const auto* actual = domain.provider();
    const bool driver_qualified =
        driver_source_sequence != 0U && driver_observation_generation != 0U;
    const bool metadata_only = driver_source_sequence == 0U &&
                               driver_observation_generation == 0U && !operation_admission_open;
    return !retained_bundle_id.empty() && !graph_revision.empty() &&
           component_lifecycle_generation.nonzero() && actual &&
           provider_occurrence.locally_valid() &&
           ProviderDomainMatchesOccurrence(domain.control_domain_id, domain.control_domain_revision,
                                           actual->provider, actual->provider_runtime_epoch,
                                           provider_occurrence) &&
           (driver_qualified || metadata_only);
  }

  friend bool operator==(const RuntimeProviderDomainConfigurationReadbackDTO&,
                         const RuntimeProviderDomainConfigurationReadbackDTO&) = default;
};

/// One domain has either controller-route facts or genuine provider facts.
/// The physical readback DTO and its existing wire representation are intact.
struct RuntimeDomainConfigurationReadbackDTO final {
  std::variant<RuntimeRouteConfigurationReadbackDTO, RuntimeProviderDomainConfigurationReadbackDTO>
      payload;

  RuntimeDomainConfigurationReadbackDTO(RuntimeRouteConfigurationReadbackDTO value)
      : payload(std::move(value)) {}
  RuntimeDomainConfigurationReadbackDTO(RuntimeProviderDomainConfigurationReadbackDTO value)
      : payload(std::move(value)) {}

  [[nodiscard]] RuntimeRouteConfigurationReadbackDTO& controller() {
    return std::get<RuntimeRouteConfigurationReadbackDTO>(payload);
  }
  [[nodiscard]] const RuntimeRouteConfigurationReadbackDTO& controller() const {
    return std::get<RuntimeRouteConfigurationReadbackDTO>(payload);
  }
  [[nodiscard]] const RuntimeProviderDomainConfigurationReadbackDTO* provider() const noexcept {
    return std::get_if<RuntimeProviderDomainConfigurationReadbackDTO>(&payload);
  }

  friend bool operator==(const RuntimeDomainConfigurationReadbackDTO&,
                         const RuntimeDomainConfigurationReadbackDTO&) = default;
};

}  // namespace manyforge::core
