#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

struct IoProgramBlackboardSnapshotDTO;
struct IoProgramBlackboardScopeDTO;

struct IoSourceIncarnationTag final {};
using IoSourceIncarnation = StrongString<IoSourceIncarnationTag>;

struct IoSignalId final {
  ControllableComponentId controllable_component_id;
  std::string name;
  friend auto operator<=>(const IoSignalId&, const IoSignalId&) = default;
};

enum class IoSignalValueType { kBoolean };
enum class IoSignalDirection { kInput, kOutput };
enum class IoSignalUpdateMode { kSampled, kChangeOnly };
enum class IoTransitionCapability { kSampledOnly, kCaptured };
enum class IoFreshnessMode { kSampleAge, kProviderValidity };
/// Receipt/liveness evidence is independent of whether a value changes.
enum class IoSignalLivenessMode { kInProcess, kPeriodic, kChangeOnly };

struct IoFreshnessContractDTO final {
  IoFreshnessMode mode;
  std::optional<Duration> maximum_age_ns;
  friend bool operator==(const IoFreshnessContractDTO&, const IoFreshnessContractDTO&) = default;
};

struct IoSignalDescriptorDTO final {
  IoSignalId signal_id;
  InterruptSourceId source_id;
  IoSignalValueType value_type;
  IoSignalDirection direction;
  IoSignalUpdateMode update_mode;
  IoTransitionCapability transition_capability;
  IoFreshnessContractDTO freshness;
  std::optional<ResourceId> output_resource;
  /// Required for inputs, absent for outputs. Source-kind qualification supplies
  /// this value; periodic inputs use freshness.maximum_age_ns as their sole budget.
  std::optional<IoSignalLivenessMode> liveness{};
  friend bool operator==(const IoSignalDescriptorDTO&, const IoSignalDescriptorDTO&) = default;
};

/// Owner identity and generation come from existing host/activation authority.
struct IoSignalCatalogDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t generation;
  std::vector<IoSignalDescriptorDTO> signals;
  /// Installed capability from sealed authoritative Mock provider profiles.
  /// Public samples/descriptors cannot grant this capability themselves.
  std::vector<InterruptSourceId> authoritative_synthetic_sources{};
};

/// Explicit installation bounds; this module supplies no product defaults.
struct IoSignalLimitsDTO final {
  std::size_t maximum_sources;
  std::size_t maximum_signals;
  std::size_t records_per_source;
  std::size_t maximum_watches;
  std::size_t maximum_identity_bytes;
  std::size_t maximum_error_bytes;
  /// Exact installed-source overrides; the common bound remains for direct callers.
  std::map<InterruptSourceId, std::size_t> records_per_source_overrides{};
};

enum class IoSampleValidity { kValid, kInvalid, kUnavailable };

struct IoSignalUpdateDTO final {
  IoSignalId signal_id;
  IoSampleValidity validity;
  std::optional<bool> value;
  std::optional<ErrorDTO> error;
  friend bool operator==(const IoSignalUpdateDTO&, const IoSignalUpdateDTO&) = default;
};

/// One acquisition from one source, applied atomically. The trusted in-process
/// receiver captures this IClock timestamp before enqueue, never from device/ROS
/// time or an external client. No cross-source simultaneity is implied.
struct IoSourceAcquisitionDTO final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t source_sequence;
  TimePoint trusted_received_at_ns;
  std::vector<IoSignalUpdateDTO> updates;
  friend bool operator==(const IoSourceAcquisitionDTO&, const IoSourceAcquisitionDTO&) = default;
};

/// Materialized per-signal evidence; acquisition is the only publication API.
struct IoSignalPublicationDTO final {
  IoSignalId signal_id;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t source_sequence;
  IoSampleValidity validity;
  std::optional<bool> value;
  std::optional<ErrorDTO> error;
  friend bool operator==(const IoSignalPublicationDTO&, const IoSignalPublicationDTO&) = default;
};

struct IoSignalSampleDTO final {
  IoSignalPublicationDTO publication;
  TimePoint received_at_ns;
  friend bool operator==(const IoSignalSampleDTO&, const IoSignalSampleDTO&) = default;
};

enum class IoSignalAvailability { kMissing, kFresh, kStale, kInvalid, kUnavailable };
enum class IoSourceLossReason {
  kSequenceGap,
  kDisconnected,
  kIncarnationChanged,
  kIngressOverflow,
  kIngressLoss
};

enum class IoIngressLossReason {
  kMiddlewareMessageLost,
  kHostIngressRejected,
  kSequenceExhausted,
  kDisconnected
};

struct IoIngressLossNotificationDTO final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  TimePoint trusted_received_at_ns;
  std::uint64_t after_ingress_source_sequence;
  IoIngressLossReason reason;
  std::optional<std::uint64_t> middleware_reported_lost_messages;
};

/// Bounded evidence coalescing, not an invented count or missing sequence range.
struct IoIngressLossBarrierDTO final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  TimePoint first_received_at_ns;
  TimePoint last_received_at_ns;
  std::uint64_t after_ingress_source_sequence;
  bool middleware_message_lost{};
  bool host_ingress_rejected{};
  bool sequence_exhausted{};
  bool disconnected{};
  bool coalesced{};
  std::optional<std::uint64_t> middleware_reported_lost_messages;
  friend bool operator==(const IoIngressLossBarrierDTO&, const IoIngressLossBarrierDTO&) = default;
};

struct IoSourceLossDTO final {
  IoSourceLossReason reason;
  std::uint64_t last_accepted_source_sequence;
  std::optional<std::uint64_t> first_lost_source_sequence;
  std::optional<std::uint64_t> last_lost_source_sequence;
  std::optional<ErrorDTO> error;
  std::optional<IoIngressLossBarrierDTO> ingress_barrier;
  friend bool operator==(const IoSourceLossDTO&, const IoSourceLossDTO&) = default;
};

/// Host record ordering includes availability changes with no provider sample.
struct IoSourceCursorDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t catalog_generation;
  InterruptSourceId source_id;
  std::uint64_t record_sequence;
  friend bool operator==(const IoSourceCursorDTO&, const IoSourceCursorDTO&) = default;
};

struct IoSignalRecordDTO final {
  IoSourceCursorDTO cursor;
  IoSourceIncarnation incarnation;
  std::optional<IoSourceAcquisitionDTO> acquisition;
  std::optional<IoSourceLossDTO> loss;
  std::shared_ptr<const IoProgramBlackboardSnapshotDTO> blackboard_snapshot{};
  /// Exact metadata of an actually retired source, within existing retention.
  std::shared_ptr<const IoProgramBlackboardScopeDTO> retired_blackboard_scope{};
  friend bool operator==(const IoSignalRecordDTO&, const IoSignalRecordDTO&);
};

struct IoSignalObservationDTO final {
  IoSignalDescriptorDTO descriptor;
  IoSignalAvailability availability;
  std::optional<IoSignalSampleDTO> latest_sample;
  std::optional<IoSourceIncarnation> installed_incarnation;
  IoSourceCursorDTO cursor;
  std::uint64_t continuity_loss_generation;
  std::optional<ErrorDTO> source_error;
  TimePoint evaluated_at_ns{};
};

/// Immutable routing/bounds snapshot for the existing host ingress lifetime.
struct IoSignalIngressSourceDTO final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t last_source_sequence;
  std::optional<TimePoint> last_received_at_ns;
  std::size_t records_per_source{};
};
struct IoSignalIngressConfigurationDTO final {
  IoSignalLimitsDTO limits;
  std::vector<IoSignalIngressSourceDTO> sources;
};

struct IoSignalRecordPageDTO final {
  std::vector<IoSignalRecordDTO> records;
  IoSourceCursorDTO next_cursor;
  IoSourceCursorDTO latest_cursor;
  bool retention_gap;
};

struct IoWatchScopeDTO final {
  RequestOwnerEpoch program_runner_owner_epoch;
  ProgramRunId program_run_id;
  ProgramNodeExecutionScopeDTO node;
  friend bool operator==(const IoWatchScopeDTO&, const IoWatchScopeDTO&) = default;
};

/// Initial internal slice supports a single-signal level condition only.
struct IoLevelWatchRequestDTO final {
  IoWatchScopeDTO scope;
  IoSignalId signal_id;
  bool expected_value;
};

struct IoWatchRegistrationDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t catalog_generation;
  std::uint64_t registration_id;
  IoWatchScopeDTO scope;
  friend bool operator==(const IoWatchRegistrationDTO&, const IoWatchRegistrationDTO&) = default;
};

enum class IoWatchState { kPending, kMatched, kReleased };

struct IoWatchResultDTO final {
  IoWatchState state;
  std::optional<IoSignalObservationDTO> observation;
  std::optional<IoSignalSampleDTO> matched_sample;
};

}  // namespace manyforge::core
