#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// ROS-free owner-to-owner boundary from CCLC to the Kernel Disable handoff.
class ICCLCKernelDisableHandoffPort {
 public:
  virtual ~ICCLCKernelDisableHandoffPort() = default;

  [[nodiscard]] virtual Result<DisableHandoffAdmissionSnapshotDTO> ObserveDisableHandoffAdmission(
      const DisableHandoffAdmissionQueryDTO& query) const = 0;

  [[nodiscard]] virtual Result<DisableHandoffAcceptedDTO> BeginDisableHandoff(
      const DisableHandoffRequestDTO& request) = 0;

  [[nodiscard]] virtual Result<DisableHandoffStatusDTO> DisableHandoffStatus(
      const LifecycleTransactionId& lifecycle_transaction_id,
      LifecycleTransactionGeneration lifecycle_transaction_generation) = 0;

  [[nodiscard]] virtual Result<DisableHandoffAttemptReconciliationDTO>
  ReconcileDisableHandoffAttempt(const DisableHandoffRequestDTO& request) = 0;

  [[nodiscard]] virtual Result<DisableFenceReleasedDTO> ReleaseDisableFence(
      const DisableFenceReleaseRequestDTO& request) = 0;

  [[nodiscard]] virtual Result<DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(const DisableFenceReleaseRequestDTO& request) = 0;
};

}  // namespace manyforge::core
