#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::component_manager {

inline constexpr std::string_view kManagerBootstrapJournalSchemaId =
    "manyforge.internal.component-manager.bootstrap-journal.i4.v1";
inline constexpr std::int64_t kManagerBootstrapJournalSchemaVersion = 1;
inline constexpr std::int64_t kManagerBootstrapJournalSqliteApplicationId =
    1296450122;
inline constexpr std::size_t kMaximumBootstrapAttempts = 64U;
inline constexpr std::size_t kMaximumBootstrapExecutionHostBindings = 64U;
inline constexpr std::size_t kMaximumBootstrapJournalIdentifierBytes = 256U;
inline constexpr std::int64_t
    kMaximumBootstrapJournalDatabasePageImageBytes =
        64LL * 1024LL * 1024LL;

class ManagerBootstrapJournalError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ManagerBootstrapJournalConflictError final
    : public ManagerBootstrapJournalError {
 public:
  using ManagerBootstrapJournalError::ManagerBootstrapJournalError;
};

class ManagerBootstrapJournalLimitError final
    : public ManagerBootstrapJournalError {
 public:
  using ManagerBootstrapJournalError::ManagerBootstrapJournalError;
};

class ManagerBootstrapJournalBusyError final
    : public ManagerBootstrapJournalError {
 public:
  using ManagerBootstrapJournalError::ManagerBootstrapJournalError;
};

class ManagerBootstrapJournalIntegrityError final
    : public ManagerBootstrapJournalError {
 public:
  using ManagerBootstrapJournalError::ManagerBootstrapJournalError;
};

class ManagerBootstrapJournalClosedError final
    : public ManagerBootstrapJournalError {
 public:
  using ManagerBootstrapJournalError::ManagerBootstrapJournalError;
};

/// Exact immutable input persisted before a future bootstrap scene-owner call.
///
/// This value carries only identities already frozen by the owner contract.
/// It does not accept a caller-built retained bundle, compiler manifest,
/// planning product, process command, or runtime-readiness assertion.
struct ManagerBootstrapIntentDTO final {
  manyforge::core::BootstrapId bootstrap_id;
  manyforge::core::BootstrapGeneration bootstrap_generation;
  manyforge::core::CellId cell_id;
  manyforge::core::RetainedBundleId canonical_empty_retained_bundle_id;
  manyforge::core::GraphRevision canonical_empty_graph_revision;
  std::vector<manyforge::core::ProjectionExecutionHostBindingDTO>
      execution_host_bindings;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(
      const ManagerBootstrapIntentDTO&,
      const ManagerBootstrapIntentDTO&) = default;
};

/// Scene-owner accepted target persisted before any future native rebuild.
///
/// The incomplete logical-scene object deliberately does not cross this
/// storage boundary. It remains owned by the scene owner. This record retains
/// only the exact durable projection identity needed for replay.
struct ManagerBootstrapAcceptedTargetDTO final {
  manyforge::core::BootstrapId bootstrap_id;
  manyforge::core::BootstrapGeneration bootstrap_generation;
  manyforge::core::ProjectionRevisionDTO canonical_empty_target;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(
      const ManagerBootstrapAcceptedTargetDTO&,
      const ManagerBootstrapAcceptedTargetDTO&) = default;
};

struct PersistedManagerBootstrapIntent final {
  ManagerBootstrapIntentDTO intent;
  manyforge::core::ContentDigest intent_digest;

  friend bool operator==(
      const PersistedManagerBootstrapIntent&,
      const PersistedManagerBootstrapIntent&) = default;
};

struct PersistedManagerBootstrapAcceptedTarget final {
  ManagerBootstrapAcceptedTargetDTO accepted_target;
  manyforge::core::ContentDigest accepted_target_digest;

  friend bool operator==(
      const PersistedManagerBootstrapAcceptedTarget&,
      const PersistedManagerBootstrapAcceptedTarget&) = default;
};

enum class ManagerBootstrapJournalPhase {
  kEmpty,
  kIntentPersisted,
  kAcceptedTargetPersisted,
};

struct ManagerBootstrapJournalSnapshot final {
  manyforge::core::CellId cell_id;
  ManagerBootstrapJournalPhase phase;
  std::optional<PersistedManagerBootstrapIntent> intent;
  std::optional<PersistedManagerBootstrapAcceptedTarget> accepted_target;

  [[nodiscard]] bool valid() const noexcept;
};

struct ManagerBootstrapIntentWriteResult final {
  PersistedManagerBootstrapIntent record;
  bool inserted;
};

struct ManagerBootstrapAcceptedTargetWriteResult final {
  PersistedManagerBootstrapAcceptedTarget record;
  bool inserted;
};

struct ManagerBootstrapJournalIntegrityReport final {
  manyforge::core::CellId cell_id;
  std::string schema_id;
  std::int64_t schema_version;
  std::string schema_artifact_digest;
  std::int64_t bootstrap_attempt_count;
  std::int64_t execution_host_binding_row_count;
  std::int64_t accepted_target_count;
  std::optional<manyforge::core::BootstrapId> bootstrap_id;
  std::optional<manyforge::core::BootstrapGeneration>
      bootstrap_generation_high_water;
  ManagerBootstrapJournalPhase phase;
};

/// Durable typed owner of a bounded canonical-empty bootstrap-attempt
/// sequence and each accepted scene target.
///
/// The journal exposes no arbitrary event bytes, mutable generic phase, Apply
/// row, active alias, publication operation, scene/native/process/Kernel
/// command, or robot semantic. Exact duplicate writes replay the original
/// record. New attempts must retain the first BootstrapId and advance its
/// durable generation high-water mark by exactly one. Every payload conflict
/// fails without another durable effect.
class SqliteManagerBootstrapJournal final {
 public:
  SqliteManagerBootstrapJournal(
      std::filesystem::path database_path,
      manyforge::core::CellId cell_id);
  ~SqliteManagerBootstrapJournal();

  SqliteManagerBootstrapJournal(
      SqliteManagerBootstrapJournal&&) noexcept;
  SqliteManagerBootstrapJournal& operator=(
      SqliteManagerBootstrapJournal&&) noexcept;
  SqliteManagerBootstrapJournal(
      const SqliteManagerBootstrapJournal&) = delete;
  SqliteManagerBootstrapJournal& operator=(
      const SqliteManagerBootstrapJournal&) = delete;

  [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
  [[nodiscard]] const manyforge::core::CellId& cell_id() const noexcept;

  [[nodiscard]] ManagerBootstrapIntentWriteResult EnsureIntent(
      const ManagerBootstrapIntentDTO& intent);
  [[nodiscard]] ManagerBootstrapAcceptedTargetWriteResult
  RecordAcceptedTarget(
      const ManagerBootstrapAcceptedTargetDTO& accepted_target);
  [[nodiscard]] std::optional<manyforge::core::BootstrapGeneration>
  ReadBootstrapGenerationHighWater(
      const manyforge::core::BootstrapId& bootstrap_id) const;
  [[nodiscard]] ManagerBootstrapJournalSnapshot ReadSnapshot() const;
  [[nodiscard]] ManagerBootstrapJournalIntegrityReport VerifyIntegrity() const;

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view ToString(
    ManagerBootstrapJournalPhase phase) noexcept;

}  // namespace manyforge::component_manager
