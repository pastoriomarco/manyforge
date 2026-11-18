#pragma once

// Private to the logical-scene owner sources; not installed.

#include "logical_scene_owner_internal.hpp"

namespace manyforge::scene {

class LogicalSceneOwner::Impl final {
public:
  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, nullptr, nullptr, nullptr, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       core::IKernelCellAdmissionPort &cell_admission)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, nullptr, nullptr, &cell_admission, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source, nullptr, nullptr,
             nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source,
             &projection_resolver, nullptr, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver,
       core::IKernelCellAdmissionPort &cell_admission,
       persistence::SqliteRecordWriter *record_writer = nullptr)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source, &projection_resolver,
             &cell_admission, record_writer) {}

  Impl(std::filesystem::path database_path,
       AdmittedBootstrapAuthority authority,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource *retained_bundle_source,
       const core::IPlannerSceneProjectionResolver *projection_resolver,
       core::IKernelCellAdmissionPort *cell_admission,
       persistence::SqliteRecordWriter *record_writer)
      : database_path_(std::move(database_path)),
        fixed_query_(std::move(authority.query)),
        admission_digest_(std::move(authority.admission_digest)),
        admitted_bundle_(std::move(authority.retained_bundle)),
        publication_reader_(publication_reader),
        retained_bundle_source_(retained_bundle_source),
        projection_resolver_(projection_resolver),
        cell_admission_(cell_admission) {
    ValidateFixedQuery(fixed_query_);
    if (database_path_.empty() || database_path_.filename().empty()) {
      throw std::invalid_argument(
          "logical-scene database path must name one file");
    }
    const auto parent = database_path_.parent_path();
    if (!parent.empty()) {
      std::error_code error;
      const auto status = std::filesystem::symlink_status(parent, error);
      if (error || std::filesystem::is_symlink(status) ||
          !std::filesystem::is_directory(status)) {
        throw std::invalid_argument(
            "logical-scene database parent must be an existing non-symlink "
            "directory");
      }
    }

    auto lifecycle_lock = sqlite_lifecycle_internal::
        TryAcquireCanonicalEmptySceneStoreLifecycleLock(
            std::chrono::milliseconds(kBusyTimeoutMilliseconds));
    if (!lifecycle_lock.owns_lock()) {
      throw LogicalSceneOwnerBusyError(
          "timed out waiting for process-local scene-store startup");
    }
    try {
      static_cast<void>(manyforge::persistence::PreflightExistingSqliteReadOnly(
          database_path_,
          {
              static_cast<std::uintmax_t>(
                  kMaximumSceneStoreDatabasePageImageBytes),
              static_cast<std::uintmax_t>(kMaximumSceneStoreWalBytes),
          },
          [&](sqlite3 *preflight) {
            if (!IsExactUninitializedDatabase(preflight)) {
              ValidateDatabase(preflight, fixed_query_, admission_digest_);
            }
          }));
    } catch (const LogicalSceneOwnerError &) {
      throw;
    } catch (const std::exception &error) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene read-only startup preflight failed: " +
          std::string(error.what()));
    }

    const auto result =
        sqlite3_open_v2(database_path_.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                        nullptr);
    if (result != SQLITE_OK) {
      const std::string detail = database_ == nullptr
                                     ? sqlite3_errstr(result)
                                     : sqlite3_errmsg(database_);
      if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
      }
      throw LogicalSceneOwnerError(
          "cannot open logical-scene database: " + detail);
    }

    try {
      ConfigureConnection(database_);
      const auto schema_count = ScalarInteger(
          database_, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE "
                     "'sqlite_%'");
      bool changed_schema = false;
      if (schema_count == 0) {
        if (!IsExactUninitializedDatabase(database_)) {
          throw LogicalSceneOwnerIntegrityError(
              "schema-free logical-scene database carries foreign version "
              "metadata");
        }
        ConfigureDurability(database_);
        Transaction transaction(database_, "BEGIN IMMEDIATE");
        Exec(database_, detail::kEmbeddedCanonicalEmptySceneSchema);
        InsertMetadata(database_, fixed_query_, admission_digest_);
        Exec(database_, detail::kEmbeddedLogicalSceneTransactionMigration);
        InsertMigrationMetadata(database_);
        Exec(database_, detail::kEmbeddedNativeProjectionHandoffMigration);
        InsertNativeHandoffMigrationMetadata(database_);
        Exec(database_, detail::kEmbeddedBoundedHistoryMigration);
        InsertBoundedHistoryMigrationMetadata(database_);
        Exec(database_, detail::kEmbeddedMaintenanceInitializationMigration);
        InsertMaintenanceInitializationMigrationMetadata(database_);
        Exec(database_, detail::kEmbeddedInitialSceneBaselineMigration);
        InsertInitialSceneBaselineMigrationMetadata(database_);
        // The numbered resources stamp their own step numbers; the store is
        // created at the current version in this one transaction.
        Exec(database_, "PRAGMA user_version = " +
                            std::to_string(kLogicalSceneStoreSchemaVersion));
        transaction.Commit();
        changed_schema = true;
      } else {
        ValidateDatabase(database_, fixed_query_, admission_digest_);
        ConfigureDurability(database_);
      }
      ValidateDatabase(database_, fixed_query_, admission_digest_);
      if (changed_schema) {
        FsyncDatabaseDirectory(database_path_);
      }
      memory_ = LoadSceneStoreMemory(database_, fixed_query_);
      if (record_writer == nullptr) {
        owned_writer_ = std::make_unique<persistence::SqliteRecordWriter>();
        record_writer = owned_writer_.get();
      }
      writer_ = record_writer;
      // From here on only the writer thread uses the connection.
      slot_ = writer_->RegisterConnection(
          database_,
          "logical-scene",
          "UPDATE logical_scene_store_metadata SET records_lost = 1 "
          "WHERE singleton = 1");
    } catch (...) {
      owned_writer_.reset();
      sqlite3_close(database_);
      database_ = nullptr;
      throw;
    }
  }

  ~Impl() {
    try {
      Close();
    } catch (...) {
      if (database_ != nullptr && sqlite3_close_v2(database_) == SQLITE_OK) {
        database_ = nullptr;
      }
    }
  }

  // An owned writer drains here; a handed-in writer is drained by its owner
  // before this runs.
  void Close() {
    if (owned_writer_ != nullptr) {
      owned_writer_->DrainAndStop();
    }
    std::lock_guard operation_lock(maintenance_operation_mutex_);
    std::lock_guard lock(mutex_);
    InvalidateSceneCommitState();
    auto lifecycle_lock = sqlite_lifecycle_internal::
        AcquireCanonicalEmptySceneStoreLifecycleLockForClose();
    if (database_ == nullptr) {
      return;
    }
    const auto checkpoint = sqlite3_wal_checkpoint_v2(
        database_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    if (checkpoint != SQLITE_OK && checkpoint != SQLITE_BUSY &&
        checkpoint != SQLITE_LOCKED) {
      ThrowSqlite(database_, checkpoint, "checkpoint scene database");
    }
    const auto result = sqlite3_close(database_);
    if (result != SQLITE_OK) {
      ThrowSqlite(database_, result, "close scene database");
    }
    database_ = nullptr;
  }

  // Throws once the owner is closed, as every operation did when it opened
  // its database connection.
  void RequireOpen() const {
    if (database_ == nullptr) {
      throw LogicalSceneOwnerClosedError(
          "logical-scene owner is closed");
    }
  }

  [[nodiscard]] DurableState ReadStateTransaction() const {
    RequireOpen();
    return ReadCompleteDurableState(memory_);
  }

  [[nodiscard]] const SceneCommitStateCache &ReadSceneCommitState() const {
    RequireOpen();
    if (scene_commit_state_cache_.has_value()) {
      return *scene_commit_state_cache_;
    }
    auto durable_state = ReadCompleteDurableState(memory_);
    std::optional<SceneTransactionStoreState> transaction_state;
    if (durable_state.accepted.has_value() &&
        durable_state.finalization_receipt.has_value()) {
      transaction_state =
          ReadSceneTransactionStoreState(memory_, durable_state);
    }
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state)};
    return *scene_commit_state_cache_;
  }

  // Queues the record for the file; called while `mutex_` is held, after
  // `memory_` has changed.
  void Post(persistence::SqliteRecord record) const {
    if (!record.empty()) {
      static_cast<void>(writer_->Post(slot_, std::move(record)));
    }
  }

  void StoreSceneCommitState(
      DurableState durable_state,
      std::optional<SceneTransactionStoreState> transaction_state) {
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state)};
  }

  void InvalidateSceneCommitState() noexcept {
    scene_commit_state_cache_.reset();
  }

  std::filesystem::path database_path_;
  ProjectionBootstrapPrerequisiteQueryDTO fixed_query_;
  ContentDigest admission_digest_;
  RetainedBundleDTO admitted_bundle_;
  const core::IActiveBundlePublicationReader &publication_reader_;
  const deployment::IVerifiedRetainedBundleSource *retained_bundle_source_;
  const core::IPlannerSceneProjectionResolver *projection_resolver_;
  core::IKernelCellAdmissionPort *cell_admission_;
  // Only the original live continuation can complete a pending private
  // adoption. Durable rows are evidence, not a replacement for that request.
  // Historical ordinary maintenance also preserved Scene revisions, so the
  // revision shape cannot distinguish a private operation from those rows.
  std::optional<QualifiedSceneConfigurationAdoptionRequest> live_adoption_;
  mutable std::mutex maintenance_operation_mutex_;
  mutable std::mutex mutex_;
  sqlite3 *database_{};
  SceneStoreMemory memory_;
  mutable std::optional<SceneCommitStateCache> scene_commit_state_cache_;
  // Numbers each ReadPublication under `mutex_` (OD-302).
  mutable std::uint64_t publication_read_sequence_{};
  std::unique_ptr<persistence::SqliteRecordWriter> owned_writer_;
  persistence::SqliteRecordWriter *writer_{};
  persistence::SqliteRecordWriter::StoreSlot slot_{};
};

} // namespace manyforge::scene
