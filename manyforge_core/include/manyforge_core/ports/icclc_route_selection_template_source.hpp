#pragma once

#include <manyforge_core/route_selection_owner.hpp>
#include <vector>

namespace manyforge::core {

/// One fresh, complete-domain route request template reconstructed only from
/// the selected retained bundle and current CCLC facts. A caller adds its own
/// request context; it must not edit or merge any field in this value.
struct CCLCRouteSelectionTemplateDTO final {
  DesiredControlConfigurationDTO desired_configuration;
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;
  std::vector<RouteSelectionComponentExpectationDTO> component_expectations;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteSelectionTemplateDTO&,
                         const CCLCRouteSelectionTemplateDTO&) = default;
};

/// Retained/current route templates relevant to one exact component. Empty is
/// a truthful statement that the component has no retained selectable route.
struct CCLCRouteSelectionTemplateSetDTO final {
  ControllableComponentId controllable_component_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::vector<CCLCRouteSelectionTemplateDTO> templates;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteSelectionTemplateSetDTO&,
                         const CCLCRouteSelectionTemplateSetDTO&) = default;
};

class ICCLCRouteSelectionTemplateSource {
 public:
  virtual ~ICCLCRouteSelectionTemplateSource() = default;

  [[nodiscard]] virtual Result<CCLCRouteSelectionTemplateSetDTO> ReadCurrentRouteSelectionTemplates(
      const ControllableComponentId& component_id) const = 0;
};

}  // namespace manyforge::core
