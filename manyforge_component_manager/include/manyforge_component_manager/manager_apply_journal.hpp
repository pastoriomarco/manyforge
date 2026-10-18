#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_component_manager/apply_completion.hpp>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_component_manager/selected_bundle_transition.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::string_view kManagerApplyJournalSchemaId =
    "manyforge.internal.component-manager.apply-journal.h1a.v18";
inline constexpr std::int64_t kManagerApplyJournalSchemaVersion = 18;
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
  /// The candidate Program binding, or absent exactly when the prepared
  /// activation's typed Program outcome is non-executable (H1a). The receipt
  /// below carries that outcome; the Program itself is never rewritten.
  std::optional<manyforge::core::ProgramBindingId> program_binding_id;
  manyforge::core::ContentDigest prepared_activation_id;
  /// Canonical JSON for the complete bounded preparation receipt.  This is
  /// operation input, not a second catalog: it lets observers recover the
  /// exact Scenario/Program identity without reopening mutable authoring data.
  std::string canonical_receipt_json;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyPreparedActivationDTO &,
                         const ManagerApplyPreparedActivationDTO &) = default;
};

/// Scene preparation policy for this Apply operation, independent of prepared
/// content.
enum class ManagerSceneInitialization { kPreserve, kInitialize };

[[nodiscard]] const char *ToString(ManagerSceneInitialization value) noexcept;

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
  std::optional<ManagerIsaacPoseObserverLaunchFactsDTO>
      isaac_pose_observer_launch_facts{};
  ManagerSceneInitialization scene_initialization{
      ManagerSceneInitialization::kPreserve};

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

/// Original per-component input captured before any configured bring-up
/// effect. This value carries no parent permission or fabricated owner facts.
struct ManagerApplyBringUpChildRequestDTO final {
  manyforge::core::ComponentLifecycleIntentDTO intent;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerApplyBringUpChildRequestDTO &,
                         const ManagerApplyBringUpChildRequestDTO &) = default;
};

/// Complete immutable configured set and the ordered subset requiring work.
/// The separately sealed plan is bookkeeping; only a positively verified live
/// ApplyComponentBringUpAuthorityDTO can delegate the parent's permission.
struct ManagerApplyBringUpPlanDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ContentDigest parent_intent_digest;
  std::vector<manyforge::core::ControllableComponentId> configured_components;
  std::vector<ManagerApplyBringUpChildRequestDTO> children;
  manyforge::core::ActiveBundlePublicationRequestDTO expected_publication;
  manyforge::core::ContentDigest predecessor_public_pointer_digest;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerApplyBringUpPlanDTO &,
                         const ManagerApplyBringUpPlanDTO &) = default;
};

struct PersistedManagerApplyBringUpPlan final {
  ManagerApplyBringUpPlanDTO plan;
  manyforge::core::ContentDigest plan_digest;
  friend bool operator==(const PersistedManagerApplyBringUpPlan &,
                         const PersistedManagerApplyBringUpPlan &) = default;
};

struct ManagerApplyBringUpIntentWriteResult final {
  PersistedManagerApplyIntent intent;
  PersistedManagerApplyBringUpPlan plan;
  bool inserted;
};

enum class ManagerApplyBringUpChildPhase { kAccepted, kHolding, kTerminal };

/// One owner-minted observation at a durable child milestone. Reads and shape
/// checks grant no continuation, installation, or publication permission.
struct ManagerApplyBringUpChildProofDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ContentDigest plan_digest;
  manyforge::core::ControllableComponentId controllable_component_id;
  ManagerApplyBringUpChildPhase phase;
  manyforge::core::CCLCLifecycleRequestResultDTO result;
};

struct PersistedManagerApplyBringUpChildProof final {
  ManagerApplyBringUpChildProofDTO proof;
  manyforge::core::ContentDigest proof_digest;
};

struct ManagerApplyBringUpChildProofWriteResult final {
  PersistedManagerApplyBringUpChildProof record;
  bool inserted;
};

/// Mechanical helpers reuse the journal's existing encoders; no receipt or
/// authority is minted by computing an immutable input's content digest.
[[nodiscard]] manyforge::core::ContentDigest
ComputeManagerApplyIntentDigest(const ManagerApplyIntentDTO &intent);
[[nodiscard]] manyforge::core::ContentDigest
ComputeManagerApplyPrivateMaterializationDigest(
    const ManagerApplyPrivateMaterializationDTO &materialization);
[[nodiscard]] manyforge::core::ContentDigest
ComputeManagerApplyBringUpPlanDigest(const ManagerApplyBringUpPlanDTO &plan);

/// Immutable whole-Apply admission scope and per-component decision, both
/// derived from sealed bundles and persisted together before any effect.
struct ManagerApplyAffectedScopeDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ConfigurationFenceScopeDTO scope;
  manyforge::core::ApplyFenceBusyDispositionDTO busy_disposition;
  manyforge::deployment::ApplyComponentDeltaDTO component_delta;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() && scope.valid() &&
           manyforge::core::IsKnownApplyFenceBusyDisposition(busy_disposition) &&
           component_delta.valid() &&
           manyforge::deployment::apply_component_delta_is_fenced(component_delta,
                                                                 scope);
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
  std::optional<PersistedManagerApplyBringUpPlan> bring_up_plan;
  std::vector<PersistedManagerApplyBringUpChildProof> bring_up_child_proofs;
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
  std::int64_t bring_up_plan_count;
  std::int64_t bring_up_child_proof_count;
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
  /// Insert parent and complete chosen plan in the same transaction. A plan
  /// cannot be retrofitted to an ordinary accepted parent.
  [[nodiscard]] ManagerApplyBringUpIntentWriteResult
  EnsureBringUpIntent(const ManagerApplyIntentDTO &intent,
                      const ManagerApplyBringUpPlanDTO &plan);
  [[nodiscard]] std::optional<PersistedManagerApplyBringUpPlan>
  ReadBringUpPlan(const manyforge::core::ApplyId &apply_id,
                  manyforge::core::ApplyGeneration apply_generation) const;
  [[nodiscard]] std::optional<PersistedManagerApplyBringUpPlan>
  ReadBringUpPlanByRequest(
      const manyforge::core::RequestOwnerEpoch &request_owner_epoch,
      const manyforge::core::RequestId &request_id) const;
  [[nodiscard]] ManagerApplyBringUpChildProofWriteResult
  EnsureBringUpChildProof(const ManagerApplyBringUpChildProofDTO &proof);
  [[nodiscard]] std::optional<PersistedManagerApplyBringUpChildProof>
  ReadBringUpChildProof(
      const manyforge::core::ApplyId &apply_id,
      manyforge::core::ApplyGeneration apply_generation,
      const manyforge::core::ControllableComponentId &component,
      ManagerApplyBringUpChildPhase phase) const;
  [[nodiscard]] std::optional<PersistedManagerApplyIntent>
  ReadIntent(const manyforge::core::ApplyId &apply_id,
             manyforge::core::ApplyGeneration apply_generation) const;
  /// Every durable Apply status, each record digest-verified.
  ///
  /// All Read* operations reuse records they already decoded and verified
  /// only while the database is provably unchanged: the same SQLite
  /// `data_version` (no commit through any other connection) and the same
  /// total change count of this connection (no own write). Any commit, or a
  /// failed read, discards every reused record, so a changed or corrupted
  /// record is re-read and re-verified by the very next read. VerifyIntegrity
  /// always verifies the complete database.
  [[nodiscard]] std::vector<PersistedManagerApplyStatus>
  ReadStatusInventory() const;
  /// Number of records decoded and digest-verified from SQLite (a diagnostic
  /// for change-gated reuse; reuse does not advance it).
  [[nodiscard]] std::uint64_t record_verification_count() const;
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
  friend manyforge::core::Result<PersistedManagerApplyStatus>
  ReadManagerApplyStatusReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::NamedApplyPublicationTransactionDTO &);
  friend manyforge::core::Result<PersistedManagerApplyStatus>
  ReadNativeApplyParentStatusReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::NativeApplyPlanDTO &,
      const manyforge::deployment::RetainedBundle &);
  friend manyforge::core::Result<PersistedManagerApplyStatus>
  ReadManagerApplyBringUpStatusReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::ApplyComponentBringUpReferenceDTO &);
  friend manyforge::core::Result<PersistedManagerApplyStatus>
  ReadManagerApplyBringUpStatusReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::CellAdmissionClaimIntentDTO &);
  friend manyforge::core::Result<PersistedManagerApplyStatus>
  ReadManagerApplyConfigurationStatusReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::NamedApplyPublicationTransactionDTO &,
      const manyforge::core::CellAdmissionClaimDTO &);
  friend manyforge::core::Result<void>
  ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::CellAdmissionClaimDTO &,
      const manyforge::core::CellAdmissionClaimReleaseDTO &);
  friend manyforge::core::Result<void>
  ValidateTerminalManagerApplyClaimsReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      std::span<const manyforge::core::CellAdmissionClaimDTO>);
  friend manyforge::core::Result<void> ValidateFreshManagerApplySuccessReadOnly(
      const std::filesystem::path &, const manyforge::core::CellId &,
      const manyforge::core::CellAdmissionClaimDTO &,
      const manyforge::core::CellAdmissionClaimReleaseDTO &);
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

/// Prove immutable failed/unknown terminals for the exact old claims. A missing
/// terminal stays LIVE; this read neither terminalizes nor settles anything.
[[nodiscard]] manyforge::core::Result<void>
ValidateTerminalManagerApplyClaimsReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    std::span<const manyforge::core::CellAdmissionClaimDTO> old_claims);
/// Fresh immutable success only. The Kernel separately proves that every old
/// fence was released before its admission owner settles the old claim rows.
[[nodiscard]] manyforge::core::Result<void>
ValidateFreshManagerApplySuccessReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::CellAdmissionClaimDTO &fresh_claim,
    const manyforge::core::CellAdmissionClaimReleaseDTO &release);

/// Observation-only exact record from an existing journal, including terminal
/// records with no live claim. It grants no Apply/effect/currentness
/// permission.
[[nodiscard]] manyforge::core::Result<PersistedManagerApplyStatus>
ReadManagerApplyStatusReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::NamedApplyPublicationTransactionDTO &apply);

/// Read one exact sealed parent and its existing phase records without opening
/// a mutable journal owner. These retained values are observation-only: the
/// original live Manager/host must independently prove current authority.
[[nodiscard]] manyforge::core::Result<PersistedManagerApplyStatus>
ReadManagerApplyBringUpStatusReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::ApplyComponentBringUpReferenceDTO &parent);

/// Resolve retained parent data from the exact original acquire intent. The
/// full ordinary intent digest or linked plan digest must match. Only a
/// successful read with no bring-up plan identifies the ordinary shape;
/// unavailable/corrupt/conflicting reads never mean ordinary absence. No
/// acquired claim identity or current effect authority is inferred.
[[nodiscard]] manyforge::core::Result<PersistedManagerApplyStatus>
ReadManagerApplyBringUpStatusReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::CellAdmissionClaimIntentDTO &parent_claim_intent);

/// Observe one ordinary configuration Apply through its exact original full
/// Kernel parent. Direct Manager and stored authoring parents keep their own
/// grammars; an authoring claim is never equated to the Manager intent digest.
/// No acquired/current authority is inferred from this read-only owner snapshot.
[[nodiscard]] manyforge::core::Result<PersistedManagerApplyStatus>
ReadManagerApplyConfigurationStatusReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::NamedApplyPublicationTransactionDTO &apply,
    const manyforge::core::CellAdmissionClaimDTO &actual_parent_claim);

/// Read exact retained parent data for a captured native plan and sealed
/// candidate. This grants no authority; the caller queries the original claim
/// through the existing Kernel recovery port after this snapshot has closed.
[[nodiscard]] manyforge::core::Result<PersistedManagerApplyStatus>
ReadNativeApplyParentStatusReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::NativeApplyPlanDTO &native_plan,
    const manyforge::deployment::RetainedBundle &verified_candidate);

/// Observe only the exact original Prepare catch before installation. A
/// matching result ends the additional native early-plan classifier; it grants
/// no effect continuation, adoption, settlement or claim/fence release. The
/// caller supplies its captured full native plan and an independently verified
/// candidate, and obtains the actual HeldExact parent receipt outside SQL.
[[nodiscard]] manyforge::core::Result<std::optional<PersistedManagerApplyStatus>>
ReadNativeApplyPreparationFailureReadOnly(
    const std::filesystem::path &database_path,
    const manyforge::core::CellId &cell_id,
    const manyforge::core::NativeApplyPlanDTO &native_plan,
    const manyforge::deployment::RetainedBundle &verified_candidate,
    const manyforge::core::CellAdmissionClaimDTO &parent_claim,
    const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &claim_query,
    const manyforge::core::CellAdmissionRecoveryReceiptDTO &claim_receipt);

/// Selected Apply settlement is proven against its separately sealed plan,
/// actual child terminal proofs and publication/fence release. The ordinary
/// direct-Apply validator deliberately excludes every linked parent.
[[nodiscard]] manyforge::core::Result<void>
ValidateManagerApplyBringUpCellAdmissionSettlementReadOnly(
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
