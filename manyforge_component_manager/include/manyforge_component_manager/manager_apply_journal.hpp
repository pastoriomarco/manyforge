#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_component_manager/runtime_convergence.hpp>

namespace manyforge::component_manager {

inline constexpr std::string_view kManagerApplyJournalSchemaId =
    "manyforge.internal.component-manager.apply-journal.i4.v3";
inline constexpr std::int64_t kManagerApplyJournalSchemaVersion = 3;
inline constexpr std::int64_t kManagerApplyJournalSqliteApplicationId =
    1296450128;
inline constexpr std::size_t kMaximumManagerApplyIntents = 256U;
inline constexpr std::size_t kMaximumManagerApplyFenceAttempts =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyAffectedScopes =
    kMaximumManagerApplyIntents;
inline constexpr std::size_t kMaximumManagerApplyFenceComponents = 256U;
inline constexpr std::size_t kMaximumManagerApplyFenceDomains = 64U;
inline constexpr std::size_t kMaximumManagerApplyFenceBindings = 256U;
inline constexpr std::size_t kMaximumManagerApplyFenceResources = 1024U;
inline constexpr std::size_t kMaximumManagerApplyJournalIdentifierBytes = 256U;
inline constexpr std::size_t kMaximumManagerApplySourceLabelBytes = 256U;
inline constexpr std::size_t kMaximumManagerApplyCorrelationIdBytes = 1024U;
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

/// Immutable named-Apply input for the first durable Apply boundary.
///
/// The predecessor projection is the complete active composite expected by
/// the request. The candidate pair is only an exact retained identity; the
/// Manager must obtain and verify the corresponding retained bundle through
/// the sealed repository before constructing this DTO. The future operational
/// Apply coordinator must persist this record before its first configuration
/// fence or runtime-unit effect; this storage primitive owns no external
/// effect surface and therefore cannot establish that ordering by itself.
/// Runtime convergence, affected scope, scene transition rows, stage identity,
/// and activation selection are deliberately later journal records.
struct ManagerApplyIntentDTO final {
  manyforge::core::PublicRequestContextDTO request_context;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::CellId cell_id;
  manyforge::core::ProjectionRevisionDTO expected_active_projection_revision;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;

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

  friend bool operator==(
      const PersistedManagerApplyRuntimeConvergenceIntent &,
      const PersistedManagerApplyRuntimeConvergenceIntent &) = default;
};

struct ManagerApplyRuntimeConvergenceIntentWriteResult final {
  PersistedManagerApplyRuntimeConvergenceIntent record;
  bool inserted;
};

struct PersistedManagerApplyRuntimeStagedReady final {
  ManagerApplyRuntimeStagedReadyDTO staged_ready;
  manyforge::core::ContentDigest staged_ready_digest;

  friend bool operator==(const PersistedManagerApplyRuntimeStagedReady &,
                         const PersistedManagerApplyRuntimeStagedReady &) =
      default;
};

struct ManagerApplyRuntimeStagedReadyWriteResult final {
  PersistedManagerApplyRuntimeStagedReady record;
  bool inserted;
};

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
};

/// Durable append-only owner of bounded named-Apply fence ordering evidence.
///
/// Exact duplicate writes replay the original record. One public
/// `(request_owner_epoch, request_id)` identifies exactly one immutable Apply
/// intent. Reusing an Apply ID requires the next contiguous generation.
/// Each Apply generation may then append one derived affected-scope record,
/// digest-bound to its intent, one exact configuration-fence attempt bound to
/// both records, and one complete successful acceptance bound to the attempt.
/// Conflicts, gaps, corruption,
/// foreign-cell databases, and resource-limit exhaustion fail before another
/// journal write.
///
/// This focused journal owns no mutable Apply phase, external Kernel effect,
/// process, native projection, scene mutation, active publication, or robot
/// semantic.
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
  [[nodiscard]] ManagerApplyJournalIntegrityReport VerifyIntegrity() const;

  void Close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::component_manager
