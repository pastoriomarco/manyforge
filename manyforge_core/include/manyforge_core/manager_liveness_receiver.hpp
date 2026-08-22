#pragma once

#include <functional>
#include <manyforge_core/ports/i_manager_heartbeat_sink.hpp>
#include <manyforge_core/ports/i_manager_liveness_fail_safe_port.hpp>
#include <manyforge_core/ports/i_manager_liveness_observation_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <memory>

namespace manyforge::core {

using ManagerLossLocalSafeCallback = std::function<ContentDigest()>;

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
  ~ManagerLivenessReceiver();

  ManagerLivenessReceiver(const ManagerLivenessReceiver&) = delete;
  ManagerLivenessReceiver& operator=(const ManagerLivenessReceiver&) = delete;

  [[nodiscard]] Result<void> Renew(const ManagerHeartbeatDTO& heartbeat) override;

  [[nodiscard]] Result<void> BeginManagerEpochReplacement(
      RuntimeUnitId manager_runtime_unit_id, RuntimeEpoch manager_runtime_epoch,
      RequestOwnerEpoch manager_owner_epoch);
  [[nodiscard]] Result<void> CompleteReconciliation(
      const ManagerLivenessReconciliationAck& acknowledgement);
  [[nodiscard]] ManagerLivenessStatusDTO status() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace manyforge::core
