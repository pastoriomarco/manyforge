#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumStopFaultRecoveryEvidenceRefs = 4096U;
inline constexpr std::size_t kMaximumStopSurveillanceSources = 262144U;
inline constexpr std::size_t kMaximumStopSurveillanceCheckpoints = 4096U;

enum class StopForensicProjectionStateDTO : std::uint8_t {
  kRecorded = 0,
  kFailed,
};

struct StopForensicProjectionResultDTO final {
  StopForensicProjectionStateDTO state{StopForensicProjectionStateDTO::kFailed};
  std::optional<std::uint64_t> journal_sequence;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopForensicProjectionResultDTO&,
                         const StopForensicProjectionResultDTO&) = default;
};

enum class StopStatusStateDTO : std::uint8_t {
  kAccepted = 0,
  kStopping,
  kCertifying,
  kSucceeded,
  kPartial,
  kFailed,
};

struct StopCurrentStageDTO final {
  StopScopeEntryKeyDTO scope_key;
  StopEscalationStageId stop_escalation_stage_id;
  StopEscalationStageKind stage_kind{StopEscalationStageKind::kNominal};
  std::size_t stage_index{};
  RemainingStopBudgetDTO remaining_budget;

  friend bool operator==(const StopCurrentStageDTO&, const StopCurrentStageDTO&) = default;
};

enum class StopFaultTriggerDTO : std::uint8_t {
  kScopePromotion = 0,
  kEscalated,
  kPartial,
  kFailedUncontained,
  kPhysicalRestUnproven,
  kPersistenceDegraded,
};

enum class StopFaultLatchStateDTO : std::uint8_t {
  kLatched = 0,
  kRecoveryPending,
  kCleared,
};

enum class StopFaultRecoveryStatusDTO : std::uint8_t {
  kWaitingForObservation = 0,
  kWaitingForQuiescence,
  kWaitingForConfiguration,
  kWaitingForExternalReconciliation,
  kWaitingForDurableRecord,
  kReady,
  kRecoveryPending,
  kCleared,
};

enum class StopFaultRecoveryEvidenceKindDTO : std::uint8_t {
  kSettlement = 0,
  kLateQuiescence,
  kConfiguration,
  kExternalReconciliation,
  kBackendOrLifecycleReset,
};

struct StopFaultRecoveryEvidenceRefDTO final {
  StopFaultRecoveryEvidenceKindDTO kind{StopFaultRecoveryEvidenceKindDTO::kSettlement};
  ContentDigest evidence_digest;

  friend bool operator==(const StopFaultRecoveryEvidenceRefDTO&,
                         const StopFaultRecoveryEvidenceRefDTO&) = default;
};

struct StopFaultLatchDTO final {
  CellId cell_id;
  StopId stop_id;
  StopGeneration stop_generation;
  std::optional<StopCertificateRefDTO> stop_certificate;
  StopFaultLatchId latch_id;
  StopFaultLatchGeneration latch_generation;
  StopFaultTriggerDTO trigger{StopFaultTriggerDTO::kPersistenceDegraded};
  bool persistence_degraded{};
  std::vector<OperationId> captured_operation_ids;
  std::vector<ResourceId> captured_resources;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  StopDurableProjectionStatusDTO terminal_projection;
  StopDurableProjectionStatusDTO clear_projection;
  StopFaultLatchStateDTO state{StopFaultLatchStateDTO::kLatched};
  std::uint64_t recovery_assessment_revision{};
  StopFaultRecoveryStatusDTO recovery_status{StopFaultRecoveryStatusDTO::kWaitingForObservation};
  std::string reason_code;
  std::vector<StopFaultRecoveryEvidenceRefDTO> evidence_refs;

  friend bool operator==(const StopFaultLatchDTO&, const StopFaultLatchDTO&) = default;
};

struct StopFaultRecoveryConfigurationExpectationDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;

  friend bool operator==(const StopFaultRecoveryConfigurationExpectationDTO&,
                         const StopFaultRecoveryConfigurationExpectationDTO&) = default;
};

/// Atomic current fault/recovery context. Before a certificate exists this
/// carries only the waiting latch and an empty configuration table: it does
/// not authorize recovery. The actionable table derives from the same exact
/// resident terminal status, never from caller input or separate reads.
struct StopFaultRecoveryOfferDTO final {
  StopFaultLatchDTO latch;
  std::vector<StopFaultRecoveryConfigurationExpectationDTO> configuration_expectations;

  friend bool operator==(const StopFaultRecoveryOfferDTO&,
                         const StopFaultRecoveryOfferDTO&) = default;
};

struct StopFaultRecoveryIntentDTO final {
  PublicRequestContextDTO request_context;
  StopFaultLatchId expected_latch_id;
  StopFaultLatchGeneration expected_latch_generation;
  std::uint64_t expected_recovery_assessment_revision{};
  StopCertificateId expected_stop_certificate_id;
  StopCertificateRevision expected_stop_certificate_revision;
  CellId expected_cell_id;
  RetainedBundleId expected_retained_bundle_id;
  GraphRevision expected_graph_revision;
  std::vector<StopFaultRecoveryConfigurationExpectationDTO> configuration_expectations;
  std::optional<std::string> reason;

  friend bool operator==(const StopFaultRecoveryIntentDTO&,
                         const StopFaultRecoveryIntentDTO&) = default;
};

struct StopFaultRecoveryResultDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
  bool accepted{};
  StopFaultLatchDTO latch;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopFaultRecoveryResultDTO&,
                         const StopFaultRecoveryResultDTO&) = default;
};

struct StopSurveillancePolicyDTO final {
  std::int64_t sample_cadence_ns{};
  std::int64_t checkpoint_interval_ns{};
  std::size_t retained_checkpoint_limit{};

  friend bool operator==(const StopSurveillancePolicyDTO&,
                         const StopSurveillancePolicyDTO&) = default;
};

enum class StopSurveillanceSourceAvailabilityDTO : std::uint8_t {
  kAvailable = 0,
  kUnavailable,
  kInvalid,
};

struct StopSurveillanceSourceStatusDTO final {
  StopScopeEntryKeyDTO scope_key;
  StopEvidenceItemId evidence_item_id;
  ResolvedStopEvidenceSourceDTO resolved_source;
  StopSurveillanceSourceAvailabilityDTO availability{
      StopSurveillanceSourceAvailabilityDTO::kUnavailable};
  std::optional<RuntimeEpoch> source_epoch;
  std::optional<std::uint64_t> source_sequence;
  std::optional<std::int64_t> source_age_ns;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopSurveillanceSourceStatusDTO&,
                         const StopSurveillanceSourceStatusDTO&) = default;
};

enum class StopPostTerminalSurveillanceStateDTO : std::uint8_t {
  kMonitoring = 0,
  kQuiescenceObserved,
};

struct StopPostTerminalSurveillanceDTO final {
  StopId stop_id;
  StopGeneration stop_generation;
  StopCertificateRefDTO stop_certificate;
  StopFaultLatchId latch_id;
  StopFaultLatchGeneration latch_generation;
  RequestOwnerEpoch owner_epoch;
  std::uint64_t surveillance_generation{};
  std::vector<ResourceId> resources;
  std::vector<StopSurveillanceSourceStatusDTO> source_statuses;
  StopSurveillancePolicyDTO policy;
  std::uint64_t continued_motion_count{};
  std::optional<std::int64_t> first_continued_motion_at_ns;
  std::optional<std::int64_t> last_continued_motion_at_ns;
  std::optional<ContentDigest> late_quiescence_evidence_digest;
  std::optional<std::int64_t> late_quiescence_observed_at_ns;
  StopPostTerminalSurveillanceStateDTO state{StopPostTerminalSurveillanceStateDTO::kMonitoring};
  std::uint64_t checkpoint_generation{};
  StopDurableProjectionStatusDTO checkpoint_projection;

  friend bool operator==(const StopPostTerminalSurveillanceDTO&,
                         const StopPostTerminalSurveillanceDTO&) = default;
};

struct StopStatusDTO final {
  StopAcceptedDTO accepted;
  StopStatusStateDTO state{StopStatusStateDTO::kAccepted};
  std::vector<StopScopeEntryDTO> scope_entries;
  std::vector<StopCurrentStageDTO> current_stages;
  std::vector<StopAttemptedStageResultDTO> attempted_stage_results;
  std::vector<StopCertificateEvidenceItemDTO> evidence_items;
  std::vector<StopScopeEntryKeyDTO> unsettled;
  std::vector<StopScopeEntryKeyDTO> failed;
  StopDurableProjectionStatusDTO acceptance_projection;
  StopDurableProjectionStatusDTO terminal_projection;
  StopDurableProjectionStatusDTO surveillance_projection;
  std::optional<StopCertificateRefDTO> stop_certificate;
  std::optional<StopFaultLatchDTO> fault_latch;
  std::optional<StopPostTerminalSurveillanceDTO> surveillance;

  friend bool operator==(const StopStatusDTO&, const StopStatusDTO&) = default;
};

[[nodiscard]] std::optional<std::string_view> stop_forensic_projection_result_field_error(
    const StopForensicProjectionResultDTO& result) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_fault_latch_field_error(
    const StopFaultLatchDTO& latch) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_fault_recovery_offer_field_error(
    const StopFaultRecoveryOfferDTO& offer) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_post_terminal_surveillance_field_error(
    const StopPostTerminalSurveillanceDTO& surveillance) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_status_field_error(
    const StopStatusDTO& status) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_terminal_projection_field_error(
    const StopStatusDTO& status, const StopCertificateDTO& certificate,
    const std::optional<StopFaultLatchDTO>& fault_latch) noexcept;

}  // namespace manyforge::core
