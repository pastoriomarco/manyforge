#pragma once

#include <cstdint>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::component_manager {

/// The complete native receiver batch rejected a predecessor publication
/// before accepting any heartbeat.  Callers may refresh the Manager snapshot
/// immediately; this outcome carries no partial-renewal uncertainty.
class ManagerLivenessPublicationSuperseded final : public std::runtime_error {
public:
  ManagerLivenessPublicationSuperseded()
      : std::runtime_error(
            "Manager-liveness batch names a superseded active publication") {}
};

/// The Manager snapshot still names a predecessor process while convergence
/// is replacing that process.  No heartbeat was sent, so callers can keep the
/// cell closed and retry as soon as the converged snapshot advances.
class ManagerLivenessRuntimeTransition final : public std::runtime_error {
public:
  ManagerLivenessRuntimeTransition()
      : std::runtime_error(
            "Manager-liveness snapshot is awaiting the current runtime epoch") {}
};

/// One exact Manager-to-runtime renewal transaction.  The required set and
/// policy are repeated deliberately: the private child never consults ambient
/// Manager state and can reject a changed bundle/set/policy before renewal.
struct ManagerLivenessTransportRequestDTO final {
  manyforge::core::RuntimeUnitId receiver_runtime_unit_id;
  manyforge::core::ActiveBundlePublicationGeneration
      active_publication_generation;
  manyforge::core::GraphRevision active_graph_revision;
  manyforge::core::ContentDigest active_public_pointer_digest;
  manyforge::core::ManagerLivenessRequiredSetDTO required_set;
  manyforge::core::ManagerLivenessPolicyDTO policy;
  manyforge::core::ManagerHeartbeatDTO heartbeat;
  std::optional<manyforge::core::ManagerLivenessReconciliationAck>
      reconciliation;

  friend bool operator==(const ManagerLivenessTransportRequestDTO &,
                         const ManagerLivenessTransportRequestDTO &) = default;
};

class IManagerLivenessTransportPort {
public:
  virtual ~IManagerLivenessTransportPort() = default;
  [[nodiscard]] virtual manyforge::core::ManagerLivenessObservationDTO
  RenewManagerLiveness(const ManagerLivenessTransportRequestDTO &request) = 0;
  [[nodiscard]] virtual std::vector<
      manyforge::core::ManagerLivenessObservationDTO>
  RenewManagerLivenessBatch(
      const std::vector<ManagerLivenessTransportRequestDTO> &requests) = 0;
};

} // namespace manyforge::component_manager
