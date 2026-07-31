#pragma once

#include <manyforge_core/types/configuration_fence.hpp>

namespace manyforge::core {

/// Bounded Component-Manager/Kernel Apply-fence acquire/reconcile/rehydrate
/// boundary.
///
/// Acquire, read-only lost-response reconciliation, and restart rehydration
/// have frozen DTO shapes and are exposed here. An attempt-only reconciliation
/// result is the only authority for retrying an Acquire whose response was
/// lost; request-ledger absence must never be inferred from a transport error.
/// Status/query and compare-and-swap release remain deliberately absent until
/// spec 210 freezes the required configuration-fence reconciliation-evidence
/// value. This partial substrate exposes no generic containment or Stop
/// primitive. The final `IManagerKernelApplyFencePort` name remains reserved
/// for the complete operational port required by spec 220.
class IManagerKernelApplyFenceAcquireRehydratePort {
 public:
  virtual ~IManagerKernelApplyFenceAcquireRehydratePort() = default;

  [[nodiscard]] virtual ConfigurationFenceAcceptedDTO Acquire(
      const ApplyConfigurationFenceIntentDTO& intent) = 0;

  [[nodiscard]] virtual ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO& query) const = 0;

  [[nodiscard]] virtual ApplyFenceReconcileResultDTO Rehydrate(
      const ApplyFenceRehydrateDTO& request) = 0;
};

}  // namespace manyforge::core
