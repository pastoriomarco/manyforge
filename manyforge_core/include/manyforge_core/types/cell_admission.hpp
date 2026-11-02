#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumCellAdmissionClaims = 4096U;
inline constexpr std::size_t kMaximumCellAdmissionScopeEntries = 4096U;
inline constexpr std::size_t kMaximumCellAdmissionIdentityBytes = 128U;
inline constexpr std::size_t kMaximumProgramResetCallerIdentityBytes = 256U;
inline constexpr std::size_t kMaximumCellAdmissionPrivateBinaryBytes =
    256U * 1024U;
inline constexpr std::size_t kMaximumCellAdmissionJsonRequestBytes =
    1024U * 1024U;
inline constexpr std::size_t kMaximumCellAdmissionJsonResponseBytes =
    4U * 1024U * 1024U;

enum class CellAdmissionClaimKindDTO {
  kProgramCycle,
  kAuthoringMutation,
  kManagerApply,
  kAuthoredScene,
  kManualOperation,
  kInterventionOrRecovery,
  kProgramReset,
};

enum class CellAdmissionOwnerRoleDTO {
  kProgramRunner,
  kAuthoringOwner,
  kManager,
  kAuthoredSceneOwner,
  kManualOperationOwner,
  kInterventionOrRecoveryOwner,
};

enum class ProgramCycleStateDTO {
  kIdleAfterReset,
  kStarting,
  kRunning,
  kStopping,
  kTerminalAwaitingReset,
  kInterventionOrRecovery,
  kResetting,
  kRecoveryRequired,
};

enum class CellAdmissionSettlementKindDTO {
  kOwnerCommit,
  kProvenNoEffect,
  kTerminalEffectsSettled,
  kExplicitReset,
  kReconciled,
};

enum class CellAdmissionRecoveryReceiptStatusDTO {
  kUnknown,
  kConflicting,
  kHeldExact,
  kReleasedExact,
  kReconciledExact,
};

[[nodiscard]] std::string_view to_string(CellAdmissionClaimKindDTO value) noexcept;
[[nodiscard]] std::string_view to_string(CellAdmissionOwnerRoleDTO value) noexcept;
[[nodiscard]] std::string_view to_string(ProgramCycleStateDTO value) noexcept;
[[nodiscard]] std::string_view to_string(CellAdmissionSettlementKindDTO value) noexcept;
[[nodiscard]] std::string_view to_string(
    CellAdmissionRecoveryReceiptStatusDTO value) noexcept;

struct CellAdmissionConfigurationScopeDTO final {
  ControlDomainId control_domain_id;
  ActiveControlConfigurationId active_configuration_id;
  ActiveControlConfigurationGeneration active_configuration_generation;

  friend bool operator==(const CellAdmissionConfigurationScopeDTO&,
                         const CellAdmissionConfigurationScopeDTO&) = default;
};

struct CellAdmissionScopeDTO final {
  std::vector<ResourceId> resources;
  std::vector<CellAdmissionConfigurationScopeDTO> configurations;

  friend bool operator==(const CellAdmissionScopeDTO&, const CellAdmissionScopeDTO&) = default;
};

struct CellAdmissionInterventionEvidenceDTO final {
  CellAdmissionOwnerRoleDTO offering_owner_role{
      CellAdmissionOwnerRoleDTO::kInterventionOrRecoveryOwner};
  RequestOwnerEpoch offering_owner_epoch;
  std::string offer_transaction_id;
  ContentDigest offer_digest;
  std::optional<CellAdmissionClaimGeneration> parent_program_cycle_claim_generation;

  friend bool operator==(const CellAdmissionInterventionEvidenceDTO&,
                         const CellAdmissionInterventionEvidenceDTO&) = default;
};

struct ProgramCycleAdmissionEvidenceDTO final {
  ProgramRevision draft_program_revision;
  ProgramRevision applied_program_revision;

  friend bool operator==(const ProgramCycleAdmissionEvidenceDTO&,
                         const ProgramCycleAdmissionEvidenceDTO&) = default;
};

/// One runtime Scene/CCLC transaction, attributed to its sealed Program node.
/// This is a child of the existing cycle claim, never authoring permission.
struct ProgramNodeAdmissionEvidenceDTO final {
  RequestOwnerEpoch program_runner_owner_epoch;
  ProgramRunId program_run_id;
  CellAdmissionClaimGeneration parent_program_cycle_claim_generation;
  std::string node_instance_id;
  std::string node_attempt_id;
  std::string owner_api;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ProgramNodeAdmissionEvidenceDTO&,
                         const ProgramNodeAdmissionEvidenceDTO&) = default;
};

struct CellAdmissionClaimIntentDTO final {
  CellAdmissionClaimKindDTO claim_kind{CellAdmissionClaimKindDTO::kAuthoringMutation};
  CellAdmissionOwnerRoleDTO owner_role{CellAdmissionOwnerRoleDTO::kAuthoringOwner};
  RequestOwnerEpoch owner_epoch;
  std::string owner_transaction_id;
  RequestId request_id;
  ContentDigest request_payload_digest;
  AdmissionGeneration expected_admission_generation{0U};
  CellAdmissionScopeDTO scope;
  std::optional<ProgramCycleAdmissionEvidenceDTO> program_cycle_evidence;
  std::optional<CellAdmissionInterventionEvidenceDTO> intervention_evidence;
  std::optional<ProgramNodeAdmissionEvidenceDTO> program_node_evidence{};

  friend bool operator==(const CellAdmissionClaimIntentDTO&,
                         const CellAdmissionClaimIntentDTO&) = default;
};

[[nodiscard]] inline std::optional<CellAdmissionClaimGeneration> parent_program_cycle_claim(
    const CellAdmissionClaimIntentDTO& intent) noexcept {
  if (intent.program_node_evidence.has_value())
    return intent.program_node_evidence->parent_program_cycle_claim_generation;
  return intent.intervention_evidence.has_value()
             ? intent.intervention_evidence->parent_program_cycle_claim_generation
             : std::nullopt;
}

struct CellAdmissionClaimDTO final {
  CellAdmissionClaimIntentDTO intent;
  CellAdmissionClaimGeneration claim_generation{0U};
  AdmissionGeneration acquired_admission_generation{0U};
  std::int64_t accepted_at_ns{};

  friend bool operator==(const CellAdmissionClaimDTO&, const CellAdmissionClaimDTO&) = default;
};

struct CellAdmissionSettlementEvidenceDTO final {
  CellAdmissionSettlementKindDTO settlement_kind{
      CellAdmissionSettlementKindDTO::kTerminalEffectsSettled};
  ContentDigest evidence_digest;

  friend bool operator==(const CellAdmissionSettlementEvidenceDTO&,
                         const CellAdmissionSettlementEvidenceDTO&) = default;
};

struct CellAdmissionClaimReleaseDTO final {
  CellAdmissionOwnerRoleDTO owner_role{CellAdmissionOwnerRoleDTO::kAuthoringOwner};
  RequestOwnerEpoch owner_epoch;
  std::string owner_transaction_id;
  RequestId request_id;
  ContentDigest request_payload_digest;
  AdmissionGeneration acquired_admission_generation{0U};
  CellAdmissionClaimGeneration claim_generation{0U};
  CellAdmissionSettlementEvidenceDTO settlement;

  friend bool operator==(const CellAdmissionClaimReleaseDTO&,
                         const CellAdmissionClaimReleaseDTO&) = default;
};

struct CellAdmissionCurrentOwnerEpochDTO final {
  CellAdmissionOwnerRoleDTO owner_role{CellAdmissionOwnerRoleDTO::kAuthoringOwner};
  RequestOwnerEpoch owner_epoch;

  friend bool operator==(const CellAdmissionCurrentOwnerEpochDTO&,
                         const CellAdmissionCurrentOwnerEpochDTO&) = default;
};

struct CellAdmissionReconcileDTO final {
  RequestOwnerEpoch kernel_owner_epoch;
  std::vector<CellAdmissionClaimDTO> retained_claims;
  std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owner_epochs;
  ContentDigest reconciliation_evidence_digest;

  friend bool operator==(const CellAdmissionReconcileDTO&,
                         const CellAdmissionReconcileDTO&) = default;
};

struct CellAdmissionReconcileSettlementDTO final {
  CellAdmissionClaimDTO predecessor_claim;
  CellAdmissionOwnerRoleDTO successor_owner_role{
      CellAdmissionOwnerRoleDTO::kAuthoringOwner};
  RequestOwnerEpoch successor_owner_epoch;
  CellAdmissionSettlementKindDTO settlement_predicate{
      CellAdmissionSettlementKindDTO::kReconciled};
  ContentDigest reconciliation_evidence_digest;

  friend bool operator==(const CellAdmissionReconcileSettlementDTO&,
                         const CellAdmissionReconcileSettlementDTO&) = default;
};

/// Private recovery lookup. The complete intent is always required; the
/// expected claim and Release identities are supplied once their caller has
/// durably learned them.
struct CellAdmissionRecoveryReceiptQueryDTO final {
  CellAdmissionClaimIntentDTO intent;
  std::optional<CellAdmissionClaimDTO> expected_claim;
  std::optional<CellAdmissionClaimReleaseDTO> expected_release;

  friend bool operator==(const CellAdmissionRecoveryReceiptQueryDTO&,
                         const CellAdmissionRecoveryReceiptQueryDTO&) = default;
};

struct CellAdmissionRecoveryReceiptDTO final {
  CellAdmissionRecoveryReceiptStatusDTO status{
      CellAdmissionRecoveryReceiptStatusDTO::kUnknown};
  RequestOwnerEpoch kernel_owner_epoch;
  AdmissionGeneration admission_generation{0U};
  std::optional<CellAdmissionClaimDTO> matched_claim;
  std::optional<CellAdmissionClaimReleaseDTO> matched_release;
  std::optional<CellAdmissionReconcileSettlementDTO> matched_reconcile_settlement;
  std::vector<CellAdmissionClaimDTO> held_claims;
  std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owner_epochs;

  friend bool operator==(const CellAdmissionRecoveryReceiptDTO&,
                         const CellAdmissionRecoveryReceiptDTO&) = default;
};

struct CellAdmissionRecoveryLedgerEntryDTO final {
  CellAdmissionClaimDTO claim;
  std::optional<CellAdmissionClaimReleaseDTO> release;
  std::optional<CellAdmissionReconcileSettlementDTO> reconcile_settlement;

  friend bool operator==(const CellAdmissionRecoveryLedgerEntryDTO&,
                         const CellAdmissionRecoveryLedgerEntryDTO&) = default;
};

struct CellAdmissionRecoveryFootprintDTO final {
  std::size_t maximum_binary_request_bytes{};
  std::size_t maximum_binary_response_bytes{};
  std::size_t maximum_json_request_bytes{};
  std::size_t maximum_json_response_bytes{};

  [[nodiscard]] bool admitted() const noexcept {
    return maximum_binary_request_bytes <=
               kMaximumCellAdmissionPrivateBinaryBytes &&
           maximum_binary_response_bytes <=
               kMaximumCellAdmissionPrivateBinaryBytes &&
           maximum_json_request_bytes <=
               kMaximumCellAdmissionJsonRequestBytes &&
           maximum_json_response_bytes <=
               kMaximumCellAdmissionJsonResponseBytes;
  }

  friend bool operator==(const CellAdmissionRecoveryFootprintDTO&,
                         const CellAdmissionRecoveryFootprintDTO&) = default;
};

struct AuthoringAdmissionDTO final {
  bool authoring_allowed{};
  AdmissionGeneration admission_generation{0U};

  friend bool operator==(const AuthoringAdmissionDTO&, const AuthoringAdmissionDTO&) = default;
};

/// Original accepted Reset identity and selectors. A joining caller never
/// replaces these facts with its own attribution or selection.
struct AcceptedProgramResetDTO final {
  RequestOwnerEpoch request_owner_epoch;
  RequestId request_id;
  ContentDigest request_payload_digest;
  bool reload_initial_scene{};
  bool reset_robot_position{};
  std::optional<CellAdmissionClaimGeneration> reset_claim_generation;

  friend bool operator==(const AcceptedProgramResetDTO&,
                         const AcceptedProgramResetDTO&) = default;
};

struct ProgramCycleStatusDTO final {
  RequestOwnerEpoch program_runner_owner_epoch;
  std::uint64_t cycle_generation{};
  ProgramCycleStateDTO state{ProgramCycleStateDTO::kTerminalAwaitingReset};
  std::optional<ProgramRunId> program_run_id;
  std::optional<CellAdmissionClaimGeneration> cycle_claim_generation;
  std::optional<ContentDigest> terminal_or_settlement_reference;
  std::optional<AcceptedProgramResetDTO> accepted_reset{};

  friend bool operator==(const ProgramCycleStatusDTO&, const ProgramCycleStatusDTO&) = default;
};

/// Narrow authorization for logical-scene restoration owned by an accepted
/// Program-cycle Reset.  This is not a general authoring credential: Kernel
/// validates it against the current Resetting projection and the exact
/// retained or reconciled Program-cycle claim before the scene owner commits
/// one compare-and-swap transaction.
struct ProgramResetClaimSceneAuthorizationDTO final {
  std::uint64_t cycle_generation{};
  RequestOwnerEpoch request_owner_epoch;
  RequestId request_id;
  ContentDigest request_payload_digest;
  CellAdmissionClaimGeneration reset_claim_generation{0U};

  friend bool operator==(const ProgramResetClaimSceneAuthorizationDTO&,
                         const ProgramResetClaimSceneAuthorizationDTO&) = default;
};

struct ProgramResetSceneAuthorizationDTO final {
  RequestOwnerEpoch program_runner_owner_epoch;
  ProgramRunId program_run_id;
  CellAdmissionClaimGeneration cycle_claim_generation{0U};
  ContentDigest terminal_or_settlement_reference;
  /// Selected Reset-claim variant (idle or proven-no-effect failed Start).
  /// The run fields above must be empty/zero; the journal retains the real run.
  std::optional<ProgramResetClaimSceneAuthorizationDTO> reset_claim{};

  friend bool operator==(const ProgramResetSceneAuthorizationDTO&,
                         const ProgramResetSceneAuthorizationDTO&) = default;
};

/// Position-specific authority, joined to the original selected Reset and its
/// exact held Program-cycle or selected-idle Reset claim. Contains no target.
struct ProgramResetPositionAuthorizationDTO final {
  RequestOwnerEpoch program_runner_owner_epoch;
  std::uint64_t cycle_generation{};
  RequestOwnerEpoch request_owner_epoch;
  RequestId request_id;
  ContentDigest request_payload_digest;
  std::optional<ProgramRunId> program_run_id;
  std::optional<CellAdmissionClaimGeneration> cycle_claim_generation;
  std::optional<ContentDigest> terminal_or_settlement_reference;
  std::optional<CellAdmissionClaimGeneration> reset_claim_generation;
};
[[nodiscard]] Result<void> ValidateProgramResetPositionAuthorization(
    const ProgramResetPositionAuthorizationDTO& authorization);

/// Owner-produced O(1) reduction. Counts are complete bounded aggregates, not
/// collections to be rescanned by admission or Stop.
/// Manager's first failed contact with an established active Program owner.
/// This is availability evidence, never proof of physical containment.
struct ProgramRunnerContactLossDTO final {
  RequestOwnerEpoch manager_owner_epoch;
  RuntimeEpoch manager_runtime_epoch;
  ProgramCycleStatusDTO established_cycle;
  std::string failure_predicate;
  std::string message;

  friend bool operator==(const ProgramRunnerContactLossDTO&,
                         const ProgramRunnerContactLossDTO&) = default;
};

[[nodiscard]] Result<void> ValidateProgramRunnerContactLoss(
    const ProgramRunnerContactLossDTO& loss);

struct CellAdmissionReadinessSnapshotDTO final {
  bool program_cycle_status_current{};
  std::optional<ProgramCycleStatusDTO> program_cycle_status;
  bool required_component_status_current{};
  std::uint32_t required_component_total{};
  std::uint32_t required_component_current_compatible_ready_converged{};
  bool deployment_status_current{};
  bool deployment_converged{};
  std::uint32_t blocking_fault_count{};
  std::uint32_t unresolved_owner_count{};
  std::uint32_t unsettled_effect_count{};
  std::optional<ProgramRunnerContactLossDTO> program_runner_loss{};

  friend bool operator==(const CellAdmissionReadinessSnapshotDTO&,
                         const CellAdmissionReadinessSnapshotDTO&) = default;
};

[[nodiscard]] Result<void> ValidateCellAdmissionScope(const CellAdmissionScopeDTO& scope);
[[nodiscard]] Result<void> ValidateCellAdmissionClaimIntent(
    const CellAdmissionClaimIntentDTO& intent);
[[nodiscard]] Result<void> ValidateCellAdmissionClaimRelease(
    const CellAdmissionClaimReleaseDTO& release);
[[nodiscard]] Result<void> ValidateCellAdmissionRecoveryReceiptQuery(
    const CellAdmissionRecoveryReceiptQueryDTO& query);
[[nodiscard]] Result<void> ValidateCellAdmissionRecoveryReceipt(
    const CellAdmissionRecoveryReceiptDTO& receipt);
[[nodiscard]] Result<CellAdmissionRecoveryFootprintDTO>
ComputeCellAdmissionRecoveryFootprint(
    const RequestOwnerEpoch& kernel_owner_epoch,
    AdmissionGeneration admission_generation,
    const std::vector<CellAdmissionRecoveryLedgerEntryDTO>& entries,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current_owner_epochs);
[[nodiscard]] Result<void> ValidateProgramCycleStatus(const ProgramCycleStatusDTO& status);
[[nodiscard]] Result<RequestId> ProgramResetClaimRequestId(const ProgramCycleStatusDTO& status);
[[nodiscard]] Result<void> ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization);
[[nodiscard]] Result<ContentDigest> CellAdmissionClaimIntentDigest(
    const CellAdmissionClaimIntentDTO& intent);
[[nodiscard]] Result<ContentDigest>
CellAdmissionOwnerEpochReconciliationEvidenceDigest(
    CellAdmissionOwnerRoleDTO owner_role,
    const RequestOwnerEpoch& owner_epoch);

}  // namespace manyforge::core
