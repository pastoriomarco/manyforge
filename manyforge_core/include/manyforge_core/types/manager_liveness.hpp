#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/retained_artifact.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumManagerLivenessReceivers = 4096U;
inline constexpr std::uint64_t kMaximumManagerLivenessCounter =
    9'007'199'254'740'991ULL;

enum class ManagerLivenessStateDTO { kCurrent, kUnreconciled };

[[nodiscard]] std::string_view to_string(ManagerLivenessStateDTO value) noexcept;

struct ManagerLivenessPolicyDTO final {
  RuntimePolicyId runtime_policy_id;
  RuntimePolicyRevision runtime_policy_revision;
  std::int64_t heartbeat_period_ns{};

  friend bool operator==(const ManagerLivenessPolicyDTO&,
                         const ManagerLivenessPolicyDTO&) = default;
};

struct ManagerHeartbeatDTO final {
  CellId cell_id;
  RuntimeUnitId manager_runtime_unit_id;
  std::uint64_t heartbeat_sequence{};
  RuntimePolicyId runtime_policy_id;
  RuntimePolicyRevision runtime_policy_revision;

  friend bool operator==(const ManagerHeartbeatDTO&, const ManagerHeartbeatDTO&) = default;
};

struct ManagerLivenessStatusDTO final {
  CellId cell_id;
  RuntimeUnitId receiver_runtime_unit_id;
  RuntimeEpoch receiver_runtime_epoch;
  RuntimeUnitId manager_runtime_unit_id;
  RuntimePolicyId runtime_policy_id;
  RuntimePolicyRevision runtime_policy_revision;
  std::uint64_t last_accepted_heartbeat_sequence{};
  std::uint64_t receiver_status_sequence{};
  ManagerLivenessStateDTO state{ManagerLivenessStateDTO::kUnreconciled};
  bool command_fenced{true};
  std::optional<ContentDigest> fault_or_effect_reference;

  friend bool operator==(const ManagerLivenessStatusDTO&,
                         const ManagerLivenessStatusDTO&) = default;
};

struct ManagerLivenessObservationDTO final {
  ManagerLivenessStatusDTO status;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;

  friend bool operator==(const ManagerLivenessObservationDTO&,
                         const ManagerLivenessObservationDTO&) = default;
};

struct RequiredManagerLivenessReceiverDTO final {
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;
  RuntimePolicyId runtime_policy_id;
  RuntimePolicyRevision runtime_policy_revision;

  friend bool operator==(const RequiredManagerLivenessReceiverDTO&,
                         const RequiredManagerLivenessReceiverDTO&) = default;
};

struct ManagerLivenessRequiredSetDTO final {
  CellId cell_id;
  RetainedBundleId active_bundle_id;
  ContentDigest required_runtime_set_digest;
  RuntimeUnitId manager_runtime_unit_id;
  std::vector<RequiredManagerLivenessReceiverDTO> required_receivers;

  friend bool operator==(const ManagerLivenessRequiredSetDTO&,
                         const ManagerLivenessRequiredSetDTO&) = default;
};

[[nodiscard]] Result<void> ValidateManagerLivenessPolicy(
    const ManagerLivenessPolicyDTO& policy);
[[nodiscard]] Result<void> ValidateManagerHeartbeat(const ManagerHeartbeatDTO& heartbeat);
[[nodiscard]] Result<void> ValidateManagerLivenessStatus(
    const ManagerLivenessStatusDTO& status);
[[nodiscard]] Result<void> ValidateManagerLivenessObservation(
    const ManagerLivenessObservationDTO& observation);
[[nodiscard]] Result<void> ValidateManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set);
[[nodiscard]] ContentDigest ComputeRequiredRuntimeSetDigest(
    const std::vector<RequiredManagerLivenessReceiverDTO>& receivers);

}  // namespace manyforge::core
