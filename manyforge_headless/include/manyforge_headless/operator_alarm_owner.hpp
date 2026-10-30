#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

#include <manyforge_core/time/host_boot_identity.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_headless/operator_alarm_catalog.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kOperatorAlarmSnapshotApiVersion =
    "manyforge.operator_alarm_snapshot.v2";
/// Wire schema of the Kernel-bound motion-attention publication; equal to
/// manyforge::core::kOperatorAlarmAttentionSchemaVersion.
inline constexpr std::string_view kOperatorAlarmMotionAttentionApiVersion =
    "manyforge.operator_alarm_motion_attention.v3";
inline constexpr std::string_view kOperatorAlarmMetricsApiVersion =
    "manyforge.operator_alarm_metrics.v1";
inline constexpr std::size_t kDefaultAlarmConditionCapacity = 8192U;
inline constexpr std::size_t kDefaultTerminalAlarmRetention = 4096U;
inline constexpr std::size_t kMaximumLiveAlarmCapacity = 256U;
inline constexpr std::size_t kDefaultLiveAlarmCapacity =
    kMaximumLiveAlarmCapacity;
inline constexpr std::size_t kMaximumAlarmSubjectResources = 16U;
inline constexpr std::size_t kMaximumAlarmResourceRegistry = 4096U;
inline constexpr std::size_t kMaximumAlarmSnapshotPage = 128U;
inline constexpr std::size_t kMaximumAlarmBulkMembers =
    kMaximumLiveAlarmCapacity;

class OperatorAlarmError : public manyforge::core::TypedError {
public:
  OperatorAlarmError(std::string code, std::string message);
  [[nodiscard]] const std::string &code() const noexcept;
};

enum class AlarmConditionQuality {
  kGood,
  kUnknown,
};

[[nodiscard]] std::string_view ToString(AlarmConditionQuality quality) noexcept;

enum class AlarmLifecycleState {
  kActiveUnacknowledged,
  kActiveAcknowledged,
  kReturnedUnacknowledged,
  kReturnedAcknowledged,
};

enum class AlarmSnapshotOrdering {
  kDefault,
  kAttentionNewest,
};

[[nodiscard]] std::string_view ToString(AlarmLifecycleState state) noexcept;
[[nodiscard]] bool IsActive(AlarmLifecycleState state) noexcept;
[[nodiscard]] bool IsAcknowledged(AlarmLifecycleState state) noexcept;
[[nodiscard]] bool RequiresAttention(AlarmLifecycleState state) noexcept;

struct AlarmSubject final {
  std::string subject_kind;
  std::string subject_id;
  std::vector<std::string> resource_ids;

  friend bool operator==(const AlarmSubject &, const AlarmSubject &) = default;
};

struct AlarmConditionUpdate final {
  std::string definition_id;
  AlarmSubject subject;
  bool active;
  std::string source_id;
  std::string source_epoch;
  std::uint64_t source_sequence;
  std::int64_t observed_at_ns;
  std::string observed_at_wall;
  AlarmConditionQuality quality;
  manyforge::content_identity::CanonicalValue display_args;
  manyforge::content_identity::CanonicalValue details;
  std::vector<std::string> evidence_refs;
  manyforge::content_identity::CanonicalValue correlations;

  friend bool operator==(const AlarmConditionUpdate &,
                         const AlarmConditionUpdate &) = default;
};

struct AlarmSourceCursor final {
  std::string source_id;
  std::string source_epoch;
  std::uint64_t source_epoch_order;
  std::uint64_t source_sequence;
  std::string fingerprint;
  std::int64_t observed_at_ns;
};

struct AlarmCallerAttribution final {
  std::string request_id;
  std::string request_owner_epoch;
  std::string source_label;
  std::optional<std::string> correlation_id;
  std::string transport_kind;
  std::string adapter_id;
  std::string assurance;
  std::optional<std::string> trusted_principal_id;

  friend bool operator==(const AlarmCallerAttribution &,
                         const AlarmCallerAttribution &) = default;
};

/// Validate the common public-mutation attribution envelope shared by alarm
/// acknowledgement and the other H0 owner mutations.
void ValidateAlarmCallerAttribution(const AlarmCallerAttribution &caller);

struct OperatorAlarmRecord final {
  std::string catalog_schema_version;
  std::string catalog_version;
  std::string catalog_descriptor_digest;
  std::uint64_t definition_version;
  std::string definition_id;
  std::string condition_id;
  std::string activation_id;
  AlarmSubject subject;
  std::string alarm_class;
  std::string priority;
  std::string title;
  std::string message;
  std::string consequence;
  std::string operator_response;
  bool acknowledgement_required;
  bool requires_motion_attention;
  AlarmLifecycleState lifecycle_state;
  std::uint64_t activation_count;
  std::uint64_t occurrence_count;
  std::uint64_t record_revision;
  std::int64_t activated_at_ns;
  std::string activated_at_wall;
  std::int64_t first_seen_at_ns;
  std::string first_seen_at_wall;
  std::int64_t last_seen_at_ns;
  std::string last_seen_at_wall;
  std::optional<std::int64_t> returned_at_ns;
  std::optional<std::string> returned_at_wall;
  std::optional<std::int64_t> acknowledged_at_ns;
  std::optional<std::string> acknowledged_at_wall;
  std::optional<AlarmCallerAttribution> acknowledgement_attribution;
  std::optional<std::string> acknowledgement_reason;
  std::string source_id;
  std::string source_epoch;
  std::uint64_t source_sequence;
  AlarmConditionQuality source_quality;
  manyforge::content_identity::CanonicalValue display_args;
  manyforge::content_identity::CanonicalValue details;
  std::vector<std::string> evidence_refs;
  manyforge::content_identity::CanonicalValue correlations;
};

/// Whether `record` is a motion-attention member: a motion-attention
/// definition that requires acknowledgement gates while unacknowledged; one
/// that does not gates exactly while active.
[[nodiscard]] bool GatesMotion(const OperatorAlarmRecord &record) noexcept;

struct AlarmLifecycleFact final {
  std::string event_type;
  std::optional<std::string> definition_id;
  std::optional<std::string> condition_id;
  std::optional<std::string> activation_id;
  std::optional<AlarmSubject> subject;
  std::optional<AlarmLifecycleState> previous_state;
  std::optional<AlarmLifecycleState> current_state;
  std::optional<std::uint64_t> record_revision;
  std::uint64_t owner_generation;
  std::string owner_epoch;
  std::int64_t occurred_at_ns;
  std::string occurred_at_wall;
  std::optional<std::string> source_id;
  std::optional<std::string> source_epoch;
  std::optional<std::uint64_t> source_sequence;
  std::optional<AlarmCallerAttribution> caller;
  std::optional<std::string> reason;
  manyforge::content_identity::CanonicalValue correlations;
  manyforge::content_identity::CanonicalValue details;
};

struct AlarmPointState final {
  std::string condition_id;
  std::uint64_t activation_count{};
  std::optional<OperatorAlarmRecord> record;
  std::optional<AlarmSourceCursor> cursor;
};

struct AlarmTransitionResult final {
  AlarmPointState point;
  std::string status;
  bool changed;
  bool activation_changed;
  std::vector<AlarmLifecycleFact> lifecycle_facts;
  std::optional<OperatorAlarmRecord> terminal_record;
};

struct AlarmMutation final {
  std::string status;
  bool changed;
  std::uint64_t owner_generation;
  std::uint64_t state_revision;
  std::vector<OperatorAlarmRecord> records;
  std::vector<AlarmLifecycleFact> lifecycle_facts;
};

struct AlarmAcknowledgementMember final {
  std::string condition_id;
  std::string activation_id;
  std::uint64_t record_revision;

  friend bool operator==(const AlarmAcknowledgementMember &,
                         const AlarmAcknowledgementMember &) = default;
  friend bool operator<(const AlarmAcknowledgementMember &left,
                        const AlarmAcknowledgementMember &right) noexcept;
};

struct AlarmMotionAttentionMember final {
  AlarmAcknowledgementMember acknowledgement;
  AlarmLifecycleState lifecycle_state;
  std::string definition_id;
  AlarmSubject subject;
};

struct AlarmSourceDomainStatus final {
  std::string domain;
  bool required;
  bool reconciled;
  std::size_t source_count;
  std::size_t unknown_source_count;
};

struct OperatorAlarmSnapshot final {
  std::string catalog_schema_version;
  std::string catalog_version;
  std::string catalog_descriptor_digest;
  std::string owner_id;
  std::string owner_epoch;
  std::uint64_t generation;
  std::uint64_t state_revision;
  bool reconciled;
  std::map<std::string, std::string> health_errors;
  std::vector<AlarmSourceDomainStatus> domains;
  std::size_t live_count;
  std::size_t unacknowledged_count;
  std::size_t motion_attention_count;
  std::size_t active_count;
  std::size_t active_acknowledged_count;
  std::vector<AlarmAcknowledgementMember> unacknowledged_membership;
  std::vector<AlarmMotionAttentionMember> motion_attention_membership;
  std::map<std::string, std::size_t> counts_by_state;
  std::map<std::string, std::size_t> counts_by_priority;
  std::map<std::string, std::size_t> counts_by_class;
  std::vector<OperatorAlarmRecord> alarms;
  bool truncated;
  std::int64_t timestamp_ns;
};

/// One record that gates motion (`GatesMotion`) with its canonical subject.
/// A condition interlock (`acknowledgement_required=false`) gates exactly
/// while active; any other record gates while unacknowledged.
struct AlarmConfigurationIdentity final {
  std::string retained_bundle_id;
  std::string graph_revision;

  friend bool operator==(const AlarmConfigurationIdentity &,
                         const AlarmConfigurationIdentity &) = default;
};

struct AlarmMotionAttentionGate final {
  AlarmAcknowledgementMember acknowledgement;
  bool condition_interlock;
  AlarmSubject subject;
};

/// The generation-fenced motion-attention projection published to the
/// Kernel. The Kernel alone evaluates it against an operation's exact
/// reserved resources at admission (spec 450 §5.2, spec 530 §11).
struct AlarmMotionAttentionProjection final {
  std::string cell_id;
  std::string owner_epoch;
  /// This owner instance's incarnation (host boot id and CLOCK_BOOTTIME at
  /// construction): the Kernel accepts a new owner epoch only from a strictly
  /// later incarnation of the same host boot.
  manyforge::core::HostBootStamp owner_incarnation;
  std::uint64_t generation;
  /// False while unreconciled or unhealthy: no trustworthy scope exists.
  bool reconciled;
  /// The active configuration (retained bundle and graph revision) whose
  /// resource registry judged these subjects; absent before one is observed.
  /// The Kernel treats attention as available only when this equals its own
  /// active configuration (spec 530 §11).
  std::optional<AlarmConfigurationIdentity> active_configuration;
  /// Complete membership sorted by condition id.
  std::vector<AlarmMotionAttentionGate> members;
};

struct AlarmDurationSummary final {
  std::size_t count;
  std::uint64_t sum_ns;
  std::optional<std::uint64_t> minimum_ns;
  std::optional<std::uint64_t> maximum_ns;
  std::optional<std::uint64_t> p50_ns;
  std::optional<std::uint64_t> p95_ns;
  std::optional<std::uint64_t> p99_ns;
  bool sum_saturated{};
};

struct AlarmChatteringCandidate final {
  std::string condition_id;
  std::size_t activations_in_window;
};

struct OperatorAlarmMetrics final {
  std::string owner_id;
  std::string owner_epoch;
  std::uint64_t generation;
  std::uint64_t state_revision;
  std::map<std::string, std::uint64_t> totals;
  std::size_t live_condition_points;
  std::size_t live_alarms;
  std::size_t unacknowledged_attention;
  std::size_t motion_attention;
  std::size_t active_standing;
  std::size_t terminal_records;
  std::size_t unknown_quality_sources;
  std::map<std::string, std::size_t> counts_by_state;
  std::map<std::string, std::size_t> counts_by_priority;
  std::map<std::string, std::size_t> counts_by_class;
  AlarmDurationSummary ingest_latency_ns;
  AlarmDurationSummary acknowledgement_latency_ns;
  AlarmDurationSummary completed_standing_duration_ns;
  AlarmDurationSummary current_standing_duration_ns;
  bool alarm_flood_candidate;
  std::size_t activations_in_flood_window;
  std::vector<AlarmChatteringCandidate> chattering_candidates;
  bool chattering_candidates_truncated;
  std::int64_t timestamp_ns;
};

/// The resource inventory of the exact active configuration that Headless
/// observes through the Manager publication. Resource-scoped activations are
/// admitted only against this inventory; it starts empty and is replaced as a
/// whole when the active retained bundle changes.
struct AlarmResourceRegistry final {
  std::string retained_bundle_id;
  std::string graph_revision;
  std::set<std::string> resource_ids;
};

[[nodiscard]] std::string
CanonicalAlarmConditionId(const OperatorAlarmCatalog &catalog,
                          const OperatorAlarmDefinition &definition,
                          const AlarmSubject &subject);

[[nodiscard]] AlarmTransitionResult ReduceAlarmConditionUpdate(
    const OperatorAlarmCatalog &catalog,
    const OperatorAlarmDefinition &definition, const AlarmPointState &point,
    const AlarmConditionUpdate &update, std::uint64_t source_epoch_order,
    std::string activation_id);

[[nodiscard]] AlarmTransitionResult ReduceAlarmAcknowledgement(
    const OperatorAlarmDefinition &definition, const AlarmPointState &point,
    std::string_view activation_id, std::uint64_t expected_record_revision,
    const AlarmCallerAttribution &caller,
    const std::optional<std::string> &reason, std::int64_t acknowledged_at_ns,
    std::string acknowledged_at_wall);

class AlarmLifecycleOwner final {
public:
  using MonotonicClock = std::function<std::int64_t()>;
  using WallClock = std::function<std::string()>;

  AlarmLifecycleOwner(
      OperatorAlarmCatalog catalog, std::string cell_id, std::string owner_id,
      std::string owner_epoch, manyforge::core::HostBootStamp owner_incarnation,
      MonotonicClock monotonic_clock, WallClock wall_clock,
      std::set<std::string> required_domains = {"motion", "runtime_health",
                                                "scene"},
      std::size_t terminal_retention = kDefaultTerminalAlarmRetention,
      std::size_t condition_capacity = kDefaultAlarmConditionCapacity,
      std::size_t live_alarm_capacity = kDefaultLiveAlarmCapacity);

  void RegisterSource(std::string domain, std::string source_id,
                      std::string adapter_id, std::string source_epoch,
                      std::uint64_t source_epoch_order);
  void CompleteSourceSnapshot(std::string_view source_id,
                              std::string_view source_epoch,
                              AlarmConditionQuality quality);
  void MarkSourceQualityUnknown(std::string_view source_id,
                                std::string_view source_epoch);

  /// Replace the active-configuration resource inventory. Existing condition
  /// points are retained unchanged, including those naming resources that the
  /// new configuration no longer contains. A changed configuration identity
  /// advances the generation, so the projection names the new identity.
  void ReplaceResourceRegistry(AlarmResourceRegistry registry);

  [[nodiscard]] AlarmMutation Submit(AlarmConditionUpdate update);
  [[nodiscard]] AlarmMutation
  Acknowledge(std::string_view condition_id, std::string_view activation_id,
              std::uint64_t expected_record_revision,
              const AlarmCallerAttribution &caller,
              std::optional<std::string> reason, bool confirmed);
  [[nodiscard]] AlarmMutation
  AcknowledgeAll(std::vector<AlarmAcknowledgementMember> members,
                 std::string_view expected_owner_epoch,
                 std::uint64_t expected_generation,
                 const AlarmCallerAttribution &caller,
                 std::optional<std::string> reason, bool confirmed);

  [[nodiscard]] OperatorAlarmSnapshot Snapshot(
      std::optional<std::size_t> limit = std::nullopt,
      const std::set<AlarmLifecycleState> &states = {},
      AlarmSnapshotOrdering ordering = AlarmSnapshotOrdering::kDefault) const;
  /// Read one existing condition point under the owner lock; no new state is created.
  [[nodiscard]] std::optional<AlarmPointState>
  ConditionPoint(std::string_view definition_id, const AlarmSubject &subject) const;
  [[nodiscard]] std::vector<OperatorAlarmRecord>
  TerminalRecords(std::optional<std::size_t> limit = std::nullopt) const;
  /// The latest published motion-attention projection. The owner rebuilds
  /// it in the same critical section as every generation advance and swaps
  /// it in under its own short publication lock, so this read never waits on
  /// an owner mutation, a snapshot, or any caller's coordination lock.
  [[nodiscard]] std::shared_ptr<const AlarmMotionAttentionProjection>
  MotionAttentionProjection() const;
  [[nodiscard]] OperatorAlarmMetrics Metrics() const;

  /// Name an owner health fact. Restating an identical fact is a no-op; only
  /// a newly named code or a changed message advances the generation.
  void MarkHealthUnavailable(std::string code, std::string message);
  /// As `MarkHealthUnavailable`, but only when `still_unavailable` holds when
  /// evaluated inside the owner's critical section (it must be cheap and must
  /// not call back into the owner).
  void MarkHealthUnavailableIf(std::string code, std::string message,
                               const std::function<bool()> &still_unavailable);
  void ReconcileHealth(std::string_view code);

private:
  void MarkHealthUnavailableLocked(std::string code, std::string message);

  struct SourceRegistration final {
    std::string domain;
    std::string source_id;
    std::string adapter_id;
    std::string source_epoch;
    std::uint64_t source_epoch_order;
    AlarmConditionQuality quality{AlarmConditionQuality::kUnknown};
    bool snapshot_complete{};
  };

  [[nodiscard]] std::int64_t MonotonicNow() const;
  [[nodiscard]] std::string WallNow() const;
  [[nodiscard]] bool ReconciledLocked() const;
  [[nodiscard]] std::vector<AlarmAcknowledgementMember>
  UnacknowledgedMembershipLocked(bool motion_only) const;
  void EnsureLiveAlarmCapacityLocked(const AlarmPointState &current,
                                     const AlarmPointState &candidate);
  void EnsureConditionCapacityLocked(std::string_view condition_id);
  void AppendTerminalLocked(const OperatorAlarmRecord &record);
  void StampFactsLocked(std::vector<AlarmLifecycleFact> &facts) const;
  [[nodiscard]] AlarmMutation SubmitImpl(AlarmConditionUpdate update,
                                         std::uint64_t ingest_started_ns,
                                         std::int64_t received_at_ns);
  [[nodiscard]] AlarmMutation
  AcknowledgeImpl(std::string_view condition_id, std::string_view activation_id,
                  std::uint64_t expected_record_revision,
                  const AlarmCallerAttribution &caller,
                  std::optional<std::string> reason, bool confirmed);
  [[nodiscard]] AlarmMutation
  AcknowledgeAllImpl(std::vector<AlarmAcknowledgementMember> members,
                     std::string_view expected_owner_epoch,
                     std::uint64_t expected_generation,
                     const AlarmCallerAttribution &caller,
                     std::optional<std::string> reason, bool confirmed);
  void RecordRejectedUpdateLocked(std::string_view code);
  void RecordIngestLatencyLocked(std::uint64_t started_ns);
  /// Advance the owner generation and publish the projection it names.
  /// Called only after the critical section's state changes are complete.
  void AdvanceGenerationLocked();

  OperatorAlarmCatalog catalog_;
  std::string cell_id_;
  std::optional<AlarmResourceRegistry> resource_registry_;
  std::string owner_id_;
  std::string owner_epoch_;
  manyforge::core::HostBootStamp owner_incarnation_;
  MonotonicClock monotonic_clock_;
  WallClock wall_clock_;
  std::set<std::string> required_domains_;
  std::size_t terminal_retention_;
  std::size_t condition_capacity_;
  std::size_t live_alarm_capacity_;
  mutable std::mutex mutex_;
  std::map<std::string, AlarmPointState> points_;
  std::deque<OperatorAlarmRecord> terminal_records_;
  std::map<std::string, SourceRegistration> sources_;
  std::map<std::string, std::string> health_errors_;
  std::map<std::string, std::uint64_t> metrics_;
  std::deque<std::uint64_t> ingest_latency_ns_;
  std::deque<std::uint64_t> acknowledgement_latency_ns_;
  std::deque<std::uint64_t> completed_standing_duration_ns_;
  std::deque<std::int64_t> recent_activations_ns_;
  std::map<std::string, std::deque<std::int64_t>> condition_activations_ns_;
  std::uint64_t generation_{};
  std::uint64_t state_revision_{};
  std::uint64_t activation_serial_{};
  /// Guards only the swap of `motion_attention_`; never held across work.
  mutable std::mutex motion_attention_mutex_;
  std::shared_ptr<const AlarmMotionAttentionProjection> motion_attention_;
};

[[nodiscard]] manyforge::content_identity::CanonicalValue
OperatorAlarmRecordValue(const OperatorAlarmRecord &record);
[[nodiscard]] manyforge::content_identity::CanonicalValue
AlarmLifecycleFactValue(const AlarmLifecycleFact &fact);
[[nodiscard]] manyforge::content_identity::CanonicalValue
OperatorAlarmSnapshotValue(const OperatorAlarmSnapshot &snapshot);
[[nodiscard]] manyforge::content_identity::CanonicalValue
OperatorAlarmMetricsValue(const OperatorAlarmMetrics &metrics);
[[nodiscard]] manyforge::content_identity::CanonicalValue
OperatorAlarmMotionAttentionValue(
    const AlarmMotionAttentionProjection &projection);

} // namespace manyforge::headless
