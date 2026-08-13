#pragma once

#include <manyforge_core/route_selection_owner.hpp>

namespace manyforge::core {

/// NRT CCLC-to-composition boundary for complete active-configuration and
/// participant Stop-readiness publication. The implementation owns retained
/// product/repository/Kernel joins; CCLC owns transaction sequencing.
class ICCLCRouteConfigurationPublicationPort {
 public:
  virtual ~ICCLCRouteConfigurationPublicationPort() = default;

  /// Pure/read-only construction of the complete candidate fact from one
  /// fresh exact observation and retained expected-domain closure.
  [[nodiscard]] virtual Result<ActiveControlConfigurationDTO>
  ResolveResultingActiveControlConfiguration(
      const RouteSelectionAcceptedDTO& acceptance,
      const ControllerLifecycleExpectedDomain& expected_controller_domain,
      const ControllerLifecycleObservationDTO& observation,
      const ControllerLifecycleReconciliationDTO& reconciliation,
      RouteSelectionEffectDTO transaction_effect) const = 0;

  /// Idempotently publish and independently read back the already-durable
  /// configuration. Reconciled truth includes all immutable base plans and
  /// sealed readiness before Kernel admission can open; unreconciled truth
  /// installs only the persistent closed configuration gate.
  [[nodiscard]] virtual Result<RouteReadinessPublicationReceiptDTO> PublishAndReadBack(
      const RouteSelectionAcceptedDTO& acceptance,
      const ControllerLifecycleExpectedDomain& expected_controller_domain,
      const ActiveControlConfigurationDTO& durable_configuration) = 0;
};

}  // namespace manyforge::core
