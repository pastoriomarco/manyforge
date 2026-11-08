#pragma once

#include <functional>
#include <manyforge_core/ports/i_manager_heartbeat_sink.hpp>
#include <manyforge_core/ports/i_manager_liveness_fail_safe_port.hpp>
#include <manyforge_core/ports/i_manager_liveness_observation_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <memory>
#include <chrono>

namespace manyforge::core {

using ManagerLossLocalSafeCallback =
    std::function<ContentDigest(const ManagerLivenessLossIntentDTO&)>;
using LegacyManagerLossLocalSafeCallback = std::function<ContentDigest()>;

struct ManagerLivenessReceiverConfig final {
  CellId cell_id;
  RuntimeUnitId receiver_runtime_unit_id;
  RuntimeEpoch receiver_runtime_epoch;
  RuntimeUnitId manager_runtime_unit_id;
  RuntimeEpoch manager_runtime_epoch;
  RequestOwnerEpoch manager_owner_epoch;
  ManagerLivenessPolicyDTO policy;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;
};

struct ManagerLivenessReconciliationAck final {
  CellId cell_id;
  RuntimeUnitId manager_runtime_unit_id;
  RuntimeEpoch manager_runtime_epoch;
  RequestOwnerEpoch manager_owner_epoch;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;
  ContentDigest reconciliation_evidence_digest;
};

/// Reusable receiver-local watchdog. Dependencies must outlive the receiver
/// and any scheduler callback already dequeued when destruction begins.
class ManagerLivenessReceiver final : public IManagerHeartbeatSink {
 public:
  ManagerLivenessReceiver(ManagerLivenessReceiverConfig config, IClock& clock,
                          IScheduler& scheduler,
                          ManagerLossLocalSafeCallback local_safe_callback,
                          IManagerLivenessFailSafePort& fail_safe_port,
                          IManagerLivenessObservationPort& observation_port);
  ManagerLivenessReceiver(ManagerLivenessReceiverConfig config, IClock& clock,
                          IScheduler& scheduler,
                          LegacyManagerLossLocalSafeCallback local_safe_callback,
                          IManagerLivenessFailSafePort& fail_safe_port,
                          IManagerLivenessObservationPort& observation_port);
  ~ManagerLivenessReceiver();
  /// Retire the watchdog before its borrowed scheduler and clock are destroyed.
  void DetachScheduler() noexcept;
  [[nodiscard]] bool DetachSchedulerUntil(std::chrono::steady_clock::time_point deadline) noexcept;

  ManagerLivenessReceiver(const ManagerLivenessReceiver&) = delete;
  ManagerLivenessReceiver& operator=(const ManagerLivenessReceiver&) = delete;

  [[nodiscard]] Result<void> Renew(const ManagerHeartbeatDTO& heartbeat) override;

  [[nodiscard]] Result<void> BeginManagerEpochReplacement(
      RuntimeUnitId manager_runtime_unit_id, RuntimeEpoch manager_runtime_epoch,
      RequestOwnerEpoch manager_owner_epoch);
  /// Atomically adopts the exact successor required-set identity without
  /// changing this receiver or extending its predecessor receipt.  The caller
  /// must already have closed command ingress and installed the complete set
  /// in the Kernel.
  [[nodiscard]] Result<void> RebindRequiredSet(
      RetainedBundleId active_bundle_id,
      ContentDigest required_runtime_set_digest);
  [[nodiscard]] Result<void> CompleteReconciliation(
      const ManagerLivenessReconciliationAck& acknowledgement);
  [[nodiscard]] ManagerLivenessStatusDTO status() const;
  [[nodiscard]] ManagerLivenessObservationDTO observation() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace manyforge::core
