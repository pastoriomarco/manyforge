#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_component_manager/apply_completion.hpp>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_component_manager/selected_bundle_transition.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::string_view kManagerApplyJournalSchemaId =
    "manyforge.internal.component-manager.apply-journal.ua4.v15";
inline constexpr std::int64_t kManagerApplyJournalSchemaVersion = 15;
inline constexpr std::int64_t kManagerApplyJournalSqliteApplicationId =
    1296450128;
inline constexpr std::size_t kMaximumManagerApplyIntents = 256U;
inline constexpr std::size_t kMaximumManagerApplyFenceAttempts =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyAffectedScopes =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyPrivateMaterializations =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyPublicationReceipts =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyFenceReleaseIntents =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyFenceReleasedRecords =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyTerminalOutcomes =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyFenceComponents = 128U;
inline constexpr std::size_t kMaximumManagerApplyFenceDomains = 64U;
inline constexpr std::size_t kMaximumManagerApplyFenceBindings = 128U;
inline constexpr std::size_t kMaximumManagerApplyFenceResources = 384U;
inline constexpr std::size_t kMaximumManagerApplyJournalIdentifierBytes = 256U;
inline constexpr std::size_t kMaximumManagerApplySourceLabelBytes = 256U;
inline constexpr std::size_t kMaximumManagerApplyCorrelationIdBytes = 1024U;
inline constexpr std::size_t kMaximumManagerApplyPreparedReceiptBytes = 65536U;
inline constexpr std::size_t kMaximumManagerApplyFenceReasonBytes = 1024U;
inline constexpr std::int64_t
    kMaximumManagerApplyJournalDatabasePageImageBytes = 64LL * 1024LL * 1024LL;

class ManagerApplyJournalError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ManagerApplyJournalConflictError final : public ManagerApplyJournalError {
public:
  using ManagerApplyJournalError::ManagerApplyJournalError;
};

class ManagerApplyJournalLimitError final : public ManagerApplyJournalError {
public:
  using ManagerApplyJournalError::ManagerApplyJournalError;
};

class ManagerApplyJournalBusyError final : public ManagerApplyJournalError {
public:
  using ManagerApplyJournalError::ManagerApplyJournalError;
};

class ManagerApplyJournalIntegrityError final
    : public ManagerApplyJournalError {
public:
  using ManagerApplyJournalError::ManagerApplyJournalError;
};

class ManagerApplyJournalClosedError final : public ManagerApplyJournalError {
public:
  using ManagerApplyJournalError::ManagerApplyJournalError;
};

/// Exact UA2A prepared activation accepted by one named Apply.
///
/// Every fact here is captured once before acceptance. The Manager records it
/// in the durable intent and must never reopen a mutable Scenario/catalog
/// selection, draft or current-selection fact to reconstruct the operation
/// input afterwards. The bounded prepared runtime input itself stays with its
/// existing prepared-value owner: this record carries that owner's exact
/// `prepared_activation_id` transfer handle together with the prepared Program
/// binding identity and the captured source-closure digest.
struct ManagerApplyPreparedActivationDTO final {
  manyforge::core::ContentDigest captured_input_digest;
  manyforge::core::DeploymentArtifactId deployment_artifact_id;
  manyforge::core::DeploymentRevision deployment_revision;
  manyforge::core::ProgramBindingId program_binding_id;
  manyforge::core::ContentDigest prepared_activation_id;
  /// Canonical JSON for the complete bounded preparation receipt.  This is
  /// operation input, not a second catalog: it lets observers recover the
  /// exact Scenario/Program identity without reopening mutable authoring data.
  std::string canonical_receipt_json;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPreparedActivationDTO &,
                         const ManagerApplyPreparedActivationDTO &) = default;
};

/// Immutable named-Apply input for the first durable Apply boundary.
///
/// The predecessor projection is the complete active composite expected by
/// the request. The candidate pair is only an exact retained identity; the
/// Manager must obtain and verify the corresponding retained bundle through
/// the sealed repository before constructing this DTO. The future operational
/// Apply coordinator must persist this record before its first configuration
/// fence or runtime-unit effect; this storage primitive owns no external
/// effect surface and therefore cannot establish that ordering by itself.
/// Runtime convergence and affected scope are deliberately later journal
/// records. Scene initialization evidence is owned by Scene itself.
struct ManagerApplyIntentDTO final {
  manyforge::core::PublicRequestContextDTO request_context;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::CellId cell_id;
  manyforge::core::ProjectionRevisionDTO expected_active_projection_revision;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  ManagerApplyPreparedActivationDTO prepared_activation;
  std::optional<manyforge::core::AdmissionGeneration>
      expected_admission_generation;
  std::optional<manyforge::core::CellAdmissionClaimDTO>
      parent_authoring_claim;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyIntentDTO &,
                         const ManagerApplyIntentDTO &) = default;
};

struct PersistedManagerApplyIntent final {
  ManagerApplyIntentDTO intent;
  manyforge::core::ContentDigest intent_digest;

  friend bool operator==(const PersistedManagerApplyIntent &,
                         const PersistedManagerApplyIntent &) = default;
};

struct ManagerApplyIntentWriteResult final {
  PersistedManagerApplyIntent record;
  bool inserted;
};

/// Immutable whole-Apply admission scope derived from sealed bundles.
struct ManagerApplyAffectedScopeDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ConfigurationFenceScopeDTO scope;
  manyforge::core::ApplyFenceBusyDispositionDTO busy_disposition;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() && scope.valid() &&
           manyforge::core::IsKnownApplyFenceBusyDisposition(busy_disposition);
  }

  friend bool operator==(const ManagerApplyAffectedScopeDTO &,
                         const ManagerApplyAffectedScopeDTO &) = default;
};

struct PersistedManagerApplyAffectedScope final {
  ManagerApplyAffectedScopeDTO affected_scope;
  manyforge::core::ContentDigest intent_digest;
  manyforge::core::ContentDigest affected_scope_digest;

  friend bool operator==(const PersistedManagerApplyAffectedScope &,
                         const PersistedManagerApplyAffectedScope &) = default;
};

struct ManagerApplyAffectedScopeWriteResult final {
  PersistedManagerApplyAffectedScope record;
  bool inserted;
};

/// Exact immutable Kernel-fence request durably bound to one Apply intent.
struct PersistedManagerApplyFenceAttempt final {
  manyforge::core::ApplyConfigurationFenceIntentDTO fence_intent;
  manyforge::core::ContentDigest intent_digest;
  manyforge::core::ContentDigest affected_scope_digest;
  manyforge::core::ContentDigest fence_attempt_digest;
};

struct ManagerApplyFenceAttemptWriteResult final {
  PersistedManagerApplyFenceAttempt record;
  bool inserted;
};

/// Exact immutable successful Kernel response bound to one fence attempt.
struct PersistedManagerApplyFenceAcceptance final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ContentDigest fence_attempt_digest;
  manyforge::core::ConfigurationFenceAcceptedDTO accepted_fence;
  manyforge::core::ContentDigest acceptance_digest;
};

struct ManagerApplyFenceAcceptanceWriteResult final {
  PersistedManagerApplyFenceAcceptance record;
  bool inserted;
};

struct PersistedManagerApplyRuntimeConvergenceIntent final {
  ManagerApplyRuntimeConvergenceIntentDTO runtime_convergence_intent;
  manyforge::core::ContentDigest intent_digest;
  manyforge::core::ContentDigest runtime_convergence_intent_digest;

  friend bool
  operator==(const PersistedManagerApplyRuntimeConvergenceIntent &,
             const PersistedManagerApplyRuntimeConvergenceIntent &) = default;
};

struct ManagerApplyRuntimeConvergenceIntentWriteResult final {
  PersistedManagerApplyRuntimeConvergenceIntent record;
  bool inserted;
};

struct PersistedManagerApplyRuntimeStagedReady final {
  ManagerApplyRuntimeStagedReadyDTO staged_ready;
  manyforge::core::ContentDigest staged_ready_digest;

  friend bool
  operator==(const PersistedManagerApplyRuntimeStagedReady &,
             const PersistedManagerApplyRuntimeStagedReady &) = default;
};

struct ManagerApplyRuntimeStagedReadyWriteResult final {
  PersistedManagerApplyRuntimeStagedReady record;
  bool inserted;
};

struct PersistedManagerApplyPrivateMaterialization final {
  ManagerApplyPrivateMaterializationDTO private_materialization;
  manyforge::core::ContentDigest private_materialization_content_digest;

  friend bool
  operator==(const PersistedManagerApplyPrivateMaterialization &,
             const PersistedManagerApplyPrivateMaterialization &) = default;
};

struct ManagerApplyPrivateMaterializationWriteResult final {
  PersistedManagerApplyPrivateMaterialization record;
  bool inserted;
};

struct PersistedManagerApplyPublicationReceipt final {
  ManagerApplyPublicationReceiptDTO publication;
  manyforge::core::ContentDigest publication_receipt_digest;

  friend bool
  operator==(const PersistedManagerApplyPublicationReceipt &,
             const PersistedManagerApplyPublicationReceipt &) = default;
};

struct ManagerApplyPublicationReceiptWriteResult final {
  PersistedManagerApplyPublicationReceipt record;
  bool inserted;
};

struct PersistedManagerApplyFenceReleaseIntent final {
  ManagerApplyFenceReleaseIntentDTO fence_release_intent;
  manyforge::core::ContentDigest fence_release_intent_digest;

  friend bool
  operator==(const PersistedManagerApplyFenceReleaseIntent &,
             const PersistedManagerApplyFenceReleaseIntent &) = default;
};

struct ManagerApplyFenceReleaseIntentWriteResult final {
  PersistedManagerApplyFenceReleaseIntent record;
  bool inserted;
};

struct PersistedManagerApplyFenceReleased final {
  ManagerApplyFenceReleasedDTO fence_released;
  manyforge::core::ContentDigest fence_released_digest;

  friend bool operator==(const PersistedManagerApplyFenceReleased &,
                         const PersistedManagerApplyFenceReleased &) = default;
};

struct ManagerApplyFenceReleasedWriteResult final {
  PersistedManagerApplyFenceReleased record;
  bool inserted;
};

struct PersistedManagerApplyTerminalOutcome final {
  ManagerApplyTerminalOutcomeDTO terminal_outcome;
  manyforge::core::ContentDigest terminal_outcome_digest;

  friend bool
  operator==(const PersistedManagerApplyTerminalOutcome &,
             const PersistedManagerApplyTerminalOutcome &) = default;
};

struct ManagerApplyTerminalOutcomeWriteResult final {
  PersistedManagerApplyTerminalOutcome record;
  bool inserted;
};

/// One transactionally consistent, immutable view of every durable record for
/// one Apply generation. Missing members mean that the corresponding phase
/// has not been durably reached; no mutable phase label is stored or inferred
/// by the journal.
struct PersistedManagerApplyStatus final {
  PersistedManagerApplyIntent intent;
  std::optional<PersistedManagerApplyAffectedScope> affected_scope;
  std::optional<PersistedManagerApplyFenceAttempt> fence_attempt;
  std::optional<PersistedManagerApplyFenceAcceptance> fence_acceptance;
  std::optional<PersistedManagerApplyRuntimeConvergenceIntent>
      runtime_convergence_intent;
  std::optional<PersistedManagerApplyRuntimeStagedReady>
      latest_runtime_staged_ready;
  std::optional<PersistedManagerApplyPrivateMaterialization>
      private_materialization;
  std::optional<PersistedManagerApplyPublicationReceipt> publication_receipt;
  std::optional<PersistedManagerApplyFenceReleaseIntent> fence_release_intent;
  std::optional<PersistedManagerApplyFenceReleased> fence_released;
  std::optional<PersistedManagerApplyTerminalOutcome> terminal_outcome;
};

/// Last real progress for one Apply generation: the deepest durably reached
/// milestone and the exact evidence digest that established it.
struct ManagerApplyProgressDTO final {
  ManagerApplyPhase phase;
  manyforge::core::ContentDigest record_digest;

  friend bool operator==(const ManagerApplyProgressDTO &,
                         const ManagerApplyProgressDTO &) = default;
};

/// Return the deepest durably reached milestone for a complete status view.
[[nodiscard]] ManagerApplyProgressDTO DeriveManagerApplyProgress(
    const PersistedManagerApplyStatus &status);

struct ManagerApplyJournalIntegrityReport final {
  manyforge::core::CellId cell_id;
  std::string schema_id;
  std::int64_t schema_version;
  std::string schema_artifact_digest;
  std::int64_t apply_intent_count;
  std::int64_t distinct_apply_id_count;
  std::int64_t distinct_request_identity_count;
  std::int64_t affected_scope_count;
  std::int64_t fence_attempt_count;
  std::int64_t fence_acceptance_count;
  std::int64_t runtime_convergence_intent_count;
  std::int64_t runtime_staged_ready_count;
  std::int64_t private_materialization_count;
  std::int64_t publication_receipt_count;
  std::int64_t fence_release_intent_count;
  std::int64_t fence_released_count;
  std::int64_t terminal_outcome_count;
};

/// Durable append-only owner of bounded named-Apply ordering evidence.
///
/// Exact duplicate writes replay the original record. One public
/// `(request_owner_epoch, request_id)` identifies exactly one immutable Apply
/// intent. Reusing an Apply ID requires the next contiguous generation.
/// Each Apply generation may append only the exact parent-linked records for
/// affected scope, fence admission, runtime staged readiness, selected-bundle
/// publication, fence release, and terminal success. Scene maintenance is
/// durably owned by Scene. Conflicts, gaps, corruption, foreign-cell databases, and
/// resource-limit exhaustion fail before another journal write.
///
/// This journal owns no mutable phase row and performs no external Kernel,
/// process, scene, publication, or robot effect. Those semantic owners return
/// the exact evidence recorded here.
class SqliteManagerApplyJournal final {
public:
  SqliteManagerApplyJournal(std::filesystem::path database_path,
                            manyforge::core::CellId cell_id);
  ~SqliteManagerApplyJournal();

  SqliteManagerApplyJournal(SqliteManagerApplyJournal &&) noexcept;
  SqliteManagerApplyJournal &operator=(SqliteManagerApplyJournal &&) noexcept;
  SqliteManagerApplyJournal(const SqliteManagerApplyJournal &) = delete;
  SqliteManagerApplyJournal &
  operator=(const SqliteManagerApplyJournal &) = delete;

  [[nodiscard]] const std::filesystem::path &database_path() const noexcept;
  [[nodiscard]] const manyforge::core::CellId &cell_id() const noexcept;

  [[nodiscard]] ManagerApplyIntentWriteResult
  EnsureIntent(const ManagerApplyIntentDTO &intent);
  [[nodiscard]] std::optional<PersistedManagerApplyIntent>
  ReadIntent(const manyforge::core::ApplyId &apply_id,
             manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] std::vector<PersistedManagerApplyStatus>
  ReadStatusInventory() const;
  /// One transactionally consistent immutable view of every durable record for
  /// exactly one Apply generation, or nullopt when that identity is unknown.
  [[nodiscard]] std::optional<PersistedManagerApplyStatus>
  ReadStatus(const manyforge::core::ApplyId &apply_id,
             manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] std::optional<PersistedManagerApplyIntent>
  ReadRequest(const manyforge::core::RequestOwnerEpoch &request_owner_epoch,
              const manyforge::core::RequestId &request_id) const;
  [[nodiscard]] std::optional<manyforge::core::ApplyGeneration>
  ReadApplyGenerationHighWater(const manyforge::core::ApplyId &apply_id) const;
  [[nodiscard]] ManagerApplyAffectedScopeWriteResult
  EnsureAffectedScope(const manyforge::core::ContentDigest &intent_digest,
                      const ManagerApplyAffectedScopeDTO &affected_scope);
  [[nodiscard]] std::optional<PersistedManagerApplyAffectedScope>
  ReadAffectedScope(const manyforge::core::ApplyId &apply_id,
                    manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyFenceAttemptWriteResult EnsureFenceAttempt(
      const manyforge::core::ContentDigest &intent_digest,
      const manyforge::core::ContentDigest &affected_scope_digest,
      const manyforge::core::ApplyConfigurationFenceIntentDTO &fence_intent);
  [[nodiscard]] std::optional<PersistedManagerApplyFenceAttempt>
  ReadFenceAttempt(const manyforge::core::ApplyId &apply_id,
                   manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyFenceAcceptanceWriteResult EnsureFenceAcceptance(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation,
      const manyforge::core::ContentDigest &fence_attempt_digest,
      const manyforge::core::ConfigurationFenceAcceptedDTO &accepted_fence);
  [[nodiscard]] std::optional<PersistedManagerApplyFenceAcceptance>
  ReadFenceAcceptance(const manyforge::core::ApplyId &apply_id,
                      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyRuntimeConvergenceIntentWriteResult
  EnsureRuntimeConvergenceIntent(
      const manyforge::core::ContentDigest &intent_digest,
      const ManagerApplyRuntimeConvergenceIntentDTO &runtime_intent);
  [[nodiscard]] std::optional<PersistedManagerApplyRuntimeConvergenceIntent>
  ReadRuntimeConvergenceIntent(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyRuntimeStagedReadyWriteResult
  EnsureRuntimeStagedReady(
      const ManagerApplyRuntimeStagedReadyDTO &staged_ready);
  [[nodiscard]] std::optional<PersistedManagerApplyRuntimeStagedReady>
  ReadLatestRuntimeStagedReady(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyPrivateMaterializationWriteResult
  EnsurePrivateMaterialization(
      const ManagerApplyPrivateMaterializationDTO &private_materialization);
  [[nodiscard]] std::optional<PersistedManagerApplyPrivateMaterialization>
  ReadPrivateMaterialization(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyPublicationReceiptWriteResult
  EnsurePublicationReceipt(
      const ManagerApplyPublicationReceiptDTO &publication);
  [[nodiscard]] std::optional<PersistedManagerApplyPublicationReceipt>
  ReadPublicationReceipt(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyFenceReleaseIntentWriteResult
  EnsureFenceReleaseIntent(
      const ManagerApplyFenceReleaseIntentDTO &fence_release_intent);
  [[nodiscard]] std::optional<PersistedManagerApplyFenceReleaseIntent>
  ReadFenceReleaseIntent(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyFenceReleasedWriteResult
  EnsureFenceReleased(const ManagerApplyFenceReleasedDTO &fence_released);
  [[nodiscard]] std::optional<PersistedManagerApplyFenceReleased>
  ReadFenceReleased(const manyforge::core::ApplyId &apply_id,
                    manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyTerminalOutcomeWriteResult
  EnsureTerminalOutcome(const ManagerApplyTerminalOutcomeDTO &terminal_outcome);
  [[nodiscard]] std::optional<PersistedManagerApplyTerminalOutcome>
  ReadTerminalOutcome(const manyforge::core::ApplyId &apply_id,
                      manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] ManagerApplyJournalIntegrityReport VerifyIntegrity() const;

  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read-only native-Kernel settlement check for one exact Manager-owned cell
/// claim. It opens the owner journal without initialization, locks, mutation,
/// checkpointing, or durability side effects.
[[nodiscard]] manyforge::core::Result<void>
ValidateManagerApplyCellAdmissionSettlementReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::CellAdmissionClaimDTO &claim,
    const manyforge::core::CellAdmissionClaimReleaseDTO &release);

/// Authorize one Composer owner-epoch rotation without a Composer journal.
/// A short local claim absent from the Manager journal is proven to have no
/// surviving external effect. A claim transferred to Manager is eligible only
/// after that exact operation has a known terminal outcome; pending and
/// unknown-effect operations remain held and keep authoring closed.
[[nodiscard]] manyforge::core::Result<
    manyforge::core::CellAdmissionReconcileValidationDTO>
ValidateManagerAuthoringOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const std::vector<manyforge::core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<manyforge::core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const manyforge::core::CellAdmissionReconcileDTO &reconcile);

} // namespace manyforge::component_manager
