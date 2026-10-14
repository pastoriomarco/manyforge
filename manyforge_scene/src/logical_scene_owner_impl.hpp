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
             publication_reader, nullptr, nullptr, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       core::IKernelCellAdmissionPort &cell_admission)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, nullptr, nullptr, &cell_admission) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source, nullptr, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source,
             &projection_resolver, nullptr) {}

  Impl(std::filesystem::path database_path,
       const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
       const core::IPlannerSceneProjectionResolver &projection_resolver,
       core::IKernelCellAdmissionPort &cell_admission)
      : Impl(std::move(database_path), AuthorityFromAdmission(admission),
             publication_reader, &retained_bundle_source, &projection_resolver,
             &cell_admission) {}

  Impl(std::filesystem::path database_path,
       AdmittedBootstrapAuthority authority,
       const core::IActiveBundlePublicationReader &publication_reader,
       const deployment::IVerifiedRetainedBundleSource *retained_bundle_source,
       const core::IPlannerSceneProjectionResolver *projection_resolver,
       core::IKernelCellAdmissionPort *cell_admission)
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
              const auto version =
                  ScalarInteger(preflight, "PRAGMA user_version");
              if (version == kCanonicalEmptySceneStoreSchemaVersion) {
                ValidateBaseDatabase(preflight, fixed_query_,
                                     admission_digest_);
              } else if (version ==
                         kLogicalSceneTransactionStoreSchemaVersion) {
                ValidateTransactionDatabase(preflight, fixed_query_,
                                            admission_digest_);
              } else if (version ==
                         kNativeProjectionHandoffStoreSchemaVersion) {
                ValidateNativeHandoffDatabase(preflight, fixed_query_,
                                              admission_digest_);
              } else if (version == kBoundedHistoryStoreSchemaVersion) {
                ValidateBoundedHistoryDatabase(preflight, fixed_query_,
                                               admission_digest_);
              } else if (version ==
                         kMaintenanceInitializationSchemaVersion) {
                ValidateMaintenanceDatabase(preflight, fixed_query_,
                                            admission_digest_);
              } else {
                ValidateDatabase(preflight, fixed_query_, admission_digest_);
              }
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
        transaction.Commit();
        changed_schema = true;
      } else {
        const auto version = ScalarInteger(database_, "PRAGMA user_version");
        if (version == kCanonicalEmptySceneStoreSchemaVersion) {
          ValidateBaseDatabase(database_, fixed_query_, admission_digest_);
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
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
          transaction.Commit();
          changed_schema = true;
        } else if (version == kLogicalSceneTransactionStoreSchemaVersion) {
          ValidateTransactionDatabase(database_, fixed_query_,
                                      admission_digest_);
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
          Exec(database_, detail::kEmbeddedNativeProjectionHandoffMigration);
          InsertNativeHandoffMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedBoundedHistoryMigration);
          InsertBoundedHistoryMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedMaintenanceInitializationMigration);
          InsertMaintenanceInitializationMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedInitialSceneBaselineMigration);
          InsertInitialSceneBaselineMigrationMetadata(database_);
          transaction.Commit();
          changed_schema = true;
        } else if (version == kNativeProjectionHandoffStoreSchemaVersion) {
          ValidateNativeHandoffDatabase(database_, fixed_query_,
                                        admission_digest_);
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
          Exec(database_, detail::kEmbeddedBoundedHistoryMigration);
          InsertBoundedHistoryMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedMaintenanceInitializationMigration);
          InsertMaintenanceInitializationMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedInitialSceneBaselineMigration);
          InsertInitialSceneBaselineMigrationMetadata(database_);
          transaction.Commit();
          changed_schema = true;
        } else if (version == kBoundedHistoryStoreSchemaVersion) {
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
          Exec(database_, detail::kEmbeddedMaintenanceInitializationMigration);
          InsertMaintenanceInitializationMigrationMetadata(database_);
          Exec(database_, detail::kEmbeddedInitialSceneBaselineMigration);
          InsertInitialSceneBaselineMigrationMetadata(database_);
          transaction.Commit();
          changed_schema = true;
        } else if (version == kMaintenanceInitializationSchemaVersion) {
          ConfigureDurability(database_);
          Transaction transaction(database_, "BEGIN IMMEDIATE");
          Exec(database_, detail::kEmbeddedInitialSceneBaselineMigration);
          InsertInitialSceneBaselineMigrationMetadata(database_);
          transaction.Commit();
          changed_schema = true;
        } else {
          ValidateDatabase(database_, fixed_query_, admission_digest_);
          ConfigureDurability(database_);
        }
      }
      const bool interrupted_initialization =
          SettleInterruptedMaintenanceInitializations(database_);
      ValidateDatabase(database_, fixed_query_, admission_digest_);
      const bool recovered_scene_transaction =
          RecoverIncompleteSceneTransaction(database_, fixed_query_);
      if (recovered_scene_transaction) {
        ValidateDatabase(database_, fixed_query_, admission_digest_);
      }
      if (cell_admission_ != nullptr) {
        try {
          SettleRecoveredAuthoredSceneClaims(database_, *cell_admission_);
        } catch (const CellAdmissionTemporarilyUnavailable &) {
          // The immutable scene and durable outcomes remain readable. Kernel
          // admission stays closed until Manager retries exact settlement.
        }
      }
      if (changed_schema || recovered_scene_transaction ||
          interrupted_initialization) {
        FsyncDatabaseDirectory(database_path_);
      }
    } catch (...) {
      sqlite3_close(database_);
      database_ = nullptr;
      throw;
    }
  }

  ~Impl() {
    try {
      std::lock_guard operation_lock(maintenance_operation_mutex_);
      std::lock_guard lock(mutex_);
      auto lifecycle_lock = sqlite_lifecycle_internal::
          AcquireCanonicalEmptySceneStoreLifecycleLockForClose();
      if (database_ != nullptr) {
        if (sqlite3_close_v2(database_) == SQLITE_OK) {
          database_ = nullptr;
        }
      }
    } catch (...) {
    }
  }

  void Close() {
    std::lock_guard operation_lock(maintenance_operation_mutex_);
    std::lock_guard lock(mutex_);
    InvalidateSceneCommitState();
    provider_freshness_records_.clear();
    provider_sequence_watermarks_.clear();
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

  [[nodiscard]] sqlite3 *database() const {
    if (database_ == nullptr) {
      throw LogicalSceneOwnerClosedError(
          "logical-scene owner is closed");
    }
    return database_;
  }

  [[nodiscard]] DurableState ReadStateTransaction() const {
    auto *value = database();
    Transaction transaction(value, "BEGIN");
    auto state = ReadCompleteDurableState(value, fixed_query_);
    transaction.Commit();
    return state;
  }

  [[nodiscard]] const SceneCommitStateCache &ReadSceneCommitState() const {
    auto *value = database();
    const auto data_version = ScalarInteger(value, "PRAGMA data_version");
    if (scene_commit_state_cache_.has_value() &&
        scene_commit_state_cache_->external_data_version == data_version) {
      return *scene_commit_state_cache_;
    }

    Transaction transaction(value, "BEGIN");
    auto durable_state = ReadCompleteDurableState(value, fixed_query_);
    std::optional<SceneTransactionStoreState> transaction_state;
    if (durable_state.accepted.has_value() &&
        durable_state.finalization_receipt.has_value()) {
      transaction_state = ReadSceneTransactionStoreState(value, durable_state);
    }
    const auto stable_data_version =
        ScalarInteger(value, "PRAGMA data_version");
    transaction.Commit();
    if (stable_data_version != data_version) {
      throw LogicalSceneOwnerConflictError(
          "logical-scene database changed while rebuilding the live commit "
          "state");
    }
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state), data_version};
    return *scene_commit_state_cache_;
  }

  [[nodiscard]] bool SceneCommitStateStillCurrent(
      const std::int64_t expected_external_data_version) const {
    return ScalarInteger(database(), "PRAGMA data_version") ==
           expected_external_data_version;
  }

  void StoreSceneCommitState(
      DurableState durable_state,
      std::optional<SceneTransactionStoreState> transaction_state,
      const std::int64_t external_data_version) {
    scene_commit_state_cache_ = SceneCommitStateCache{
        std::move(durable_state), std::move(transaction_state),
        external_data_version};
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
  std::optional<std::variant<QualifiedSceneProductAdoptionRequest,
                             QualifiedSceneConfigurationAdoptionRequest>> live_product_adoption_;
  mutable std::mutex maintenance_operation_mutex_;
  mutable std::mutex mutex_;
  sqlite3 *database_{};
  mutable std::optional<SceneCommitStateCache> scene_commit_state_cache_;
  /// Drops freshness records and sequence watermarks of objects that no
  /// longer exist in `snapshot`.
  void PruneProviderFreshnessState(const LogicalSceneSnapshotDTO &snapshot) {
    const auto absent = [&](const auto &entry) {
      return FindSceneObject(snapshot, entry.first) == nullptr;
    };
    std::erase_if(provider_freshness_records_, absent);
    std::erase_if(provider_sequence_watermarks_, absent);
  }

  /// Raises the object's same-origin sequence high-water mark, or resets it
  /// to `evidence` when the provider origin changed.
  void AdvanceProviderSequenceWatermark(
      const SceneResourceId &scene_resource_id,
      const core::SceneObjectProviderEvidenceDTO &evidence) {
    const auto [watermark, inserted] =
        provider_sequence_watermarks_.try_emplace(scene_resource_id, evidence);
    if (!inserted &&
        (!SameProviderOrigin(watermark->second, evidence) ||
         evidence.provider_source_sequence >
             watermark->second.provider_source_sequence)) {
      watermark->second = evidence;
    }
  }

  /// The same-origin sequence high-water mark for `evidence`, if any.
  [[nodiscard]] std::optional<std::uint64_t> ProviderSequenceWatermark(
      const SceneResourceId &scene_resource_id,
      const core::SceneObjectProviderEvidenceDTO &evidence) const {
    const auto watermark = provider_sequence_watermarks_.find(scene_resource_id);
    if (watermark == provider_sequence_watermarks_.end() ||
        !SameProviderOrigin(watermark->second, evidence)) {
      return std::nullopt;
    }
    return watermark->second.provider_source_sequence;
  }

  /// Never persisted: owner reconstruction starts with no confirmations.
  std::map<SceneResourceId, ProviderFreshnessRecord>
      provider_freshness_records_;
  /// Per-object (provider origin, highest accepted provider source sequence)
  /// over committed evidence and confirmations. Only the origin fields and
  /// the sequence are meaningful. It survives record clearing, history
  /// compaction and unrelated commits for the owner process lifetime, and is
  /// reset only by object removal or a committed origin change.
  std::map<SceneResourceId, core::SceneObjectProviderEvidenceDTO>
      provider_sequence_watermarks_;
};

} // namespace manyforge::scene
