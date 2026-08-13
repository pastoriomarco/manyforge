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
};

}  // namespace manyforge::core
