#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/emergency_stop_acceptance_reserve.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_terminal_state.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_operation_diagnostics.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

enum class EmergencyStopEntryStateDTO {
  kActive,
  kTerminal,
};

/// Exact internal record already linearized by the Kernel emergency reserve.
/// The journal is only a forensic projection of this fact. A normalized
/// request has a digest and no malformed reason; adapter-recognized malformed
/// ingress has the inverse shape and is conservatively represented by the
/// monostate intent alternative below.
struct EmergencyStopAcceptanceRecordDTO final {
  RequestOwnerEpoch owner_epoch;
  RequestId request_id;
  std::optional<ContentDigest> normalized_request_digest;
  std::optional<std::string> malformed_ingress_reason;
  RequestAttributionDTO attribution;
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
  StopId stop_id;
  StopGeneration stop_generation{0U};
  std::int64_t accepted_at_ns{};
  StopResolutionDispositionDTO resolution_disposition{
      StopResolutionDispositionDTO::kPromotedToFull};
  ContentDigest captured_scope_digest;
  EmergencyStopEntryStateDTO entry_state{EmergencyStopEntryStateDTO::kActive};
  EmergencyStopAcceptancePathDTO acceptance_path{EmergencyStopAcceptancePathDTO::kFreshReserved};
  EmergencyStopProjectionStateDTO projection_state{EmergencyStopProjectionStateDTO::kPending};

  friend bool operator==(const EmergencyStopAcceptanceRecordDTO&,
                         const EmergencyStopAcceptanceRecordDTO&) = default;
};

/// The empty alternative is legal only for recognized malformed ingress. The
/// record retains its adapter-minted identity, attribution, and stable reason.
using EmergencyStopProjectedIntentDTO =
    std::variant<std::monostate, StopIntentDTO, FullStopIntentDTO>;

/// One immutable canonical capture taken at the Stop linearization boundary.
/// Resources include affected externally uncertain resources even when no
/// resident operation can represent them. Operations and resources are sorted
/// and unique; every operation is complete and byte-for-byte owner state.
struct EmergencyStopAffectedStateSnapshotDTO final {
  std::vector<OperationStateDTO> operations;
  std::vector<ResourceId> resources;
  /// Diagnostic observation of the accepted, unsent takeover candidate. It
  /// confers no physical affected scope or operation-snapshot authority.
  std::optional<OperationStateDTO> motion_takeover_candidate_snapshot{};

  friend bool operator==(const EmergencyStopAffectedStateSnapshotDTO&,
                         const EmergencyStopAffectedStateSnapshotDTO&) = default;
};

/// Asynchronous forensic projection result. `failed` is not a rejected Stop:
/// containment continues and the Kernel closes admission through its
/// persistence-degraded fault path.
struct EmergencyStopAcceptanceProjectionResultDTO final {
  EmergencyStopProjectionStateDTO projection_state{EmergencyStopProjectionStateDTO::kFailed};
  std::optional<std::uint64_t> journal_sequence;
  std::optional<ErrorDTO> error;

  friend bool operator==(const EmergencyStopAcceptanceProjectionResultDTO&,
                         const EmergencyStopAcceptanceProjectionResultDTO&) = default;
};

/// Complete durable Stop truth recovered at one owner-epoch boundary. The
/// acceptance is always present. Terminal truth is present only after its
/// atomic projection committed. The
/// affected-state capture stays in the acceptance journal row only (journal
/// v17); recovery has no reader for it.
struct RecoveredStopJournalStateDTO final {
  StopAcceptedDTO accepted;
  std::optional<StopCertificateDTO> terminal_certificate;

  friend bool operator==(const RecoveredStopJournalStateDTO&,
                         const RecoveredStopJournalStateDTO&) = default;
};

/// Trusted, internal reconciliation result.  The public reconcile DTO is only
/// evidence input; this result is the exact authority consumed by Kernel and
/// its mechanical journal.
using CellAdmissionReconcileSettlementAuthorizationDTO = CellAdmissionReconcileSettlementDTO;

struct CellAdmissionReconcileValidationDTO final {
  std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owner_epochs;
  std::vector<CellAdmissionReconcileSettlementAuthorizationDTO> settlements;

  friend bool operator==(const CellAdmissionReconcileValidationDTO&,
                         const CellAdmissionReconcileValidationDTO&) = default;
};

struct OperationJournalRecoveryDTO final {
  std::vector<OperationStateDTO> historical_operations;
  std::vector<RecoveredStopJournalStateDTO> recovered_stops;
  std::optional<RequestOwnerEpoch> replaced_unclosed_owner_epoch;
  std::vector<ResourceId> unreconciled_resources;
  AdmissionGeneration cell_admission_generation_high_water{0U};
  CellAdmissionClaimGeneration cell_claim_generation_high_water{0U};
  struct RecoveredCellAdmissionClaimDTO final {
    CellAdmissionClaimDTO claim;
    std::optional<CellAdmissionClaimReleaseDTO> release;
    std::optional<CellAdmissionReconcileSettlementDTO> reconcile_settlement;

    friend bool operator==(const RecoveredCellAdmissionClaimDTO&,
                           const RecoveredCellAdmissionClaimDTO&) = default;
  };
  std::vector<RecoveredCellAdmissionClaimDTO> recovered_cell_admission_claims;
  struct RecoveredCellAdmissionMembershipDTO final {
    CellAdmissionOwnerRoleDTO owner_role{CellAdmissionOwnerRoleDTO::kAuthoringOwner};
    RequestOwnerEpoch owner_epoch;
    RequestId request_id;
    ContentDigest semantic_intent_digest;

    friend bool operator==(const RecoveredCellAdmissionMembershipDTO&,
                           const RecoveredCellAdmissionMembershipDTO&) = default;
  };
  std::vector<RecoveredCellAdmissionMembershipDTO> recovered_cell_admission_membership;
  std::vector<CellAdmissionCurrentOwnerEpochDTO> recovered_cell_admission_current_owners;
};

/// Durable boundary owned by the resource-addressed Execution Kernel.
///
/// Ordinary acceptance methods commit the complete public intent,
/// owner-produced result, and resulting immutable state before their
/// subordinate effect. Stop acceptance is deliberately different: the Kernel
/// has already linearized/fenced it, and this port only attempts its forensic
/// projection. Lifecycle/error transitions are committed before they become
/// visible through owner projections. Progress-only observations update the
/// owner's live snapshot; they do not require one durable record per sample.
class IOperationJournal {
 public:
  virtual ~IOperationJournal() = default;

  [[nodiscard]] virtual Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_capacity, const std::vector<ResourceId>& resource_inventory) = 0;

  /// Atomically retain the new acceptance and compact only resolved history
  /// outside the live owner's sorted active/unresolved operation inventory.
  [[nodiscard]] virtual Result<std::uint64_t> RecordOperationAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
      const OperationAcceptedDTO& accepted, const OperationStateDTO& state,
      std::span<const OperationId> protected_operation_ids = {}) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordControlAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
      const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) = 0;

  [[nodiscard]] virtual EmergencyStopAcceptanceProjectionResultDTO RecordStopAccepted(
      const RequestOwnerEpoch& owner_epoch, const EmergencyStopAcceptanceRecordDTO& acceptance,
      const EmergencyStopProjectedIntentDTO& intent, const StopAcceptedDTO& accepted,
      const EmergencyStopAffectedStateSnapshotDTO& affected_state) noexcept = 0;

  /// Atomically projects one complete terminal Stop status and its immutable
  /// certificate. Exact replay returns the original sequence; conflicting
  /// replay fails without modifying durable truth.
  [[nodiscard]] virtual StopForensicProjectionResultDTO RecordStopTerminal(
      const RequestOwnerEpoch& owner_epoch, const StopStatusDTO& stop_status,
      const StopCertificateDTO& certificate) noexcept = 0;

  /// The sole durable clean-owner marker. It succeeds only when no journaled
  /// operation, Stop projection, or orphan remains unresolved.
  [[nodiscard]] virtual Result<std::uint64_t> CloseOwnerEpoch(const RequestOwnerEpoch& owner_epoch,
                                                              std::int64_t closed_at_ns) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordRuntimeScaleAccepted(
      const RequestOwnerEpoch& owner_epoch, const RuntimeScaleRequestDTO& request,
      const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO& scale_status) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO* scale_status, const char* transition_reason,
      std::span<const PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics = {}) = 0;

  /// Forensic projection of an actual Kernel-owned global rule transition.
  /// This never installs a rule or grants Stop/latch authority. The activation
  /// owner preserves transition order and atomic baseline/sequence capture.
  [[nodiscard]] virtual Result<std::uint64_t> RecordInterruptRuleStateChanged(
      const IoRuleStateChangeDTO&) {
    return Result<std::uint64_t>::Err({"unsupported_capability",
                                       "Global rule-state journal projection is not installed",
                                       {{"reason", "io_rule_state_journal_unsupported"}}});
  }

  /// Active TCP: one confirmed Active TCP change, the
  /// operational `active_tcp_changed` row (the Runner's mirror and History
  /// read it). The robot owner appends it under its own lock; the returned
  /// sequence is its place in the stream.
  [[nodiscard]] virtual Result<std::uint64_t> RecordActiveTcpChanged(
      const RequestOwnerEpoch&, const ActiveTcpChangeDTO&) {
    return Result<std::uint64_t>::Err({"unsupported_capability",
                                       "Active TCP journal projection is not installed",
                                       {{"reason", "active_tcp_journal_unsupported"}}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordCellAdmissionClaimAcquired(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO&) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "the operation journal does not implement durable cell admission",
                 {}});
  }

  /// Trusted owner adaptation, using the existing ordinary claim tables.
  [[nodiscard]] virtual Result<CellAdmissionClaimDTO> RecordConfigurationApplyClaimAcquired(
      const RequestOwnerEpoch& kernel_owner_epoch, const CellAdmissionClaimDTO& fresh_claim,
      const CellAdmissionCurrentOwnerEpochDTO& verified_manager,
      const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners) {
    static_cast<void>(kernel_owner_epoch);
    static_cast<void>(fresh_claim);
    static_cast<void>(verified_manager);
    static_cast<void>(expected_held_claims);
    static_cast<void>(expected_current_owners);
    return Result<CellAdmissionClaimDTO>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "configuration Apply journal adaptation is unavailable",
                 {}});
  }
  [[nodiscard]] virtual Result<AdmissionGeneration> RecordTerminalApplyClaimsReleasedBySuccess(
      const RequestOwnerEpoch& kernel_owner_epoch, const CellAdmissionClaimDTO& fresh_claim,
      const CellAdmissionClaimReleaseDTO& fresh_success_release,
      const std::vector<CellAdmissionClaimDTO>& expected_held_claims,
      const std::vector<CellAdmissionCurrentOwnerEpochDTO>& expected_current_owners,
      AdmissionGeneration expected_admission_generation) {
    static_cast<void>(kernel_owner_epoch);
    static_cast<void>(fresh_claim);
    static_cast<void>(fresh_success_release);
    static_cast<void>(expected_held_claims);
    static_cast<void>(expected_current_owners);
    static_cast<void>(expected_admission_generation);
    return Result<AdmissionGeneration>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "configuration Apply journal adaptation is unavailable",
                 {}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordCellAdmissionClaimReleased(
      const RequestOwnerEpoch&, const CellAdmissionClaimDTO&, const CellAdmissionClaimReleaseDTO&,
      AdmissionGeneration) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "the operation journal does not implement durable cell admission",
                 {}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordCellAdmissionClaimsReconciled(
      const RequestOwnerEpoch&, const CellAdmissionReconcileDTO&,
      const CellAdmissionReconcileValidationDTO&, AdmissionGeneration) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "the operation journal does not implement durable cell admission",
                 {}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordCellAdmissionGenerationAdvanced(
      const RequestOwnerEpoch&, AdmissionGeneration, AdmissionGeneration) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{"operation_journal_cell_admission_unavailable",
                 "the operation journal does not implement durable cell admission",
                 {}});
  }
};

}  // namespace manyforge::core
