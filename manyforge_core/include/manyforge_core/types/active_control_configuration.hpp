#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/kinematic_state_view.hpp>
#include <string_view>
#include <tuple>
#include <vector>

namespace manyforge::core {

/// Defensive Core-local collection ceilings for one active domain fact.
///
/// These bounds prevent an untrusted transport or adapter from presenting an
/// unbounded value. They do not prove that a row belongs to the retained graph;
/// CCLC performs that graph/topology reconciliation before publication.
inline constexpr std::size_t kMaxActiveControlConfigurationBindingRoutes = 4096U;
inline constexpr std::size_t kMaxActiveControlConfigurationResourceSourceProjections = 4096U;

/// One selected route in the complete active configuration of a control
/// domain. A valid complete configuration contains at most one row for a
/// stable control-binding ID.
struct ActiveControlConfigurationBindingRouteDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlRouteId control_route_id;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !control_binding_id.empty() && !control_binding_revision.empty() &&
           !control_route_id.empty();
  }

  friend bool operator==(const ActiveControlConfigurationBindingRouteDTO&,
                         const ActiveControlConfigurationBindingRouteDTO&) = default;
};

[[nodiscard]] inline bool CanonicalActiveControlConfigurationBindingRouteLess(
    const ActiveControlConfigurationBindingRouteDTO& lhs,
    const ActiveControlConfigurationBindingRouteDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision, lhs.control_route_id) <
         std::tie(rhs.control_binding_id, rhs.control_binding_revision, rhs.control_route_id);
}

enum class ActiveControlConfigurationStateDTO : std::uint8_t {
  kExternalOrUnknownUnreconciled = 0,
  kReconciled,
};

[[nodiscard]] constexpr std::string_view to_string(
    ActiveControlConfigurationStateDTO state) noexcept {
  switch (state) {
    case ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled:
      return "external_or_unknown_unreconciled";
    case ActiveControlConfigurationStateDTO::kReconciled:
      return "reconciled";
  }
  return {};
}

[[nodiscard]] constexpr bool IsKnownActiveControlConfigurationState(
    ActiveControlConfigurationStateDTO state) noexcept {
  switch (state) {
    case ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled:
    case ActiveControlConfigurationStateDTO::kReconciled:
      return true;
  }
  return false;
}

/// CCLC-owned complete runtime fact for one exact control-domain revision.
///
/// `locally_valid()` checks only bounded, canonical, internally consistent
/// value shape. It deliberately cannot prove completeness against the retained
/// graph, installed controllers, endpoint inventory, or observed claims; those
/// are reconciliation postconditions owned by CCLC.
struct ActiveControlConfigurationDTO final {
  ActiveControlConfigurationId active_control_configuration_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  std::vector<ActiveControlConfigurationBindingRouteDTO> binding_routes;
  ActivationSetId activation_set_id;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  RuntimeUnitId source_runtime_unit_id;
  RuntimeEpoch source_runtime_epoch;
  /// CCLC verifies source-local monotonicity; this value layer does not invent
  /// a first-sequence convention that the active-configuration spec omits.
  std::uint64_t source_sequence{};
  std::vector<KinematicResourceSourceProjectionDTO> kinematic_resource_source_projections;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ActiveControlConfigurationStateDTO state{
      ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled};

  [[nodiscard]] bool locally_valid() const noexcept {
    if (active_control_configuration_id.empty() || control_domain_id.empty() ||
        control_domain_revision.empty() || activation_set_id.empty() ||
        controller_topology_id.empty() || topology_revision.empty() ||
        source_runtime_unit_id.empty() || source_runtime_epoch.empty() ||
        !active_control_configuration_generation.nonzero() ||
        !IsKnownActiveControlConfigurationState(state) ||
        binding_routes.empty() ||
        binding_routes.size() > kMaxActiveControlConfigurationBindingRoutes ||
        kinematic_resource_source_projections.size() >
            kMaxActiveControlConfigurationResourceSourceProjections) {
      return false;
    }

    if (!std::is_sorted(binding_routes.begin(), binding_routes.end(),
                        CanonicalActiveControlConfigurationBindingRouteLess)) {
      return false;
    }
    for (std::size_t index = 0U; index < binding_routes.size(); ++index) {
      if (!binding_routes[index].locally_valid() ||
          (index != 0U &&
           binding_routes[index - 1U].control_binding_id ==
               binding_routes[index].control_binding_id)) {
        return false;
      }
    }

    const auto projection_less = [](const KinematicResourceSourceProjectionDTO& lhs,
                                    const KinematicResourceSourceProjectionDTO& rhs) noexcept {
      return lhs.resource_id < rhs.resource_id;
    };
    if (!std::is_sorted(kinematic_resource_source_projections.begin(),
                        kinematic_resource_source_projections.end(), projection_less)) {
      return false;
    }

    for (std::size_t projection_index = 0U;
         projection_index < kinematic_resource_source_projections.size(); ++projection_index) {
      const auto& projection = kinematic_resource_source_projections[projection_index];
      if (projection.resource_id.empty() || projection.provider_instance_id.empty() ||
          projection.provider_manifest_id.empty() ||
          projection.provider_manifest_revision.empty() ||
          projection.state_authority_id.empty() ||
          projection.contributing_control_bindings.empty() ||
          projection.contributing_control_bindings.size() >
              kMaxActiveControlConfigurationBindingRoutes ||
          (projection_index != 0U &&
           kinematic_resource_source_projections[projection_index - 1U].resource_id ==
               projection.resource_id)) {
        return false;
      }

      const auto contributing_less = [](const KinematicControlBindingSourceDTO& lhs,
                                        const KinematicControlBindingSourceDTO& rhs) noexcept {
        return std::tie(lhs.control_binding_id, lhs.control_binding_revision) <
               std::tie(rhs.control_binding_id, rhs.control_binding_revision);
      };
      if (!std::is_sorted(projection.contributing_control_bindings.begin(),
                          projection.contributing_control_bindings.end(), contributing_less)) {
        return false;
      }

      for (std::size_t source_index = 0U;
           source_index < projection.contributing_control_bindings.size(); ++source_index) {
        const auto& source = projection.contributing_control_bindings[source_index];
        if (source.control_binding_id.empty() || source.control_binding_revision.empty() ||
            (source_index != 0U &&
             projection.contributing_control_bindings[source_index - 1U].control_binding_id ==
                 source.control_binding_id)) {
          return false;
        }

        const auto selected_route = std::lower_bound(
            binding_routes.begin(), binding_routes.end(), source.control_binding_id,
            [](const ActiveControlConfigurationBindingRouteDTO& route,
               const ControlBindingId& binding_id) noexcept {
              return route.control_binding_id < binding_id;
            });
        if (selected_route == binding_routes.end() ||
            selected_route->control_binding_id != source.control_binding_id ||
            selected_route->control_binding_revision != source.control_binding_revision) {
          return false;
        }
      }

      switch (projection.state_authority_class) {
        case StateAuthorityClass::kMeasured:
          if (projection.publication_source != ObservedStateSourceKind::kActionFeedback &&
              projection.publication_source != ObservedStateSourceKind::kControllerState &&
              projection.publication_source != ObservedStateSourceKind::kJointState) {
            return false;
          }
          break;
        case StateAuthorityClass::kCommanded:
          if (projection.publication_source != ObservedStateSourceKind::kCommandedState) {
            return false;
          }
          break;
        case StateAuthorityClass::kPredicted:
          if (projection.publication_source != ObservedStateSourceKind::kPredictedState) {
            return false;
          }
          break;
        default:
          return false;
      }
    }
    return true;
  }

  friend bool operator==(const ActiveControlConfigurationDTO&,
                         const ActiveControlConfigurationDTO&) = default;
};

}  // namespace manyforge::core
