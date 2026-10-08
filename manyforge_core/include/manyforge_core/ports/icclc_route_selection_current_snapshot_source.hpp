#pragma once

#include <manyforge_core/route_selection_owner.hpp>

namespace manyforge::core {

/// Read-only complete current closure for one candidate desired domain.
class ICCLCRouteSelectionCurrentSnapshotSource {
 public:
  virtual ~ICCLCRouteSelectionCurrentSnapshotSource() = default;

  [[nodiscard]] virtual Result<RouteSelectionCurrentSnapshot> ReadCurrentRouteSelectionSnapshot(
      const RetainedBundleId& expected_retained_bundle_id,
      const GraphRevision& expected_graph_revision,
      const DesiredControlConfigurationDTO& desired_configuration) const = 0;

  /// Separate observation-only read (Stop recovery / configuration
  /// observation). Same retained identity, controller source, current
  /// configuration agreement and expected domain as the route snapshot, but no
  /// component lifecycle expectation: a Declared component, or one without
  /// lifecycle facts, can be observed. Its result never admits a route change.
  [[nodiscard]] virtual Result<RouteConfigurationObservationSnapshot>
  ReadCurrentRouteConfigurationObservation(
      const RetainedBundleId& expected_retained_bundle_id,
      const GraphRevision& expected_graph_revision,
      const DesiredControlConfigurationDTO& desired_configuration) const = 0;
};

}  // namespace manyforge::core
