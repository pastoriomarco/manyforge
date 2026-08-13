#pragma once

#include <manyforge_core/route_selection.hpp>

namespace manyforge::core {

/// ROS-free owner-to-owner boundary from CCLC to the Kernel route fence.
class ICCLCKernelRouteConfigurationFencePort {
 public:
  virtual ~ICCLCKernelRouteConfigurationFencePort() = default;

  [[nodiscard]] virtual Result<RouteConfigurationFenceAdmissionSnapshotDTO>
  ObserveRouteConfigurationFenceAdmission(
      const RouteConfigurationFenceAdmissionQueryDTO& query) const = 0;

  [[nodiscard]] virtual Result<RouteConfigurationFenceAcceptedDTO> AcquireRouteConfigurationFence(
      const RouteConfigurationFenceIntentDTO& intent) = 0;

  [[nodiscard]] virtual Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const RouteConfigurationFenceIntentDTO& intent) const = 0;

  [[nodiscard]] virtual Result<RouteConfigurationFenceStatusDTO>
  ObserveRouteConfigurationFenceStatus(
      const RouteConfigurationFenceStatusQueryDTO& query) const = 0;

  [[nodiscard]] virtual Result<RouteConfigurationFenceReleasedDTO> ReleaseRouteConfigurationFence(
      const RouteConfigurationFenceReleaseRequestDTO& request) = 0;

  [[nodiscard]] virtual Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const RouteConfigurationFenceReleaseRequestDTO& request) const = 0;
};

}  // namespace manyforge::core
