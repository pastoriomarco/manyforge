#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kOperatorAlarmAttentionSchemaVersion =
    "manyforge.operator_alarm_motion_attention.v3";
/// Matches the Alarm Lifecycle Owner hard live cap.
inline constexpr std::size_t kMaximumOperatorAlarmAttentionMembers = 256U;
/// Matches the owner's per-subject resource bound.
inline constexpr std::size_t kMaximumOperatorAlarmSubjectResources = 16U;
inline constexpr std::size_t kMaximumOperatorAlarmAttentionTextBytes = 2048U;
/// Bound on each refusal membership detail's canonical JSON. Whole entries
/// beyond it are omitted, never shortened, and the refusal then carries
/// `alarm_membership_truncated = "true"`.
inline constexpr std::size_t kMaximumOperatorAlarmRefusalMembershipBytes = 4096U;
/// Kernel freshness bound F. The Headless publisher runs on its own thread and
/// starts a publication every P = 250 ms, each ending by its monotonic
/// deadline D = 200 ms < P, so consecutive receipts are at most P + D = 450 ms
/// apart while the path is healthy; F = 8 P leaves margin for six consecutive
/// abandoned publications (manyforge_headless_api/alarm_attention.py). A
/// production Kernel treats a projection older than F as stale and fails
/// Program motion closed.
inline constexpr Duration kOperatorAlarmAttentionMaximumSnapshotAgeNs = 2'000'000'000;

/// Stable refusal reasons.
inline constexpr std::string_view kOperatorAlarmInterlockActiveReason =
    "operator_alarm_interlock_active";
inline constexpr std::string_view kOperatorAlarmStateUnavailableReason =
    "operator_alarm_state_unavailable";

/// One gating `motion_attention_membership` record with its canonical subject:
/// a condition interlock (an alarm that needs no acknowledgement), present
/// exactly while active. Acknowledgement never gates work (V2-5).
struct OperatorAlarmAttentionMemberDTO final {
  std::string condition_id;
  std::string activation_id;
  std::uint64_t record_revision{};
  std::string subject_kind;
  std::string subject_id;
  /// Sorted, unique canonical Kernel resource ids; empty only for the
  /// cell-global subject.
  std::vector<ResourceId> resource_ids;

  friend bool operator==(const OperatorAlarmAttentionMemberDTO&,
                         const OperatorAlarmAttentionMemberDTO&) = default;
};

/// The active configuration (retained bundle and graph revision) whose
/// resource registry judged the alarm subjects.
struct OperatorAlarmAttentionConfigurationDTO final {
  RetainedBundleId retained_bundle_id{std::string{}};
  GraphRevision graph_revision{std::string{}};

  friend bool operator==(const OperatorAlarmAttentionConfigurationDTO&,
                         const OperatorAlarmAttentionConfigurationDTO&) = default;
};

/// Immutable generation-fenced projection that the Headless Alarm Lifecycle
/// Owner publishes to the Kernel. It carries no authority: the Kernel alone
/// evaluates it against an operation's exact reserved resources.
struct OperatorAlarmAttentionSnapshotDTO final {
  CellId cell_id{std::string{}};
  std::string owner_epoch;
  /// The owner instance's incarnation: the host boot it was constructed in
  /// and its CLOCK_BOOTTIME at construction (core/time/host_boot_identity.hpp).
  /// Strictly increasing across owner instances on one host and boot; it
  /// orders owner epochs, whose text carries no order.
  std::string owner_host_boot_id;
  std::uint64_t owner_incarnation_us{};
  std::uint64_t generation{};
  /// False while the owner is unreconciled or unhealthy; admission then
  /// fails closed with `operator_alarm_state_unavailable`.
  bool reconciled{};
  /// The owner's active resource-registry configuration; absent before it
  /// observed one. Attention is available only while it equals the Kernel's
  /// own active configuration: subjects judged against another configuration
  /// carry no trustworthy scope for this Kernel's resources.
  std::optional<OperatorAlarmAttentionConfigurationDTO> active_configuration;
  /// Complete membership, sorted by condition id.
  std::vector<OperatorAlarmAttentionMemberDTO> members;

  friend bool operator==(const OperatorAlarmAttentionSnapshotDTO&,
                         const OperatorAlarmAttentionSnapshotDTO&) = default;
};

/// Kernel-side composition policy. A composition that hosts the Alarm
/// Lifecycle Owner enables it; a received snapshot older than the bound is
/// stale and fails admission closed.
struct OperatorAlarmAttentionPolicyDTO final {
  Duration maximum_snapshot_age_ns{};
  /// The Kernel host's boot id. Single-host precondition: owner incarnations
  /// are compared only when published from this same host boot; any other
  /// boot id is refused rather than compared.
  std::string host_boot_id;

  friend bool operator==(const OperatorAlarmAttentionPolicyDTO&,
                         const OperatorAlarmAttentionPolicyDTO&) = default;
};

/// Kernel acknowledgement of one accepted publication.
struct OperatorAlarmAttentionAcceptedDTO final {
  std::string owner_epoch;
  std::uint64_t generation{};

  friend bool operator==(const OperatorAlarmAttentionAcceptedDTO&,
                         const OperatorAlarmAttentionAcceptedDTO&) = default;
};

/// The snapshot most recently accepted by the Kernel, with its receipt time
/// on the Kernel's injected clock.
struct RetainedOperatorAlarmAttentionDTO final {
  OperatorAlarmAttentionSnapshotDTO snapshot;
  TimePoint received_at_ns{};
};

[[nodiscard]] Result<void> validate_operator_alarm_attention_snapshot(
    const OperatorAlarmAttentionSnapshotDTO& snapshot);

/// Attention and fencing check for one candidate operation whose exact
/// reserved resource set (union over coordinated participants) is
/// `reserved_resources`. Returns the refusal, or nothing when this attention
/// input admits the operation. A matching condition interlock reports
/// `operator_alarm_interlock_active` with its membership (bounded, see
/// `kMaximumOperatorAlarmRefusalMembershipBytes`).
/// A missing, unreconciled, foreign-cell, stale snapshot, or one judged
/// against another active configuration than `active_configuration` (the
/// Kernel's own), reports `operator_alarm_state_unavailable`.
[[nodiscard]] std::optional<ErrorDTO> evaluate_operator_alarm_motion_attention(
    const std::optional<RetainedOperatorAlarmAttentionDTO>& retained,
    const OperatorAlarmAttentionPolicyDTO& policy, const CellId& cell_id,
    const std::optional<OperatorAlarmAttentionConfigurationDTO>& active_configuration,
    TimePoint now_ns, const std::vector<ResourceId>& reserved_resources);

}  // namespace manyforge::core
