#pragma once

#include <manyforge_core/ports/i_manager_heartbeat_sink.hpp>
#include <manyforge_core/ports/i_manager_liveness_observation_port.hpp>
#include <memory>

namespace manyforge::core {

struct ManagerLivenessReceiverConfig final {
  CellId cell_id;
  RuntimeUnitId receiver_runtime_unit_id;
  RuntimeEpoch receiver_runtime_epoch;
  RuntimeUnitId manager_runtime_unit_id;
  ManagerLivenessPolicyDTO policy;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;
};

struct ManagerLivenessReconciliationAck final {
  CellId cell_id;
  RuntimeUnitId manager_runtime_unit_id;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;
  ContentDigest reconciliation_evidence_digest;

  friend bool operator==(const ManagerLivenessReconciliationAck&,
                         const ManagerLivenessReconciliationAck&) = default;
};

/// Receiver of the Manager's status and identity reports. It holds no timer:
/// a report never expires. Dependencies must outlive the receiver.
class ManagerLivenessReceiver final : public IManagerHeartbeatSink {
 public:
  ManagerLivenessReceiver(ManagerLivenessReceiverConfig config,
                          IManagerLivenessObservationPort& observation_port);
  ~ManagerLivenessReceiver();

  ManagerLivenessReceiver(const ManagerLivenessReceiver&) = delete;
  ManagerLivenessReceiver& operator=(const ManagerLivenessReceiver&) = delete;

  [[nodiscard]] Result<void> Renew(const ManagerHeartbeatDTO& heartbeat) override;

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
