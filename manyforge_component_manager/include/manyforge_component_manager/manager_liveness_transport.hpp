#pragma once

#include <manyforge_core/manager_liveness_receiver.hpp>
#include <cstdint>
#include <limits>
#include <vector>

namespace manyforge::component_manager {

namespace detail {

/// One Manager-wide retry latch. A failed exact batch blocks every receiver
/// until all partial renewals have had a full expiry interval to contain.
class ManagerLivenessRetryGate final {
 public:
  [[nodiscard]] bool attempt_allowed(const std::uint64_t now_ns) const noexcept {
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

  [[nodiscard]] bool failed() const noexcept { return failed_; }

 private:
  bool failed_{};
  std::uint64_t retry_not_before_ns_{};
};

}  // namespace detail

/// One exact Manager-to-runtime renewal transaction.  The required set and
/// policy are repeated deliberately: the private child never consults ambient
/// Manager state and can reject a changed bundle/set/policy before renewal.
struct ManagerLivenessTransportRequestDTO final {
  manyforge::core::RuntimeUnitId receiver_runtime_unit_id;
  manyforge::core::ManagerLivenessRequiredSetDTO required_set;
  manyforge::core::ManagerLivenessPolicyDTO policy;
  manyforge::core::ManagerHeartbeatDTO heartbeat;
  std::optional<manyforge::core::ManagerLivenessReconciliationAck>
      reconciliation;

  friend bool operator==(const ManagerLivenessTransportRequestDTO&,
                         const ManagerLivenessTransportRequestDTO&) = default;
};

class IManagerLivenessTransportPort {
 public:
  virtual ~IManagerLivenessTransportPort() = default;
  [[nodiscard]] virtual manyforge::core::ManagerLivenessObservationDTO
  RenewManagerLiveness(
      const ManagerLivenessTransportRequestDTO& request) = 0;
  [[nodiscard]] virtual std::vector<
      manyforge::core::ManagerLivenessObservationDTO>
  RenewManagerLivenessBatch(
      const std::vector<ManagerLivenessTransportRequestDTO>& requests) = 0;
};

}  // namespace manyforge::component_manager
