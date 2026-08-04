#pragma once

#include <manyforge_core/types/configuration_fence.hpp>

namespace manyforge::core {

/// Bounded Component-Manager/Kernel Apply-fence lifecycle boundary.
///
/// Acquire, read-only lost-response reconciliation, and restart rehydration
/// are followed by read-only exact status and compare-and-swap release. An
/// attempt-only reconciliation result is the only authority for retrying an
/// Acquire whose response was lost; request-ledger absence must never be
/// inferred from a transport error. Release accepts only Kernel-issued
/// reconciliation evidence for the exact finalized Apply. This focused port
/// exposes no generic containment or Stop primitive.
class IManagerKernelApplyFencePort {
 public:
  virtual ~IManagerKernelApplyFencePort() = default;

  [[nodiscard]] virtual ConfigurationFenceAcceptedDTO Acquire(
      const ApplyConfigurationFenceIntentDTO& intent) = 0;

  [[nodiscard]] virtual ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO& query) const = 0;

  [[nodiscard]] virtual ApplyFenceReconcileResultDTO Rehydrate(
      const ApplyFenceRehydrateDTO& request) = 0;

  [[nodiscard]] virtual ConfigurationFenceStatusDTO QueryStatus(
      const ConfigurationFenceStatusQueryDTO& query) const = 0;

  [[nodiscard]] virtual ConfigurationFenceReleasedDTO Release(
      const ConfigurationFenceReleaseDTO& request) = 0;
};

}  // namespace manyforge::core
