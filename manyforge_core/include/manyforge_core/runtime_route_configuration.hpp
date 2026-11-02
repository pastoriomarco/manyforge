#pragma once

#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <vector>

namespace manyforge::core {

/// Readiness row for one selected control binding of an installed route: the
/// binding's sealed Stop base plan.
struct RouteReadinessPublicationRowDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ResolvedStopBasePlanId resolved_stop_base_plan_id;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteReadinessPublicationRowDTO&,
                         const RouteReadinessPublicationRowDTO&) = default;
};

[[nodiscard]] bool CanonicalRouteReadinessPublicationRowLess(
    const RouteReadinessPublicationRowDTO& lhs,
    const RouteReadinessPublicationRowDTO& rhs) noexcept;

/// The Kernel's installed route of one controller domain: the active
/// controller configuration the execution host published after `enable`, and
/// its per-binding Stop readiness.
struct RuntimeRouteConfigurationReadbackDTO final {
  ActiveControlConfigurationDTO active_configuration;
  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  /// Domain route readiness: the route and its Stop readiness are coherent.
  /// Commands additionally need the component `enabled`.
  bool operation_admission_open{};

  friend bool operator==(const RuntimeRouteConfigurationReadbackDTO&,
                         const RuntimeRouteConfigurationReadbackDTO&) = default;
};

}  // namespace manyforge::core
