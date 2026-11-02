#pragma once

#include <functional>
#include <stdexcept>

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
  /// Borrowed readback from an actual held owner interlock. It cannot be
  /// constructed from a decoded/copied DTO or retained by value. Consumers
  /// use it synchronously inside that interlock's callback; it is not a wire
  /// capability and cannot establish original-owner continuation by itself.
  class HeldStatusView final {
   public:
    HeldStatusView(const HeldStatusView&) = delete;
    HeldStatusView& operator=(const HeldStatusView&) = delete;
    HeldStatusView(HeldStatusView&&) = delete;
    HeldStatusView& operator=(HeldStatusView&&) = delete;
    [[nodiscard]] const ConfigurationFenceStatusDTO& status() const noexcept {
      return status_;
    }
   private:
    friend class IManagerKernelApplyFencePort;
    explicit HeldStatusView(const ConfigurationFenceStatusDTO& status) : status_(status) {}
    const ConfigurationFenceStatusDTO& status_;
  };

  virtual ~IManagerKernelApplyFencePort() = default;

  [[nodiscard]] virtual ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const ConfigurationFenceAdmissionQueryDTO& query) const = 0;

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

 protected:
  /// Only a trusted owner implementation may call this after positively
  /// establishing its held interlock, keeping it held through the callback.
  static void WithHeldStatusView(
      const ConfigurationFenceStatusDTO& status,
      const std::function<void(const HeldStatusView&)>& effect) {
    if (status.state != ConfigurationFenceState::kHeld || !status.admission_fenced || !effect)
      throw std::invalid_argument("held fence readback requires the live owner interlock");
    const HeldStatusView view(status);
    effect(view);
  }
};

}  // namespace manyforge::core
