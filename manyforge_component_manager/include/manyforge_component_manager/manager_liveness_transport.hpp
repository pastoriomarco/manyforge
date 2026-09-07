#pragma once

#include <cstdint>
#include <limits>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::component_manager {

namespace detail {

enum class ManagerLivenessBatchProgress { kNotStarted, kInFlight, kValidated };

/// One native-batch retry latch. An indeterminate failure inside the exact
/// native batch blocks retries until partial renewals have expired. Failures
/// before that batch or in a separately addressed receiver retry normally.
class ManagerLivenessRetryGate final {
public:
  [[nodiscard]] bool
  attempt_allowed(const std::uint64_t now_ns) const noexcept {
    return !failed_ || now_ns >= retry_not_before_ns_;
  }

  void RecordFailure(const std::uint64_t now_ns,
                     const std::uint64_t expiry_ns) noexcept {
    failed_ = true;
    retry_not_before_ns_ =
        now_ns > std::numeric_limits<std::uint64_t>::max() - expiry_ns
            ? std::numeric_limits<std::uint64_t>::max()
            : now_ns + expiry_ns;
  }

  void RecordSuccess() noexcept {
    failed_ = false;
    retry_not_before_ns_ = 0U;
  }

  void RecordAttemptFailure(ManagerLivenessBatchProgress progress,
                            std::uint64_t now_ns,
                            std::uint64_t expiry_ns) noexcept {
    if (progress == ManagerLivenessBatchProgress::kInFlight) {
      RecordFailure(now_ns, expiry_ns);
    } else {
      // A preflight failure or failure in a separately addressed receiver
      // cannot make an already validated native batch uncertain.
      RecordSuccess();
    }
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
  bool failed_{};
  std::uint64_t retry_not_before_ns_{};
};

} // namespace detail

/// The complete native receiver batch rejected a predecessor publication
/// before accepting any heartbeat.  Callers may refresh the Manager snapshot
/// immediately; this outcome carries no partial-renewal uncertainty.
class ManagerLivenessPublicationSuperseded final : public std::runtime_error {
public:
  ManagerLivenessPublicationSuperseded()
      : std::runtime_error(
            "Manager-liveness batch names a superseded active publication") {}
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
